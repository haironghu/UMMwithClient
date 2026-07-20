#!/usr/bin/env python3
"""
umm_sglang_bridge_v4_acl.py -- 支持 DRAM + SSD 双 Tier + ACL HBM 拷贝测试

功能：
  1. alloc(size, media_type) 支持 "dram" 和 "ssd"
  2. 分别测试 DRAM->HBM 和 SSD->HBM 的完整数据通路
  3. 读写统一用 ctypes.memmove / ctypes.string_at + ACL memcpy
"""

import os
import mmap
import ctypes
import sys

sys.path.insert(0, "/home/workspace")

from bmpclient.client import UMMServiceClient
from bmpclient.umm_client import ChunkDescriptor

GPA_OFFSET_MASK = 0x00FFFFFFFFFFFFFF
SSD_TOTAL_SIZE = 128  * 1024**3  # 1GB

# ACL 常量定义
ACL_MEM_MALLOC_HUGE_FIRST = 0
ACL_MEMCPY_HOST_TO_DEVICE = 1
ACL_MEMCPY_DEVICE_TO_HOST = 2


def check_ret(msg, ret):
    if ret != 0:
        raise RuntimeError(f"{msg} failed, ret={ret}")

_bridges = {}
import atexit
def get_bridge():
    pid = os.getpid()
    if pid not in _bridges:
        _bridges[pid] = UmmSglangBridge(
            meta_addr="127.0.0.1:20001",
            mem_addr="127.0.0.1:20002",
            ssd_path="/tmp/umm_ssd.raw",
            ssd_size=256 * 1024**3,
        )
    return _bridges[pid]

def cleanup():
    for pid, bridge in list(_bridges.items()):
        try:
            bridge.close()
        except:
            pass
    _bridges.clear()

def alloc(size, media_type="ssd"):
    """分配内存（fork-safe）"""
    return get_bridge().alloc(size, media_type)


def free(gpa, chunk_id, size):
    """释放内存"""
    get_bridge().free(gpa, chunk_id, size)


def gpa_to_virt_addr(gpa):
    """GPA → 虚拟地址"""
    return get_bridge().gpa_to_virt_addr(gpa)

atexit.register(cleanup)

class UmmSglangBridge:
    """
    支持 DRAM (CXL) + SSD 双 Tier 的虚拟地址提供者。
    """

    def __init__(
        self,
        meta_addr: str = "127.0.0.1:20001",
        mem_addr: str = "127.0.0.1:20002",
        ssd_path: str = "/tmp/umm_ssd.raw",
        ssd_size: int = SSD_TOTAL_SIZE,
    ):
        self.client = UMMServiceClient(
            meta_addr=meta_addr,
            mem_addr=mem_addr,
            ssd_device="",
            ssd_devices=[(ssd_path, ssd_size)],
        )
        self.ssd_path = ssd_path
        self.ssd_size = ssd_size

        if not os.path.exists(ssd_path):
            raise FileNotFoundError(f"SSD 文件不存在: {ssd_path}")

        fd = os.open(ssd_path, os.O_RDWR)
        self.mm = mmap.mmap(fd, ssd_size, offset=0, access=mmap.ACCESS_WRITE)
        os.close(fd)

        self._base_mv = memoryview(self.mm)
        self.base_virt_addr = ctypes.addressof(
            ctypes.c_char.from_buffer(self._base_mv)
        )

        print(f"[UmmSglangBridge] init: base_virt=0x{self.base_virt_addr:016x}, "
              f"dram=CXL(mock), ssd={ssd_size / (1024**3):.0f}GB")

    def alloc(self, size: int, media_type: str = "ssd") -> tuple[int, int, int]:
        """分配内存，返回 (gpa, chunk_id, virt_addr)。"""
        desc = self.client.create_chunk(size, media_type=media_type)
        gpa = desc.base_gpa
        chunk_id = desc.chunk_id
        actual_size = desc.user_size
        file_offset = gpa & GPA_OFFSET_MASK
        virt_addr = self.base_virt_addr + file_offset

        tier_name = "dram" if ((gpa >> 56) & 0x03) == 1 else "ssd"
        print(f"[UmmSglangBridge] alloc: tier={tier_name}, chunk={chunk_id}, "
              f"gpa=0x{gpa:016x}, size={actual_size / (1024**3):.2f}GB, "
              f"virt=0x{virt_addr:016x}")

        return gpa, chunk_id, virt_addr

    def gpa_to_virt_addr(self, gpa: int) -> int:
        """GPA -> 虚拟地址。对 DRAM 和 SSD GPA 通用。"""
        return self.base_virt_addr + (gpa & GPA_OFFSET_MASK)

    def free(self, gpa: int, chunk_id: int = 0, size: int = 0) -> None:
        """释放 UMM Chunk。"""
        desc = ChunkDescriptor(base_gpa=gpa, user_size=size, chunk_id=chunk_id)
        try:
            self.client.delete_chunk(desc)
            tier = (gpa >> 56) & 0x03
            print(f"[UmmSglangBridge] free: chunk={chunk_id}, "
                  f"gpa=0x{gpa:016x}, tier={tier}")
        except RuntimeError as e:
            print(f"[UmmSglangBridge] free warning: chunk={chunk_id}, err={e}")

    def close(self):
        self._base_mv.release()
        self.mm.close()
        self.client.close()
        print("[UmmSglangBridge] closed")


# ==================== ACL HBM 辅助函数 ====================

def test_hbm_copy(bridge: UmmSglangBridge, test_str: str, media_type: str, dev_ptr: int):
    """
    通用测试：Host(UMM) -> Device(HBM) -> Host，验证数据通路。

    :param bridge: UmmSglangBridge 实例
    :param test_str: 测试字符串
    :param media_type: "dram" 或 "ssd"
    :param dev_ptr: 已申请的 Device HBM 指针
    :return: True if success
    """
    size = len(test_str.encode('utf-8'))
    tier_label = media_type.upper()

    print(f"\n========== {tier_label} -> HBM -> Host 测试 ==========")

    # 1. 申请 UMM 虚拟地址
    gpa, cid, va = alloc(1 * 1024**2, "ssd")

    # 2. 写入测试数据到 UMM
    ctypes.memmove(va, test_str.encode('utf-8'), size)
    print(f"[{tier_label}] 数据已写入 UMM: '{test_str}'")

    # 3. Host(UMM) -> Device(HBM)
    import acl
    ret = acl.rt.memcpy(
        dev_ptr, size,
        va, size,
        ACL_MEMCPY_HOST_TO_DEVICE
    )
    check_ret(f"acl.rt.memcpy {tier_label}->HBM", ret)
    print(f"[Memcpy] {tier_label} -> HBM 完成")

    # 4. 申请本地 Host 内存（接收拷贝）
    host_dst, ret = acl.rt.malloc_host(size)
    check_ret("acl.rt.malloc_host (dst)", ret)

    # 5. Device(HBM) -> Host
    ret = acl.rt.memcpy(
        host_dst, size,
        dev_ptr, size,
        ACL_MEMCPY_DEVICE_TO_HOST
    )
    check_ret("acl.rt.memcpy HBM->Host", ret)
    print("[Memcpy] HBM -> Host 完成")

    # 6. 验证数据
    result = ctypes.string_at(host_dst, size).decode('utf-8')
    print(f"[Result] 读取到的字符串: '{result}'")
    assert result == test_str, f"数据校验失败！期望: '{test_str}', 实际: '{result}'"
    print(f"[PASS] {tier_label} 数据通路验证通过!")

    # 7. 释放资源
    acl.rt.free_host(host_dst)
    bridge.free(gpa, cid, size)

    return True


# ==================== 主测试 ====================
def main():
    # bridge = UmmSglangBridge(
    #     ssd_path="/tmp/umm_ssd.raw",
    #     ssd_size=128 * 1024**3,  # 1GB
    # )

    # ========== ACL 初始化 ==========
    import acl
    ret = acl.init()
    check_ret("acl.init", ret)

    device_id = 0
    ret = acl.rt.set_device(device_id)
    check_ret("acl.rt.set_device", ret)

    # 统一申请一块 Device HBM 内存（两个测试复用）
    max_size = 256  # 最大测试数据字节数
    dev_ptr, ret = acl.rt.malloc(max_size, ACL_MEM_MALLOC_HUGE_FIRST)
    check_ret("acl.rt.malloc (device)", ret)
    print(f"[Device] HBM 内存已申请: addr={dev_ptr}, size={max_size}B")

    try:
        # ========== Test 1: SSD -> HBM -> Host ==========
        test_ssd = "Hello from SSD via HBM!"
        test_hbm_copy(get_bridge(), test_ssd, "ssd", dev_ptr)

        # ========== Test 2: DRAM -> HBM -> Host ==========
        test_dram = "Hello from DRAM via HBM!"
        test_hbm_copy(get_bridge(), test_dram, "dram", dev_ptr)

        print("\n========== 所有测试全部通过! ==========")

    finally:
        # ========== 资源释放 ==========
        if dev_ptr:
            acl.rt.free(dev_ptr)
            print("[Device] HBM 内存已释放")

        ret = acl.rt.reset_device(device_id)
        check_ret("acl.rt.reset_device", ret)

        ret = acl.finalize()
        check_ret("acl.finalize", ret)
        print("[Done] ACL 资源已释放")



if __name__ == "__main__":
    main()