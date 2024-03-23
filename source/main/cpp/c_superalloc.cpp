#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "cbase/c_integer.h"

#include "csuperalloc/private/c_doubly_linked_list.h"
#include "csuperalloc/private/c_binmap.h"
#include "csuperalloc/c_superalloc.h"

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

    // Chunk Config
    // NOTE: This info should be moved and part of the initialization of a superallocator_config_t
    struct chunkconfig_t
    {
        u8 m_chunk_size_shift;  // The shift of the chunk size (e.g. 12 for 4KB)
        u8 m_chunk_info_index;  // The index of the chunk size that this bin requires
    };
    static const chunkconfig_t c4KB              = {12, 0};
    static const chunkconfig_t c16KB             = {14, 1};
    static const chunkconfig_t c32KB             = {15, 2};
    static const chunkconfig_t c64KB             = {16, 3};
    static const chunkconfig_t c128KB            = {17, 4};
    static const chunkconfig_t c256KB            = {18, 5};
    static const chunkconfig_t c512KB            = {19, 6};
    static const chunkconfig_t c1MB              = {20, 7};
    static const chunkconfig_t c2MB              = {21, 8};
    static const chunkconfig_t c4MB              = {22, 9};
    static const chunkconfig_t c8MB              = {23, 10};
    static const chunkconfig_t c16MB             = {24, 11};
    static const chunkconfig_t c32MB             = {25, 12};
    static const chunkconfig_t c64MB             = {26, 13};
    static const chunkconfig_t c128MB            = {27, 14};
    static const chunkconfig_t c256MB            = {28, 15};
    static const chunkconfig_t c512MB            = {29, 16};
    static const chunkconfig_t cChunkInfoArray[] = {c4KB, c16KB, c32KB, c64KB, c128KB, c256KB, c512KB, c1MB, c2MB, c4MB, c8MB, c16MB, c32MB, c64MB, c128MB, c256MB, c512MB};
    static const u32           cNumChunkConfigs  = sizeof(cChunkInfoArray) / sizeof(chunkconfig_t);

    // Can only allocate, used internally to allocate initially required memory
    namespace nsuperheap
    {
        class alloc_t
        {
        public:
            void*   m_address;
            u64     m_address_range;
            vmem_t* m_vmem;
            u32     m_allocsize_alignment;
            u32     m_page_size;
            u32     m_page_size_shift;
            u32     m_page_count_current;
            u32     m_page_count_maximum;
            u64     m_ptr;

            void initialize(vmem_t* vmem, u64 memory_range, u64 size_to_pre_allocate);
            void deinitialize();

            void* alloc(u32 size);   // allocate
            void* calloc(u32 size);  // allocate and clear
            void  dealloc(void* ptr);
        };

        void alloc_t::initialize(vmem_t* vmem, u64 memory_range, u64 size_to_pre_allocate)
        {
            u32 attributes  = 0;
            m_vmem          = vmem;
            m_address_range = memory_range;
            m_vmem->reserve(memory_range, m_page_size, attributes, m_address);
            m_page_size_shift     = math::ilog2(m_page_size);
            m_allocsize_alignment = 32;
            m_page_count_maximum  = (u32)(memory_range / m_page_size);
            m_page_count_current  = 0;
            m_ptr                 = 0;

            if (size_to_pre_allocate > 0)
            {
                u32 const pages_to_commit = (u32)(math::alignUp(size_to_pre_allocate, (u64)m_page_size) >> m_page_size_shift);
                m_vmem->commit(m_address, m_page_size, pages_to_commit);
                m_page_count_current = pages_to_commit;
            }
        }

        void alloc_t::deinitialize()
        {
            m_vmem->release(m_address, m_address_range);
            m_address             = nullptr;
            m_address_range       = 0;
            m_vmem                = nullptr;
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
                m_vmem->commit(toaddress(m_address, commit_base), m_page_size, page_count_to_commit);
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
            static const u16 NIL = 0xffff;

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
                m_item_freelist       = NIL;
                m_alloc_config        = alloccfg;
            }

            inline bool  is_full() const { return m_item_count == m_item_count_max; }
            inline bool  is_empty() const { return m_item_count == 0; }
            inline u32   ptr2idx(void const* const ptr, void const* const elem) const { return (u32)(((u64)elem - (u64)ptr) >> m_alloc_config.m_alloc_size_shift); }
            inline void* idx2ptr(void* const ptr, u32 const index) const { return toaddress(ptr, ((u64)index << m_alloc_config.m_alloc_size_shift)); }

            void* allocate_item(void* block_address, u32& item_index)
            {
                item_index = NIL;
                u16* item  = nullptr;
                if (m_item_freelist != NIL)
                {
                    item_index      = m_item_freelist;
                    item            = (u16*)idx2ptr(block_address, item_index);
                    m_item_freelist = item[0];
                }
                else if (m_item_freepos < m_item_count_max)
                {
                    item_index = m_item_freepos;
                    item       = (u16*)idx2ptr(block_address, item_index);
                    m_item_freepos++;
                }
                else
                {
                    return item;  // panic
                }

                m_item_count++;
#ifdef SUPERALLOC_DEBUG
                nmem::memset(item, 0xCDCDCDCD, ((u64)1 << m_alloc_config.m_alloc_size_shift));
#endif
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
            vmem_t*       m_vmem;
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
            void     checkout(nsuperheap::alloc_t* heap, vmem_t* vmem, s8 segment_size_shift, blockconfig_t const& blockcfg);
            block_t* checkout_block(allocconfig_t const& alloccfg);
            void     release_block(block_t* block);
            void*    address_of_block(u32 block) const { return toaddress(m_address, block << m_block_config.m_block_size_shift); }
            bool     is_empty() const { return m_block_used_count == m_block_max_count; }

            block_t* get_block_from_address(void const* ptr) const
            {
                ASSERT(ptr >= m_address && ptr < toaddress(m_address, m_address_range));
                return &m_block_array[(u32)(todistance(m_address, ptr) >> m_block_config.m_block_size_shift)];
            }

            inline void* idx2ptr(u32 i) const
            {
                if (i == 0xffffffff)
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
                    return 0xffffffff;
                u32 const            block_index   = (u32)(todistance(m_address, ptr) >> m_block_config.m_block_size_shift);
                block_t const* const block         = &m_block_array[block_index];
                void* const          block_address = address_of_block(block_index);
                u32 const            elem_index    = block->ptr2idx(block_address, ptr);
                return (block_index << 16) | (elem_index & 0xFFFF);
            }
        };

        void segment_t::initialize(void* address, u32 address_range, u8 index)
        {
            m_vmem              = nullptr;
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

        void segment_t::checkout(nsuperheap::alloc_t* heap, vmem_t* vmem, s8 segment_size_shift, blockconfig_t const& blockcfg)
        {
            m_vmem             = vmem;
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

            m_vmem->commit(m_address, (u32)1 << m_block_config.m_block_size_shift, m_block_max_count);
            m_committed = true;
        }

        void segment_t::deinitialize(nsuperheap::alloc_t* heap)
        {
            heap->dealloc(m_block_free_binmap.m_l[0]);
            heap->dealloc(m_block_free_binmap.m_l[1]);
            heap->dealloc(m_block_free_binmap.m_l[2]);
            heap->dealloc(m_block_array);
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
                }
            }

#ifdef SUPERALLOC_DEBUG
            nmem::memset(address_of_block(free_block_index), 0xCDCDCDCD, 1 << m_block_config.m_block_size_shift);
#endif
            m_block_used_count++;
            block_t* block = &m_block_array[free_block_index];
            block->initialize(m_segment_index, free_block_index, alloccfg);
            return block;
        }

        void segment_t::release_block(block_t* block)
        {
#ifdef SUPERALLOC_DEBUG
            nmem::memset(address_of_block(block->m_segment_block_index), 0xFEFEFEFE, sizeof(block_t));
#endif
            m_block_used_count--;
            m_block_free_binmap.clr(block->m_segment_block_index);
        }

        // @note: The format of the returned index is u32[u8(segment-index):u24(item-index)]
        class alloc_t
        {
        public:
            static const u32 NIL = 0xffffffff;

            void         initialize(nsuperheap::alloc_t* heap, vmem_t* vmem, u64 address_range, u32 segment_size);
            void         deinitialize(nsuperheap::alloc_t* heap);
            u32          sizeof_alloc(u32 size) const;
            inline void* base_address() const { return m_address_base; }
            inline u32   alloc(u32 size) { return alloc_item(size).m_index; }
            inline void  dealloc(u32 index) { dealloc_item(index); }
            inline void* allocptr(u32 size) { return alloc_item(size).m_ptr; }
            inline void  deallocptr(void* ptr) { dealloc_item_ptr(ptr); }

            inline void* idx2ptr(u32 i) const
            {
                if (i == 0xffffffff)
                    return nullptr;
                u32 const c = (i >> 24) & 0xFF;
                return m_segments[c].idx2ptr(i);
            }

            inline u32 ptr2idx(void const* ptr) const
            {
                if (ptr == nullptr)
                    return 0xffffffff;
                u32 const segment_index = (u32)(todistance(m_address_base, ptr) >> m_segment_size_shift);
                ASSERT(segment_index < m_segments_array_size);
                u32 const idx = ((segment_index & 0xFF) << 24) | m_segments[segment_index].ptr2idx(ptr);
                return idx;
            }

            template <typename T>
            inline T* idx2ptr(u32 i) const
            {
                if (i == 0xffffffff)
                    return nullptr;
                u32 const segment_index = (i >> 24) & 0xFF;
                return (T*)m_segments[segment_index].idx2ptr(i);
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
            vmem_t*              m_vmem;
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

        void alloc_t::initialize(nsuperheap::alloc_t* heap, vmem_t* vmem, u64 address_range, u32 segment_size)
        {
            m_heap                = heap;
            m_vmem                = vmem;
            m_address_range       = address_range;
            m_segment_size_shift  = math::ilog2(segment_size);
            m_segments_array_size = address_range / segment_size;
            m_segments            = (segment_t*)m_heap->calloc(sizeof(segment_t) * m_segments_array_size);

            m_segments_free_index = 0;
            u32 l0len, l1len, l2len, l3len;
            binmap_t::compute_levels(m_segments_array_size, l0len, l1len, l2len, l3len);
            u32* l1 = l1len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
            u32* l2 = l2len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
            u32* l3 = l3len > 0 ? (u32*)m_heap->alloc(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
            m_segments_free_binmap.init_lazy_0(m_segments_array_size, l0len, l1, l1len, l2, l2len, l3, l3len);
            m_active_segment_binmap_per_blockcfg = (binmap_t*)m_heap->calloc(sizeof(binmap_t) * c_max_num_blocks);

            u32 attributes = 0;
            u32 page_size  = 0;
            m_vmem->reserve(address_range, page_size, attributes, m_address_base);

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

            m_active_block_list_per_allocsize = (block_t**)m_heap->calloc(sizeof(block_t*) * c_max_num_sizes);
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
                        m_segments_free_binmap.lazy_init_0(m_segments_free_index);
                        m_segments_free_index++;
                    }
                    else
                    {
                        // panic
                    }
                }
                m_active_segment_binmap_per_blockcfg[alloccfg.m_block_index].clr(segment_index);

                segment_t* segment         = &m_segments[segment_index];
                void*      segment_address = toaddress(m_address_base, segment_index << m_segment_size_shift);
                segment->initialize(segment_address, 1 << m_segment_size_shift, segment_index);
                segment->checkout(m_heap, m_vmem, m_segment_size_shift, blockcfg);
            }

            ASSERT(segment_index >= 0 && segment_index < m_segments_array_size);
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
            ASSERT(alloc_size <= (1 << alloccfg.m_alloc_size_shift));
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
            if (block->is_empty())
            {
                block_t*& head = m_active_block_list_per_allocsize[alloccfg.m_index];
                if (head == block)
                    head = block->m_next;
                block->m_prev->m_next = block->m_next;
                block->m_next->m_prev = block->m_prev;
            }

            // The allocation index also needs to encode the segment and block indices
            ASSERT(segment_index < m_segments_array_size && segment_index <= 0xFF);
            ASSERT(block->m_segment_block_index < m_segments[segment_index].m_block_max_count && block->m_segment_block_index <= 0xFF);
            alloc.m_index |= ((segment_index & 0xFF) << 24) | ((block->m_segment_block_index & 0xFF) << 16);
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
            block->deallocate_item(segment->m_address, elem_index & 0xFFFF);
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
                    head->m_prev  = block;
                    head          = block;
                }
            }
        }

        void alloc_t::dealloc_item_ptr(void* item_ptr)
        {
            ASSERT(item_ptr >= m_address_base && item_ptr < toaddress(m_address_base, m_address_range));
            u32 const  segment_index = (u32)(todistance(m_address_base, item_ptr) >> m_segment_size_shift);
            segment_t* segment       = &m_segments[segment_index];
            block_t*   block         = segment->get_block_from_address(item_ptr);
            u32 const  elem_index    = block->ptr2idx(segment->m_address, item_ptr);
            u32 const  item_index    = (segment_index << 24) | (block->m_segment_block_index << 16) | elem_index;
            dealloc_item(item_index);
        }
    }  // namespace nsuperfsa

    struct superbin_t
    {
        inline superbin_t(u8 binidx, u32 allocsize, chunkconfig_t chunkconfig)
            : m_alloc_size(allocsize)
            , m_chunk_config(chunkconfig)
            , m_alloc_bin_index(binidx)
        {
            u32 const chunkshift = chunkconfig.m_chunk_size_shift;
            u32 const chunk_size = ((u32)1 << chunkshift);
            m_max_alloc_count    = (u16)(chunk_size / allocsize);
        }

        inline u16  binmap_l1len() const { return (u16)(m_max_alloc_count >> 8); }
        inline u16  binmap_l2len() const { return (u16)(m_max_alloc_count >> 4); }
        inline bool use_binmap() const { return m_max_alloc_count == 1 && binmap_l1len() == 0 && binmap_l2len() == 0 ? 0 : 1; }

        u32           m_alloc_size;       // The size of the allocation that this bin is managing
        u32           m_max_alloc_count;  // The maximum number of allocations that can be made from a single chunk
        chunkconfig_t m_chunk_config;     // The index of the chunk size that this bin requires
        u8            m_alloc_bin_index;  // The index of this bin, also used as an indirection (only one indirection allowed)
    };

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
            u32*          m_chunks_array;          // The array of chunk iptr's
            u32           m_chunks_free_index;     // index of the first free chunk in the array
            binmap_t      m_chunks_free_binmap;    // pointer to a binmap_t marking free chunks
            binmap_t      m_chunks_cached_binmap;  // pointer to a binmap_t marking cached chunks
            u32           m_count_chunks_cached;   // number of chunks that can are cached
            u32           m_count_chunks_free;     // number of chunks that can are free
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
                m_elem_free_binmap_iptr = nsuperfsa::alloc_t::NIL;
                m_elem_tag_array_iptr   = nsuperfsa::alloc_t::NIL;
                m_physical_pages        = 0;
            }
        };

        nsuperfsa::alloc_t*  m_fsa;
        vmem_t*              m_vmem;
        void*                m_address_base;
        u64                  m_address_range;
        u32                  m_page_size;
        s8                   m_page_shift;
        s8                   m_segment_shift;       // 1 << m_segment_shift = segment size
        u32                  m_segment_count;       // Space Address Range / Segment Size = Number of segments
        u32                  m_segment_free_index;  //
        segment_t*           m_segment_array;       // Array of segments
        binmap_t             m_segment_free_binmap;
        binmap_t*            m_segment_active_binmaps;  // This needs to be per chunk-size
        u32                  m_used_physical_pages;     // The number of pages that are currently committed
        u32                  m_num_chunk_configs;
        u32                  m_num_superbins;
        superbin_t const*    m_superbins;      // Array of superbins
        chunkconfig_t const* m_chunk_configs;  // Array of chunk configs  (c64KB, c128KB, c256KB, c512KB, c1MB ... c512MB)

        superspace_t()
            : m_fsa(nullptr)
            , m_vmem(nullptr)
            , m_address_base(nullptr)
            , m_address_range(0)
            , m_page_size(0)
            , m_page_shift(0)
            , m_used_physical_pages(0)
            , m_segment_shift(0)
            , m_segment_count(0)
            , m_segment_free_index(0)
            , m_segment_array(nullptr)
            , m_segment_free_binmap()
            , m_segment_active_binmaps()
            , m_num_chunk_configs(0)
            , m_num_superbins(0)
            , m_superbins(nullptr)
            , m_chunk_configs(nullptr)
        {
        }

        void initialize(vmem_t* vmem, u64 address_range, s8 segment_shift, nsuperheap::alloc_t* heap, nsuperfsa::alloc_t* fsa, u32 num_superbins, superbin_t const* superbins, u32 num_chunk_configs, chunkconfig_t const* chunk_configs)
        {
            ASSERT(math::ispo2(address_range));

            m_fsa           = fsa;
            m_vmem          = vmem;
            m_address_range = address_range;
            u32 const attrs = 0;
            m_vmem->reserve(address_range, m_page_size, attrs, m_address_base);
            m_page_shift          = math::ilog2(m_page_size);
            m_used_physical_pages = 0;
            m_segment_shift       = segment_shift;

            m_segment_count = (u32)(m_address_range >> segment_shift);
            m_segment_array = (segment_t*)heap->calloc(m_segment_count * sizeof(segment_t));

            m_superbins         = superbins;
            m_num_chunk_configs = num_chunk_configs;
            m_chunk_configs     = chunk_configs;

            u32 l0len, l1len, l2len, l3len;
            binmap_t::compute_levels(m_segment_count, l0len, l1len, l2len, l3len);
            u32* l3 = (l3len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
            u32* l2 = (l2len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
            u32* l1 = (l1len > 0) ? (u32*)heap->alloc(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;
            m_segment_free_binmap.init_0(m_segment_count, l0len, l1, l1len, l2, l2len, l3, l3len);

            m_segment_active_binmaps = (binmap_t*)heap->calloc(m_num_chunk_configs * sizeof(binmap_t));
            for (u32 i = 0; i < m_num_chunk_configs; i++)
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
            if (m_vmem != nullptr)
            {
                m_vmem->release(m_address_base, m_address_range);
                m_vmem = nullptr;
            }
            heap->dealloc(m_segment_array);

            heap->dealloc(m_segment_free_binmap.m_l[0]);
            heap->dealloc(m_segment_free_binmap.m_l[1]);
            heap->dealloc(m_segment_free_binmap.m_l[2]);

            for (u32 i = 0; i < m_num_chunk_configs; i++)
            {
                binmap_t& bm = m_segment_active_binmaps[i];
                heap->dealloc(bm.m_l[0]);
                heap->dealloc(bm.m_l[1]);
                heap->dealloc(bm.m_l[2]);
            }
            heap->dealloc(m_segment_active_binmaps);
        }

        u32 checkout_segment(u8 chunk_config_index)
        {
            u32 const segment_chunk_count = (1 << (m_segment_shift - m_chunk_configs[chunk_config_index].m_chunk_size_shift));
            s32       segment_index       = m_segment_free_binmap.findandset();
            if (segment_index < 0)
            {
                segment_index = m_segment_free_index++;
            }
            ASSERT(m_segment_free_index < m_segment_count);
            segment_t* segment                 = &m_segment_array[segment_index];
            u32 const  chunks_index_array_iptr = m_fsa->alloc(sizeof(u32) * segment_chunk_count);
            segment->m_chunks_array            = (u32*)m_fsa->idx2ptr(chunks_index_array_iptr);

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
            segment->m_count_chunks_free   = 0;
            segment->m_count_chunks_used   = 0;
            segment->m_count_chunks_max    = segment_chunk_count;
            segment->m_chunk_config        = m_chunk_configs[chunk_config_index];

            return segment_index;
        }

        u32 chunk_physical_pages(superbin_t const& bin, u32 alloc_size) const
        {
            u64 const size = (bin.use_binmap()) ? (bin.m_alloc_size * bin.m_max_alloc_count) : alloc_size;
            return (u32)((size + (((u64)1 << m_page_shift) - 1)) >> m_page_shift);
        }

        void initialize_chunk(superspace_t::chunk_t* chunk, superbin_t const& bin, u32 segment_index, u32 segment_chunk_index)
        {
            chunk->m_bin_index           = bin.m_alloc_bin_index;
            chunk->m_segment_index       = segment_index;
            chunk->m_segment_chunk_index = segment_chunk_index;

            // Allocate allocation tag array
            chunk->m_elem_tag_array_iptr = nsuperfsa::alloc_t::NIL;
            if (bin.m_max_alloc_count > 1)
            {
                chunk->m_elem_tag_array_iptr = m_fsa->alloc(sizeof(u32) * bin.m_max_alloc_count);
            }

            if (bin.use_binmap())
            {
                u32 l0len, l1len, l2len, l3len;
                binmap_t::compute_levels(bin.m_max_alloc_count, l0len, l1len, l2len, l3len);

                u32* l3 = (l3len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l3len + 31) >> 5)) : nullptr;
                u32* l2 = (l2len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l2len + 31) >> 5)) : nullptr;
                u32* l1 = (l1len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * ((l1len + 31) >> 5)) : nullptr;

                chunk->m_elem_free_binmap_iptr = m_fsa->alloc(sizeof(binmap_t));
                binmap_t* bm                   = m_fsa->idx2ptr<binmap_t>(chunk->m_elem_free_binmap_iptr);
                bm->init_lazy_1(bin.m_max_alloc_count, l0len, l1, l1len, l2, l2len, l3, l3len);
            }

            chunk->m_elem_used_count = 0;
            chunk->m_elem_free_index = 0;
            chunk->m_physical_pages  = 0;
        }

        chunk_t* checkout_chunk(superbin_t const& bin)
        {
            // Get the chunk info index
            u32 const chunk_info_index = bin.m_chunk_config.m_chunk_info_index;

            s32 segment_index = m_segment_active_binmaps[chunk_info_index].findandset();
            if (segment_index < 0)
            {
                segment_index = checkout_segment(chunk_info_index);
                m_segment_active_binmaps[chunk_info_index].clr(segment_index);
            }
            ASSERT(segment_index < m_segment_count);

            u32 const required_physical_pages = chunk_physical_pages(bin, bin.m_alloc_size);
            m_used_physical_pages += required_physical_pages;

            // Here we have a segment where we can get a chunk from.
            // Remember to also lazily initialize the free and cached chunk binmaps.
            segment_t* segment                 = &m_segment_array[segment_index];
            chunk_t*   chunk                   = nullptr;
            u32        already_committed_pages = 0;
            s32        segment_chunk_index     = -1;

            if (segment->m_count_chunks_cached > 0)
            {
                segment->m_count_chunks_cached -= 1;
                segment_chunk_index     = segment->m_chunks_cached_binmap.findandset();
                u32 const chunk_iptr    = segment->m_chunks_array[segment_chunk_index];
                chunk                   = (chunk_t*)m_fsa->idx2ptr(chunk_iptr);
                already_committed_pages = chunk->m_physical_pages;
            }
            else if (segment->m_count_chunks_free > 0)
            {
                segment->m_count_chunks_free -= 1;
                segment_chunk_index     = segment->m_chunks_free_binmap.findandset();
                u32 const chunk_iptr    = segment->m_chunks_array[segment_chunk_index];
                chunk                   = (chunk_t*)m_fsa->idx2ptr(chunk_iptr);
                already_committed_pages = chunk->m_physical_pages;
            }
            else
            {
                // Use segment->m_chunks_free_index to take the next free chunk index.
                segment_chunk_index = segment->m_chunks_free_index;
                if ((segment_chunk_index & 0x1F) == 0)
                {  // If the lower 5 bits are 0, we need to clear a branch in the free and cached binmaps.
                    segment->m_chunks_cached_binmap.lazy_init_1(segment_chunk_index);
                    segment->m_chunks_free_binmap.lazy_init_1(segment_chunk_index);
                }
                segment->m_chunks_free_index += 1;
                u32 const chunk_iptr                         = m_fsa->alloc(sizeof(chunk_t));
                chunk                                        = (chunk_t*)m_fsa->idx2ptr(chunk_iptr);
                segment->m_chunks_array[segment_chunk_index] = chunk_iptr;
            }

            initialize_chunk(chunk, bin, segment_index, segment_chunk_index);

            // Commit the virtual pages for this chunk
            if (required_physical_pages < already_committed_pages)
            {
                // TODO Overcommitted, uncommit pages ?

                chunk->m_physical_pages = required_physical_pages;
            }
            else if (required_physical_pages > already_committed_pages)
            {
                // TODO Undercommitted, commit necessary pages

                chunk->m_physical_pages = required_physical_pages;
            }

            segment->m_count_chunks_used += 1;

            // Check if segment is full, if so we need to remove it from the list of active segments
            if (segment->m_count_chunks_used == segment->m_count_chunks_max)
            {
                m_segment_active_binmaps[chunk_info_index].set(segment_index);
            }

            return chunk;
        }

        void deinitialize_chunk(nsuperfsa::alloc_t& fsa, superspace_t::chunk_t* chunk, superbin_t const& bin)
        {
            if (chunk->m_elem_tag_array_iptr != nsuperfsa::alloc_t::NIL)
            {
                fsa.dealloc(chunk->m_elem_tag_array_iptr);
                chunk->m_elem_tag_array_iptr = nsuperfsa::alloc_t::NIL;
            }

            if (bin.use_binmap())
            {
                binmap_t* bm = fsa.idx2ptr<binmap_t>(chunk->m_elem_free_binmap_iptr);
                for (u32 i = 0; i < bm->num_levels(); i++)
                {
                    m_fsa->deallocptr(bm->m_l[i]);
                    bm->m_l[i] = nullptr;
                }
                fsa.deallocptr(bm);
                chunk->m_elem_free_binmap_iptr = nsuperfsa::alloc_t::NIL;
            }
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

            m_used_physical_pages -= chunk->m_physical_pages;

            // TODO: We need to limit the number of cached chunks
            segment->m_count_chunks_cached += 1;

            // Release the tracking array that was allocated for this chunk
            deinitialize_chunk(*m_fsa, chunk, m_superbins[chunk->m_bin_index]);

            // See if this segment is now empty, if so we need to release it
            segment->m_count_chunks_used -= 1;
            if (segment->m_count_chunks_used == 0)
            {
                m_segment_active_binmaps[segment->m_chunk_config.m_chunk_info_index].set(chunk->m_segment_index);

                // Maybe every size should cache at least one segment otherwise single alloc/dealloc calls will
                // checkout and release a segment every time?

                // Release all chunks in this segment
                while (segment->m_count_chunks_cached > 0)
                {
                    u32 const segment_chunk_index = 0;  // segment->m_chunks_cached_list_head_array->remove_headi(m_segment_list_data);
                    chunk_t*  chunk               = m_fsa->idx2ptr<chunk_t>(segment->m_chunks_array[segment_chunk_index]);
                    // TODO We need to decommit virtual memory here !
                    deinitialize_chunk(*m_fsa, chunk, m_superbins[chunk->m_bin_index]);
                    m_fsa->dealloc(segment->m_chunks_array[segment_chunk_index]);
                    segment->m_count_chunks_cached -= 1;
                }

                m_fsa->deallocptr(segment->m_chunks_array);
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

                segment->m_chunks_array        = nullptr;
                segment->m_count_chunks_cached = 0;
                segment->m_count_chunks_free   = 0;

                m_segment_free_binmap.clr(chunk->m_segment_index);
            }

            // Release the chunk structure back to the fsa
            m_fsa->deallocptr(chunk);
            segment->m_chunks_array[chunk->m_segment_chunk_index] = nsuperfsa::alloc_t::NIL;
        }

        void set_tag(void* ptr, u32 assoc, u32 num_superbins, superbin_t const* superbins)
        {
            ASSERT(ptr >= m_address_base && ptr < ((u8*)m_address_base + m_address_range));
            chunk_t*  chunk     = address_to_chunk(ptr);
            u32 const sbinindex = chunk->m_bin_index;
            ASSERT(sbinindex < num_superbins);
            superbin_t const& bin = superbins[sbinindex];

            segment_t* segment        = &m_segment_array[chunk->m_segment_index];
            u32*       elem_tag_array = (u32*)m_fsa->idx2ptr(chunk->m_elem_tag_array_iptr);

            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            if (bin.use_binmap())
            {
                void* const chunk_address        = chunk_to_address(segment, chunk);
                u32 const   chunk_item_index     = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
                elem_tag_array[chunk_item_index] = assoc;
            }
            else
            {
                elem_tag_array[0] = assoc;
            }
        }

        u32 get_tag(void* ptr, u32 num_superbins, superbin_t const* superbins) const
        {
            ASSERT(ptr >= m_address_base && ptr < ((u8*)m_address_base + m_address_range));
            chunk_t*  chunk     = address_to_chunk(ptr);
            u32 const sbinindex = chunk->m_bin_index;
            ASSERT(sbinindex < num_superbins);
            superbin_t const& bin = superbins[sbinindex];            

            segment_t* segment        = &m_segment_array[chunk->m_segment_index];
            u32* const elem_tag_array = (u32*)m_fsa->idx2ptr(chunk->m_elem_tag_array_iptr);

            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            if (bin.use_binmap())
            {
                void* const chunk_address       = chunk_to_address(segment, chunk);
                u32 const   chunk_element_index = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
                return elem_tag_array[chunk_element_index];
            }
            return elem_tag_array[0];
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
            u64 const segment_offset = ((u64)(segment - m_segment_array) << m_segment_shift);
            return toaddress(m_address_base, segment_offset + ((u64)chunk->m_segment_chunk_index << segment->m_chunk_config.m_chunk_size_shift));
        }
    };

    /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    /// The following is a strict data-drive initialization of the bins and allocators, please know what you are doing when modifying any of this.

    struct superallocator_config_t
    {
        superallocator_config_t()
            : m_total_address_size(0)
            , m_segment_address_range(0)
            , m_segment_address_range_shift(0)
            , m_num_superbins(0)
            , m_asuperbins(nullptr)
            , m_internal_heap_address_range(0)
            , m_internal_heap_pre_size(0)
            , m_internal_fsa_address_range(0)
            , m_internal_fsa_segment_size(0)
            , m_internal_fsa_pre_size(0)
        {
        }

        superallocator_config_t(const superallocator_config_t& other)
            : m_total_address_size(other.m_total_address_size)
            , m_segment_address_range(other.m_segment_address_range)
            , m_segment_address_range_shift(other.m_segment_address_range_shift)
            , m_num_superbins(other.m_num_superbins)
            , m_asuperbins(other.m_asuperbins)
            , m_internal_heap_address_range(other.m_internal_heap_address_range)
            , m_internal_heap_pre_size(other.m_internal_heap_pre_size)
            , m_internal_fsa_address_range(other.m_internal_fsa_address_range)
            , m_internal_fsa_segment_size(other.m_internal_fsa_segment_size)
            , m_internal_fsa_pre_size(other.m_internal_fsa_pre_size)
        {
        }

        superallocator_config_t(u64 space_address_range, u32 segment_address_range, s32 const num_superbins, superbin_t const* asuperbins, u32 const internal_heap_address_range, u32 const internal_heap_pre_size, u32 const internal_fsa_address_range,
                                u32 const internal_fsa_segment_size, u32 const internal_fsa_pre_size)
            : m_total_address_size(space_address_range)
            , m_segment_address_range(segment_address_range)
            , m_num_superbins(num_superbins)
            , m_asuperbins(asuperbins)
            , m_internal_heap_address_range(internal_heap_address_range)
            , m_internal_heap_pre_size(internal_heap_pre_size)
            , m_internal_fsa_address_range(internal_fsa_address_range)
            , m_internal_fsa_segment_size(internal_fsa_segment_size)
            , m_internal_fsa_pre_size(internal_fsa_pre_size)
        {
            m_segment_address_range_shift = math::ilog2(segment_address_range);
        }

        u64               m_total_address_size;
        u64               m_segment_address_range;
        s8                m_segment_address_range_shift;
        u32               m_num_superbins;
        superbin_t const* m_asuperbins;
        u32               m_internal_heap_address_range;
        u32               m_internal_heap_pre_size;
        u32               m_internal_fsa_address_range;
        u32               m_internal_fsa_segment_size;
        u32               m_internal_fsa_pre_size;
    };

    // 25% allocation waste (based on empirical data)
    namespace superallocator_config_windows_desktop_app_25p_t
    {
        // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly

        // clang-format off
        // superbin_t(bin-index or remap, alloc-size, chunk-config)
        static const s32        c_num_bins           = 112;
        static const superbin_t c_asbins[c_num_bins] = {
          superbin_t(8,         8,  c64KB),                    superbin_t(8,    8,  c64KB),                    // 8, 8
          superbin_t(8,         8,  c64KB),                    superbin_t(8,    8,  c64KB),                    // 8, 8
          superbin_t(8,         8,  c64KB),                    superbin_t(8,    8,  c64KB),                    // 8, 8
          superbin_t(8,         8,  c64KB),                    superbin_t(8,    8,  c64KB),                    // 8, 8
          superbin_t(8,         8,  c64KB),                    superbin_t(10,   10, c64KB),                    // 8, 12
          superbin_t(10,        12, c64KB),                    superbin_t(12,   14, c64KB),                    // 12, 16
          superbin_t(12,        16, c64KB),                    superbin_t(13,   20, c64KB),                    // 16, 20
          superbin_t(14,        24, c64KB),                    superbin_t(15,   28, c64KB),                    // 24, 28
          superbin_t(16,        32, c64KB),                    superbin_t(17,   40, c64KB),                    // 32, 40
          superbin_t(18,        48, c64KB),                    superbin_t(19,   56, c64KB),                    // 48, 56
          superbin_t(20,        64, c64KB),                    superbin_t(21,   80, c64KB),                    //
          superbin_t(22,        96, c64KB),                    superbin_t(23,   112, c64KB),                   //
          superbin_t(24,       128, c64KB),                    superbin_t(25,   160, c64KB),                   //
          superbin_t(26,       192, c64KB),                    superbin_t(27,   224, c64KB),                   //
          superbin_t(28,       256, c64KB),                    superbin_t(29,   320, c64KB),                   //
          superbin_t(30,       384, c64KB),                    superbin_t(31,   448, c64KB),                   //
          superbin_t(32,       512, c64KB),                    superbin_t(33,   640, c64KB),                   //
          superbin_t(34,       768, c64KB),                    superbin_t(35,   896, c64KB),                   //
          superbin_t(36,   1 * cKB, c64KB),                    superbin_t(37,  1*cKB + 256, c128KB),           //
          superbin_t(38,   1 * cKB + 512, c128KB),             superbin_t(39,  1*cKB + 768, c128KB),           //
          superbin_t(40,   2 * cKB, c128KB),                   superbin_t(41,  2*cKB + 512, c128KB),           //
          superbin_t(42,   3 * cKB, c128KB),                   superbin_t(43,  3*cKB + 512, c128KB),           //
          superbin_t(44,   4 * cKB, c128KB),                   superbin_t(45,  5*cKB, c128KB),                 //
          superbin_t(46,   6 * cKB, c128KB),                   superbin_t(47,  7*cKB, c128KB),                 //
          superbin_t(48,   8 * cKB, c128KB),                   superbin_t(49,  10*cKB, c128KB),                //
          superbin_t(50,  12 * cKB, c128KB),                   superbin_t(51,  14*cKB, c128KB),                //
          superbin_t(52,  16 * cKB, c128KB),                   superbin_t(53,  20*cKB, c128KB),                //
          superbin_t(54,  24 * cKB, c128KB),                   superbin_t(55,  28*cKB, c128KB),                //
          superbin_t(56,  32 * cKB, c128KB),                   superbin_t(57,  40*cKB, c128KB),                //
          superbin_t(58,  48 * cKB, c128KB),                   superbin_t(59,  56*cKB, c128KB),                //
          superbin_t(60,  64 * cKB, c128KB),                   superbin_t(61,  80*cKB, c128KB),                //
          superbin_t(62,  96 * cKB, c128KB),                   superbin_t(63,  112*cKB, c512KB),               //
          superbin_t(64, 128 * cKB, c512KB),                   superbin_t(65,  160*cKB, c512KB),               //
          superbin_t(66, 192 * cKB, c512KB),                   superbin_t(67,  224*cKB, c1MB),               //
          superbin_t(68, 256 * cKB, c512KB),                   superbin_t(69,  320*cKB, c1MB),               //
          superbin_t(70, 384 * cKB, c1MB),                     superbin_t(71,  448*cKB, c1MB),               //
          superbin_t(72, 512 * cKB, c1MB),                     superbin_t(73,  640*cKB, c1MB),               //
          superbin_t(74, 768 * cKB, c1MB),                     superbin_t(75,  896*cKB, c1MB),               //
          superbin_t(76,   1 * cMB, c1MB),                     superbin_t(77, 1*cMB + 256*cKB, c2MB),        //
          superbin_t(78,   1 * cMB + 512 * cKB, c2MB),         superbin_t(79, 1*cMB + 768*cKB, c2MB),        //
          superbin_t(80,   2 * cMB, c32MB),                    superbin_t(81, 2*cMB + 512*cKB, c32MB),        //
          superbin_t(82,   3 * cMB, c32MB),                    superbin_t(83, 3*cMB + 512*cKB, c32MB),        //
          superbin_t(84,   4 * cMB, c32MB),                    superbin_t(85, 5*cMB, c32MB),                  //
          superbin_t(86,   6 * cMB, c32MB),                    superbin_t(87, 7*cMB, c32MB),                  //
          superbin_t(88,   8 * cMB, c32MB),                    superbin_t(89, 10*cMB, c32MB),                 //
          superbin_t(90,  12 * cMB, c32MB),                    superbin_t(91, 14*cMB, c32MB),                 //
          superbin_t(92,  16 * cMB, c32MB),                    superbin_t(93, 20*cMB, c32MB),                 //
          superbin_t(94,  24 * cMB, c32MB),                    superbin_t(95, 28*cMB, c32MB),                 //
          superbin_t(96,  32 * cMB, c32MB),                    superbin_t(97, 40*cMB, c128MB),                 //
          superbin_t(98,  48 * cMB, c128MB),                   superbin_t(99, 56*cMB, c128MB),                 //
          superbin_t(100,  64 * cMB, c128MB),                  superbin_t(101, 80*cMB, c128MB),                //
          superbin_t(102,  96 * cMB, c128MB),                  superbin_t(103, 112*cMB, c128MB),               //
          superbin_t(104, 128 * cMB, c128MB),                  superbin_t(105, 160*cMB, c512MB),               //
          superbin_t(106, 192 * cMB, c512MB),                  superbin_t(107, 224*cMB, c512MB),               //
          superbin_t(108, 256 * cMB, c512MB),                  superbin_t(109, 320*cMB, c512MB),               //
          superbin_t(110, 384 * cMB, c512MB),                  superbin_t(111, 448*cMB, c512MB),               //
        };
        // clang-format on

        static superallocator_config_t get_config()
        {
            const u32 c_page_size                   = 4096;  // Windows OS page size
            const u64 c_total_address_space         = 128 * cGB;
            const u64 c_segment_address_range       = 1 * cGB;
            const u32 c_internal_heap_address_range = 16 * cMB;
            const u32 c_internal_heap_pre_size      = 2 * cMB;
            const u32 c_internal_fsa_address_range  = 256 * cMB;
            const u32 c_internal_fsa_segment_size   = 8 * cMB;
            const u32 c_internal_fsa_pre_size       = 16 * cMB;
            return superallocator_config_t(c_total_address_space, c_segment_address_range, c_num_bins, c_asbins, c_internal_heap_address_range, c_internal_heap_pre_size, c_internal_fsa_address_range, c_internal_fsa_segment_size, c_internal_fsa_pre_size);
        }

        static inline s32 size2bin(u32 size)
        {
            const s32 w = math::countLeadingZeros(size);
            const u32 f = (u32)0x80000000 >> w;
            const u32 r = 0xFFFFFFFF << (29 - w);
            const u32 t = ((f - 1) >> 2);
            size        = (size + t) & ~t;
            const int i = (int)((size & r) >> (29 - w)) + ((29 - w) * 4);
            return i;
        }

    };  // namespace superallocator_config_windows_desktop_app_25p_t

    // 10% allocation waste (based on empirical data)
    namespace superallocator_config_windows_desktop_app_10p_t
    {
        // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly

        // clang-format off
        // superbin_t(bin-index or remap, alloc-size, chunk-config)
        static const s32        c_num_bins           = 216;
        static const superbin_t c_asbins[c_num_bins] = {
          superbin_t(8,   8, c64KB),                              superbin_t(8,   8, c64KB),                    //
          superbin_t(8,   8, c64KB),                              superbin_t(8,   8, c64KB),                    //
          superbin_t(8,   8, c64KB),                              superbin_t(8,   8, c64KB),                    //
          superbin_t(8,   8, c64KB),                              superbin_t(8,   8, c64KB),                    //
          superbin_t(8,   8, c64KB),                              superbin_t(12,   9, c64KB),                   //
          superbin_t(12,   10, c64KB),                            superbin_t(12,   11, c64KB),                  //
          superbin_t(12,   12, c64KB),                            superbin_t(16,   13, c64KB),                  //
          superbin_t(16,   14, c64KB),                            superbin_t(16,   15, c64KB),                  //
          superbin_t(16,   16, c64KB),                            superbin_t(18,   18, c64KB),                  //
          superbin_t(18,   20, c64KB),                            superbin_t(20,   22, c64KB),                  //
          superbin_t(20,   24, c64KB),                            superbin_t(22,   26, c64KB),                  //
          superbin_t(22,   28, c64KB),                            superbin_t(24,   30, c64KB),                  //
          superbin_t(24,   32, c64KB),                            superbin_t(25,   36, c64KB),                  //
          superbin_t(26,   40, c64KB),                            superbin_t(27,   44, c64KB),                  //
          superbin_t(28,   48, c64KB),                            superbin_t(29,   52, c64KB),                  //
          superbin_t(30,   56, c64KB),                            superbin_t(31,   60, c64KB),                  //
          superbin_t(32,   64, c64KB),                            superbin_t(33,   72, c64KB),                  //
          superbin_t(34,   80, c64KB),                            superbin_t(35,   88, c64KB),                  //
          superbin_t(36,   96, c64KB),                            superbin_t(37,   104, c64KB),                 //
          superbin_t(38,   112, c64KB),                           superbin_t(39,   120, c64KB),                 //
          superbin_t(40,   128, c64KB),                           superbin_t(41,   144, c64KB),                 //
          superbin_t(42,   160, c64KB),                           superbin_t(43,   176, c64KB),                 //
          superbin_t(44,   192, c64KB),                           superbin_t(45,   208, c64KB),                 //
          superbin_t(46,   224, c64KB),                           superbin_t(47,   240, c64KB),                 //
          superbin_t(48,   256, c64KB),                           superbin_t(49,   288, c64KB),                 //
          superbin_t(50,   320, c64KB),                           superbin_t(51,   352, c64KB),                 //
          superbin_t(52,   384, c64KB),                           superbin_t(53,   416, c64KB),                 //
          superbin_t(54,   448, c64KB),                           superbin_t(55,   480, c64KB),                 //
          superbin_t(56,   512, c64KB),                           superbin_t(57,   576, c64KB),                 //
          superbin_t(58,   640, c64KB),                           superbin_t(59,   704, c64KB),                 //
          superbin_t(60,   768, c64KB),                           superbin_t(61,   832, c64KB),                 //
          superbin_t(62,   896, c64KB),                           superbin_t(63,   960, c64KB),                 //
          superbin_t(64,  1*cKB, c64KB),                          superbin_t(65,  1*cKB + 128, c64KB),          //
          superbin_t(66,  1*cKB + 256, c64KB),                    superbin_t(67,  1*cKB + 384, c64KB),          //
          superbin_t(68,  1*cKB + 512, c64KB),                    superbin_t(69,  1*cKB + 640, c64KB),          //
          superbin_t(70,  1*cKB + 768, c64KB),                    superbin_t(71,  1*cKB + 896, c64KB),          //
          superbin_t(72,  2*cKB, c64KB),                          superbin_t(73,  2*cKB + 256, c64KB),          //
          superbin_t(74,  2*cKB + 512, c64KB),                    superbin_t(75,  2*cKB + 768, c64KB),          //
          superbin_t(76,  3*cKB, c64KB),                          superbin_t(77,  3*cKB + 256, c64KB),          //
          superbin_t(78,  3*cKB + 512, c64KB),                    superbin_t(79,  3*cKB + 768, c64KB),          //
          superbin_t(80,  4*cKB, c64KB),                          superbin_t(81,  4*cKB + 512, c64KB),          //
          superbin_t(82,  5*cKB, c64KB),                          superbin_t(83,  5*cKB + 512, c64KB),          //
          superbin_t(84,  6*cKB, c64KB),                          superbin_t(85,  6*cKB + 512, c64KB),          //
          superbin_t(86,  7*cKB, c64KB),                          superbin_t(87,  7*cKB + 512, c64KB),          //
          superbin_t(88,  8*cKB, c64KB),                          superbin_t(89,  9*cKB, c64KB),                //
          superbin_t(90,  10*cKB, c64KB),                         superbin_t(91,  11*cKB, c64KB),               //
          superbin_t(92,  12*cKB, c64KB),                         superbin_t(93,  13*cKB, c64KB),               //
          superbin_t(94,  14*cKB, c64KB),                         superbin_t(95,  15*cKB, c64KB),               //
          superbin_t(96,  16*cKB, c64KB),                         superbin_t(97,  18*cKB, c64KB),               //
          superbin_t(98,  20*cKB, c64KB),                         superbin_t(99,  22*cKB, c64KB),               //
          superbin_t(100,  24*cKB, c64KB),                        superbin_t(101,  26*cKB, c64KB),              //
          superbin_t(102,  28*cKB, c64KB),                        superbin_t(103,  30*cKB, c64KB),              //
          superbin_t(104,  32*cKB, c64KB),                        superbin_t(105,  36*cKB, c64KB),              //
          superbin_t(106,  40*cKB, c64KB),                        superbin_t(107,  44*cKB, c64KB),              //
          superbin_t(108,  48*cKB, c64KB),                        superbin_t(109,  52*cKB, c64KB),              //
          superbin_t(110,  56*cKB, c64KB),                        superbin_t(111,  60*cKB, c64KB),              //
          superbin_t(112,  64*cKB, c64KB),                        superbin_t(113,  72*cKB, c64KB),              //
          superbin_t(114,  80*cKB, c64KB),                        superbin_t(115,  88*cKB, c64KB),              //
          superbin_t(116,  96*cKB, c64KB),                        superbin_t(117,  104*cKB, c64KB),             //
          superbin_t(118,  112*cKB, c64KB),                       superbin_t(119,  120*cKB, c64KB),             //
          superbin_t(120,  128*cKB, c64KB),                       superbin_t(121,  144*cKB, c64KB),             //
          superbin_t(122,  160*cKB, c64KB),                       superbin_t(123,  176*cKB, c64KB),             //
          superbin_t(124,  192*cKB, c64KB),                       superbin_t(125,  208*cKB, c64KB),             //
          superbin_t(126,  224*cKB, c64KB),                       superbin_t(127,  240*cKB, c64KB),             //
          superbin_t(128,  256*cKB, c64KB),                       superbin_t(129,  288*cKB, c64KB),             //
          superbin_t(130,  320*cKB, c64KB),                       superbin_t(131,  352*cKB, c64KB),             //
          superbin_t(132,  384*cKB, c64KB),                       superbin_t(133,  416*cKB, c64KB),             //
          superbin_t(134,  448*cKB, c64KB),                       superbin_t(135,  480*cKB, c64KB),             //
          superbin_t(136,  512*cKB, c64KB),                       superbin_t(137,  576*cKB, c64KB),             //
          superbin_t(138,  640*cKB, c64KB),                       superbin_t(139,  704*cKB, c64KB),             //
          superbin_t(140,  768*cKB, c64KB),                       superbin_t(141,  832*cKB, c64KB),             //
          superbin_t(142,  896*cKB, c64KB),                       superbin_t(143,  960*cKB, c64KB),             //
          superbin_t(144, 1*cMB, c64KB),                          superbin_t(145, 1*cMB + 128*cKB, c64KB),      //
          superbin_t(146, 1*cMB + 256*cKB, c64KB),                superbin_t(147, 1*cMB + 384*cKB, c64KB),      //
          superbin_t(148, 1*cMB + 512*cKB, c64KB),                superbin_t(149, 1*cMB + 640*cKB, c64KB),      //
          superbin_t(150, 1*cMB + 768*cKB, c64KB),                superbin_t(151, 1*cMB + 896*cKB, c64KB),      //
          superbin_t(152, 2*cMB, c64KB),                          superbin_t(153, 2*cMB + 256*cKB, c64KB),      //
          superbin_t(154, 2*cMB + 512*cKB, c64KB),                superbin_t(155, 2*cMB + 768*cKB, c64KB),      //
          superbin_t(156, 3*cMB, c64KB),                          superbin_t(157, 3*cMB + 256*cKB, c64KB),      //
          superbin_t(158, 3*cMB + 512*cKB, c64KB),                superbin_t(159, 3*cMB + 768*cKB, c64KB),      //
          superbin_t(160, 4*cMB, c64KB),                          superbin_t(161, 4*cMB + 512*cKB, c64KB),      //
          superbin_t(162, 5*cMB, c64KB),                          superbin_t(163, 5*cMB + 512*cKB, c64KB),      //
          superbin_t(164, 6*cMB, c64KB),                          superbin_t(165, 6*cMB + 512*cKB, c64KB),      //
          superbin_t(166, 7*cMB, c64KB),                          superbin_t(167, 7*cMB + 512*cKB, c64KB),      //
          superbin_t(168, 8*cMB, c64KB),                          superbin_t(169, 9*cMB, c64KB),                //
          superbin_t(170, 10*cKB, c64KB),                         superbin_t(171, 11*cMB, c64KB),               //
          superbin_t(172, 12*cMB, c64KB),                         superbin_t(173, 13*cMB, c64KB),               //
          superbin_t(174, 14*cMB, c64KB),                         superbin_t(175, 15*cMB, c64KB),               //
          superbin_t(176, 16*cMB, c64KB),                         superbin_t(177, 18*cMB, c64KB),               //
          superbin_t(178, 20*cKB, c64KB),                         superbin_t(179, 22*cMB, c64KB),               //
          superbin_t(180, 24*cMB, c64KB),                         superbin_t(181, 26*cMB, c64KB),               //
          superbin_t(182, 28*cMB, c64KB),                         superbin_t(183, 30*cKB, c64KB),               //
          superbin_t(184, 32*cMB, c64KB),                         superbin_t(185, 36*cMB, c64KB),               //
          superbin_t(186, 40*cKB, c64KB),                         superbin_t(187, 44*cMB, c64KB),               //
          superbin_t(188, 48*cMB, c64KB),                         superbin_t(189, 52*cMB, c64KB),               //
          superbin_t(190, 56*cMB, c64KB),                         superbin_t(191, 60*cKB, c64KB),               //
          superbin_t(192, 64*cMB, c64KB),                         superbin_t(193, 72*cMB, c64KB),               //
          superbin_t(194, 80*cKB, c64KB),                         superbin_t(195, 88*cMB, c64KB),               //
          superbin_t(196, 96*cMB, c64KB),                         superbin_t(197, 104*cMB, c64KB),              //
          superbin_t(198, 112*cMB, c64KB),                        superbin_t(199, 120*cKB, c64KB),              //
          superbin_t(200, 128*cMB, c64KB),                        superbin_t(201, 144*cMB, c64KB),              //
          superbin_t(202, 160*cKB, c64KB),                        superbin_t(203, 176*cMB, c64KB),              //
          superbin_t(204, 192*cMB, c64KB),                        superbin_t(205, 208*cMB, c64KB),              //
          superbin_t(206, 224*cMB, c64KB),                        superbin_t(207, 240*cKB, c64KB),              //
          superbin_t(208, 256*cMB, c64KB),                        superbin_t(209, 288*cMB, c64KB),              //
          superbin_t(210, 320*cKB, c64KB),                        superbin_t(211, 352*cMB, c64KB),              //
          superbin_t(212, 384*cMB, c64KB),                        superbin_t(213, 416*cMB, c64KB),              //
          superbin_t(214, 448*cMB, c64KB),                        superbin_t(215, 480*cKB, c64KB),              //
        };
        // clang-format on

        static superallocator_config_t get_config()
        {
            const u32 c_page_size                   = 4096;  // Windows OS page size
            const u64 c_total_address_space         = 128 * cGB;
            const u64 c_segment_address_range       = 1 * cGB;
            const u32 c_internal_heap_address_range = 16 * cMB;
            const u32 c_internal_heap_pre_size      = 2 * cMB;
            const u32 c_internal_fsa_address_range  = 256 * cMB;
            const u32 c_internal_fsa_segment_size   = 8 * cMB;
            const u32 c_internal_fsa_pre_size       = 16 * cMB;
            return superallocator_config_t(c_total_address_space, c_segment_address_range, c_num_bins, c_asbins, c_internal_heap_address_range, c_internal_heap_pre_size, c_internal_fsa_address_range, c_internal_fsa_segment_size, c_internal_fsa_pre_size);
        }

        static inline s32 size2bin(u32 size)
        {
            s32 w = math::countLeadingZeros(size);
            u32 f = (u32)0x80000000 >> w;
            u32 r = 0xFFFFFFFF << (28 - w);
            u32 t = ((f - 1) >> 3);
            size  = (size + t) & ~t;
            int i = (int)((size & r) >> (28 - w)) + ((28 - w) * 8);
            return i;
        }

    };  // namespace superallocator_config_windows_desktop_app_10p_t

    namespace superallocator_config = superallocator_config_windows_desktop_app_10p_t;
    // namespace superallocator_config = superallocator_config_windows_desktop_app_25p_t;

    class superalloc_t : public valloc_t
    {
    public:
        u32                 m_num_superbins;
        superbin_t const*   m_asuperbins;
        vmem_t*             m_vmem;
        void*               m_vmem_membase;
        nsuperheap::alloc_t m_internal_heap;
        nsuperfsa::alloc_t  m_internal_fsa;
        superspace_t*       m_superspace;
        lldata_t            m_chunk_list_data;
        llhead_t*           m_active_chunk_list_per_alloc_size;
        alloc_t*            m_main_allocator;

        superalloc_t(alloc_t* main_allocator)
            : m_num_superbins(0)
            , m_asuperbins(nullptr)
            , m_vmem(nullptr)
            , m_internal_heap()
            , m_internal_fsa()
            , m_superspace(nullptr)
            , m_chunk_list_data()
            , m_active_chunk_list_per_alloc_size(nullptr)
            , m_main_allocator(main_allocator)
        {
        }

        DCORE_CLASS_PLACEMENT_NEW_DELETE

        void initialize(vmem_t* vmem, superallocator_config_t const& config);
        void deinitialize();

        void* v_allocate(u32 size, u32 alignment);
        u32   v_deallocate(void* ptr);
        void  v_release() {}
        u32   v_get_size(void* ptr) const;
        void  v_set_tag(void* ptr, u32 assoc);
        u32   v_get_tag(void* ptr) const;
    };

    void superalloc_t::initialize(vmem_t* vmem, superallocator_config_t const& config)
    {
        m_num_superbins = config.m_num_superbins;
        m_asuperbins    = config.m_asuperbins;
        m_vmem          = vmem;

        m_internal_heap.initialize(m_vmem, config.m_internal_heap_address_range, config.m_internal_heap_pre_size);
        m_internal_fsa.initialize(&m_internal_heap, m_vmem, config.m_internal_fsa_address_range, config.m_internal_fsa_segment_size);

#ifdef SUPERALLOC_DEBUG
        // sanity check on the superbin_t config
        for (u32 s = 0; s < config.m_num_superbins; s++)
        {
            u32 const rs            = config.m_asuperbins[s].m_alloc_bin_index;
            u32 const size          = config.m_asuperbins[rs].m_alloc_size;
            u32 const bin_index     = superallocator_config::size2bin(size);
            u32 const bin_reindex   = config.m_asuperbins[bin_index].m_alloc_bin_index;
            u32 const bin_allocsize = config.m_asuperbins[bin_reindex].m_alloc_size;
            ASSERT(size <= bin_allocsize);
        }
#endif

        m_superspace = (superspace_t*)m_internal_heap.calloc(sizeof(superspace_t));
        m_superspace->initialize(vmem, config.m_total_address_size, config.m_segment_address_range_shift, &m_internal_heap, &m_internal_fsa, config.m_num_superbins, config.m_asuperbins, cNumChunkConfigs, cChunkInfoArray);

        m_chunk_list_data.m_data           = m_internal_fsa.base_address();
        m_chunk_list_data.m_itemsize       = sizeof(superspace_t::chunk_t);
        m_active_chunk_list_per_alloc_size = (llhead_t*)m_internal_heap.alloc(config.m_num_superbins * sizeof(llhead_t));
        for (u32 i = 0; i < config.m_num_superbins; i++)
            m_active_chunk_list_per_alloc_size[i].reset();
    }

    void superalloc_t::deinitialize()
    {
        m_num_superbins = 0;
        m_asuperbins    = nullptr;
        m_internal_fsa.deinitialize(&m_internal_heap);
        m_internal_heap.deinitialize();
        m_superspace     = nullptr;
        m_vmem           = nullptr;
        m_vmem_membase   = nullptr;
        m_main_allocator = nullptr;
    }

    void* superalloc_t::v_allocate(u32 alloc_size, u32 alignment)
    {
        alloc_size                  = math::alignUp(alloc_size, alignment);
        u32 const         sbinindex = m_asuperbins[superallocator_config::size2bin(alloc_size)].m_alloc_bin_index;
        superbin_t const& bin       = m_asuperbins[sbinindex];
        ASSERT(bin.m_alloc_bin_index == sbinindex);
        ASSERT(alloc_size <= bin.m_alloc_size);

        u32 const              chunk_iptr = m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].m_index;
        superspace_t::chunk_t* chunk      = nullptr;
        if (chunk_iptr == nsuperfsa::alloc_t::NIL)
        {
            chunk                                                     = m_superspace->checkout_chunk(bin);
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index] = m_internal_fsa.ptr2idx(chunk);
        }
        else
        {
            chunk = m_internal_fsa.idx2ptr<superspace_t::chunk_t>(chunk_iptr);
        }

        ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);

        superspace_t::segment_t* segment = &m_superspace->m_segment_array[chunk->m_segment_index];
        void*                    ptr     = m_superspace->chunk_to_address(segment, chunk);
        if (bin.use_binmap())
        {
            // If we have elements in the binmap, we can use it to get a free element.
            // If not, we need to use free_index as the free element.
            u32 elem_index;
            if (chunk->m_elem_used_count == chunk->m_elem_free_index)
            {  // seems all elements are used, lazy initialize binmap
                if ((chunk->m_elem_free_index & 0x1F) == 0)
                {
                    binmap_t* bm = m_internal_fsa.idx2ptr<binmap_t>(chunk->m_elem_free_binmap_iptr);
                    bm->lazy_init_1(chunk->m_elem_free_index);
                }
                elem_index = chunk->m_elem_free_index++;
            }
            else
            {
                binmap_t* bm = m_internal_fsa.idx2ptr<binmap_t>(chunk->m_elem_free_binmap_iptr);
                elem_index   = bm->findandset();
            }
            ASSERT(elem_index < bin.m_max_alloc_count);
            ptr = toaddress(ptr, (u64)elem_index * bin.m_alloc_size);

            // Initialize the tag value for this element
            u32* elem_tag_array        = (u32*)m_internal_fsa.idx2ptr(chunk->m_elem_tag_array_iptr);
            elem_tag_array[elem_index] = 0;
        }
        else
        {
            chunk->m_physical_pages = (alloc_size + (m_superspace->m_page_size - 1)) >> m_superspace->m_page_shift;
            // Initialize the tag value for this element, abuse the iptr as the tag
            chunk->m_elem_tag_array_iptr = 0;
        }

        chunk->m_elem_used_count += 1;
        if (bin.m_max_alloc_count == chunk->m_elem_used_count)
        {
            // Chunk is full, so remove it from the list of active chunks
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].remove_headi(m_chunk_list_data);
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
        superbin_t const&      bin   = m_asuperbins[chunk->m_bin_index];

        u32 alloc_size;
        if (bin.use_binmap())
        {
            superspace_t::segment_t* segment       = &m_superspace->m_segment_array[chunk->m_segment_index];
            void* const              chunk_address = m_superspace->chunk_to_address(segment, chunk);
            u32 const                elem_index    = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            ASSERT(elem_index < bin.m_max_alloc_count);
            binmap_t* bm = m_internal_fsa.idx2ptr<binmap_t>(chunk->m_elem_free_binmap_iptr);
            bm->clr(elem_index);
            u32* elem_tag_array = (u32*)m_internal_fsa.idx2ptr(chunk->m_elem_tag_array_iptr);
            ASSERT(elem_tag_array[elem_index] != 0xFEFEFEFE);  // Double freeing this element ?
            elem_tag_array[elem_index] = 0xFEFEFEFE;           // Clear the tag for this element (mark it as freed)
            alloc_size                 = bin.m_alloc_size;
        }
        else
        {
            // Single element chunk, m_elem_tag_array_iptr is abused as the tag
            ASSERT(chunk->m_elem_tag_array_iptr != 0xFEFEFEFE);  // Double freeing this element ?
            chunk->m_elem_tag_array_iptr = 0xFEFEFEFE;           // Clear the tag for this element (mark it as freed)
            alloc_size                   = chunk->m_physical_pages * m_superspace->m_page_size;
        }

        const bool chunk_was_full = (bin.m_max_alloc_count == chunk->m_elem_used_count);

        chunk->m_elem_used_count -= 1;
        if (0 == chunk->m_elem_used_count)
        {
            if (!chunk_was_full)
            {
                // We are going to release this chunk, so remove it from the active chunk list before doing that.
                m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].remove_item(m_chunk_list_data, m_internal_fsa.ptr2idx(chunk));
            }
            m_superspace->release_chunk(chunk);
        }
        else if (chunk_was_full)
        {
            // Ok, this chunk can be used to allocate from again, so add it to the list of active chunks
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].insert(m_chunk_list_data, m_internal_fsa.ptr2idx(chunk));
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
        superbin_t const&      bin   = m_asuperbins[chunk->m_bin_index];
        if (bin.use_binmap())
        {
            return bin.m_alloc_size;
        }
        else
        {
            return chunk->m_physical_pages * m_superspace->m_page_size;
        }
    }

    void superalloc_t::v_set_tag(void* ptr, u32 assoc)
    {
        if (ptr != nullptr)
            m_superspace->set_tag(ptr, assoc, m_num_superbins, m_asuperbins);
    }

    u32 superalloc_t::v_get_tag(void* ptr) const
    {
        if (ptr == nullptr)
            return 0xffffffff;
        return m_superspace->get_tag(ptr, m_num_superbins, m_asuperbins);
    }

    valloc_t* gCreateVmAllocator(alloc_t* main_heap, vmem_t* vmem)
    {
        superalloc_t* superalloc = main_heap->construct<superalloc_t>(main_heap);
        superalloc->initialize(vmem, superallocator_config::get_config());
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
