# ========================================================================
# UMM (Unified Memory Management) System - GNU Makefile
# ========================================================================

CC      = gcc
CFLAGS  = -std=gnu11 -O2 -Wall -Wextra -pthread -I./include -I./src
CXX     = g++
CXXFLAGS = -std=c++11 -O2 -Wall -fPIC -I./include
LDFLAGS = -pthread -ldl
AR      = ar
ARFLAGS = rcs

# Directories
SRC_DIR    = src
BIN_DIR    = bin
TEST_DIR   = test
BUILD_DIR  = build

# ========================================================================
# Source files by module (flat object names in build/)
# ========================================================================

COMMON_SRC := \
    error_codes.c \
    log.c \
    config_parser.c \
    bitmap_allocator.c \
    umm_network.c

PROTOCOL_SRC := \
    protocol_common.c \
    meta_protocol.c \
    mem_protocol.c

TRANSPORT_SRC := \
    transport_local.c \
    transport_ssd.c \
    transport_remote.c \
    ssd_pool.c \
    ssd_backend_libnvm.c \
    ssd_backend_nds.c \
    tier_router.c

METADATA_SRC := \
    meta_hash_table.c \
    meta_service_direct.c \
    meta_service_rpc_server.c \
    meta_service_rpc_client.c

MEMORY_SRC := \
    mem_service_direct.c \
    mem_service_rpc_server.c \
    mem_service_rpc_client.c

CLIENT_SRC := \
    umm_transport.c \
    umm_router.c \
    umm_descriptor.c \
    umm_api.c

CIS_SRC := cis_router.c

SERVER_SRC := meta_server.c mem_server.c

# Object files (all flat in build/)
COMMON_OBJ    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(COMMON_SRC))
PROTOCOL_OBJ  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(PROTOCOL_SRC))
TRANSPORT_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(TRANSPORT_SRC))
METADATA_OBJ  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(METADATA_SRC))
MEMORY_OBJ    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(MEMORY_SRC))
CLIENT_OBJ    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CLIENT_SRC))
CIS_OBJ       := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CIS_SRC))
SERVER_OBJ    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SERVER_SRC))

ALL_OBJ := \
    $(COMMON_OBJ) \
    $(PROTOCOL_OBJ) \
    $(TRANSPORT_OBJ) \
    $(METADATA_OBJ) \
    $(MEMORY_OBJ) \
    $(CLIENT_OBJ) \
    $(CIS_OBJ) \
    $(SERVER_OBJ)

# ========================================================================
# Phony targets
# ========================================================================

.PHONY: all dirs server client test test-build clean shared test-bootstrap

# 注意：all 只编译不运行测试；运行测试套件用 make test；
# 只编译测试二进制（不运行）用 make test-build。
all: dirs server client tools shared

dirs: $(BUILD_DIR) $(BIN_DIR)

$(BUILD_DIR):
	@mkdir -p $@

$(BIN_DIR):
	@mkdir -p $@

# ========================================================================
# Pattern rule for all object files
# ========================================================================

$(BUILD_DIR)/%.o: $(SRC_DIR)/common/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/protocol/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/transport/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/metadata_service/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/memory_service/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/client_lib/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/cis/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/server/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

# ========================================================================
# Server binaries
# ========================================================================

server: $(BIN_DIR)/ummd $(BIN_DIR)/umms
# storage-tierd disabled during SSD refactor — will re-enable after RPC update

tools: $(BIN_DIR)/ssd_sim_test $(BIN_DIR)/umm_nds_rpc_server

$(BIN_DIR)/ssd_sim_test: $(BIN_DIR)/ssd_sim_test.c | dirs
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	@echo "  LD      $@"

$(BIN_DIR)/umm_nds_rpc_server: $(BIN_DIR)/umm_nds_rpc_server.c | dirs
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	@echo "  LD      $@"

UMMD_OBJ := \
    $(BUILD_DIR)/meta_server.o \
    $(COMMON_OBJ) \
    $(PROTOCOL_OBJ) \
    $(METADATA_OBJ)

UMMS_OBJ := \
    $(BUILD_DIR)/mem_server.o \
    $(COMMON_OBJ) \
    $(PROTOCOL_OBJ) \
    $(MEMORY_OBJ) \
    $(METADATA_OBJ) \
    $(BUILD_DIR)/ssd_pool.o \
    $(BUILD_DIR)/ssd_backend_libnvm.o \
    $(BUILD_DIR)/ssd_backend_nds.o \
    $(BUILD_DIR)/transport_ssd.o

$(BIN_DIR)/ummd: $(BIN_DIR)/ummd.c $(UMMD_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(UMMD_OBJ) -o $@ $(LDFLAGS)
	@echo "  LD      $@"

$(BIN_DIR)/umms: $(BIN_DIR)/umms.c $(UMMS_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(UMMS_OBJ) -o $@ $(LDFLAGS)
	@echo "  LD      $@"

# SSD Tier daemon (disabled during refactor)
# STORAGE_TIERD_OBJ := \
#     $(TRANSPORT_OBJ) \
#     $(COMMON_OBJ) \
#     $(PROTOCOL_OBJ)
# $(BIN_DIR)/storage-tierd: $(BIN_DIR)/storage-tierd.c $(STORAGE_TIERD_OBJ) | dirs
#	$(CC) $(CFLAGS) $< $(STORAGE_TIERD_OBJ) -o $@ $(LDFLAGS)
#	@echo "  LD      $@"

# ========================================================================
# Static client library
# ========================================================================

client: $(BUILD_DIR)/libumm.a

LIBUMM_OBJ := \
    $(COMMON_OBJ) \
    $(PROTOCOL_OBJ) \
    $(TRANSPORT_OBJ) \
    $(METADATA_OBJ) \
    $(MEMORY_OBJ) \
    $(CLIENT_OBJ) \
    $(CIS_OBJ)

$(BUILD_DIR)/libumm.a: $(LIBUMM_OBJ) | dirs
	$(AR) $(ARFLAGS) $@ $(LIBUMM_OBJ)
	@echo "  AR      $@"

# ========================================================================
# Shared library: build/libumm.so（Python client / ctypes 依赖）
# ========================================================================

PIC_DIR := build_pic
PIC_OBJ := $(patsubst $(BUILD_DIR)/%.o,$(PIC_DIR)/%.o,$(LIBUMM_OBJ))

$(PIC_DIR):
	@mkdir -p $@

$(PIC_DIR)/%.o: $(SRC_DIR)/common/%.c | $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(PIC_DIR)/%.o: $(SRC_DIR)/protocol/%.c | $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(PIC_DIR)/%.o: $(SRC_DIR)/transport/%.c | $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(PIC_DIR)/%.o: $(SRC_DIR)/metadata_service/%.c | $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(PIC_DIR)/%.o: $(SRC_DIR)/memory_service/%.c | $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(PIC_DIR)/%.o: $(SRC_DIR)/client_lib/%.c | $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(PIC_DIR)/%.o: $(SRC_DIR)/cis/%.c | $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/libumm.so: $(PIC_OBJ) | dirs
	$(CC) -shared -fPIC $^ -o $@ $(LDFLAGS)
	@echo "  LD      $@"

shared: $(BUILD_DIR)/libumm.so

# ========================================================================
# Test binaries
# ========================================================================

TEST_ALLOCATOR_OBJ := \
    $(BUILD_DIR)/bitmap_allocator.o \
    $(BUILD_DIR)/error_codes.o \
    $(BUILD_DIR)/log.o

TEST_TRANSPORT_OBJ := \
    $(BUILD_DIR)/transport_local.o \
    $(BUILD_DIR)/mem_service_direct.o \
    $(BUILD_DIR)/ssd_pool.o \
    $(BUILD_DIR)/ssd_backend_libnvm.o \
    $(BUILD_DIR)/ssd_backend_nds.o \
    $(BUILD_DIR)/transport_ssd.o \
    $(BUILD_DIR)/bitmap_allocator.o \
    $(BUILD_DIR)/error_codes.o \
    $(BUILD_DIR)/log.o

TEST_METADATA_OBJ := \
    $(BUILD_DIR)/meta_service_direct.o \
    $(BUILD_DIR)/meta_hash_table.o \
    $(BUILD_DIR)/error_codes.o \
    $(BUILD_DIR)/log.o

TEST_INTEGRATION_OBJ := \
    $(BUILD_DIR)/umm_api.o \
    $(BUILD_DIR)/umm_transport.o \
    $(BUILD_DIR)/umm_router.o \
    $(BUILD_DIR)/umm_descriptor.o \
    $(BUILD_DIR)/transport_local.o \
    $(BUILD_DIR)/transport_ssd.o \
    $(BUILD_DIR)/transport_remote.o \
    $(BUILD_DIR)/ssd_pool.o \
    $(BUILD_DIR)/ssd_backend_libnvm.o \
    $(BUILD_DIR)/ssd_backend_nds.o \
    $(BUILD_DIR)/tier_router.o \
    $(BUILD_DIR)/meta_service_direct.o \
    $(BUILD_DIR)/meta_hash_table.o \
    $(BUILD_DIR)/meta_service_rpc_client.o \
    $(BUILD_DIR)/mem_service_direct.o \
    $(BUILD_DIR)/mem_service_rpc_client.o \
    $(BUILD_DIR)/bitmap_allocator.o \
    $(BUILD_DIR)/error_codes.o \
    $(BUILD_DIR)/log.o \
    $(BUILD_DIR)/protocol_common.o \
    $(BUILD_DIR)/meta_protocol.o \
    $(BUILD_DIR)/mem_protocol.o \
    $(BUILD_DIR)/umm_network.o \
    $(BUILD_DIR)/cis_router.o

TEST_CROSS_NODE_OBJ := $(TEST_INTEGRATION_OBJ)

$(BIN_DIR)/test_allocator: $(TEST_DIR)/test_allocator.c $(TEST_ALLOCATOR_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_ALLOCATOR_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_transport_mock: $(TEST_DIR)/test_transport_mock.c $(TEST_TRANSPORT_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_TRANSPORT_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_metadata: $(TEST_DIR)/test_metadata.c $(TEST_METADATA_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_METADATA_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_integration: $(TEST_DIR)/test_integration.c $(TEST_INTEGRATION_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_INTEGRATION_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_cross_node: $(TEST_DIR)/test_cross_node.c $(TEST_CROSS_NODE_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_CROSS_NODE_OBJ) -o $@ $(LDFLAGS)

TEST_CXL_OBJ := $(TEST_CROSS_NODE_OBJ)

$(BIN_DIR)/test_cxl: $(TEST_DIR)/test_cxl.c $(TEST_CXL_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_CXL_OBJ) -o $@ $(LDFLAGS)

# Multi-tier test
TEST_TIER_OBJ := \
    $(BUILD_DIR)/transport_ssd.o \
    $(BUILD_DIR)/transport_remote.o \
    $(BUILD_DIR)/ssd_pool.o \
    $(BUILD_DIR)/ssd_backend_libnvm.o \
    $(BUILD_DIR)/ssd_backend_nds.o \
    $(BUILD_DIR)/tier_router.o \
    $(BUILD_DIR)/transport_local.o \
    $(BUILD_DIR)/umm_transport.o \
    $(BUILD_DIR)/umm_router.o \
    $(BUILD_DIR)/umm_descriptor.o \
    $(BUILD_DIR)/umm_api.o \
    $(BUILD_DIR)/meta_service_direct.o \
    $(BUILD_DIR)/meta_hash_table.o \
    $(BUILD_DIR)/meta_service_rpc_client.o \
    $(BUILD_DIR)/mem_service_direct.o \
    $(BUILD_DIR)/mem_service_rpc_client.o \
    $(BUILD_DIR)/bitmap_allocator.o \
    $(BUILD_DIR)/error_codes.o \
    $(BUILD_DIR)/log.o \
    $(BUILD_DIR)/config_parser.o \
    $(BUILD_DIR)/protocol_common.o \
    $(BUILD_DIR)/meta_protocol.o \
    $(BUILD_DIR)/mem_protocol.o \
    $(BUILD_DIR)/umm_network.o \
    $(BUILD_DIR)/cis_router.o

$(BIN_DIR)/test_tier: $(TEST_DIR)/test_tier.c $(TEST_TIER_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

# Phase 1 远程数据面单元测试（ACL/token/CIS 静态表/tier_router node 位分派）
TEST_REMOTE_PHASE1_OBJ := \
    $(BUILD_DIR)/umm_network.o \
    $(BUILD_DIR)/protocol_common.o \
    $(BUILD_DIR)/cis_router.o \
    $(BUILD_DIR)/tier_router.o \
    $(BUILD_DIR)/error_codes.o \
    $(BUILD_DIR)/log.o

$(BIN_DIR)/test_remote_phase1: $(TEST_DIR)/test_remote_phase1.c $(TEST_REMOTE_PHASE1_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_REMOTE_PHASE1_OBJ) -o $@ $(LDFLAGS)

TEST_SSD_DIST_OBJ := $(TEST_TIER_OBJ)

$(BIN_DIR)/test_ssd_dist: $(TEST_DIR)/test_ssd_dist.c $(TEST_SSD_DIST_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_SSD_DIST_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_ssd_simple_dist: $(TEST_DIR)/test_ssd_simple_dist.c $(TEST_TIER_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_ssd_multi_device: $(TEST_DIR)/test_ssd_multi_device.c $(TEST_TIER_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_rw: $(TEST_DIR)/test_rw.c $(TEST_TIER_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_p0_concurrent: $(TEST_DIR)/test_p0_concurrent.c $(TEST_TIER_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

$(BIN_DIR)/test_ssd_pool_io: $(TEST_DIR)/test_ssd_pool_io.c $(TEST_TIER_OBJ) | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

# libnvm 桩库（文件模拟控制器）+ 接入测试
$(BIN_DIR)/libnvm_host.so: $(TEST_DIR)/stub_libnvm_host.c | dirs
	$(CC) $(CFLAGS) -shared -fPIC $< -o $@

$(BIN_DIR)/test_ssd_libnvm: $(TEST_DIR)/test_ssd_libnvm.c $(TEST_TIER_OBJ) $(BIN_DIR)/libnvm_host.so | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

# nds 桩库（C++，文件模拟 SSD）+ 接入测试
$(BIN_DIR)/libnds_aiv.so: $(TEST_DIR)/stub_nds_aiv.cpp | dirs
	$(CXX) $(CXXFLAGS) -shared $< -o $@

$(BIN_DIR)/test_ssd_nds: $(TEST_DIR)/test_ssd_nds.c $(TEST_TIER_OBJ) $(BIN_DIR)/libnds_aiv.so | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

# nvm_host RPC 桩库（server + client 双侧面符号）
$(BIN_DIR)/libnvm_host_rpc_stub.so: $(TEST_DIR)/stub_nvm_host_rpc.c | dirs
	$(CC) $(CFLAGS) -shared -fPIC $< -o $@

# 不导出 nvm_host_rpc_disconnect 的变体（模拟老版本库，可选符号缺失路径）
$(BIN_DIR)/libnvm_host_rpc_stub_nodisconn.so: $(TEST_DIR)/stub_nvm_host_rpc.c | dirs
	$(CC) $(CFLAGS) -DSTUB_NO_DISCONNECT -shared -fPIC $< -o $@

# RPC 连接断开测试（close 时 rpc_disconnect 释放 server 侧连接）
$(BIN_DIR)/test_nds_rpc_disconnect: $(TEST_DIR)/test_nds_rpc_disconnect.c \
      $(TEST_TIER_OBJ) $(BIN_DIR)/libnds_aiv.so \
      $(BIN_DIR)/libnvm_host_rpc_stub.so \
      $(BIN_DIR)/libnvm_host_rpc_stub_nodisconn.so | dirs
	$(CC) $(CFLAGS) $< $(TEST_TIER_OBJ) -o $@ $(LDFLAGS)

TEST_BINS := \
      $(BIN_DIR)/test_allocator $(BIN_DIR)/test_transport_mock \
      $(BIN_DIR)/test_metadata $(BIN_DIR)/test_integration \
      $(BIN_DIR)/test_cross_node $(BIN_DIR)/test_cxl $(BIN_DIR)/test_tier \
      $(BIN_DIR)/test_ssd_dist $(BIN_DIR)/test_ssd_simple_dist \
      $(BIN_DIR)/test_ssd_multi_device $(BIN_DIR)/test_rw \
      $(BIN_DIR)/test_p0_concurrent $(BIN_DIR)/test_ssd_pool_io \
      $(BIN_DIR)/test_ssd_libnvm $(BIN_DIR)/test_ssd_nds \
      $(BIN_DIR)/test_nds_rpc_disconnect \
      $(BIN_DIR)/test_remote_phase1

# 只编译测试二进制，不运行（CI 构建/交叉编译场景用）
test-build: dirs $(TEST_BINS)

# 编译并运行完整测试套件
test: test-build
	@echo ""
	@echo "========================================"
	@echo "Running test suite..."
	@echo "========================================"
	@echo ""
	@$(BIN_DIR)/test_allocator
	@echo ""
	@$(BIN_DIR)/test_transport_mock
	@echo ""
	@$(BIN_DIR)/test_metadata
	@echo ""
	@$(BIN_DIR)/test_integration
	@echo ""
	@$(BIN_DIR)/test_cross_node
	@echo ""
	@$(BIN_DIR)/test_cxl
	@echo ""
	@$(BIN_DIR)/test_tier
	@echo ""
	@$(BIN_DIR)/test_ssd_dist
	@echo ""
	@$(BIN_DIR)/test_ssd_simple_dist
	@echo ""
	@$(BIN_DIR)/test_ssd_multi_device
	@echo ""
	@$(BIN_DIR)/test_rw
	@echo ""
	@$(BIN_DIR)/test_p0_concurrent
	@echo ""
	@$(BIN_DIR)/test_ssd_pool_io
	@echo ""
	@$(BIN_DIR)/test_remote_phase1
	@echo ""
	@cd $(BIN_DIR) && env -u UMM_TEST_LIBNVM_CTRL -u UMM_TEST_LIBNVM_OFF \
	      -u UMM_TEST_LIBNVM_INITONLY \
	      -u UMM_NDS_RPC_SOCKET -u UMM_NDS_PRELOAD \
	      UMM_LIBNVM_PATH=$(abspath $(BIN_DIR)/libnvm_host.so) \
	      ./test_ssd_libnvm
	@echo ""
	@cd $(BIN_DIR) && env -u UMM_TEST_NDS_SPEC -u UMM_TEST_NDS_OFF \
	      -u UMM_TEST_NDS_INITONLY \
	      -u UMM_NDS_RPC_SOCKET -u UMM_NDS_PRELOAD -u UMM_NDS_RPC_WAIT_MS \
	      UMM_NDS_PATH=$(abspath $(BIN_DIR)/libnds_aiv.so) \
	      ./test_ssd_nds
	@echo ""
	@cd $(BIN_DIR) && env -u UMM_NDS_RPC_SOCKET -u UMM_NDS_PRELOAD \
	      -u UMM_NDS_RPC_WAIT_MS -u UMM_NDS_PATH \
	      ./test_nds_rpc_disconnect single \
	      $(abspath $(BIN_DIR)/libnds_aiv.so) \
	      $(abspath $(BIN_DIR)/libnvm_host_rpc_stub.so) \
	      /tmp/umm_test_rpc_disconn_single.sock
	@cd $(BIN_DIR) && env -u UMM_NDS_RPC_SOCKET -u UMM_NDS_PRELOAD \
	      -u UMM_NDS_RPC_WAIT_MS -u UMM_NDS_PATH \
	      ./test_nds_rpc_disconnect shared \
	      $(abspath $(BIN_DIR)/libnds_aiv.so) \
	      $(abspath $(BIN_DIR)/libnvm_host_rpc_stub.so) \
	      /tmp/umm_test_rpc_disconn_shared.sock
	@cd $(BIN_DIR) && env -u UMM_NDS_RPC_SOCKET -u UMM_NDS_PRELOAD \
	      -u UMM_NDS_RPC_WAIT_MS -u UMM_NDS_PATH \
	      ./test_nds_rpc_disconnect nodisconn \
	      $(abspath $(BIN_DIR)/libnds_aiv.so) \
	      $(abspath $(BIN_DIR)/libnvm_host_rpc_stub_nodisconn.so) \
	      /tmp/umm_test_rpc_disconn_nodisconn.sock
	@cd $(BIN_DIR) && env -u UMM_NDS_RPC_SOCKET -u UMM_NDS_PRELOAD \
	      -u UMM_NDS_RPC_WAIT_MS -u UMM_NDS_PATH \
	      ./test_nds_rpc_disconnect nosock \
	      $(abspath $(BIN_DIR)/libnds_aiv.so) \
	      $(abspath $(BIN_DIR)/libnvm_host_rpc_stub.so) \
	      /tmp/umm_test_rpc_disconn_nosock.sock
	@echo ""
	@echo "========================================"
	@echo "All tests completed."
	@echo "========================================"

# NDS RPC 运维加固端到端验证（bind 重试 + umms 托管拉起 RPC server）。
# 不进 make test 默认套件：依赖进程编排与端口，防真机/flaky 影响回归。
test-bootstrap: $(BIN_DIR)/umms $(BIN_DIR)/umm_nds_rpc_server $(BIN_DIR)/libnds_aiv.so
	@bash $(TEST_DIR)/test_umms_nds_bootstrap.sh

# ========================================================================
# Clean
# ========================================================================

clean:
	rm -rf $(BUILD_DIR) $(PIC_DIR)
	rm -f $(BIN_DIR)/test_* $(BIN_DIR)/ummd $(BIN_DIR)/umms \
	       $(BIN_DIR)/ssd_sim_test $(BIN_DIR)/umm_nds_rpc_server \
	       $(BIN_DIR)/libnvm_host.so $(BIN_DIR)/libnds_aiv.so \
	       $(BIN_DIR)/libnvm_host_rpc_stub.so \
	       $(BIN_DIR)/libnvm_host_rpc_stub_nodisconn.so
