#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_binmap1.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"
#include "ccore/c_arena.h"

#include "callocator/c_allocator_segment.h"

#include "csuperalloc/private/c_list.h"
#include "csuperalloc/c_fsa.h"
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
                u16        m_elem_used_count;      // The number of elements used in this chunk
                u16        m_elem_free_index;      // The index of the first free chunk (used to quickly take a free element)
                s16        m_bin_index;            // The index of the bin that this chunk is used for
                u16        m_section_chunk_index;  // index of this chunk in its section
                u32        m_physical_pages;       // number of physical pages that this chunk has committed
                u32        m_padding[1];           // padding
                section_t* m_section;              // The section that this chunk belongs to
                u32*       m_elem_tag_array;       // index to an array which we use for set_tag/get_tag
                u64        m_elem_free_bin0;       // nbinmap12, bin0 and bin1 for free elements
                u64*       m_elem_free_bin1;       //
                chunk_t*   m_next;                 // next/prev for the doubly linked list
                chunk_t*   m_prev;                 // next/prev for the doubly linked list

                void clear()
                {
                    m_elem_used_count     = 0;
                    m_elem_free_index     = 0;
                    m_section_chunk_index = 0;
                    m_bin_index           = 0;
                    m_section             = nullptr;
                    m_physical_pages      = 0;
                    m_elem_tag_array      = nullptr;
                    m_elem_free_bin0      = 0;
                    m_elem_free_bin1      = nullptr;
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
                u64           m_chunks_free_bin0;     // binmap of free chunks
                u64*          m_chunks_free_bin1;     //
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
                    m_chunks_free_bin0    = 0;
                    m_chunks_free_bin1    = nullptr;
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
                section_t**     m_section_active_array;     // This needs to be per section config
                s8              m_section_minsize_shift;    // The minimum size of a section in log2
                s8              m_section_maxsize_shift;    // 1 << m_section_maxsize_shift = segment size
                segment_alloc_t m_section_allocator;        // Allocator for obtaining a new section with a power-of-two size
                u16*            m_section_map;              // This a full memory mapping of index to section_t* (16 bits)
                u32             m_sections_array_capacity;  // The capacity of sections array
                u32             m_sections_free_index;      // Lower bound index of free sections
                section_t*      m_section_free_list;        // List of free sections
                section_t*      m_sections_array;           // Array of sections ()

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

                void initialize(config_t const* config, arena_t* heap, fsa_t* fsa)
                {
                    ASSERT(math::ispo2(config->m_total_address_size));

                    m_address_range           = config->m_total_address_size;
                    m_address_base            = (byte*)v_alloc_reserve(m_address_range);
                    //const u32 page_size       = v_alloc_get_page_size();
                    m_section_active_array    = g_allocate_array_and_clear<section_t*>(heap, config->m_num_chunkconfigs);
                    m_chunk_active_array      = g_allocate_array_and_clear<chunk_t*>(heap, config->m_num_chunkconfigs);
                    m_config                  = config;
                    m_used_physical_pages     = 0;
                    m_page_size_shift         = v_alloc_get_page_size_shift();
                    m_section_minsize_shift   = config->m_section_minsize_shift;
                    m_section_maxsize_shift   = config->m_section_maxsize_shift;
                    m_section_map             = g_allocate_array_and_fill<u16>(heap, (u32)(m_address_range >> m_section_minsize_shift), 0xFFFFFFFF);
                    m_sections_array_capacity = (u32)(m_address_range >> m_section_maxsize_shift);  // @note: This should be coming from configuration
                    m_sections_free_index     = 0;
                    m_section_free_list       = nullptr;
                    m_sections_array          = g_allocate_array_and_clear<section_t>(heap, m_sections_array_capacity);

                    arena_alloc_t heap_alloc(heap);
                    segment_initialize(&m_section_allocator, &heap_alloc, (int_t)1 << m_section_minsize_shift, (int_t)1 << m_section_maxsize_shift, (int_t)m_address_range);
                }

                void deinitialize(arena_t* heap)
                {
                    v_alloc_release(m_address_base, m_address_range);

                    g_deallocate(heap, m_section_active_array);
                    g_deallocate(heap, m_chunk_active_array);
                    g_deallocate(heap, m_sections_array);

                    m_address_base          = nullptr;
                    m_address_range         = 0;
                    m_page_size_shift       = 0;
                    m_section_maxsize_shift = 0;
                    m_used_physical_pages   = 0;
                    m_config                = nullptr;
                }

                inline static u32 s_chunk_physical_pages(binconfig_t const& bin, s8 page_size_shift) { return (u32)((bin.m_alloc_size * bin.m_max_alloc_count) + (((u64)1 << page_size_shift) - 1)) >> page_size_shift; }

                chunk_t* checkout_chunk(binconfig_t const& bin, fsa_t* fsa)
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

                        s32 const section_chunk_index = nbinmap12::find_and_set(&section->m_chunks_free_bin0, section->m_chunks_free_bin1, section->m_count_chunks_max);
                        if (section_chunk_index >= 0)
                        {
                            section->m_chunk_array[section_chunk_index] = chunk;
                            chunk->m_section_chunk_index                = (u16)section_chunk_index;
                        }
                        else
                        {
                            nbinmap12::tick_used_lazy(&section->m_chunks_free_bin0, section->m_chunks_free_bin1, section->m_count_chunks_max, section->m_chunks_free_index);
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
                        chunk->m_elem_free_bin1 = g_allocate_array<u64>(fsa, 8);
                        nbinmap12::setup_used_lazy(&chunk->m_elem_free_bin0, chunk->m_elem_free_bin1, bin.m_max_alloc_count);
                    }

                    // Make sure that only the required physical pages are committed
                    if (required_physical_pages < already_committed_pages)
                    {
                        // Overcommitted, uncommit tail pages
                        void* address = chunk_to_address(chunk);
                        address       = toaddress(address, (u64)required_physical_pages << m_page_size_shift);
                        v_alloc_decommit(address, ((u64)1 << m_page_size_shift) * (u64)(already_committed_pages - required_physical_pages));
                        chunk->m_physical_pages = required_physical_pages;
                        m_used_physical_pages -= (already_committed_pages - required_physical_pages);
                    }
                    else if (required_physical_pages > already_committed_pages)
                    {
                        // Undercommitted, commit necessary tail pages
                        void* address = chunk_to_address(chunk);
                        address       = toaddress(address, (u64)already_committed_pages << m_page_size_shift);
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

                void release_chunk(chunk_t* chunk, fsa_t* fsa)
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
                        nfsa::deallocate(fsa, chunk->m_elem_tag_array);
                        nfsa::deallocate(fsa, chunk->m_elem_free_bin1);
                        chunk->m_elem_tag_array = nullptr;
                        chunk->m_elem_free_bin1 = nullptr;

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
                        v_alloc_decommit(chunk_to_address(chunk), ((u32)1 << m_page_size_shift) * chunk->m_physical_pages);
                        m_used_physical_pages -= chunk->m_physical_pages;

                        // Mark this chunk in the binmap as free
                        nbinmap12::clr(&section->m_chunks_free_bin0, section->m_chunks_free_bin1, section->m_count_chunks_max, chunk->m_section_chunk_index);

                        // Deallocate and unregister the chunk
                        section->m_chunk_array[chunk->m_section_chunk_index] = nullptr;
                        nfsa::deallocate(fsa, chunk);
                        chunk = nullptr;

                        section->m_count_chunks_used -= 1;
                        if (section->m_count_chunks_used == 0)
                        {
                            // This section is now empty, we need to release it
                            release_section(section, fsa);
                        }
                    }
                }

                section_t* checkout_section(chunkconfig_t const& chunk_config, fsa_t* fsa)
                {
                    // section allocator, we are allocating number of nodes, where each node has the size
                    // equal to (1 << m_section_minsize_shift) so the number of nodes is equal to size:
                    // size = 'number of nodes' * (1 << m_section_minsize_shift)
                    ASSERT(chunk_config.m_section_sizeshift >= m_section_minsize_shift && chunk_config.m_section_sizeshift <= m_section_maxsize_shift);

                    // num nodes we need to allocatate = (1 << sectionconfig.m_sizeshift) / (1 << m_section_minsize_shift)
                    s64 section_ptr  = 0;
                    s64 section_size = 1 << chunk_config.m_section_sizeshift;
                    segment_allocate(&m_section_allocator, section_size, section_ptr);

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
                    section->m_chunks_free_bin0    = D_U64_MAX;
                    section->m_chunks_free_bin1    = g_allocate_array<u64>(fsa, 8);
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
                    // section->m_chunks_free_binmap->setup_used_lazy(fsa, section_chunk_count);
                    nbinmap12::setup_used_lazy(&section->m_chunks_free_bin0, section->m_chunks_free_bin1, section_chunk_count);

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

                void release_section(section_t* section, fsa_t* fsa)
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

                        nbinmap12::clr(&section->m_chunks_free_bin0, section->m_chunks_free_bin1, section->m_count_chunks_max, section_chunk_index);
                        v_alloc_decommit(chunk_to_address(chunk), ((u32)1 << m_page_size_shift) * chunk->m_physical_pages);

                        {  // release resources allocated for this chunk
                            nfsa::deallocate(fsa, chunk->m_elem_free_bin1);
                            nfsa::deallocate(fsa, chunk->m_elem_tag_array);
                            chunk->m_elem_tag_array = nullptr;
                            chunk->m_elem_free_bin0 = D_U64_MAX;
                            chunk->m_elem_free_bin1 = nullptr;
                        }
                        nfsa::deallocate(fsa, chunk);
                        section->m_chunk_array[section_chunk_index] = nullptr;
                        section->m_count_chunks_cached -= 1;
                    }

                    g_deallocate_array(fsa, section->m_chunk_array);

                    nfsa::deallocate(fsa, section->m_chunks_free_bin1);
                    section->m_chunks_free_bin1 = nullptr;

                    // Deallocate the memory segment that was associated with this section
                    s64       section_ptr  = todistance(m_address_base, section->m_section_address);
                    const s64 section_size = 1 << section->m_chunk_config.m_section_sizeshift;
                    segment_deallocate(&m_section_allocator, section_ptr, section_size);

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
            arena_t*               m_internal_heap;
            fsa_t*                 m_internal_fsa;
            nsuperspace::alloc_t*  m_superspace;
            nsuperspace::chunk_t** m_active_chunk_list_per_bin;
            alloc_t*               m_main_allocator;

            superalloc_t(alloc_t* main_allocator)
                : m_config(nullptr)
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

            m_internal_heap = narena::new_arena(config->m_internal_heap_address_range, config->m_internal_heap_pre_size);
            m_internal_fsa  = nfsa::new_fsa(config->m_internal_fsa_address_range);

            m_superspace = g_allocate<nsuperspace::alloc_t>(m_internal_heap);
            m_superspace->initialize(config, m_internal_heap, m_internal_fsa);

            m_active_chunk_list_per_bin = g_allocate_array_and_clear<nsuperspace::chunk_t*>(m_internal_heap, config->m_num_binconfigs);
            for (s16 i = 0; i < config->m_num_binconfigs; i++)
                m_active_chunk_list_per_bin[i] = nullptr;
        }

        void superalloc_t::deinitialize()
        {
            m_superspace->deinitialize(m_internal_heap);
            g_deallocate(m_internal_heap, m_superspace);

            nfsa::destroy(m_internal_fsa);
            narena::destroy(m_internal_heap);

            m_config         = nullptr;
            m_superspace     = nullptr;
            m_main_allocator = nullptr;
        }

        void* superalloc_t::v_allocate(u32 alloc_size, u32 alignment)
        {
            alloc_size             = math::alignUp(alloc_size, alignment);
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
            s32 elem_index = nbinmap12::find_and_set(&chunk->m_elem_free_bin0, chunk->m_elem_free_bin1, bin.m_max_alloc_count);
            if (elem_index < 0)
            {
                elem_index = chunk->m_elem_free_index++;
                nbinmap12::tick_used_lazy(&chunk->m_elem_free_bin0, chunk->m_elem_free_bin1, bin.m_max_alloc_count, elem_index);
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
                nbinmap12::clr(&chunk->m_elem_free_bin0, chunk->m_elem_free_bin1, bin.m_max_alloc_count, elem_index);
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
            arena_t                            m_internal_heap;   // per instance
            fsa_t*                             m_internal_fsa;    // per instance
            s16                                m_instance_index;  // instance index

            // We need to defer all deallocations here so that we can release them when on the owning thread.
            // Using the allocations as a linked-list is best for this purpose, so that we do not need to
            // allocate memory for a separate array or a separate list.
        };
    }  // namespace nvmalloc

}  // namespace ncore
