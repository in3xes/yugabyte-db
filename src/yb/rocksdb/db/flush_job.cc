//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "yb/rocksdb/db/flush_job.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>

#include <algorithm>
#include <vector>
#include <chrono>

#include "yb/ash/wait_state.h"

#include "yb/rocksdb/db/builder.h"
#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/db/event_helpers.h"
#include "yb/rocksdb/db/filename.h"
#include "yb/rocksdb/db/file_numbers.h"
#include "yb/rocksdb/db/internal_stats.h"
#include "yb/rocksdb/db/log_reader.h"
#include "yb/rocksdb/db/log_writer.h"
#include "yb/rocksdb/db/memtable.h"
#include "yb/rocksdb/db/memtable_list.h"
#include "yb/rocksdb/db/version_set.h"
#include "yb/rocksdb/port/likely.h"
#include "yb/rocksdb/port/port.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/statistics.h"
#include "yb/rocksdb/status.h"
#include "yb/rocksdb/table.h"
#include "yb/rocksdb/table/merger.h"
#include "yb/rocksdb/table/scoped_arena_iterator.h"
#include "yb/rocksdb/util/coding.h"
#include "yb/rocksdb/util/event_logger.h"
#include "yb/rocksdb/util/log_buffer.h"
#include "yb/rocksdb/util/logging.h"
#include "yb/rocksdb/util/mutexlock.h"
#include "yb/rocksdb/util/statistics.h"
#include "yb/rocksdb/util/stop_watch.h"

#include "yb/util/atomic.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/result.h"
#include "yb/util/stats/iostats_context_imp.h"
#include "yb/util/sync_point.h"

DEFINE_UNKNOWN_int32(rocksdb_nothing_in_memtable_to_flush_sleep_ms, 10,
    "Used for a temporary workaround for http://bit.ly/ybissue437. How long to wait (ms) in case "
    "we could not flush any memtables, usually due to filters preventing us from doing so.");

DEFINE_test_flag(bool, rocksdb_crash_on_flush, false,
                 "When set, memtable flush in rocksdb crashes.");

DEFINE_UNKNOWN_bool(rocksdb_release_mutex_during_wait_for_memtables_to_flush, true,
            "When a flush is scheduled, but there isn't a memtable eligible yet, release "
            "the mutex before going to sleep and reacquire it post sleep.");

namespace rocksdb {

FlushJob::FlushJob(const std::string& dbname, ColumnFamilyData* cfd,
                   const DBOptions& db_options,
                   const MutableCFOptions& mutable_cf_options,
                   const EnvOptions& env_options, VersionSet* versions,
                   InstrumentedMutex* db_mutex,
                   std::atomic<bool>* shutting_down,
                   std::atomic<bool>* disable_flush_on_shutdown,
                   std::vector<SequenceNumber> existing_snapshots,
                   SequenceNumber earliest_write_conflict_snapshot,
                   MemTableFilter mem_table_flush_filter,
                   FileNumbersProvider* file_numbers_provider,
                   JobContext* job_context, LogBuffer* log_buffer,
                   Directory* db_directory, Directory* output_file_directory,
                   CompressionType output_compression, Statistics* stats,
                   EventLogger* event_logger)
    : dbname_(dbname),
      cfd_(cfd),
      db_options_(db_options),
      mutable_cf_options_(mutable_cf_options),
      env_options_(env_options),
      versions_(versions),
      db_mutex_(db_mutex),
      shutting_down_(shutting_down),
      disable_flush_on_shutdown_(disable_flush_on_shutdown),
      existing_snapshots_(std::move(existing_snapshots)),
      earliest_write_conflict_snapshot_(earliest_write_conflict_snapshot),
      mem_table_flush_filter_(std::move(mem_table_flush_filter)),
      file_numbers_provider_(file_numbers_provider),
      job_context_(job_context),
      log_buffer_(log_buffer),
      db_directory_(db_directory),
      output_file_directory_(output_file_directory),
      output_compression_(output_compression),
      stats_(stats),
      event_logger_(event_logger),
      wait_state_(yb::ash::WaitStateInfo::CreateIfAshIsEnabled<yb::ash::WaitStateInfo>()) {
  if (wait_state_) {
    wait_state_->UpdateMetadata(
        {.root_request_id = yb::Uuid::Generate(),
         .query_id = std::to_underlying(yb::ash::FixedQueryId::kQueryIdForFlush),
         .rpc_request_id = job_context_->job_id});
    wait_state_->UpdateAuxInfo({.tablet_id = db_options_.tablet_id, .method = "Flush"});
    SET_WAIT_STATUS_TO(wait_state_, OnCpu_Passive);
    yb::ash::FlushAndCompactionWaitStatesTracker().Track(wait_state_);
  }
  // Update the thread status to indicate flush.
  ReportStartedFlush();
  DEBUG_ONLY_TEST_SYNC_POINT("FlushJob::FlushJob()");
}

FlushJob::~FlushJob() {
  if (wait_state_) {
    yb::ash::FlushAndCompactionWaitStatesTracker().Untrack(wait_state_);
  }
}

void FlushJob::ReportStartedFlush() {
  IOSTATS_RESET(bytes_written);
}

void FlushJob::RecordFlushIOStats() {
  RecordTick(stats_, FLUSH_WRITE_BYTES, IOSTATS(bytes_written));
  IOSTATS_RESET(bytes_written);
}

Result<FileNumbersHolder> FlushJob::Run(FileMetaData* file_meta) {
  ADOPT_WAIT_STATE(wait_state_);
  SCOPED_WAIT_STATUS(RocksDB_Flush);
  if (PREDICT_FALSE(yb::GetAtomicFlag(&FLAGS_TEST_rocksdb_crash_on_flush))) {
    CHECK(false) << "a flush should not have been scheduled.";
  }

  // Save the contents of the earliest memtable as a new Table
  FileMetaData meta;
  autovector<MemTable*> mems;
  cfd_->imm()->PickMemtablesToFlush(&mems, mem_table_flush_filter_, &mutable_cf_options_);
  if (mems.empty()) {
    // A temporary workaround for repeated "Nothing in memtable to flush" messages in a
    // transactional workload due to the flush filter preventing us from flushing any memtables in
    // the provisional records RocksDB.
    //
    // See https://github.com/yugabyte/yugabyte-db/issues/437 for more details.
    YB_LOG_EVERY_N_SECS(INFO, 1)
        << db_options_.log_prefix
        << "[" << cfd_->GetName() << "] No eligible memtables to flush.";

    bool release_mutex = FLAGS_rocksdb_release_mutex_during_wait_for_memtables_to_flush;

    if (release_mutex) {
      // Release the mutex before the sleep, so as to unblock writers.
      db_mutex_->Unlock();
    }

      std::this_thread::sleep_for(std::chrono::milliseconds(
        FLAGS_rocksdb_nothing_in_memtable_to_flush_sleep_ms));

    if (release_mutex) {
      db_mutex_->Lock();
    }

    return FileNumbersHolder();
  }

  // entries mems are (implicitly) sorted in ascending order by their created
  // time. We will use the first memtable's `edit` to keep the meta info for
  // this flush.
  MemTable* m = mems[0];
  VersionEdit* edit = m->GetEdits();
  edit->SetPrevLogNumber(0);
  // SetLogNumber(log_num) indicates logs with number smaller than log_num
  // will no longer be picked up for recovery.
  edit->SetLogNumber(mems.back()->GetNextLogNumber());
  edit->SetColumnFamily(cfd_->GetID());

  // This will release and re-acquire the mutex.
  auto fnum = WriteLevel0Table(mems, edit, &meta);

  if (fnum.ok() && ((shutting_down_->load(std::memory_order_acquire) &&
                     disable_flush_on_shutdown_->load(std::memory_order_acquire)) ||
                    cfd_->IsDropped())) {
    fnum = STATUS(ShutdownInProgress, "Database shutdown or Column family drop during flush");
  }

  if (!fnum.ok()) {
    cfd_->imm()->RollbackMemtableFlush(mems, meta.fd.GetNumber());
  } else {
    DEBUG_ONLY_TEST_SYNC_POINT("FlushJob::InstallResults");
    // Replace immutable memtable with the generated Table
    Status s = cfd_->imm()->InstallMemtableFlushResults(
        cfd_, mutable_cf_options_, mems, versions_, db_mutex_,
        meta.fd.GetNumber(), &job_context_->memtables_to_free, db_directory_,
        log_buffer_, *fnum);
    if (!s.ok()) {
      fnum = s;
    }
  }

  if (fnum.ok() && file_meta != nullptr) {
    *file_meta = meta;
  }

  // This includes both SST and MANIFEST files IO.
  RecordFlushIOStats();

  auto stream = event_logger_->LogToBuffer(log_buffer_);
  stream << "job" << job_context_->job_id << "event"
         << "flush_finished";
  stream << "lsm_state";
  stream.StartArray();
  auto vstorage = cfd_->current()->storage_info();
  for (int level = 0; level < vstorage->num_levels(); ++level) {
    stream << vstorage->NumLevelFiles(level);
  }
  stream.EndArray();

  return fnum;
}

Result<FileNumbersHolder> FlushJob::WriteLevel0Table(
    const autovector<MemTable*>& mems, VersionEdit* edit, FileMetaData* meta) {
  db_mutex_->AssertHeld();
  const uint64_t start_micros = db_options_.env->NowMicros();
  auto file_number_holder = file_numbers_provider_->NewFileNumber();
  auto file_number = file_number_holder.Last();
  // path 0 for level 0 file.
  meta->fd = FileDescriptor(file_number, 0, 0, 0);

  Status s;
  {
    db_mutex_->Unlock();
    if (log_buffer_) {
      log_buffer_->FlushBufferToLog();
    }
    std::vector<InternalIterator*> memtables;
    ReadOptions ro;
    ro.total_order_seek = true;
    Arena arena;
    uint64_t total_num_entries = 0, total_num_deletes = 0;
    size_t total_memory_usage = 0;
    for (MemTable* m : mems) {
      RLOG(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
          "[%s] [JOB %d] Flushing memtable with next log file: %" PRIu64 "\n",
          cfd_->GetName().c_str(), job_context_->job_id, m->GetNextLogNumber());
      memtables.push_back(m->NewIterator(ro, &arena));
      total_num_entries += m->num_entries();
      total_num_deletes += m->num_deletes();
      total_memory_usage += m->ApproximateMemoryUsage();
      const auto* range = m->Frontiers();
      if (range) {
        UserFrontier::Update(
            &range->Smallest(), UpdateUserValueType::kSmallest, &meta->smallest.user_frontier);
        UserFrontier::Update(
            &range->Largest(), UpdateUserValueType::kLargest, &meta->largest.user_frontier);
      }
    }

    event_logger_->Log() << "job" << job_context_->job_id << "event"
                         << "flush_started"
                         << "num_memtables" << mems.size() << "num_entries"
                         << total_num_entries << "num_deletes"
                         << total_num_deletes << "memory_usage"
                         << total_memory_usage;

    TableFileCreationInfo info;
    {
      ScopedArenaIterator iter(
          NewMergingIterator(cfd_->internal_comparator().get(), &memtables[0],
                             static_cast<int>(memtables.size()), &arena));
      RLOG(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
          "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": started",
          cfd_->GetName().c_str(), job_context_->job_id, meta->fd.GetNumber());

      DEBUG_ONLY_TEST_SYNC_POINT_CALLBACK(
          "FlushJob::WriteLevel0Table:output_compression", &output_compression_);
      s = BuildTable(dbname_,
                     db_options_,
                     *cfd_->ioptions(),
                     env_options_,
                     cfd_->table_cache(),
                     iter.get(),
                     meta,
                     cfd_->internal_comparator(),
                     cfd_->int_tbl_prop_collector_factories(),
                     cfd_->GetID(),
                     existing_snapshots_,
                     earliest_write_conflict_snapshot_,
                     output_compression_,
                     cfd_->ioptions()->compression_opts,
                     mutable_cf_options_.paranoid_file_checks,
                     cfd_->internal_stats(),
                     yb::IOPriority::kHigh,
                     &table_properties_);
      info.table_properties = table_properties_;
      LogFlush(db_options_.info_log);
    }
    RLOG(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": %" PRIu64
        " bytes %s%s %s",
        cfd_->GetName().c_str(), job_context_->job_id, meta->fd.GetNumber(),
        meta->fd.GetTotalFileSize(), s.ToString().c_str(),
        meta->marked_for_compaction ? " (needs compaction)" : "",
        meta->FrontiersToString().c_str());

    // output to event logger
    if (s.ok()) {
      info.db_name = dbname_;
      info.cf_name = cfd_->GetName();
      info.file_path = TableFileName(db_options_.db_paths,
                                     meta->fd.GetNumber(),
                                     meta->fd.GetPathId());
      info.file_size = meta->fd.GetTotalFileSize();
      info.job_id = job_context_->job_id;
      EventHelpers::LogAndNotifyTableFileCreation(
          event_logger_, db_options_.listeners,
          meta->fd, info);
      DEBUG_ONLY_TEST_SYNC_POINT("FlushJob::LogAndNotifyTableFileCreation()");
    }

    if (!db_options_.disableDataSync && output_file_directory_ != nullptr) {
      RETURN_NOT_OK(output_file_directory_->Fsync());
    }
    DEBUG_ONLY_TEST_SYNC_POINT("FlushJob::WriteLevel0Table");
    db_mutex_->Lock();
  }

  // Note that if total_file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  if (s.ok() && meta->fd.GetTotalFileSize() > 0) {
    // if we have more than 1 background thread, then we cannot
    // insert files directly into higher levels because some other
    // threads could be concurrently producing compacted files for
    // that key range.
    // Add file to L0
    edit->AddCleanedFile(0 /* level */, *meta);
  }

  InternalStats::CompactionStats stats(1);
  stats.micros = db_options_.env->NowMicros() - start_micros;
  stats.bytes_written = meta->fd.GetTotalFileSize();
  cfd_->internal_stats()->AddCompactionStats(0 /* level */, stats);
  cfd_->internal_stats()->AddCFStats(InternalStats::BYTES_FLUSHED,
      meta->fd.GetTotalFileSize());
  RecordFlushIOStats();
  if (s.ok()) {
    return file_number_holder;
  } else {
    return s;
  }
}

}  // namespace rocksdb
