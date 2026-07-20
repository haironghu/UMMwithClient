#ifndef UMM_CONFIG_PARSER_H
#define UMM_CONFIG_PARSER_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse a YAML-ish configuration file and fill in an UMMConfig.
 *
 * Recognised keys:
 *   mode               (string)
 *   transport          (string)
 *   consistency_model  (string)
 *   mock_node_count    (integer)
 *   mock_node_size     (integer)
 *   meta_server_addr   (string)
 *   mem_server_addr    (string)
 *   my_node_id         (integer)
 *
 * Lines beginning with '#' or blank lines are ignored.
 *
 * @param filepath  Path to the configuration file.
 * @param out_cfg   Structure to populate (zero-filled first).
 * @return          UMM_OK on success, or a negative error code.
 */
int umm_parse_config(const char *filepath, UMMConfig *out_cfg);

#ifdef __cplusplus
}
#endif

#endif /* UMM_CONFIG_PARSER_H */
