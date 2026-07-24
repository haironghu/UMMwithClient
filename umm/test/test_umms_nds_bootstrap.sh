#!/bin/bash
# ========================================================================
export LC_ALL=C.UTF-8
# test_umms_nds_bootstrap.sh — NDS RPC 运维加固（Phase 1 bind 重试 +
# Phase 2 umms 托管拉起 RPC server）端到端验证脚本
#
# 不进 `make test` 默认套件（防真机/flaky 环境影响回归），由可选目标
# `make test-bootstrap` 触发，也可直接执行：
#   cd umm && bash test/test_umms_nds_bootstrap.sh
#
# 覆盖场景：
#   Phase 1（fake libnvm_host 桩，STUB_BIND_FAIL_FIRST_N 注入 bind 失败）：
#     P1.1 前 5 次 bind 失败 + UMM_NDS_RPC_WAIT_MS=3000 → 退避重试后 open 成功
#     P1.2 bind 恒失败 + UMM_NDS_RPC_WAIT_MS=300       → ~300ms 后 UMM_E_IO
#     P1.3 UMM_NDS_RPC_WAIT_MS=0                        → 单次失败立即返回
#   Phase 2（umms 托管拉起 umm_nds_rpc_server，真 socket 监听桩）：
#     P2.1 端到端：spawn(PID) → wait_ready 成功 → setenv 一致性日志 →
#          nds 设备注册经 RPC 引导成功 → mem server 端口就绪
#     P2.2 keep_alive=false：SIGTERM umms → RPC server 子进程同退
#     P2.3 keep_alive=true（缺省）：SIGTERM umms → 子进程保留
#     P2.4 helper 不存在（临时改名 bin/umm_nds_rpc_server）→ fatal 退出
# ========================================================================
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
WORK="$(mktemp -d /tmp/umms_nds_bootstrap.XXXXXX)"
FAKE="$WORK/libnvm_host_rpc_stub.so"
SOCK="$WORK/rpc.sock"
PASS=0
FAIL=0

cleanup() {
    [ -n "${UMMS_PID:-}" ] && kill "$UMMS_PID" 2>/dev/null
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

say()  { echo "[bootstrap] $*"; }
ok()   { PASS=$((PASS+1)); say "PASS: $*"; }
bad()  { FAIL=$((FAIL+1)); say "FAIL: $*"; }

# ---- 构建 ----
say "building deps..."
make -s bin/umms bin/umm_nds_rpc_server bin/libnds_aiv.so >/dev/null || {
    say "FATAL: make failed"; exit 1; }
gcc -std=gnu11 -O2 -Wall -pthread -I./include -I./src -shared -fPIC \
    test/stub_nvm_host_rpc.c -o "$FAKE" || { say "FATAL: stub build"; exit 1; }
gcc -std=gnu11 -O2 -Wall -pthread -I./include -I./src \
    test/test_nds_rpc_open.c \
    build/ssd_backend_nds.o build/log.o build/error_codes.o \
    -o "$WORK/test_nds_rpc_open" -pthread -ldl || {
    say "FATAL: helper build"; exit 1; }

wait_sock() {  # 等 socket 文件出现且可 connect（由 python 探测）
    for _ in $(seq 1 50); do
        [ -S "$1" ] && return 0
        sleep 0.1
    done
    return 1
}

# ========================================================================
# Phase 1 — bind 重试
# ========================================================================
say "---- Phase 1: bind retry ----"

# P1.1: 前 5 次 bind -6 后成功（真实 fake server 在线）
UMM_LIBNVM_PATH="$FAKE" ./bin/umm_nds_rpc_server --socket "$SOCK" \
    >"$WORK/p1_srv.log" 2>&1 &
SRV_PID=$!
wait_sock "$SOCK" || { bad "P1.1 fake server not listening"; exit 1; }

UMM_NDS_PATH="$ROOT/bin/libnds_aiv.so" UMM_NDS_PRELOAD="$FAKE" \
UMM_NDS_RPC_SOCKET="$SOCK" UMM_NDS_RPC_WAIT_MS=3000 \
STUB_BIND_FAIL_FIRST_N=5 \
    "$WORK/test_nds_rpc_open" >"$WORK/p1_1.log" 2>&1
rc=$?
if [ $rc -eq 0 ] && grep -a -q "OPEN_OK" "$WORK/p1_1.log" && \
   grep -a -q "retry in" "$WORK/p1_1.log" && \
   grep -a -q "succeeded after" "$WORK/p1_1.log"; then
    ok "P1.1 retry-then-success (5 injected failures, WAIT_MS=3000)"
else
    bad "P1.1 (rc=$rc)"; cat "$WORK/p1_1.log"
fi
kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=""

# P1.2: 恒失败 + WAIT_MS=300 → ~300ms 后 UMM_E_IO
t0=$(date +%s%N)
UMM_NDS_PATH="$ROOT/bin/libnds_aiv.so" UMM_NDS_PRELOAD="$FAKE" \
UMM_NDS_RPC_SOCKET="$SOCK" UMM_NDS_RPC_WAIT_MS=300 \
STUB_BIND_FAIL_FIRST_N=9999 \
    "$WORK/test_nds_rpc_open" >"$WORK/p1_2.log" 2>&1
rc=$?
t1=$(date +%s%N)
el=$(( (t1 - t0) / 1000000 ))
if [ $rc -ne 0 ] && grep -a -q "OPEN_FAIL rc=-7" "$WORK/p1_2.log" && \
   grep -a -q "已等待" "$WORK/p1_2.log" && \
   [ $el -ge 280 ] && [ $el -lt 2000 ]; then
    ok "P1.2 always-fail → UMM_E_IO after ${el}ms (budget 300ms)"
else
    bad "P1.2 (rc=$rc elapsed=${el}ms)"; cat "$WORK/p1_2.log"
fi

# P1.3: WAIT_MS=0 → 单次失败立即返回（原行为）
t0=$(date +%s%N)
UMM_NDS_PATH="$ROOT/bin/libnds_aiv.so" UMM_NDS_PRELOAD="$FAKE" \
UMM_NDS_RPC_SOCKET="$SOCK" UMM_NDS_RPC_WAIT_MS=0 \
STUB_BIND_FAIL_FIRST_N=9999 \
    "$WORK/test_nds_rpc_open" >"$WORK/p1_3.log" 2>&1
rc=$?
t1=$(date +%s%N)
el=$(( (t1 - t0) / 1000000 ))
if [ $rc -ne 0 ] && grep -a -q "OPEN_FAIL rc=-7" "$WORK/p1_3.log" && \
   ! grep -a -q "retry in" "$WORK/p1_3.log" && [ $el -lt 200 ]; then
    ok "P1.3 WAIT_MS=0 → single immediate failure (${el}ms)"
else
    bad "P1.3 (rc=$rc elapsed=${el}ms)"; cat "$WORK/p1_3.log"
fi

# ========================================================================
# Phase 2 — umms 托管拉起 RPC server
# ========================================================================
say "---- Phase 2: umms-managed RPC server ----"

# 随机空闲端口（共享环境可能有其他租户占用固定端口）
PORT=$(python3 -c 'import socket; s=socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()' 2>/dev/null)
[ -n "$PORT" ] || PORT=$(( (RANDOM % 20000) + 20000 ))
mk_yaml() {  # $1=keep_alive 行（可空）
    cat > "$WORK/umms_test.yaml" <<EOF
node_id: 0
listen_addr: "127.0.0.1"
listen_port: $PORT
memory_size: 67108864
base_gpa: 0
ssd_devices: "nds:0:64M"
nds_rpc_server_enable: true
nds_rpc_server_ctrl: /dev/libnvm_helper0
nds_rpc_server_ns: 1
nds_rpc_server_qd: 64
nds_rpc_server_socket: $SOCK
$1
EOF
}

start_umms() {  # 结果日志在 $WORK/umms.log，PID 在 UMMS_PID
    UMM_LIBNVM_PATH="$FAKE" UMM_NDS_PATH="$ROOT/bin/libnds_aiv.so" \
    UMM_NDS_PRELOAD="$FAKE" \
        ./bin/umms -c "$WORK/umms_test.yaml" >"$WORK/umms.log" 2>&1 &
    UMMS_PID=$!
    for _ in $(seq 1 100); do
        grep -a -q "mem_server: listening on" "$WORK/umms.log" 2>/dev/null && return 0
        kill -0 "$UMMS_PID" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

stop_umms() {  # SIGTERM（触发优雅退出/atexit 钩子），5s 内不退则 KILL
    [ -n "${UMMS_PID:-}" ] || return 0
    kill -TERM "$UMMS_PID" 2>/dev/null
    for _ in $(seq 1 50); do
        kill -0 "$UMMS_PID" 2>/dev/null || break
        ps -o stat= -p "$UMMS_PID" 2>/dev/null | grep -a -q Z && break
        sleep 0.1
    done
    kill -9 "$UMMS_PID" 2>/dev/null
    wait "$UMMS_PID" 2>/dev/null
    UMMS_PID=""
}

# P2.1: 端到端闭环（keep_alive: true）
mk_yaml "nds_rpc_server_keep_alive: true"
if start_umms; then
    L="$WORK/umms.log"
    if grep -a -q "spawned RPC server (Process A) pid=" "$L" && \
       grep -a -q "RPC server ready" "$L" && \
       grep -a -q "已按配置 setenv" "$L" && \
       grep -a -q "RPC context established" "$L" && \
       grep -a -q "registered SSD device" "$L" && \
       grep -a -q "listening on 127.0.0.1:$PORT" "$L"; then
        ok "P2.1 e2e: spawn→wait_ready→setenv→RPC bootstrap→register→listen"
    else
        bad "P2.1 log missing key lines"; cat "$L"
    fi
    CHILD=$(grep -a -o "spawned RPC server (Process A) pid=[0-9]*" "$L" | grep -a -o "[0-9]*$")
    stop_umms
    sleep 0.3
    if [ -n "$CHILD" ] && kill -0 "$CHILD" 2>/dev/null; then
        ok "P2.3 keep_alive=true: child $CHILD survives umms exit"
        kill "$CHILD" 2>/dev/null
    else
        bad "P2.3 keep_alive=true: child $CHILD not alive after umms exit"
    fi
else
    bad "P2.1 umms did not start"; cat "$WORK/umms.log"
fi

# P2.2: keep_alive=false → SIGTERM umms 子进程同退
mk_yaml "nds_rpc_server_keep_alive: false"
if start_umms; then
    CHILD=$(grep -a -o "spawned RPC server (Process A) pid=[0-9]*" "$WORK/umms.log" | grep -a -o "[0-9]*$")
    stop_umms
    sleep 0.5
    if [ -n "$CHILD" ] && ! kill -0 "$CHILD" 2>/dev/null; then
        ok "P2.2 keep_alive=false: child $CHILD exited with umms (SIGTERM)"
    else
        bad "P2.2 keep_alive=false: child $CHILD still alive"
        [ -n "$CHILD" ] && kill "$CHILD" 2>/dev/null
    fi
else
    bad "P2.2 umms did not start"; cat "$WORK/umms.log"
fi

# P2.4: helper 不存在 → fatal 退出码非零 + 提示 make tools
mv ./bin/umm_nds_rpc_server "$WORK/umm_nds_rpc_server.bak"
mk_yaml ""
UMM_LIBNVM_PATH="$FAKE" UMM_NDS_PATH="$ROOT/bin/libnds_aiv.so" \
UMM_NDS_PRELOAD="$FAKE" \
    ./bin/umms -c "$WORK/umms_test.yaml" >"$WORK/p2_4.log" 2>&1
rc=$?
mv "$WORK/umm_nds_rpc_server.bak" ./bin/umm_nds_rpc_server
if [ $rc -ne 0 ] && grep -a -q "make tools" "$WORK/p2_4.log"; then
    ok "P2.4 missing helper → fatal exit rc=$rc with 'make tools' hint"
else
    bad "P2.4 (rc=$rc)"; cat "$WORK/p2_4.log"
fi

# ========================================================================
say "========================================"
say "RESULT: $PASS passed, $FAIL failed"
say "========================================"
[ $FAIL -eq 0 ]
