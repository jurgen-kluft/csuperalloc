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

    struct superbin_t
    {
        inline superbin_t(u32 allocsize_mb, u32 allocsize_kb, u32 allocsize_b, u8 binidx, u8 allocindex, u8 use_binmap, u16 count, u16 l1len, u16 l2len)
            : m_alloc_size((cMB * allocsize_mb) + (cKB * allocsize_kb) + (allocsize_b))
            , m_alloc_bin_index(binidx)
            , m_alloc_index(allocindex)
            , m_use_binmap(use_binmap)
            , m_max_alloc_count(count)
            , m_binmap_l1len(l1len)
            , m_binmap_l2len(l2len)
        {
        }

        u32 m_alloc_size;           // The size of the allocation that this bin is managing
        u32 m_alloc_bin_index : 8;  // Only one indirection is allowed
        u32 m_alloc_index : 8;      // The index of the allocator/supersegment for this alloc size
        u32 m_use_binmap : 1;       // How do we manage a chunk (binmap or page-count)
        u32 m_max_alloc_count;      // The maximum number of allocations
        u16 m_binmap_l1len;         // The length of the first level of the binmap
        u16 m_binmap_l2len;         // The length of the second level of the binmap
    };

    // A segment is divided into blocks, each block is divided into chunks and every chunk
    // is of the same size.
    //
    // Functionality:
    //   Allocate
    //    - Handling the request of a new chunk, either creating one or taking one from the cache
    //   Deallocate
    //    - Quickly finding the block_t*, chunk_t* and superalloc_t* that belong to a 'void* ptr'
    //    - Collecting a now empty-chunk and either release or cache it
    //   Get/Set Assoc
    //    - Set an associated value for a pointer
    //    - Get the associated value for a pointer
    //
    //   Get chunk by index
    //   Get address of chunk
    //
    struct supersegment_t
    {
        struct block_t : llnode_t
        {
            u16* m_chunks_physical_pages;
            u32* m_chunks_array;
            u32* m_chunks_alloc_tracking_array;
            u32  m_binmap_chunks_free_iptr;    // index to a binmap_t
            u32  m_binmap_chunks_cached_iptr;  // index to a binmap_t
            u32  m_count_chunks_cached;        // what is the [min,max] number of chunks that can be counted?
            u32  m_count_chunks_free;          //
            u32  m_chunks_used;                // what is the [min,max] number of chunks that can be used?
        };

        struct chunk_t : llnode_t
        {
            u32      m_chunk_index;     // The index of the chunk in the full segment
            u16      m_elem_used;       // The number of elements used in this chunk
            u16      m_bin_index;       // The index of the bin that this chunk belongs to
            binmap_t m_binmap;          // The binmap for this chunk
            u32      m_physical_pages;  // The number of physical pages that this chunk uses
            u32      m_alloc_tracking;  // The index to the tracking array which we use for set_tag/get_tag
        };

        struct chunkinfo_t
        {
            u16 m_segment_block_index;  // The block index of the segment
            u16 m_block_chunk_index;    // The index of the chunk in the block
            u32 m_chunk_iptr;
        };

        struct config_t
        {
            u16 m_blocks_shift;  // block-size = 1 << m_blocks_shift
            u16 m_chunks_max;    // maximum number of chunks per block
            u8  m_chunks_shift;  // chunk-size = 1 << m_chunks_shift
            u8  m_binmap_l1;
            u16 m_binmap_l2;
        };

        superfsa_t* m_fsa;
        vmem_t*     m_vmem;
        void*       m_address_base;
        u64         m_address_range;
        u32         m_page_count;
        u32         m_page_size;
        u8          m_page_shift;
        config_t    m_config;
        block_t*    m_blocks_array;
        lldata_t    m_blocks_list_data;
        llist_t     m_blocks_list_free;
        llhead_t    m_active_block_list;

        supersegment_t()
            : m_fsa(nullptr)
            , m_vmem(nullptr)
            , m_address_base(nullptr)
            , m_address_range(0)
            , m_page_count(0)
            , m_page_size(0)
            , m_page_shift(0)
            , m_blocks_array(nullptr)
            , m_blocks_list_data()
            , m_blocks_list_free()
            , m_active_block_list()
        {
        }

        void initialize(vmem_t* vmem, u64 address_range, u64 block_range, u32 chunk_size, superheap_t* heap, superfsa_t* fsa)
        {
            ASSERT(math::ispo2(address_range));
            ASSERT(math::ispo2(block_range));
            ASSERT(math::ispo2(chunk_size));
            ASSERT((block_range / chunk_size) <= 65535);  // 16-bit index

            m_vmem          = vmem;
            m_address_range = address_range;
            u32 const attrs = 0;
            m_vmem->reserve(address_range, m_page_size, attrs, m_address_base);
            m_page_shift = math::ilog2(m_page_size);
            m_page_count = 0;

            m_fsa = fsa;

            u32 const blocks_shift        = math::ilog2(block_range);
            u32 const num_blocks          = (u32)(m_address_range >> blocks_shift);
            m_blocks_array                = (block_t*)heap->allocate(num_blocks * sizeof(block_t));
            m_blocks_list_data.m_data     = m_blocks_array;
            m_blocks_list_data.m_itemsize = sizeof(block_t);
            m_blocks_list_free.initialize(m_blocks_list_data, 0, num_blocks, num_blocks);
            m_active_block_list.reset();

            m_config.m_blocks_shift = blocks_shift;
            m_config.m_chunks_shift = math::ilog2(chunk_size);
            m_config.m_chunks_max   = (u16)(block_range >> m_config.m_chunks_shift);
            m_config.m_binmap_l2    = (m_config.m_chunks_max + 15) / 16;
            m_config.m_binmap_l1    = (m_config.m_binmap_l2 + 15) / 16;
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

        u16 checkout_block()
        {
            u32 const block_index                      = m_blocks_list_free.remove_headi(m_blocks_list_data);
            block_t*  block                            = &m_blocks_array[block_index];
            u32 const chunks_index_array_iptr          = m_fsa->alloc(sizeof(u32) * m_config.m_chunks_max);
            u32 const chunks_pages_array_iptr          = m_fsa->alloc(sizeof(u32) * m_config.m_chunks_max);
            u32 const chunks_alloc_tracking_array_iptr = m_fsa->alloc(sizeof(u32) * m_config.m_chunks_max);

            block->m_prev                        = llnode_t::NIL;
            block->m_next                        = llnode_t::NIL;
            block->m_chunks_physical_pages       = (u16*)m_fsa->idx2ptr(chunks_pages_array_iptr);
            block->m_chunks_array                = (u32*)m_fsa->idx2ptr(chunks_index_array_iptr);
            block->m_chunks_alloc_tracking_array = (u32*)m_fsa->idx2ptr(chunks_alloc_tracking_array_iptr);
            block->m_binmap_chunks_cached_iptr   = m_fsa->alloc(sizeof(binmap_t));
            block->m_binmap_chunks_free_iptr     = m_fsa->alloc(sizeof(binmap_t));
            if (m_config.m_binmap_l2 > 0)
            {
                initialize_binmap(block->m_binmap_chunks_cached_iptr, true);
                initialize_binmap(block->m_binmap_chunks_free_iptr, false);
            }

            block->m_chunks_used         = 0;
            block->m_count_chunks_cached = 0;
            block->m_count_chunks_free   = m_config.m_chunks_max;
            return block_index;
        }

        u32 chunk_physical_pages(superbin_t const& bin, u32 alloc_size) const
        {
            u32 size;
            if (bin.m_use_binmap == 1)
                size = (bin.m_alloc_size * bin.m_max_alloc_count);
            else
                size = alloc_size;
            return (size + (m_page_size - 1)) >> m_page_shift;
        }

        // we do not need chunk_shift here since super segments is locked at initialization on a specific chunk size
        chunkinfo_t checkout_chunk(u32 alloc_size, u32 chunk_iptr, superbin_t const& bin)
        {
            u32 block_index = 0xffffffff;
            if (m_active_block_list.is_nil())
            {
                block_index = checkout_block();
                m_active_block_list.insert(m_blocks_list_data, block_index);
            }
            else
            {
                block_index = m_active_block_list.m_index;
            }

            u32 const required_physical_pages = chunk_physical_pages(bin, alloc_size);
            m_page_count += required_physical_pages;

            // Here we have a block where we can get a chunk from
            block_t* block                   = &m_blocks_array[block_index];
            u32      block_chunk_index       = 0xffffffff;
            u32      already_committed_pages = 0;
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
                ASSERT(false);  // Error, this block should have been removed from 'm_active_block_list'
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
                m_active_block_list.remove_item(m_blocks_list_data, block_index);
            }

            // Return the chunk info
            chunkinfo_t chunkinfo;
            chunkinfo.m_block_chunk_index   = block_chunk_index;
            chunkinfo.m_segment_block_index = block_index;
            chunkinfo.m_chunk_iptr          = chunk_iptr;
            return chunkinfo;
        }

        void release_chunk(chunkinfo_t const& chunkinfo, u32 alloc_size)
        {
            // See if this block was full, if so we need to add it back to the list of active blocks again so that
            // we can checkout chunks from it again.
            block_t* block = &m_blocks_array[chunkinfo.m_segment_block_index];
            if (block->m_chunks_used == m_config.m_chunks_max)
            {
                m_active_block_list.insert(m_blocks_list_data, chunkinfo.m_segment_block_index);
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
                m_active_block_list.remove_item(m_blocks_list_data, chunkinfo.m_segment_block_index);

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

                m_blocks_list_free.insert(m_blocks_list_data, chunkinfo.m_segment_block_index);
            }

            // Release the chunk structure back to the fsa
            m_fsa->dealloc(chunkinfo.m_chunk_iptr);
            block->m_chunks_array[chunkinfo.m_block_chunk_index] = 0xffffffff;
        }

        void set_assoc(void* ptr, u32 assoc, chunkinfo_t const& chunkinfo, superbin_t const& bin)
        {
            block_t*   block                     = &m_blocks_array[chunkinfo.m_segment_block_index];
            u32 const  chunk_tracking_array_iptr = block->m_chunks_alloc_tracking_array[chunkinfo.m_block_chunk_index];
            u32* const chunk_tracking_array      = (u32*)m_fsa->idx2ptr(chunk_tracking_array_iptr);

            chunk_t* chunk = (chunk_t*)m_fsa->idx2ptr(chunkinfo.m_chunk_iptr);
            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            u32 chunk_item_index = 0;
            if (bin.m_use_binmap == 1)
            {
                void* const chunk_address = chunk_index_to_address(chunk->m_chunk_index);
                chunk_item_index          = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            }
            chunk_tracking_array[chunk_item_index] = assoc;
        }

        u32 get_assoc(void* ptr, chunkinfo_t const& chunkinfo, superbin_t const& bin) const
        {
            block_t*   block                     = &m_blocks_array[chunkinfo.m_segment_block_index];
            u32 const  chunk_tracking_array_iptr = block->m_chunks_alloc_tracking_array[chunkinfo.m_block_chunk_index];
            u32* const chunk_tracking_array      = (u32*)m_fsa->idx2ptr(chunk_tracking_array_iptr);

            chunk_t* chunk = (chunk_t*)m_fsa->idx2ptr(chunkinfo.m_chunk_iptr);
            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            u32 i = 0;
            if (bin.m_use_binmap == 1)
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
            block_t const* const block   = &m_blocks_array[chunkinfo.m_segment_block_index];
            u64 const            address = ((u64)chunkinfo.m_segment_block_index << m_config.m_blocks_shift) + ((u64)chunkinfo.m_block_chunk_index << m_config.m_chunks_shift);
            return (u32)(address >> m_page_shift);
        }

        inline u32   address_to_page_index(void* ptr) const { return (u32)(todistance(m_address_base, ptr) >> m_config.m_chunks_shift); }
        inline void* chunk_index_to_address(u32 chunk_index) const { return toaddress(m_address_base, (u64)chunk_index << m_config.m_chunks_shift); }

        chunkinfo_t chunk_index_to_chunk_info(u32 chunk_index) const
        {
            u32 const block_index                            = chunk_index >> (m_config.m_blocks_shift - m_config.m_chunks_shift);
            block_t*  block                                  = &m_blocks_array[block_index];
            u32 const block_chunk_index                      = chunk_index & ((1 << (m_config.m_blocks_shift - m_config.m_chunks_shift)) - 1);
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

    // @superalloc manages an address range through a supersegment which tracks a list of active
    //             blocks and chunks and is used by a range of allocation sizes.
    struct superalloc_t
    {
        supersegment_t* m_ssegment;
        lldata_t        m_chunk_list_data;
        llhead_t*       m_used_chunk_list_per_size;

        superalloc_t()
            : m_ssegment(nullptr)
            , m_chunk_list_data()
            , m_used_chunk_list_per_size(nullptr)
        {
        }

        superalloc_t(supersegment_t* s, llhead_t* used_chunk_list_per_size)
            : m_ssegment(s)
            , m_chunk_list_data()
            , m_used_chunk_list_per_size(used_chunk_list_per_size)
        {
        }

        void  initialize(superheap_t& heap, superfsa_t& fsa);
        void* allocate(superfsa_t& sfsa, u32 size, superbin_t const& bin);
        u32   deallocate(superfsa_t& sfsa, void* ptr, supersegment_t::chunkinfo_t const& chunkinfo, superbin_t const& bin);

        void set_assoc(void* ptr, u32 assoc, supersegment_t::chunkinfo_t const& chunkinfo, superbin_t const& bin);
        u32  get_assoc(void* ptr, supersegment_t::chunkinfo_t const& chunkinfo, superbin_t const& bin) const;

        void  initialize_chunk(superfsa_t& fsa, supersegment_t::chunkinfo_t const& chunkinfo, u32 size, superbin_t const& bin);
        void  deinitialize_chunk(superfsa_t& fsa, supersegment_t::chunkinfo_t const& chunkinfo, superbin_t const& bin);
        void* allocate_from_chunk(superfsa_t& fsa, supersegment_t::chunkinfo_t const& chunkinfo, u32 size, superbin_t const& bin, bool& chunk_is_now_full);
        u32   deallocate_from_chunk(superfsa_t& fsa, supersegment_t::chunkinfo_t const& chunkinfo, void* ptr, superbin_t const& bin, bool& chunk_is_now_empty, bool& chunk_was_full);
    };

    void superalloc_t::initialize(superheap_t& heap, superfsa_t& fsa)
    {
        m_chunk_list_data.m_data     = fsa.baseptr();
        m_chunk_list_data.m_itemsize = fsa.sizeof_alloc(sizeof(supersegment_t::chunk_t));
        m_chunk_list_data.m_pagesize = fsa.pagesize();
    }

    void* superalloc_t::allocate(superfsa_t& sfsa, u32 alloc_size, superbin_t const& bin)
    {
        u32 const                   c = bin.m_alloc_bin_index;
        supersegment_t::chunkinfo_t chunk_info;
        llindex_t                   chunk_iptr = m_used_chunk_list_per_size[c].m_index;
        if (chunk_iptr == llnode_t::NIL)
        {
            chunk_iptr = sfsa.alloc(sizeof(supersegment_t::chunk_t));
            chunk_info = m_ssegment->checkout_chunk(alloc_size, chunk_iptr, bin);
            initialize_chunk(sfsa, chunk_info, alloc_size, bin);
            m_used_chunk_list_per_size[c].insert(m_chunk_list_data, chunk_iptr);
        }
        else
        {
            supersegment_t::chunk_t* chunk      = (supersegment_t::chunk_t*)sfsa.idx2ptr(chunk_iptr);
            u32 const                page_index = chunk->m_chunk_index;
            chunk_info                          = m_ssegment->chunk_index_to_chunk_info(page_index);
        }

        bool        chunk_is_now_full = false;
        void* const ptr               = allocate_from_chunk(sfsa, chunk_info, alloc_size, bin, chunk_is_now_full);
        if (chunk_is_now_full)  // Chunk is full, no more allocations possible
        {
            m_used_chunk_list_per_size[c].remove_item(m_chunk_list_data, chunk_iptr);
        }
        return ptr;
    }

    u32 superalloc_t::deallocate(superfsa_t& fsa, void* ptr, supersegment_t::chunkinfo_t const& chunkinfo, superbin_t const& bin)
    {
        u32 const c                  = bin.m_alloc_bin_index;
        bool      chunk_is_now_empty = false;
        bool      chunk_was_full     = false;
        u32 const alloc_size         = deallocate_from_chunk(fsa, chunkinfo, ptr, bin, chunk_is_now_empty, chunk_was_full);
        if (chunk_is_now_empty)
        {
            if (!chunk_was_full)
            {
                supersegment_t::chunk_t* chunk = (supersegment_t::chunk_t*)fsa.idx2ptr(chunkinfo.m_chunk_iptr);
                m_used_chunk_list_per_size[c].remove_item(m_chunk_list_data, chunkinfo.m_chunk_iptr);
            }
            deinitialize_chunk(fsa, chunkinfo, bin);
            m_ssegment->release_chunk(chunkinfo, alloc_size);
        }
        else if (chunk_was_full)
        {
            supersegment_t::chunk_t* chunk = (supersegment_t::chunk_t*)fsa.idx2ptr(chunkinfo.m_chunk_iptr);
            m_used_chunk_list_per_size[c].insert(m_chunk_list_data, chunkinfo.m_chunk_iptr);
        }
        return alloc_size;
    }

    void superalloc_t::set_assoc(void* ptr, u32 assoc, supersegment_t::chunkinfo_t const& chunkinfo, superbin_t const& bin) { m_ssegment->set_assoc(ptr, assoc, chunkinfo, bin); }
    u32  superalloc_t::get_assoc(void* ptr, supersegment_t::chunkinfo_t const& chunkinfo, superbin_t const& bin) const { return m_ssegment->get_assoc(ptr, chunkinfo, bin); }

    void superalloc_t::initialize_chunk(superfsa_t& fsa, supersegment_t::chunkinfo_t const& info, u32 alloc_size, superbin_t const& bin)
    {
        supersegment_t::chunk_t* chunk = (supersegment_t::chunk_t*)fsa.idx2ptr(info.m_chunk_iptr);
        chunk->m_chunk_index           = m_ssegment->chunk_info_to_chunk_index(info);
        if (bin.m_use_binmap == 1)
        {
            binmap_t* binmap = (binmap_t*)&chunk->m_binmap;
            if (bin.m_max_alloc_count > 32)
            {
                u16* l2;
                binmap->m_l2_offset_iptr = fsa.alloc(sizeof(u16) * bin.m_binmap_l2len);
                l2                       = (u16*)fsa.idx2ptr(binmap->m_l2_offset_iptr);

                u16* l1;
                if (bin.m_binmap_l1len > 2)
                {
                    binmap->m_l1_offset_iptr = fsa.alloc(sizeof(u16) * bin.m_binmap_l1len);
                    l1                       = (u16*)fsa.idx2ptr(binmap->m_l1_offset_iptr);
                }
                else
                {
                    l1 = (u16*)&binmap->m_l1_offset_iptr;
                }

                binmap->init(bin.m_max_alloc_count, l1, bin.m_binmap_l1len, l2, bin.m_binmap_l2len);
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

    void superalloc_t::deinitialize_chunk(superfsa_t& fsa, supersegment_t::chunkinfo_t const& info, superbin_t const& bin)
    {
        supersegment_t::chunk_t* chunk = (supersegment_t::chunk_t*)fsa.idx2ptr(info.m_chunk_iptr);
        if (bin.m_use_binmap == 1)
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

    void* superalloc_t::allocate_from_chunk(superfsa_t& fsa, supersegment_t::chunkinfo_t const& chunkinfo, u32 size, superbin_t const& bin, bool& chunk_is_now_full)
    {
        supersegment_t::chunk_t* chunk = (supersegment_t::chunk_t*)fsa.idx2ptr(chunkinfo.m_chunk_iptr);
        ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);

        void* ptr = m_ssegment->chunk_index_to_address(chunk->m_chunk_index);
        if (bin.m_use_binmap == 1)
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
            chunk->m_physical_pages = (size + (m_ssegment->m_page_size - 1)) >> m_ssegment->m_page_shift;
        }

        chunk->m_elem_used += 1;
        chunk_is_now_full = (bin.m_max_alloc_count == chunk->m_elem_used);

        return ptr;
    }

    u32 superalloc_t::deallocate_from_chunk(superfsa_t& fsa, supersegment_t::chunkinfo_t const& chunkinfo, void* ptr, superbin_t const& bin, bool& chunk_is_now_empty, bool& chunk_was_full)
    {
        supersegment_t::chunk_t* chunk = (supersegment_t::chunk_t*)fsa.idx2ptr(chunkinfo.m_chunk_iptr);
        ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);

        u32 size;
        if (bin.m_use_binmap == 1)
        {
            void* const chunk_address = m_ssegment->chunk_index_to_address(chunk->m_chunk_index);
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
            size = chunk->m_physical_pages * m_ssegment->m_page_size;
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
            : c_block_range(0)
            , c_chunk_size(0)
            , c_memtype(0)
            , c_memprotect(0)
        {
        }

        supersegment_config_t(u64 const block_range, u32 chunk_size, u16 memtype, u16 memprotect)
            : c_block_range(block_range)
            , c_chunk_size(chunk_size)
            , c_memtype(memtype)
            , c_memprotect(memprotect)
        {
            // block-range / page-size must be a power of 2 and not larger than 65536 (16 bits)
        }

        const u64 c_block_range;
        const u32 c_chunk_size;
        const u16 c_memtype;
        const u16 c_memprotect;
    };

    struct superallocator_config_t
    {
        superallocator_config_t()
            : m_total_address_space(0)
            , m_segment_address_range(0)
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
            , m_segment_address_range(other.m_segment_address_range)
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
        u64                          m_segment_address_range;
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
    namespace superallocator_config_desktop_app_25p_t
    {
        // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly
        static const s32                   c_num_ssegments               = 3;
        static const supersegment_config_t c_assegments[c_num_ssegments] = {
          // block-size, chunk-size, memtype, memprotect
          supersegment_config_t(cGB * 1, 64 * cKB, 0, 0),    // cGB * 1 / 64 * cKB = 16 * cKB
          supersegment_config_t(cMB * 256, 16 * cKB, 0, 0),  // cMB * 256 / 16 cKB = 16 * cKB
          supersegment_config_t(cMB * 64, 4 * cKB, 0, 0),    // cMB * 64 / 4 * cKB = 16 * cKB
        };

        // superbin_t(alloc-size MB, KB, B, bin redir index, allocator / supersegment index, use binmap?, max alloc-count, binmap level 1 length (u16), binmap level 2 length (u16))
        static const s32        c_num_bins           = 112;
        static const superbin_t c_asbins[c_num_bins] = {
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),    //
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),    //
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),    //
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),    //
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 10, 10, 0, 1, 6553, 32, 512),  //
          superbin_t(0, 0, 12, 10, 0, 1, 5461, 32, 512), superbin_t(0, 0, 14, 12, 0, 1, 4681, 32, 512),  //
          superbin_t(0, 0, 16, 12, 0, 1, 4096, 16, 256), superbin_t(0, 0, 20, 13, 0, 1, 3276, 16, 256),  //
          superbin_t(0, 0, 24, 14, 0, 1, 2730, 16, 256), superbin_t(0, 0, 28, 15, 0, 1, 2340, 16, 256),  //
          superbin_t(0, 0, 32, 16, 2, 1, 128, 1, 8),     superbin_t(0, 0, 40, 17, 0, 1, 1638, 8, 128),   //
          superbin_t(0, 0, 48, 18, 0, 1, 1365, 8, 128),  superbin_t(0, 0, 56, 19, 0, 1, 1170, 8, 128),   //
          superbin_t(0, 0, 64, 20, 0, 1, 1024, 4, 64),   superbin_t(0, 0, 80, 21, 0, 1, 819, 4, 64),     //
          superbin_t(0, 0, 96, 22, 0, 1, 682, 4, 64),    superbin_t(0, 0, 112, 23, 0, 1, 585, 4, 64),    //
          superbin_t(0, 0, 128, 24, 0, 1, 512, 2, 32),   superbin_t(0, 0, 160, 25, 0, 1, 409, 2, 32),    //
          superbin_t(0, 0, 192, 26, 0, 1, 341, 2, 32),   superbin_t(0, 0, 224, 27, 0, 1, 292, 2, 32),    //
          superbin_t(0, 0, 256, 28, 0, 1, 256, 2, 16),   superbin_t(0, 0, 320, 29, 0, 1, 204, 2, 16),    //
          superbin_t(0, 0, 384, 30, 0, 1, 170, 2, 16),   superbin_t(0, 0, 448, 31, 0, 1, 146, 2, 16),    //
          superbin_t(0, 0, 512, 32, 0, 1, 128, 2, 8),    superbin_t(0, 0, 640, 33, 0, 1, 102, 2, 8),     //
          superbin_t(0, 0, 768, 34, 0, 1, 85, 2, 8),     superbin_t(0, 0, 896, 35, 0, 1, 73, 2, 8),      //
          superbin_t(0, 1, 0, 36, 0, 1, 64, 2, 4),       superbin_t(0, 1, 256, 37, 0, 1, 51, 2, 4),      //
          superbin_t(0, 1, 512, 38, 0, 1, 85, 2, 8),     superbin_t(0, 1, 768, 39, 0, 1, 73, 2, 8),      //
          superbin_t(0, 2, 0, 40, 0, 1, 32, 0, 0),       superbin_t(0, 2, 512, 41, 0, 1, 51, 2, 4),      //
          superbin_t(0, 3, 0, 42, 0, 1, 85, 2, 8),       superbin_t(0, 3, 512, 43, 0, 1, 73, 2, 8),      //
          superbin_t(0, 4, 0, 44, 0, 1, 16, 0, 0),       superbin_t(0, 5, 0, 45, 0, 1, 51, 2, 4),        //
          superbin_t(0, 6, 0, 46, 0, 1, 32, 2, 4),       superbin_t(0, 7, 0, 47, 0, 1, 73, 2, 8),        //
          superbin_t(0, 8, 0, 48, 0, 1, 8, 0, 0),        superbin_t(0, 10, 0, 49, 0, 1, 51, 2, 4),       //
          superbin_t(0, 12, 0, 50, 0, 1, 16, 0, 0),      superbin_t(0, 14, 0, 51, 0, 1, 32, 2, 4),       //
          superbin_t(0, 16, 0, 52, 0, 1, 4, 0, 0),       superbin_t(0, 20, 0, 53, 0, 1, 16, 0, 0),       //
          superbin_t(0, 24, 0, 54, 0, 1, 8, 0, 0),       superbin_t(0, 28, 0, 55, 0, 1, 16, 0, 0),       //
          superbin_t(0, 32, 0, 56, 0, 1, 2, 0, 0),       superbin_t(0, 40, 0, 57, 0, 1, 8, 0, 0),        //
          superbin_t(0, 48, 0, 58, 0, 1, 4, 0, 0),       superbin_t(0, 56, 0, 59, 0, 1, 8, 0, 0),        //
          superbin_t(0, 64, 0, 60, 0, 0, 1, 0, 0),       superbin_t(0, 80, 0, 61, 0, 1, 4, 0, 0),        //
          superbin_t(0, 96, 0, 62, 0, 1, 2, 0, 0),       superbin_t(0, 112, 0, 63, 0, 1, 4, 0, 0),       //
          superbin_t(0, 128, 0, 64, 0, 0, 1, 0, 0),      superbin_t(0, 160, 0, 65, 0, 1, 2, 0, 0),       //
          superbin_t(0, 192, 0, 66, 0, 0, 1, 0, 0),      superbin_t(0, 224, 0, 67, 0, 1, 2, 0, 0),       //
          superbin_t(0, 256, 0, 68, 0, 0, 1, 0, 0),      superbin_t(0, 320, 0, 69, 0, 0, 1, 0, 0),       //
          superbin_t(0, 384, 0, 70, 0, 0, 1, 0, 0),      superbin_t(0, 448, 0, 71, 0, 0, 1, 0, 0),       //
          superbin_t(0, 512, 0, 72, 0, 0, 1, 0, 0),      superbin_t(0, 640, 0, 73, 0, 0, 1, 0, 0),       //
          superbin_t(0, 768, 0, 74, 0, 0, 1, 0, 0),      superbin_t(0, 896, 0, 75, 0, 0, 1, 0, 0),       //
          superbin_t(1, 0, 0, 76, 0, 0, 1, 0, 0),        superbin_t(1, 256, 0, 77, 0, 0, 1, 0, 0),       //
          superbin_t(1, 512, 0, 78, 0, 0, 1, 0, 0),      superbin_t(1, 768, 0, 79, 0, 0, 1, 0, 0),       //
          superbin_t(2, 0, 0, 80, 0, 0, 1, 0, 0),        superbin_t(2, 512, 0, 81, 0, 0, 1, 0, 0),       //
          superbin_t(3, 0, 0, 82, 0, 0, 1, 0, 0),        superbin_t(3, 512, 0, 83, 0, 0, 1, 0, 0),       //
          superbin_t(4, 0, 0, 84, 0, 0, 1, 0, 0),        superbin_t(5, 0, 0, 85, 0, 0, 1, 0, 0),         //
          superbin_t(6, 0, 0, 86, 0, 0, 1, 0, 0),        superbin_t(7, 0, 0, 87, 0, 0, 1, 0, 0),         //
          superbin_t(8, 0, 0, 88, 0, 0, 1, 0, 0),        superbin_t(10, 0, 0, 89, 0, 0, 1, 0, 0),        //
          superbin_t(12, 0, 0, 90, 0, 0, 1, 0, 0),       superbin_t(14, 0, 0, 91, 0, 0, 1, 0, 0),        //
          superbin_t(16, 0, 0, 92, 0, 0, 1, 0, 0),       superbin_t(20, 0, 0, 93, 0, 0, 1, 0, 0),        //
          superbin_t(24, 0, 0, 94, 0, 0, 1, 0, 0),       superbin_t(28, 0, 0, 95, 0, 0, 1, 0, 0),        //
          superbin_t(32, 0, 0, 96, 0, 0, 1, 0, 0),       superbin_t(40, 0, 0, 97, 0, 0, 1, 0, 0),        //
          superbin_t(48, 0, 0, 98, 0, 0, 1, 0, 0),       superbin_t(56, 0, 0, 99, 0, 0, 1, 0, 0),        //
          superbin_t(64, 0, 0, 100, 0, 0, 1, 0, 0),      superbin_t(80, 0, 0, 101, 0, 0, 1, 0, 0),       //
          superbin_t(96, 0, 0, 102, 0, 0, 1, 0, 0),      superbin_t(112, 0, 0, 103, 0, 0, 1, 0, 0),      //
          superbin_t(128, 0, 0, 104, 0, 0, 1, 0, 0),     superbin_t(160, 0, 0, 105, 0, 0, 1, 0, 0),      //
          superbin_t(192, 0, 0, 106, 0, 0, 1, 0, 0),     superbin_t(224, 0, 0, 107, 0, 0, 1, 0, 0),      //
          superbin_t(256, 0, 0, 108, 0, 0, 1, 0, 0),     superbin_t(320, 0, 0, 109, 0, 0, 1, 0, 0),      //
          superbin_t(384, 0, 0, 110, 0, 0, 1, 0, 0),     superbin_t(448, 0, 0, 111, 0, 0, 1, 0, 0),      //
        };

        static superallocator_config_t get_config()
        {
            const u64 c_total_address_space         = 64 * cGB;
            const u64 c_segment_address_range       = 4 * cGB;
            const u32 c_internal_heap_address_range = 16 * cMB;
            const u32 c_internal_heap_pre_size      = 2 * cMB;
            const u32 c_internal_fsa_address_range  = 16 * cMB;
            const u32 c_internal_fsa_pre_size       = 2 * cMB;
            return superallocator_config_t(c_total_address_space, c_segment_address_range, c_num_bins, c_asbins, c_num_ssegments, c_assegments, c_internal_heap_address_range, c_internal_heap_pre_size, c_internal_fsa_address_range, c_internal_fsa_pre_size);
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

    };  // namespace superallocator_config_desktop_app_25p_t

    // 10% allocation waste (based on empirical data)
    namespace superallocator_config_desktop_app_10p_t
    {
        // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly
        static const s32                   c_num_ssegments               = 2;
        static const supersegment_config_t c_assegments[c_num_ssegments] = {
          supersegment_config_t(cGB * 1, 64 * cKB, 0, 0),  //
          supersegment_config_t(cGB * 1, 4 * cKB, 0, 0)    //
        };

        // superbin_t(alloc-size MB, KB, B, bin redir index, allocator / supersegment index, use binmap?, max alloc-count, binmap level 1 length (u16), binmap level 2 length (u16))
        static const s32        c_num_bins           = 216;
        static const superbin_t c_asbins[c_num_bins] = {
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),    //
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),    //
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),    //
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),    //
          superbin_t(0, 0, 8, 8, 0, 1, 8192, 32, 512),   superbin_t(0, 0, 9, 12, 0, 1, 7281, 32, 512),   //
          superbin_t(0, 0, 10, 12, 0, 1, 6553, 32, 512), superbin_t(0, 0, 11, 12, 0, 1, 5957, 32, 512),  //
          superbin_t(0, 0, 12, 12, 0, 1, 5461, 32, 512), superbin_t(0, 0, 13, 16, 0, 1, 5041, 32, 512),  //
          superbin_t(0, 0, 14, 16, 0, 1, 4681, 32, 512), superbin_t(0, 0, 15, 16, 0, 1, 4369, 32, 512),  //
          superbin_t(0, 0, 16, 16, 0, 1, 4096, 16, 256), superbin_t(0, 0, 18, 18, 0, 1, 3640, 16, 256),  //
          superbin_t(0, 0, 20, 18, 0, 1, 3276, 16, 256), superbin_t(0, 0, 22, 20, 0, 1, 2978, 16, 256),  //
          superbin_t(0, 0, 24, 20, 0, 1, 2730, 16, 256), superbin_t(0, 0, 26, 22, 0, 1, 2520, 16, 256),  //
          superbin_t(0, 0, 28, 22, 0, 1, 2340, 16, 256), superbin_t(0, 0, 30, 24, 0, 1, 2184, 16, 256),  //
          superbin_t(0, 0, 32, 24, 0, 1, 2048, 8, 128),  superbin_t(0, 0, 36, 25, 0, 1, 1820, 8, 128),   //
          superbin_t(0, 0, 40, 26, 0, 1, 1638, 8, 128),  superbin_t(0, 0, 44, 27, 0, 1, 1489, 8, 128),   //
          superbin_t(0, 0, 48, 28, 0, 1, 1365, 8, 128),  superbin_t(0, 0, 52, 29, 0, 1, 1260, 8, 128),   //
          superbin_t(0, 0, 56, 30, 0, 1, 1170, 8, 128),  superbin_t(0, 0, 60, 31, 0, 1, 1092, 8, 128),   //
          superbin_t(0, 0, 64, 32, 0, 1, 1024, 4, 64),   superbin_t(0, 0, 72, 33, 0, 1, 910, 4, 64),     //
          superbin_t(0, 0, 80, 34, 0, 1, 819, 4, 64),    superbin_t(0, 0, 88, 35, 0, 1, 744, 4, 64),     //
          superbin_t(0, 0, 96, 36, 0, 1, 682, 4, 64),    superbin_t(0, 0, 104, 37, 0, 1, 630, 4, 64),    //
          superbin_t(0, 0, 112, 38, 0, 1, 585, 4, 64),   superbin_t(0, 0, 120, 39, 0, 1, 546, 4, 64),    //
          superbin_t(0, 0, 128, 40, 0, 1, 512, 2, 32),   superbin_t(0, 0, 144, 41, 0, 1, 455, 2, 32),    //
          superbin_t(0, 0, 160, 42, 0, 1, 409, 2, 32),   superbin_t(0, 0, 176, 43, 0, 1, 372, 2, 32),    //
          superbin_t(0, 0, 192, 44, 0, 1, 341, 2, 32),   superbin_t(0, 0, 208, 45, 0, 1, 315, 2, 32),    //
          superbin_t(0, 0, 224, 46, 0, 1, 292, 2, 32),   superbin_t(0, 0, 240, 47, 0, 1, 273, 2, 32),    //
          superbin_t(0, 0, 256, 48, 0, 1, 256, 2, 16),   superbin_t(0, 0, 288, 49, 0, 1, 227, 2, 16),    //
          superbin_t(0, 0, 320, 50, 0, 1, 204, 2, 16),   superbin_t(0, 0, 352, 51, 0, 1, 186, 2, 16),    //
          superbin_t(0, 0, 384, 52, 0, 1, 170, 2, 16),   superbin_t(0, 0, 416, 53, 0, 1, 157, 2, 16),    //
          superbin_t(0, 0, 448, 54, 0, 1, 146, 2, 16),   superbin_t(0, 0, 480, 55, 0, 1, 136, 2, 16),    //
          superbin_t(0, 0, 512, 56, 0, 1, 128, 2, 8),    superbin_t(0, 0, 576, 57, 0, 1, 113, 2, 8),     //
          superbin_t(0, 0, 640, 58, 0, 1, 102, 2, 8),    superbin_t(0, 0, 704, 59, 0, 1, 93, 2, 8),      //
          superbin_t(0, 0, 768, 60, 0, 1, 85, 2, 8),     superbin_t(0, 0, 832, 61, 0, 1, 78, 2, 8),      //
          superbin_t(0, 0, 896, 62, 0, 1, 73, 2, 8),     superbin_t(0, 0, 960, 63, 0, 1, 68, 2, 8),      //
          superbin_t(0, 1, 0, 64, 0, 1, 64, 2, 4),       superbin_t(0, 1, 128, 65, 0, 1, 113, 2, 8),     //
          superbin_t(0, 1, 256, 66, 0, 1, 51, 2, 4),     superbin_t(0, 1, 384, 67, 0, 1, 93, 2, 8),      //
          superbin_t(0, 1, 512, 68, 0, 1, 85, 2, 8),     superbin_t(0, 1, 640, 69, 0, 1, 39, 2, 4),      //
          superbin_t(0, 1, 768, 70, 0, 1, 73, 2, 8),     superbin_t(0, 1, 896, 71, 0, 1, 34, 2, 4),      //
          superbin_t(0, 2, 0, 72, 0, 1, 32, 0, 0),       superbin_t(0, 2, 256, 73, 0, 1, 113, 2, 8),     //
          superbin_t(0, 2, 512, 74, 0, 1, 51, 2, 4),     superbin_t(0, 2, 768, 75, 0, 1, 93, 2, 8),      //
          superbin_t(0, 3, 0, 76, 0, 1, 85, 2, 8),       superbin_t(0, 3, 256, 77, 0, 1, 39, 2, 4),      //
          superbin_t(0, 3, 512, 78, 0, 1, 73, 2, 8),     superbin_t(0, 3, 768, 79, 0, 1, 17, 0, 0),      //
          superbin_t(0, 4, 0, 80, 0, 1, 16, 0, 0),       superbin_t(0, 4, 512, 81, 0, 1, 113, 2, 8),     //
          superbin_t(0, 5, 0, 82, 0, 1, 51, 2, 4),       superbin_t(0, 5, 512, 83, 0, 1, 93, 2, 8),      //
          superbin_t(0, 6, 0, 84, 0, 1, 32, 2, 4),       superbin_t(0, 6, 512, 85, 0, 1, 39, 2, 4),      //
          superbin_t(0, 7, 0, 86, 0, 1, 73, 2, 8),       superbin_t(0, 7, 512, 87, 0, 1, 17, 0, 0),      //
          superbin_t(0, 8, 0, 88, 0, 1, 8, 0, 0),        superbin_t(0, 9, 0, 89, 0, 1, 113, 2, 8),       //
          superbin_t(0, 10, 0, 90, 0, 1, 51, 2, 4),      superbin_t(0, 11, 0, 91, 0, 1, 29, 2, 4),       //
          superbin_t(0, 12, 0, 92, 0, 1, 16, 0, 0),      superbin_t(0, 13, 0, 93, 0, 1, 39, 2, 4),       //
          superbin_t(0, 14, 0, 94, 0, 1, 32, 2, 4),      superbin_t(0, 15, 0, 95, 0, 1, 17, 0, 0),       //
          superbin_t(0, 16, 0, 96, 0, 1, 4, 0, 0),       superbin_t(0, 18, 0, 97, 0, 1, 53, 2, 4),       //
          superbin_t(0, 20, 0, 98, 0, 1, 16, 0, 0),      superbin_t(0, 22, 0, 99, 0, 1, 32, 2, 4),       //
          superbin_t(0, 24, 0, 100, 0, 1, 8, 0, 0),      superbin_t(0, 26, 0, 101, 0, 1, 39, 2, 4),      //
          superbin_t(0, 28, 0, 102, 0, 1, 16, 0, 0),     superbin_t(0, 30, 0, 103, 0, 1, 17, 0, 0),      //
          superbin_t(0, 32, 0, 104, 0, 1, 2, 0, 0),      superbin_t(0, 36, 0, 105, 0, 1, 23, 0, 0),      //
          superbin_t(0, 40, 0, 106, 0, 1, 8, 0, 0),      superbin_t(0, 44, 0, 107, 0, 1, 16, 0, 0),      //
          superbin_t(0, 48, 0, 108, 0, 1, 4, 0, 0),      superbin_t(0, 52, 0, 109, 0, 1, 16, 0, 0),      //
          superbin_t(0, 56, 0, 110, 0, 1, 8, 0, 0),      superbin_t(0, 60, 0, 111, 0, 1, 17, 0, 0),      //
          superbin_t(0, 64, 0, 112, 0, 0, 1, 0, 0),      superbin_t(0, 72, 0, 113, 0, 1, 8, 0, 0),       //
          superbin_t(0, 80, 0, 114, 0, 1, 4, 0, 0),      superbin_t(0, 88, 0, 115, 0, 1, 8, 0, 0),       //
          superbin_t(0, 96, 0, 116, 0, 1, 2, 0, 0),      superbin_t(0, 104, 0, 117, 0, 1, 8, 0, 0),      //
          superbin_t(0, 112, 0, 118, 0, 1, 4, 0, 0),     superbin_t(0, 120, 0, 119, 0, 1, 8, 0, 0),      //
          superbin_t(0, 128, 0, 120, 0, 0, 1, 0, 0),     superbin_t(0, 144, 0, 121, 0, 1, 4, 0, 0),      //
          superbin_t(0, 160, 0, 122, 0, 1, 2, 0, 0),     superbin_t(0, 176, 0, 123, 0, 1, 4, 0, 0),      //
          superbin_t(0, 192, 0, 124, 0, 0, 1, 0, 0),     superbin_t(0, 208, 0, 125, 0, 1, 4, 0, 0),      //
          superbin_t(0, 224, 0, 126, 0, 1, 2, 0, 0),     superbin_t(0, 240, 0, 127, 0, 1, 4, 0, 0),      //
          superbin_t(0, 256, 0, 128, 0, 0, 1, 0, 0),     superbin_t(0, 288, 0, 129, 0, 1, 2, 0, 0),      //
          superbin_t(0, 320, 0, 130, 0, 0, 1, 0, 0),     superbin_t(0, 352, 0, 131, 0, 1, 2, 0, 0),      //
          superbin_t(0, 384, 0, 132, 0, 0, 1, 0, 0),     superbin_t(0, 416, 0, 133, 0, 1, 2, 0, 0),      //
          superbin_t(0, 448, 0, 134, 0, 0, 1, 0, 0),     superbin_t(0, 480, 0, 135, 0, 1, 2, 0, 0),      //
          superbin_t(0, 512, 0, 136, 0, 0, 1, 0, 0),     superbin_t(0, 576, 0, 137, 0, 0, 1, 0, 0),      //
          superbin_t(0, 640, 0, 138, 0, 0, 1, 0, 0),     superbin_t(0, 704, 0, 139, 0, 0, 1, 0, 0),      //
          superbin_t(0, 768, 0, 140, 0, 0, 1, 0, 0),     superbin_t(0, 832, 0, 141, 0, 0, 1, 0, 0),      //
          superbin_t(0, 896, 0, 142, 0, 0, 1, 0, 0),     superbin_t(0, 960, 0, 143, 0, 0, 1, 0, 0),      //
          superbin_t(1, 0, 0, 144, 0, 0, 1, 0, 0),       superbin_t(1, 128, 0, 145, 0, 0, 1, 0, 0),      //
          superbin_t(1, 256, 0, 146, 0, 0, 1, 0, 0),     superbin_t(1, 384, 0, 147, 0, 0, 1, 0, 0),      //
          superbin_t(1, 512, 0, 148, 0, 0, 1, 0, 0),     superbin_t(1, 640, 0, 149, 0, 0, 1, 0, 0),      //
          superbin_t(1, 768, 0, 150, 0, 0, 1, 0, 0),     superbin_t(1, 896, 0, 151, 0, 0, 1, 0, 0),      //
          superbin_t(2, 0, 0, 152, 0, 0, 1, 0, 0),       superbin_t(2, 256, 0, 153, 0, 0, 1, 0, 0),      //
          superbin_t(2, 512, 0, 154, 0, 0, 1, 0, 0),     superbin_t(2, 768, 0, 155, 0, 0, 1, 0, 0),      //
          superbin_t(3, 0, 0, 156, 0, 0, 1, 0, 0),       superbin_t(3, 256, 0, 157, 0, 0, 1, 0, 0),      //
          superbin_t(3, 512, 0, 158, 0, 0, 1, 0, 0),     superbin_t(3, 768, 0, 159, 0, 0, 1, 0, 0),      //
          superbin_t(4, 0, 0, 160, 0, 0, 1, 0, 0),       superbin_t(4, 512, 0, 161, 0, 0, 1, 0, 0),      //
          superbin_t(5, 0, 0, 162, 0, 0, 1, 0, 0),       superbin_t(5, 512, 0, 163, 0, 0, 1, 0, 0),      //
          superbin_t(6, 0, 0, 164, 0, 0, 1, 0, 0),       superbin_t(6, 512, 0, 165, 0, 0, 1, 0, 0),      //
          superbin_t(7, 0, 0, 166, 0, 0, 1, 0, 0),       superbin_t(7, 512, 0, 167, 0, 0, 1, 0, 0),      //
          superbin_t(8, 0, 0, 168, 0, 0, 1, 0, 0),       superbin_t(9, 0, 0, 169, 0, 0, 1, 0, 0),        //
          superbin_t(10, 0, 0, 170, 0, 0, 1, 0, 0),      superbin_t(11, 0, 0, 171, 0, 0, 1, 0, 0),       //
          superbin_t(12, 0, 0, 172, 0, 0, 1, 0, 0),      superbin_t(13, 0, 0, 173, 0, 0, 1, 0, 0),       //
          superbin_t(14, 0, 0, 174, 0, 0, 1, 0, 0),      superbin_t(15, 0, 0, 175, 0, 0, 1, 0, 0),       //
          superbin_t(16, 0, 0, 176, 0, 0, 1, 0, 0),      superbin_t(18, 0, 0, 177, 0, 0, 1, 0, 0),       //
          superbin_t(20, 0, 0, 178, 0, 0, 1, 0, 0),      superbin_t(22, 0, 0, 179, 0, 0, 1, 0, 0),       //
          superbin_t(24, 0, 0, 180, 0, 0, 1, 0, 0),      superbin_t(26, 0, 0, 181, 0, 0, 1, 0, 0),       //
          superbin_t(28, 0, 0, 182, 0, 0, 1, 0, 0),      superbin_t(30, 0, 0, 183, 0, 0, 1, 0, 0),       //
          superbin_t(32, 0, 0, 184, 0, 0, 1, 0, 0),      superbin_t(36, 0, 0, 185, 0, 0, 1, 0, 0),       //
          superbin_t(40, 0, 0, 186, 0, 0, 1, 0, 0),      superbin_t(44, 0, 0, 187, 0, 0, 1, 0, 0),       //
          superbin_t(48, 0, 0, 188, 0, 0, 1, 0, 0),      superbin_t(52, 0, 0, 189, 0, 0, 1, 0, 0),       //
          superbin_t(56, 0, 0, 190, 0, 0, 1, 0, 0),      superbin_t(60, 0, 0, 191, 0, 0, 1, 0, 0),       //
          superbin_t(64, 0, 0, 192, 0, 0, 1, 0, 0),      superbin_t(72, 0, 0, 193, 0, 0, 1, 0, 0),       //
          superbin_t(80, 0, 0, 194, 0, 0, 1, 0, 0),      superbin_t(88, 0, 0, 195, 0, 0, 1, 0, 0),       //
          superbin_t(96, 0, 0, 196, 0, 0, 1, 0, 0),      superbin_t(104, 0, 0, 197, 0, 0, 1, 0, 0),      //
          superbin_t(112, 0, 0, 198, 0, 0, 1, 0, 0),     superbin_t(120, 0, 0, 199, 0, 0, 1, 0, 0),      //
          superbin_t(128, 0, 0, 200, 0, 0, 1, 0, 0),     superbin_t(144, 0, 0, 201, 0, 0, 1, 0, 0),      //
          superbin_t(160, 0, 0, 202, 0, 0, 1, 0, 0),     superbin_t(176, 0, 0, 203, 0, 0, 1, 0, 0),      //
          superbin_t(192, 0, 0, 204, 0, 0, 1, 0, 0),     superbin_t(208, 0, 0, 205, 0, 0, 1, 0, 0),      //
          superbin_t(224, 0, 0, 206, 0, 0, 1, 0, 0),     superbin_t(240, 0, 0, 207, 0, 0, 1, 0, 0),      //
          superbin_t(256, 0, 0, 208, 0, 0, 1, 0, 0),     superbin_t(288, 0, 0, 209, 0, 0, 1, 0, 0),      //
          superbin_t(320, 0, 0, 210, 0, 0, 1, 0, 0),     superbin_t(352, 0, 0, 211, 0, 0, 1, 0, 0),      //
          superbin_t(384, 0, 0, 212, 0, 0, 1, 0, 0),     superbin_t(416, 0, 0, 213, 0, 0, 1, 0, 0),      //
          superbin_t(448, 0, 0, 214, 0, 0, 1, 0, 0),     superbin_t(480, 0, 0, 215, 0, 0, 1, 0, 0),      //
        };

        static superallocator_config_t get_config()
        {
            const u64 c_total_address_space         = 64 * cGB;
            const u64 c_segment_address_range       = 4 * cGB;
            const u32 c_internal_heap_address_range = 16 * cMB;
            const u32 c_internal_heap_pre_size      = 2 * cMB;
            const u32 c_internal_fsa_address_range  = 16 * cMB;
            const u32 c_internal_fsa_pre_size       = 2 * cMB;
            return superallocator_config_t(c_total_address_space, c_segment_address_range, c_num_bins, c_asbins, c_num_ssegments, c_assegments, c_internal_heap_address_range, c_internal_heap_pre_size, c_internal_fsa_address_range, c_internal_fsa_pre_size);
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

    };  // namespace superallocator_config_desktop_app_10p_t

    namespace superallocator_config = superallocator_config_desktop_app_10p_t;
    // namespace superallocator_config = superallocator_config_desktop_app_25p_t;

    class superallocator_t : public valloc_t
    {
        void*                   m_vmem_membase;
        supersegment_t*         m_asupersegments;
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

        inline supersegment_t* get_supersegment(void* ptr) const
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

        const u32 num_segments = (u32)(config.m_total_address_space / config.m_segment_address_range);
        m_asupersegments       = (supersegment_t*)m_internal_heap.allocate(sizeof(supersegment_t) * num_segments);
        for (u32 i = 0; i < num_segments; ++i)
        {
            m_asupersegments[i] = supersegment_t();
            m_aallocators[i]    = superalloc_t();
        }
        for (u32 i = 0; i < m_config.m_num_supersegments; ++i)
        {
            m_asupersegments[i].initialize(vmem, config.m_segment_address_range, m_config.m_asupersegmentconfigs[i].c_block_range, m_config.m_asupersegmentconfigs[i].c_chunk_size, &m_internal_heap, &m_internal_fsa);
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
        supersegment_t* ssegment = get_supersegment(ptr);
        ASSERT(ptr >= ssegment->m_address_base && ptr < ((u8*)ssegment->m_address_base + ssegment->m_address_range));
        u32 const                   chunk_index = ssegment->address_to_page_index(ptr);
        supersegment_t::chunkinfo_t chunk_info  = ssegment->chunk_index_to_chunk_info(chunk_index);
        supersegment_t::chunk_t*    pchunk      = (supersegment_t::chunk_t*)m_internal_fsa.idx2ptr(chunk_info.m_chunk_iptr);
        u32 const                   sbinindex   = pchunk->m_bin_index;
        u32 const                   sallocindex = m_config.m_asuperbins[sbinindex].m_alloc_index;
        u32 const                   size        = m_aallocators[sallocindex].deallocate(m_internal_fsa, ptr, chunk_info, m_config.m_asuperbins[sbinindex]);
        ASSERT(size <= m_config.m_asuperbins[sbinindex].m_alloc_size);
        return size;
    }

    void superallocator_t::v_set_tag(void* ptr, u32 assoc)
    {
        if (ptr != nullptr)
        {
            supersegment_t* ssegment = get_supersegment(ptr);
            ASSERT(ptr >= ssegment->m_address_base && ptr < ((u8*)ssegment->m_address_base + ssegment->m_address_range));
            u32 const                   chunk_index = ssegment->address_to_page_index(ptr);
            supersegment_t::chunkinfo_t chunk_info  = ssegment->chunk_index_to_chunk_info(chunk_index);
            supersegment_t::chunk_t*    pchunk      = (supersegment_t::chunk_t*)m_internal_fsa.idx2ptr(chunk_info.m_chunk_iptr);
            u32 const                   sbinindex   = pchunk->m_bin_index;
            u32 const                   sallocindex = m_config.m_asuperbins[sbinindex].m_alloc_index;
            m_aallocators[sallocindex].set_assoc(ptr, assoc, chunk_info, m_config.m_asuperbins[sbinindex]);
        }
    }

    u32 superallocator_t::v_get_tag(void* ptr) const
    {
        if (ptr == nullptr)
            return 0xffffffff;
        supersegment_t* ssegment = get_supersegment(ptr);
        ASSERT(ptr >= ssegment->m_address_base && ptr < ((u8*)ssegment->m_address_base + ssegment->m_address_range));
        u32 const                   chunk_index = ssegment->address_to_page_index(ptr);
        supersegment_t::chunkinfo_t chunk_info  = ssegment->chunk_index_to_chunk_info(chunk_index);
        supersegment_t::chunk_t*    pchunk      = (supersegment_t::chunk_t*)m_internal_fsa.idx2ptr(chunk_info.m_chunk_iptr);
        u32 const                   sbinindex   = pchunk->m_bin_index;
        u32 const                   sallocindex = m_config.m_asuperbins[sbinindex].m_alloc_index;
        return m_aallocators[sallocindex].get_assoc(ptr, chunk_info, m_config.m_asuperbins[sbinindex]);
    }

    u32 superallocator_t::v_get_size(void* ptr) const
    {
        if (ptr == nullptr)
            return 0;
        supersegment_t* ssegment = get_supersegment(ptr);
        ASSERT(ptr >= ssegment->m_address_base && ptr < ((u8*)ssegment->m_address_base + ssegment->m_address_range));
        u32 const                   chunk_index = ssegment->address_to_page_index(ptr);
        supersegment_t::chunkinfo_t chunk_info  = ssegment->chunk_index_to_chunk_info(chunk_index);
        supersegment_t::chunk_t*    pchunk      = (supersegment_t::chunk_t*)m_internal_fsa.idx2ptr(chunk_info.m_chunk_iptr);
        u32 const                   sbinindex   = pchunk->m_bin_index;
        if (m_config.m_asuperbins[sbinindex].m_use_binmap == 1)
        {
            return m_config.m_asuperbins[sbinindex].m_alloc_size;
        }
        else
        {
            supersegment_t::block_t* block = &ssegment->m_blocks_array[chunk_info.m_segment_block_index];
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
