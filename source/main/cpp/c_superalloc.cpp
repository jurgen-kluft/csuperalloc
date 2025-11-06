#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_binmap1.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"
#include "ccore/c_vmem.h"

#include "callocator/c_allocator_segment.h"

#include "csuperalloc/private/c_doubly_linked_list.h"
#include "csuperalloc/c_superalloc.h"
#include "csuperalloc/c_superalloc_config.h"

namespace ncore
{
    namespace nsuperalloc
    {
        // @TODO: Deal with jittering between block checkout/release

#define SUPERALLOC_DEBUG

        static inline void* toaddress(void* base, u64 offset) { return (void*)((ptr_t)base + offset); }
        static inline ptr_t todistance(void const* base, void const* ptr) { return (ptr_t)((ptr_t)ptr - (ptr_t)base); }

        namespace nsuperheap
        {
            // Can only allocate, used internally to allocate memory needed at initialization and runtime

            class alloc_t : public ncore::alloc_t
            {
            public:
                vmem_arena_t m_arena;

                void initialize(u64 memory_range, u64 size_to_pre_allocate);
                void deinitialize();

                void* v_allocate(u32 size, u32 alignment);  // allocate
                void  v_deallocate(void* ptr);              // deallocate

                DCORE_CLASS_PLACEMENT_NEW_DELETE
            };

            void alloc_t::initialize(u64 memory_range, u64 size_to_pre_allocate)
            {
                m_arena.reserved((int_t)memory_range);
                if (size_to_pre_allocate > 0)
                {
                    m_arena.committed((int_t)size_to_pre_allocate);
                }
            }

            void alloc_t::deinitialize()
            {
                //nvmem::release(m_address, m_address_range);
                m_arena.release();
            }

            void* alloc_t::v_allocate(u32 size, u32 alignment)
            {
                if (size == 0)
                    return nullptr;

                void* ptr = m_arena.commit(size, alignment);
#ifdef SUPERALLOC_DEBUG
                nmem::memset(ptr, 0xCDCDCDCD, (u64)size);
#endif
                return ptr;
            }

            void alloc_t::v_deallocate(void* ptr)
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
                static const u16 NIL16 = 0xffff;

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
                        item_index = NIL;
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
                binmap8_t     m_block_free_binmap;

                void     initialize(void* address, s8 section_size_shift, u8 section_index);
                void     deinitialize(nsuperheap::alloc_t* heap);
                void     checkout(nsuperheap::alloc_t* heap, void* address, s8 section_size_shift, u8 section_index, blockconfig_t const& blockcfg);
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
                m_block_config     = {0};
                m_block_free_index = 0;
                m_block_max_count  = 0;
                m_block_used_count = 0;
                m_block_array      = nullptr;
                //g_clear(&m_block_free_binmap);
                m_block_free_binmap.clear();
            }

            void section_t::checkout(nsuperheap::alloc_t* heap, void* address, s8 section_size_shift, u8 section_index, blockconfig_t const& blockcfg)
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
                //g_setup_used(&m_block_free_binmap);
                m_block_free_binmap.setup_used();

                //nvmem::commit(m_address, ((u32)1 << m_block_config.m_sizeshift) * m_block_max_count);
                v_alloc_commit(m_address, ((u32)1 << m_block_config.m_sizeshift) * m_block_max_count);
            }

            void section_t::deinitialize(nsuperheap::alloc_t* heap)
            {
                heap->deallocate(m_block_array);
                //nvmem::decommit(m_address, ((u32)1 << m_block_config.m_sizeshift) * m_block_max_count);
                v_alloc_decommit(m_address, ((u32)1 << m_block_config.m_sizeshift) * m_block_max_count);
            }

            block_t* section_t::checkout_block(allocconfig_t const& alloccfg)
            {
                // Get a block and initialize it for this size
                s32 free_block_index = m_block_free_binmap.find_and_set( m_block_max_count);
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
                m_block_free_binmap.clr( m_block_max_count, block->m_section_block_index);
            }

            class alloc_t : public ncore::alloc_t
            {
            public:
                void         initialize(nsuperheap::alloc_t* heap, u64 address_range, u32 section_size);
                void         deinitialize(nsuperheap::alloc_t* heap);
                u32          sizeof_alloc(u32 size) const;
                inline void* base_address() const { return m_address_base; }

                DCORE_CLASS_PLACEMENT_NEW_DELETE

                void* idx2ptr(u32 item_index) const { return v_idx2ptr(item_index); }
                u32   ptr2idx(void const* ptr) const { return v_ptr2idx(ptr); }

            private:
                block_t* checkout_block(allocconfig_t const& alloccfg);

                struct item_t
                {
                    u32   m_index;  // format is u32[u8(segment-index):u24(item-index)]
                    void* m_ptr;
                };
                item_t alloc_item(u32 alloc_size);
                void   dealloc_item(u32 item_index);
                void   dealloc_item_ptr(void* item_ptr);

                void* v_allocate(u32 size, u32 alignment)
                {
                    item_t item = alloc_item(size);
                    return item.m_ptr;
                }

                void v_deallocate(void* ptr) { dealloc_item_ptr(ptr); }

                void* v_idx2ptr(u32 i) const
                {
                    if (i == NIL)
                        return nullptr;
                    u32 const c = (i >> 24) & 0xFF;
                    ASSERT(c < m_sections_array_size);
                    return m_sections[c].idx2ptr(i);
                }

                u32 v_ptr2idx(void const* ptr) const
                {
                    if (ptr == nullptr)
                        return NIL;
                    u32 const section_index = (u32)(todistance(m_address_base, ptr) >> m_section_maxsize_shift);
                    ASSERT(section_index < m_sections_array_size);
                    u32 const idx = ((section_index & 0xFF) << 24) | m_sections[section_index].ptr2idx(ptr);
                    return idx;
                }

                static const allocconfig_t c_aalloc_config[];
                static const blockconfig_t c_ablock_config[];
                static const s16           c_max_num_blocks;
                static const s16           c_max_num_sizes;

                static inline allocconfig_t const& alloc_size_to_alloc_config(u32 alloc_size)
                {
                    alloc_size = (alloc_size + 7) & ~7;
                    alloc_size = math::g_ceilpo2(alloc_size);
                    s8 const c = math::g_countTrailingZeros(alloc_size) - 3;
                    return c_aalloc_config[c];
                }

                nsuperheap::alloc_t* m_heap;
                void*                m_address_base;
                u64                  m_address_range;
                s8                   m_section_maxsize_shift;
                u32                  m_sections_array_size;
                section_t*           m_sections;
                u32                  m_sections_free_index;
                binmap8_t            m_sections_free_binmap;
                binmap8_t*           m_active_section_binmap_per_blockcfg;  // segments that still have available free blocks
                block_t**            m_active_block_list_per_allocsize;     // blocks that still have available free items, a list per allocation size
            };

            // blockconfig-index, block-size-shift
            const static blockconfig_t c64KB  = {0, 16};
            const static blockconfig_t c256KB = {1, 18};
            const static blockconfig_t c1MB   = {2, 20};
            const static blockconfig_t c4MB   = {3, 22};

            const blockconfig_t alloc_t::c_ablock_config[] = {c64KB, c256KB, c1MB, c4MB};
            const allocconfig_t alloc_t::c_aalloc_config[] = {
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

            const s16 alloc_t::c_max_num_blocks = (s16)(sizeof(c_ablock_config) / sizeof(blockconfig_t));
            const s16 alloc_t::c_max_num_sizes  = (s16)(sizeof(c_aalloc_config) / sizeof(allocconfig_t));

            void alloc_t::initialize(nsuperheap::alloc_t* heap, u64 address_range, u32 section_size)
            {
                m_heap                  = heap;
                m_address_range         = address_range;
                m_section_maxsize_shift = math::g_ilog2(section_size);
                m_sections_array_size   = (u32)(address_range >> m_section_maxsize_shift);
                m_sections              = g_allocate_array<section_t>(m_heap, m_sections_array_size);

                ASSERT(m_sections_array_size <= 256);

                m_sections_free_index = 0;
                m_sections_free_binmap.setup_used();

                //nvmem::nprotect::value_t const attributes = nvmem::nprotect::ReadWrite;
                //nvmem::reserve(address_range, attributes, m_address_base);
                m_address_base = v_alloc_reserve(address_range);

                void* section_address = m_address_base;
                for (u32 i = 0; i < m_sections_array_size; i++)
                {
                    m_sections[i].initialize(section_address, section_size, i);
                    section_address = toaddress(section_address, section_size);
                }

                m_active_section_binmap_per_blockcfg = g_allocate_array<binmap8_t>(m_heap, c_max_num_blocks);
                for (u32 i = 0; i < c_max_num_blocks; i++)
                {
                    m_active_section_binmap_per_blockcfg[i].setup_used();
                }

                m_active_block_list_per_allocsize = g_allocate_array<block_t*>(m_heap, c_max_num_sizes);
                for (u32 i = 0; i < c_max_num_sizes; i++)
                    m_active_block_list_per_allocsize[i] = nullptr;
            }

            void alloc_t::deinitialize(nsuperheap::alloc_t* heap)
            {
                for (u32 i = 0; i < m_sections_free_index; i++)
                    m_sections[i].deinitialize(heap);

                heap->deallocate(m_active_section_binmap_per_blockcfg);
                heap->deallocate(m_sections);

                //nvmem::release(m_address_base, m_address_range);
                v_alloc_release(m_address_base, m_address_range);
            }

            u32 alloc_t::sizeof_alloc(u32 alloc_size) const { return math::g_ceilpo2((alloc_size + (8 - 1)) & ~(8 - 1)); }

            block_t* alloc_t::checkout_block(allocconfig_t const& alloccfg)
            {
                s16 const            config_index = alloccfg.m_index;
                blockconfig_t const& blockcfg     = c_ablock_config[alloccfg.m_block.m_index];

                binmap8_t& bm = m_active_section_binmap_per_blockcfg[alloccfg.m_block.m_index];
                s32 section_index = bm.find(m_sections_array_size);
                if (section_index < 0)
                {
                    section_index = m_sections_free_binmap.find_and_set( m_sections_array_size);
                    if (section_index < 0)
                    {
                        if (m_sections_free_index < m_sections_array_size)
                        {
                            section_index = m_sections_free_index;
                            m_sections_free_index++;
                        }
                        else
                        {
                            ASSERT(false);  // panic
                            return nullptr;
                        }
                    }
                    bm.clr( section_index, m_sections_array_size);

                    section_t* segment         = &m_sections[section_index];
                    void*      section_address = toaddress(m_address_base, (s64)section_index << m_section_maxsize_shift);
                    segment->checkout(m_heap, section_address, m_section_maxsize_shift, section_index, blockcfg);
                }

                ASSERT(section_index >= 0 && section_index < (s32)m_sections_array_size);
                section_t* segment = &m_sections[section_index];
                block_t*   block   = segment->checkout_block(alloccfg);
                if (segment->is_empty())
                {
                    // Segment is empty, remove it from the active set for this block config
                    bm.set( section_index, m_sections_array_size);
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
                ASSERT(alloc_size <= ((u32)1 << alloccfg.m_sizeshift));
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
                u32 const  section_index = block->m_section_index;
                section_t* segment       = &m_sections[section_index];

                alloc.m_ptr = block->allocate_item(segment->address_of_block(block->m_section_block_index), alloc.m_index);
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
                ASSERT(section_index < m_sections_array_size && section_index <= 0xFE);
                ASSERT(block->m_section_block_index < m_sections[section_index].m_block_max_count && block->m_section_block_index <= 0xFE);
                return alloc;
            }

            void alloc_t::dealloc_item(u32 item_index)
            {
                u32 const section_index = (item_index >> 24) & 0xFF;
                u32 const block_index   = (item_index >> 16) & 0xFF;
                u32 const elem_index    = item_index & 0xFFFF;
                ASSERT(section_index < m_sections_array_size);
                section_t* segment = &m_sections[section_index];
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
                    block_t*& head = m_active_block_list_per_allocsize[alloc_config_index];
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

            void alloc_t::dealloc_item_ptr(void* item_ptr)
            {
                u32 const item_index = ptr2idx(item_ptr);
                dealloc_item(item_index);
            }
        }  // namespace nsuperfsa

        // superspace manages sections.
        // every section is *dedicated* to a particular chunk size and holds an array of chunks.
        //
        // functionality:
        //   - checkout chunk
        //     - Handling the request of a new chunk, either creating one or taking one from the cache
        //   - release chunk
        //     - Quickly find section_t*, and chunk_t* that belong to a 'void* ptr'
        //     - Collecting a now empty-chunk and either release or cache it
        //   - checkout section
        //   - release section
        //   set/get tag for a pointer
        //    - Set an associated value for a pointer
        //    - Get the associated value for a pointer
        //
        namespace nsuperspace
        {
            struct section_t;

            struct chunk_t  //
            {
                u16         m_elem_used_count;      // The number of elements used in this chunk
                u16         m_elem_free_index;      // The index of the first free chunk (used to quickly take a free element)
                s16         m_bin_index;            // The index of the bin that this chunk is used for
                u16         m_section_chunk_index;  // index of this chunk in its section
                u32         m_physical_pages;       // number of physical pages that this chunk has committed
                u32         m_padding[3];           // padding
                section_t*  m_section;              // The section that this chunk belongs to
                u32*        m_elem_tag_array;       // index to an array which we use for set_tag/get_tag
                binmap12_t* m_elem_free_binmap;     // The binmap marking free elements for this chunk
                chunk_t*    m_next;                 // next/prev for the doubly linked list
                chunk_t*    m_prev;                 // next/prev for the doubly linked list

                void clear()
                {
                    m_elem_used_count     = 0;
                    m_elem_free_index     = 0;
                    m_section_chunk_index = 0;
                    m_bin_index           = 0;
                    m_section             = nullptr;
                    m_physical_pages      = 0;
                    m_elem_tag_array      = nullptr;
                    m_elem_free_binmap    = nullptr;
                    m_next                = nullptr;
                    m_prev                = nullptr;
                }
            };

            struct section_t  // 64 bytes
            {
                section_t*    m_next;                 // managing a list of active, cached or used sections
                section_t*    m_prev;                 //
                chunk_t**     m_chunk_array;          // array of chunk
                chunk_t*      m_chunks_cached_list;   // list of cached chunks
                binmap12_t*   m_chunks_free_binmap;   // binmap of free chunks
                void*         m_section_address;      // The address of the section
                u16           m_chunks_free_index;    // index of the first free chunk in the array
                u16           m_count_chunks_cached;  // number of chunks that are cached
                u16           m_count_chunks_used;    // number of chunks that are in use
                u16           m_count_chunks_max;     // maximum number of chunks that can be used in this segment
                u32           m_padding;              // padding
                chunkconfig_t m_chunk_config;         // chunk config

                void clear()
                {
                    m_next                = nullptr;
                    m_prev                = nullptr;
                    m_chunk_array         = nullptr;
                    m_chunks_cached_list  = nullptr;
                    m_chunks_free_binmap  = nullptr;
                    m_section_address     = nullptr;
                    m_chunks_free_index   = 0;
                    m_count_chunks_cached = 0;
                    m_count_chunks_used   = 0;
                    m_count_chunks_max    = 0;
                }
            };

            struct alloc_t
            {
                config_t const* m_config;               //
                byte*           m_address_base;         //
                u64             m_address_range;        //
                u32             m_used_physical_pages;  // The number of pages that are currently committed
                s8              m_page_size_shift;      //

                // Chunks
                chunk_t** m_chunk_active_array;  // These are chunk list heads, one list head per chunk type

                // Sections
                section_t**                  m_section_active_array;     // This needs to be per section config
                s8                           m_section_minsize_shift;    // The minimum size of a section in log2
                s8                           m_section_maxsize_shift;    // 1 << m_section_maxsize_shift = segment size
                nsegmented::segment_alloc_t* m_section_allocator;        // Allocator for obtaining a new section with a power-of-two size
                u16*                         m_section_map;              // This a full memory mapping of index to section_t* (16 bits)
                u32                          m_sections_array_capacity;  // The capacity of sections array
                u32                          m_sections_free_index;      // Lower bound index of free sections
                section_t*                   m_section_free_list;        // List of free sections
                section_t*                   m_sections_array;           // Array of sections ()

                DCORE_CLASS_PLACEMENT_NEW_DELETE

                alloc_t()
                    : m_config(nullptr)
                    , m_address_base(nullptr)
                    , m_address_range(0)
                    , m_used_physical_pages(0)
                    , m_page_size_shift(0)
                    , m_chunk_active_array(nullptr)
                    , m_section_active_array(nullptr)
                    , m_section_minsize_shift(0)
                    , m_section_maxsize_shift(0)
                    , m_section_map(nullptr)
                    , m_sections_array_capacity(0)
                    , m_sections_free_index(0)
                    , m_section_free_list(nullptr)
                    , m_sections_array(nullptr)
                {
                }

                void initialize(config_t const* config, nsuperheap::alloc_t* heap, nsuperfsa::alloc_t* fsa)
                {
                    ASSERT(math::g_ispo2(config->m_total_address_size));

                    m_address_range                     = config->m_total_address_size;
                    // nvmem::nprotect::value_t attributes = nvmem::nprotect::ReadWrite;
                    // nvmem::reserve(m_address_range, attributes, (void*&)m_address_base);
                    m_address_base = (byte*)v_alloc_reserve(m_address_range);
                    //const u32 page_size       = nvmem::get_page_size();
                    const u32 page_size       = v_alloc_get_page_size();
                    m_section_active_array    = g_allocate_array_and_clear<section_t*>(heap, config->m_num_chunkconfigs);
                    m_chunk_active_array      = g_allocate_array_and_clear<chunk_t*>(heap, config->m_num_chunkconfigs);
                    m_config                  = config;
                    m_used_physical_pages     = 0;
                    m_page_size_shift         = math::g_ilog2(page_size);
                    m_section_minsize_shift   = config->m_section_minsize_shift;
                    m_section_maxsize_shift   = config->m_section_maxsize_shift;
                    m_section_map             = g_allocate_array_and_memset<u16>(heap, (u32)(m_address_range >> m_section_minsize_shift), 0xFFFFFFFF);
                    m_sections_array_capacity = (u32)(m_address_range >> m_section_maxsize_shift);  // @note: This should be coming from configuration
                    m_sections_free_index     = 0;
                    m_section_free_list       = nullptr;
                    m_sections_array          = g_allocate_array_and_clear<section_t>(heap, m_sections_array_capacity);

                    m_section_allocator = nsegmented::g_create_segment_n_allocator(heap, (int_t)1 << m_section_minsize_shift, (int_t)1 << m_section_maxsize_shift, (int_t)m_address_range);
                }

                void deinitialize(nsuperheap::alloc_t* heap)
                {
                    //nvmem::release(m_address_base, m_address_range);
                    v_alloc_release(m_address_base, m_address_range);

                    heap->deallocate(m_section_active_array);
                    heap->deallocate(m_chunk_active_array);
                    heap->deallocate(m_sections_array);

                    m_address_base          = nullptr;
                    m_address_range         = 0;
                    m_page_size_shift       = 0;
                    m_section_maxsize_shift = 0;
                    m_used_physical_pages   = 0;
                    m_config                = nullptr;
                }

                inline static u32 s_chunk_physical_pages(binconfig_t const& bin, s8 page_size_shift) { return (u32)((bin.m_alloc_size * bin.m_max_alloc_count) + (((u64)1 << page_size_shift) - 1)) >> page_size_shift; }

                chunk_t* checkout_chunk(binconfig_t const& bin, nsuperfsa::alloc_t* fsa)
                {
                    chunk_t* chunk = nullptr;

                    // Get the section for this chunk (note: a section is locked to a certain chunk size)
                    section_t* section = ll_pop(m_section_active_array[bin.m_chunk_config.m_chunkconfig_index]);
                    if (section == nullptr)
                    {
                        section = checkout_section(bin.m_chunk_config, fsa);
                    }

                    u32 const required_physical_pages = s_chunk_physical_pages(bin, m_page_size_shift);
                    u32       already_committed_pages = 0;

                    // We have a section, now obtain a chunk from this section
                    if (section->m_count_chunks_cached > 0)
                    {
                        section->m_count_chunks_cached -= 1;
                        chunk                   = ll_pop(section->m_chunks_cached_list);
                        already_committed_pages = chunk->m_physical_pages;
                    }
                    else
                    {
                        chunk = g_allocate<chunk_t>(fsa);
                        chunk->clear();

                        s32 const section_chunk_index = section->m_chunks_free_binmap->find_and_set(section->m_count_chunks_max);
                        if (section_chunk_index >= 0)
                        {
                            section->m_chunk_array[section_chunk_index] = chunk;
                            chunk->m_section_chunk_index                = (u16)section_chunk_index;
                        }
                        else
                        {
                            section->m_chunks_free_binmap->tick_used_lazy(section->m_count_chunks_max, section->m_chunks_free_index);
                            chunk->m_section_chunk_index                         = section->m_chunks_free_index;
                            section->m_chunk_array[section->m_chunks_free_index] = chunk;
                            section->m_chunks_free_index += 1;
                        }
                    }

                    {  // Initialize the chunk
                        chunk->m_section        = section;
                        chunk->m_bin_index      = bin.m_index;                                        // The bin configuration
                        chunk->m_elem_tag_array = g_allocate_array<u32>(fsa, bin.m_max_alloc_count);  // Allocate allocation tag array

                        // Allocate and initialize the binmap for tracking free elements.
                        // We are initializing the binmap with all elements being used, since
                        // we are relying on the chunk->m_elem_free_index to quickly give us a
                        // free element. We are lazy initializing the binmap to avoid the cost of
                        // fully initializing the binmap with all elements being free, so this is
                        // mainly for performance reasons.
                        chunk->m_elem_free_binmap = g_allocate<binmap12_t>(fsa);
                        chunk->m_elem_free_binmap->setup_used_lazy(fsa,  bin.m_max_alloc_count);
                    }

                    // Make sure that only the required physical pages are committed
                    if (required_physical_pages < already_committed_pages)
                    {
                        // Overcommitted, uncommit tail pages
                        void* address = chunk_to_address(chunk);
                        address       = toaddress(address, (u64)required_physical_pages << m_page_size_shift);
                        //nvmem::decommit(address, ((u64)1 << m_page_size_shift) * (u64)(already_committed_pages - required_physical_pages));
                        v_alloc_decommit(address, ((u64)1 << m_page_size_shift) * (u64)(already_committed_pages - required_physical_pages));
                        chunk->m_physical_pages = required_physical_pages;
                        m_used_physical_pages -= (already_committed_pages - required_physical_pages);
                    }
                    else if (required_physical_pages > already_committed_pages)
                    {
                        // Undercommitted, commit necessary tail pages
                        void* address = chunk_to_address(chunk);
                        address       = toaddress(address, (u64)already_committed_pages << m_page_size_shift);
                        //nvmem::commit(address, ((u64)1 << m_page_size_shift) * (u64)(required_physical_pages - already_committed_pages));
                        v_alloc_commit(address, ((u64)1 << m_page_size_shift) * (u64)(required_physical_pages - already_committed_pages));
                        chunk->m_physical_pages = required_physical_pages;
                        m_used_physical_pages += (required_physical_pages - already_committed_pages);
                    }

                    section->m_count_chunks_used += 1;
                    if (section->m_count_chunks_used < section->m_count_chunks_max)
                    {
                        // Section still has free chunks, add it back to the active list
                        ll_insert(m_section_active_array[section->m_chunk_config.m_chunkconfig_index], section);
                    }

                    return chunk;
                }

                void release_chunk(chunk_t* chunk, nsuperfsa::alloc_t* fsa)
                {
                    // See if this segment was full, if so we need to add it back to the list of active segments again so that
                    // we can checkout chunks from it again.
                    section_t* const section = chunk->m_section;
                    if (section->m_count_chunks_used == section->m_count_chunks_max)
                    {
                        ll_insert(m_section_active_array[section->m_chunk_config.m_chunkconfig_index], section);
                    }

                    // Release any resources allocated for this chunk
                    {
                        chunk->m_elem_free_binmap->release(fsa);
                        fsa->deallocate(chunk->m_elem_tag_array);
                        fsa->deallocate(chunk->m_elem_free_binmap);
                        chunk->m_elem_tag_array   = nullptr;
                        chunk->m_elem_free_binmap = nullptr;

                        chunk->m_bin_index       = -1;
                        chunk->m_elem_used_count = 0;
                        chunk->m_elem_free_index = 0;
                    }

                    // TODO: Caching of chunks, should we make this part of the configuration where a
                    // chunk/section configuration specifies how many chunks to cache?
                    bool const cache_chunk = true;
                    if (cache_chunk)
                    {
                        // Cache the chunk
                        ll_insert(section->m_chunks_cached_list, chunk);
                        section->m_count_chunks_used -= 1;
                        section->m_count_chunks_cached += 1;
                    }
                    else
                    {
                        // Uncommit the virtual memory of this chunk
                        //nvmem::decommit(chunk_to_address(chunk), ((u32)1 << m_page_size_shift) * chunk->m_physical_pages);
                        v_alloc_decommit(chunk_to_address(chunk), ((u32)1 << m_page_size_shift) * chunk->m_physical_pages);
                        m_used_physical_pages -= chunk->m_physical_pages;

                        // Mark this chunk in the binmap as free
                        section->m_chunks_free_binmap->clr(section->m_count_chunks_max, chunk->m_section_chunk_index);

                        // Deallocate and unregister the chunk
                        section->m_chunk_array[chunk->m_section_chunk_index] = nullptr;
                        fsa->deallocate(chunk);
                        chunk = nullptr;

                        section->m_count_chunks_used -= 1;
                        if (section->m_count_chunks_used == 0)
                        {
                            // This section is now empty, we need to release it
                            release_section(section, fsa);
                        }
                    }
                }

                section_t* checkout_section(chunkconfig_t const& chunk_config, nsuperfsa::alloc_t* fsa)
                {
                    // section allocator, we are allocating number of nodes, where each node has the size
                    // equal to (1 << m_section_minsize_shift) so the number of nodes is equal to size:
                    // size = 'number of nodes' * (1 << m_section_minsize_shift)
                    ASSERT(chunk_config.m_section_sizeshift >= m_section_minsize_shift && chunk_config.m_section_sizeshift <= m_section_maxsize_shift);

                    // num nodes we need to allocatate = (1 << sectionconfig.m_sizeshift) / (1 << m_section_minsize_shift)
                    s64 section_ptr  = 0;
                    s64 section_size = 1 << chunk_config.m_section_sizeshift;
                    m_section_allocator->allocate(section_size, section_ptr);

                    section_t* section = ll_pop(m_section_free_list);
                    if (section == nullptr)
                    {
                        section = &m_sections_array[m_sections_free_index++];
                    }
                    section->clear();
                    section->m_section_address     = m_address_base + section_ptr;
                    u32 const section_chunk_count  = (1 << (chunk_config.m_section_sizeshift - chunk_config.m_sizeshift));
                    section->m_chunk_array         = g_allocate_array<chunk_t*>(fsa, section_chunk_count);
                    section->m_chunks_free_index   = 0;
                    section->m_chunks_cached_list  = nullptr;
                    section->m_chunks_free_binmap  = g_allocate<binmap12_t>(fsa);
                    section->m_count_chunks_cached = 0;
                    section->m_count_chunks_used   = 0;
                    section->m_count_chunks_max    = section_chunk_count;
                    section->m_chunk_config        = chunk_config;

                    // Allocate and initialize the binmap for tracking free chunks in this section.
                    // We are initializing the binmap with all elements being used, since
                    // we are relying on the section->m_chunks_free_index to quickly give us a
                    // free chunk. We are lazy initializing this binmap to avoid the cost of
                    // fully initializing the binmap with all elements being free, so this is
                    // mainly for performance reasons.
                    section->m_chunks_free_binmap->setup_used_lazy(fsa,  section_chunk_count);

                    // How many nodes do we span in the full mapping, based on our section size.
                    // For that whole span we need to fill in our section index, so that the
                    // deallocation can quickly obtain the section_t* for a given memory pointer.
                    u32 const node_index    = section_ptr >> m_section_minsize_shift;
                    u32 const node_count    = (u32)(1 << (chunk_config.m_section_sizeshift - m_section_minsize_shift));
                    u32 const section_index = (u32)(section - m_sections_array);
                    for (u32 o = 0; o < node_count; o++)
                        m_section_map[node_index + o] = section_index;

                    return section;
                }

                void release_section(section_t* section, nsuperfsa::alloc_t* fsa)
                {
                    ASSERT(section->m_count_chunks_used == 0);

                    // Remove this section from the active set for that chunk size
                    ll_remove(m_section_active_array[section->m_chunk_config.m_chunkconfig_index], section);

                    // TODO: Caching of sections
                    // Maybe we should cache at least one section instance otherwise a single
                    // alloc/dealloc can checkout and release a section every time?

                    // Release all cached chunks in this section
                    // Note: Is it possible to decommit the full section range in one call?
                    while (section->m_count_chunks_cached > 0)
                    {
                        chunk_t*  chunk               = ll_pop(section->m_chunks_cached_list);
                        u32 const section_chunk_index = chunk->m_section_chunk_index;

                        section->m_chunks_free_binmap->clr(section->m_count_chunks_max, section_chunk_index);
                        //nvmem::decommit(chunk_to_address(chunk), ((u32)1 << m_page_size_shift) * chunk->m_physical_pages);
                        v_alloc_decommit(chunk_to_address(chunk), ((u32)1 << m_page_size_shift) * chunk->m_physical_pages);

                        {  // release resources allocated for this chunk
                            chunk->m_elem_free_binmap->release(fsa);
                            fsa->deallocate(chunk->m_elem_tag_array);
                            fsa->deallocate(chunk->m_elem_free_binmap);
                            chunk->m_elem_tag_array   = nullptr;
                            chunk->m_elem_free_binmap = nullptr;
                        }
                        fsa->deallocate(chunk);
                        section->m_chunk_array[section_chunk_index] = nullptr;
                        section->m_count_chunks_cached -= 1;
                    }

                    g_deallocate_array(fsa, section->m_chunk_array);
                    section->m_chunks_free_binmap->release(fsa);

                    // Deallocate the memory segment that was associated with this section
                    // u16 const node = (u16)((todistance(m_address_base, section->m_section_address) >> m_section_minsize_shift) & 0xFFFFFFFF);
                    // m_section_allocator.deallocate(node);
                    s64       section_ptr  = todistance(m_address_base, section->m_section_address);
                    const s64 section_size = 1 << section->m_chunk_config.m_section_sizeshift;
                    m_section_allocator->deallocate(section_ptr, section_size);

                    // Clear our index from the section map array
                    u32 const node        = (u32)(section_ptr >> m_section_minsize_shift);
                    u32 const node_offset = (u32)node;
                    u32 const node_count  = (u32)(1 << (section->m_chunk_config.m_section_sizeshift - m_section_minsize_shift));
                    for (u32 o = 0; o < node_count; o++)
                        m_section_map[node_offset + o] = 0xFFFF;

                    // We would like to keep the array of sections contiguous, so we are inserting
                    // this section into the list of free sections. When a new section is needed
                    // we use this list to give as an instance of a section.
                    section->clear();
                    ll_insert(m_section_free_list, section);
                }

                void set_tag(void* ptr, u32 assoc)
                {
                    ASSERT(ptr >= m_address_base && ptr < ((u8*)m_address_base + m_address_range));
                    chunk_t* chunk = address_to_chunk(ptr);
                    ASSERT(chunk->m_bin_index < m_config->m_num_binconfigs);
                    binconfig_t const& bin = m_config->m_abinconfigs[chunk->m_bin_index];

                    u32*        elem_tag_array       = chunk->m_elem_tag_array;
                    void* const chunk_address        = chunk_to_address(chunk);
                    u32 const   chunk_item_index     = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
                    elem_tag_array[chunk_item_index] = assoc;
                }

                u32 get_tag(void* ptr) const
                {
                    ASSERT(ptr >= m_address_base && ptr < ((u8*)m_address_base + m_config->m_total_address_size));
                    chunk_t const* chunk     = address_to_chunk(ptr);
                    s16 const      sbinindex = chunk->m_bin_index;
                    ASSERT(sbinindex < m_config->m_num_binconfigs);
                    binconfig_t const& bin                 = m_config->m_abinconfigs[sbinindex];
                    void* const        chunk_address       = chunk_to_address(chunk);
                    u32 const          chunk_element_index = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
                    return chunk->m_elem_tag_array[chunk_element_index];
                }

                inline chunk_t* address_to_chunk(void* ptr) const
                {
                    u32 const mapped_index = (u32)((todistance(m_address_base, ptr) >> m_section_minsize_shift) & 0xFFFFFFFF);
                    ASSERT(mapped_index < m_sections_array_capacity);
                    u32 const section_mapped_index = m_section_map[mapped_index];
                    ASSERT(section_mapped_index != 0xFFFF && section_mapped_index < m_sections_free_index);
                    section_t* const section             = &m_sections_array[section_mapped_index];
                    u32 const        section_chunk_index = (u32)(todistance(section->m_section_address, ptr) >> (section->m_chunk_config.m_sizeshift));
                    chunk_t*         chunk               = section->m_chunk_array[section_chunk_index];
                    if (chunk == nullptr || chunk == (void*)0xCDCDCDCDCDCDCDCDul)
                    {
                        ASSERT(false);
                    }
                    return chunk;
                }

                inline void* chunk_to_address(chunk_t const* chunk) const
                {
                    s8 const  chunk_sizeshift = m_config->m_abinconfigs[chunk->m_bin_index].m_chunk_config.m_sizeshift;
                    u64 const chunk_offset    = ((u64)chunk->m_section_chunk_index << chunk_sizeshift);
                    return toaddress(chunk->m_section->m_section_address, chunk_offset);
                }
            };
        }  // namespace nsuperspace

        class superalloc_t : public vmalloc_t
        {
        public:
            config_t const*        m_config;
            nsuperheap::alloc_t*   m_internal_heap;
            nsuperfsa::alloc_t*    m_internal_fsa;
            nsuperspace::alloc_t*  m_superspace;
            nsuperspace::chunk_t** m_active_chunk_list_per_bin;
            alloc_t*               m_main_allocator;

            superalloc_t(alloc_t* main_allocator)
                : m_config(nullptr)
                , m_internal_heap()
                , m_internal_fsa()
                , m_superspace(nullptr)
                , m_active_chunk_list_per_bin(nullptr)
                , m_main_allocator(main_allocator)
            {
            }

            DCORE_CLASS_PLACEMENT_NEW_DELETE

            void initialize(config_t const* config);
            void deinitialize();

            virtual void* v_allocate(u32 size, u32 alignment);
            virtual void  v_deallocate(void* ptr);
            virtual void  v_release() {}

            virtual u32  v_get_size(void* ptr) const final;
            virtual void v_set_tag(void* ptr, u32 assoc) final;
            virtual u32  v_get_tag(void* ptr) const final;
        };

        void superalloc_t::initialize(config_t const* config)
        {
            m_config = config;

            m_internal_heap = new (m_main_allocator->allocate(sizeof(nsuperheap::alloc_t))) nsuperheap::alloc_t();
            m_internal_heap->initialize(config->m_internal_heap_address_range, config->m_internal_heap_pre_size);

            m_internal_fsa = new (m_internal_heap->allocate(sizeof(nsuperfsa::alloc_t))) nsuperfsa::alloc_t();
            m_internal_fsa->initialize(m_internal_heap, config->m_internal_fsa_address_range, config->m_internal_fsa_segment_size);

            m_superspace = new (m_internal_heap->allocate(sizeof(nsuperspace::alloc_t))) nsuperspace::alloc_t();
            m_superspace->initialize(config, m_internal_heap, m_internal_fsa);

            m_active_chunk_list_per_bin = g_allocate_array_and_clear<nsuperspace::chunk_t*>(m_internal_heap, config->m_num_binconfigs);
            for (s16 i = 0; i < config->m_num_binconfigs; i++)
                m_active_chunk_list_per_bin[i] = nullptr;
        }

        void superalloc_t::deinitialize()
        {
            m_superspace->deinitialize(m_internal_heap);
            m_internal_heap->deallocate(m_superspace);

            m_internal_fsa->deinitialize(m_internal_heap);
            m_internal_heap->deallocate(m_internal_fsa);

            m_internal_heap->deinitialize();
            m_main_allocator->deallocate(m_internal_heap);

            m_config         = nullptr;
            m_superspace     = nullptr;
            m_internal_fsa   = nullptr;
            m_internal_heap  = nullptr;
            m_main_allocator = nullptr;
        }

        void* superalloc_t::v_allocate(u32 alloc_size, u32 alignment)
        {
            alloc_size             = math::g_alignUp(alloc_size, alignment);
            binconfig_t const& bin = m_config->size2bin(alloc_size);

            nsuperspace::chunk_t* chunk = m_active_chunk_list_per_bin[bin.m_index];
            if (chunk == nullptr)
            {
                chunk = m_superspace->checkout_chunk(bin, m_internal_fsa);
                ll_insert(m_active_chunk_list_per_bin[bin.m_index], chunk);
            }

            ASSERT(chunk->m_bin_index == bin.m_index);
            ASSERT(alloc_size <= bin.m_alloc_size);

            // If we have elements in the binmap, we can use it to get a free element.
            // If not, we need to use free_index to obtain a free element.
            s32 elem_index = chunk->m_elem_free_binmap->find_and_set(bin.m_max_alloc_count);
            if (elem_index < 0)
            {
                elem_index = chunk->m_elem_free_index++;
                chunk->m_elem_free_binmap->tick_used_lazy(bin.m_max_alloc_count, elem_index);
            }
            ASSERT(elem_index < (s32)bin.m_max_alloc_count);

            // Initialize the tag value for this element
            chunk->m_elem_tag_array[elem_index] = 0;

            chunk->m_elem_used_count += 1;
            if (chunk->m_elem_used_count >= bin.m_max_alloc_count)
            {
                // Chunk is full, so remove it from the list of active chunks
                ll_remove(m_active_chunk_list_per_bin[bin.m_index], chunk);
            }

            void* chunk_address = m_superspace->chunk_to_address(chunk);
            void* item_ptr      = toaddress(chunk_address, (u64)elem_index * bin.m_alloc_size);
            ASSERT(item_ptr >= m_superspace->m_address_base && item_ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
            return item_ptr;
        }

        void superalloc_t::v_deallocate(void* ptr)
        {
            if (ptr == nullptr)
                return;

            ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));

            nsuperspace::chunk_t* chunk = m_superspace->address_to_chunk(ptr);
            binconfig_t const&    bin   = m_config->m_abinconfigs[chunk->m_bin_index];

            {
                void* const chunk_address = m_superspace->chunk_to_address(chunk);
                u32 const   elem_index    = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
                ASSERT(elem_index < chunk->m_elem_free_index && elem_index < bin.m_max_alloc_count);
                binmap12_t* bm = chunk->m_elem_free_binmap;
                bm->clr(bin.m_max_alloc_count, elem_index);
                u32* elem_tag_array = chunk->m_elem_tag_array;
                if (elem_tag_array[elem_index] == 0xFEFEEFEE)  // Double freeing this element ?
                {
                    ASSERT(false);
                    return;
                }
                elem_tag_array[elem_index] = 0xFEFEEFEE;  // Clear the tag for this element (mark it as freed)
            }

            // We have deallocated an element from this chunk
            const bool chunk_was_full = (bin.m_max_alloc_count == chunk->m_elem_used_count);
            chunk->m_elem_used_count--;
            const bool chunk_is_empty = (0 == chunk->m_elem_used_count);

            // Check the state of this chunk, was it full before we deallocated an element?
            // Or maybe now it has become empty ?
            if (chunk_is_empty)
            {
                if (!chunk_was_full)
                {
                    // We are going to release this chunk, so remove it from the active chunk list before doing that.
                    ll_remove(m_active_chunk_list_per_bin[bin.m_index], chunk);
                }
                m_superspace->release_chunk(chunk, m_internal_fsa);
            }
            else if (chunk_was_full)
            {
                // Ok, this chunk can be used to allocate from again, so add it to the list of active chunks
                ll_insert(m_active_chunk_list_per_bin[bin.m_index], chunk);
            }
        }

        u32 superalloc_t::v_get_size(void* ptr) const
        {
            if (ptr == nullptr)
                return 0;
            ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
            nsuperspace::chunk_t* chunk = m_superspace->address_to_chunk(ptr);
            binconfig_t const&    bin   = m_config->m_abinconfigs[chunk->m_bin_index];
            return bin.m_alloc_size;
        }

        void superalloc_t::v_set_tag(void* ptr, u32 assoc)
        {
            if (ptr != nullptr)
                m_superspace->set_tag(ptr, assoc);
        }

        u32 superalloc_t::v_get_tag(void* ptr) const { return (ptr == nullptr) ? 0xffffffff : m_superspace->get_tag(ptr); }

    }  // namespace nsuperalloc

    nsuperalloc::vmalloc_t* gCreateVmAllocator(alloc_t* main_heap)
    {
        // nsuperalloc::config_t const* config  = nsuperalloc::gConfigWindowsDesktopApp10p();
        nsuperalloc::config_t const* config     = nsuperalloc::gConfigWindowsDesktopApp25p();
        nsuperalloc::superalloc_t*   superalloc = new (main_heap->allocate(sizeof(nsuperalloc::superalloc_t))) nsuperalloc::superalloc_t(main_heap);
        superalloc->initialize(config);
        return superalloc;
    }

    void gDestroyVmAllocator(nsuperalloc::vmalloc_t* valloc)
    {
        nsuperalloc::superalloc_t* superalloc     = static_cast<nsuperalloc::superalloc_t*>(valloc);
        alloc_t*                   main_allocator = superalloc->m_main_allocator;
        superalloc->deinitialize();
        g_destruct(main_allocator, superalloc);
    }

    namespace nvmalloc
    {
        class vmalloc_instance_t
        {
        public:
            nsuperalloc::nsuperspace::alloc_t* m_superspace;      // shared among instances
            nsuperalloc::nsuperheap::alloc_t*  m_internal_heap;   // per instance
            nsuperalloc::nsuperfsa::alloc_t*   m_internal_fsa;    // per instance
            s16                                m_instance_index;  // instance index

            // We need to defer all deallocations here so that we can release them when on the owning thread.
            // Using the allocations as a linked-list is best for this purpose, so that we do not need to
            // allocate memory for a separate array or a separate list.
        };
    }  // namespace nvmalloc

}  // namespace ncore
