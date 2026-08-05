#!/usr/bin/env bash
# 06_run_experiment.sh — 两个 VM 并行执行共享池实验客户端，回收结果并做
# 集群级断言：所有 chunk 的 offset 区间两两不相交（分配权威唯一性证据）。
#
# 每个 VM 上跑 demo_shared_pool.py（S0-S4）：
#   申请 EXP_CHUNKS 个 EXP_CHUNK_SIZE chunk → 流式写确定性 pattern →
#   读回逐字节校验 → 负路径三件套 → 结果 JSON。
#   混合池模式（EXP_MEM_TIER>=0）：追加 S1m 内存层分配校验 + S4 tier 隔离。
# 数据面全程本机 I/O（SSD 落本机盘、内存层落本机 malloc 后备；
# 不设 peer_nodes，remote transport 不挂载）。
#
# S5 阶段（EXP_S5=1，S0-S4 之后追加）——落盘/刷盘语义：
#   vmA writer  分配 SSD chunk → 写 pattern → flush()（fence=CPU 屏障+全池
#               msync(MS_SYNC) 落盘），随后进程保活等 vmB 验完才释放
#   vmB preread 预读目标区域（用旧内容填充本 VM 页缓存=制造陈旧视图）
#   vmB verify  刷盘前读（不符=页缓存陷阱负路径证据）→ invalidate_chunk()
#               （msync(MS_INVALIDATE)）→ 再读必须与写入端 digest 完全一致
#   三角色经各 VM ~ 下的 s5_* 标记文件带外同步（本脚本负责轮询与盖章）。
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh
cd "$WORK_DIR"

# 内存层容量与设备参数：共享窗口模式下客户端映射整个窗口（与 umms 一致）
if [ -n "$MEM_TIER_BACKING" ]; then
    MEM_ARG_SIZE=$SHM_SIZE
    MEM_DEVICE_ARG="--mem-device $MEM_TIER_BACKING"
else
    MEM_ARG_SIZE=$MEM_TIER_SIZE
    MEM_DEVICE_ARG=""
fi

run_client() {  # run_client <port> <node_id> <tag>
    local port=$1 node=$2 tag=$3
    ssh_vm "$port" "
        cd ~/UMM && \
        UMM_ALLOW_BLOCK_DEVICE=1 UMM_BUILD_DIR=\$HOME/UMM/umm/build \
        python3 bmpclient/scripts/demo_shared_pool.py \
            --meta-addr $VM1_IP:$META_PORT --mem-addr $VM1_IP:$MEM_PORT \
            --node-id $node --tag $tag --token $RPC_TOKEN \
            --devices '$POOL_DEVICES' \
            --chunks $EXP_CHUNKS --chunk-size $EXP_CHUNK_SIZE \
            --mem-tier $EXP_MEM_TIER --mem-chunks $EXP_MEM_CHUNKS \
            --mem-chunk-size $EXP_MEM_CHUNK_SIZE --mem-tier-size $MEM_ARG_SIZE \
            $MEM_DEVICE_ARG \
            --keep --out ~/result_$tag.json
    " > "exp-$tag.log" 2>&1
}

echo "[exp] 并行启动两个 VM 的客户端（chunks=$EXP_CHUNKS x $EXP_CHUNK_SIZE）"
run_client "$SSH_PORT_VM1" "$UMM_NODE_VM1" vmA & P1=$!
run_client "$SSH_PORT_VM2" "$UMM_NODE_VM2" vmB & P2=$!
r1=0; r2=0; wait $P1 || r1=$?; wait $P2 || r2=$?

for tag in vmA vmB; do
    echo "----- $tag 末尾日志 -----"; tail -8 "exp-$tag.log"
done
[ "$r1" -eq 0 ] && [ "$r2" -eq 0 ] \
    || { echo "[exp] 失败: vmA exit=$r1 vmB exit=$r2（看 exp-*.log）"; exit 1; }
echo "[exp] 两客户端 S0-S3 全部 PASS"

# A3 断言：共享盘模型下任何客户端都不得挂载 remote transport——
# 一旦挂载，"PASS"的读写实际走了网络 RPC（跨节点数据面），实验语义即被破坏。
# （本实验踩过：旧 libumm.so 无 opt-in 门，vmB 全部 I/O 偷跑 remote 却报 PASS。）
if grep -l "remote transport attached" exp-vmA.log exp-vmB.log 2>/dev/null; then
    echo "[exp] 失败：检测到 remote transport 挂载，数据面会跨节点（共享盘模型禁止）"
    exit 1
fi
echo "[exp] A3 断言 PASS：两客户端均未挂载 remote transport（I/O 全部本机盘）"

# 回收结果并做集群级 offset 唯一性断言（SSD 层与内存层分别断言：
# 两层的 offset 空间各自独立，umms 在每层内都是唯一分配权威）
scp_vm "$SSH_PORT_VM1" "umm@127.0.0.1:~/result_vmA.json" .
scp_vm "$SSH_PORT_VM2" "umm@127.0.0.1:~/result_vmB.json" .
python3 - <<'PY'
import json
a = json.load(open("result_vmA.json"))
b = json.load(open("result_vmB.json"))

def assert_disjoint(records, label):
    ivs = sorted(
        [(c["offset"], c["offset"] + c["size"], r["tag"]) for r in records
         for c in r["chunks"]])
    for i in range(len(ivs) - 1):
        assert ivs[i + 1][0] >= ivs[i][1], (
            f"{label} 分配重叠: {ivs[i]} vs {ivs[i+1]}")
    print("[exp] 集群级断言 PASS（%s）：%d 个 chunk 区间两两不相交"
          % (label, len(ivs)))
    for s, e, tag in ivs:
        print(f"      [{s:#010x}, {e:#010x})  {tag}")

assert_disjoint([{"tag": a["tag"], "chunks": a["chunks"]},
                 {"tag": b["tag"], "chunks": b["chunks"]}], "SSD 层")
mem = [{"tag": r["tag"], "chunks": r.get("mem_chunks", [])} for r in (a, b)]
if any(m["chunks"] for m in mem):
    assert_disjoint(mem, "内存层")
PY

# ================= S5：A 写 → fence 落盘 → B invalidate → 读回 =================
if [ "$EXP_S5" = "1" ]; then
    echo "[s5] 启动落盘/刷盘语义场景（chunk=$EXP_S5_SIZE）"
    S5_TAG="s5-$(date +%s)"
    S5_MARKERS="s5_alloc.json s5_alloc_done s5_preread_done s5_written s5_verify_done"

    s5_client() {  # s5_client <port> <node_id> <mode> [extra args...]
        local port=$1 node=$2 mode=$3; shift 3
        ssh_vm "$port" "
            cd ~/UMM && \
            UMM_ALLOW_BLOCK_DEVICE=1 UMM_BUILD_DIR=\$HOME/UMM/umm/build \
            python3 bmpclient/scripts/demo_shared_pool.py \
                --meta-addr $VM1_IP:$META_PORT --mem-addr $VM1_IP:$MEM_PORT \
                --node-id $node --tag $S5_TAG --token $RPC_TOKEN \
                --devices '$POOL_DEVICES' \
                --s5-mode $mode --s5-size $EXP_S5_SIZE --s5-tag $S5_TAG \
                --s5-timeout $EXP_S5_TIMEOUT $*
        "
    }

    poll_vm_file() {  # poll_vm_file <port> <path> <背景进程PID或0>
        local port=$1 path=$2 pid=$3 t0=$SECONDS
        while true; do
            ssh_vm "$port" "test -f $path" 2>/dev/null && return 0
            if [ "$pid" != "0" ] && ! kill -0 "$pid" 2>/dev/null; then
                echo "[s5] 失败：对端进程已退出，标记 $path 未出现"; return 1
            fi
            [ $((SECONDS - t0)) -lt "$EXP_S5_TIMEOUT" ] \
                || { echo "[s5] 失败：等待 $path 超时（${EXP_S5_TIMEOUT}s）"; return 1; }
            sleep 2
        done
    }

    # 0) 清两侧旧标记（幂等，可重跑）
    ssh_vm "$SSH_PORT_VM1" "rm -f $S5_MARKERS" 2>/dev/null || true
    ssh_vm "$SSH_PORT_VM2" "rm -f $S5_MARKERS" 2>/dev/null || true

    # 1) vmA writer 后台启动（分配 chunk → 等预读 → 写 → fence 落盘 → 保活）
    s5_client "$SSH_PORT_VM1" "$UMM_NODE_VM1" writer > s5-writer.log 2>&1 &
    WPID=$!
    poll_vm_file "$SSH_PORT_VM1" "\$HOME/s5_alloc_done" "$WPID" \
        || { echo "[s5] writer 日志："; tail -20 s5-writer.log; exit 1; }

    # 2) 取回分配信息（GPA + chunk_id 带外传给 vmB）
    scp_vm "$SSH_PORT_VM1" "umm@127.0.0.1:~/s5_alloc.json" .
    S5_GPA=$(python3 -c 'import json; print(json.load(open("s5_alloc.json"))["gpa"])')
    S5_CID=$(python3 -c 'import json; print(json.load(open("s5_alloc.json"))["chunk_id"])')
    echo "[s5] writer 已分配 chunk_id=$S5_CID gpa=$S5_GPA"

    # 3) vmB 预读（旧内容填充 vmB 页缓存=陈旧视图），然后放行 writer 写入
    s5_client "$SSH_PORT_VM2" "$UMM_NODE_VM2" preread \
        --s5-gpa "$S5_GPA" --s5-chunk-id "$S5_CID" > s5-preread.log 2>&1 \
        || { echo "[s5] 失败：preread 出错"; tail -20 s5-preread.log; exit 1; }
    tail -2 s5-preread.log
    ssh_vm "$SSH_PORT_VM1" "touch \$HOME/s5_preread_done"

    # 4) 等 writer 写完并 fence 落盘
    poll_vm_file "$SSH_PORT_VM1" "\$HOME/s5_written" "$WPID" \
        || { echo "[s5] writer 日志："; tail -20 s5-writer.log; exit 1; }
    grep -q "已 fence 落盘" s5-writer.log \
        || { echo "[s5] 失败：writer 未报告 fence 落盘"; tail -20 s5-writer.log; exit 1; }
    echo "[s5] writer 写入完成并已 fence 落盘（进程保活中）"

    # 5) vmB 验证：刷盘前读（负路径观察）→ invalidate → 刷盘后读（正路径断言）
    s5_client "$SSH_PORT_VM2" "$UMM_NODE_VM2" verify \
        --s5-gpa "$S5_GPA" --s5-chunk-id "$S5_CID" > s5-verify.log 2>&1 \
        || { echo "[s5] 失败：verify 断言未过"; tail -30 s5-verify.log; exit 1; }
    grep -q "刷盘后读回 digest 与写入端 pattern 完全一致 PASS" s5-verify.log \
        || { echo "[s5] 失败：verify 缺少 PASS 证据"; tail -30 s5-verify.log; exit 1; }
    if grep -q "负路径证据：刷盘前读到陈旧数据" s5-verify.log; then
        echo "[s5] 负路径复现：刷盘前 vmB 读到陈旧数据（页缓存陷阱证据）"
    else
        echo "[s5] 提示：本次刷盘前读到的已是新数据（页缓存未命中旧页，"
        echo "      负路径未复现；正路径 invalidate→读回 已验证）"
    fi
    echo "[s5] 正路径 PASS：invalidate 刷盘后读回与写入端 pattern 逐字节一致"

    # 6) 放行 writer 释放 chunk，收其退出码
    ssh_vm "$SSH_PORT_VM1" "touch \$HOME/s5_verify_done"
    wr=0; wait $WPID || wr=$?
    [ "$wr" -eq 0 ] \
        || { echo "[s5] 失败：writer 退出码 $wr"; tail -20 s5-writer.log; exit 1; }
    echo "[s5] writer 已释放 chunk 并正常退出"
else
    echo "[s5] EXP_S5=$EXP_S5，跳过落盘/刷盘语义场景"
fi

# ============ S6：共享内存窗口对照实验（A 写→屏障→B 直接读，不 invalidate） ============
# 与 S5 的语义对照：共享 SSD 隔着两层 guest 页缓存（须 fence 落盘+invalidate）；
# 共享内存窗口经 DAX mmap 直达宿主共享物理页（x86 硬件 cacheline 一致），
# B 无需任何刷盘动作即可读到 A 的写入。
if [ "$EXP_S6" = "1" ] && [ -n "$MEM_TIER_BACKING" ]; then
    echo "[s6] 启动共享内存窗口对照场景（tier=$EXP_MEM_TIER, "
    echo "      backing=$MEM_TIER_BACKING, chunk=$EXP_S6_SIZE）"
    S6_TAG="s6-$(date +%s)"
    S6_MARKERS="s6_alloc.json s6_alloc_done s6_written s6_verify_done"

    s6_client() {  # s6_client <port> <node_id> <mode> [extra args...]
        local port=$1 node=$2 mode=$3; shift 3
        ssh_vm "$port" "
            cd ~/UMM && \
            UMM_ALLOW_BLOCK_DEVICE=1 UMM_BUILD_DIR=\$HOME/UMM/umm/build \
            python3 bmpclient/scripts/demo_shared_pool.py \
                --meta-addr $VM1_IP:$META_PORT --mem-addr $VM1_IP:$MEM_PORT \
                --node-id $node --tag $S6_TAG --token $RPC_TOKEN \
                --devices '$POOL_DEVICES' \
                --mem-tier $EXP_MEM_TIER --mem-tier-size $MEM_ARG_SIZE \
                --mem-device $MEM_TIER_BACKING \
                --s6-mode $mode --s6-size $EXP_S6_SIZE --s6-tag $S6_TAG \
                --s6-timeout $EXP_S6_TIMEOUT $*
        "
    }

    poll_vm_file() {  # poll_vm_file <port> <path> <背景进程PID或0>
        local port=$1 path=$2 pid=$3 t0=$SECONDS
        while true; do
            ssh_vm "$port" "test -f $path" 2>/dev/null && return 0
            if [ "$pid" != "0" ] && ! kill -0 "$pid" 2>/dev/null; then
                echo "[s6] 失败：对端进程已退出，标记 $path 未出现"; return 1
            fi
            [ $((SECONDS - t0)) -lt "$EXP_S6_TIMEOUT" ] \
                || { echo "[s6] 失败：等待 $path 超时（${EXP_S6_TIMEOUT}s）"; return 1; }
            sleep 2
        done
    }

    # 0) 清两侧旧标记
    ssh_vm "$SSH_PORT_VM1" "rm -f $S6_MARKERS" 2>/dev/null || true
    ssh_vm "$SSH_PORT_VM2" "rm -f $S6_MARKERS" 2>/dev/null || true

    # 1) vmA writer 后台启动（分配 → 写 → CPU 屏障 → 保活）
    s6_client "$SSH_PORT_VM1" "$UMM_NODE_VM1" writer > s6-writer.log 2>&1 &
    WPID=$!
    poll_vm_file "$SSH_PORT_VM1" "\$HOME/s6_alloc_done" "$WPID" \
        || { echo "[s6] writer 日志："; tail -20 s6-writer.log; exit 1; }
    scp_vm "$SSH_PORT_VM1" "umm@127.0.0.1:~/s6_alloc.json" .
    S6_GPA=$(python3 -c 'import json; print(json.load(open("s6_alloc.json"))["gpa"])')
    S6_CID=$(python3 -c 'import json; print(json.load(open("s6_alloc.json"))["chunk_id"])')
    echo "[s6] writer 已分配 chunk_id=$S6_CID gpa=$S6_GPA"

    # 2) 等 writer 写完（仅 CPU 屏障，无落盘概念）
    poll_vm_file "$SSH_PORT_VM1" "\$HOME/s6_written" "$WPID" \
        || { echo "[s6] writer 日志："; tail -20 s6-writer.log; exit 1; }
    echo "[s6] writer 写入完成（内存层 fence=CPU 屏障）"

    # 3) vmB 直接读验证（不 invalidate）+ 全链路 malloc 回退守卫
    s6_client "$SSH_PORT_VM2" "$UMM_NODE_VM2" verify \
        --s6-gpa "$S6_GPA" --s6-chunk-id "$S6_CID" > s6-verify.log 2>&1 \
        || { echo "[s6] 失败：verify 断言未过"; tail -30 s6-verify.log; exit 1; }
    grep -q "未执行任何 invalidate/刷盘动作，读回 digest 与写入端 逐字节一致 PASS" \
        s6-verify.log || grep -q "未执行任何 invalidate" s6-verify.log \
        || { echo "[s6] 失败：verify 缺少 PASS 证据"; tail -30 s6-verify.log; exit 1; }
    # 静默回退守卫：任何一端回退 malloc 私有后备，"共享"即无声失效
    if grep -l "falling back to malloc backing" s6-writer.log s6-verify.log 2>/dev/null; then
        echo "[s6] 失败：客户端回退了 malloc 私有后备（共享窗口未生效）"
        exit 1
    fi
    ssh_vm "$SSH_PORT_VM1" "grep -q 'falling back to malloc backing' ~/umms.log" \
        && { echo "[s6] 失败：umms 回退了 malloc 私有后备（共享窗口未生效）"; exit 1; }
    echo "[s6] 正路径 PASS：B 无刷盘动作直接读回一致（硬件一致共享内存）"
    echo "[s6] 守卫 PASS：全链路无 malloc 私有后备回退"

    # 4) 放行 writer 释放 chunk
    ssh_vm "$SSH_PORT_VM1" "touch \$HOME/s6_verify_done"
    wr=0; wait $WPID || wr=$?
    [ "$wr" -eq 0 ] \
        || { echo "[s6] 失败：writer 退出码 $wr"; tail -20 s6-writer.log; exit 1; }
    echo "[s6] writer 已释放 chunk 并正常退出"
elif [ "$EXP_S6" = "1" ]; then
    echo "[s6] MEM_TIER_BACKING 未配置（内存层为私有 malloc/mock），"
    echo "      跳过共享内存窗口场景。启用：MEM_TIER_BACKING=/dev/pmem0 后重跑 03→06"
else
    echo "[s6] EXP_S6=$EXP_S6，跳过共享内存窗口场景"
fi

echo "== 共享盘池实验完成：两 VM 经 UMM 分配、各自本机读写，无跨节点读写/搬运 =="
echo "   可选：在宿主对 $SSD0/$SSD1 做盘上字节级校验（参考 bmpclient/scripts/simulate_shared_pool.py 的 A4）"
