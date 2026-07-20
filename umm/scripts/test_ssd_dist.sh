#!/bin/bash
# ========================================================================
# test_ssd_dist.sh -- Multi-terminal distributed SSD test
#
# Scenario: Node 0 (writer) allocates SSD chunk, writes data to file.
#           Node 1 (reader) looks up chunk in ummd, reads from SAME file.
#
# KEY INSIGHT: SSD files are shared storage. No transport for data.
# ummd only stores metadata (name -> GPA). GPA encodes the offset.
#
# Usage:
#   cd /mnt/agents/output/um
#   ./scripts/test_ssd_dist.sh
#
# Architecture:
#   Terminal A (this script): ./ummd  (metadata directory)
#   Terminal B (this script): ./umms -d /tmp/umm_ssd_shared (allocator + SSD)
#   Terminal C (manual):      ./writer_ssd (alloc + write file directly)
#   Terminal D (manual):      ./reader_ssd <chunk_name> (lookup + read file)
# ========================================================================

set -e

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="$PROJ_DIR/bin"
SSD_DIR="/tmp/umm_ssd_shared"

# Colors
G='\033[0;32m'
Y='\033[1;33m'
R='\033[0;31m'
NC='\033[0m'

cleanup() {
    echo ""
    echo "[cleanup] Stopping servers..."
    kill $UMMS_PID $UMMD_PID 2>/dev/null || true
    rm -rf "$SSD_DIR"
    rm -f /tmp/umm_test_pids
}
trap cleanup EXIT INT TERM

# Build if needed
if [ ! -x "$BIN_DIR/ummd" ] || [ ! -x "$BIN_DIR/umms" ] || \
   [ ! -x "$BIN_DIR/writer" ] || [ ! -x "$BIN_DIR/reader" ]; then
    echo "[build] Building..."
    cd "$PROJ_DIR" && make server dist_tools
fi

# Clean & prepare
rm -rf "$SSD_DIR"
mkdir -p "$SSD_DIR"

echo "========================================"
echo "  Distributed SSD Test (Multi-Process)"
echo "========================================"
echo ""
echo "SSD shared directory: $SSD_DIR"
echo ""

# ---- Start ummd (Terminal A) ----
echo "[1/4] Starting ummd on 127.0.0.1:20001..."
/lib64/ld-linux-x86-64.so.2 "$BIN_DIR/ummd" -p 20001 -b 127.0.0.1 >/tmp/ummd.log 2>&1 &
UMMD_PID=$!
sleep 1

# ---- Start umms with SSD backend (Terminal B) ----
echo "[2/4] Starting umms (node=0, SSD backend) on 127.0.0.1:20002..."
/lib64/ld-linux-x86-64.so.2 "$BIN_DIR/umms" -p 20002 -b 127.0.0.1 \
    -n 0 -s 67108864 -d "$SSD_DIR" >/tmp/umms0.log 2>&1 &
UMMS0_PID=$!
sleep 1

echo "$UMMD_PID $UMMS0_PID" > /tmp/umm_test_pids

# ---- Check servers are ready ----
if ! nc -z 127.0.0.1 20001 2>/dev/null; then
    echo -e "${R}[ERROR] ummd not ready${NC}"; exit 1
fi
if ! nc -z 127.0.0.1 20002 2>/dev/null; then
    echo -e "${R}[ERROR] umms not ready${NC}"; exit 1
fi
echo -e "${G}[OK] Servers running.${NC}"
echo ""

# ---- Terminal C: writer (Node 0) ----
echo "========================================"
echo -e "${Y}Terminal C (Node 0 writer):${NC}"
echo "  cd $PROJ_DIR"
echo "  # Write to SSD — alloc via RPC to umms, then write file directly"
echo "  /lib64/ld-linux-x86-64.so.2 $BIN_DIR/writer -c $PROJ_DIR/config/writer_ssd.yaml -t ssd"
echo ""
echo "  OR use the direct-write test tool:"
echo "  /lib64/ld-linux-x86-64.so.2 $BIN_DIR/test_ssd_simple_dist"
echo ""

# ---- Auto-run test ----
echo "[3/4] Running automated test..."
/lib64/ld-linux-x86-64.so.2 "$BIN_DIR/test_ssd_simple_dist" 2>&1

if [ $? -eq 0 ]; then
    echo ""
    echo -e "${G}========================================${NC}"
    echo -e "${G}  Distributed SSD test PASSED!${NC}"
    echo -e "${G}========================================${NC}"
    echo ""
    echo "Summary:"
    echo "  - umms (node=0) managed SSD allocation via bitmap"
    echo "  - Node 0 allocated SSD chunk, wrote directly to .raw file"
    echo "  - Node 1 looked up chunk in ummd, got GPA→offset"
    echo "  - Node 1 opened the SAME shared file, read data"
    echo "  - NO transport involved for data movement"
else
    echo -e "${R}[FAIL] Automated test failed${NC}"
fi

echo ""
echo "Press Enter to stop servers and exit..."
read
