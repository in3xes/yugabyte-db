// tslint:disable
/**
 * Yugabyte Cloud
 * YugabyteDB as a Service
 *
 * The version of the OpenAPI document: v1
 * Contact: support@yugabyte.com
 *
 * NOTE: This class is auto generated by OpenAPI Generator (https://openapi-generator.tech).
 * https://openapi-generator.tech
 * Do not edit the class manually.
 */


// eslint-disable-next-line no-duplicate-imports
import type { NamespacesInfo } from './NamespacesInfo';
// eslint-disable-next-line no-duplicate-imports
import type { XClusterPlacementLocation } from './XClusterPlacementLocation';


/**
 * 
 * @export
 * @interface XClusterNamespaceMetrics
 */
export interface XClusterNamespaceMetrics  {
  /**
   * Unique replication group id of each replication
   * @type {string}
   * @memberof XClusterNamespaceMetrics
   */
  replication_group_id: string;
  /**
   * 
   * @type {NamespacesInfo[]}
   * @memberof XClusterNamespaceMetrics
   */
  namespace_list: NamespacesInfo[];
  /**
   * 
   * @type {XClusterPlacementLocation[]}
   * @memberof XClusterNamespaceMetrics
   */
  source_placement_location?: XClusterPlacementLocation[];
  /**
   * 
   * @type {XClusterPlacementLocation[]}
   * @memberof XClusterNamespaceMetrics
   */
  target_placement_location?: XClusterPlacementLocation[];
  /**
   * 
   * @type {string}
   * @memberof XClusterNamespaceMetrics
   */
  source_universe_uuid?: string;
  /**
   * 
   * @type {string}
   * @memberof XClusterNamespaceMetrics
   */
  target_universe_uuid?: string;
}



