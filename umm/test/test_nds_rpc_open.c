/*
 * test_nds_rpc_open.c — NDS RPC 引导（bind 重试）最小驱动
 *
 * 仅做一次 ssd_nds_open + close，用于 test_umms_nds_bootstrap.sh
 * 驱动 Phase 1（UMM_NDS_RPC_WAIT_MS 重试）场景验证。
 * 用法: test_nds_rpc_open [spec]   （缺省 "0"）
 * 退出码 0=open 成功，1=失败（打印 OPEN_FAIL rc=<umm_err>）
 */
#include "../src/transport/ssd_backend_nds.h"
#include "../include/umm.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    const char *spec = argc > 1 ? argv[1] : "0";
    SsdNdsBackend *b = NULL;
    int rc = ssd_nds_open(spec, &b);
    if (rc != UMM_OK) {
        printf("OPEN_FAIL rc=%d\n", rc);
        return 1;
    }
    printf("OPEN_OK\n");
    ssd_nds_close(b);
    return 0;
}
