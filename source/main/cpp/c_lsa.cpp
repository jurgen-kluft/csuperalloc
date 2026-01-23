#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_arena.h"
#include "ccore/c_binmap1.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"
#include "ccore/c_limits.h"

#include "csuperalloc/c_lsa.h"

namespace ncore
{
    struct lsa_t
    {
        byte* m_base_address;      // base address of the managed memory
        u32   m_base_offset;       // offset to the base address (unit = pages)
        u16   m_block_free_index;  // next unused block index
        u16   m_block_free_list;   // free list head (indices)
        u16   m_block_capacity;    // total blocks
        u16   m_block_count;       // currently allocated blocks
        u8    m_block_size_shift;  // log2(block size)
        u8    m_page_size_shift;   // log2(page size)
        u16   m_padding;           // padding for alignment
    };

    namespace nlsa
    {
        // Uncomment to enable debug fills
#define LSA_DEBUG

        struct block_t
        {
            u32 m_pages;  // number of committed pages for this block
            u16 m_next;   // index for linked lists
            u16 m_prev;   // index for linked lists
        };

        static inline byte* base_address(lsa_t* lsa) { return lsa->m_base_address + (lsa->m_base_offset << lsa->m_page_size_shift); }
        static inline bool  is_managed_by(lsa_t* lsa, void const* ptr) { return ptr >= base_address(lsa) && ptr < (base_address(lsa) + ((u64)lsa->m_block_capacity << lsa->m_block_size_shift)); }

        namespace nblock
        {
            static inline byte*    block_index_to_address(lsa_t* lsa, u16 block_index) { return base_address(lsa) + ((u64)block_index << lsa->m_block_size_shift); }
            static inline block_t* get_block_array(lsa_t* lsa) { return (block_t*)((byte*)lsa + sizeof(lsa_t)); }

            static inline u16      block_index_from_ptr(lsa_t* lsa, byte const* ptr) { return (u16)((u64)(ptr - base_address(lsa)) >> lsa->m_block_size_shift); }
            static inline block_t* block_from_index(lsa_t* lsa, u16 index)
            {
                ASSERT(index < lsa->m_block_capacity);
                block_t* block_array = get_block_array(lsa);
                return &block_array[index];
            }

            static inline u16 block_to_index(lsa_t* lsa, block_t* block)
            {
                block_t* block_array = get_block_array(lsa);
                return (u16)(block - &block_array[0]);
            }

            // allocate block metadata (does not commit the data pages of the block)
            block_t* allocate_block(lsa_t* lsa)
            {
                ASSERT(lsa->m_block_count < lsa->m_block_capacity);

                block_t* block       = nullptr;
                u16      block_index = D_NILL_U16;
                if (lsa->m_block_free_list != D_NILL_U16)
                {
                    block_index            = lsa->m_block_free_list;
                    block                  = block_from_index(lsa, block_index);
                    lsa->m_block_free_list = block->m_next;
                    if (lsa->m_block_free_list != D_NILL_U16)
                    {
                        block_t* head = block_from_index(lsa, lsa->m_block_free_list);
                        head->m_prev  = D_NILL_U16;
                    }
                }
                else if (lsa->m_block_free_index < lsa->m_block_capacity)
                {
                    block_index = lsa->m_block_free_index++;
                    block       = block_from_index(lsa, block_index);

                    // ensure block array metadata pages are committed as we grow the array
                    const u64 base_page_idx = ((u64)lsa >> lsa->m_page_size_shift);
                    const u64 prev_page_idx = ((u64)block >> lsa->m_page_size_shift);
                    const u64 next_page_idx = ((u64)(block + 1) >> lsa->m_page_size_shift);
                    if (next_page_idx > prev_page_idx)
                    {
                        const u64   page_offset = next_page_idx - base_page_idx;
                        const int_t page_size   = (int_t)1 << lsa->m_page_size_shift;
                        v_alloc_commit((void*)((u64)lsa + (page_offset << lsa->m_page_size_shift)), page_size);
                    }
                }
                else
                {
                    return nullptr;
                }

                block->m_next  = D_NILL_U16;
                block->m_prev  = D_NILL_U16;
                block->m_pages = 0;

                lsa->m_block_count++;
                return block;
            }

            void deallocate_block(lsa_t* lsa, block_t* block)
            {
                ASSERT(block->m_pages == 0);
                ASSERT(lsa->m_block_count > 0);

                block->m_next = lsa->m_block_free_list;
                block->m_prev = D_NILL_U16;
                if (lsa->m_block_free_list != D_NILL_U16)
                {
                    block_t* head = block_from_index(lsa, lsa->m_block_free_list);
                    head->m_prev  = block_to_index(lsa, block);
                }

                lsa->m_block_free_list = block_to_index(lsa, block);
                lsa->m_block_count--;
            }

            void activate(lsa_t* lsa, block_t* block, u32 alloc_size)
            {
                const u16   block_index   = block_to_index(lsa, block);
                byte*       block_address = block_index_to_address(lsa, block_index);
                const u32   num_pages     = ((u64)alloc_size + (((u64)1 << lsa->m_page_size_shift) - 1)) >> lsa->m_page_size_shift;
                const int_t commit_size   = (int_t)((u64)num_pages << lsa->m_page_size_shift);
                v_alloc_commit(block_address, commit_size);
#ifdef LSA_DEBUG
                nmem::memset(block_address, 0xCDCDCDCD, commit_size);
#endif
                block->m_pages = num_pages;
            }

            void deactivate(lsa_t* lsa, block_t* block)
            {
                if (block->m_pages > 0)
                {
                    byte*       block_address   = block_index_to_address(lsa, block_to_index(lsa, block));
                    const int_t committed_bytes = (int_t)block->m_pages << lsa->m_page_size_shift;
                    v_alloc_decommit(block_address, committed_bytes);
                    block->m_pages = 0;
                }
            }
        }  // namespace nblock

        // ------------------------------------------------------------------------------
        // lsa functions
        lsa_t* new_lsa(void* data, u32& data_page_offset, void* base, u32& base_page_offset, u32 sizeof_block, u16 num_blocks)
        {
            const u32 page_size        = v_alloc_get_page_size();
            const u8  page_size_shift  = v_alloc_get_page_size_shift();
            const u8  block_size_shift = (u8)math::ilog2(sizeof_block);
            const u16 block_capacity   = num_blocks;

            const u32   lsa_pages     = 1;
            const int_t lsa_size      = (u64)lsa_pages << page_size_shift;
            const u32   lsa_max_pages = (sizeof(lsa_t) + ((u64)block_capacity * sizeof(block_t)) + (page_size - 1)) >> page_size_shift;

            if (!v_alloc_commit(data, (int_t)lsa_size))
            {
                return nullptr;
            }

            lsa_t* lsa              = (lsa_t*)data;
            lsa->m_base_address     = (byte*)base;
            lsa->m_base_offset      = base_page_offset;
            lsa->m_block_free_index = 0;
            lsa->m_block_free_list  = D_NILL_U16;
            lsa->m_block_capacity   = block_capacity;
            lsa->m_block_count      = 0;
            lsa->m_block_size_shift = block_size_shift;
            lsa->m_page_size_shift  = page_size_shift;

            base_page_offset += ((int_t)block_capacity << block_size_shift) >> page_size_shift;
            data_page_offset += lsa_max_pages;

            return lsa;
        }

        lsa_t* new_lsa(u32 sizeof_block, u16 num_blocks)
        {
            const u32 page_size        = v_alloc_get_page_size();
            const u8  page_size_shift  = v_alloc_get_page_size_shift();
            const u8  block_size_shift = (u8)math::ilog2(sizeof_block);

            const u16 block_capacity = num_blocks;

            const int_t lsa_size       = (u64)1 << page_size_shift;
            const u32   lsa_full_pages = (sizeof(lsa_t) + ((u64)block_capacity * sizeof(block_t)) + (page_size - 1)) >> page_size_shift;

            const int_t address_range = ((int_t)lsa_full_pages << page_size_shift) + ((int_t)block_capacity << block_size_shift);
            void*       base_address  = v_alloc_reserve(address_range);
            if (base_address == nullptr)
                return nullptr;
            ASSERT(((u64)base_address & (u64)(page_size - 1)) == 0);  // should be page aligned

            if (!v_alloc_commit(base_address, (int_t)lsa_size))
            {
                v_alloc_release(base_address, address_range);
                return nullptr;
            }

            lsa_t* lsa              = (lsa_t*)base_address;
            lsa->m_base_address     = (byte*)base_address;
            lsa->m_base_offset      = lsa_full_pages;
            lsa->m_block_free_index = 0;
            lsa->m_block_free_list  = D_NILL_U16;
            lsa->m_block_capacity   = block_capacity;
            lsa->m_block_count      = 0;
            lsa->m_block_size_shift = block_size_shift;
            lsa->m_page_size_shift  = page_size_shift;

            return lsa;
        }

        void destroy(lsa_t* lsa)
        {
            int_t address_range = ((u64)lsa->m_base_offset << lsa->m_page_size_shift);
            address_range += ((u64)lsa->m_block_capacity << lsa->m_block_size_shift);
            v_alloc_release((void*)lsa, address_range);
        }

        void* allocate(lsa_t* lsa, u32 alloc_size)
        {
            // The allocation size should fall between (1 << (block_size_shift-1)) and (1 << block_size_shift), furthermore
            // it should not be zero.
            if (alloc_size == 0 || alloc_size > ((u32)1 << lsa->m_block_size_shift))
                return nullptr;

            // allocate a new block, and activate it
            block_t* block = nblock::allocate_block(lsa);
            if (block == nullptr)
                return nullptr;
            nblock::activate(lsa, block, alloc_size);

            const u16 block_index   = nblock::block_to_index(lsa, block);
            byte*     block_address = nblock::block_index_to_address(lsa, block_index);
            return block_address;
        }

        void deallocate(lsa_t* lsa, void* ptr)
        {
            if (ptr == nullptr)
                return;

            u16 const block_index = nblock::block_index_from_ptr(lsa, (byte const*)ptr);
            block_t*  block       = nblock::block_from_index(lsa, block_index);

            nblock::deactivate(lsa, block);
            nblock::deallocate_block(lsa, block);
        }

        u32 get_size(lsa_t* lsa, void* ptr)
        {
            if (ptr == nullptr)
                return 0;
            ASSERT(is_managed_by(lsa, ptr));
            const u16 block_index = nblock::block_index_from_ptr(lsa, (byte const*)ptr);
            ASSERT(block_index < lsa->m_block_free_index);
            const block_t* block = nblock::block_from_index(lsa, block_index);
            return (block->m_pages << lsa->m_page_size_shift);
        }

        // The maximum address range of the lsa is 4 GB, and since the smallest
        // allocation size is 8 bytes, we can shift right 3 bits and fit in u32.
        void* idx2ptr(lsa_t* lsa, u32 i)
        {
            const u64 dist = ((u64)i << 3);
            byte*     ptr  = base_address(lsa) + dist;
            ASSERT(is_managed_by(lsa, ptr));
            return (void*)ptr;
        }

        u32 ptr2idx(lsa_t* lsa, void* ptr)
        {
            if (ptr == nullptr)
                return D_NILL_U16;
            ASSERT(is_managed_by(lsa, ptr));
            const byte* base = base_address(lsa);
            const u64   dist = ((u64)((byte const*)ptr - base)) >> 3;
            ASSERT(dist <= 0xFFFFFFFF);
            return (u32)dist;
        }
    }  // namespace nlsa
}  // namespace ncore
