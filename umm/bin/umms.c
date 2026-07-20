/* ========================================================================
 * umms — UMM Memory Server
 *
 * Standalone TCP server that hosts the memory service (allocation + tier mgmt).
 *
 * Usage:
 *   ./umms -c config/umms.yaml          # Config file mode (recommended)
 *   ./umms -p 20002 -n 0 -s 64M -d dir  # Command-line mode (legacy)
 *
 * Config file (YAML):
 *   node_id: 0
 *   listen_addr: "0.0.0.0"
 *   listen_port: 20002
 *   memory_size: 67108864
 *   base_gpa: 0
 *   ssd_devices: "/data/ssd0.raw:250G,/data/ssd1.raw:250G"
 * ======================================================================== */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <stdarg.h>

#include "../src/common/log.h"
#include "../src/common/error_codes.h"
#include "../src/common/config_parser.h"
#include "../include/umm.h"
#include "../src/memory_service/mem_service.h"
#include "../src/server/mem_server.h"

/* ------------------------------------------------------------------------ */
/* Logging wrappers                                                         */
/* ------------------------------------------------------------------------ */

static void daemon_log(int level, const char *file, int line,
                       const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(level, file, line, fmt, ap);
    va_end(ap);
}

#define DLOG_DEBUG(fmt, ...) \
    daemon_log(UMM_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DLOG_INFO(fmt, ...) \
    daemon_log(UMM_LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DLOG_WARN(fmt, ...) \
    daemon_log(UMM_LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DLOG_ERROR(fmt, ...) \
    daemon_log(UMM_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DLOG_FATAL(fmt, ...) \
    daemon_log(UMM_LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------------ */
/* Globals                                                                  */
/* ------------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;

/* ------------------------------------------------------------------------ */
/* Signal handler                                                           */
/* ------------------------------------------------------------------------ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------------ */
/* Print usage                                                              */
/* ------------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-c config.yaml] [-p port] [-n node_id] [-s memory_size] "
            "[-g base_gpa] [-b bind_addr] [-l log_level] [-d ssd_dir]\n"
            "  -c config    Config file (YAML, recommended)\n"
            "  -p port      Listening port        (default: 20002)\n"
            "  -n node_id   Node ID               (default: 0)\n"
            "  -s size      Memory size in bytes  (default: 1073741824 = 1GB)\n"
            "  -g gpa       Base GPA              (default: 0)\n"
            "  -b addr      Bind address          (default: 0.0.0.0)\n"
            "  -l level     Log level 0-4         (default: 1 = INFO)\n"
            "                 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=FATAL\n"
            "  -d dir       SSD backend directory (default: none)\n"
            "\n"
            "Config file example (umms.yaml):\n"
            "  node_id: 0\n"
            "  listen_addr: \"0.0.0.0\"\n"
            "  listen_port: 20002\n"
            "  memory_size: 67108864\n"
            "  ssd_devices: \"/data/ssd0.raw:250G,/data/ssd1.raw:250G\"\n",
            prog);
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int         port        = 20002;
    node_id_t   node_id     = 0;
    uint64_t    memory_size = 1073741824ULL;  /* 1 GB */
    uint64_t    base_gpa    = 0;
    const char *bind_addr   = "0.0.0.0";
    const char *ssd_dir     = NULL;
    const char *cfg_file    = NULL;
    int         log_level   = UMM_LOG_INFO;

    int opt;
    while ((opt = getopt(argc, argv, "c:p:n:s:g:b:l:d:h")) != -1) {
        switch (opt) {
        case 'c':
            cfg_file = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'n':
            node_id = (node_id_t)atoi(optarg);
            break;
        case 's':
            memory_size = strtoull(optarg, NULL, 0);
            if (memory_size == 0) {
                fprintf(stderr, "Invalid memory size: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'g':
            base_gpa = strtoull(optarg, NULL, 0);
            break;
        case 'b':
            bind_addr = optarg;
            break;
        case 'l':
            log_level = atoi(optarg);
            if (log_level < UMM_LOG_DEBUG || log_level > UMM_LOG_FATAL) {
                fprintf(stderr, "Invalid log level: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'd':
            ssd_dir = optarg;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    /* ---- Load config file if provided ---- */
    UMMConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    uint32_t cfg_num_ssd = 0;

    if (cfg_file) {
        if (umm_parse_config(cfg_file, &cfg) == UMM_OK) {
            /* Override defaults with config values */
            if (cfg.my_node_id != 0)     node_id     = cfg.my_node_id;
            if (cfg.memory_size > 0)     memory_size = cfg.memory_size;
            if (cfg.base_gpa != 0)       base_gpa    = cfg.base_gpa;
            if (cfg.listen_port > 0)     port        = cfg.listen_port;
            if (cfg.meta_server_addr[0]) bind_addr   = cfg.meta_server_addr;

            /* Build SSD device list from config */
            if (cfg.num_ssd_devices > 0) {
                cfg_num_ssd = cfg.num_ssd_devices;
            } else if (cfg.ssd_device[0] != '\0') {
                /* Legacy single device → convert to list */
                cfg_num_ssd = 1;
                snprintf(cfg.ssd_devices[0].path,
                         sizeof(cfg.ssd_devices[0].path), "%s",
                         cfg.ssd_device);
                cfg.ssd_devices[0].size = memory_size;
            }

            /* Command-line -d overrides config */
            if (ssd_dir && cfg_num_ssd > 0) {
                snprintf(cfg.ssd_devices[0].path,
                         sizeof(cfg.ssd_devices[0].path), "%s", ssd_dir);
                cfg.ssd_devices[0].size = memory_size;
                cfg_num_ssd = 1;
            } else if (!ssd_dir && cfg_num_ssd > 0) {
                ssd_dir = cfg.ssd_devices[0].path;  /* legacy hint */
            }

            DLOG_INFO("Loaded config: %s (node=%u, bind=%s:%d, mem=%lu, "
                      "ssd_devices=%u)",
                      cfg_file, (unsigned)node_id, bind_addr, port,
                      (unsigned long)memory_size, cfg_num_ssd);
        } else {
            DLOG_WARN("Failed to parse config: %s, using defaults", cfg_file);
        }
    }

    /* ---- Logging ---- */
    umm_log_set_level(log_level);
    DLOG_INFO("umms starting: bind=%s port=%d node=%u size=%lu base_gpa=0x%lx "
              "ssd=%s",
              bind_addr, port, (unsigned)node_id,
              (unsigned long)memory_size, (unsigned long)base_gpa,
              ssd_dir ? ssd_dir : "(none)");

    /* ---- Signal handlers ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        DLOG_FATAL("sigaction(SIGINT) failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        DLOG_FATAL("sigaction(SIGTERM) failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    /* ---- Create and start server ---- */
    MemServer *server;
    if (cfg_num_ssd > 0) {
        server = mem_server_create_multi(bind_addr, port,
                                          node_id, memory_size, base_gpa,
                                          ssd_dir,
                                          cfg.ssd_devices, cfg_num_ssd);
        DLOG_INFO("Creating memory server with %u SSD device(s)", cfg_num_ssd);
    } else {
        server = mem_server_create(bind_addr, port,
                                    node_id, memory_size, base_gpa,
                                    ssd_dir);
    }
    if (!server) {
        DLOG_FATAL("Failed to create memory server");
        return EXIT_FAILURE;
    }

    if (mem_server_start(server) != UMM_OK) {
        DLOG_FATAL("Failed to start memory server");
        mem_server_destroy(server);
        return EXIT_FAILURE;
    }

    DLOG_INFO("umms running. Press Ctrl-C to stop.");

    /* ---- Main loop ---- */
    while (g_running) {
        sleep(1);
    }

    DLOG_INFO("umms shutting down...");

    /* ---- Shutdown ---- */
    mem_server_stop(server);
    mem_server_destroy(server);

    DLOG_INFO("umms exited cleanly");
    return EXIT_SUCCESS;
}
