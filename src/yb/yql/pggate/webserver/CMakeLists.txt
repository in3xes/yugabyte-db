# Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

set(YB_PCH_PATH ../)

set(PGGATE_WEBSERVER_SRCS
    ybc_pg_webserver_wrapper.cc
    )

set(PGGATE_WEBSERVER_LIBS
    server_process
    yb_common
    yb_pggate_util
    yb_util
    )

ADD_YB_LIBRARY(yb_pggate_webserver
               SRCS ${PGGATE_WEBSERVER_SRCS}
               DEPS ${PGGATE_WEBSERVER_LIBS})
