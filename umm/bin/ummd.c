/* ========================================================================
 * ummd – UMM Metadata Daemon
 *
 * Standalone TCP server that hosts the metadata service.
 *
 * Usage:
 *   ./ummd -c config/ummd.yaml          # Config file mode (recommended)
 *   ./ummd -p 20001 -b 0.0.0.0          # Command-line mode (legacy)
 *
 * Config file (YAML):
 *   listen_addr: "0.0.0.0"
 *   listen_port: 20001
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
#include "../src/metadata_service/meta_service.h"
#include "../src/metadata_service/meta_service_direct.h"
#include "../src/server/meta_server.h"

/* ------------------------------------------------------------------------ */
/* Local variadic logging wrappers                                          */
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
            "Usage: %s [-c config.yaml] [-p port] [-b bind_addr] [-l log_level]\n"
            "  -c config    Config file (YAML, recommended)\n"
            "  -p port      Listening port  (default: 20001)\n"
            "  -b addr      Bind address    (default: 0.0.0.0)\n"
            "  -l level     Log level 0-4   (default: 1 = INFO)\n"
            "                 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=FATAL\n"
            "\n"
            "Config file example (ummd.yaml):\n"
            "  listen_addr: \"0.0.0.0\"\n"
            "  listen_port: 20001\n",
            prog);
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int         port      = 20001;
    const char *bind_addr = "0.0.0.0";
    int         log_level = UMM_LOG_INFO;
    const char *cfg_file  = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:p:b:l:h")) != -1) {
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
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    /* ---- Load config file if provided ---- */
    UMMConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (cfg_file) {
        if (umm_parse_config(cfg_file, &cfg) == UMM_OK) {
            /* listen_addr from config */
            if (cfg.meta_server_addr[0] != '\0')
                bind_addr = cfg.meta_server_addr;
            /* listen_port from config */
            if (cfg.listen_port > 0)
                port = cfg.listen_port;
            DLOG_INFO("Loaded config: %s (bind=%s, port=%d)",
                      cfg_file, bind_addr, port);
        } else {
            DLOG_WARN("Failed to parse config: %s, using defaults", cfg_file);
        }
    }

    /* ---- Logging ---- */
    umm_log_set_level(log_level);
    DLOG_INFO("ummd starting: bind=%s port=%d log_level=%d",
              bind_addr, port, log_level);

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

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* ---- Create metadata service ---- */
    void *meta_ctx = NULL;
    MetadataServiceVtbl *vtbl = meta_service_direct_create(&meta_ctx);
    if (!vtbl || !meta_ctx) {
        DLOG_FATAL("Failed to create metadata service");
        return EXIT_FAILURE;
    }
    DLOG_INFO("Metadata service created");

    /* ---- Create and start server ---- */
    MetaServer *server = meta_server_create(bind_addr, port, vtbl, meta_ctx);
    if (!server) {
        DLOG_FATAL("Failed to create metadata server");
        meta_service_direct_destroy(meta_ctx);
        return EXIT_FAILURE;
    }

    if (meta_server_start(server) != UMM_OK) {
        DLOG_FATAL("Failed to start metadata server");
        meta_server_destroy(server);
        meta_service_direct_destroy(meta_ctx);
        return EXIT_FAILURE;
    }

    DLOG_INFO("ummd running. Press Ctrl-C to stop.");

    /* ---- Main loop ---- */
    while (g_running) {
        sleep(1);
    }

    DLOG_INFO("ummd shutting down...");

    /* ---- Shutdown ---- */
    meta_server_stop(server);
    meta_server_destroy(server);
    meta_service_direct_destroy(meta_ctx);

    DLOG_INFO("ummd exited cleanly");
    return EXIT_SUCCESS;
}
