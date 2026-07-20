import threading
from dataclasses import dataclass, field
from typing import Optional, List, Dict, Tuple

from bmpclient.client import UMMServiceClient
from bmpclient.umm_client import ChunkDescriptor


@dataclass
class Block:
    """细粒度分配后返回给用户的小块内存描述符。"""
    chunk_id: int       # UMM chunk_id
    offset: int         # 在所属 Chunk 内的字节偏移
    size: int           # 实际请求并分配的字节数
    gpa: int = 0        # UMM base_gpa（Chunk 的全局物理地址）
    chunk_size: int = 0 # UMM 实际分配的 page 对齐大小


@dataclass
class ChunkBuffer:
    """代表一个从 UMM 申请的大块 Chunk，内部用空闲链表维护可分配空间。"""
    chunk_id: int
    base_gpa: int
    total_size: int     # UMM 实际分配的 page 对齐大小
    media_type: str
    device_idx: Optional[int] = None   # SSD 指定设备时使用
    free_list: List[Tuple[int, int]] = field(default_factory=list)

    def __post_init__(self):
        if not self.free_list:
            self.free_list = [(0, self.total_size)]

    def alloc(self, size: int) -> Optional[int]:
        """首次适应算法，返回分配到的 offset，若无空间则返回 None。"""
        for i, (offset, free_size) in enumerate(self.free_list):
            if free_size >= size:
                if free_size == size:
                    self.free_list.pop(i)
                else:
                    self.free_list[i] = (offset + size, free_size - size)
                return offset
        return None

    def free(self, offset: int, size: int) -> None:
        """释放指定区间并与相邻空闲块合并。"""
        new_list = list(self.free_list)
        new_list.append((offset, size))
        new_list.sort(key=lambda x: x[0])

        # 合并相邻块
        merged = []
        for off, sz in new_list:
            if merged and merged[-1][0] + merged[-1][1] == off:
                merged[-1] = (merged[-1][0], merged[-1][1] + sz)
            else:
                merged.append((off, sz))

        self.free_list = merged

    def is_fully_free(self) -> bool:
        return len(self.free_list) == 1 and self.free_list[0] == (0, self.total_size)


class MemoryExhaustedError(Exception):
    """底层 UMM 返回 UMM_E_NO_MEMORY 时抛出。"""
    pass


class FineGrainedAllocator:
    """
    细粒度分配器。
    向 UMM 申请大块 Chunk，并在本地将 Chunk 切分为小块 Block 供用户使用。
    对 DRAM 和 SSD 两种介质独立管理。
    """

    def __init__(self, meta_addr: str, mem_addr: str, node_id: int = 0,
                 default_chunk_size: int = 4 * 1024 * 1024,
                 ssd_device: str = "",
                 ssd_devices=None):
        self.client = UMMServiceClient(meta_addr, mem_addr, node_id,
                                       ssd_device=ssd_device,
                                       ssd_devices=ssd_devices)
        self.default_chunk_size = default_chunk_size
        self.chunks_by_type: Dict[str, Dict[int, ChunkBuffer]] = {"dram": {}, "ssd": {}}
        self.lock = threading.Lock()

    def get_chunk_buffer(self, chunk_id: int) -> Optional[ChunkBuffer]:
        """根据 chunk_id 查找对应的 ChunkBuffer。"""
        with self.lock:
            for media_type in ("dram", "ssd"):
                cb = self.chunks_by_type[media_type].get(chunk_id)
                if cb is not None:
                    return cb
        return None

    def alloc(
        self,
        size: int,
        media_type: str = "dram",
        device_idx: Optional[int] = None,
    ) -> Block:
        """
        分配指定大小的 Block。
        先在现有 Chunk 中查找，若无空间则申请新 Chunk。
        :param device_idx: 仅对 SSD 有效，指定从第几个 SSD 设备分配。
        """
        if media_type not in ("dram", "ssd"):
            raise ValueError("media_type must be 'dram' or 'ssd'")
        if device_idx is not None and media_type != "ssd":
            raise ValueError("device_idx 仅在 media_type='ssd' 时支持")

        with self.lock:
            # 1. 在现有 Chunk 中查找（device_idx 必须匹配）
            chunks = self.chunks_by_type[media_type]
            for chunk_id, cb in list(chunks.items()):
                if cb.device_idx != device_idx:
                    continue
                offset = cb.alloc(size)
                if offset is not None:
                    return Block(
                        chunk_id=chunk_id,
                        offset=offset,
                        size=size,
                        gpa=cb.base_gpa,
                        chunk_size=cb.total_size,
                    )

            # 2. 申请新 Chunk
            chunk_size = max(size, self.default_chunk_size)
            try:
                desc = self.client.create_chunk(
                    chunk_size, media_type, device_idx=device_idx
                )
            except RuntimeError as e:
                raise MemoryExhaustedError(f"No available {media_type} space: {e}")

            cb = ChunkBuffer(
                chunk_id=desc.chunk_id,
                base_gpa=desc.base_gpa,
                total_size=desc.user_size,
                media_type=media_type,
                device_idx=device_idx,
            )
            offset = cb.alloc(size)
            if offset is None:
                # 按逻辑不会发生，因为 user_size >= size
                self.client.delete_chunk(desc)
                raise MemoryExhaustedError("Chunk allocation failed")

            chunks[desc.chunk_id] = cb
            return Block(
                chunk_id=desc.chunk_id,
                offset=offset,
                size=size,
                gpa=desc.base_gpa,
                chunk_size=desc.user_size,
            )

    def free(self, block: Block) -> None:
        """释放 Block，若所属 Chunk 完全空闲则归还给 UMM。"""
        with self.lock:
            for media_type in ("dram", "ssd"):
                chunks = self.chunks_by_type[media_type]
                cb = chunks.get(block.chunk_id)
                if cb:
                    cb.free(block.offset, block.size)
                    if cb.is_fully_free():
                        # 构造临时 ChunkDescriptor 用于 free
                        desc = ChunkDescriptor(
                            chunk_id=cb.chunk_id,
                            base_gpa=cb.base_gpa,
                            user_size=cb.total_size,
                        )
                        self.client.delete_chunk(desc)
                        del chunks[block.chunk_id]
                    return

    def preallocate_chunks(
        self,
        count: int,
        chunk_size: int,
        media_type: str = "dram",
        device_idx: Optional[int] = None,
    ) -> None:
        """批量预申请多个 Chunk，减少 UMM 往返。"""
        if media_type not in ("dram", "ssd"):
            raise ValueError("media_type must be 'dram' or 'ssd'")
        if device_idx is not None and media_type != "ssd":
            raise ValueError("device_idx 仅在 media_type='ssd' 时支持")

        with self.lock:
            for _ in range(count):
                try:
                    desc = self.client.create_chunk(
                        chunk_size, media_type, device_idx=device_idx
                    )
                except RuntimeError as e:
                    raise MemoryExhaustedError(f"Batch allocation failed: {e}")

                cb = ChunkBuffer(
                    chunk_id=desc.chunk_id,
                    base_gpa=desc.base_gpa,
                    total_size=desc.user_size,
                    media_type=media_type,
                    device_idx=device_idx,
                )
                self.chunks_by_type[media_type][desc.chunk_id] = cb

    def read(self, block: Block, offset: int = 0, size: int = None) -> bytes:
        """
        从指定 Block 中读取数据。
        调用 UMM C API 的 umm_read 进行底层数据读取。

        :param block: 待读取的 Block
        :param offset: 相对于 Block 起始的偏移（字节）
        :param size: 读取字节数，默认读取到 Block 末尾
        :return: 读取到的数据
        """
        if offset < 0:
            raise ValueError("offset must be non-negative")
        if size is None:
            size = block.size - offset
        if size < 0:
            raise ValueError("size must be non-negative")
        if offset + size > block.size:
            raise ValueError(f"read range exceeds block size: offset={offset}, size={size}, block.size={block.size}")

        desc = ChunkDescriptor(
            chunk_id=block.chunk_id,
            base_gpa=block.gpa,
            user_size=block.chunk_size,
        )
        chunk_offset = block.offset + offset
        return self.client.read_chunk(desc, chunk_offset, size)

    def write(self, block: Block, offset: int = 0, data: bytes = b"") -> None:
        """
        向指定 Block 中写入数据。
        调用 UMM C API 的 umm_write 进行底层数据写入。

        :param block: 待写入的 Block
        :param offset: 相对于 Block 起始的偏移（字节）
        :param data: 待写入的数据
        """
        if offset < 0:
            raise ValueError("offset must be non-negative")
        if not data:
            return
        if offset + len(data) > block.size:
            raise ValueError(f"write range exceeds block size: offset={offset}, data_len={len(data)}, block.size={block.size}")

        desc = ChunkDescriptor(
            chunk_id=block.chunk_id,
            base_gpa=block.gpa,
            user_size=block.chunk_size,
        )
        chunk_offset = block.offset + offset
        self.client.write_chunk(desc, chunk_offset, data)

    def get_device_list(self) -> List[dict]:
        """查询当前管理的存储设备列表。"""
        return self.client.get_device_list()

    def close(self) -> None:
        """释放所有持有的 Chunk。"""
        with self.lock:
            for media_type in ("dram", "ssd"):
                for chunk_id in list(self.chunks_by_type[media_type].keys()):
                    cb = self.chunks_by_type[media_type][chunk_id]
                    desc = ChunkDescriptor(
                        chunk_id=cb.chunk_id,
                        base_gpa=cb.base_gpa,
                        user_size=cb.total_size,
                    )
                    self.client.delete_chunk(desc)
                self.chunks_by_type[media_type].clear()
            self.client.close()
