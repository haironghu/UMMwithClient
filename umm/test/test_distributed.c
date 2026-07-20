/* ========================================================================
 * test_distributed.c — Distributed multi-node CXL memory sharing test
 *
 * Scenario:
 *   - Node 0 allocates a chunk, writes data, registers with ummd
 *   - Node 1 looks up the chunk by name via ummd
 *   - Node 1 reads the data directly through CXL fabric (mock)
 *
 * Architecture (all in one process via threads):
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  Main Process                                                │
 *   │                                                              │
 *   │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
 *   │  │  ummd       │  │  umms-0     │  │  umms-1     │        │
 *   │  │  port 20001 │  │  port 20002 │  │  port 20003 │        │
 *   │  │             │  │  node=0     │  │  node=1     │        │
 *   │  │             │  │  mem=64MB   │  │  mem=64MB   │        │
 *   │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘        │
 *   │         │                │                │                │
 *   │         └────────────────┴────────────────┘                │
 *   │                          │                                 │
 *   │         ┌────────────────┴────────────────┐                │
 *   │         ▼                                 ▼                │
 *   │  ┌─────────────┐                   ┌─────────────┐        │
 *   │  │ Client-0    │                   │ Client-1    │        │
 *   │  │ node=0      │                   │ node=1      │        │
 *   │  │ alloc       │                   │ lookup      │        │
 *   │  │ write data  │──→ ummd ←─────────│ read data   │        │
 *   │  │             │   (chunk name)    │             │        │
 *   │  └─────────────┘                   └─────────────┘        │
 *   └─────────────────────────────────────────────────────────────┘
 * ======================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

#include "../include/umm.h"
#include "../src/common/log.h"
#include "../src/common/umm_network.h"
#include "../src/metadata_service/meta_service.h"
#include "../src/metadata_service/meta_service_direct.h"
#include "../src/metadata_service/meta_service_rpc_server.h"
#include "../src/memory_service/mem_service.h"
#include "../src/memory_service/mem_service_direct.h"
#include "../src/memory_service/mem_service_rpc_server.h"
#include "../src/server/meta_server.h"
#include "../src/server/mem_server.h"

#include "test_framework.h"

/* ==================================================================== */
/* Test parameters                                                      */
/* ==================================================================== */

#define UMMD_PORT       20001
#define UMMS0_PORT      20002
#define UMMS1_PORT      20003
#define MEM_SIZE        (64ULL * 1024 * 1024)  /* 64 MiB per node */
#define NODE0_NAME      "node0"
#define NODE1_NAME      "node1"

static volatile int g_servers_running = 1;

/* ==================================================================== */
/* Server thread wrappers                                               */
/* ==================================================================== */

typedef struct {
    int        port;
    node_id_t  node_id;
    uint64_t   mem_size;
} umms_args_t;

/* ummd thread: metadata directory server */
static void* ummd_thread(void *arg)
{
    (void)arg;

    void *ctx = NULL;
    MetadataServiceVtbl *vtbl = meta_service_direct_create(&ctx);
    if (!vtbl) {
        fprintf(stderr, "[ummd] failed to create metadata service\n");
        return NULL;
    }

    MetaServer *server = meta_server_create("127.0.0.1", UMMD_PORT, vtbl, ctx);
    if (!server) {
        fprintf(stderr, "[ummd] failed to create server\n");
        meta_service_direct_destroy(ctx);
        return NULL;
    }

    if (meta_server_start(server) != 0) {
        fprintf(stderr, "[ummd] failed to start server\n");
        meta_server_destroy(server);
        meta_service_direct_destroy(ctx);
        return NULL;
    }

    /* Wait for shutdown signal */
    while (g_servers_running)
        usleep(100000);  /* 100ms */

    meta_server_stop(server);
    meta_server_destroy(server);
    meta_service_direct_destroy(ctx);
    return NULL;
}

/* umms thread: memory server for one node */
static void* umms_thread(void *arg)
{
    umms_args_t *args = (umms_args_t *)arg;

    MemServer *server = mem_server_create("127.0.0.1", args->port,
                                           args->node_id, args->mem_size, 0);
    if (!server) {
        fprintf(stderr, "[umms-%u] failed to create server\n", args->node_id);
        return NULL;
    }

    if (mem_server_start(server) != 0) {
        fprintf(stderr, "[umms-%u] failed to start server\n", args->node_id);
        mem_server_destroy(server);
        return NULL;
    }

    /* Wait for shutdown signal */
    while (g_servers_running)
        usleep(100000);

    mem_server_stop(server);
    mem_server_destroy(server);
    return NULL;
}

/* ==================================================================== */
/* Helper: wait for a TCP port to become ready                          */
/* ==================================================================== */
static int wait_for_port(int port, int timeout_ms)
{
    int sock;
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (umm_tcp_connect("127.0.0.1", port, &sock) == 0) {
            umm_tcp_close(sock);
            return 0;
        }
        usleep(50000);  /* 50ms */
        elapsed += 50;
    }
    return -1;
}

/* ==================================================================== */
/* Helper: get auto-generated chunk name from descriptor                */
/* ==================================================================== */
static void get_chunk_name(const ChunkDescriptor *desc, char *out, size_t out_len)
{
    uint64_t offset = gpa_to_offset(desc->base_gpa);
    node_id_t node = gpa_to_node(desc->base_gpa);
    snprintf(out, out_len, "chunk_%u_%lu", (unsigned)node, (unsigned long)offset);
}

/* ==================================================================== */
/* Test 1: Node 0 allocates, Node 1 can see it in ummd                  */
/* ==================================================================== */
TEST(node0_alloc_node1_lookup)
{
    /* ---- Node 0: allocate chunk ---- */
    UMMConfig cfg0;
    memset(&cfg0, 0, sizeof(cfg0));
    strncpy(cfg0.transport, "mock", sizeof(cfg0.transport) - 1);
    strncpy(cfg0.consistency_model, "hardware", sizeof(cfg0.consistency_model) - 1);
    cfg0.memory_size      = MEM_SIZE;
    cfg0.my_node_id       = 0;
    snprintf(cfg0.meta_server_addr, sizeof(cfg0.meta_server_addr),
             "127.0.0.1:%d", UMMD_PORT);
    snprintf(cfg0.mem_server_addr, sizeof(cfg0.mem_server_addr),
             "127.0.0.1:%d", UMMS0_PORT);

    int rc = umm_init(&cfg0);
    ASSERT_EQ(rc, UMM_OK);

    /* Allocate a named chunk on Node 0 */
    ChunkDescriptor desc0;
    memset(&desc0, 0, sizeof(desc0));
    rc = umm_alloc(4096, &desc0);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc0.chunk_id, (chunk_id_t)0);

    /* Write data */
    const char *msg = "Hello from Node 0 via CXL!";
    rc = umm_write(&desc0, 0, strlen(msg) + 1, msg);
    ASSERT_EQ(rc, UMM_OK);

    /* Get the chunk name */
    char chunk_name[64];
    get_chunk_name(&desc0, chunk_name, sizeof(chunk_name));

    /* ---- Node 1: look up the chunk via ummd ---- */
    UMMConfig cfg1;
    memset(&cfg1, 0, sizeof(cfg1));
    strncpy(cfg1.transport, "mock", sizeof(cfg1.transport) - 1);
    strncpy(cfg1.consistency_model, "hardware", sizeof(cfg1.consistency_model) - 1);
    cfg1.memory_size      = MEM_SIZE;
    cfg1.my_node_id       = 1;
    snprintf(cfg1.meta_server_addr, sizeof(cfg1.meta_server_addr),
             "127.0.0.1:%d", UMMD_PORT);
    snprintf(cfg1.mem_server_addr, sizeof(cfg1.mem_server_addr),
             "127.0.0.1:%d", UMMS1_PORT);

    rc = umm_init(&cfg1);
    ASSERT_EQ(rc, UMM_OK);

    /* Lookup chunk by name — this goes through ummd */
    ChunkMetadata meta;
    memset(&meta, 0, sizeof(meta));
    rc = umm_lookup_chunk(chunk_name, &meta);
    ASSERT_EQ(rc, UMM_OK);

    /* Verify metadata matches */
    ASSERT_EQ(meta.chunk_id, desc0.chunk_id);
    ASSERT_EQ(meta.gpa,      desc0.base_gpa);
    ASSERT_EQ(meta.size,     desc0.user_size);

    /* Verify GPA shows it's on Node 0 */
    node_id_t home = gpa_to_node(meta.gpa);
    ASSERT_EQ(home, (node_id_t)0);

    /* ---- Node 1: read data directly through CXL fabric ---- */
    /* Node 1 creates a descriptor from the metadata and reads */
    ChunkDescriptor desc1;
    memset(&desc1, 0, sizeof(desc1));
    desc1.chunk_id  = meta.chunk_id;
    desc1.base_gpa  = meta.gpa;
    desc1.user_size = meta.size;

    char read_buf[256];
    memset(read_buf, 0, sizeof(read_buf));
    rc = umm_read(&desc1, 0, strlen(msg) + 1, read_buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(read_buf, msg), 0);

    /* Cleanup */
    umm_free(&desc0);
    umm_deinit();  /* Node 0 client */
    umm_deinit();  /* Node 1 client — wait, this deinits global state */
}

/* ==================================================================== */
/* Test 2: Multiple chunks across nodes, all visible in ummd              */
/* ==================================================================== */
TEST(multi_node_chunks)
{
    /* Node 0: allocate 3 chunks */
    UMMConfig cfg0;
    memset(&cfg0, 0, sizeof(cfg0));
    strncpy(cfg0.transport, "mock", sizeof(cfg0.transport) - 1);
    strncpy(cfg0.consistency_model, "hardware", sizeof(cfg0.consistency_model) - 1);
    cfg0.memory_size      = MEM_SIZE;
    cfg0.my_node_id       = 0;
    snprintf(cfg0.meta_server_addr, sizeof(cfg0.meta_server_addr),
             "127.0.0.1:%d", UMMD_PORT);
    snprintf(cfg0.mem_server_addr, sizeof(cfg0.mem_server_addr),
             "127.0.0.1:%d", UMMS0_PORT);

    int rc = umm_init(&cfg0);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor n0_chunks[3];
    char n0_names[3][64];
    for (int i = 0; i < 3; i++) {
        rc = umm_alloc(4096, &n0_chunks[i]);
        ASSERT_EQ(rc, UMM_OK);
        get_chunk_name(&n0_chunks[i], n0_names[i], 64);

        uint64_t val = 0xA0000000ULL + (uint64_t)i;
        rc = umm_write(&n0_chunks[i], 0, sizeof(val), &val);
        ASSERT_EQ(rc, UMM_OK);
    }

    /* Node 1: allocate 2 chunks */
    UMMConfig cfg1;
    memset(&cfg1, 0, sizeof(cfg1));
    strncpy(cfg1.transport, "mock", sizeof(cfg1.transport) - 1);
    strncpy(cfg1.consistency_model, "hardware", sizeof(cfg1.consistency_model) - 1);
    cfg1.memory_size      = MEM_SIZE;
    cfg1.my_node_id       = 1;
    snprintf(cfg1.meta_server_addr, sizeof(cfg1.meta_server_addr),
             "127.0.0.1:%d", UMMD_PORT);
    snprintf(cfg1.mem_server_addr, sizeof(cfg1.mem_server_addr),
             "127.0.0.1:%d", UMMS1_PORT);

    rc = umm_init(&cfg1);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor n1_chunks[2];
    char n1_names[2][64];
    for (int i = 0; i < 2; i++) {
        rc = umm_alloc(4096, &n1_chunks[i]);
        ASSERT_EQ(rc, UMM_OK);
        get_chunk_name(&n1_chunks[i], n1_names[i], 64);

        uint64_t val = 0xB0000000ULL + (uint64_t)i;
        rc = umm_write(&n1_chunks[i], 0, sizeof(val), &val);
        ASSERT_EQ(rc, UMM_OK);
    }

    /* Node 0 looks up Node 1's chunks */
    for (int i = 0; i < 2; i++) {
        ChunkMetadata meta;
        rc = umm_lookup_chunk(n1_names[i], &meta);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(meta.chunk_id, n1_chunks[i].chunk_id);
        ASSERT_EQ(gpa_to_node(meta.gpa), (node_id_t)1);

        /* Read Node 1's data from Node 0 */
        uint64_t val = 0;
        ChunkDescriptor tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.base_gpa  = meta.gpa;
        tmp.user_size = meta.size;
        rc = umm_read(&tmp, 0, sizeof(val), &val);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(val, 0xB0000000ULL + (uint64_t)i);
    }

    /* Node 1 looks up Node 0's chunks */
    for (int i = 0; i < 3; i++) {
        ChunkMetadata meta;
        rc = umm_lookup_chunk(n0_names[i], &meta);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(meta.chunk_id, n0_chunks[i].chunk_id);
        ASSERT_EQ(gpa_to_node(meta.gpa), (node_id_t)0);

        uint64_t val = 0;
        ChunkDescriptor tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.base_gpa  = meta.gpa;
        tmp.user_size = meta.size;
        rc = umm_read(&tmp, 0, sizeof(val), &val);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(val, 0xA0000000ULL + (uint64_t)i);
    }

    /* Cleanup */
    for (int i = 0; i < 3; i++) umm_free(&n0_chunks[i]);
    for (int i = 0; i < 2; i++) umm_free(&n1_chunks[i]);
    umm_deinit();
}

/* ==================================================================== */
/* Test 3: Large data transfer across nodes                               */
/* ==================================================================== */
TEST(cross_node_large_transfer)
{
    /* Node 0: allocate 1MB, fill with pattern */
    UMMConfig cfg0;
    memset(&cfg0, 0, sizeof(cfg0));
    strncpy(cfg0.transport, "mock", sizeof(cfg0.transport) - 1);
    strncpy(cfg0.consistency_model, "hardware", sizeof(cfg0.consistency_model) - 1);
    cfg0.memory_size      = MEM_SIZE;
    cfg0.my_node_id       = 0;
    snprintf(cfg0.meta_server_addr, sizeof(cfg0.meta_server_addr),
             "127.0.0.1:%d", UMMD_PORT);
    snprintf(cfg0.mem_server_addr, sizeof(cfg0.mem_server_addr),
             "127.0.0.1:%d", UMMS0_PORT);

    int rc = umm_init(&cfg0);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc(1024ULL * 1024, &desc);  /* 1 MB */
    ASSERT_EQ(rc, UMM_OK);

    /* Fill with pattern */
    uint8_t *write_buf = malloc(1024 * 1024);
    ASSERT_NOT_NULL(write_buf);
    for (size_t i = 0; i < 1024ULL * 1024; i++)
        write_buf[i] = (uint8_t)((i * 7 + 3) & 0xFF);

    rc = umm_write(&desc, 0, 1024ULL * 1024, write_buf);
    ASSERT_EQ(rc, UMM_OK);

    char chunk_name[64];
    get_chunk_name(&desc, chunk_name, sizeof(chunk_name));

    /* Node 1: look up and read */
    UMMConfig cfg1;
    memset(&cfg1, 0, sizeof(cfg1));
    strncpy(cfg1.transport, "mock", sizeof(cfg1.transport) - 1);
    strncpy(cfg1.consistency_model, "hardware", sizeof(cfg1.consistency_model) - 1);
    cfg1.memory_size      = MEM_SIZE;
    cfg1.my_node_id       = 1;
    snprintf(cfg1.meta_server_addr, sizeof(cfg1.meta_server_addr),
             "127.0.0.1:%d", UMMD_PORT);
    snprintf(cfg1.mem_server_addr, sizeof(cfg1.mem_server_addr),
             "127.0.0.1:%d", UMMS1_PORT);

    rc = umm_init(&cfg1);
    ASSERT_EQ(rc, UMM_OK);

    ChunkMetadata meta;
    rc = umm_lookup_chunk(chunk_name, &meta);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(meta.size, 1024ULL * 1024);

    /* Read back */
    uint8_t *read_buf = malloc(1024 * 1024);
    ASSERT_NOT_NULL(read_buf);

    ChunkDescriptor tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.base_gpa  = meta.gpa;
    tmp.user_size = meta.size;
    rc = umm_read(&tmp, 0, 1024ULL * 1024, read_buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(memcmp(write_buf, read_buf, 1024ULL * 1024), 0);

    free(write_buf);
    free(read_buf);
    umm_free(&desc);
    umm_deinit();
}

/* ==================================================================== */
/* main: start servers, run tests, shutdown                             */
/* ==================================================================== */
int main(void)
{
    printf("\n========================================\n");
    printf("Distributed CXL Memory Test\n");
    printf("========================================\n\n");

    /* Suppress info logs during server startup */
    umm_log_set_level(UMM_LOG_WARN);

    /* ---- Start ummd ---- */
    pthread_t ummd_tid;
    if (pthread_create(&ummd_tid, NULL, ummd_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create ummd thread\n");
        return 1;
    }
    if (wait_for_port(UMMD_PORT, 5000) != 0) {
        fprintf(stderr, "ummd failed to start\n");
        return 1;
    }
    printf("[+] ummd running on port %d\n", UMMD_PORT);

    /* ---- Start umms-0 (Node 0) ---- */
    umms_args_t umms0_args = { UMMS0_PORT, 0, MEM_SIZE };
    pthread_t umms0_tid;
    if (pthread_create(&umms0_tid, NULL, umms_thread, &umms0_args) != 0) {
        fprintf(stderr, "Failed to create umms-0 thread\n");
        return 1;
    }
    if (wait_for_port(UMMS0_PORT, 5000) != 0) {
        fprintf(stderr, "umms-0 failed to start\n");
        return 1;
    }
    printf("[+] umms-0 (node=0) running on port %d\n", UMMS0_PORT);

    /* ---- Start umms-1 (Node 1) ---- */
    umms_args_t umms1_args = { UMMS1_PORT, 1, MEM_SIZE };
    pthread_t umms1_tid;
    if (pthread_create(&umms1_tid, NULL, umms_thread, &umms1_args) != 0) {
        fprintf(stderr, "Failed to create umms-1 thread\n");
        return 1;
    }
    if (wait_for_port(UMMS1_PORT, 5000) != 0) {
        fprintf(stderr, "umms-1 failed to start\n");
        return 1;
    }
    printf("[+] umms-1 (node=1) running on port %d\n", UMMS1_PORT);

    printf("\nAll servers ready. Running tests...\n\n");
    usleep(200000);  /* 200ms settle */

    /* ---- Run tests ---- */
    int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define RUN_DIST_TEST(name) do { \
    tests_run++; \
    printf("  [%d] Testing %s... ", tests_run, #name); \
    fflush(stdout); \
    test_##name(); \
    printf("PASSED\n"); \
    tests_passed++; \
} while(0)

    /* Defensive: ensure clean state before first test */
    umm_deinit();

    RUN_DIST_TEST(node0_alloc_node1_lookup);
    RUN_DIST_TEST(multi_node_chunks);
    RUN_DIST_TEST(cross_node_large_transfer);

    /* ---- Shutdown ---- */
    printf("\nShutting down servers...\n");
    g_servers_running = 0;
    pthread_join(ummd_tid, NULL);
    pthread_join(umms0_tid, NULL);
    pthread_join(umms1_tid, NULL);

    printf("\n========================================\n");
    printf("Results: %d run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    printf("========================================\n\n");

    return tests_failed > 0 ? 1 : 0;
}
