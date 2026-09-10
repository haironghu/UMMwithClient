#include "config_parser.h"
#include "error_codes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

static char* trim_left(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    return s;
}

static void trim_right(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

static char* trim(char *s)
{
    trim_right(s);
    return trim_left(s);
}

/**
 * Split "key: value" into key and value components.
 * Modifies the input line in-place.
 */
static int split_key_value(char *line, char **key, char **value)
{
    char *colon = strchr(line, ':');
    if (!colon)
        return UMM_E_INVALID_ARG;

    *colon = '\0';
    *key = trim(line);
    *value = trim(colon + 1);

    if ((*key)[0] == '\0')
        return UMM_E_INVALID_ARG;

    return UMM_OK;
}

/**
 * Strip matching surrounding quotes (single or double) from a value.
 */
static void strip_quotes(char *s)
{
    size_t len = strlen(s);
    if (len < 2)
        return;

    if ((s[0] == '"' && s[len - 1] == '"') ||
        (s[0] == '\'' && s[len - 1] == '\'')) {
        s[len - 1] = '\0';
        memmove(s, s + 1, len - 1);
    }
}

/**
 * Copy a string value into a fixed-size buffer safely.
 */
static void set_str(char *dest, size_t dest_size, const char *value)
{
    strncpy(dest, value, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

/**
 * Parse a string value as unsigned long long.
 */
static int parse_ull(const char *s, unsigned long long *out)
{
    char *endptr = NULL;
    errno = 0;
    *out = strtoull(s, &endptr, 0);
    if (errno != 0 || *endptr != '\0')
        return UMM_E_INVALID_ARG;
    return UMM_OK;
}

static int parse_int(const char *s, int *out)
{
    char *endptr = NULL;
    errno = 0;
    long val = strtol(s, &endptr, 0);
    if (errno != 0 || *endptr != '\0' || val < INT_MIN || val > INT_MAX)
        return UMM_E_INVALID_ARG;
    *out = (int)val;
    return UMM_OK;
}

/**
 * Parse a boolean value: true/false/1/0/yes/no (大小写不敏感).
 */
static int parse_bool(const char *s, int *out)
{
    if (!strcasecmp(s, "true") || !strcmp(s, "1") ||
        !strcasecmp(s, "yes")) {
        *out = 1;
        return UMM_OK;
    }
    if (!strcasecmp(s, "false") || !strcmp(s, "0") ||
        !strcasecmp(s, "no")) {
        *out = 0;
        return UMM_OK;
    }
    return UMM_E_INVALID_ARG;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int umm_parse_config(const char *filepath, UMMConfig *out_cfg)
{
    if (!filepath || !out_cfg)
        return UMM_E_INVALID_ARG;

    memset(out_cfg, 0, sizeof(*out_cfg));
    /* NDS RPC server 托管缺省值（全部为可覆盖缺省，enable 缺省关） */
    out_cfg->nds_rpc_server_ns = 1;
    out_cfg->nds_rpc_server_qd = 64;
    out_cfg->nds_rpc_server_keep_alive = 1;

    FILE *fp = fopen(filepath, "r");
    if (!fp)
        return UMM_E_NOT_FOUND;

    /* 16 SSD paths plus sizes must fit on a single configuration line. */
    char line[8192];
    int line_no = 0;
    int rc = UMM_OK;

    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        line[sizeof(line) - 1] = '\0';
        if (!strchr(line, '\n') && !feof(fp)) {
            rc = UMM_E_INVALID_ARG;
            break;  /* Never interpret a truncated device list. */
        }

        /* Remove trailing newline if present */
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\n')
            line[ll - 1] = '\0';

        char *trimmed = trim(line);

        /* Skip blank lines and comments */
        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        char *key = NULL, *value = NULL;
        int ret = split_key_value(trimmed, &key, &value);
        if (ret != UMM_OK) {
            rc = UMM_E_INVALID_ARG;
            break;
        }

        strip_quotes(value);

        if (strcmp(key, "transport") == 0) {
            set_str(out_cfg->transport, sizeof(out_cfg->transport), value);
        } else if (strcmp(key, "consistency_model") == 0) {
            set_str(out_cfg->consistency_model, sizeof(out_cfg->consistency_model), value);
        } else if (strcmp(key, "memory_size") == 0) {
            unsigned long long v;
            if (parse_ull(value, &v) != UMM_OK) { rc = UMM_E_INVALID_ARG; break; }
            out_cfg->memory_size = (uint64_t)v;
        } else if (strcmp(key, "meta_server_addr") == 0) {
            set_str(out_cfg->meta_server_addr, sizeof(out_cfg->meta_server_addr), value);
        } else if (strcmp(key, "mem_server_addr") == 0) {
            set_str(out_cfg->mem_server_addr, sizeof(out_cfg->mem_server_addr), value);
        } else if (strcmp(key, "cxl_device") == 0) {
            set_str(out_cfg->cxl_device, sizeof(out_cfg->cxl_device), value);
        } else if (strcmp(key, "ssd_device") == 0) {
            set_str(out_cfg->ssd_device, sizeof(out_cfg->ssd_device), value);
        } else if (strcmp(key, "ssd_devices") == 0) {
            /* Parse comma-separated "path:size,path:size,..." list */
            /* Supports size suffixes: G(GB), M(MB), K(KB) */
            char *buf = strdup(value);
            char *saveptr = NULL;
            char *token = strtok_r(buf, ",", &saveptr);
            while (token && out_cfg->num_ssd_devices < 16) {
                char *t = token;
                while (isspace((unsigned char)*t)) t++;
                /* Find size separator ':' */
                char *colon = strrchr(t, ':');
                if (colon) {
                    *colon = '\0';
                    char *path = trim(t);
                    char *size_str = trim(colon + 1);
                    strip_quotes(path);
                    uint64_t size_bytes = 0;
                    /* Parse size with suffix */
                    size_t sslen = strlen(size_str);
                    if (sslen > 0) {
                        char suffix = size_str[sslen - 1];
                        char *endptr = NULL;
                        unsigned long long num = strtoull(size_str, &endptr, 0);
                        if (endptr == size_str + sslen - 1 || endptr == size_str + sslen) {
                            switch (suffix) {
                            case 'G': case 'g': size_bytes = num * 1024ULL * 1024 * 1024; break;
                            case 'M': case 'm': size_bytes = num * 1024ULL * 1024; break;
                            case 'K': case 'k': size_bytes = num * 1024ULL; break;
                            default:  size_bytes = (uint64_t)num; break;
                            }
                        } else {
                            size_bytes = (uint64_t)num;
                        }
                    }
                    if (path[0] != '\0' && size_bytes > 0) {
                        uint32_t idx = out_cfg->num_ssd_devices;
                        set_str(out_cfg->ssd_devices[idx].path,
                                sizeof(out_cfg->ssd_devices[idx].path), path);
                        out_cfg->ssd_devices[idx].size = size_bytes;
                        out_cfg->num_ssd_devices++;
                    }
                }
                token = strtok_r(NULL, ",", &saveptr);
            }
            free(buf);
        } else if (strcmp(key, "listen_addr") == 0) {
            set_str(out_cfg->meta_server_addr, sizeof(out_cfg->meta_server_addr), value);
        } else if (strcmp(key, "listen_port") == 0) {
            int v;
            if (parse_int(value, &v) != UMM_OK || v < 0 || v > 65535) {
                rc = UMM_E_INVALID_ARG; break;
            }
            out_cfg->listen_port = (uint16_t)v;
        } else if (strcmp(key, "base_gpa") == 0) {
            unsigned long long v;
            if (parse_ull(value, &v) != UMM_OK) { rc = UMM_E_INVALID_ARG; break; }
            out_cfg->base_gpa = (uint64_t)v;
        } else if (strcmp(key, "my_node_id") == 0) {
            int v;
            if (parse_int(value, &v) != UMM_OK || v < 0) { rc = UMM_E_INVALID_ARG; break; }
            out_cfg->my_node_id = (node_id_t)v;
        } else if (strcmp(key, "nds_rpc_server_enable") == 0) {
            int v;
            if (parse_bool(value, &v) != UMM_OK) { rc = UMM_E_INVALID_ARG; break; }
            out_cfg->nds_rpc_server_enable = v;
        } else if (strcmp(key, "nds_rpc_server_ctrl") == 0) {
            set_str(out_cfg->nds_rpc_server_ctrl,
                    sizeof(out_cfg->nds_rpc_server_ctrl), value);
        } else if (strcmp(key, "nds_rpc_server_ns") == 0) {
            int v;
            if (parse_int(value, &v) != UMM_OK || v <= 0) { rc = UMM_E_INVALID_ARG; break; }
            out_cfg->nds_rpc_server_ns = (uint32_t)v;
        } else if (strcmp(key, "nds_rpc_server_qd") == 0) {
            int v;
            if (parse_int(value, &v) != UMM_OK || v <= 0) { rc = UMM_E_INVALID_ARG; break; }
            out_cfg->nds_rpc_server_qd = (uint32_t)v;
        } else if (strcmp(key, "nds_rpc_server_socket") == 0) {
            set_str(out_cfg->nds_rpc_server_socket,
                    sizeof(out_cfg->nds_rpc_server_socket), value);
        } else if (strcmp(key, "nds_rpc_server_keep_alive") == 0) {
            int v;
            if (parse_bool(value, &v) != UMM_OK) { rc = UMM_E_INVALID_ARG; break; }
            out_cfg->nds_rpc_server_keep_alive = v;
        } else if (strcmp(key, "peer_nodes") == 0) {
            set_str(out_cfg->peer_nodes, sizeof(out_cfg->peer_nodes), value);
        } else if (strcmp(key, "rpc_token") == 0) {
            set_str(out_cfg->rpc_token, sizeof(out_cfg->rpc_token), value);
        } else if (strcmp(key, "allow_cidrs") == 0) {
            set_str(out_cfg->allow_cidrs, sizeof(out_cfg->allow_cidrs), value);
        } else if (strcmp(key, "ssd_owner_node") == 0) {
            int v;
            if (parse_int(value, &v) != UMM_OK || v < 0 || v > 255) {
                rc = UMM_E_INVALID_ARG; break;
            }
            out_cfg->ssd_owner_node = (uint8_t)v;
        } else if (strcmp(key, "data_max_io") == 0) {
            unsigned long long v;
            if (parse_ull(value, &v) != UMM_OK) { rc = UMM_E_INVALID_ARG; break; }
            out_cfg->data_max_io = (uint32_t)v;
        }
        /* Unknown keys are silently ignored */
    }

    fclose(fp);
    return rc;
}
