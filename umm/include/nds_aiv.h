#ifndef NDS_AIV_H
#define NDS_AIV_H
#include <cstdint>
#include <cstdlib>

struct IOVec
{
    void* vaddr;
    uint64_t length;
    uint64_t offset;
};

class NDS
{
public:
    static NDS &Instance() noexcept;
    void nds_init(uint32_t deviceId, size_t queueDepth, size_t coreNum, uint64_t page_size, uint64_t max_page_num);
    void nds_uninit();
    void nds_register(void* dev_mem, uint64_t aligned_read_size);
    void nds_single_write(void* vaddr, uint64_t bytes, uint64_t f_offset);
    void nds_single_read(void* vaddr, uint64_t bytes, uint64_t f_offset);
    void nds_batch_write(IOVec* iovecs, size_t n_iov);
    void nds_batch_read(IOVec* iovecs, size_t n_iov);
    NDS(const NDS&) = delete;
    NDS& operator=(const NDS&) = delete;
    NDS(NDS&&) = delete;
    NDS& operator=(NDS&&) = delete;
private:
    struct Impl;
    Impl* m_impl;
    NDS();
    ~NDS();
};
#endif
