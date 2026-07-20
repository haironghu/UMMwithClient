# ========================================================================
# UMM (Unified Memory Management) System - GNU Makefile
# ========================================================================

CC      = gcc
CFLAGS  = -std=gnu11 -O2 -Wall -Wextra -pthread -I./include -I./src
LDFLAGS = -pthread
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
    ssd_pool.c \
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

.PHONY: all dirs server client test clean

all: dirs server client test tools

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

tools: $(BIN_DIR)/ssd_sim_test

$(BIN_DIR)/ssd_sim_test: $(BIN_DIR)/ssd_sim_test.c | dirs
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
    $(BUILD_DIR)/ssd_pool.o \
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
    $(BUILD_DIR)/ssd_pool.o \
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

test: dirs $(BIN_DIR)/test_allocator $(BIN_DIR)/test_transport_mock \
      $(BIN_DIR)/test_metadata $(BIN_DIR)/test_integration \
      $(BIN_DIR)/test_cross_node $(BIN_DIR)/test_cxl $(BIN_DIR)/test_tier \
      $(BIN_DIR)/test_ssd_dist $(BIN_DIR)/test_ssd_simple_dist \
      $(BIN_DIR)/test_ssd_multi_device $(BIN_DIR)/test_rw \
      $(BIN_DIR)/test_p0_concurrent
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
	@echo "========================================"
	@echo "All tests completed."
	@echo "========================================"

# ========================================================================
# Clean
# ========================================================================

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BIN_DIR)/test_* $(BIN_DIR)/ummd $(BIN_DIR)/umms \
	       $(BIN_DIR)/ssd_sim_test
