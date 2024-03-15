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
    static inline ptr_t todistance(void* base, void* ptr)
    {
        ASSERT(ptr >= base);
        return (ptr_t)((ptr_t)ptr - (ptr_t)base);
    }

    // Can only allocate, used internally to allocate initially required memory
    class superheap_t
    {
    public:
        void*   m_address;
        u64     m_address_range;
        vmem_t* m_vmem;
        u32     m_allocsize_alignment;
        u32     m_page_size;
        u32     m_page_count_current;
        u32     m_page_count_maximum;
        u64     m_ptr;

        void  initialize(vmem_t* vmem, u64 memory_range, u64 size_to_pre_allocate);
        void  deinitialize();
        void* allocate(u32 size);
    };

    void superheap_t::initialize(vmem_t* vmem, u64 memory_range, u64 size_to_pre_allocate)
    {
        u32 attributes  = 0;
        m_vmem          = vmem;
        m_address_range = memory_range;
        m_vmem->reserve(memory_range, m_page_size, attributes, m_address);
        m_allocsize_alignment = 32;
        m_page_count_maximum  = (u32)(memory_range / m_page_size);
        m_page_count_current  = 0;
        m_ptr                 = 0;

        if (size_to_pre_allocate > 0)
        {
            u32 const pages_to_commit = (u32)(math::alignUp(size_to_pre_allocate, (u64)m_page_size) / m_page_size);
            m_vmem->commit(m_address, m_page_size, pages_to_commit);
            m_page_count_current = pages_to_commit;
        }
    }

    void superheap_t::deinitialize()
    {
        m_vmem->release(m_address, m_address_range);
        m_address             = nullptr;
        m_address_range       = 0;
        m_vmem                = nullptr;
        m_allocsize_alignment = 0;
        m_page_size           = 0;
        m_page_count_current  = 0;
        m_page_count_maximum  = 0;
        m_ptr                 = 0;
    }

    void* superheap_t::allocate(u32 size)
    {
        size        = math::alignUp(size, m_allocsize_alignment);
        u64 ptr_max = ((u64)m_page_count_current * m_page_size);
        if ((m_ptr + size) > ptr_max)
        {
            // add more pages
            u32 const page_count           = (u32)(math::alignUp(m_ptr + size, (u64)m_page_size) / (u64)m_page_size);
            u32 const page_count_to_commit = page_count - m_page_count_current;
            u64       commit_base          = ((u64)m_page_count_current * m_page_size);
            m_vmem->commit(toaddress(m_address, commit_base), m_page_size, page_count_to_commit);
            m_page_count_current += page_count_to_commit;
        }
        u64 const offset = m_ptr;
        m_ptr += size;
        return toaddress(m_address, offset);
    }

    struct superpage_t
    {
        static const u16 NIL = 0xffff;

        u16 m_item_size;
        u16 m_item_count;
        u16 m_item_freepos;
        u16 m_item_freelist;

        void initialize(u32 itemsize)
        {
            ASSERT(itemsize >= 2);
            m_item_size     = itemsize;
            m_item_count    = 0;
            m_item_freepos  = 0;
            m_item_freelist = NIL;
        }

        inline bool is_full(u32 pagesize) const { return m_item_freelist == NIL && (((u32)m_item_freepos + m_item_size) > pagesize); }
        inline bool is_empty() const { return m_item_count == 0; }
        inline u32  ptr2idx(void* const ptr, void* const elem) const { return (u32)(((u64)elem - (u64)ptr) / m_item_size); }
        inline u32* idx2ptr(void* const ptr, u32 const index) const { return (u32*)((u8*)ptr + ((u64)index * m_item_size)); }

        u16 iallocate(void* page_address, u32 pagesize)
        {
            if (m_item_freelist != NIL)
            {
                u16 const  iitem = m_item_freelist;
                u16* const pitem = (u16*)idx2ptr(page_address, iitem);
                m_item_freelist  = pitem[0];
                m_item_count++;
                return iitem;
            }
            else if (((u32)m_item_freepos + m_item_size) <= pagesize)
            {
                u16 const ielem = m_item_freepos / m_item_size;
                m_item_freepos += m_item_size;
                m_item_count++;
                return ielem;
            }
            // panic
            return NIL;
        }

        void deallocate(void* page_address, u16 item_index)
        {
            ASSERT(m_item_count > 0);
            ASSERT(item_index < m_item_freepos);
            u16* const pelem = (u16*)idx2ptr(page_address, item_index);
#ifdef SUPERALLOC_DEBUG
            nmem::memset(pelem, 0xFEFEFEFE, m_item_size);
#endif
            pelem[0]        = m_item_freelist;
            m_item_freelist = item_index;
            m_item_count--;
        }
    };

    struct superpages_t
    {
        vmem_t*      m_vmem;
        void*        m_address;
        u64          m_address_range;
        u32          m_page_count;
        u32          m_page_size;
        superpage_t* m_page_array;
        lldata_t     m_page_list_data;
        llist_t      m_free_page_list;
        llist_t      m_cached_page_list;

        void  initialize(superheap_t& heap, vmem_t* vmem, u64 address_range, u32 size_to_pre_allocate);
        void  deinitialize(superheap_t& heap);
        u32   checkout_page(u32 const alloc_size);
        void  release_page(u32 ipage);
        void* address_of_page(u32 ipage) const { return toaddress(m_address, (u64)ipage * m_page_size); }

        inline void* idx2ptr(u32 i) const
        {
            if (i == 0xffffffff)
                return nullptr;
            u16 const                pageindex = i >> 16;
            u16 const                itemindex = i & 0xFFFF;
            superpage_t const* const ppage     = &m_page_array[pageindex];
            void* const              paddr     = address_of_page(pageindex);
            return ppage->idx2ptr(paddr, itemindex);
        }

        inline u32 ptr2idx(void* ptr) const
        {
            if (ptr == nullptr)
                return 0xffffffff;
            u32 const                pageindex = (u32)(todistance(m_address, ptr) / m_page_size);
            superpage_t const* const ppage     = &m_page_array[pageindex];
            void* const              paddr     = address_of_page(pageindex);
            u32 const                itemindex = ppage->ptr2idx(paddr, ptr);
            return (pageindex << 16) | (itemindex & 0xFFFF);
        }
    };

    void superpages_t::initialize(superheap_t& heap, vmem_t* vmem, u64 address_range, u32 size_to_pre_allocate)
    {
        m_page_size    = 65536;
        m_vmem         = vmem;
        u32 attributes = 0;
        m_vmem->reserve(address_range, m_page_size, attributes, m_address);
        m_address_range = address_range;

        m_page_count = (u32)(address_range / (u64)m_page_size);
        m_page_array = (superpage_t*)heap.allocate(m_page_count * sizeof(superpage_t));
#ifdef SUPERALLOC_DEBUG
        nmem::memset(m_page_array, 0xCDCDCDCD, m_page_count * sizeof(superpage_t));
#endif

        m_page_list_data.m_data     = heap.allocate(m_page_count * sizeof(llnode_t));
        m_page_list_data.m_itemsize = sizeof(llnode_t);
        m_page_list_data.m_pagesize = m_page_count * sizeof(llnode_t);

        u32 const num_pages_to_cache = math::alignUp(size_to_pre_allocate, m_page_size) / m_page_size;
        ASSERT(num_pages_to_cache <= m_page_count);
        m_free_page_list.initialize(m_page_list_data, num_pages_to_cache, m_page_count - num_pages_to_cache, m_page_count);
        if (num_pages_to_cache > 0)
        {
            m_cached_page_list.initialize(m_page_list_data, 0, num_pages_to_cache, num_pages_to_cache);
            m_vmem->commit(m_address, m_page_size, num_pages_to_cache);
        }
    }

    void superpages_t::deinitialize(superheap_t& heap)
    {
        // NOTE: Do we need to decommit physical pages, or is 'release' enough?
        m_vmem->release(m_address, m_address_range);
    }

    u32 superpages_t::checkout_page(u32 const alloc_size)
    {
        // Get a page and initialize that page for this size
        u32 ipage = llnode_t::NIL;
        if (!m_cached_page_list.is_empty())
        {
            ipage = m_cached_page_list.remove_headi(m_page_list_data);
        }
        else if (!m_free_page_list.is_empty())
        {
            ipage       = m_free_page_list.remove_headi(m_page_list_data);
            void* apage = address_of_page(ipage);
            m_vmem->commit(apage, m_page_size, 1);
        }
#ifdef SUPERALLOC_DEBUG
        u64* apage = (u64*)address_of_page(ipage);
        nmem::memset(apage, 0xCDCDCDCD, m_page_size);
#endif
        superpage_t* ppage = &m_page_array[ipage];
        ppage->initialize(alloc_size);
        return ipage;
    }

    void superpages_t::release_page(u32 pageindex)
    {
        superpage_t* const ppage = &m_page_array[pageindex];
#ifdef SUPERALLOC_DEBUG
        nmem::memset(ppage, 0xFEFEFEFE, sizeof(superpage_t));
#endif
        if (!m_cached_page_list.is_full())
        {
            m_cached_page_list.insert(m_page_list_data, pageindex);
        }
        else
        {
            void* const paddr = address_of_page(pageindex);
            m_vmem->decommit(paddr, m_page_size, 1);
            m_free_page_list.insert(m_page_list_data, pageindex);
        }
    }

    // Power-of-2 sizes, minimum size = 8, maximum_size = 65536
    // @note: The format of the returned index is u32[u16(page-index):u16(item-index)]
    class superfsa_t
    {
    public:
        static const u32 NIL = 0xffffffff;

        void initialize(superheap_t& heap, vmem_t* vmem, u64 address_range, u32 size_to_pre_allocate);
        void deinitialize(superheap_t& heap);
        u32  sizeof_alloc(u32 size) const;

        u32  alloc(u32 size);
        void dealloc(u32 index);

        inline void* idx2ptr(u32 i) const { return m_pages.idx2ptr(i); }
        inline u32   ptr2idx(void* ptr) const { return m_pages.ptr2idx(ptr); }

        void* baseptr() const { return m_pages.m_address; }
        u32   pagesize() const { return m_pages.m_page_size; }

    private:
        static const s32 c_max_num_sizes = 20;

        superpages_t m_pages;
        llhead_t     m_used_page_list_per_size[c_max_num_sizes];
    };

    void superfsa_t::initialize(superheap_t& heap, vmem_t* vmem, u64 address_range, u32 size_to_pre_allocate)
    {
        m_pages.initialize(heap, vmem, address_range, size_to_pre_allocate);
        for (u32 i = 0; i < c_max_num_sizes; i++)
            m_used_page_list_per_size[i].reset();
    }

    void superfsa_t::deinitialize(superheap_t& heap) { m_pages.deinitialize(heap); }
    u32  superfsa_t::sizeof_alloc(u32 alloc_size) const { return math::ceilpo2(alloc_size); }

    u32 superfsa_t::alloc(u32 alloc_size)
    {
        alloc_size      = math::ceilpo2(alloc_size);
        s32 const c     = math::countTrailingZeros(alloc_size);
        u32       ipage = 0xffffffff;
        ASSERT(c >= 0 && c < c_max_num_sizes);
        if (m_used_page_list_per_size[c].is_nil())
        {
            // Get a page and initialize that page for this size
            ipage = m_pages.checkout_page(alloc_size);
            m_used_page_list_per_size[c].insert(m_pages.m_page_list_data, ipage);
        }
        else
        {
            ipage = m_used_page_list_per_size[c].m_index;
        }

        if (ipage != llnode_t::NIL)
        {
            superpage_t* ppage    = &m_pages.m_page_array[ipage];
            void*        paddress = m_pages.address_of_page(ipage);
            u16 const    itemidx  = ppage->iallocate(paddress, m_pages.m_page_size);
            if (ppage->is_full(m_pages.m_page_size))
            {
                m_used_page_list_per_size[c].remove_item(m_pages.m_page_list_data, ipage);
            }
            return (ipage << 16) + itemidx;
        }
        else
        {
            return NIL;
        }
    }

    void superfsa_t::dealloc(u32 i)
    {
        u16 const          pageindex = i >> 16;
        u16 const          itemindex = i & 0xFFFF;
        superpage_t* const ppage     = &m_pages.m_page_array[pageindex];
        void* const        paddr     = m_pages.address_of_page(pageindex);
        ppage->deallocate(paddr, itemindex);
        if (ppage->is_empty())
        {
            s32 const c = math::countTrailingZeros(ppage->m_item_size);
            ASSERT(c >= 0 && c < c_max_num_sizes);
            m_used_page_list_per_size[c].remove_item(m_pages.m_page_list_data, pageindex);
            m_pages.release_page(pageindex);
        }
    }

    typedef u32 chunk_config_t;

    struct superbin_t
    {
        inline superbin_t(u64 allocsize, u8 binidx, chunk_config_t chunkinfo)
            : m_alloc_size(allocsize)
            , m_chunk_info(chunkinfo)
            , m_alloc_bin_index(binidx)
        {
            u32 const chunk_size = 1 << ((chunkinfo >> 8) & 0xff);
            m_max_alloc_count    = (u16)(chunk_size / allocsize);
        }

        inline u16  binmap_l1len() const { return (u16)(m_max_alloc_count >> 8); }
        inline u16  binmap_l2len() const { return (u16)(m_max_alloc_count >> 4); }
        inline bool use_binmap() const { return m_max_alloc_count == 1 && binmap_l1len() == 0 && binmap_l2len() == 0 ? 0 : 1; }

        u64            m_alloc_size;       // The size of the allocation that this bin is managing
        u32            m_max_alloc_count;  // The maximum number of allocations that can be made from a single chunk
        chunk_config_t m_chunk_info;       // The index of the allocator/superspace for this alloc size
        u8             m_alloc_bin_index;  // Only one indirection is allowed
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
            u32*      m_chunks_array;                   // The array of chunk iptr's
            llnode_t* m_chunks_llnode_array;            // The array of chunk llnode's
            u32       m_chunks_free_index;              // index of the first free chunk in the array
            llhead_t  m_chunks_free_list_head;          // pointer to a binmap_t managing free chunks
            llhead_t* m_chunks_active_list_head_array;  // index to an array of list heads for active chunks per allocation size
            llhead_t* m_chunks_cached_list_head_array;  // index to an array of list heads for cached chunks per allocation size
            u32       m_count_chunks_cached;            // number of chunks that can are cached
            u32       m_count_chunks_free;              // number of chunks that can are free
            u32       m_count_chunks_used;              // number of chunks that can are used
            u32       m_chunk_size_index;               // chunk size index
        };

        struct chunk_t
        {
            u32      m_chunk_index;          // The index of the chunk in the full segment
            u16      m_elem_used;            // The number of elements used in this chunk
            u16      m_bin_index;            // The index of the bin that this chunk belongs to
            binmap_t m_binmap;               // The binmap for this chunk
            u32      m_physical_pages;       // The number of physical pages that this chunk has committed
            u32      m_alloc_tracking_iptr;  // The index to the tracking array which we use for set_tag/get_tag
        };

        struct chunkinfo_t
        {
            u16 m_space_segment_index;  // The segment index in superspace
            u16 m_segment_chunk_index;  // The index of the chunk in the block
            u32 m_chunk_iptr;
        };

        superfsa_t* m_fsa;
        vmem_t*     m_vmem;
        void*       m_address_base;
        u64         m_address_range;
        u32         m_page_size;
        u8          m_page_shift;
        u8          m_segment_shift;  // 1 << m_segment_shift = segment size
        u32         m_segment_count;  // Space Address Range / Segment Size = Number of segments
        segment_t*  m_segment_array;  // Array of segments

        lldata_t m_segment_list_data;
        llist_t  m_segment_list_free;
        llist_t  m_segment_list_full;
        llist_t* m_segment_list_active;  // This needs to be per chunk-size

        u32*     m_chunk_sizes;      // Array of chunk sizes (c64KB, c128KB, c256KB, c512KB, c1MB ... c512MB)
        lldata_t m_chunk_list_data;  // Initialize this beforehand

        superspace_t()
            : m_fsa(nullptr)
            , m_vmem(nullptr)
            , m_address_base(nullptr)
            , m_address_range(0)
            , m_page_size(0)
            , m_page_shift(0)
            , m_segment_array(nullptr)
            , m_segment_list_data()
            , m_segment_list_free()
            , m_segment_list_active()
        {
        }

        void initialize(vmem_t* vmem, u64 address_range, u64 segment_size, superheap_t* heap, superfsa_t* fsa)
        {
            ASSERT(math::ispo2(address_range));
            ASSERT(math::ispo2(segment_size));

            m_vmem          = vmem;
            m_address_range = address_range;
            u32 const attrs = 0;
            m_vmem->reserve(address_range, m_page_size, attrs, m_address_base);
            m_page_shift = math::ilog2(m_page_size);

            m_fsa = fsa;

            u32 const segment_shift        = math::ilog2(segment_size);
            u32 const num_segments         = (u32)(m_address_range >> segment_shift);
            m_segment_array                = (segment_t*)heap->allocate(num_segments * sizeof(segment_t));
            m_segment_list_data.m_data     = m_segment_array;
            m_segment_list_data.m_itemsize = sizeof(segment_t);
            m_segment_list_free.initialize(m_segment_list_data, 0, num_segments, num_segments);
            m_segment_list_active.reset();
        }

        void deinitialize(superheap_t& heap)
        {
            if (m_vmem != nullptr)
            {
                m_vmem->release(m_address_base, m_address_range);
                m_vmem = nullptr;
            }
        }

        void initialize_binmap(u32 const binmap_iptr, bool set)
        {
            binmap_t* bm = (binmap_t*)m_fsa->idx2ptr(binmap_iptr);

            u16* l2;
            bm->m_l2_offset_iptr = m_fsa->alloc(sizeof(u16) * m_config.m_binmap_l2);
            l2                   = (u16*)m_fsa->idx2ptr(bm->m_l2_offset_iptr);

            u16* l1;
            if (m_config.m_binmap_l1 > 2)
            {
                bm->m_l1_offset_iptr = m_fsa->alloc(sizeof(u16) * m_config.m_binmap_l1);
                l1                   = (u16*)m_fsa->idx2ptr(bm->m_l1_offset_iptr);
            }
            else
            {
                bm->m_l1_offset_iptr = set ? 0xffffffff : 0;
                l1                   = (u16*)&bm->m_l1_offset_iptr;
            }

            if (set)
                bm->init1(m_config.m_chunks_max, l1, m_config.m_binmap_l1, l2, m_config.m_binmap_l2);
            else
                bm->init(m_config.m_chunks_max, l1, m_config.m_binmap_l1, l2, m_config.m_binmap_l2);
        }

        binmap_t* get_binmap_by_index(u32 const binmap_iptr, u16*& l1, u16*& l2)
        {
            binmap_t* bm = (binmap_t*)m_fsa->idx2ptr(binmap_iptr);
            l1           = (u16*)m_fsa->idx2ptr(bm->m_l1_offset_iptr);
            l2           = (u16*)m_fsa->idx2ptr(bm->m_l2_offset_iptr);
            return bm;
        }

        u32 checkout_segment(u32 chunk_size)
        {
            u32 const  segment_chunk_count           = (1 << m_segment_shift) / chunk_size;
            u32 const  segment_index                 = m_segment_list_free.remove_headi(m_segment_list_data);
            segment_t* segment                       = &m_segment_array[segment_index];
            u32 const  chunks_index_array_iptr       = m_fsa->alloc(sizeof(u32) * segment_chunk_count);
            u32 const  chunks_llnode_array_iptr      = m_fsa->alloc(sizeof(llnode_t) * segment_chunk_count);
            u32 const  chunks_active_list_array_iptr = m_fsa->alloc(sizeof(llhead_t) * segment_chunk_count);
            u32 const  chunks_cached_list_array_iptr = m_fsa->alloc(sizeof(llhead_t) * segment_chunk_count);

            segment->m_chunks_array                  = (u32*)m_fsa->idx2ptr(chunks_index_array_iptr);
            segment->m_chunks_llnode_array           = (llnode_t*)m_fsa->idx2ptr(chunks_llnode_array_iptr);
            segment->m_chunks_active_list_head_array = (llhead_t*)m_fsa->idx2ptr(chunks_active_list_array_iptr);
            segment->m_chunks_cached_list_head_array = (llhead_t*)m_fsa->idx2ptr(chunks_cached_list_array_iptr);

            segment->m_count_chunks_cached = 0;
            segment->m_count_chunks_free   = segment_chunk_count;
            segment->m_count_chunks_used   = 0;
            segment->m_chunk_size          = chunk_size;

            return segment_index;
        }

        u32 chunk_physical_pages(superbin_t const& bin, u32 alloc_size) const
        {
            u32 size;
            if (bin.use_binmap())
                size = (bin.m_alloc_size * bin.m_max_alloc_count);
            else
                size = alloc_size;
            return (size + (m_page_size - 1)) >> m_page_shift;
        }

        chunkinfo_t checkout_chunk(u32 alloc_size, superbin_t const& bin)
        {
            // Get the chunk size index

            u32 segment_index = 0xffffffff;
            if (m_segment_list_active.is_nil())
            {
                segment_index = checkout_block();
                m_segment_list_active.insert(m_segment_list_data, segment_index);
            }
            else
            {
                segment_index = m_segment_list_active.m_index;
            }

            u32 const required_physical_pages = chunk_physical_pages(bin, alloc_size);
            m_page_count += required_physical_pages;

            // Here we have a block where we can get a chunk from
            segment_t* block                   = &m_segment_array[segment_index];
            u32        block_chunk_index       = 0xffffffff;
            u32        already_committed_pages = 0;
            if (block->m_count_chunks_cached > 0)
            {
                u16 *     l1, *l2;
                binmap_t* bm      = get_binmap_by_index(block->m_binmap_chunks_cached_iptr, l1, l2);
                block_chunk_index = bm->findandset(m_config.m_chunks_max, l1, l2);
                block->m_count_chunks_cached -= 1;
                already_committed_pages = block->m_chunks_physical_pages[block_chunk_index];
            }
            else if (block->m_count_chunks_free > 0)
            {
                u16 *     l1, *l2;
                binmap_t* bm      = get_binmap_by_index(block->m_binmap_chunks_free_iptr, l1, l2);
                block_chunk_index = bm->findandset(m_config.m_chunks_max, l1, l2);
                block->m_count_chunks_free -= 1;
            }
            else
            {
                ASSERT(false);  // Error, this block should have been removed from 'm_segment_list_active'
            }

            u32 const  chunk_tracking_array_iptr = m_fsa->alloc(sizeof(u32) * bin.m_max_alloc_count);
            u32* const chunk_tracking_array      = (u32*)m_fsa->idx2ptr(chunk_tracking_array_iptr);

            block->m_chunks_alloc_tracking_array[block_chunk_index] = chunk_tracking_array_iptr;
            block->m_chunks_array[block_chunk_index]                = chunk_iptr;
            block->m_chunks_physical_pages[block_chunk_index]       = required_physical_pages;

            // Commit the virtual pages for this chunk
            if (required_physical_pages < already_committed_pages)
            {
                // TODO Overcommitted, uncommit pages ?
            }
            else if (required_physical_pages > already_committed_pages)
            {
                // TODO Undercommitted, commit necessary pages
            }

            // Check if block is now empty
            block->m_chunks_used += 1;
            if (block->m_chunks_used == m_config.m_chunks_max)
            {
                m_segment_list_active.remove_item(m_segment_list_data, segment_index);
            }

            // Return the chunk info
            chunkinfo_t chunkinfo;
            chunkinfo.m_block_chunk_index   = block_chunk_index;
            chunkinfo.m_segment_block_index = segment_index;
            chunkinfo.m_chunk_iptr          = chunk_iptr;
            return chunkinfo;
        }

        void release_chunk(chunkinfo_t const& chunkinfo, u32 alloc_size)
        {
            // See if this block was full, if so we need to add it back to the list of active blocks again so that
            // we can checkout chunks from it again.
            segment_t* block = &m_segment_array[chunkinfo.m_segment_block_index];
            if (block->m_chunks_used == m_config.m_chunks_max)
            {
                m_segment_list_active.insert(m_segment_list_data, chunkinfo.m_segment_block_index);
            }

            m_page_count -= block->m_chunks_physical_pages[chunkinfo.m_block_chunk_index];

            // We need to limit the number of cached chunks, once that happens we need to add the
            // block_chunk_index to the m_binmap_chunks_free_iptr.
            u16 *     l1, *l2;
            binmap_t* bm = get_binmap_by_index(block->m_binmap_chunks_cached_iptr, l1, l2);
            bm->clr(m_config.m_chunks_max, l1, l2, chunkinfo.m_block_chunk_index);
            block->m_count_chunks_cached += 1;

            // Release the tracking array that was allocated for this chunk
            u32 const chunk_tracking_array_iptr                                 = block->m_chunks_alloc_tracking_array[chunkinfo.m_block_chunk_index];
            block->m_chunks_alloc_tracking_array[chunkinfo.m_block_chunk_index] = 0xffffffff;
            m_fsa->dealloc(chunk_tracking_array_iptr);

            // See if this block is now empty, if so we need to release it
            block->m_chunks_used -= 1;
            if (block->m_chunks_used == 0)
            {
                m_segment_list_active.remove_item(m_segment_list_data, chunkinfo.m_segment_block_index);

                // Maybe every size should cache at least one block otherwise single alloc/dealloc calls will
                // checkout and release a block every time?

                // Release back all physical pages of the cached chunks
                binmap_t* bm = get_binmap_by_index(block->m_binmap_chunks_cached_iptr, l1, l2);
                while (block->m_count_chunks_cached > 0)
                {
                    u32 const ci = bm->findandset(m_config.m_chunks_max, l1, l2);
                    // TODO We need to decommit memory here.
                    block->m_count_chunks_cached -= 1;
                }

                u32 const chunks_array_iptr = m_fsa->ptr2idx(block->m_chunks_array);
                m_fsa->dealloc(chunks_array_iptr);
                u32 const chunks_pages_iptr                = m_fsa->ptr2idx(block->m_chunks_physical_pages);
                u32 const chunks_alloc_tracking_array_iptr = m_fsa->ptr2idx(block->m_chunks_alloc_tracking_array);
                m_fsa->dealloc(chunks_pages_iptr);
                m_fsa->dealloc(block->m_binmap_chunks_cached_iptr);
                m_fsa->dealloc(block->m_binmap_chunks_free_iptr);
                m_fsa->dealloc(chunks_alloc_tracking_array_iptr);

                block->m_prev                = llnode_t::NIL;
                block->m_next                = llnode_t::NIL;
                block->m_chunks_array        = nullptr;
                block->m_count_chunks_cached = 0;
                block->m_count_chunks_free   = 0;

                m_segment_list_free.insert(m_segment_list_data, chunkinfo.m_segment_block_index);
            }

            // Release the chunk structure back to the fsa
            m_fsa->dealloc(chunkinfo.m_chunk_iptr);
            block->m_chunks_array[chunkinfo.m_block_chunk_index] = 0xffffffff;
        }

        void set_assoc(void* ptr, u32 assoc, chunkinfo_t const& chunkinfo, superbin_t const& bin)
        {
            segment_t* block                     = &m_segment_array[chunkinfo.m_segment_block_index];
            u32 const  chunk_tracking_array_iptr = block->m_chunks_alloc_tracking_array[chunkinfo.m_block_chunk_index];
            u32* const chunk_tracking_array      = (u32*)m_fsa->idx2ptr(chunk_tracking_array_iptr);

            chunk_t* chunk = (chunk_t*)m_fsa->idx2ptr(chunkinfo.m_chunk_iptr);
            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            u32 chunk_item_index = 0;
            if (bin.use_binmap())
            {
                void* const chunk_address = chunk_index_to_address(chunk->m_chunk_index);
                chunk_item_index          = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            }
            chunk_tracking_array[chunk_item_index] = assoc;
        }

        u32 get_assoc(void* ptr, chunkinfo_t const& chunkinfo, superbin_t const& bin) const
        {
            segment_t* block                     = &m_segment_array[chunkinfo.m_segment_block_index];
            u32 const  chunk_tracking_array_iptr = block->m_chunks_alloc_tracking_array[chunkinfo.m_block_chunk_index];
            u32* const chunk_tracking_array      = (u32*)m_fsa->idx2ptr(chunk_tracking_array_iptr);

            chunk_t* chunk = (chunk_t*)m_fsa->idx2ptr(chunkinfo.m_chunk_iptr);
            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            u32 i = 0;
            if (bin.use_binmap())
            {
                void* const chunk_address = chunk_index_to_address(chunk->m_chunk_index);
                i                         = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            }
            return chunk_tracking_array[i];
        }

        // When deallocating, call this to get the page-index which you can than use
        // to get the 'chunk_t*'.
        inline u32 chunk_info_to_chunk_index(chunkinfo_t const& chunkinfo) const
        {
            segment_t const* const block   = &m_segment_array[chunkinfo.m_segment_block_index];
            u64 const              address = ((u64)chunkinfo.m_segment_block_index << m_config.m_blocks_shift) + ((u64)chunkinfo.m_block_chunk_index << m_config.m_chunks_shift);
            return (u32)(address >> m_page_shift);
        }

        inline u32   address_to_page_index(void* ptr) const { return (u32)(todistance(m_address_base, ptr) >> m_config.m_chunks_shift); }
        inline void* chunk_index_to_address(u32 chunk_index) const { return toaddress(m_address_base, (u64)chunk_index << m_config.m_chunks_shift); }

        chunkinfo_t chunk_index_to_chunk_info(u32 chunk_index) const
        {
            u32 const  block_index       = chunk_index >> (m_config.m_blocks_shift - m_config.m_chunks_shift);
            segment_t* block             = &m_segment_array[block_index];
            u32 const  block_chunk_index = chunk_index & ((1 << (m_config.m_blocks_shift - m_config.m_chunks_shift)) - 1);
            ASSERT(block_chunk_index < m_config.m_chunks_max);
            u32 const chunk_iptr = block->m_chunks_array[block_chunk_index];
            ASSERT(chunk_iptr != 0xffffffff);
            chunkinfo_t chunkinfo;
            chunkinfo.m_segment_block_index = block_index;
            chunkinfo.m_block_chunk_index   = block_chunk_index;
            chunkinfo.m_chunk_iptr          = chunk_iptr;
            return chunkinfo;
        }
    };

    // @superalloc manages an address range through a superspace which tracks a list of active
    //             blocks and chunks and is used by a range of allocation sizes.
    struct superalloc_t
    {
        superspace_t* m_superspace;
        lldata_t      m_chunk_list_data;
        llhead_t*     m_used_chunk_list_per_size;

        superalloc_t()
            : m_superspace(nullptr)
            , m_chunk_list_data()
            , m_used_chunk_list_per_size(nullptr)
        {
        }

        superalloc_t(superspace_t* s, llhead_t* used_chunk_list_per_size)
            : m_superspace(s)
            , m_chunk_list_data()
            , m_used_chunk_list_per_size(used_chunk_list_per_size)
        {
        }

        void  initialize(superheap_t& heap, superfsa_t& fsa);
        void* allocate(superfsa_t& sfsa, u32 size, superbin_t const& bin);
        u32   deallocate(superfsa_t& sfsa, void* ptr, superspace_t::chunkinfo_t const& chunkinfo, superbin_t const& bin);

        void set_assoc(void* ptr, u32 assoc, superspace_t::chunkinfo_t const& chunkinfo, superbin_t const& bin);
        u32  get_assoc(void* ptr, superspace_t::chunkinfo_t const& chunkinfo, superbin_t const& bin) const;

        void  initialize_chunk(superfsa_t& fsa, superspace_t::chunkinfo_t const& chunkinfo, u32 size, superbin_t const& bin);
        void  deinitialize_chunk(superfsa_t& fsa, superspace_t::chunkinfo_t const& chunkinfo, superbin_t const& bin);
        void* allocate_from_chunk(superfsa_t& fsa, superspace_t::chunkinfo_t const& chunkinfo, u32 size, superbin_t const& bin, bool& chunk_is_now_full);
        u32   deallocate_from_chunk(superfsa_t& fsa, superspace_t::chunkinfo_t const& chunkinfo, void* ptr, superbin_t const& bin, bool& chunk_is_now_empty, bool& chunk_was_full);
    };

    void superalloc_t::initialize(superheap_t& heap, superfsa_t& fsa)
    {
        m_chunk_list_data.m_data     = fsa.baseptr();
        m_chunk_list_data.m_itemsize = fsa.sizeof_alloc(sizeof(superspace_t::chunk_t));
        m_chunk_list_data.m_pagesize = fsa.pagesize();
    }

    void* superalloc_t::allocate(superfsa_t& sfsa, u32 alloc_size, superbin_t const& bin)
    {
        u32 const                 c = bin.m_alloc_bin_index;
        superspace_t::chunkinfo_t chunk_info;
        llindex_t                 chunk_iptr = m_used_chunk_list_per_size[c].m_index;
        if (chunk_iptr == llnode_t::NIL)
        {
            chunk_iptr = sfsa.alloc(sizeof(superspace_t::chunk_t));
            chunk_info = m_superspace->checkout_chunk(alloc_size, chunk_iptr, bin);
            initialize_chunk(sfsa, chunk_info, alloc_size, bin);
            m_used_chunk_list_per_size[c].insert(m_chunk_list_data, chunk_iptr);
        }
        else
        {
            superspace_t::chunk_t* chunk      = (superspace_t::chunk_t*)sfsa.idx2ptr(chunk_iptr);
            u32 const              page_index = chunk->m_chunk_index;
            chunk_info                        = m_superspace->chunk_index_to_chunk_info(page_index);
        }

        bool        chunk_is_now_full = false;
        void* const ptr               = allocate_from_chunk(sfsa, chunk_info, alloc_size, bin, chunk_is_now_full);
        if (chunk_is_now_full)  // Chunk is full, no more allocations possible
        {
            m_used_chunk_list_per_size[c].remove_item(m_chunk_list_data, chunk_iptr);
        }
        return ptr;
    }

    u32 superalloc_t::deallocate(superfsa_t& fsa, void* ptr, superspace_t::chunkinfo_t const& chunkinfo, superbin_t const& bin)
    {
        u32 const c                  = bin.m_alloc_bin_index;
        bool      chunk_is_now_empty = false;
        bool      chunk_was_full     = false;
        u32 const alloc_size         = deallocate_from_chunk(fsa, chunkinfo, ptr, bin, chunk_is_now_empty, chunk_was_full);
        if (chunk_is_now_empty)
        {
            if (!chunk_was_full)
            {
                superspace_t::chunk_t* chunk = (superspace_t::chunk_t*)fsa.idx2ptr(chunkinfo.m_chunk_iptr);
                m_used_chunk_list_per_size[c].remove_item(m_chunk_list_data, chunkinfo.m_chunk_iptr);
            }
            deinitialize_chunk(fsa, chunkinfo, bin);
            m_superspace->release_chunk(chunkinfo, alloc_size);
        }
        else if (chunk_was_full)
        {
            superspace_t::chunk_t* chunk = (superspace_t::chunk_t*)fsa.idx2ptr(chunkinfo.m_chunk_iptr);
            m_used_chunk_list_per_size[c].insert(m_chunk_list_data, chunkinfo.m_chunk_iptr);
        }
        return alloc_size;
    }

    void superalloc_t::set_assoc(void* ptr, u32 assoc, superspace_t::chunkinfo_t const& chunkinfo, superbin_t const& bin) { m_superspace->set_assoc(ptr, assoc, chunkinfo, bin); }
    u32  superalloc_t::get_assoc(void* ptr, superspace_t::chunkinfo_t const& chunkinfo, superbin_t const& bin) const { return m_superspace->get_assoc(ptr, chunkinfo, bin); }

    void superalloc_t::initialize_chunk(superfsa_t& fsa, superspace_t::chunkinfo_t const& info, u32 alloc_size, superbin_t const& bin)
    {
        superspace_t::chunk_t* chunk = (superspace_t::chunk_t*)fsa.idx2ptr(info.m_chunk_iptr);
        chunk->m_chunk_index         = m_superspace->chunk_info_to_chunk_index(info);
        if (bin.use_binmap())
        {
            binmap_t* binmap = (binmap_t*)&chunk->m_binmap;

            u16 const l1len = bin.binmap_l1len();
            u16 const l2len = bin.binmap_l2len();
            if (bin.m_max_alloc_count > 32)
            {
                u16* l2;
                binmap->m_l2_offset_iptr = fsa.alloc(sizeof(u16) * l2len);
                l2                       = (u16*)fsa.idx2ptr(binmap->m_l2_offset_iptr);

                u16* l1;
                if (l1len > 2)
                {
                    binmap->m_l1_offset_iptr = fsa.alloc(sizeof(u16) * l1len);
                    l1                       = (u16*)fsa.idx2ptr(binmap->m_l1_offset_iptr);
                }
                else
                {
                    l1 = (u16*)&binmap->m_l1_offset_iptr;
                }

                binmap->init(bin.m_max_alloc_count, l1, l1len, l2, l2len);
            }
            else
            {
                binmap->m_l1_offset_iptr = superfsa_t::NIL;
                binmap->m_l2_offset_iptr = superfsa_t::NIL;
                binmap->init(bin.m_max_alloc_count, nullptr, 0, nullptr, 0);
            }
        }
        else
        {
            chunk->m_physical_pages = 0;
        }

        chunk->m_bin_index = bin.m_alloc_bin_index;
        chunk->m_elem_used = 0;
    }

    void superalloc_t::deinitialize_chunk(superfsa_t& fsa, superspace_t::chunkinfo_t const& info, superbin_t const& bin)
    {
        superspace_t::chunk_t* chunk = (superspace_t::chunk_t*)fsa.idx2ptr(info.m_chunk_iptr);
        if (bin.use_binmap())
        {
            binmap_t* bm = (binmap_t*)&chunk->m_binmap;
            if (bm->m_l1_offset_iptr != superfsa_t::NIL)
            {
                fsa.dealloc(bm->m_l1_offset_iptr);
                fsa.dealloc(bm->m_l2_offset_iptr);
            }
            chunk->m_binmap.m_l1_offset_iptr = superfsa_t::NIL;
            chunk->m_binmap.m_l2_offset_iptr = superfsa_t::NIL;
        }
    }

    void* superalloc_t::allocate_from_chunk(superfsa_t& fsa, superspace_t::chunkinfo_t const& chunkinfo, u32 size, superbin_t const& bin, bool& chunk_is_now_full)
    {
        superspace_t::chunk_t* chunk = (superspace_t::chunk_t*)fsa.idx2ptr(chunkinfo.m_chunk_iptr);
        ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);

        void* ptr = m_superspace->chunk_index_to_address(chunk->m_chunk_index);
        if (bin.use_binmap())
        {
            binmap_t* bm = (binmap_t*)&chunk->m_binmap;
            u16*      l1 = nullptr;
            u16*      l2 = nullptr;
            if (bin.m_max_alloc_count > 32)
            {
                l1 = (u16*)fsa.idx2ptr(bm->m_l1_offset_iptr);
                l2 = (u16*)fsa.idx2ptr(bm->m_l2_offset_iptr);
            }
            u32 const i = bm->findandset(bin.m_max_alloc_count, l1, l2);
            ASSERT(i < bin.m_max_alloc_count);
            ptr = toaddress(ptr, (u64)i * bin.m_alloc_size);
        }
        else
        {
            chunk->m_physical_pages = (size + (m_superspace->m_page_size - 1)) >> m_superspace->m_page_shift;
        }

        chunk->m_elem_used += 1;
        chunk_is_now_full = (bin.m_max_alloc_count == chunk->m_elem_used);

        return ptr;
    }

    u32 superalloc_t::deallocate_from_chunk(superfsa_t& fsa, superspace_t::chunkinfo_t const& chunkinfo, void* ptr, superbin_t const& bin, bool& chunk_is_now_empty, bool& chunk_was_full)
    {
        superspace_t::chunk_t* chunk = (superspace_t::chunk_t*)fsa.idx2ptr(chunkinfo.m_chunk_iptr);
        ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);

        u32 size;
        if (bin.use_binmap())
        {
            void* const chunk_address = m_superspace->chunk_index_to_address(chunk->m_chunk_index);
            u32 const   i             = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            ASSERT(i < bin.m_max_alloc_count);
            binmap_t* binmap = (binmap_t*)&chunk->m_binmap;
            u16*      l1     = (u16*)fsa.idx2ptr(binmap->m_l1_offset_iptr);
            u16*      l2     = (u16*)fsa.idx2ptr(binmap->m_l2_offset_iptr);
            binmap->clr(bin.m_max_alloc_count, l1, l2, i);
            size = bin.m_alloc_size;
        }
        else
        {
            size = chunk->m_physical_pages * m_superspace->m_page_size;
        }

        chunk_was_full = (bin.m_max_alloc_count == chunk->m_elem_used);
        chunk->m_elem_used -= 1;
        chunk_is_now_empty = (0 == chunk->m_elem_used);

        return size;
    }

    /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    /// The following is a strict data-drive initialization of the bins and allocators, please know what you are doing when modifying any of this.
    struct supersegment_config_t
    {
        supersegment_config_t()
            : c_block_size(0)
            , c_chunk_size(0)
            , c_memtype(0)
            , c_memprotect(0)
        {
        }

        supersegment_config_t(u64 segment_size, u32 chunk_size, u16 memtype, u16 memprotect)
            : c_block_size(segment_size)
            , c_chunk_size(chunk_size)
            , c_memtype(memtype)
            , c_memprotect(memprotect)
        {
            // block-size / chunk-size must be a power of 2 and not larger than 65536 (16 bits)
        }

        const u64 c_block_size;
        const u32 c_chunk_size;
        const u16 c_memtype;
        const u16 c_memprotect;
    };

    struct superallocator_config_t
    {
        superallocator_config_t()
            : m_total_address_space(0)
            , m_space_address_range(0)
            , m_num_superbins(0)
            , m_asuperbins(nullptr)
            , m_internal_heap_address_range(0)
            , m_internal_heap_pre_size(0)
            , m_internal_fsa_address_range(0)
            , m_internal_fsa_pre_size(0)
        {
        }

        superallocator_config_t(const superallocator_config_t& other)
            : m_total_address_space(other.m_total_address_space)
            , m_space_address_range(other.m_space_address_range)
            , m_num_superbins(other.m_num_superbins)
            , m_asuperbins(other.m_asuperbins)
            , m_internal_heap_address_range(other.m_internal_heap_address_range)
            , m_internal_heap_pre_size(other.m_internal_heap_pre_size)
            , m_internal_fsa_address_range(other.m_internal_fsa_address_range)
            , m_internal_fsa_pre_size(other.m_internal_fsa_pre_size)
        {
        }

        superallocator_config_t(u64 total_address_space, u64 segment_address_range, s32 const num_superbins, superbin_t const* asuperbins, s32 const num_supersegment, supersegment_config_t const* asupersegment, u32 const internal_heap_address_range,
                                u32 const internal_heap_pre_size, u32 const internal_fsa_address_range, u32 const internal_fsa_pre_size)
            : m_num_superbins(num_superbins)
            , m_asuperbins(asuperbins)
            , m_num_supersegments(num_supersegment)
            , m_asupersegmentconfigs(asupersegment)
            , m_internal_heap_address_range(internal_heap_address_range)
            , m_internal_heap_pre_size(internal_heap_pre_size)
            , m_internal_fsa_address_range(internal_fsa_address_range)
            , m_internal_fsa_pre_size(internal_fsa_pre_size)
        {
            m_segment_address_range_shift = math::ilog2(segment_address_range);
        }

        u64                          m_total_address_space;
        u64                          m_space_address_range;
        s16                          m_segment_address_range_shift;
        u16                          m_num_supersegments;
        u32                          m_num_superbins;
        supersegment_config_t const* m_asupersegmentconfigs;
        superbin_t const*            m_asuperbins;
        u32                          m_internal_heap_address_range;
        u32                          m_internal_heap_pre_size;
        u32                          m_internal_fsa_address_range;
        u32                          m_internal_fsa_pre_size;
    };

    // 25% allocation waste (based on empirical data)
    namespace superallocator_config_windows_desktop_app_25p_t
    {
        // Windows OS desktop application superallocator configuration
        static const u32 c_page_size = 4096;  // Windows OS page size

        static const u64 c_superspace_size = cGB * 1;

        // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly

        // Chunk Info
        typedef u32                 chunk_config_t;
        static const chunk_config_t c4KB   = (12 << 8) | 0;
        static const chunk_config_t c16KB  = (14 << 8) | 0;
        static const chunk_config_t c64KB  = (16 << 8) | 1;
        static const chunk_config_t c128KB = (17 << 8) | 1;
        static const chunk_config_t c256KB = (18 << 8) | 1;
        static const chunk_config_t c512KB = (19 << 8) | 1;
        static const chunk_config_t c1MB   = (20 << 8) | 1;
        static const chunk_config_t c2MB   = (21 << 8) | 1;
        static const chunk_config_t c4MB   = (22 << 8) | 1;
        static const chunk_config_t c8MB   = (23 << 8) | 1;
        static const chunk_config_t c16MB  = (24 << 8) | 1;
        static const chunk_config_t c32MB  = (25 << 8) | 1;
        static const chunk_config_t c64MB  = (26 << 8) | 1;
        static const chunk_config_t c128MB = (27 << 8) | 2;
        static const chunk_config_t c256MB = (28 << 8) | 2;
        static const chunk_config_t c512MB = (29 << 8) | 2;

        // clang-format off
        // superbin_t(alloc-size MB, KB, B, bin redir index, chunk-info, max-alloc-count, binmap level 1 length (u16), binmap level 2 length (u16))
        static const s32        c_num_bins           = 112;
        static const superbin_t c_asbins[c_num_bins] = {
          superbin_t(        8,  8, c64KB),                superbin_t(   8,  8, c64KB),                // 8, 8
          superbin_t(        8,  8, c64KB),                superbin_t(   8,  8, c64KB),                // 8, 8
          superbin_t(        8,  8, c64KB),                superbin_t(   8,  8, c64KB),                // 8, 8
          superbin_t(        8,  8, c64KB),                superbin_t(   8,  8, c64KB),                // 8, 8
          superbin_t(        8,  8, c64KB),                superbin_t(  10, 10, c64KB),                // 8, 12
          superbin_t(       12, 10, c64KB),                superbin_t(  14, 12, c64KB),                // 12, 16
          superbin_t(       16, 12, c64KB),                superbin_t(  20, 13, c64KB),                // 16, 20
          superbin_t(       24, 14, c64KB),                superbin_t(  28, 15, c64KB),                // 24, 28
          superbin_t(       32, 16,  c4KB),                superbin_t(  40, 17, c64KB),                // 32, 40
          superbin_t(       48, 18, c64KB),                superbin_t(  56, 19, c64KB),                // 48, 56
          superbin_t(       64, 20, c64KB),                superbin_t(  80, 21, c64KB),                //
          superbin_t(       96, 22, c64KB),                superbin_t(  112, 23, c64KB),               //
          superbin_t(      128, 24, c64KB),                superbin_t(  160, 25, c64KB),               //
          superbin_t(      192, 26, c64KB),                superbin_t(  224, 27, c64KB),               //
          superbin_t(      256, 28, c64KB),                superbin_t(  320, 29, c64KB),               //
          superbin_t(      384, 30, c64KB),                superbin_t(  448, 31, c64KB),               //
          superbin_t(      512, 32, c64KB),                superbin_t(  640, 33, c64KB),               //
          superbin_t(      768, 34, c64KB),                superbin_t(  896, 35, c64KB),               //
          superbin_t(  1 * cKB, 36, c64KB),                    superbin_t( 1*cKB + 256, 37, c128KB),            //
          superbin_t(  1 * cKB + 512, 38, c128KB),              superbin_t( 1*cKB + 768, 39, c128KB),            //
          superbin_t(  2 * cKB, 40, c128KB),                    superbin_t( 2*cKB + 512, 41, c128KB),            //
          superbin_t(  3 * cKB, 42, c128KB),                    superbin_t( 3*cKB + 512, 43, c128KB),            //
          superbin_t(  4 * cKB, 44, c128KB),                    superbin_t( 5*cKB, 45, c128KB),                  //
          superbin_t(  6 * cKB, 46, c128KB),                    superbin_t( 7*cKB, 47, c128KB),                  //
          superbin_t(  8 * cKB, 48, c128KB),                    superbin_t( 10*cKB, 49, c128KB),                 //
          superbin_t( 12 * cKB, 50, c128KB),                    superbin_t( 14*cKB, 51, c128KB),                 //
          superbin_t( 16 * cKB, 52, c128KB),                    superbin_t( 20*cKB, 53, c128KB),                 //
          superbin_t( 24 * cKB, 54, c128KB),                    superbin_t( 28*cKB, 55, c128KB),                 //
          superbin_t( 32 * cKB, 56, c128KB),                    superbin_t( 40*cKB, 57, c128KB),                 //
          superbin_t( 48 * cKB, 58, c128KB),                    superbin_t( 56*cKB, 59, c128KB),                 //
          superbin_t( 64 * cKB, 60, c128KB),                    superbin_t( 80*cKB, 61, c128KB),                 //
          superbin_t( 96 * cKB, 62, c128KB),                    superbin_t( 112*cKB, 63, c128KB),                //
          superbin_t(128 * cKB, 64, c128KB),                    superbin_t( 160*cKB, 65, c128KB),                //
          superbin_t(192 * cKB, 66, c128KB),                    superbin_t( 224*cKB, 67, c128KB),                //
          superbin_t(256 * cKB, 68, c128KB),                    superbin_t( 320*cKB, 69, c128KB),                //
          superbin_t(384 * cKB, 70, c128KB),                    superbin_t( 448*cKB, 71, c128KB),                //
          superbin_t(512 * cKB, 72, c128KB),                    superbin_t( 640*cKB, 73, c128KB),                //
          superbin_t(768 * cKB, 74, c128KB),                    superbin_t( 896*cKB, 75, c128KB),                //
          superbin_t(  1 * cMB, 76, c128KB),                    superbin_t(1*cMB + 256*cKB, 77, c128KB),         //
          superbin_t(  1 * cMB + 512 * cKB, 78, c128KB),        superbin_t(1*cMB + 768*cKB, 79, c128KB),         //
          superbin_t(  2 * cMB, 80, c128KB),                    superbin_t(2*cMB + 512*cKB, 81, c128KB),         //
          superbin_t(  3 * cMB, 82, c128KB),                    superbin_t(3*cMB + 512*cKB, 83, c128KB),         //
          superbin_t(  4 * cMB, 84, c128KB),                    superbin_t(5*cMB, 85, c128KB),                   //
          superbin_t(  6 * cMB, 86, c128KB),                    superbin_t(7*cMB, 87, c128KB),                   //
          superbin_t(  8 * cMB, 88, c128KB),                    superbin_t(10*cMB, 89, c128KB),                  //
          superbin_t( 12 * cMB, 90, c128KB),                    superbin_t(14*cMB, 91, c128KB),                  //
          superbin_t( 16 * cMB, 92, c128KB),                    superbin_t(20*cMB, 93, c128KB),                  //
          superbin_t( 24 * cMB, 94, c128KB),                    superbin_t(28*cMB, 95, c128KB),                  //
          superbin_t( 32 * cMB, 96, c128KB),                    superbin_t(40*cMB, 97, c128KB),                  //
          superbin_t( 48 * cMB, 98, c128KB),                    superbin_t(56*cMB, 99, c128KB),                  //
          superbin_t( 64 * cMB, 100, c128KB),                   superbin_t(80*cMB, 101, c128KB),                 //
          superbin_t( 96 * cMB, 102, c128KB),                   superbin_t(112*cMB, 103, c128KB),                //
          superbin_t(128 * cMB, 104, c128KB),                   superbin_t(160*cMB, 105, c128KB),                //
          superbin_t(192 * cMB, 106, c128KB),                   superbin_t(224*cMB, 107, c128KB),                //
          superbin_t(256 * cMB, 108, c128KB),                   superbin_t(320*cMB, 109, c128KB),                //
          superbin_t(384 * cMB, 110, c128KB),                   superbin_t(448*cMB, 111, c128KB),                //
        };
        // clang-format on

        static superallocator_config_t get_config()
        {
            const u64 c_total_address_space         = 64 * cGB;
            const u64 c_segment_address_range       = 4 * cGB;
            const u32 c_internal_heap_address_range = 16 * cMB;
            const u32 c_internal_heap_pre_size      = 2 * cMB;
            const u32 c_internal_fsa_address_range  = 16 * cMB;
            const u32 c_internal_fsa_pre_size       = 2 * cMB;
            return superallocator_config_t(c_total_address_space, c_segment_address_range, c_num_bins, c_asbins, c_num_segments, c_assegments, c_internal_heap_address_range, c_internal_heap_pre_size, c_internal_fsa_address_range, c_internal_fsa_pre_size);
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
        static const s32                   c_num_segments               = 2;
        static const supersegment_config_t c_assegments[c_num_segments] = {
          supersegment_config_t(cGB * 1, 64 * cKB, 0, 0),  //
          supersegment_config_t(cGB * 1, 4 * cKB, 0, 0)    //
        };

        // clang-format off
        // superbin_t(alloc-size, bin redir index, chunk-block-info)
        static const s32        c_num_bins           = 216;
        static const superbin_t c_asbins[c_num_bins] = {
          superbin_t(  8, 8, c64KB),                              superbin_t(  8, 8, c64KB),                    //
          superbin_t(  8, 8, c64KB),                              superbin_t(  8, 8, c64KB),                    //
          superbin_t(  8, 8, c64KB),                              superbin_t(  8, 8, c64KB),                    //
          superbin_t(  8, 8, c64KB),                              superbin_t(  8, 8, c64KB),                    //
          superbin_t(  8, 8, c64KB),                              superbin_t(  9, 12, c64KB),                   //
          superbin_t(  10, 12, c64KB),                            superbin_t(  11, 12, c64KB),                  //
          superbin_t(  12, 12, c64KB),                            superbin_t(  13, 16, c64KB),                  //
          superbin_t(  14, 16, c64KB),                            superbin_t(  15, 16, c64KB),                  //
          superbin_t(  16, 16, c64KB),                            superbin_t(  18, 18, c64KB),                  //
          superbin_t(  20, 18, c64KB),                            superbin_t(  22, 20, c64KB),                  //
          superbin_t(  24, 20, c64KB),                            superbin_t(  26, 22, c64KB),                  //
          superbin_t(  28, 22, c64KB),                            superbin_t(  30, 24, c64KB),                  //
          superbin_t(  32, 24, c64KB),                            superbin_t(  36, 25, c64KB),                  //
          superbin_t(  40, 26, c64KB),                            superbin_t(  44, 27, c64KB),                  //
          superbin_t(  48, 28, c64KB),                            superbin_t(  52, 29, c64KB),                  //
          superbin_t(  56, 30, c64KB),                            superbin_t(  60, 31, c64KB),                  //
          superbin_t(  64, 32, c64KB),                            superbin_t(  72, 33, c64KB),                  //
          superbin_t(  80, 34, c64KB),                            superbin_t(  88, 35, c64KB),                  //
          superbin_t(  96, 36, c64KB),                            superbin_t(  104, 37, c64KB),                 //
          superbin_t(  112, 38, c64KB),                           superbin_t(  120, 39, c64KB),                 //
          superbin_t(  128, 40, c64KB),                           superbin_t(  144, 41, c64KB),                 //
          superbin_t(  160, 42, c64KB),                           superbin_t(  176, 43, c64KB),                 //
          superbin_t(  192, 44, c64KB),                           superbin_t(  208, 45, c64KB),                 //
          superbin_t(  224, 46, c64KB),                           superbin_t(  240, 47, c64KB),                 //
          superbin_t(  256, 48, c64KB),                           superbin_t(  288, 49, c64KB),                 //
          superbin_t(  320, 50, c64KB),                           superbin_t(  352, 51, c64KB),                 //
          superbin_t(  384, 52, c64KB),                           superbin_t(  416, 53, c64KB),                 //
          superbin_t(  448, 54, c64KB),                           superbin_t(  480, 55, c64KB),                 //
          superbin_t(  512, 56, c64KB),                           superbin_t(  576, 57, c64KB),                 //
          superbin_t(  640, 58, c64KB),                           superbin_t(  704, 59, c64KB),                 //
          superbin_t(  768, 60, c64KB),                           superbin_t(  832, 61, c64KB),                 //
          superbin_t(  896, 62, c64KB),                           superbin_t(  960, 63, c64KB),                 //
          superbin_t( 1*cKB, 64, c64KB),                          superbin_t( 1*cKB + 128, 65, c64KB),          //
          superbin_t( 1*cKB + 256, 66, c64KB),                    superbin_t( 1*cKB + 384, 67, c64KB),          //
          superbin_t( 1*cKB + 512, 68, c64KB),                    superbin_t( 1*cKB + 640, 69, c64KB),          //
          superbin_t( 1*cKB + 768, 70, c64KB),                    superbin_t( 1*cKB + 896, 71, c64KB),          //
          superbin_t( 2*cKB, 72, c64KB),                          superbin_t( 2*cKB + 256, 73, c64KB),          //
          superbin_t( 2*cKB + 512, 74, c64KB),                    superbin_t( 2*cKB + 768, 75, c64KB),          //
          superbin_t( 3*cKB, 76, c64KB),                          superbin_t( 3*cKB + 256, 77, c64KB),          //
          superbin_t( 3*cKB + 512, 78, c64KB),                    superbin_t( 3*cKB + 768, 79, c64KB),          //
          superbin_t( 4*cKB, 80, c64KB),                          superbin_t( 4*cKB + 512, 81, c64KB),          //
          superbin_t( 5*cKB, 82, c64KB),                          superbin_t( 5*cKB + 512, 83, c64KB),          //
          superbin_t( 6*cKB, 84, c64KB),                          superbin_t( 6*cKB + 512, 85, c64KB),          //
          superbin_t( 7*cKB, 86, c64KB),                          superbin_t( 7*cKB + 512, 87, c64KB),          //
          superbin_t( 8*cKB, 88, c64KB),                          superbin_t( 9*cKB, 89, c64KB),                //
          superbin_t( 10*cKB, 90, c64KB),                         superbin_t( 11*cKB, 91, c64KB),               //
          superbin_t( 12*cKB, 92, c64KB),                         superbin_t( 13*cKB, 93, c64KB),               //
          superbin_t( 14*cKB, 94, c64KB),                         superbin_t( 15*cKB, 95, c64KB),               //
          superbin_t( 16*cKB, 96, c64KB),                         superbin_t( 18*cKB, 97, c64KB),               //
          superbin_t( 20*cKB, 98, c64KB),                         superbin_t( 22*cKB, 99, c64KB),               //
          superbin_t( 24*cKB, 100, c64KB),                        superbin_t( 26*cKB, 101, c64KB),              //
          superbin_t( 28*cKB, 102, c64KB),                        superbin_t( 30*cKB, 103, c64KB),              //
          superbin_t( 32*cKB, 104, c64KB),                        superbin_t( 36*cKB, 105, c64KB),              //
          superbin_t( 40*cKB, 106, c64KB),                        superbin_t( 44*cKB, 107, c64KB),              //
          superbin_t( 48*cKB, 108, c64KB),                        superbin_t( 52*cKB, 109, c64KB),              //
          superbin_t( 56*cKB, 110, c64KB),                        superbin_t( 60*cKB, 111, c64KB),              //
          superbin_t( 64*cKB, 112, c64KB),                        superbin_t( 72*cKB, 113, c64KB),              //
          superbin_t( 80*cKB, 114, c64KB),                        superbin_t( 88*cKB, 115, c64KB),              //
          superbin_t( 96*cKB, 116, c64KB),                        superbin_t( 104*cKB, 117, c64KB),             //
          superbin_t( 112*cKB, 118, c64KB),                       superbin_t( 120*cKB, 119, c64KB),             //
          superbin_t( 128*cKB, 120, c64KB),                       superbin_t( 144*cKB, 121, c64KB),             //
          superbin_t( 160*cKB, 122, c64KB),                       superbin_t( 176*cKB, 123, c64KB),             //
          superbin_t( 192*cKB, 124, c64KB),                       superbin_t( 208*cKB, 125, c64KB),             //
          superbin_t( 224*cKB, 126, c64KB),                       superbin_t( 240*cKB, 127, c64KB),             //
          superbin_t( 256*cKB, 128, c64KB),                       superbin_t( 288*cKB, 129, c64KB),             //
          superbin_t( 320*cKB, 130, c64KB),                       superbin_t( 352*cKB, 131, c64KB),             //
          superbin_t( 384*cKB, 132, c64KB),                       superbin_t( 416*cKB, 133, c64KB),             //
          superbin_t( 448*cKB, 134, c64KB),                       superbin_t( 480*cKB, 135, c64KB),             //
          superbin_t( 512*cKB, 136, c64KB),                       superbin_t( 576*cKB, 137, c64KB),             //
          superbin_t( 640*cKB, 138, c64KB),                       superbin_t( 704*cKB, 139, c64KB),             //
          superbin_t( 768*cKB, 140, c64KB),                       superbin_t( 832*cKB, 141, c64KB),             //
          superbin_t( 896*cKB, 142, c64KB),                       superbin_t( 960*cKB, 143, c64KB),             //
          superbin_t(1*cMB, 144, c64KB),                          superbin_t(1*cMB + 128*cKB, 145, c64KB),      //
          superbin_t(1*cMB + 256*cKB, 146, c64KB),                superbin_t(1*cMB + 384*cKB, 147, c64KB),      //
          superbin_t(1*cMB + 512*cKB, 148, c64KB),                superbin_t(1*cMB + 640*cKB, 149, c64KB),      //
          superbin_t(1*cMB + 768*cKB, 150, c64KB),                superbin_t(1*cMB + 896*cKB, 151, c64KB),      //
          superbin_t(2*cMB, 152, c64KB),                          superbin_t(2*cMB + 256*cKB, 153, c64KB),      //
          superbin_t(2*cMB + 512*cKB, 154, c64KB),                superbin_t(2*cMB + 768*cKB, 155, c64KB),      //
          superbin_t(3*cMB, 156, c64KB),                          superbin_t(3*cMB + 256*cKB, 157, c64KB),      //
          superbin_t(3*cMB + 512*cKB, 158, c64KB),                superbin_t(3*cMB + 768*cKB, 159, c64KB),      //
          superbin_t(4*cMB, 160, c64KB),                          superbin_t(4*cMB + 512*cKB, 161, c64KB),      //
          superbin_t(5*cMB, 162, c64KB),                          superbin_t(5*cMB + 512*cKB, 163, c64KB),      //
          superbin_t(6*cMB, 164, c64KB),                          superbin_t(6*cMB + 512*cKB, 165, c64KB),      //
          superbin_t(7*cMB, 166, c64KB),                          superbin_t(7*cMB + 512*cKB, 167, c64KB),      //
          superbin_t(8*cMB, 168, c64KB),                          superbin_t(9*cMB, 169, c64KB),                //
          superbin_t(10*cKB, 170, c64KB),                         superbin_t(11*cMB, 171, c64KB),               //
          superbin_t(12*cMB, 172, c64KB),                         superbin_t(13*cMB, 173, c64KB),               //
          superbin_t(14*cMB, 174, c64KB),                         superbin_t(15*cMB, 175, c64KB),               //
          superbin_t(16*cMB, 176, c64KB),                         superbin_t(18*cMB, 177, c64KB),               //
          superbin_t(20*cKB, 178, c64KB),                         superbin_t(22*cMB, 179, c64KB),               //
          superbin_t(24*cMB, 180, c64KB),                         superbin_t(26*cMB, 181, c64KB),               //
          superbin_t(28*cMB, 182, c64KB),                         superbin_t(30*cKB, 183, c64KB),               //
          superbin_t(32*cMB, 184, c64KB),                         superbin_t(36*cMB, 185, c64KB),               //
          superbin_t(40*cKB, 186, c64KB),                         superbin_t(44*cMB, 187, c64KB),               //
          superbin_t(48*cMB, 188, c64KB),                         superbin_t(52*cMB, 189, c64KB),               //
          superbin_t(56*cMB, 190, c64KB),                         superbin_t(60*cKB, 191, c64KB),               //
          superbin_t(64*cMB, 192, c64KB),                         superbin_t(72*cMB, 193, c64KB),               //
          superbin_t(80*cKB, 194, c64KB),                         superbin_t(88*cMB, 195, c64KB),               //
          superbin_t(96*cMB, 196, c64KB),                         superbin_t(104*cMB, 197, c64KB),              //
          superbin_t(112*cMB, 198, c64KB),                        superbin_t(120*cKB, 199, c64KB),              //
          superbin_t(128*cMB, 200, c64KB),                        superbin_t(144*cMB, 201, c64KB),              //
          superbin_t(160*cKB, 202, c64KB),                        superbin_t(176*cMB, 203, c64KB),              //
          superbin_t(192*cMB, 204, c64KB),                        superbin_t(208*cMB, 205, c64KB),              //
          superbin_t(224*cMB, 206, c64KB),                        superbin_t(240*cKB, 207, c64KB),              //
          superbin_t(256*cMB, 208, c64KB),                        superbin_t(288*cMB, 209, c64KB),              //
          superbin_t(320*cKB, 210, c64KB),                        superbin_t(352*cMB, 211, c64KB),              //
          superbin_t(384*cMB, 212, c64KB),                        superbin_t(416*cMB, 213, c64KB),              //
          superbin_t(448*cMB, 214, c64KB),                        superbin_t(480*cKB, 215, c64KB),              //
        };
        // clang-format on

        static superallocator_config_t get_config()
        {
            const u64 c_total_address_space         = 64 * cGB;
            const u64 c_segment_address_range       = 4 * cGB;
            const u32 c_internal_heap_address_range = 16 * cMB;
            const u32 c_internal_heap_pre_size      = 2 * cMB;
            const u32 c_internal_fsa_address_range  = 16 * cMB;
            const u32 c_internal_fsa_pre_size       = 2 * cMB;
            return superallocator_config_t(c_total_address_space, c_segment_address_range, c_num_bins, c_asbins, c_num_segments, c_assegments, c_internal_heap_address_range, c_internal_heap_pre_size, c_internal_fsa_address_range, c_internal_fsa_pre_size);
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

    class superallocator_t : public valloc_t
    {
        void*                   m_vmem_membase;
        superspace_t*           m_asupersegments;
        superalloc_t*           m_aallocators;  // Total Address Space / Supersegment Address Space = Size of m_aallocators
        vmem_t*                 m_vmem;
        superheap_t             m_internal_heap;
        superfsa_t              m_internal_fsa;
        superallocator_config_t m_config;

    public:
        superallocator_t()
            : m_vmem_membase(0)
            , m_asupersegments(nullptr)
            , m_aallocators(nullptr)
            , m_vmem(nullptr)
            , m_internal_heap()
            , m_internal_fsa()
            , m_config()
        {
        }

        void initialize(vmem_t* vmem, superallocator_config_t const& config);
        void deinitialize();

        inline superspace_t* get_supersegment(void* ptr) const
        {
            u64 const dist = todistance(m_vmem_membase, ptr);
            u32 const ssai = (u32)(dist >> m_config.m_segment_address_range_shift);
            return &m_asupersegments[ssai];
        }

        virtual void* v_allocate(u32 size, u32 align);
        virtual u32   v_deallocate(void* p);
        virtual void  v_release() { deinitialize(); }

        virtual void v_set_tag(void* ptr, u32 assoc);
        virtual u32  v_get_tag(void* ptr) const;
        virtual u32  v_get_size(void* ptr) const;
    };

    void superallocator_t::initialize(vmem_t* vmem, superallocator_config_t const& config)
    {
        m_config = config;
        m_vmem   = vmem;
        m_internal_heap.initialize(m_vmem, m_config.m_internal_heap_address_range, m_config.m_internal_heap_pre_size);
        m_internal_fsa.initialize(m_internal_heap, m_vmem, m_config.m_internal_fsa_address_range, m_config.m_internal_fsa_pre_size);

        llhead_t* used_chunk_list_per_size = (llhead_t*)m_internal_heap.allocate(sizeof(llhead_t) * m_config.m_num_superbins);
        for (u32 i = 0; i < m_config.m_num_superbins; ++i)
        {
            used_chunk_list_per_size[i].reset();
        }

        const u32 num_segments = (u32)(config.m_total_address_space / config.m_space_address_range);
        m_asupersegments       = (superspace_t*)m_internal_heap.allocate(sizeof(superspace_t) * num_segments);
        for (u32 i = 0; i < num_segments; ++i)
        {
            m_asupersegments[i] = superspace_t();
            m_aallocators[i]    = superalloc_t();
        }
        for (u32 i = 0; i < m_config.m_num_supersegments; ++i)
        {
            m_asupersegments[i].initialize(vmem, config.m_space_address_range, m_config.m_asupersegmentconfigs[i].c_block_size, m_config.m_asupersegmentconfigs[i].c_chunk_size, &m_internal_heap, &m_internal_fsa);
            m_aallocators[i] = superalloc_t(&m_asupersegments[i], used_chunk_list_per_size);
            m_aallocators[i].initialize(m_internal_heap, m_internal_fsa);
        }

#ifdef SUPERALLOC_DEBUG
        // sanity check on the superbin_t config
        for (u32 s = 0; s < m_config.m_num_superbins; s++)
        {
            u32 const rs            = m_config.m_asuperbins[s].m_alloc_bin_index;
            u32 const size          = m_config.m_asuperbins[rs].m_alloc_size;
            u32 const bin_index     = superallocator_config::size2bin(size);
            u32 const bin_reindex   = m_config.m_asuperbins[bin_index].m_alloc_bin_index;
            u32 const bin_allocsize = m_config.m_asuperbins[bin_reindex].m_alloc_size;
            ASSERT(size <= bin_allocsize);
        }
#endif
    }

    void superallocator_t::deinitialize()
    {
        m_internal_fsa.deinitialize(m_internal_heap);
        for (s32 i = 0; i < m_config.m_num_supersegments; ++i)
        {
            m_asupersegments[i].deinitialize(m_internal_heap);
        }
        m_internal_heap.deinitialize();
        m_asupersegments = nullptr;
        m_aallocators    = nullptr;
        m_vmem           = nullptr;
    }

    void* superallocator_t::v_allocate(u32 size, u32 alignment)
    {
        size                  = math::alignUp(size, alignment);
        u32 const sbinindex   = m_config.m_asuperbins[superallocator_config::size2bin(size)].m_alloc_bin_index;
        s32 const sallocindex = m_config.m_asuperbins[sbinindex].m_alloc_index;
        ASSERT(size <= m_config.m_asuperbins[sbinindex].m_alloc_size);
        ASSERT(m_config.m_asuperbins[sbinindex].m_alloc_bin_index == sbinindex);
        void* ptr = m_aallocators[sallocindex].allocate(m_internal_fsa, size, m_config.m_asuperbins[sbinindex]);
        ASSERT(ptr >= m_asupersegments[sallocindex].m_address_base && ptr < ((u8*)m_asupersegments[sallocindex].m_address_base + m_asupersegments[sallocindex].m_address_range));
        return ptr;
    }

    u32 superallocator_t::v_deallocate(void* ptr)
    {
        if (ptr == nullptr)
            return 0;
        superspace_t* ssegment = get_supersegment(ptr);
        ASSERT(ptr >= ssegment->m_address_base && ptr < ((u8*)ssegment->m_address_base + ssegment->m_address_range));
        u32 const                 chunk_index = ssegment->address_to_page_index(ptr);
        superspace_t::chunkinfo_t chunk_info  = ssegment->chunk_index_to_chunk_info(chunk_index);
        superspace_t::chunk_t*    pchunk      = (superspace_t::chunk_t*)m_internal_fsa.idx2ptr(chunk_info.m_chunk_iptr);
        u32 const                 sbinindex   = pchunk->m_bin_index;
        u32 const                 sallocindex = m_config.m_asuperbins[sbinindex].m_alloc_index;
        u32 const                 size        = m_aallocators[sallocindex].deallocate(m_internal_fsa, ptr, chunk_info, m_config.m_asuperbins[sbinindex]);
        ASSERT(size <= m_config.m_asuperbins[sbinindex].m_alloc_size);
        return size;
    }

    void superallocator_t::v_set_tag(void* ptr, u32 assoc)
    {
        if (ptr != nullptr)
        {
            superspace_t* ssegment = get_supersegment(ptr);
            ASSERT(ptr >= ssegment->m_address_base && ptr < ((u8*)ssegment->m_address_base + ssegment->m_address_range));
            u32 const                 chunk_index = ssegment->address_to_page_index(ptr);
            superspace_t::chunkinfo_t chunk_info  = ssegment->chunk_index_to_chunk_info(chunk_index);
            superspace_t::chunk_t*    pchunk      = (superspace_t::chunk_t*)m_internal_fsa.idx2ptr(chunk_info.m_chunk_iptr);
            u32 const                 sbinindex   = pchunk->m_bin_index;
            u32 const                 sallocindex = m_config.m_asuperbins[sbinindex].m_alloc_index;
            m_aallocators[sallocindex].set_assoc(ptr, assoc, chunk_info, m_config.m_asuperbins[sbinindex]);
        }
    }

    u32 superallocator_t::v_get_tag(void* ptr) const
    {
        if (ptr == nullptr)
            return 0xffffffff;
        superspace_t* ssegment = get_supersegment(ptr);
        ASSERT(ptr >= ssegment->m_address_base && ptr < ((u8*)ssegment->m_address_base + ssegment->m_address_range));
        u32 const                 chunk_index = ssegment->address_to_page_index(ptr);
        superspace_t::chunkinfo_t chunk_info  = ssegment->chunk_index_to_chunk_info(chunk_index);
        superspace_t::chunk_t*    pchunk      = (superspace_t::chunk_t*)m_internal_fsa.idx2ptr(chunk_info.m_chunk_iptr);
        u32 const                 sbinindex   = pchunk->m_bin_index;
        u32 const                 sallocindex = m_config.m_asuperbins[sbinindex].m_alloc_index;
        return m_aallocators[sallocindex].get_assoc(ptr, chunk_info, m_config.m_asuperbins[sbinindex]);
    }

    u32 superallocator_t::v_get_size(void* ptr) const
    {
        if (ptr == nullptr)
            return 0;
        superspace_t* ssegment = get_supersegment(ptr);
        ASSERT(ptr >= ssegment->m_address_base && ptr < ((u8*)ssegment->m_address_base + ssegment->m_address_range));
        u32 const                 chunk_index = ssegment->address_to_page_index(ptr);
        superspace_t::chunkinfo_t chunk_info  = ssegment->chunk_index_to_chunk_info(chunk_index);
        superspace_t::chunk_t*    pchunk      = (superspace_t::chunk_t*)m_internal_fsa.idx2ptr(chunk_info.m_chunk_iptr);
        u32 const                 sbinindex   = pchunk->m_bin_index;
        if (m_config.m_asuperbins[sbinindex].m_use_binmap == 1)
        {
            return m_config.m_asuperbins[sbinindex].m_alloc_size;
        }
        else
        {
            superspace_t::segment_t* block = &ssegment->m_segment_array[chunk_info.m_segment_block_index];
            return block->m_chunks_physical_pages[chunk_info.m_block_chunk_index] * ssegment->m_page_size;
        }
    }

    valloc_t* gCreateVmAllocator(alloc_t* main_heap, vmem_t* vmem)
    {
        superallocator_t* superalloc = (superallocator_t*)main_heap->allocate(sizeof(superallocator_t));
        superalloc->initialize(vmem, superallocator_config::get_config());
        return superalloc;
    }

}  // namespace ncore
