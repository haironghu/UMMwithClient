/* ========================================================================
 * cis_router.h  --  Cluster routing placeholder
 *
 * The CIS (Cluster Information Service) router is responsible for:
 *   - Tracking which nodes are in the cluster and their addresses
 *   - Routing allocation requests to appropriate nodes
 *   - Load balancing across the cluster
 *
 * This is a placeholder implementation that logs a warning and falls back
 * to local-node routing.  A full implementation would use a distributed
 * consensus service (e.g. etcd, ZooKeeper) to track cluster membership.
 * ======================================================================== */

#ifndef CIS_ROUTER_H
#define CIS_ROUTER_H

#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the CIS router.
 *
 * @param cfg  UMM configuration.
 * @return     UMM_OK on success, error code otherwise.
 */
int cis_router_init(const UMMConfig *cfg);

/**
 * Shut down the CIS router and release all resources.
 */
void cis_router_deinit(void);

/**
 * Select a node for a new allocation of the given size.
 *
 * Current placeholder: returns the local node.
 *
 * @param size  Requested allocation size in bytes.
 * @return      Selected node ID.
 */
node_id_t cis_route_alloc_node(uint64_t size);

/**
 * Register a node in the cluster routing table.
 *
 * @param node  Node ID.
 * @param addr  Network address of the node (host:port).
 * @return      UMM_OK on success, error code otherwise.
 */
int cis_router_register_node(node_id_t node, const char *addr);

/**
 * Look up the network address of a node.
 *
 * @param node     Node ID to look up.
 * @param out_addr Buffer to write the address string into.
 * @param len      Size of out_addr buffer.
 * @return         UMM_OK on success, UMM_E_NOT_FOUND if unknown.
 */
int cis_router_get_node_addr(node_id_t node, char *out_addr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CIS_ROUTER_H */
