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
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

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

/* 托管的 NDS RPC server（Process A）子进程 PID，-1 = 未托管 */
static pid_t g_rpc_child = -1;

/* keep_alive=false 时的退出钩子：SIGTERM 子进程并回收。
 * SIGINT/SIGTERM 仅置 g_running=0，主循环退出后走正常 return →
 * atexit 触发本钩子，与现有信号处理兼容（信号 handler 内不做 wait）。 */
static void rpc_child_atexit(void)
{
    if (g_rpc_child <= 0)
        return;
    kill(g_rpc_child, SIGTERM);
    for (int i = 0; i < 50; i++) {          /* 最多等 5s */
        int st;
        if (waitpid(g_rpc_child, &st, WNOHANG) == g_rpc_child)
            break;
        usleep(100 * 1000);
    }
    kill(g_rpc_child, SIGKILL);             /* 兜底（已退出则无害） */
    waitpid(g_rpc_child, NULL, 0);
    g_rpc_child = -1;
}

/* wait_ready：每 100ms 轮询 connect() 到 RPC unix socket，成功即
 * ready（随即 close——仅探测 listen 是否就绪；stale socket 文件存在
 * 但无监听时 connect 返回 ECONNREFUSED，可区分，继续轮询）。
 * 同时检测子进程早夭（exec 失败/启动即崩）提前 fail。
 * 返回 0=ready，-1=超时，-2=子进程已退出 */
static int nds_rpc_wait_ready(const char *sock_path, pid_t child,
                              uint64_t timeout_ms)
{
    if (strlen(sock_path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, sock_path);

    uint64_t waited = 0;
    while (waited < timeout_ms) {
        int st;
        pid_t r = waitpid(child, &st, WNOHANG);
        if (r == child)
            return -2;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                close(fd);
                return 0;
            }
            close(fd);
        }
        usleep(100 * 1000);
        waited += 100;
    }
    return -1;
}

/* 定位 helper：env UMM_NDS_RPC_SERVER_BIN 覆盖 → 缺省
 * /proc/self/exe 同目录下 umm_nds_rpc_server */
static int nds_rpc_locate_helper(char *out, size_t out_size)
{
    const char *env = getenv("UMM_NDS_RPC_SERVER_BIN");
    if (env && env[0]) {
        snprintf(out, out_size, "%s", env);
        return 0;
    }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return -1;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    int w = snprintf(out, out_size, "%s/%s", exe, "umm_nds_rpc_server");
    if (w < 0 || (size_t)w >= out_size)
        return -1;
    return 0;
}

/* NDS RPC server（Process A）托管拉起。
 *
 * 设计取舍：库层（libumm.so）绝不拉起特权进程——多进程各自 dlopen
 * libumm.so 会产生拉起竞态（谁 fork？重复 bind 控制器？），且
 * 持有 NVMe 控制器需要特权，客户端进程权限模型不一，进程生命周期
 * 倒挂（server 应长于任一客户端）。托管只发生在 umms 服务层：
 * umms 是部署形态中唯一的长驻特权服务进程，启动顺序天然正确。
 *
 * 返回 0=成功（或未启用），-1=fatal（调用方应退出） */
static int nds_rpc_server_bootstrap(const UMMConfig *cfg, uint32_t num_ssd)
{
    if (!cfg->nds_rpc_server_enable)
        return 0;

    /* 仅当 SSD 设备列表含 nds: 设备时才拉起，否则配置无意义 */
    int has_nds = 0;
    for (uint32_t i = 0; i < num_ssd; i++)
        if (strncmp(cfg->ssd_devices[i].path, "nds:", 4) == 0)
            has_nds = 1;
    if (!has_nds) {
        DLOG_WARN("nds_rpc_server_enable=true 但 ssd_devices 不含 nds: "
                  "设备，跳过 RPC server 托管");
        return 0;
    }

    const char *ctrl = cfg->nds_rpc_server_ctrl[0]
                       ? cfg->nds_rpc_server_ctrl : "/dev/libnvm_helper0";
    const char *sock = cfg->nds_rpc_server_socket[0]
                       ? cfg->nds_rpc_server_socket : "/tmp/nvm_host_rpc.sock";
    uint32_t ns = cfg->nds_rpc_server_ns ? cfg->nds_rpc_server_ns : 1;
    uint32_t qd = cfg->nds_rpc_server_qd ? cfg->nds_rpc_server_qd : 64;

    /* 配置→env 一致性：同进程 ssd_backend_nds 的 RPC 引导读
     * UMM_NDS_RPC_SOCKET，必须与托管 server 的 socket 一致 */
    const char *env_sock = getenv("UMM_NDS_RPC_SOCKET");
    if (!env_sock || !env_sock[0]) {
        setenv("UMM_NDS_RPC_SOCKET", sock, 0);
        DLOG_INFO("nds: UMM_NDS_RPC_SOCKET 未设置，已按配置 setenv 为 %s",
                  sock);
    } else if (strcmp(env_sock, sock) != 0) {
        DLOG_WARN("nds: UMM_NDS_RPC_SOCKET=%s 与配置 "
                  "nds_rpc_server_socket=%s 不一致，以 env 为准"
                  "（二选一：改配置或 unset env）", env_sock, sock);
    }

    char helper[4096];
    if (nds_rpc_locate_helper(helper, sizeof(helper)) != 0 ||
        access(helper, X_OK) != 0) {
        DLOG_FATAL("nds: 找不到 RPC server helper %s：先 make tools 构建 "
                   "bin/umm_nds_rpc_server，或用 env UMM_NDS_RPC_SERVER_BIN "
                   "指定完整路径", helper);
        return -1;
    }

    char ns_str[16], qd_str[16];
    snprintf(ns_str, sizeof(ns_str), "%u", (unsigned)ns);
    snprintf(qd_str, sizeof(qd_str), "%u", (unsigned)qd);

    pid_t pid = fork();
    if (pid < 0) {
        DLOG_FATAL("nds: fork RPC server 失败: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* 子进程：继承环境（UMM_LIBNVM_PATH 等）与 umms 日志流
         *（stdout/stderr 不重定向），exec 失败立即退出 */
        execl(helper, "umm_nds_rpc_server",
              "--ctrl", ctrl, "--ns", ns_str, "--qd", qd_str,
              "--socket", sock, (char *)NULL);
        fprintf(stderr, "[umms] exec(%s) failed: %s\n",
                helper, strerror(errno));
        _exit(127);
    }
    g_rpc_child = pid;
    DLOG_INFO("nds: spawned RPC server (Process A) pid=%d: %s --ctrl %s "
              "--ns %s --qd %s --socket %s",
              (int)pid, helper, ctrl, ns_str, qd_str, sock);

    /* wait_ready（缺省 10000ms，env UMM_NDS_RPC_READY_TIMEOUT_MS 可调） */
    uint64_t timeout_ms = 10000;
    const char *e = getenv("UMM_NDS_RPC_READY_TIMEOUT_MS");
    if (e && e[0]) {
        unsigned long long v = strtoull(e, NULL, 10);
        if (v)
            timeout_ms = (uint64_t)v;
    }
    int wrc = nds_rpc_wait_ready(sock, pid, timeout_ms);
    if (wrc != 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        g_rpc_child = -1;
        if (wrc == -2)
            DLOG_FATAL("nds: RPC server (pid=%d) 启动后立即退出：检查 "
                       "UMM_LIBNVM_PATH、控制器 %s 路径与访问权限、"
                       "libnvm_helper 内核模块", (int)pid, ctrl);
        else
            DLOG_FATAL("nds: RPC server (pid=%d) %lu ms 内未就绪 "
                       "(socket=%s)：检查 UMM_LIBNVM_PATH、控制器 %s "
                       "路径与访问权限",
                       (int)pid, (unsigned long)timeout_ms, sock, ctrl);
        return -1;
    }
    DLOG_INFO("nds: RPC server ready (pid=%d, socket=%s)", (int)pid, sock);

    if (cfg->nds_rpc_server_keep_alive) {
        DLOG_INFO("nds: RPC server keep_alive=true：umms 退出后 server "
                  "(pid=%d) 保留运行", (int)pid);
        g_rpc_child = -1;   /* 不纳管退出清理 */
    } else {
        atexit(rpc_child_atexit);
        DLOG_INFO("nds: RPC server keep_alive=false：umms 退出时将 "
                  "SIGTERM 子进程 (pid=%d)", (int)pid);
    }
    return 0;
}

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
            "  memory_tier: \"cxl\"   # cxl(默认) / dram(Phase 2 混合池, 无 CXL 硬件)\n"
            "  memory_device: \"/dev/pmem0\"  # 可选(Phase 2.5 共享内存窗口);\n"
            "                                 # 不配 = malloc/mock 私有后备\n"
            "  ssd_devices: \"/data/ssd0.raw:250G,/data/ssd1.raw:250G\"\n",
            prog);
}

/* ------------------------------------------------------------------------ */
/* memory_tier 是 umms 专属配置（不进 UMMConfig ABI），config_parser 对     */
/* 未知 key 静默忽略，故此处自行做行级扫描。合法值：cxl(默认) / dram。       */
/* ------------------------------------------------------------------------ */

static tier_id_t parse_memory_tier_key(const char *cfg_file)
{
    if (!cfg_file)
        return UMM_TIER_CXL;
    FILE *fp = fopen(cfg_file, "r");
    if (!fp)
        return UMM_TIER_CXL;
    char line[512];
    tier_id_t tier = UMM_TIER_CXL;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "memory_tier", 11) != 0)
            continue;
        p += 11;
        while (*p == ' ' || *p == '\t' || *p == ':') p++;
        /* 去引号与行尾 */
        if (*p == '"' || *p == '\'') p++;
        if (strncmp(p, "dram", 4) == 0)
            tier = UMM_TIER_DRAM;
        else if (strncmp(p, "cxl", 3) == 0)
            tier = UMM_TIER_CXL;
        else
            fprintf(stderr, "umms: 未知 memory_tier 值，按 cxl 处理: %s", p);
        break;
    }
    fclose(fp);
    return tier;
}

/* ------------------------------------------------------------------------ */
/* memory_device 同样是 umms 专属配置（行级扫描，理由同 memory_tier）。       */
/* 内存层后备设备（Phase 2.5 共享内存窗口，如 virtio-pmem 的 /dev/pmem0）；   */
/* 未配置返回 NULL = 旧行为（CXL mock / DRAM malloc 私有后备）。              */
/* ------------------------------------------------------------------------ */

static const char *parse_memory_device_key(const char *cfg_file,
                                           char *out, size_t out_sz)
{
    if (!cfg_file || !out || out_sz == 0)
        return NULL;
    FILE *fp = fopen(cfg_file, "r");
    if (!fp)
        return NULL;
    char line[512];
    const char *ret = NULL;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "memory_device", 13) != 0)
            continue;
        p += 13;
        while (*p == ' ' || *p == '\t' || *p == ':') p++;
        if (*p == '"' || *p == '\'') p++;
        char *end = p;
        while (*end && *end != '"' && *end != '\'' &&
               *end != '\n' && *end != '\r' && *end != '#' &&
               *end != ' ' && *end != '\t') end++;
        size_t len = (size_t)(end - p);
        if (len > 0 && len < out_sz) {
            memcpy(out, p, len);
            out[len] = '\0';
            ret = out;
        }
        break;
    }
    fclose(fp);
    return ret;
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

    /* ---- NDS RPC server (Process A) 托管：解析配置之后、memory
     * service/storage 注册之前（storage 注册时 nds 设备 open 需要
     * RPC server 已就绪） ---- */
    if (nds_rpc_server_bootstrap(&cfg, cfg_num_ssd) != 0)
        return EXIT_FAILURE;

    /* ---- Create and start server ---- */
    tier_id_t mem_tier = parse_memory_tier_key(cfg_file);
    char mem_device_buf[256];
    const char *mem_device = parse_memory_device_key(cfg_file,
                                                     mem_device_buf,
                                                     sizeof(mem_device_buf));
    MemServer *server;
    if (cfg_num_ssd > 0) {
        /* 显式设备列表场景传 NULL legacy hint——否则
         * mem_server_create 会先以 memory_size 容量注册一遍
         * ssd_dir 设备，create_multi 再注册一遍显式列表：
         * 同一设备进池两次，pool 虚拟空间虚增且区间互相重叠 */
        server = mem_server_create_multi_tiered(bind_addr, port,
                                                 node_id, memory_size,
                                                 base_gpa, mem_tier,
                                                 mem_device,
                                                 cfg.ssd_devices, cfg_num_ssd);
        DLOG_INFO("Creating memory server with %u SSD device(s), mem_tier=%s%s%s",
                  cfg_num_ssd, mem_tier == UMM_TIER_DRAM ? "dram" : "cxl",
                  mem_device ? ", mem_device=" : "",
                  mem_device ? mem_device : "");
    } else {
        if (mem_tier == UMM_TIER_DRAM)
            DLOG_WARN("memory_tier=dram 需要 ssd_devices 显式列表，"
                      "legacy ssd_dir 路径按 cxl 处理");
        server = mem_server_create(bind_addr, port,
                                    node_id, memory_size, base_gpa,
                                    ssd_dir);
    }
    if (!server) {
        DLOG_FATAL("Failed to create memory server");
        return EXIT_FAILURE;
    }

    /* ---- Phase 1 安全配置：token / CIDR 白名单 / 数据面帧上限 ---- */
    if (mem_server_set_security(server,
                                cfg.rpc_token[0]    ? cfg.rpc_token    : NULL,
                                cfg.allow_cidrs[0]  ? cfg.allow_cidrs  : NULL,
                                cfg.data_max_io) != UMM_OK) {
        DLOG_WARN("mem_server_set_security failed, running WITHOUT "
                  "token/ACL protection");
    } else if (cfg.rpc_token[0] == '\0' && cfg.allow_cidrs[0] == '\0') {
        DLOG_WARN("rpc_token / allow_cidrs 均未配置：数据面端口无保护，"
                  "仅限可信内网（跨节点部署请务必配置）");
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
