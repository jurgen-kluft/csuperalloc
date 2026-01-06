#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_binmap1.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"
#include "ccore/c_arena.h"

#include "csuperalloc/c_superheap.h"
#include "csuperalloc/c_superfsa.h"

namespace ncore
{
    namespace nsuperfsa
    {
        static inline void* toaddress(void* base, u64 offset) { return (void*)((ptr_t)base + offset); }
        static inline ptr_t todistance(void const* base, void const* ptr) { return (ptr_t)((ptr_t)ptr - (ptr_t)base); }

        const u32 NIL32 = 0xffffffff;
        const u16 NIL16 = 0xffff;

        struct blockconfig_t
        {
            u8 m_index;
            s8 m_sizeshift;
        };

        struct allocconfig_t
        {
            u8            m_index;
            s8            m_sizeshift;
            blockconfig_t m_block;
        };

        // Maximum number of items in a block is 32768 items
        // This struct is 32 bytes on a 64-bit system
        struct block_t
        {
            block_t*      m_next;
            block_t*      m_prev;
            u16           m_section_index;
            u16           m_section_block_index;
            u16           m_item_freeindex;
            u16           m_item_count;
            u16           m_item_count_max;
            u16           m_item_freelist;
            allocconfig_t m_alloc_config;

            void initialize(u16 section_index, u16 section_block_index, allocconfig_t const& alloccfg)
            {
                ASSERT(section_index < 0xFF);
                ASSERT(section_block_index < 0xFF);
                ASSERT((1 << (alloccfg.m_block.m_sizeshift - alloccfg.m_sizeshift)) <= 32768);

                m_next                = nullptr;
                m_prev                = nullptr;
                m_section_index       = section_index;
                m_section_block_index = section_block_index;
                m_item_freeindex      = 0;
                m_item_count          = 0;
                m_item_count_max      = (1 << (alloccfg.m_block.m_sizeshift - alloccfg.m_sizeshift));
                m_item_freelist       = NIL16;
                m_alloc_config        = alloccfg;
            }

            inline bool  is_full() const { return m_item_count == m_item_count_max; }
            inline bool  is_empty() const { return m_item_count == 0; }
            inline u32   ptr2idx(void const* const ptr, void const* const elem) const { return (u32)(((u64)elem - (u64)ptr) >> m_alloc_config.m_sizeshift); }
            inline void* idx2ptr(void* const ptr, u32 const index) const { return toaddress(ptr, ((u64)index << m_alloc_config.m_sizeshift)); }

            void* allocate_item(void* block_address, u32& item_index)
            {
                u16* item = nullptr;
                if (m_item_freelist != NIL16)
                {
                    item_index      = m_item_freelist;
                    item            = (u16*)idx2ptr(block_address, item_index);
                    m_item_freelist = item[0];
                }
                else if (m_item_freeindex < m_item_count_max)
                {
                    item_index = m_item_freeindex++;
                    item       = (u16*)idx2ptr(block_address, item_index);
                }
                else
                {
                    ASSERT(false);  // panic
                    item_index = NIL32;
                    return item;
                }

                m_item_count++;
#ifdef SUPERALLOC_DEBUG
                nmem::memset(item, 0xCDCDCDCD, ((u64)1 << m_alloc_config.m_sizeshift));
#endif
                ASSERT(m_section_index < 0xFF);
                ASSERT(m_section_block_index < 0xFF);

                item_index |= ((u32)m_section_index << 24) | ((u32)m_section_block_index << 16);
                return item;
            }

            void deallocate_item(void* block_address, u16 item_index)
            {
                ASSERT(m_item_count > 0);
                ASSERT(item_index < m_item_freeindex);
                u16* const pelem = (u16*)idx2ptr(block_address, item_index);
#ifdef SUPERALLOC_DEBUG
                nmem::memset(pelem, 0xFEFEFEFE, ((u64)1 << m_alloc_config.m_sizeshift));
#endif
                pelem[0]        = m_item_freelist;
                m_item_freelist = item_index;
                m_item_count--;
            }
        };

        struct section_t
        {
            void*         m_address;
            u32           m_address_range;
            u32           m_section_index;
            u16           m_block_free_index;
            u16           m_block_used_count;
            u16           m_block_max_count;
            blockconfig_t m_block_config;
            block_t*      m_block_array;
            u32           m_block_free_bin0;
            u32*          m_block_free_bin1;

            void     initialize(void* address, s8 section_size_shift, u8 section_index);
            void     deinitialize(superheap_t* heap);
            void     checkout(superheap_t* heap, void* address, s8 section_size_shift, u8 section_index, blockconfig_t const& blockcfg);
            block_t* checkout_block(allocconfig_t const& alloccfg);
            void     release_block(block_t* block);
            bool     is_ptr_inside(void const* ptr) const { return ptr >= m_address && ptr < toaddress(m_address, m_address_range); }
            u32      index_of_block(void const* ptr) const
            {
                ASSERT(is_ptr_inside(ptr));
                return (u32)(todistance(m_address, ptr) >> m_block_config.m_sizeshift);
            }
            void* address_of_block(u32 block) const
            {
                void* block_ptr = toaddress(m_address, (u64)block << m_block_config.m_sizeshift);
                ASSERT(is_ptr_inside(block_ptr));
                return block_ptr;
            }
            bool is_empty() const { return m_block_used_count == m_block_max_count; }

            inline void* idx2ptr(u32 i) const
            {
                if (i == NIL32)
                    return nullptr;
                u16 const            block_index   = (i >> 16) & 0xFF;
                u16 const            elem_index    = i & 0xFFFF;
                block_t const* const block         = &m_block_array[block_index];
                void* const          block_address = address_of_block(block_index);
                return block->idx2ptr(block_address, elem_index);
            }

            inline u32 ptr2idx(void const* ptr) const
            {
                if (ptr == nullptr)
                    return NIL32;

                u32 const            block_index   = index_of_block(ptr);
                block_t const* const block         = &m_block_array[block_index];
                void* const          block_address = address_of_block(block_index);
                u32 const            elem_index    = block->ptr2idx(block_address, ptr);
                return (m_section_index << 24) | (block_index << 16) | (elem_index & 0xFFFF);
            }
        };

        void section_t::initialize(void* address, s8 section_size_shift, u8 section_index)
        {
            m_address          = address;
            m_address_range    = 1 << section_size_shift;
            m_section_index    = section_index;
            m_block_config     = {0, 0};
            m_block_free_index = 0;
            m_block_max_count  = 0;
            m_block_used_count = 0;
            m_block_array      = nullptr;
            m_block_free_bin0  = 0xffffffff;
            m_block_free_bin1  = nullptr;
        }

        void section_t::checkout(superheap_t* heap, void* address, s8 section_size_shift, u8 section_index, blockconfig_t const& blockcfg)
        {
            m_address          = address;
            m_address_range    = 1 << section_size_shift;
            m_section_index    = section_index;
            m_block_config     = blockcfg;
            m_block_free_index = 0;
            m_block_max_count  = 1 << (section_size_shift - m_block_config.m_sizeshift);
            m_block_used_count = 0;
            m_block_array      = g_allocate_array<block_t>(heap, m_block_max_count);

            ASSERT(m_block_max_count <= 256);
            m_block_free_bin1 = (m_block_max_count > 32) ? g_allocate_array<u32>(heap, ((m_block_max_count - 1) >> 5) + 1) : nullptr;
            nbinmap10::setup_used_lazy(&m_block_free_bin0, m_block_free_bin1, m_block_max_count);

            v_alloc_commit(m_address, ((u32)1 << m_block_config.m_sizeshift) * m_block_max_count);
        }

        void section_t::deinitialize(superheap_t* heap)
        {
            nsuperheap::deallocate(heap, m_block_array);
            nsuperheap::deallocate(heap, m_block_free_bin1);
            m_block_array     = nullptr;
            m_block_free_bin1 = nullptr;
            m_block_free_bin0 = 0xffffffff;
            v_alloc_decommit(m_address, ((u32)1 << m_block_config.m_sizeshift) * m_block_max_count);
        }

        block_t* section_t::checkout_block(allocconfig_t const& alloccfg)
        {
            // Get a block and initialize it for this size
            s32 free_block_index = nbinmap10::find_and_set(&m_block_free_bin0, m_block_free_bin1, m_block_max_count);
            if (free_block_index < 0)
            {
                if (m_block_free_index < m_block_max_count)
                {
                    free_block_index = m_block_free_index++;
                }
                else
                {
                    // panic
                    ASSERT(false);
                    return nullptr;
                }
            }

#ifdef SUPERALLOC_DEBUG
            nmem::memset(address_of_block(free_block_index), 0xCDCDCDCD, (int_t)1 << m_block_config.m_sizeshift);
#endif
            m_block_used_count++;
            block_t* block = &m_block_array[free_block_index];
            block->initialize(m_section_index, free_block_index, alloccfg);
            return block;
        }

        void section_t::release_block(block_t* block)
        {
#ifdef SUPERALLOC_DEBUG
            nmem::memset(address_of_block(block->m_section_block_index), 0xFEFEFEFE, (int_t)1 << m_block_config.m_sizeshift);
#endif
            m_block_used_count--;
            // m_block_free_binmap.clr(m_block_max_count, block->m_section_block_index);
            nbinmap10::clr(&m_block_free_bin0, m_block_free_bin1, m_block_max_count, block->m_section_block_index);
        }

        // ------------------------------------------------------------------------------
        // ------------------------------------------------------------------------------
        // superfsa functions

        void     initialize(superfsa_t* fsa, superheap_t* heap, u64 address_range, u32 section_size);
        void     deinitialize(superfsa_t* fsa);
        void*    base_address(superfsa_t* fsa) { return fsa->m_address_base; }
        block_t* checkout_block(superfsa_t* fsa, allocconfig_t const& alloccfg);

        struct item_t
        {
            u32   m_index;  // format is u32[u8(segment-index):u24(item-index)]
            void* m_ptr;
        };

        item_t alloc_item(superfsa_t* fsa, u32 alloc_size);
        void   dealloc_item(superfsa_t* fsa, u32 item_index);
        void   deallocate(superfsa_t* fsa, void* item_ptr);

        void* idx2ptr(superfsa_t* fsa, u32 i)
        {
            if (i == NIL32)
                return nullptr;
            u32 const c = (i >> 24) & 0xFF;
            ASSERT(c < fsa->m_sections_array_size);
            return fsa->m_sections[c].idx2ptr(i);
        }

        u32 ptr2idx(superfsa_t* fsa, void const* ptr)
        {
            if (ptr == nullptr)
                return NIL32;
            u32 const section_index = (u32)(todistance(fsa->m_address_base, ptr) >> fsa->m_section_maxsize_shift);
            ASSERT(section_index < fsa->m_sections_array_size);
            u32 const idx = ((section_index & 0xFF) << 24) | fsa->m_sections[section_index].ptr2idx(ptr);
            return idx;
        }

        static inline allocconfig_t const& alloc_size_to_alloc_config(u32 alloc_size)
        {
            alloc_size = (alloc_size + 7) & ~7;
            alloc_size = math::ceilpo2(alloc_size);
            s8 const c = math::countTrailingZeros(alloc_size) - 3;
            return c_aalloc_config[c];
        }

        // blockconfig-index, block-size-shift
        static const blockconfig_t c64KB  = {0, 16};
        static const blockconfig_t c256KB = {1, 18};
        static const blockconfig_t c1MB   = {2, 20};
        static const blockconfig_t c4MB   = {3, 22};

        static const blockconfig_t c_ablock_config[] = {c64KB, c256KB, c1MB, c4MB};
        static const allocconfig_t c_aalloc_config[] = {
          // alloc-config { index, sizeshift, block-config}
          {0, 3, c64KB},     // 0,         8
          {1, 4, c64KB},     // 1,        16
          {2, 5, c64KB},     // 2,        32
          {3, 6, c64KB},     // 3,        64
          {4, 7, c64KB},     // 4,       128
          {5, 8, c64KB},     // 5,       256
          {6, 9, c64KB},     // 6,       512
          {7, 10, c64KB},    // 7,      1024
          {8, 11, c64KB},    // 8,      2048
          {9, 12, c64KB},    // 9,      4096
          {10, 13, c64KB},   // 10,      8192
          {11, 14, c64KB},   // 11,     16384
          {12, 15, c256KB},  // 12,     32768
          {13, 16, c256KB},  // 13,  64 * cKB
          {14, 17, c1MB},    // 14, 128 * cKB
          {15, 18, c1MB},    // 15, 256 * cKB
          {16, 19, c4MB},    // 16, 512 * cKB
          {17, 20, c4MB},    // 17,   1 * cMB
          {18, 21, c4MB},    // 18,   2 * cMB
        };

        static const u32 c_max_num_blocks = (u32)(DARRAYSIZE(c_ablock_config));
        static const u32 c_max_num_sizes  = (u32)(DARRAYSIZE(c_aalloc_config));

        void initialize(superfsa_t* fsa, superheap_t* heap, u64 address_range, u32 section_size)
        {
            fsa->m_heap                  = heap;
            fsa->m_address_range         = address_range;
            fsa->m_section_maxsize_shift = math::ilog2(section_size);
            fsa->m_sections_array_size   = (u32)(address_range >> fsa->m_section_maxsize_shift);
            fsa->m_sections              = g_allocate_array<section_t>(fsa->m_heap, fsa->m_sections_array_size);

            ASSERT(fsa->m_sections_array_size <= 256);
            fsa->m_sections_free_index = 0;
            fsa->m_sections_free_bin0  = 0xffffffff;
            fsa->m_sections_free_bin1  = g_allocate_array_and_memset<u32>(fsa->m_heap, 8, 0xffffffff);

            fsa->m_address_base   = v_alloc_reserve(address_range);
            void* section_address = fsa->m_address_base;
            for (u32 i = 0; i < fsa->m_sections_array_size; i++)
            {
                fsa->m_sections[i].initialize(section_address, section_size, i);
                section_address = toaddress(section_address, section_size);
            }

            fsa->m_active_section_bin0_per_blockcfg = g_allocate_array_and_memset<u32>(fsa->m_heap, c_max_num_blocks, 0xffffffff);
            fsa->m_active_section_bin1_per_blockcfg = g_allocate_array_and_memset<u32>(fsa->m_heap, c_max_num_blocks * 8, 0xffffffff);
            fsa->m_active_block_list_per_allocsize  = g_allocate_array_and_clear<block_t*>(fsa->m_heap, c_max_num_sizes);
        }

        void deinitialize(superfsa_t* fsa)
        {
            for (u32 i = 0; i < fsa->m_sections_free_index; i++)
                fsa->m_sections[i].deinitialize(fsa->m_heap);

            nsuperheap::deallocate(fsa->m_heap, fsa->m_active_block_list_per_allocsize);
            nsuperheap::deallocate(fsa->m_heap, fsa->m_active_section_bin0_per_blockcfg);
            nsuperheap::deallocate(fsa->m_heap, fsa->m_active_section_bin1_per_blockcfg);
            nsuperheap::deallocate(fsa->m_heap, fsa->m_sections_free_bin1);
            nsuperheap::deallocate(fsa->m_heap, fsa->m_sections);
            v_alloc_release(fsa->m_address_base, fsa->m_address_range);
        }

        static inline u32 sizeof_alloc(u32 alloc_size) { return math::ceilpo2((alloc_size + (8 - 1)) & ~(8 - 1)); }

        block_t* checkout_block(superfsa_t* fsa, allocconfig_t const& alloccfg)
        {
            blockconfig_t const& blockcfg      = c_ablock_config[alloccfg.m_block.m_index];
            u32*                 bin0          = &fsa->m_active_section_bin0_per_blockcfg[alloccfg.m_block.m_index];
            u32*                 bin1          = &fsa->m_active_section_bin1_per_blockcfg[alloccfg.m_block.m_index * 8];
            s32                  section_index = nbinmap10::find(bin0, bin1, fsa->m_sections_array_size);
            if (section_index < 0)
            {
                section_index = nbinmap10::find_and_set(&fsa->m_sections_free_bin0, fsa->m_sections_free_bin1, fsa->m_sections_array_size);
                if (section_index < 0)
                {
                    if (fsa->m_sections_free_index < fsa->m_sections_array_size)
                    {
                        section_index = fsa->m_sections_free_index;
                        fsa->m_sections_free_index++;
                    }
                    else
                    {
                        ASSERT(false);  // panic
                        return nullptr;
                    }
                }
                nbinmap10::clr(bin0, bin1, fsa->m_sections_array_size, section_index);

                section_t* section         = &fsa->m_sections[section_index];
                void*      section_address = toaddress(fsa->m_address_base, (s64)section_index << fsa->m_section_maxsize_shift);
                section->checkout(fsa->m_heap, section_address, fsa->m_section_maxsize_shift, section_index, blockcfg);
            }

            ASSERT(section_index >= 0 && section_index < (s32)fsa->m_sections_array_size);
            section_t* segment = &fsa->m_sections[section_index];
            block_t*   block   = segment->checkout_block(alloccfg);
            if (segment->is_empty())
            {
                // Segment is empty, remove it from the active set for this block config
                // bm.set(section_index, m_sections_array_size);
                nbinmap10::set(bin0, bin1, fsa->m_sections_array_size, section_index);
            }
            return block;
        }

        item_t alloc_item(superfsa_t* fsa, u32 alloc_size)
        {
            item_t alloc = {NIL32, nullptr};

            // Determine the block size for this allocation size
            // See if we have a segment available for this block size
            // If we do not have a segment available, we need to claim a new segment
            allocconfig_t const& alloccfg = alloc_size_to_alloc_config(alloc_size);
            ASSERT(alloc_size <= ((u32)1 << alloccfg.m_sizeshift));
            block_t* block = fsa->m_active_block_list_per_allocsize[alloccfg.m_index];
            if (block == nullptr)
            {
                block = checkout_block(fsa, alloccfg);
                if (block == nullptr)
                {
                    // panic
                    return alloc;
                }
                block->m_next                                            = block;
                block->m_prev                                            = block;
                fsa->m_active_block_list_per_allocsize[alloccfg.m_index] = block;
            }

            // Allocate item from active block, if it becomes full then remove it from the active list
            u32 const  section_index = block->m_section_index;
            section_t* segment       = &fsa->m_sections[section_index];

            alloc.m_ptr = block->allocate_item(segment->address_of_block(block->m_section_block_index), alloc.m_index);
            if (block->is_full())
            {
                block_t*& head = fsa->m_active_block_list_per_allocsize[alloccfg.m_index];
                if (head == block)
                {
                    head = block->m_next;
                    if (head == block)
                        head = nullptr;
                }
                block->m_prev->m_next = block->m_next;
                block->m_next->m_prev = block->m_prev;
            }

            // The allocation index also needs to encode the segment and block indices
            ASSERT(section_index < m_sections_array_size && section_index <= 0xFE);
            ASSERT(block->m_section_block_index < m_sections[section_index].m_block_max_count && block->m_section_block_index <= 0xFE);
            return alloc;
        }

        void dealloc_item(superfsa_t* fsa, u32 item_index)
        {
            u32 const section_index = (item_index >> 24) & 0xFF;
            u32 const block_index   = (item_index >> 16) & 0xFF;
            u32 const elem_index    = item_index & 0xFFFF;
            ASSERT(section_index < fsa->m_sections_array_size);
            section_t* segment = &fsa->m_sections[section_index];
            ASSERT(block_index < segment->m_block_max_count);
            block_t* block = &segment->m_block_array[block_index];
            ASSERT(elem_index < block->m_item_freeindex);
            u32 const alloc_config_index = block->m_alloc_config.m_index;

            bool const block_was_full = block->is_full();
            void*      block_address  = segment->address_of_block(block_index);
            block->deallocate_item(block_address, elem_index);
            if (block_was_full)
            {
                // This block is available again, add it to the active list
                block_t*& head = fsa->m_active_block_list_per_allocsize[alloc_config_index];
                if (head == nullptr)
                {
                    head          = block;
                    block->m_next = block;
                    block->m_prev = block;
                }
                else
                {
                    block->m_next        = head;
                    block->m_prev        = head->m_prev;
                    head->m_prev->m_next = block;
                    head->m_prev         = block;
                }
            }
        }

        void* allocate(superfsa_t* fsa, u32 size)
        {
            item_t item = alloc_item(fsa, size);
            return item.m_ptr;
        }

        void deallocate(superfsa_t* fsa, void* item_ptr)
        {
            u32 const item_index = ptr2idx(fsa, item_ptr);
            dealloc_item(fsa, item_index);
        }
    }  // namespace nsuperfsa
}  // namespace ncore
