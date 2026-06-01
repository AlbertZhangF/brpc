// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#if BRPC_WITH_IOURING

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include <gflags/gflags.h>
#include <liburing.h>

#include "butil/errno.h"          // berror()
#include "butil/logging.h"
#include "butil/scoped_lock.h"      // BAIDU_SCOPED_LOCK
#include "butil/iobuf.h"          // butil::iobuf::blockmem_allocate
#include "brpc/iouring/iouring_block_pool.h"

// iobuf internal hooks – declared in iobuf.cpp / iobuf_inl.h
namespace butil {
namespace iobuf {
extern void* (*blockmem_allocate)(size_t);
extern void  (*blockmem_deallocate)(void*);
}
}

namespace brpc {
namespace iouring {

// ---------------------------------------------------------------------------
// gflags
// ---------------------------------------------------------------------------

DEFINE_bool(iouring_register_buffers, false,
            "Enable io_uring pre-registered buffer I/O (READ_FIXED / "
            "WRITE_FIXED).  When true, all IOBuf blocks are allocated from a "
            "registered slab so that writes can use IORING_OP_WRITE_FIXED "
            "with no per-op page pinning, and reads use IORING_OP_READ_FIXED. "
            "Requires kernel >= 5.1.");

DEFINE_int32(iouring_mem_pool_initial_mb, 256,
             "Initial size of the io_uring fixed-buffer memory pool (MB). "
             "Effective only with --iouring_register_buffers=true.");

DEFINE_int32(iouring_mem_pool_increase_mb, 256,
             "Growth increment when the pool is exhausted (MB). "
             "Effective only with --iouring_register_buffers=true.");

DEFINE_int32(iouring_mem_pool_max_regions, 8,
             "Maximum number of memory regions. "
             "Each region causes one io_uring_register_buffers_update() per "
             "ring on growth.");

DEFINE_int32(iouring_iobuf_block_size, 8192,
             "Size of each IOBuf block when --iouring_register_buffers=true. "
             "butil::SetDefaultBlockSize() is called with this value at "
             "startup so IOBuf and the registered slab are always in sync.");

DEFINE_int32(iouring_read_slot_num, 256,
             "Initial read slots per Poller ring.");

DEFINE_int32(iouring_read_slot_max, 4096,
             "Maximum read slots per Poller ring.");

// ---------------------------------------------------------------------------
bool IsFixedBuffersEnabled() { return FLAGS_iouring_register_buffers; }

// ---------------------------------------------------------------------------
// IouringMemPool – implementation
// ---------------------------------------------------------------------------

__thread IouringMemPool::FreeNode* IouringMemPool::tls_free_     = nullptr;
__thread size_t                    IouringMemPool::tls_free_cnt_ = 0;

static const size_t kTlsCacheMax = 128;   // blocks cached per thread
static const size_t kBytesPerMB  = 1UL << 20;

IouringMemPool& IouringMemPool::Instance() {
    static IouringMemPool inst;
    return inst;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool IouringMemPool::Init(size_t block_size) {
    if (initialized_) {
        LOG(WARNING) << "IouringMemPool already initialized";
        return true;
    }
    if (block_size == 0 || block_size % 4096 != 0) {
        LOG(ERROR) << "IouringMemPool::Init: block_size must be a nonzero "
                      "multiple of 4096, got " << block_size;
        return false;
    }

    block_size_ = block_size;

    // Hook IOBuf's block allocator.
    prev_allocate_   = butil::iobuf::blockmem_allocate;
    prev_deallocate_ = butil::iobuf::blockmem_deallocate;
    butil::iobuf::blockmem_allocate   = MemPoolAllocate;
    butil::iobuf::blockmem_deallocate = MemPoolDeallocate;

    // Allocate the initial region.
    if (!AddRegion(static_cast<size_t>(FLAGS_iouring_mem_pool_initial_mb))) {
        // Unhook on failure.
        butil::iobuf::blockmem_allocate   = prev_allocate_;
        butil::iobuf::blockmem_deallocate = prev_deallocate_;
        return false;
    }

    initialized_ = true;
    LOG(INFO) << "IouringMemPool ready: block_size=" << block_size_
              << " initial_mb=" << FLAGS_iouring_mem_pool_initial_mb;
    return true;
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------
void IouringMemPool::Destroy() {
    if (!initialized_) { return; }

    butil::iobuf::blockmem_allocate   = prev_allocate_;
    butil::iobuf::blockmem_deallocate = prev_deallocate_;

    {
        BAIDU_SCOPED_LOCK(extend_lock_);
        for (auto& r : regions_) {
            free(reinterpret_cast<void*>(r.base));
        }
        regions_.clear();
    }
    {
        BAIDU_SCOPED_LOCK(free_lock_);
        global_free_ = nullptr;
    }
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// AddRingRegistrar / RemoveRingRegistrar
// ---------------------------------------------------------------------------
void IouringMemPool::AddRingRegistrar(struct io_uring* ring, RegionRegisterCb cb) {
    BAIDU_SCOPED_LOCK(registrar_lock_);
    // Register all existing regions with the new ring using the already-computed
    // buf_index_base (identical for every ring).
    {
        BAIDU_SCOPED_LOCK(extend_lock_);
        for (const auto& r : regions_) {
            cb(reinterpret_cast<void*>(r.base), r.size, r.block_size,
               r.buf_index_base);
        }
    }
    reg_rings_.push_back(ring);
    reg_cbs_.push_back(std::move(cb));
}

void IouringMemPool::RemoveRingRegistrar(struct io_uring* ring) {
    BAIDU_SCOPED_LOCK(registrar_lock_);
    for (size_t i = 0; i < reg_rings_.size(); ++i) {
        if (reg_rings_[i] == ring) {
            reg_rings_.erase(reg_rings_.begin() + i);
            reg_cbs_.erase(reg_cbs_.begin()   + i);
            break;
        }
    }
    // No per-region ring state to clean up: buf_index_base is ring-agnostic.
}

// ---------------------------------------------------------------------------
// AddRegion
// Must be called under extend_lock_ (but NOT under free_lock_ or
// registrar_lock_, which are acquired internally in the correct order).
//
// Lock ordering enforced throughout IouringMemPool:
//   extend_lock_  (coarsest – serialises region growth)
//     registrar_lock_  (ring registration callbacks)
//       free_lock_  (free-list access – fine-grained, brief)
// ---------------------------------------------------------------------------
bool IouringMemPool::AddRegion(size_t region_size_mb) {
    // Must be called under extend_lock_ (caller's responsibility).
    if (static_cast<int>(regions_.size()) >= FLAGS_iouring_mem_pool_max_regions) {
        LOG_EVERY_SECOND(ERROR)
            << "IouringMemPool: max regions (" << FLAGS_iouring_mem_pool_max_regions
            << ") reached.  Increase --iouring_mem_pool_max_regions.";
        return false;
    }

    const size_t region_size = region_size_mb * kBytesPerMB;
    // Round down to a multiple of block_size_.
    const size_t aligned_size = (region_size / block_size_) * block_size_;
    if (aligned_size == 0) {
        LOG(ERROR) << "IouringMemPool: region_size_mb too small";
        return false;
    }

    void* mem = nullptr;
    if (posix_memalign(&mem, 4096, aligned_size) != 0) {
        PLOG(ERROR) << "IouringMemPool: posix_memalign failed";
        return false;
    }
    memset(mem, 0, aligned_size);

    // Compute buf_index_base: sum of blocks in all existing regions.
    // Safe to read regions_ here because we hold extend_lock_.
    // The same value is used for every ring (all rings share the same
    // iovec layout), so we only need to store it once per Region.
    int buf_index_base = 0;
    for (const auto& r : regions_) {
        buf_index_base += r.block_count;
    }

    const int blocks = static_cast<int>(aligned_size / block_size_);

    Region region;
    region.base            = reinterpret_cast<uintptr_t>(mem);
    region.size            = aligned_size;
    region.block_size      = block_size_;
    region.buf_index_base  = buf_index_base;
    region.block_count     = blocks;

    // Notify all registered rings (registrar_lock_ < extend_lock_ in the
    // global ordering, but here extend_lock_ is already held, so we must NOT
    // acquire extend_lock_ inside registrar_lock_ elsewhere).
    {
        BAIDU_SCOPED_LOCK(registrar_lock_);
        for (size_t i = 0; i < reg_rings_.size(); ++i) {
            reg_cbs_[i](mem, aligned_size, block_size_, buf_index_base);
        }
    }

    regions_.push_back(std::move(region));
    // Publish the new region count atomically so GetBufIndex lock-free readers
    // can discover the new region without holding extend_lock_.
    region_count_.store(static_cast<int>(regions_.size()),
                        butil::memory_order_release);

    // Populate the global free-list with all blocks in the new region.
    // free_lock_ is the innermost lock; safe to acquire under extend_lock_.
    {
        BAIDU_SCOPED_LOCK(free_lock_);
        for (int i = blocks - 1; i >= 0; --i) {
            auto* node = reinterpret_cast<FreeNode*>(
                reinterpret_cast<char*>(mem) + i * block_size_);
            node->next   = global_free_;
            global_free_ = node;
        }
    }

    LOG(INFO) << "IouringMemPool: added region base=" << mem
              << " size_mb=" << region_size_mb
              << " blocks=" << blocks
              << " buf_index_base=" << buf_index_base;
    return true;
}

// ---------------------------------------------------------------------------
// Allocate / Deallocate
// ---------------------------------------------------------------------------

// TLS fast-path: no lock needed.
void* IouringMemPool::Allocate(size_t /*size*/) {
    // TLS fast-path: no contention.
    if (tls_free_) {
        FreeNode* node = tls_free_;
        tls_free_ = node->next;
        --tls_free_cnt_;
        return node;
    }

    // TLS cache is empty.  Try to refill from the global free-list.
    // If the global list is also empty, grow the pool first.
    // ---------------------------------------------------------------
    // Lock ordering: extend_lock_ (coarsest) → free_lock_ (finest).
    // We must NEVER hold free_lock_ while acquiring extend_lock_.
    // Strategy:
    //   1. Try free_lock_ briefly to steal blocks.
    //   2. If empty, release free_lock_, acquire extend_lock_, grow,
    //      release extend_lock_, then re-try free_lock_.
    // ---------------------------------------------------------------

    // Step 1: fast steal under free_lock_ alone.
    {
        BAIDU_SCOPED_LOCK(free_lock_);
        if (global_free_) {
            // There are already free blocks — just refill TLS.
            size_t moved = 0;
            const size_t target = kTlsCacheMax / 2;
            while (global_free_ && moved < target) {
                FreeNode* node = global_free_;
                global_free_ = node->next;
                node->next = tls_free_;
                tls_free_  = node;
                ++tls_free_cnt_;
                ++moved;
            }
            goto done;
        }
    }  // free_lock_ released here

    // Step 2: global list was empty — try to grow the pool.
    {
        BAIDU_SCOPED_LOCK(extend_lock_);  // serialize growth
        // Re-check under extend_lock_: another thread may have grown already.
        bool need_grow = false;
        {
            BAIDU_SCOPED_LOCK(free_lock_);
            need_grow = (global_free_ == nullptr);
        }
        if (need_grow) {
            if (!AddRegion(
                    static_cast<size_t>(FLAGS_iouring_mem_pool_increase_mb))) {
                LOG_EVERY_SECOND(ERROR)
                    << "IouringMemPool: out of memory, cannot grow.";
                return nullptr;
            }
        }
    }  // extend_lock_ released here

    // Step 3: steal blocks from the now-non-empty global list.
    {
        BAIDU_SCOPED_LOCK(free_lock_);
        if (!global_free_) { return nullptr; }  // still empty (shouldn't happen)
        size_t moved = 0;
        const size_t target = kTlsCacheMax / 2;
        while (global_free_ && moved < target) {
            FreeNode* node = global_free_;
            global_free_ = node->next;
            node->next = tls_free_;
            tls_free_  = node;
            ++tls_free_cnt_;
            ++moved;
        }
    }

done:
    if (!tls_free_) { return nullptr; }
    FreeNode* node = tls_free_;
    tls_free_ = node->next;
    --tls_free_cnt_;
    return node;
}

void IouringMemPool::Deallocate(void* ptr) {
    if (!ptr) { return; }

    // TLS fast-path: cache locally.
    if (tls_free_cnt_ < kTlsCacheMax) {
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = tls_free_;
        tls_free_  = node;
        ++tls_free_cnt_;
        return;
    }

    // TLS is full: flush half to the global list.
    BAIDU_SCOPED_LOCK(free_lock_);
    const size_t flush = kTlsCacheMax / 2;
    for (size_t i = 0; i < flush && tls_free_; ++i) {
        FreeNode* node = tls_free_;
        tls_free_ = node->next;
        --tls_free_cnt_;
        node->next   = global_free_;
        global_free_ = node;
    }
    // Then put the current block into TLS.
    auto* node = reinterpret_cast<FreeNode*>(ptr);
    node->next = tls_free_;
    tls_free_  = node;
    ++tls_free_cnt_;
}

// Static hooks for butil::iobuf.
void* IouringMemPool::MemPoolAllocate(size_t size) {
    return Instance().Allocate(size);
}
void IouringMemPool::MemPoolDeallocate(void* ptr) {
    Instance().Deallocate(ptr);
}

// ---------------------------------------------------------------------------
// GetBufIndex
//
// Hot path: called once per IOBuf block in CutFromIOBufList.
//
// Lock-free fast path: read region_count_ (acquire) to get a stable count,
// then walk regions_[0..count-1] without a lock.  This is safe because:
//   - regions_ only ever grows (entries are never removed or modified once
//     published).
//   - AddRegion stores to region_count_ with memory_order_release AFTER
//     pushing the new entry, so if we observe count == N we can safely
//     read regions_[0..N-1] without tearing.
//   - If AddRegion is running concurrently and we read a stale count we
//     simply miss the new region and return -1, causing the caller to fall
//     back to WRITEV (correct, just not zero-copy for that block).
//
// Slow path (ring lookup): once the region is found we still need to find
// the ring's buf_index_base, which lives in per-region parallel vectors.
// These are also append-only for a given region once published, so they
// are safe to read without locks after the region is visible.
// ---------------------------------------------------------------------------
int IouringMemPool::GetBufIndex(struct io_uring* ring, const void* ptr) const {
    if (!ptr) { return -1; }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    // Load the count with acquire semantics so we see all stores from
    // AddRegion that preceded the region_count_ store.
    const int count = region_count_.load(butil::memory_order_acquire);

    for (int ri = 0; ri < count; ++ri) {
        const Region& r = regions_[ri];
        if (addr < r.base || addr >= r.base + r.size) { continue; }

        // Found the containing region.  buf_index_base is identical for every
        // ring, so no per-ring lookup is needed — O(1) direct computation.
        const int block_offset =
            static_cast<int>((addr - r.base) / r.block_size);
        (void)ring;  // ring param kept for API compatibility / future use
        return r.buf_index_base + block_offset;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// IouringReadSlotPool – implementation
// ---------------------------------------------------------------------------

bool IouringReadSlotPool::Init(struct io_uring* ring,
                               int initial_slots,
                               int max_slot_count,
                               size_t slot_buf_size) {
    if (!ring || initial_slots <= 0 || max_slot_count < initial_slots
        || slot_buf_size == 0) {
        LOG(ERROR) << "IouringReadSlotPool::Init: invalid arguments";
        return false;
    }

    // Receive slots must come from IouringMemPool so that they reside in
    // pre-registered memory and the kernel can DMA directly into them
    // (IORING_OP_READ_FIXED) without per-op page pinning.
    IouringMemPool& mp = IouringMemPool::Instance();
    if (!mp.initialized()) {
        LOG(ERROR) << "IouringReadSlotPool::Init: IouringMemPool not initialised; "
                      "--iouring_register_buffers must be true";
        return false;
    }
    if (slot_buf_size != mp.block_size()) {
        LOG(ERROR) << "IouringReadSlotPool::Init: slot_buf_size " << slot_buf_size
                   << " != MemPool block_size " << mp.block_size();
        return false;
    }

    ring_           = ring;
    slot_buf_size_  = slot_buf_size;
    max_slot_count_ = max_slot_count;

    if (!GrowBy(initial_slots)) {
        ring_ = nullptr;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// IouringReadSlotPool::GrowBy
// ---------------------------------------------------------------------------
bool IouringReadSlotPool::GrowBy(int n) {
    if (total_slot_count_ + n > max_slot_count_) {
        n = max_slot_count_ - total_slot_count_;
        if (n <= 0) {
            LOG_EVERY_SECOND(WARNING)
                << "IouringReadSlotPool: max_slot_count reached ("
                << max_slot_count_ << ")";
            return false;
        }
    }

    const int base_idx = total_slot_count_;

    // All receive buffers come from IouringMemPool so they are automatically
    // covered by the ring's registered buffer table.
    IouringMemPool& mp = IouringMemPool::Instance();

    // Accumulate allocations before committing; allows clean rollback on OOM.
    std::vector<void*> allocated;
    allocated.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        void* blk = mp.Allocate(slot_buf_size_);
        if (!blk) {
            LOG(ERROR) << "IouringReadSlotPool: IouringMemPool OOM after "
                       << i << " of " << n << " receive buffers";
            for (void* p : allocated) { mp.Deallocate(p); }
            return false;
        }
        allocated.push_back(blk);
    }

    // Commit: add entries and mark all new slots as free.
    // Invariant preserved: entries_[k].buf is always slot index k.
    for (int i = 0; i < n; ++i) {
        entries_.push_back({allocated[static_cast<size_t>(i)]});
        free_slot_indices_.push_back(base_idx + i);
        ++total_slot_count_;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Acquire
// ---------------------------------------------------------------------------
bool IouringReadSlotPool::Acquire(IouringReadSlot* out) {
    if (free_slot_indices_.empty()) {
        const int grow = std::min(total_slot_count_,
                                  max_slot_count_ - total_slot_count_);
        if (grow <= 0 || !GrowBy(grow)) { return false; }
    }

    const int idx = free_slot_indices_.back();
    free_slot_indices_.pop_back();

    // entries_[k].buf is always slot index k (GrowBy appends in order),
    // so direct index access is O(1).  An out-of-range idx can only happen
    // due to memory corruption – DCHECK catches it in debug builds.
    DCHECK_GE(idx, 0);
    DCHECK_LT(idx, static_cast<int>(entries_.size()));
    const ReadSlotEntry& e = entries_[static_cast<size_t>(idx)];
    out->buf       = e.buf;
    out->buf_index = IouringMemPool::Instance().GetBufIndex(ring_, e.buf);
    out->size      = slot_buf_size_;
    out->slot_idx  = idx;
    return true;
}

// ---------------------------------------------------------------------------
// Release
// ---------------------------------------------------------------------------
void IouringReadSlotPool::Release(const IouringReadSlot& slot) {
    // slot.slot_idx was stored by Acquire; use it directly for O(1) release.
    const int idx = slot.slot_idx;
    DCHECK_GE(idx, 0);
    DCHECK_LT(idx, static_cast<int>(entries_.size()));
    // buf match is a sanity check: in correct usage Acquire always fills
    // slot_idx to match the entry, so this can only fail on double-release
    // or memory corruption.
    DCHECK_EQ(entries_[static_cast<size_t>(idx)].buf, slot.buf)
        << "IouringReadSlotPool::Release: buf mismatch at slot_idx=" << idx;
    free_slot_indices_.push_back(idx);
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------
void IouringReadSlotPool::Destroy() {
    if (!ring_) { return; }

    IouringMemPool& mp = IouringMemPool::Instance();
    for (auto& e : entries_) {
        mp.Deallocate(e.buf);
        e.buf = nullptr;
    }
    entries_.clear();
    free_slot_indices_.clear();
    total_slot_count_ = 0;
    ring_             = nullptr;
}

}  // namespace iouring
}  // namespace brpc

#endif  // BRPC_WITH_IOURING
