#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "cbase/c_integer.h"

#include "csuperalloc/private/c_doubly_linked_list.h"
#include "csuperalloc/private/c_binmap.h"
#include "csuperalloc/c_superalloc.h"
#include "csuperalloc/c_superalloc_config.h"

#include "cvmem/c_virtual_memory.h"

namespace ncore
{
    // @TODO: Deal with jittering between block checkout/release

#define SUPERALLOC_DEBUG

    static inline void* toaddress(void* base, u64 offset) { return (void*)((ptr_t)base + offset); }
    static inline ptr_t todistance(void const* base, void const* ptr)
    {
        ASSERT(ptr >= base);
        return (ptr_t)((ptr_t)ptr - (ptr_t)base);
    }

    // Can only allocate, used internally to allocate initially required memory
    namespace nsuperheap
    {
        class alloc_t
        {
        public:
            void* m_address;
            u64   m_address_range;
            u64   m_ptr;
            u32   m_allocsize_alignment;
            u32   m_page_size;
            u32   m_page_size_shift;
            u32   m_page_count_current;
            u32   m_page_count_maximum;

            void initialize(u64 memory_range, u64 size_to_pre_allocate);
            void deinitialize();

            void* alloc(u32 size);   // allocate
            void* calloc(u32 size);  // allocate and clear
            void  dealloc(void* ptr);
        };

        void alloc_t::initialize(u64 memory_range, u64 size_to_pre_allocate)
        {
            u32 attributes  = 0;
            m_address_range = memory_range;
            vmem_t::reserve(memory_range, m_page_size, attributes, m_address);
            m_page_size_shift     = math::ilog2(m_page_size);
            m_allocsize_alignment = 32;
            m_page_count_maximum  = (u32)(memory_range >> m_page_size_shift);
            m_page_count_current  = 0;
            m_ptr                 = 0;

            if (size_to_pre_allocate > 0)
            {
                u32 const pages_to_commit = (u32)(math::alignUp(size_to_pre_allocate, (u64)m_page_size) >> m_page_size_shift);
                vmem_t::commit(m_address, m_page_size, pages_to_commit);
                m_page_count_current = pages_to_commit;
            }
        }

        void alloc_t::deinitialize()
        {
            vmem_t::release(m_address, m_address_range);
            m_address             = nullptr;
            m_address_range       = 0;
            m_allocsize_alignment = 0;
            m_page_size           = 0;
            m_page_size_shift     = 0;
            m_page_count_current  = 0;
            m_page_count_maximum  = 0;
            m_ptr                 = 0;
        }

        void* alloc_t::alloc(u32 size)
        {
            if (size == 0)
                return nullptr;
            size        = math::alignUp(size, m_allocsize_alignment);
            u64 ptr_max = ((u64)m_page_count_current << m_page_size_shift);
            if ((m_ptr + size) > ptr_max)
            {
                // add more pages
                u32 const page_count           = (u32)(math::alignUp(m_ptr + size, (u64)m_page_size) >> m_page_size_shift);
                u32 const page_count_to_commit = page_count - m_page_count_current;
                u64       commit_base          = ((u64)m_page_count_current << m_page_size_shift);
                vmem_t::commit(toaddress(m_address, commit_base), m_page_size, page_count_to_commit);
                m_page_count_current += page_count_to_commit;
            }
            void* ptr = (void*)((ptr_t)m_ptr + (ptr_t)m_address);
            m_ptr += size;
#ifdef SUPERALLOC_DEBUG
            nmem::memset(ptr, 0xCDCDCDCD, (u64)size);
#endif
            return ptr;
        }

        void* alloc_t::calloc(u32 size)
        {
            void* ptr = alloc(size);
            size      = math::alignUp(size, m_allocsize_alignment);
            nmem::memset(ptr, 0, (u64)size);
            return ptr;
        }

        void alloc_t::dealloc(void* ptr)
        {
            if (ptr == nullptr)
                return;
            // Purely some validation
            ASSERT(ptr >= m_address);
            ASSERT(ptr >= m_address && (((uint_t)ptr - (uint_t)m_address) < (uint_t)m_address_range));
        }
    }  // namespace nsuperheap

    namespace nsuperfsa
    {
        const u32 NIL = 0xffffffff;

        struct allocconfig_t
        {
            u8 m_index;
            s8 m_alloc_size_shift;
            u8 m_block_index;
            s8 m_block_size_shift;
        };
        struct blockconfig_t
        {
            u8 m_index;
            s8 m_block_size_shift;
        };

        // Maximum number of items in a block is 2^16 = 65536 items
        struct block_t
        {
            static const u16 NIL16 = 0xffff;

            block_t*      m_next;
            block_t*      m_prev;
            u16           m_segment_index;
            u16           m_segment_block_index;
            u16           m_item_freepos;
            u16           m_item_count;
            u16           m_item_count_max;
            u16           m_item_freelist;
            allocconfig_t m_alloc_config;

            void initialize(u16 segment_index, u16 segment_block_index, allocconfig_t const& alloccfg)
            {
                m_next                = nullptr;
                m_prev                = nullptr;
                m_segment_index       = segment_index;
                m_segment_block_index = segment_block_index;
                m_item_freepos        = 0;
                m_item_count          = 0;
                m_item_count_max      = (1 << (alloccfg.m_block_size_shift - alloccfg.m_alloc_size_shift));
                m_item_freelist       = NIL16;
                m_alloc_config        = alloccfg;
            }

            inline bool  is_full() const { return m_item_count == m_item_count_max; }
            inline bool  is_empty() const { return m_item_count == 0; }
            inline u32   ptr2idx(void const* const ptr, void const* const elem) const { return (u32)(((u64)elem - (u64)ptr) >> m_alloc_config.m_alloc_size_shift); }
            inline void* idx2ptr(void* const ptr, u32 const index) const { return toaddress(ptr, ((u64)index << m_alloc_config.m_alloc_size_shift)); }

            void* allocate_item(void* block_address, u32& item_index)
            {
                u16* item  = nullptr;
                if (m_item_freelist != NIL16)
                {
                    item_index      = m_item_freelist;
                    item            = (u16*)idx2ptr(block_address, item_index);
                    m_item_freelist = item[0];
                }
                else if (m_item_freepos < m_item_count_max)
                {
                    item_index = m_item_freepos++;
                    item       = (u16*)idx2ptr(block_address, item_index);
                }
                else
                {
                    item_index = NIL;
                    return item;  // panic
                }

                m_item_count++;
#ifdef SUPERALLOC_DEBUG
                nmem::memset(item, 0xCDCDCDCD, ((u64)1 << m_alloc_config.m_alloc_size_shift));
#endif

                item_index |= ((u32)m_segment_index << 24) | ((u32)m_segment_block_index << 16);
                return item;
            }

            void deallocate_item(void* block_address, u16 item_index)
            {
                ASSERT(m_item_count > 0);
                ASSERT(item_index < m_item_freepos);
                u16* const pelem = (u16*)idx2ptr(block_address, item_index);
#ifdef SUPERALLOC_DEBUG
                nmem::memset(pelem, 0xFEFEFEFE, ((u64)1 << m_alloc_config.m_alloc_size_shift));
#endif
                pelem[0]        = m_item_freelist;
                m_item_freelist = item_index;
                m_item_count--;
            }
        };

        struct segment_t
        {
            void*         m_address;
            u32           m_address_range;
            bool          m_committed;
            u32           m_segment_index;
            blockconfig_t m_block_config;
            u32           m_block_free_index;
            u32           m_block_used_count;
            u32           m_block_max_count;
            block_t*      m_block_array;
            binmap_t      m_block_free_binmap;

            void     initialize(void* address, u32 address_range, u8 segment_index);
            void     deinitialize(nsuperheap::alloc_t* heap);
            void     checkout(nsuperheap::alloc_t* heap, s8 segment_size_shift, blockconfig_t const& blockcfg);
            block_t* checkout_block(allocconfig_t const& alloccfg);
            void     release_block(block_t* block);
            void*    address_of_block(u32 block) const { return toaddress(m_address, (u64)block << m_block_config.m_block_size_shift); }
            bool     is_empty() const { return m_block_used_count == m_block_max_count; }

            block_t* get_block_from_address(void const* ptr) const
            {
                ASSERT(ptr >= m_address && ptr < toaddress(m_address, m_address_range));
                return &m_block_array[(u32)(todistance(m_address, ptr) >> m_block_config.m_block_size_shift)];
            }

            inline void* idx2ptr(u32 i) const
            {
                if (i == NIL)
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
                    return NIL;
                u32 const            block_index   = (u32)(todistance(m_address, ptr) >> m_block_config.m_block_size_shift);
                block_t const* const block         = &m_block_array[block_index];
                void* const          block_address = address_of_block(block_index);
                u32 const            elem_index    = block->ptr2idx(block_address, ptr);
                return (m_segment_index<<24) | (block_index << 16) | (elem_index & 0xFFFF);
            }
        };

        void segment_t::initialize(void* address, u32 address_range, u8 index)
        {
            m_address           = address;
            m_address_range     = address_range;
            m_committed         = false;
            m_segment_index     = index;
            m_block_config      = {0};
            m_block_free_index  = 0;
            m_block_max_count   = 0;
            m_block_used_count  = 0;
            m_block_array       = nullptr;
            m_block_free_binmap = {};
        }

        void segment_t::checkout(nsuperheap::alloc_t* heap, s8 segment_size_shift, blockconfig_t const& blockcfg)
        {
            m_block_config     = blockcfg;
            m_block_max_count  = 1 << (segment_size_shift - m_block_config.m_block_size_shift);
            m_block_array      = (block_t*)heap->alloc(m_block_max_count * sizeof(block_t));
            m_block_free_index = 0;

            u32 l0len, l1len, l2len, l3len;
            binmap_t::compute_levels(m_block_max_count, l0len, l1len, l2len, l3len);
            u32* l1 = l1len > 0 ? (u32*)heap->alloc(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
            u32* l2 = l2len > 0 ? (u32*)heap->alloc(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
            u32* l3 = l3len > 0 ? (u32*)heap->alloc(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
            m_block_free_binmap.init_lazy_1(m_block_max_count, l0len, l1, l1len, l2, l2len, l3, l3len);

            vmem_t::commit(m_address, (u32)1 << m_block_config.m_block_size_shift, m_block_max_count);
            m_committed = true;
        }

        void segment_t::deinitialize(nsuperheap::alloc_t* heap)
        {
            heap->dealloc(m_block_free_binmap.m_l[0]);
            heap->dealloc(m_block_free_binmap.m_l[1]);
            heap->dealloc(m_block_free_binmap.m_l[2]);
            heap->dealloc(m_block_array);
            vmem_t::decommit(m_address, (u32)1 << m_block_config.m_block_size_shift, m_block_max_count);
        }

        block_t* segment_t::checkout_block(allocconfig_t const& alloccfg)
        {
            // Get a block and initialize it for this size
            s32 free_block_index = m_block_free_binmap.findandset();
            if (free_block_index < 0)
            {
                if (m_block_free_index < m_block_max_count)
                {
                    free_block_index = m_block_free_index;
                    m_block_free_binmap.lazy_init_1(m_block_free_index);
                    m_block_free_index++;
                }
                else
                {
                    // panic
                    ASSERT(false); 
                    return nullptr;
                }
            }

#ifdef SUPERALLOC_DEBUG
            nmem::memset(address_of_block(free_block_index), 0xCDCDCDCD, (int_t)1 << m_block_config.m_block_size_shift);
#endif
            m_block_used_count++;
            block_t* block = &m_block_array[free_block_index];
            block->initialize(m_segment_index, free_block_index, alloccfg);
            return block;
        }

        void segment_t::release_block(block_t* block)
        {
#ifdef SUPERALLOC_DEBUG
            nmem::memset(address_of_block(block->m_segment_block_index), 0xFEFEFEFE, (int_t)1 << m_block_config.m_block_size_shift);
#endif
            m_block_used_count--;
            m_block_free_binmap.clr(block->m_segment_block_index);
        }

        // @note: The format of the returned index is u32[u8(segment-index):u24(item-index)]
        class alloc_t : public dexer_t
        {
        public:
            void         initialize(nsuperheap::alloc_t* heap, u64 address_range, u32 segment_size);
            void         deinitialize(nsuperheap::alloc_t* heap);
            u32          sizeof_alloc(u32 size) const;
            inline void* base_address() const { return m_address_base; }
            inline u32   alloc(u32 size) { return alloc_item(size).m_index; }
            inline void  dealloc(u32 index) { dealloc_item(index); }
            inline void* allocptr(u32 size) { return alloc_item(size).m_ptr; }
            inline void  deallocptr(void* ptr) { dealloc_item_ptr(ptr); }

            void* v_idx2ptr(u32 i) const override final
            {
                if (i == NIL)
                    return nullptr;
                u32 const c = (i >> 24) & 0xFF;
                ASSERT(c < m_segments_array_size);
                return m_segments[c].idx2ptr(i);
            }

            u32 v_ptr2idx(void* ptr) const override final
            {
                if (ptr == nullptr)
                    return NIL;
                u32 const segment_index = (u32)(todistance(m_address_base, ptr) >> m_segment_size_shift);
                ASSERT(segment_index < m_segments_array_size);
                u32 const idx = ((segment_index & 0xFF) << 24) | m_segments[segment_index].ptr2idx(ptr);
                return idx;
            }

        private:
            block_t* checkout_block(allocconfig_t const& alloccfg);
            struct item_t
            {
                u32   m_index;
                void* m_ptr;
            };
            item_t alloc_item(u32 alloc_size);
            void   dealloc_item(u32 item_index);
            void   dealloc_item_ptr(void* item_ptr);

            static const allocconfig_t c_aalloc_config[];
            static const blockconfig_t c_ablock_config[];
            static const u32           c_max_num_blocks;
            static const u32           c_max_num_sizes;

            static inline allocconfig_t const& alloc_size_to_alloc_config(u32 alloc_size)
            {
                alloc_size = (alloc_size + 7) & ~7;
                alloc_size = math::ceilpo2(alloc_size);
                s8 const c = math::countTrailingZeros(alloc_size) - 3;
                return c_aalloc_config[c];
            }

            nsuperheap::alloc_t* m_heap;
            void*                m_address_base;
            u64                  m_address_range;
            s8                   m_segment_size_shift;
            u32                  m_segments_array_size;
            segment_t*           m_segments;
            u32                  m_segments_free_index;
            binmap_t             m_segments_free_binmap;
            binmap_t*            m_active_segment_binmap_per_blockcfg;  // segments that still have available free blocks
            block_t**            m_active_block_list_per_allocsize;     // blocks that still have available free items, a list per allocation size
        };

        const blockconfig_t alloc_t::c_ablock_config[] = {
          // blockconfig-index, block-size-shift
          {0, 16},  //  64 * cKB
          {1, 18},  // 256 * cKB
          {2, 20},  //   1 * cMB
          {3, 22},  //   4 * cMB
        };

        const allocconfig_t alloc_t::c_aalloc_config[] = {
          // allocconfig-index, alloc-size-shift, block-index, block-size-shift (duplicate of the one in c_ablock_config[block_index])
          {0, 3, 0, 16},    // 0,         8
          {1, 4, 0, 16},    // 1,        16
          {2, 5, 0, 16},    // 2,        32
          {3, 6, 0, 16},    // 3,        64
          {4, 7, 0, 16},    // 4,       128
          {5, 8, 0, 16},    // 5,       256
          {6, 9, 0, 16},    // 6,       512
          {7, 10, 0, 16},   // 7,      1024
          {8, 11, 0, 16},   // 8,      2048
          {9, 12, 0, 16},   // 9,      4096
          {10, 13, 0, 16},  // 10,      8192
          {11, 14, 0, 16},  // 11,     16384
          {12, 15, 1, 18},  // 12,     32768
          {13, 16, 1, 18},  // 13,  64 * cKB
          {14, 17, 2, 20},  // 14, 128 * cKB
          {15, 18, 2, 20},  // 15, 256 * cKB
          {16, 19, 3, 22},  // 16, 512 * cKB
          {17, 20, 3, 22},  // 17,   1 * cMB
          {18, 21, 3, 22},  // 18,   2 * cMB
        };

        const u32 alloc_t::c_max_num_blocks = sizeof(c_ablock_config) / sizeof(blockconfig_t);
        const u32 alloc_t::c_max_num_sizes  = sizeof(c_aalloc_config) / sizeof(allocconfig_t);

        void alloc_t::initialize(nsuperheap::alloc_t* heap, u64 address_range, u32 segment_size)
        {
            m_heap                = heap;
            m_address_range       = address_range;
            m_segment_size_shift  = math::ilog2(segment_size);
            m_segments_array_size = (u32)(address_range >> m_segment_size_shift);
            m_segments            = (segment_t*)m_heap->calloc(sizeof(segment_t) * m_segments_array_size);

            m_segments_free_index = 0;
            u32 l0len, l1len, l2len, l3len;
            binmap_t::compute_levels(m_segments_array_size, l0len, l1len, l2len, l3len);
            u32* l1 = l1len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
            u32* l2 = l2len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
            u32* l3 = l3len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
            m_segments_free_binmap.init_lazy_1(m_segments_array_size, l0len, l1, l1len, l2, l2len, l3, l3len);
            m_active_segment_binmap_per_blockcfg = (binmap_t*)m_heap->calloc(sizeof(binmap_t) * c_max_num_blocks);

            u32 attributes = 0;
            u32 page_size  = 0;
            vmem_t::reserve(address_range, page_size, attributes, m_address_base);

            void* segment_address = m_address_base;
            for (u32 i = 0; i < m_segments_array_size; i++)
            {
                m_segments[i].initialize(segment_address, segment_size, i);
                segment_address = toaddress(segment_address, segment_size);
            }

            for (u32 i = 0; i < c_max_num_blocks; i++)
            {
                binmap_t& bm = m_active_segment_binmap_per_blockcfg[i];
                binmap_t::compute_levels(m_segments_array_size, l0len, l1len, l2len, l3len);
                l1 = l1len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
                l2 = l2len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
                l3 = l3len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
                bm.init_1(m_segments_array_size, l0len, l1, l1len, l2, l2len, l3, l3len);
            }

            m_active_block_list_per_allocsize = (block_t**)m_heap->alloc(sizeof(block_t*) * c_max_num_sizes);
            for (u32 i = 0; i < c_max_num_sizes; i++)
                m_active_block_list_per_allocsize[i] = nullptr;
        }

        void alloc_t::deinitialize(nsuperheap::alloc_t* heap)
        {
            for (u32 i = 0; i < m_segments_free_index; i++)
                m_segments[i].deinitialize(heap);

            for (u32 i = 0; i < c_max_num_blocks; i++)
            {
                heap->dealloc(m_active_segment_binmap_per_blockcfg[i].m_l[0]);
                heap->dealloc(m_active_segment_binmap_per_blockcfg[i].m_l[1]);
                heap->dealloc(m_active_segment_binmap_per_blockcfg[i].m_l[2]);
            }
            heap->dealloc(m_active_segment_binmap_per_blockcfg);

            heap->dealloc(m_segments_free_binmap.m_l[0]);
            heap->dealloc(m_segments_free_binmap.m_l[1]);
            heap->dealloc(m_segments_free_binmap.m_l[2]);

            heap->dealloc(m_segments);

            vmem_t::release(m_address_base, m_address_range);
        }

        u32 alloc_t::sizeof_alloc(u32 alloc_size) const { return math::ceilpo2((alloc_size + (8 - 1)) & ~(8 - 1)); }

        block_t* alloc_t::checkout_block(allocconfig_t const& alloccfg)
        {
            s16 const            config_index = alloccfg.m_index;
            blockconfig_t const& blockcfg     = c_ablock_config[alloccfg.m_block_index];

            s32 segment_index = m_active_segment_binmap_per_blockcfg[alloccfg.m_block_index].find();
            if (segment_index < 0)
            {
                segment_index = m_segments_free_binmap.findandset();
                if (segment_index < 0)
                {
                    if (m_segments_free_index < m_segments_array_size)
                    {
                        segment_index = m_segments_free_index;
                        m_segments_free_binmap.lazy_init_1(m_segments_free_index);
                        m_segments_free_index++;
                    }
                    else
                    {
                        // panic
                    }
                }
                m_active_segment_binmap_per_blockcfg[alloccfg.m_block_index].clr(segment_index);

                segment_t* segment         = &m_segments[segment_index];
                void*      segment_address = toaddress(m_address_base, (s64)segment_index << m_segment_size_shift);
                segment->initialize(segment_address, 1 << m_segment_size_shift, segment_index);
                segment->checkout(m_heap, m_segment_size_shift, blockcfg);
            }

            ASSERT(segment_index >= 0 && segment_index < (s32)m_segments_array_size);
            segment_t* segment = &m_segments[segment_index];
            block_t*   block   = segment->checkout_block(alloccfg);
            if (segment->is_empty())
            {
                // Segment is empty, remove it from the active list for this config
                m_active_segment_binmap_per_blockcfg[alloccfg.m_block_index].set(segment_index);
            }
            return block;
        }

        alloc_t::item_t alloc_t::alloc_item(u32 alloc_size)
        {
            item_t alloc = {NIL, nullptr};

            // Determine the block size for this allocation size
            // See if we have a segment available for this block size
            // If we do not have a segment available, we need to claim a new segment
            allocconfig_t const& alloccfg = alloc_size_to_alloc_config(alloc_size);
            ASSERT(alloc_size <= ((u32)1 << alloccfg.m_alloc_size_shift));
            block_t* block = m_active_block_list_per_allocsize[alloccfg.m_index];
            if (block == nullptr)
            {
                block = checkout_block(alloccfg);
                if (block == nullptr)
                {
                    // panic
                    return alloc;
                }
                block->m_next                                       = block;
                block->m_prev                                       = block;
                m_active_block_list_per_allocsize[alloccfg.m_index] = block;
            }

            // Allocate item from active block, if it becomes full then remove it from the active list
            u32 const  segment_index = block->m_segment_index;
            segment_t* segment       = &m_segments[segment_index];

            alloc.m_ptr = block->allocate_item(segment->address_of_block(block->m_segment_block_index), alloc.m_index);
            if (block->is_full())
            {
                block_t*& head = m_active_block_list_per_allocsize[alloccfg.m_index];
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
            ASSERT(segment_index < m_segments_array_size && segment_index <= 0xFE);
            ASSERT(block->m_segment_block_index < m_segments[segment_index].m_block_max_count && block->m_segment_block_index <= 0xFE);
            return alloc;
        }

        void alloc_t::dealloc_item(u32 item_index)
        {
            u32 const segment_index = (item_index >> 24) & 0xFF;
            u32 const block_index   = (item_index >> 16) & 0xFF;
            u32 const elem_index    = item_index & 0xFFFF;
            ASSERT(segment_index < m_segments_array_size);
            segment_t* segment = &m_segments[segment_index];
            ASSERT(block_index < segment->m_block_max_count);
            block_t* block = &segment->m_block_array[block_index];
            ASSERT(elem_index < block->m_item_freepos);
            u32 const alloc_config_index = block->m_alloc_config.m_index;

            bool const block_was_full = block->is_full();
            block->deallocate_item(segment->m_address, elem_index);
            if (block_was_full)
            {
                // This block is available again, add it to the active list
                block_t*& head = m_active_block_list_per_allocsize[alloc_config_index];
                if (head == nullptr)
                {
                    head          = block;
                    block->m_next = block;
                    block->m_prev = block;
                }
                else
                {
                    block->m_next = head;
					block->m_prev = head->m_prev;
					head->m_prev->m_next = block;
					head->m_prev        = block;
                }
            }
        }

        void alloc_t::dealloc_item_ptr(void* item_ptr)
        {
            u32 const item_index = ptr2idx(item_ptr);
            dealloc_item(item_index);
        }
    }  // namespace nsuperfsa

    // SuperSpace manages an array of SuperSegment
    // Every SuperSegment has an array of Chunks
    //
    // Functionality:
    //   Allocate
    //    - Handling the request of a new chunk, either creating one or taking one from the cache
    //   Deallocate
    //    - Quickly finding the supersegment_t*, segment_t*, chunk_t* and superalloc_t* that belong to a 'void* ptr'
    //    - Collecting a now empty-chunk and either release or cache it
    //   Get/Set Assoc
    //    - Set an associated value for a pointer
    //    - Get the associated value for a pointer
    //
    //   Get chunk by index
    //   Get address of chunk
    //
    struct superspace_t
    {
        struct segment_t
        {
            u32           m_segment_index;
            u32           m_chunks_free_index;     // index of the first free chunk in the array
            u32*          m_chunks_array;          // The array of chunk iptr's
            binmap_t      m_chunks_free_binmap;    // pointer to a binmap_t marking free chunks
            binmap_t      m_chunks_cached_binmap;  // pointer to a binmap_t marking cached chunks
            u32           m_count_chunks_cached;   // number of chunks that can are cached
            u32           m_count_chunks_used;     // number of chunks that can are used
            u32           m_count_chunks_max;      // The maximum number of chunks that can be used in this segment
            chunkconfig_t m_chunk_config;          // chunk config
        };

        // 32 bytes (optimal)
        struct chunk_t : public llnode_t
        {
            u16 m_segment_index;          // The index of the segment in the superspace
            u16 m_bin_index;              // The index of the bin that this chunk belongs to
            u32 m_segment_chunk_index;    // The index of the chunk in the segment
            u16 m_elem_used_count;        // The number of elements used in this chunk
            u16 m_elem_free_index;        // The index of the first free chunk (used to quickly take a free element)
            u32 m_elem_free_binmap_iptr;  // The binmap marking free elements for this chunk
            u32 m_elem_tag_array_iptr;    // The index to an array which we use for set_tag/get_tag
            u32 m_physical_pages;         // The number of physical pages that this chunk has committed

            void clear()
            {
                m_next                  = llnode_t::NIL;
                m_prev                  = llnode_t::NIL;
                m_segment_index         = 0xffff;
                m_bin_index             = 0xffff;
                m_segment_chunk_index   = 0xffffffff;
                m_elem_used_count       = 0;
                m_elem_free_index       = 0;
                m_elem_free_binmap_iptr = nsuperfsa::NIL;
                m_elem_tag_array_iptr   = nsuperfsa::NIL;
                m_physical_pages        = 0;
            }
        };

        nsuperfsa::alloc_t*        m_fsa;
        void*                      m_address_base;
        u64                        m_address_range;
        s8                         m_page_size_shift;
        s8                         m_segment_shift;       // 1 << m_segment_shift = segment size
        u32                        m_segment_count;       // Space Address Range / Segment Size = Number of segments
        u32                        m_segment_free_index;  //
        segment_t*                 m_segment_array;       // Array of segments
        binmap_t*                  m_segment_free_binmap;
        binmap_t*                  m_segment_active_binmaps;  // This needs to be per chunk-size
        u32                        m_used_physical_pages;     // The number of pages that are currently committed
        superalloc_config_t const* m_config;

        superspace_t()
            : m_fsa(nullptr)
            , m_address_base(nullptr)
            , m_address_range(0)
            , m_page_size_shift(0)
            , m_segment_shift(0)
            , m_segment_count(0)
            , m_segment_free_index(0)
            , m_segment_array(nullptr)
            , m_segment_free_binmap(nullptr)
            , m_segment_active_binmaps(nullptr)
            , m_used_physical_pages(0)
            , m_config(nullptr)
        {
        }

        void initialize(u64 address_range, s8 segment_shift, nsuperheap::alloc_t* heap, nsuperfsa::alloc_t* fsa, superalloc_config_t const* config)
        {
            ASSERT(math::ispo2(address_range));

            m_fsa           = fsa;
            m_address_range = address_range;
            u32 const attrs = 0;
            u32 page_size = 0;
            vmem_t::reserve(address_range, page_size, attrs, m_address_base);
            m_page_size_shift     = math::ilog2(page_size);
            m_used_physical_pages = 0;
            m_segment_shift       = segment_shift;

            m_segment_count = (u32)(m_address_range >> segment_shift);
            m_segment_array = (segment_t*)heap->calloc(m_segment_count * sizeof(segment_t));

            m_config = config;

            u32 l0len, l1len, l2len, l3len;
            binmap_t::compute_levels(m_segment_count, l0len, l1len, l2len, l3len);
            u32* l3               = (l3len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
            u32* l2               = (l2len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
            u32* l1               = (l1len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
            m_segment_free_binmap = (binmap_t*)heap->alloc(sizeof(binmap_t));
            m_segment_free_binmap->init_1(m_segment_count, l0len, l1, l1len, l2, l2len, l3, l3len);

            m_segment_active_binmaps = (binmap_t*)heap->calloc(config->m_num_chunkconfigs * sizeof(binmap_t));
            for (u32 i = 0; i < config->m_num_chunkconfigs; i++)
            {
                binmap_t& bm = m_segment_active_binmaps[i];
                binmap_t::compute_levels(m_segment_count, l0len, l1len, l2len, l3len);
                u32* l3 = (l3len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
                u32* l2 = (l2len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
                u32* l1 = (l1len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
                bm.init_1(m_segment_count, l0len, l1, l1len, l2, l2len, l3, l3len);
            }
        }

        void deinitialize(nsuperheap::alloc_t* heap)
        {
            vmem_t::release(m_address_base, m_address_range);

            heap->dealloc(m_segment_array);

            heap->dealloc(m_segment_free_binmap->m_l[0]);
            heap->dealloc(m_segment_free_binmap->m_l[1]);
            heap->dealloc(m_segment_free_binmap->m_l[2]);
            heap->dealloc(m_segment_free_binmap);

            for (u32 i = 0; i < m_config->m_num_chunkconfigs; i++)
            {
                binmap_t& bm = m_segment_active_binmaps[i];
                heap->dealloc(bm.m_l[0]);
                heap->dealloc(bm.m_l[1]);
                heap->dealloc(bm.m_l[2]);
            }
            heap->dealloc(m_segment_active_binmaps);

            m_fsa                    = nullptr;
            m_address_base           = nullptr;
            m_address_range          = 0;
            m_page_size_shift        = 0;
            m_segment_shift          = 0;
            m_segment_count          = 0;
            m_segment_free_index     = 0;
            m_segment_array          = nullptr;
            m_segment_free_binmap    = nullptr;
            m_segment_active_binmaps = nullptr;
            m_used_physical_pages    = 0;
            m_config                 = nullptr;
        }

        u32 checkout_segment(u8 chunk_config_index)
        {
            s32 segment_index = m_segment_free_binmap->findandset();
            if (segment_index < 0)
            {
                segment_index = m_segment_free_index++;
            }
            ASSERT(m_segment_free_index < m_segment_count);
            segment_t* segment       = &m_segment_array[segment_index];
            segment->m_segment_index = segment_index;

            u32 const segment_chunk_count = (1 << (m_segment_shift - m_config->m_achunkconfigs[chunk_config_index].m_chunk_size_shift));
            segment->m_chunks_array       = (u32*)m_fsa->allocptr(sizeof(u32) * segment_chunk_count);

            // To avoid fully initializing the binmap's, we are using an index that marks the first free chunk.
            // We also use this index to lazily initialize each binmap. So each time we checkout a chunk
            // we progressively initialize both binmaps by looking at the lower 5 bits of the index, when 0 we
            // call lazy_init on each binmap.
            segment->m_chunks_free_index = 0;

            u32 l0len, l1len, l2len, l3len;
            binmap_t::compute_levels(segment_chunk_count, l0len, l1len, l2len, l3len);
            u32* l3 = (l3len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
            u32* l2 = (l2len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
            u32* l1 = (l1len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
            segment->m_chunks_cached_binmap.init_lazy_1(segment_chunk_count, l0len, l1, l1len, l2, l2len, l3, l3len);

            l3 = (l3len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
            l2 = (l2len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
            l1 = (l1len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
            segment->m_chunks_free_binmap.init_lazy_1(segment_chunk_count, l0len, l1, l1len, l2, l2len, l3, l3len);

            segment->m_count_chunks_cached = 0;
            segment->m_count_chunks_used   = 0;
            segment->m_count_chunks_max    = segment_chunk_count;
            segment->m_chunk_config        = m_config->m_achunkconfigs[chunk_config_index];

            return segment_index;
        }

        static inline u32 chunk_physical_pages(binconfig_t const& bin, s8 page_size_shift) { return (u32)((bin.m_alloc_size * bin.m_max_alloc_count) + (((u64)1 << page_size_shift) - 1)) >> page_size_shift; }

        static void activate_chunk(nsuperfsa::alloc_t* fsa, superspace_t::chunk_t* chunk, binconfig_t const& bin, u32 segment_index, u32 segment_chunk_index)
        {
            chunk->clear();
            chunk->m_bin_index           = bin.m_alloc_bin_index;
            chunk->m_segment_index       = segment_index;
            chunk->m_segment_chunk_index = segment_chunk_index;

            // Allocate allocation tag array
            chunk->m_elem_tag_array_iptr = fsa->alloc(sizeof(u32) * bin.m_max_alloc_count);

            u32 l0len, l1len, l2len, l3len;
            binmap_t::compute_levels(bin.m_max_alloc_count, l0len, l1len, l2len, l3len);

            u32* l3 = (l3len > 0) ? (u32*)fsa->allocptr(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
            u32* l2 = (l2len > 0) ? (u32*)fsa->allocptr(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
            u32* l1 = (l1len > 0) ? (u32*)fsa->allocptr(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;

            chunk->m_elem_free_binmap_iptr = fsa->alloc(sizeof(binmap_t));
            binmap_t* bm                   = fsa->idx2obj<binmap_t>(chunk->m_elem_free_binmap_iptr);
            bm->init_lazy_1(bin.m_max_alloc_count, l0len, l1, l1len, l2, l2len, l3, l3len);
        }

        chunk_t* checkout_chunk(binconfig_t const& bin)
        {
            // Get the chunk info index
            u32 const chunk_info_index = bin.m_chunk_config.m_chunk_info_index;

            s32 segment_index = m_segment_active_binmaps[chunk_info_index].findandset();
            if (segment_index < 0)
            {
                segment_index = checkout_segment(chunk_info_index);
                m_segment_active_binmaps[chunk_info_index].clr(segment_index);
            }
            ASSERT(segment_index < (s32)m_segment_count);

            u32 const required_physical_pages = chunk_physical_pages(bin, m_page_size_shift);

            // Here we have a segment where we can get a chunk from.
            // Remember to also lazily initialize the free and cached chunk binmaps.
            segment_t* segment                 = &m_segment_array[segment_index];
            chunk_t*   chunk                   = nullptr;
            u32        already_committed_pages = 0;
            s32        segment_chunk_index;

            if (segment->m_count_chunks_cached > 0)
            {
                segment->m_count_chunks_cached -= 1;
                segment_chunk_index     = segment->m_chunks_cached_binmap.findandset();
                ASSERT(segment_chunk_index >= 0 && segment_chunk_index < (s32)segment->m_chunks_free_index);
                u32 const chunk_iptr    = segment->m_chunks_array[segment_chunk_index];
                chunk                   = (chunk_t*)m_fsa->idx2ptr(chunk_iptr);
                already_committed_pages = chunk->m_physical_pages;
            }
            else
            {
                // Use segment->m_chunks_free_index to take the next free chunk index.
                segment_chunk_index = segment->m_chunks_free_binmap.find();
                if (segment_chunk_index < 0)
                {
                    segment_chunk_index = segment->m_chunks_free_index++;
                    if ((segment_chunk_index & 0x1F) == 0)
                    {  // If the lower 5 bits are 0, we need to clear a branch in the free and cached binmaps.
                        segment->m_chunks_cached_binmap.lazy_init_1(segment_chunk_index);
                        segment->m_chunks_free_binmap.lazy_init_1(segment_chunk_index);
                    }
                }
                u32 const chunk_iptr                         = m_fsa->alloc(sizeof(chunk_t));
                chunk                                        = (chunk_t*)m_fsa->idx2ptr(chunk_iptr);
                segment->m_chunks_array[segment_chunk_index] = chunk_iptr;
            }

            activate_chunk(m_fsa, chunk, bin, segment_index, segment_chunk_index);
            segment->m_chunks_free_binmap.set(segment_chunk_index);

            if (required_physical_pages < already_committed_pages)
            {
                // Overcommitted, uncommit tail pages
                void* address = chunk_to_address(segment, chunk);
                address       = toaddress(address, required_physical_pages << m_page_size_shift);
                vmem_t::decommit(address, 1 << m_page_size_shift, (already_committed_pages - required_physical_pages));
                chunk->m_physical_pages = required_physical_pages;
                m_used_physical_pages -= (already_committed_pages - required_physical_pages);
            }
            else if (required_physical_pages > already_committed_pages)
            {
                // Undercommitted, commit necessary tail pages
                void* address = chunk_to_address(segment, chunk);
                address       = toaddress(address, already_committed_pages << m_page_size_shift);
                vmem_t::commit(address, 1 << m_page_size_shift, (required_physical_pages - already_committed_pages));
                chunk->m_physical_pages = required_physical_pages;
                m_used_physical_pages += (required_physical_pages - already_committed_pages);
            }

            segment->m_count_chunks_used += 1;
            if (segment->m_count_chunks_used == segment->m_count_chunks_max)
            {
                // Segment is full, we need to remove it from the list of active segments
                m_segment_active_binmaps[chunk_info_index].set(segment_index);
            }

            return chunk;
        }

        void release_segment(segment_t* segment)
        {
            ASSERT(segment->m_count_chunks_used == 0);

            m_segment_active_binmaps[segment->m_chunk_config.m_chunk_info_index].set(segment->m_segment_index);

            // Maybe every size should cache at least one segment otherwise single alloc/dealloc calls will
            // checkout and release a segment every time?

            // Release all cached chunks in this segment
            while (segment->m_count_chunks_cached > 0)
            {
                s32 const segment_chunk_index = segment->m_chunks_cached_binmap.findandset();
                u32 const chunk_iptr          = segment->m_chunks_array[segment_chunk_index];
                chunk_t*  chunk               = m_fsa->idx2obj<chunk_t>(chunk_iptr);
                vmem_t::decommit(chunk_to_address(segment, chunk), 1 << m_page_size_shift, chunk->m_physical_pages);
                deinitialize_chunk(m_fsa, chunk, m_config->m_abinconfigs[chunk->m_bin_index]);
                m_fsa->deallocptr(chunk);
                segment->m_chunks_array[segment_chunk_index] = nsuperfsa::NIL;
                segment->m_count_chunks_cached -= 1;
                segment->m_chunks_free_binmap.clr(segment_chunk_index);
            }

            m_fsa->deallocptr(segment->m_chunks_array);
            segment->m_chunks_array = nullptr;

            binmap_t* bm = (binmap_t*)&segment->m_chunks_cached_binmap;
            for (u32 i = 0; i < bm->num_levels(); i++)
            {
                m_fsa->deallocptr(bm->m_l[i]);
                bm->m_l[i] = nullptr;
            }

            bm = (binmap_t*)&segment->m_chunks_free_binmap;
            for (u32 i = 0; i < bm->num_levels(); i++)
            {
                m_fsa->deallocptr(bm->m_l[i]);
                bm->m_l[i] = nullptr;
            }

            m_segment_free_binmap->clr(segment->m_segment_index);
        }

        void deinitialize_chunk(nsuperfsa::alloc_t* fsa, superspace_t::chunk_t* chunk, binconfig_t const& bin)
        {
            fsa->dealloc(chunk->m_elem_tag_array_iptr);
            chunk->m_elem_tag_array_iptr = nsuperfsa::NIL;

            binmap_t* bm = fsa->idx2obj<binmap_t>(chunk->m_elem_free_binmap_iptr);
            for (u32 i = 0; i < bm->num_levels(); i++)
            {
                m_fsa->deallocptr(bm->m_l[i]);
                bm->m_l[i] = nullptr;
            }
            fsa->dealloc(chunk->m_elem_free_binmap_iptr);
            chunk->m_elem_free_binmap_iptr = nsuperfsa::NIL;
        }

        void release_chunk(chunk_t* chunk)
        {
            // See if this segment was full, if so we need to add it back to the list of active segments again so that
            // we can checkout chunks from it again.
            segment_t* segment = &m_segment_array[chunk->m_segment_index];
            if (segment->m_count_chunks_used == segment->m_count_chunks_max)
            {
                u32 const chunk_info_index = segment->m_chunk_config.m_chunk_info_index;
                m_segment_active_binmaps[chunk_info_index].clr(chunk->m_segment_index);
            }

            // TODO: Caching of chunks
            // segment->m_chunks_cached_binmap.clr(chunk->m_segment_chunk_index);
            // segment->m_count_chunks_used   -= 1;
            // segment->m_count_chunks_cached += 1;

            // Uncommit the virtual memory of this chunk
            vmem_t::decommit(chunk_to_address(segment, chunk), 1 << m_page_size_shift, chunk->m_physical_pages);
            m_used_physical_pages -= chunk->m_physical_pages;

            // Release the tracking array that was allocated for this chunk
            deinitialize_chunk(m_fsa, chunk, m_config->m_abinconfigs[chunk->m_bin_index]);

            segment->m_chunks_free_binmap.clr(segment->m_segment_index);
            segment->m_chunks_array[chunk->m_segment_chunk_index] = nsuperfsa::NIL;

            // Deallocate and unregister the chunk
            m_fsa->deallocptr(chunk);
            chunk = nullptr;

            segment->m_count_chunks_used -= 1;
            if (segment->m_count_chunks_used == 0)
            {
                // This segment is now empty, we need to release it
                release_segment(segment);
                segment = nullptr;
            }
        }

        void set_tag(void* ptr, u32 assoc, u32 num_superbins, binconfig_t const* superbins)
        {
            ASSERT(ptr >= m_address_base && ptr < ((u8*)m_address_base + m_address_range));
            chunk_t*  chunk     = address_to_chunk(ptr);
            u32 const sbinindex = chunk->m_bin_index;
            ASSERT(sbinindex < num_superbins);
            binconfig_t const& bin = superbins[sbinindex];

            segment_t* segment        = &m_segment_array[chunk->m_segment_index];
            u32*       elem_tag_array = (u32*)m_fsa->idx2ptr(chunk->m_elem_tag_array_iptr);

            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            void* const chunk_address        = chunk_to_address(segment, chunk);
            u32 const   chunk_item_index     = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            elem_tag_array[chunk_item_index] = assoc;
        }

        u32 get_tag(void* ptr, u32 num_superbins, binconfig_t const* superbins) const
        {
            ASSERT(ptr >= m_address_base && ptr < ((u8*)m_address_base + m_address_range));
            chunk_t*  chunk     = address_to_chunk(ptr);
            u32 const sbinindex = chunk->m_bin_index;
            ASSERT(sbinindex < num_superbins);
            binconfig_t const& bin = superbins[sbinindex];

            segment_t* segment        = &m_segment_array[chunk->m_segment_index];
            u32* const elem_tag_array = (u32*)m_fsa->idx2ptr(chunk->m_elem_tag_array_iptr);

            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            void* const chunk_address       = chunk_to_address(segment, chunk);
            u32 const   chunk_element_index = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            return elem_tag_array[chunk_element_index];
        }

        inline chunk_t* address_to_chunk(void* ptr) const
        {
            u32 const  segment_index       = (u32)(todistance(m_address_base, ptr) >> m_segment_shift);
            segment_t* segment             = &m_segment_array[segment_index];
            void*      segment_address     = toaddress(m_address_base, ((u64)segment_index << m_segment_shift));
            u32 const  segment_chunk_index = (u32)(todistance(segment_address, ptr) >> (segment->m_chunk_config.m_chunk_size_shift));
            u32 const  chunk_iptr          = segment->m_chunks_array[segment_chunk_index];
            return (chunk_t*)m_fsa->idx2ptr(chunk_iptr);
        }

        inline void* chunk_to_address(segment_t const* segment, chunk_t const* chunk) const
        {
            ASSERT(chunk->m_segment_index == segment->m_segment_index);
            ASSERT(segment->m_segment_index < m_segment_free_index);
            u64 const segment_offset = ((u64)segment->m_segment_index << m_segment_shift);
            return toaddress(m_address_base, segment_offset + ((u64)chunk->m_segment_chunk_index << segment->m_chunk_config.m_chunk_size_shift));
        }
    };

    class superalloc_t : public valloc_t
    {
    public:
        superalloc_config_t const* m_config;
        nsuperheap::alloc_t        m_internal_heap;
        nsuperfsa::alloc_t         m_internal_fsa;
        superspace_t*              m_superspace;
        lldata_t                   m_chunk_list_data;
        llhead_t*                  m_active_chunk_list_per_alloc_size;
        alloc_t*                   m_main_allocator;

        superalloc_t(alloc_t* main_allocator)
            : m_config(nullptr)
            , m_internal_heap()
            , m_internal_fsa()
            , m_superspace(nullptr)
            , m_chunk_list_data()
            , m_active_chunk_list_per_alloc_size(nullptr)
            , m_main_allocator(main_allocator)
        {
        }

        DCORE_CLASS_PLACEMENT_NEW_DELETE

        void initialize(superalloc_config_t const* config);
        void deinitialize();

        void* v_allocate(u32 size, u32 alignment);
        u32   v_deallocate(void* ptr);
        void  v_release() {}
        u32   v_get_size(void* ptr) const;
        void  v_set_tag(void* ptr, u32 assoc);
        u32   v_get_tag(void* ptr) const;
    };

    void superalloc_t::initialize(superalloc_config_t const* config)
    {
        m_config = config;

        m_internal_heap.initialize(config->m_internal_heap_address_range, config->m_internal_heap_pre_size);
        m_internal_fsa.initialize(&m_internal_heap, config->m_internal_fsa_address_range, config->m_internal_fsa_segment_size);

        m_superspace = (superspace_t*)m_internal_heap.calloc(sizeof(superspace_t));
        m_superspace->initialize(config->m_total_address_size, config->m_segment_address_range_shift, &m_internal_heap, &m_internal_fsa, config);

        m_chunk_list_data                  = {&m_internal_fsa};
        m_active_chunk_list_per_alloc_size = (llhead_t*)m_internal_heap.alloc(config->m_num_binconfigs * sizeof(llhead_t));
        for (u32 i = 0; i < config->m_num_binconfigs; i++)
            m_active_chunk_list_per_alloc_size[i].reset();
    }

    void superalloc_t::deinitialize()
    {
        m_config = nullptr;
        m_internal_fsa.deinitialize(&m_internal_heap);
        m_internal_heap.deinitialize();
        m_superspace     = nullptr;
        m_main_allocator = nullptr;
    }

    void* superalloc_t::v_allocate(u32 alloc_size, u32 alignment)
    {
        alloc_size             = math::alignUp(alloc_size, alignment);
        binconfig_t const& bin = m_config->size2bin(alloc_size);

        superspace_t::chunk_t* chunk      = nullptr;
        u32                    chunk_iptr = m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].m_index;
        if (chunk_iptr == nsuperfsa::NIL)
        {
            chunk      = m_superspace->checkout_chunk(bin);
            chunk_iptr = m_internal_fsa.ptr2idx(chunk);
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].insert(m_chunk_list_data, chunk_iptr);
        }
        else
        {
            chunk = m_internal_fsa.idx2obj<superspace_t::chunk_t>(chunk_iptr);
        }

        ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);

        superspace_t::segment_t* segment = &m_superspace->m_segment_array[chunk->m_segment_index];
        void*                    ptr     = m_superspace->chunk_to_address(segment, chunk);

        // If we have elements in the binmap, we can use it to get a free element.
        // If not, we need to use free_index as the free element.
        u32 elem_index;
        if (chunk->m_elem_used_count == chunk->m_elem_free_index && chunk->m_elem_free_index < bin.m_max_alloc_count)
        {  // seems all elements are used, lazy initialize binmap
            if ((chunk->m_elem_free_index & 0x1F) == 0)
            {
                binmap_t* bm = m_internal_fsa.idx2obj<binmap_t>(chunk->m_elem_free_binmap_iptr);
                bm->lazy_init_1(chunk->m_elem_free_index);
            }
            elem_index = chunk->m_elem_free_index++;
        }
        else
        {
            binmap_t* bm = m_internal_fsa.idx2obj<binmap_t>(chunk->m_elem_free_binmap_iptr);
            elem_index   = bm->findandset();
        }
        ASSERT(elem_index < bin.m_max_alloc_count);
        ptr = toaddress(ptr, (u64)elem_index * bin.m_alloc_size);

        // Initialize the tag value for this element
        u32* elem_tag_array        = (u32*)m_internal_fsa.idx2ptr(chunk->m_elem_tag_array_iptr);
        elem_tag_array[elem_index] = 0;

        chunk->m_elem_used_count += 1;
        if (bin.m_max_alloc_count == chunk->m_elem_used_count)
        {
            // Chunk is full, so remove it from the list of active chunks
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].remove_item(m_chunk_list_data, chunk_iptr);
        }

        ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
        return ptr;
    }

    u32 superalloc_t::v_deallocate(void* ptr)
    {
        if (ptr == nullptr)
            return 0;

        ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
        superspace_t::chunk_t* chunk = m_superspace->address_to_chunk(ptr);
        binconfig_t const&     bin   = m_config->m_abinconfigs[chunk->m_bin_index];

        u32 alloc_size;
        {
            superspace_t::segment_t* segment       = &m_superspace->m_segment_array[chunk->m_segment_index];
            void* const              chunk_address = m_superspace->chunk_to_address(segment, chunk);
            u32 const                elem_index    = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            ASSERT(elem_index < bin.m_max_alloc_count);
            binmap_t* bm = m_internal_fsa.idx2obj<binmap_t>(chunk->m_elem_free_binmap_iptr);
            bm->clr(elem_index);
            u32* elem_tag_array = (u32*)m_internal_fsa.idx2ptr(chunk->m_elem_tag_array_iptr);
            ASSERT(elem_tag_array[elem_index] != 0xFEFEEFEE);  // Double freeing this element ?
            elem_tag_array[elem_index] = 0xFEFEEFEE;           // Clear the tag for this element (mark it as freed)
            alloc_size                 = bin.m_alloc_size;
        }

        const u32  chunk_iptr     = m_internal_fsa.ptr2idx(chunk);
        const bool chunk_was_full = (bin.m_max_alloc_count == chunk->m_elem_used_count);

        chunk->m_elem_used_count -= 1;
        if (0 == chunk->m_elem_used_count)
        {
            if (!chunk_was_full)
            {
                // We are going to release this chunk, so remove it from the active chunk list before doing that.
                m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].remove_item(m_chunk_list_data, chunk_iptr);
            }
            m_superspace->release_chunk(chunk);
        }
        else if (chunk_was_full)
        {
            // Ok, this chunk can be used to allocate from again, so add it to the list of active chunks
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].insert(m_chunk_list_data, chunk_iptr);
        }

        ASSERT(alloc_size <= bin.m_alloc_size);
        return alloc_size;
    }

    u32 superalloc_t::v_get_size(void* ptr) const
    {
        if (ptr == nullptr)
            return 0;
        ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
        superspace_t::chunk_t* chunk = m_superspace->address_to_chunk(ptr);
        binconfig_t const&     bin   = m_config->m_abinconfigs[chunk->m_bin_index];
        return bin.m_alloc_size;
    }

    void superalloc_t::v_set_tag(void* ptr, u32 assoc)
    {
        if (ptr != nullptr)
            m_superspace->set_tag(ptr, assoc, m_config->m_num_binconfigs, m_config->m_abinconfigs);
    }

    u32 superalloc_t::v_get_tag(void* ptr) const
    {
        if (ptr == nullptr)
            return 0xffffffff;
        return m_superspace->get_tag(ptr, m_config->m_num_binconfigs, m_config->m_abinconfigs);
    }

    valloc_t* gCreateVmAllocator(alloc_t* main_heap)
    {
        // superalloc_config_t const* config = gGetSuperAllocConfigWindowsDesktopApp10p();
        superalloc_config_t const* config = gGetSuperAllocConfigWindowsDesktopApp25p();

        superalloc_t* superalloc = main_heap->construct<superalloc_t>(main_heap);
        superalloc->initialize(config);
        return superalloc;
    }

    void gDestroyVmAllocator(valloc_t* valloc)
    {
        superalloc_t* superalloc     = static_cast<superalloc_t*>(valloc);
        alloc_t*      main_allocator = superalloc->m_main_allocator;
        superalloc->deinitialize();
        main_allocator->destruct(superalloc);
    }

}  // namespace ncore
