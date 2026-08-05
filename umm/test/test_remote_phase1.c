/* ========================================================================
 * test_remote_phase1.c -- Phase 1 跨节点远程数据面单元测试
 *
 * 覆盖（不依赖真实服务端/网络）：
 *   1. CIDR 白名单匹配器（umm_net_acl_match）
 *   2. token FNV-1a 摘要（umm_token_digest）
 *   3. CIS 静态节点表（peer_nodes 解析 / 查询 / 更新）
 *   4. tier_router node 位分派（fake transport 计数）
 *   5. 旧行为回归：未挂 remote 时 node!=my 的 GPA 仍走 tier 分派
 *
 * 真实 socket 往返由 bmpclient/scripts/demo_e2e_remote_ssd.py 覆盖。
 * ======================================================================== */

#include "../include/umm.h"
#include "../src/common/umm_network.h"
#include "../src/protocol/protocol_common.h"
#include "../src/cis/cis_router.h"
#include "../src/transport/tier_router.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* 1-2. ACL / token digest                                                   */
/* ------------------------------------------------------------------------ */

TEST(acl_empty_allows_all)
{
    ASSERT_EQ(umm_net_acl_match("", "1.2.3.4"), 1);
    ASSERT_EQ(umm_net_acl_match(NULL, "1.2.3.4"), 1);
}

TEST(acl_cidr_match)
{
    const char *acl = "10.0.0.0/8, 192.168.1.0/24";
    ASSERT_EQ(umm_net_acl_match(acl, "10.1.2.3"), 1);
    ASSERT_EQ(umm_net_acl_match(acl, "10.255.255.255"), 1);
    ASSERT_EQ(umm_net_acl_match(acl, "192.168.1.99"), 1);
    ASSERT_EQ(umm_net_acl_match(acl, "192.168.2.1"), 0);
    ASSERT_EQ(umm_net_acl_match(acl, "11.0.0.1"), 0);
}

TEST(acl_edge_prefixes)
{
    ASSERT_EQ(umm_net_acl_match("172.16.0.1/32", "172.16.0.1"), 1);
    ASSERT_EQ(umm_net_acl_match("172.16.0.1/32", "172.16.0.2"), 0);
    ASSERT_EQ(umm_net_acl_match("0.0.0.0/0", "203.0.113.7"), 1);
}

TEST(acl_malformed_entries_deny)
{
    /* 非法条目不命中（不误放）；合法条目仍生效 */
    ASSERT_EQ(umm_net_acl_match("bogus,10.0.0.0/8", "10.0.0.1"), 1);
    ASSERT_EQ(umm_net_acl_match("bogus,10.0.0.0/8", "11.0.0.1"), 0);
    ASSERT_EQ(umm_net_acl_match("10.0.0.0/33", "10.0.0.1"), 0);
    ASSERT_EQ(umm_net_acl_match("10.0.0.0/8", "not-an-ip"), 0);
}

TEST(token_digest_semantics)
{
    uint8_t a[6], b[6], c[6], e[6];
    umm_token_digest("s3cr3t", a);
    umm_token_digest("s3cr3t", b);
    umm_token_digest("other", c);
    umm_token_digest("", e);

    ASSERT_EQ(memcmp(a, b, 6), 0);      /* 同 token 同摘要 */
    ASSERT_NE(memcmp(a, c, 6), 0);      /* 不同 token 不同摘要 */
    ASSERT_EQ(memcmp(e, "\0\0\0\0\0\0", 6), 0);  /* 空 token = 全 0（旧线格式） */
}

/* ------------------------------------------------------------------------ */
/* 3. CIS 静态节点表                                                         */
/* ------------------------------------------------------------------------ */

TEST(cis_static_peer_table)
{
    UMMConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.my_node_id = 1;
    strncpy(cfg.peer_nodes,
            "0:10.0.0.11:20002, 2:node-b.example:20002,"
            "bad-entry, 300:x:1, :missing-node",
            sizeof(cfg.peer_nodes) - 1);

    cis_router_deinit();  /* 防御：清理可能的残留状态 */
    ASSERT_EQ(cis_router_init(&cfg), UMM_OK);

    char addr[256];

    ASSERT_EQ(cis_router_get_node_addr(0, addr, sizeof(addr)), UMM_OK);
    ASSERT_EQ(strcmp(addr, "10.0.0.11:20002"), 0);

    ASSERT_EQ(cis_router_get_node_addr(2, addr, sizeof(addr)), UMM_OK);
    ASSERT_EQ(strcmp(addr, "node-b.example:20002"), 0);

    /* 畸形/越界条目被跳过 */
    ASSERT_EQ(cis_router_get_node_addr(7, addr, sizeof(addr)),
              UMM_E_NOT_FOUND);

    /* register：新增 + 更新 */
    ASSERT_EQ(cis_router_register_node(3, "10.0.0.13:20002"), UMM_OK);
    ASSERT_EQ(cis_router_get_node_addr(3, addr, sizeof(addr)), UMM_OK);
    ASSERT_EQ(cis_router_register_node(3, "10.0.0.99:20002"), UMM_OK);
    ASSERT_EQ(cis_router_get_node_addr(3, addr, sizeof(addr)), UMM_OK);
    ASSERT_EQ(strcmp(addr, "10.0.0.99:20002"), 0);

    cis_router_deinit();
    /* deinit 后查询报未初始化 */
    ASSERT_EQ(cis_router_get_node_addr(0, addr, sizeof(addr)),
              UMM_E_NOT_INITIALIZED);
}

/* ------------------------------------------------------------------------ */
/* 4-5. tier_router node 位分派（fake transport 计数）                        */
/* ------------------------------------------------------------------------ */

static int g_fake_cxl_get, g_fake_cxl_put;
static int g_fake_remote_get, g_fake_remote_put;

static int fake_get(void *ctx, gpa_t gpa, uint64_t len, void *out)
{
    (void)ctx; (void)gpa; (void)len; (void)out;
    if (ctx == (void *)1) g_fake_cxl_get++;
    else                  g_fake_remote_get++;
    return UMM_OK;
}

static int fake_put(void *ctx, gpa_t gpa, uint64_t len, const void *buf)
{
    (void)gpa; (void)len; (void)buf;
    if (ctx == (void *)1) g_fake_cxl_put++;
    else                  g_fake_remote_put++;
    return UMM_OK;
}

static int fake_atomic_set(void *ctx, gpa_t gpa, uint64_t v)
{
    (void)ctx; (void)gpa; (void)v;
    return UMM_OK;
}

static MemoryTransportVtbl g_fake_vtbl = {
    .get        = fake_get,
    .put        = fake_put,
    .atomic_set = fake_atomic_set,
};

TEST(router_remote_dispatch_by_node)
{
    g_fake_cxl_get = g_fake_cxl_put = 0;
    g_fake_remote_get = g_fake_remote_put = 0;

    /* CXL transport ctx 用 (void*)1 标记；remote ctx 用 (void*)2 标记 */
    TierRouter *tr = tier_router_create(&g_fake_vtbl, (void *)1,
                                        NULL, NULL);
    ASSERT_NOT_NULL(tr);

    /* my_node = 1；remote 挂载 */
    tier_router_set_remote(tr, 1, &g_fake_vtbl, (void *)2);

    MemoryTransportVtbl *r = tier_router_get_vtbl(tr);
    uint8_t buf[16];

    /* owner=0 != my=1 → 远端 */
    gpa_t g_remote = make_gpa(0, UMM_TIER_SSD, 4096);
    ASSERT_EQ(r->put(tr, g_remote, 16, buf), UMM_OK);
    ASSERT_EQ(g_fake_remote_put, 1);
    ASSERT_EQ(g_fake_cxl_put, 0);

    ASSERT_EQ(r->get(tr, g_remote, 16, buf), UMM_OK);
    ASSERT_EQ(g_fake_remote_get, 1);
    ASSERT_EQ(g_fake_cxl_get, 0);

    /* owner=1 == my=1 → 本地 tier 分派（SSD 槽位为 NULL → INVALID_ARG，
     * 但绝不能走到 remote） */
    gpa_t g_local = make_gpa(1, UMM_TIER_SSD, 4096);
    int rc = r->put(tr, g_local, 16, buf);
    ASSERT_EQ(rc, UMM_E_INVALID_ARG);
    ASSERT_EQ(g_fake_remote_put, 1);   /* 未再增长 */

    /* 本地 CXL GPA → fake CXL */
    gpa_t g_cxl = make_gpa(1, UMM_TIER_CXL, 0);
    ASSERT_EQ(r->put(tr, g_cxl, 16, buf), UMM_OK);
    ASSERT_EQ(g_fake_cxl_put, 1);

    /* 远端原子 → UNSUPPORTED（Phase 1 明确边界） */
    ASSERT_EQ(r->atomic_set(tr, g_remote, 1), UMM_E_UNSUPPORTED);
    /* 本地原子不受影响 */
    ASSERT_EQ(r->atomic_set(tr, g_cxl, 1), UMM_OK);

    tier_router_destroy(tr);
}

TEST(router_legacy_without_remote)
{
    g_fake_cxl_get = g_fake_cxl_put = 0;
    g_fake_remote_get = g_fake_remote_put = 0;

    TierRouter *tr = tier_router_create(&g_fake_vtbl, (void *)1,
                                        &g_fake_vtbl, (void *)2);
    ASSERT_NOT_NULL(tr);

    /* 不调用 set_remote（旧部署形态）：node!=my 的 GPA 仍走 tier 分派。
     * 这里 SSD 槽位 ctx=(void*)2，命中 fake_put 的 remote 计数分支，
     * 用于证明走的是 tier 槽位而非 remote 通道。 */
    MemoryTransportVtbl *r = tier_router_get_vtbl(tr);
    uint8_t buf[16];
    gpa_t g = make_gpa(9, UMM_TIER_SSD, 0);
    ASSERT_EQ(r->put(tr, g, 16, buf), UMM_OK);
    ASSERT_EQ(g_fake_remote_put, 1);   /* tier 槽位被调用（旧行为保持） */

    tier_router_destroy(tr);
}

/* ------------------------------------------------------------------------ */

TEST_SUITE("remote_phase1")
    RUN_TEST(acl_empty_allows_all);
    RUN_TEST(acl_cidr_match);
    RUN_TEST(acl_edge_prefixes);
    RUN_TEST(acl_malformed_entries_deny);
    RUN_TEST(token_digest_semantics);
    RUN_TEST(cis_static_peer_table);
    RUN_TEST(router_remote_dispatch_by_node);
    RUN_TEST(router_legacy_without_remote);
END_TEST_SUITE()
