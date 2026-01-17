#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_arena.h"
#include "ccore/c_binmap1.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"
#include "ccore/c_limits.h"

#include "csuperalloc/private/c_list.h"
#include "csuperalloc/c_fsa.h"

namespace ncore
{
    struct fsa_t
    {
        u32 m_base_offset;            // offset to the base address
        u16 m_header_pages;           // number of pages used by fsa header
        u32 m_block_free_index;       //
        u32 m_block_free_list;        //
        u32 m_block_capacity;         //
        u32 m_block_count;            //
        u8  m_block_size_shift;       //
        u8  m_page_size_shift;        //
        u32 m_active_block_list[16];  // head of the active block list (per alloc size, we have 16 sizes)
    };

    namespace nfsa
    {

// Constraints
#define D_MAX_BLOCKS          65534  // maximum number of blocks
#define D_MAX_ITEMS_PER_BLOCK 8192   // maximum number of items in a block

        // Uncomment to enable debug fills
#define FSA_DEBUG

        // One large address space, the first page(s) contain the fsa_t struct
        // as well as the start of the block_t[] array.
        //

        static const u8 c64KB = 16;

        // Maximum number of items in a block is 32768 items
        // This struct is 16 bytes on a 64-bit system
        struct block_t
        {
            u16 m_item_freeindex;    // index of the next free item if freelist is empty
            u16 m_item_count;        // current number of allocated items
            u16 m_item_freelist;     // index of the first free item in the freelist, D_NILL_U32 if none
            u8  m_alloc_size_shift;  // allocation size shift
            u8  m_padding;           //
            u32 m_next;              //
            u32 m_prev;              //

            inline u16 capacity() const { return (1 << (c64KB - m_alloc_size_shift)); }
        };

        static inline void* toaddress(void* base, u64 offset) { return (void*)((ptr_t)base + offset); }
        static inline ptr_t todistance(void const* base, void const* ptr) { return (ptr_t)((ptr_t)ptr - (ptr_t)base); }
        static inline byte* base_address(fsa_t* fsa) { return (byte*)fsa + fsa->m_base_offset; }
        static inline bool  is_managed_by(fsa_t* fsa, void const* ptr) { return ptr >= base_address(fsa) && ptr < toaddress(base_address(fsa), ((u64)fsa->m_block_capacity << fsa->m_block_size_shift)); }

        namespace nblock
        {
            inline bool is_full(block_t* block) { return block->m_item_count == block->capacity(); }
            inline bool is_empty(block_t* block) { return block->m_item_count == 0; }

            inline u16   ptr_to_item_idx(u8 alloc_size_shift, const byte* block_address, const byte* elem) { return (u16)(((u64)elem - (u64)block_address) >> alloc_size_shift); }
            inline void* item_idx_to_ptr(u8 alloc_size_shift, byte* block_address, u16 index) { return toaddress(block_address, ((u64)index << alloc_size_shift)); }

            void* allocate_item(block_t* block, byte* block_address)
            {
                u16* item = nullptr;
                if (block->m_item_freelist != D_NILL_U32)
                {
                    const u16 item_index   = block->m_item_freelist;
                    item                   = (u16*)item_idx_to_ptr(block->m_alloc_size_shift, block_address, item_index);
                    block->m_item_freelist = item[0];
                }
                else if (block->m_item_freeindex < block->capacity())
                {
                    const u16 item_index = block->m_item_freeindex++;
                    item                 = (u16*)item_idx_to_ptr(block->m_alloc_size_shift, block_address, item_index);
                }
                else
                {
                    ASSERT(false);  // panic
                    return item;
                }

                block->m_item_count++;
#ifdef FSA_DEBUG
                nmem::memset(item, 0xCDCDCDCD, ((u64)1 << block->m_alloc_size_shift));
#endif
                return item;
            }

            void deallocate_item(block_t* block, byte* block_address, u16 item_index)
            {
                ASSERT(block->m_item_count > 0);
                ASSERT(item_index < block->m_item_freeindex);
                u16* const item = (u16*)item_idx_to_ptr(block->m_alloc_size_shift, block_address, item_index);
#ifdef FSA_DEBUG
                nmem::memset(item, 0xFEFEFEFE, ((u64)1 << block->m_alloc_size_shift));
#endif
                item[0]                = block->m_item_freelist;
                block->m_item_freelist = item_index;
                block->m_item_count--;
            }

            static inline block_t* get_block_array(fsa_t* fsa) { return (block_t*)((byte*)fsa + sizeof(fsa_t)); }

            static inline block_t* block_from_index(fsa_t* fsa, u16 index)
            {
                ASSERT(index < section->m_block_max_count);
                block_t* block_array = get_block_array(fsa);
                return &block_array[index];
            }

            static inline u32 block_to_index(fsa_t* fsa, block_t* block)
            {
                block_t* block_array = get_block_array(fsa);
                return (u32)(block - &block_array[0]);
            }

            static inline u32& get_active_block_list(fsa_t* fsa, u8 alloc_size_shift)
            {
                ASSERT(alloc_size_shift >= 16 && alloc_size_shift < 32);
                return fsa->m_active_block_list[alloc_size_shift - c64KB];
            }

            static inline block_t* get_active_block(fsa_t* fsa, u8 alloc_size_shift)
            {
                ASSERT(alloc_size_shift >= 16 && alloc_size_shift < 32);
                const u32 head = get_active_block_list(fsa, alloc_size_shift);
                if (head == D_NILL_U32)
                    return nullptr;
                return block_from_index(fsa, head);
            }

            static inline void rem_active_block(fsa_t* fsa, block_t* block)
            {
                u32&      head        = get_active_block_list(fsa, block->m_alloc_size_shift);
                const u32 block_index = block_to_index(fsa, block);
                if (head == block_index)
                {
                    head = block->m_next;
                    if (head != D_NILL_U32)
                    {
                        block_t* head_block = block_from_index(fsa, head);
                        head_block->m_prev  = D_NILL_U32;
                    }
                }
                else
                {
                    if (block->m_prev != D_NILL_U32)
                    {
                        block_t* prev_block = block_from_index(fsa, block->m_prev);
                        prev_block->m_next  = block->m_next;
                    }
                    if (block->m_next != D_NILL_U32)
                    {
                        block_t* next_block = block_from_index(fsa, block->m_next);
                        next_block->m_prev  = block->m_prev;
                    }
                }
            }

            static inline void add_active_block(fsa_t* fsa, block_t* block)
            {
                u32&      head        = get_active_block_list(fsa, block->m_alloc_size_shift);
                const u32 block_index = block_to_index(fsa, block);
                if (head == D_NILL_U32)
                {
                    head          = block_index;
                    block->m_next = D_NILL_U32;
                    block->m_prev = D_NILL_U32;
                }
                else
                {
                    block_t* head_block = block_from_index(fsa, head);
                    head_block->m_prev  = block_index;
                    block->m_next       = head;
                    block->m_prev       = D_NILL_U32;
                    head                = block_index;
                }
            }

            static inline byte* get_block_address(fsa_t* fsa, u32 block_index) { return (byte*)fsa + (fsa->m_base_offset) + ((u64)block_index << fsa->m_block_size_shift); }
            static inline u32   get_block_index(fsa_t* fsa, byte const* ptr) { return (u32)((u64)(ptr - base_address(fsa)) >> fsa->m_block_size_shift); }

            block_t* allocate_block(fsa_t* fsa, u8 alloc_size_shift)
            {
                ASSERT(alloc_size_shift < 32);
                block_t* block       = nullptr;
                u32      block_index = D_NILL_U32;
                if (fsa->m_block_free_list != D_NILL_U32)
                {
                    block_index            = fsa->m_block_free_list;
                    block                  = block_from_index(fsa, block_index);
                    fsa->m_block_free_list = block->m_next;
                }
                else if (fsa->m_block_free_index < fsa->m_block_capacity)
                {
                    block_index = fsa->m_block_free_index++;
                    block       = block_from_index(fsa, block_index);

                    // TODO check if we need to commit more pages for the block array!
                }
                else
                {
                    return nullptr;
                }

                block->m_next             = D_NILL_U32;
                block->m_prev             = D_NILL_U32;
                block->m_item_freeindex   = 0;
                block->m_item_count       = 0;
                block->m_item_freelist    = D_NILL_U32;
                block->m_alloc_size_shift = alloc_size_shift;
                block->m_padding          = 0;

                fsa->m_block_count++;
                return block;
            }

            void deallocate_block(fsa_t* fsa, block_t* block)
            {
                block->m_next           = fsa->m_block_free_list;
                block->m_item_freeindex = 0;
                block->m_item_count     = 0;
                block->m_item_freelist  = D_NILL_U32;

                fsa->m_block_free_list = block_to_index(fsa, block);
                fsa->m_block_count--;
            }

            void activate(fsa_t* fsa, block_t* block)
            {
                ASSERT((1 << (fsa->m_block_size_shift - block->m_alloc_size_shift)) <= D_MAX_ITEMS_PER_BLOCK);

                // commit pages for the actual memory used by this block
                const u32   block_index   = block_to_index(fsa, block);
                int_t const block_size    = (int_t)1 << fsa->m_block_size_shift;
                byte*       block_address = get_block_address(fsa, block_index);
                v_alloc_commit(block_address, block_size);
#ifdef FSA_DEBUG
                nmem::memset(block_address, 0xCDCDCDCD, block_size);
#endif
            }

            void deactivate(fsa_t* fsa, block_t* block)
            {
                ASSERT(block->m_item_count == 0);
                int_t const block_size    = (int_t)1 << fsa->m_block_size_shift;
                byte*       block_address = get_block_address(fsa, get_block_index(fsa, (byte*)block));
                v_alloc_decommit(block_address, block_size);
            }
        }  // namespace nblock

        // ------------------------------------------------------------------------------
        // ------------------------------------------------------------------------------
        // fsa functions

        static inline u8 alloc_size_to_size_shift(u32 alloc_size)
        {
            const u8 c = math::ilog2(alloc_size);
            if (alloc_size > ((u32)1 << c))
                return c + 1;
            return c;
        }

        fsa_t* new_fsa(u64 address_range)
        {
            const u32 page_size       = v_alloc_get_page_size();
            const u8  page_size_shift = v_alloc_get_page_size_shift();

            const u8  block_size_shift = c64KB;  // 64 KB blocks
            const u32 block_capacity   = (u32)(address_range >> block_size_shift);
            ASSERT(block_capacity <= D_MAX_BLOCKS);

            // Calculate how many pages to commit for fsa_t, active section list, section array and per section active block lists
            int_t fsa_bytes = sizeof(fsa_t);
            fsa_bytes += (D_MAX_BLOCKS * sizeof(block_t));

            ASSERT(fsa_pages <= (1 << 6));                                                  // must easily fit within 64 pages
            const u32 fsa_pages = (u32)((fsa_bytes + (page_size - 1)) >> page_size_shift);  // round up to pages

            // Calculate the actual address range we need to reserve that includes the fsa struct and block array
            address_range = (u64)fsa_pages << page_size_shift;
            address_range += (u64)block_capacity << block_size_shift;

            void* base_address = v_alloc_reserve(address_range);
            if (base_address == nullptr)
                return nullptr;

            if (!v_alloc_commit(base_address, (int_t)fsa_pages << page_size_shift))
            {
                v_alloc_release(base_address, address_range);
                return nullptr;
            }

            fsa_t* fsa              = (fsa_t*)base_address;
            fsa->m_block_free_index = 0;
            fsa->m_block_free_list  = D_NILL_U32;
            fsa->m_block_capacity   = block_capacity;
            fsa->m_block_size_shift = block_size_shift;
            fsa->m_page_size_shift  = page_size_shift;
            fsa->m_header_pages     = (u16)fsa_pages;
            fsa->m_base_offset      = (u32)(fsa_pages << page_size_shift);

            return fsa;
        }

        void destroy(fsa_t* fsa)
        {
            int_t address_range = (u64)fsa->m_base_offset;
            address_range += ((u64)fsa->m_block_capacity << fsa->m_block_size_shift);
            v_alloc_release((void*)fsa, address_range);
        }

        void* allocate(fsa_t* fsa, u32 alloc_size)
        {
            const u8 alloc_size_shift = alloc_size_to_size_shift(alloc_size);
            ASSERT(alloc_size <= ((u32)1 << alloc_size_shift));

            block_t* block = nblock::get_active_block(fsa, alloc_size_shift);
            if (block == nullptr)
            {
                // allocate a new block, and activate it
                block = nblock::allocate_block(fsa, alloc_size_shift);
                nblock::activate(fsa, block);
                nblock::add_active_block(fsa, block);

                // TODO is section now empty, if so remove it from the active section list
            }

            const u32 block_index   = nblock::block_to_index(fsa, block);
            byte*     block_address = nblock::get_block_address(fsa, block_index);
            void*     item          = nblock::allocate_item(block, block_address);
            if (nblock::is_full(block))
            {
                nblock::rem_active_block(fsa, block);
            }
            return item;
        }

        void deallocate(fsa_t* fsa, void* ptr)
        {
            if (ptr == nullptr)
                return;

            u32 const block_index   = nblock::get_block_index(fsa, (byte const*)ptr);
            block_t*  block         = nblock::block_from_index(fsa, block_index);
            byte*     block_address = nblock::get_block_address(fsa, block_index);
            u16 const item_index    = nblock::ptr_to_item_idx(block->m_alloc_size_shift, block_address, (byte const*)ptr);

            const bool was_full = nblock::is_full(block);
            nblock::deallocate_item(block, block_address, item_index);
            if (nblock::is_empty(block))
            {
                if (!was_full)
                {
                    nblock::rem_active_block(fsa, block);
                }
                nblock::deallocate_block(fsa, block);
                nblock::deactivate(fsa, block);
                // TODO Is section now empty?, if so release section and add to free list
            }
            else if (was_full)
            {
                // when a block was full it was not part of the active block list, but now that
                // we have deallocated an item, it has free items again, so add it back to the active list
                nblock::add_active_block(fsa, block);
            }
        }

        u32 get_size(fsa_t* fsa, void* ptr)
        {
            if (ptr == nullptr)
                return 0;
            ASSERT(is_managed_by(fsa, ptr));
            u32 const block_index = nblock::get_block_index(fsa, (byte const*)ptr);
            ASSERT(block_index < section->m_block_free_index);
            block_t* block = nblock::block_from_index(fsa, block_index);
            return ((u32)1 << block->m_alloc_size_shift);
        }

        // The maximum address range of the fsa is 4 GB, and since the smallest
        // allocation size is 8 bytes, we can shift right 3 bits and fit in u32.
        void* idx2ptr(fsa_t* fsa, u32 i)
        {
            const u64 dist = ((u64)i << 3);
            return toaddress(base_address(fsa), dist);
        }

        u32 ptr2idx(fsa_t* fsa, void* ptr)
        {
            if (ptr == nullptr)
                return D_NILL_U32;
            const byte* base = base_address(fsa);
            const u64   dist = ((u64)((byte const*)ptr - base)) >> 3;
            ASSERT(dist <= 0xFFFFFFFF);
            return (u32)dist;
        }
    }  // namespace nfsa
}  // namespace ncore
