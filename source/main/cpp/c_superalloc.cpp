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

    // Can only allocate, used internally to allocate initially required memory
    class superheap_t
    {
    public:
        void*   m_address;
        u64     m_address_range;
        vmem_t* m_vmem;
        u32     m_allocsize_alignment;
        u32     m_block_size;
        u32     m_page_count_current;
        u32     m_page_count_maximum;
        u64     m_ptr;

        void initialize(vmem_t* vmem, u64 memory_range, u64 size_to_pre_allocate);
        void deinitialize();

        void* allocate(u32 size);
        void  deallocate(void* ptr);
    };

    void superheap_t::initialize(vmem_t* vmem, u64 memory_range, u64 size_to_pre_allocate)
    {
        u32 attributes  = 0;
        m_vmem          = vmem;
        m_address_range = memory_range;
        m_vmem->reserve(memory_range, m_block_size, attributes, m_address);
        m_allocsize_alignment = 32;
        m_page_count_maximum  = (u32)(memory_range / m_block_size);
        m_page_count_current  = 0;
        m_ptr                 = 0;

        if (size_to_pre_allocate > 0)
        {
            u32 const pages_to_commit = (u32)(math::alignUp(size_to_pre_allocate, (u64)m_block_size) / m_block_size);
            m_vmem->commit(m_address, m_block_size, pages_to_commit);
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
        m_block_size          = 0;
        m_page_count_current  = 0;
        m_page_count_maximum  = 0;
        m_ptr                 = 0;
    }

    void* superheap_t::allocate(u32 size)
    {
        size        = math::alignUp(size, m_allocsize_alignment);
        u64 ptr_max = ((u64)m_page_count_current * m_block_size);
        if ((m_ptr + size) > ptr_max)
        {
            // add more pages
            u32 const page_count           = (u32)(math::alignUp(m_ptr + size, (u64)m_block_size) / (u64)m_block_size);
            u32 const page_count_to_commit = page_count - m_page_count_current;
            u64       commit_base          = ((u64)m_page_count_current * m_block_size);
            m_vmem->commit(toaddress(m_address, commit_base), m_block_size, page_count_to_commit);
            m_page_count_current += page_count_to_commit;
        }
        u64 const offset = m_ptr;
        m_ptr += size;
        return toaddress(m_address, offset);
    }

    void superheap_t::deallocate(void* ptr)
    {
        ASSERT(ptr >= m_address);
        ASSERT(ptr >= m_address && (((uint_t)ptr - (uint_t)m_address) < (uint_t)m_address_range));  // Purely some validation
    }

    struct superblock_t
    {
        static const u16 NIL = 0xffff;

        u16 m_item_freepos;
        u16 m_item_count;
        u16 m_item_max;
        u16 m_item_freelist;
        s16 m_item_size_shift;

        void initialize(s16 item_size_shift)
        {
            ASSERT(item_size_shift >= 1);
            m_item_count      = 0;
            m_item_freepos    = 0;
            m_item_freelist   = NIL;
            m_item_size_shift = item_size_shift;
        }

        inline bool is_full() const { return m_item_count == m_item_max; }
        inline bool is_empty() const { return m_item_count == 0; }
        inline u32  ptr2idx(void const* const ptr, void const* const elem) const { return (u32)(((u64)elem - (u64)ptr) >> m_item_size_shift); }
        inline u32* idx2ptr(void* const ptr, u32 const index) const { return (u32*)((u8*)ptr + ((u32)index << m_item_size_shift)); }

        u16 allocate_item(void* page_address)
        {
            if (m_item_freelist != NIL)
            {
                u16 const  iitem = m_item_freelist;
                u16* const pitem = (u16*)idx2ptr(page_address, iitem);
                m_item_freelist  = pitem[0];
                m_item_count++;
                return iitem;
            }
            else if (m_item_count < m_item_max)
            {
                u16 const ielem = m_item_freepos;
                m_item_freepos++;
                m_item_count++;
                return ielem;
            }
            return NIL;  // panic
        }

        void deallocate(void* page_address, u16 item_index)
        {
            ASSERT(m_item_count > 0);
            ASSERT(item_index < m_item_freepos);
            u16* const pelem = (u16*)idx2ptr(page_address, item_index);
#ifdef SUPERALLOC_DEBUG
            nmem::memset(pelem, 0xFEFEFEFE, ((u64)1 << m_item_size_shift));
#endif
            pelem[0]        = m_item_freelist;
            m_item_freelist = item_index;
            m_item_count--;
        }
    };

    struct superblocks_t
    {
        vmem_t*       m_vmem;
        void*         m_address;
        u64           m_address_range;
        u32           m_page_size;
        u32           m_block_count;
        u32           m_block_index;
        u32           m_block_size;
        superblock_t* m_block_array;
        lldata_t      m_block_list_data;
        llist_t       m_free_block_list;
        llist_t       m_cached_block_list;

        void  initialize(superheap_t& heap, vmem_t* vmem, void* address, u64 address_range, u8 block_index, u32 block_size, u32 size_to_pre_allocate);
        void  deinitialize(superheap_t& heap);
        u32   checkout_block(u32 const alloc_size);
        void  release_block(u32 ipage);
        void* address_of_block(u32 ipage) const { return toaddress(m_address, (u64)ipage * m_block_size); }

        inline void* idx2ptr(u32 i) const
        {
            if (i == 0xffffffff)
                return nullptr;
            u16 const                 block_index = i >> 16;
            u16 const                 itemindex   = i & 0xFFFF;
            superblock_t const* const pblock      = &m_block_array[block_index];
            void* const               paddr       = address_of_block(block_index);
            return pblock->idx2ptr(paddr, itemindex);
        }

        inline u32 ptr2idx(void const* ptr) const
        {
            if (ptr == nullptr)
                return 0xffffffff;
            u32 const                 block_index = (u32)(todistance(m_address, ptr) / m_block_size);
            superblock_t const* const pblock      = &m_block_array[block_index];
            void* const               paddr       = address_of_block(block_index);
            u32 const                 itemindex   = pblock->ptr2idx(paddr, ptr);
            return (block_index << 16) | (itemindex & 0xFFFF);
        }
    };

    void superblocks_t::initialize(superheap_t& heap, vmem_t* vmem, void* address, u64 address_range, u8 block_index, u32 block_size, u32 size_to_pre_allocate)
    {
        m_block_index   = block_index;
        m_block_size    = block_size;
        m_vmem          = vmem;
        m_address       = address;
        m_address_range = address_range;

        m_block_count = (u32)(address_range / (u64)m_block_size);
        m_block_array = (superblock_t*)heap.allocate(m_block_count * sizeof(superblock_t));
#ifdef SUPERALLOC_DEBUG
        nmem::memset(m_block_array, 0xCDCDCDCD, m_block_count * sizeof(superblock_t));
#endif
        m_block_list_data.m_data     = heap.allocate(m_block_count * sizeof(llnode_t));
        m_block_list_data.m_itemsize = sizeof(llnode_t);
        m_block_list_data.m_pagesize = m_block_count * sizeof(llnode_t);

        u32 const num_blocks_to_cache = math::alignUp(size_to_pre_allocate, m_block_size) / m_block_size;
        ASSERT(num_blocks_to_cache <= m_block_count);
        m_free_block_list.initialize(m_block_list_data, num_blocks_to_cache, m_block_count - num_blocks_to_cache, m_block_count);
        if (num_blocks_to_cache > 0)
        {
            m_cached_block_list.initialize(m_block_list_data, 0, num_blocks_to_cache, num_blocks_to_cache);
            m_vmem->commit(m_address, m_block_size, num_blocks_to_cache);
        }
    }

    void superblocks_t::deinitialize(superheap_t& heap)
    {
        // NOTE: Do we need to decommit physical pages, or is 'release' enough?
        m_vmem->release(m_address, m_address_range);
    }

    u32 superblocks_t::checkout_block(u32 const alloc_size)
    {
        // Get a page and initialize that page for this size
        u32 ipage = llnode_t::NIL;
        if (!m_cached_block_list.is_empty())
        {
            ipage = m_cached_block_list.remove_headi(m_block_list_data);
        }
        else if (!m_free_block_list.is_empty())
        {
            ipage       = m_free_block_list.remove_headi(m_block_list_data);
            void* apage = address_of_block(ipage);
            m_vmem->commit(apage, m_block_size, 1);
        }
#ifdef SUPERALLOC_DEBUG
        u64* apage = (u64*)address_of_block(ipage);
        nmem::memset(apage, 0xCDCDCDCD, m_block_size);
#endif
        superblock_t* ppage = &m_block_array[ipage];
        ppage->initialize(alloc_size);
        return ipage;
    }

    void superblocks_t::release_block(u32 block_index)
    {
        superblock_t* const pblock = &m_block_array[block_index];
#ifdef SUPERALLOC_DEBUG
        nmem::memset(pblock, 0xFEFEFEFE, sizeof(superblock_t));
#endif
        if (!m_cached_block_list.is_full())
        {
            m_cached_block_list.insert(m_block_list_data, block_index);
        }
        else
        {
            void* const paddr = address_of_block(block_index);
            m_vmem->decommit(paddr, m_block_size, 1);
            m_free_block_list.insert(m_block_list_data, block_index);
        }
    }

    // @note: The format of the returned index is u32[u4(superblocks-index):u12(superblock-index):u16(item-index)]
    class superfsa_t
    {
    public:
        static const u32 NIL = 0xffffffff;

        void initialize(superheap_t& heap, vmem_t* vmem, u64 address_range);
        void deinitialize(superheap_t& heap);
        u32  sizeof_alloc(u32 size) const;

        u32  alloc(u32 size);
        void dealloc(u32 index);

        void* allocptr(u32 size) { return idx2ptr(alloc(size)); }
        void  deallocptr(void* ptr) { dealloc(ptr2idx(ptr)); }

        inline void* idx2ptr(u32 i) const
        {
            u32 const c = (i >> 28) & 0xF;
            return m_blocks[c].idx2ptr(i);
        }

        inline u32 ptr2idx(void const* ptr) const
        {
            u32 const block_index = (u32)(todistance(m_address_base, ptr) >> m_block_size_shift);
            return m_blocks[block_index].ptr2idx(ptr);
        }

        template <typename T>
        inline T* idx2ptr(u32 i) const
        {
            u32 const block_index = (i >> 28) & 0xF;
            return (T*)m_blocks[block_index].idx2ptr(i & 0xFFFFFFF);
        }

        inline void* baseptr(u32 alloc_size) const
        {
            u32 const c = alloc_size_to_index(alloc_size);
            u32 const b = c_aalloc_config[c].m_block_index;
            return m_blocks[b].m_address;
        }

        inline u32 blocksize(u32 alloc_size) const
        {
            u32 const c = alloc_size_to_index(alloc_size);
            return c_ablock_config[c].m_block_size;
        }

    private:
        struct blockconfig_t
        {
            u32 m_block_size;
            u32 m_preallocate;
        };
        struct allocconfig_t
        {
            u32 m_alloc_size;
            u32 m_block_index;
        };

        static const u32           c_max_num_blocks;
        static const u32           c_max_num_sizes;
        static const blockconfig_t c_ablock_config[];
        static const allocconfig_t c_aalloc_config[];

        static inline s8 alloc_size_to_index(u32 alloc_size)
        {
            alloc_size = (alloc_size + 7) & ~7;
            alloc_size = math::ceilpo2(alloc_size);
            s8 const c = math::countTrailingZeros(alloc_size) - 3;
            return c;
        }

        vmem_t*        m_vmem;
        void*          m_address_base;
        u64            m_address_range;
        s8             m_block_size_shift;
        superblocks_t* m_blocks;
        llhead_t*      m_used_block_list_per_size;
    };

    // clang-format off
    const superfsa_t::blockconfig_t superfsa_t::c_ablock_config[] = 
    {   // block-size, preallocate
        {         64 * 1024,  64 * 1024}, 
        {        256 * 1024, 256 * 1024}, 
        {   1 * 1024 * 1024,          0}, 
        {   4 * 1024 * 1024,          0}, 
    };
    // clang-format on

    // clang-format off
    const superfsa_t::allocconfig_t superfsa_t::c_aalloc_config[] = 
    {   // alloc-size, block-index
        {          8,  0}, 
        {         16,  0}, 
        {         32,  0}, 
        {         64,  0}, 
        {        128,  0}, 
        {        256,  0}, 
        {        512,  0}, 
        {       1024,  0}, 
        {       2048,  0}, 
        {       4096,  0}, 
        {       8192,  0}, 
        {      16384,  0}, 
        {      32768,  1}, 
        {  64 * 1024,  1}, 
        { 128 * 1024,  2}, 
        { 256 * 1024,  2}, 
        { 512 * 1024,  3}, 
        {1024 * 1024,  3}
    };
    // clang-format on

    const u32 superfsa_t::c_max_num_blocks = sizeof(c_ablock_config) / sizeof(superfsa_t::blockconfig_t);
    const u32 superfsa_t::c_max_num_sizes  = sizeof(c_aalloc_config) / sizeof(superfsa_t::allocconfig_t);

    void superfsa_t::initialize(superheap_t& heap, vmem_t* vmem, u64 address_range)
    {
        m_vmem          = vmem;
        m_address_range = address_range;
        m_blocks        = (superblocks_t*)heap.allocate(sizeof(superblocks_t) * c_max_num_blocks);

        u32 attributes = 0;
        u32 page_size  = 0;
        m_vmem->reserve(address_range, page_size, attributes, m_address_base);

        u64 const block_address_range = address_range / c_max_num_blocks;
        void*     block_address       = m_address_base;
        for (u32 i = 0; i < c_max_num_blocks; i++)
        {
            m_blocks[i].initialize(heap, vmem, block_address, block_address_range, i, c_ablock_config[i].m_block_size, c_ablock_config[i].m_preallocate);
            block_address = toaddress(block_address, block_address_range);
        }

        for (u32 i = 0; i < c_max_num_sizes; i++)
            m_used_block_list_per_size[i].reset();
    }

    void superfsa_t::deinitialize(superheap_t& heap)
    {
        for (u32 i = 0; i < c_max_num_blocks; i++)
            m_blocks[i].deinitialize(heap);
        heap.deallocate(m_blocks);
    }

    u32 superfsa_t::sizeof_alloc(u32 alloc_size) const { return math::ceilpo2(alloc_size); }

    u32 superfsa_t::alloc(u32 alloc_size)
    {
        u32       superblock_index = llnode_t::NIL;
        u32 const allocsize_index  = alloc_size_to_index(alloc_size);
        ASSERT(allocsize_index >= 0 && allocsize_index < c_max_num_sizes);
        u32 const b = c_aalloc_config[allocsize_index].m_block_index;
        ASSERT(b >= 0 && b < c_max_num_blocks);
        if (m_used_block_list_per_size[allocsize_index].is_nil())
        {
            // Get a block and initialize that block for this size
            superblock_index = m_blocks[b].checkout_block(alloc_size);
            m_used_block_list_per_size[allocsize_index].insert(m_blocks[b].m_block_list_data, superblock_index);
        }
        else
        {
            superblock_index = m_used_block_list_per_size[allocsize_index].m_index;
        }

        if (superblock_index != llnode_t::NIL)
        {
            superblock_t* superblock = &m_blocks[b].m_block_array[superblock_index];
            void*         paddress   = m_blocks[b].address_of_block(superblock_index);
            u16 const     itemidx    = superblock->allocate_item(paddress);
            if (superblock->is_full())
            {
                m_used_block_list_per_size[allocsize_index].remove_item(m_blocks[b].m_block_list_data, superblock_index);
            }
            return (b << 28) | (superblock_index << 16) | itemidx;
        }
        else
        {
            return NIL;
        }
    }

    void superfsa_t::dealloc(u32 i)
    {
        u8 const            superblock_index = (i >> 28) & 0xF;
        u16 const           block_index      = (i >> 16) & 0x0FFF;
        u16 const           item_index       = i & 0xFFFF;
        superblock_t* const superblock       = &m_blocks[superblock_index].m_block_array[block_index];
        void* const         paddr            = m_blocks[superblock_index].address_of_block(block_index);
        superblock->deallocate(paddr, item_index);
        if (superblock->is_empty())
        {
            s32 const c = alloc_size_to_index(1 << superblock->m_item_size_shift);
            ASSERT(c >= 0 && c < c_max_num_sizes);
            m_used_block_list_per_size[c].remove_item(m_blocks[superblock_index].m_block_list_data, block_index);
            m_blocks[superblock_index].release_block(block_index);
        }
    }

    // Chunk Config
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

        struct chunk_t : public llnode_t
        {
            u16      m_segment_index;        // The index of the segment in the superspace
            u16      m_bin_index;            // The index of the bin that this chunk belongs to
            u32      m_segment_chunk_index;  // The index of the chunk in the segment
            u16      m_elem_used_count;      // The number of elements used in this chunk
            u16      m_elem_free_index;      // The index of the first free chunk (used to quickly take a free element)
            binmap_t m_elem_free_binmap;     // The binmap marking free elements for this chunk
            u32      m_physical_pages;       // The number of physical pages that this chunk has committed
            u32      m_alloc_tracking_iptr;  // The index to the tracking array which we use for set_tag/get_tag
        };

        superfsa_t*          m_fsa;
        vmem_t*              m_vmem;
        void*                m_address_base;
        u64                  m_address_range;
        u32                  m_block_size;
        u8                   m_page_shift;
        u64                  m_block_count;    // The number of pages that are currently committed
        u8                   m_segment_shift;  // 1 << m_segment_shift = segment size
        u32                  m_segment_count;  // Space Address Range / Segment Size = Number of segments
        llnode_t*            m_segment_list_nodes;
        segment_t*           m_segment_array;  // Array of segments
        superbin_t*          m_superbins;      // Array of superbins
        lldata_t             m_segment_list_data;
        llist_t              m_segment_list_free;
        llist_t              m_segment_list_full;
        llist_t*             m_segment_list_active;  // This needs to be per chunk-size
        u32                  m_num_chunk_configs;
        chunkconfig_t const* m_chunk_configs;  // Array of chunk configs  (c64KB, c128KB, c256KB, c512KB, c1MB ... c512MB)

        superspace_t()
            : m_fsa(nullptr)
            , m_vmem(nullptr)
            , m_address_base(nullptr)
            , m_address_range(0)
            , m_block_size(0)
            , m_page_shift(0)
            , m_block_count(0)
            , m_segment_shift(0)
            , m_segment_count(0)
            , m_segment_list_nodes(nullptr)
            , m_segment_array(nullptr)
            , m_superbins(nullptr)
            , m_segment_list_data()
            , m_segment_list_free()
            , m_segment_list_active()
            , m_num_chunk_configs(0)
            , m_chunk_configs(nullptr)
        {
        }

        void initialize(vmem_t* vmem, u64 address_range, u8 segment_shift, superheap_t* heap, superfsa_t* fsa, u32 num_chunk_configs, chunkconfig_t const* chunk_configs)
        {
            ASSERT(math::ispo2(address_range));

            m_fsa           = fsa;
            m_vmem          = vmem;
            m_address_range = address_range;
            u32 const attrs = 0;
            m_vmem->reserve(address_range, m_block_size, attrs, m_address_base);
            m_page_shift    = math::ilog2(m_block_size);
            m_block_count   = 0;
            m_segment_shift = segment_shift;

            m_segment_count                = (u32)(m_address_range >> segment_shift);
            m_segment_array                = (segment_t*)heap->allocate(m_segment_count * sizeof(segment_t));
            m_segment_list_nodes           = (llnode_t*)heap->allocate(m_segment_count * sizeof(llnode_t));
            m_segment_list_data.m_data     = m_segment_list_nodes;
            m_segment_list_data.m_itemsize = sizeof(llnode_t);
            m_segment_list_free.initialize(m_segment_list_data, 0, m_segment_count, m_segment_count);

            m_num_chunk_configs = num_chunk_configs;
            m_chunk_configs     = chunk_configs;

            m_segment_list_active = (llist_t*)heap->allocate(m_num_chunk_configs * sizeof(llist_t));
            for (u32 i = 0; i < m_num_chunk_configs; i++)
                m_segment_list_active[i].reset();
        }

        void deinitialize(superheap_t& heap)
        {
            if (m_vmem != nullptr)
            {
                m_vmem->release(m_address_base, m_address_range);
                m_vmem = nullptr;
            }
        }

        u32 checkout_segment(u8 chunk_config_index)
        {
            u32 const  chunk_size               = (1 << m_chunk_configs[chunk_config_index].m_chunk_size_shift);
            u32 const  segment_chunk_count      = (1 << m_segment_shift) / chunk_size;
            u32 const  segment_index            = m_segment_list_free.remove_headi(m_segment_list_data);
            segment_t* segment                  = &m_segment_array[segment_index];
            u32 const  chunks_index_array_iptr  = m_fsa->alloc(sizeof(u32) * segment_chunk_count);
            u32 const  chunks_llnode_array_iptr = m_fsa->alloc(sizeof(llnode_t) * segment_chunk_count);

            segment->m_chunks_array = (u32*)m_fsa->idx2ptr(chunks_index_array_iptr);

            // To avoid fully initializing the binmap's, we are using an index that marks the first free chunk.
            // We also use this index to lazily initialize each binmap. So each time we checkout a chunk
            // we progressively initialize both binmaps by looking at the lower 5 bits of the index, when 0 we
            // call lazy_init on each binmap.
            segment->m_chunks_free_index = 0;

            u32 l0len, l1len, l2len, l3len;
            binmap_t::compute_levels(segment_chunk_count, l0len, l1len, l2len, l3len);

            u32* l3 = (l3len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l3len) : nullptr;
            u32* l2 = (l2len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l2len) : nullptr;
            u32* l1 = (l1len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l1len) : nullptr;

            segment->m_chunks_cached_binmap.init_lazy_1(segment_chunk_count, l0len, l1, l1len, l2, l2len, l3, l3len);

            l3 = (l3len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l3len) : nullptr;
            l2 = (l2len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l2len) : nullptr;
            l1 = (l1len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l1len) : nullptr;

            segment->m_chunks_free_binmap.init_lazy_1(segment_chunk_count, l0len, l1, l1len, l2, l2len, l3, l3len);

            segment->m_count_chunks_cached = 0;
            segment->m_count_chunks_free   = segment_chunk_count;
            segment->m_count_chunks_used   = 0;
            segment->m_count_chunks_max    = segment_chunk_count;
            segment->m_chunk_config        = m_chunk_configs[chunk_config_index];

            return segment_index;
        }

        u32 chunk_physical_pages(superbin_t const& bin, u32 alloc_size) const
        {
            u64 size;
            if (bin.use_binmap())
                size = (bin.m_alloc_size * bin.m_max_alloc_count);
            else
                size = alloc_size;
            return (u32)((size + (((u64)1 << m_page_shift) - 1)) >> m_page_shift);
        }

        void initialize_chunk(superspace_t::chunk_t* chunk, u32 alloc_size, superbin_t const& bin)
        {
            // Allocate allocation tracking array
            chunk->m_alloc_tracking_iptr = superfsa_t::NIL;
            if (bin.m_max_alloc_count > 1)
            {
                chunk->m_alloc_tracking_iptr = m_fsa->alloc(sizeof(u32) * bin.m_max_alloc_count);
            }

            if (bin.use_binmap())
            {
                u32 l0len, l1len, l2len, l3len;
                binmap_t::compute_levels(bin.m_max_alloc_count, l0len, l1len, l2len, l3len);

                u32* l3 = (l3len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l3len) : nullptr;
                u32* l2 = (l2len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l2len) : nullptr;
                u32* l1 = (l1len > 0) ? (u32*)m_fsa->allocptr(sizeof(u32) * l1len) : nullptr;

                binmap_t* bm = (binmap_t*)&chunk->m_elem_free_binmap;
                bm->init_lazy_0(bin.m_max_alloc_count, l0len, l1, l1len, l2, l2len, l3, l3len);
            }
            else
            {
                chunk->m_physical_pages = 0;
            }

            chunk->m_bin_index       = bin.m_alloc_bin_index;
            chunk->m_elem_free_index = 0;
            chunk->m_elem_used_count = 0;
        }

        chunk_t* checkout_chunk(superbin_t const& bin)
        {
            // Get the chunk info index
            u32 const chunk_info_index = bin.m_chunk_config.m_chunk_info_index;

            u32 segment_index = 0xffffffff;
            if (m_segment_list_active[chunk_info_index].is_empty())
            {
                segment_index = checkout_segment(chunk_info_index);
                m_segment_list_active[chunk_info_index].insert(m_segment_list_data, segment_index);
            }
            else
            {
                segment_index = m_segment_list_active[chunk_info_index].m_head.m_index;
            }

            u32 const required_physical_pages = chunk_physical_pages(bin, bin.m_alloc_size);
            m_block_count += required_physical_pages;

            // Here we have a segment where we can get a chunk from.
            // Remember to also lazily initialize the free and cached chunk binmaps.
            segment_t* segment                 = &m_segment_array[segment_index];
            u32        segment_chunk_index     = 0xffffffff;
            u32        already_committed_pages = 0;
            u32        chunk_iptr              = 0xffffffff;
            chunk_t*   chunk                   = nullptr;

            if (segment->m_count_chunks_cached > 0)
            {
                segment->m_count_chunks_cached -= 1;
                segment_chunk_index     = segment->m_chunks_cached_binmap.findandset();
                chunk_iptr              = segment->m_chunks_array[segment_chunk_index];
                chunk                   = (chunk_t*)m_fsa->idx2ptr(chunk_iptr);
                already_committed_pages = chunk->m_physical_pages;
            }
            else if (segment->m_count_chunks_free > 0)
            {
                segment->m_count_chunks_free -= 1;
                segment_chunk_index = segment->m_chunks_free_binmap.findandset();
                chunk_iptr          = segment->m_chunks_array[segment_chunk_index];
                chunk               = (chunk_t*)m_fsa->idx2ptr(chunk_iptr);
            }
            else
            {
                // Use segment->m_chunks_free_index to take the next free chunk index.
                segment_chunk_index = segment->m_chunks_free_index;

                if ((segment->m_chunks_free_index & 0x1F) == 0)
                {  // If the lower 5 bits are 0, we need to clear a branch in the free and cached binmaps.
                    segment->m_chunks_cached_binmap.lazy_init_1(segment->m_chunks_free_index);
                    segment->m_chunks_free_binmap.lazy_init_1(segment->m_chunks_free_index);
                }
                segment->m_chunks_free_index += 1;
            }

            initialize_chunk(chunk, bin.m_alloc_size, bin);
            segment->m_chunks_array[segment_chunk_index] = chunk_iptr;

            // Commit the virtual pages for this chunk
            if (required_physical_pages < already_committed_pages)
            {
                // TODO Overcommitted, uncommit pages ?
            }
            else if (required_physical_pages > already_committed_pages)
            {
                // TODO Undercommitted, commit necessary pages
            }

            segment->m_count_chunks_used += 1;

            // Check if segment is full, if so we need to remove it from the list of active segments
            if (segment->m_count_chunks_used == segment->m_count_chunks_max)
            {
                m_segment_list_active[chunk_info_index].remove_item(m_segment_list_data, segment_index);
            }

            return chunk;
        }

        void deinitialize_chunk(superfsa_t& fsa, superspace_t::chunk_t* chunk, superbin_t const& bin)
        {
            if (chunk->m_alloc_tracking_iptr != superfsa_t::NIL)
            {
                fsa.dealloc(chunk->m_alloc_tracking_iptr);
                chunk->m_alloc_tracking_iptr = superfsa_t::NIL;
            }

            if (bin.use_binmap())
            {
                binmap_t* bm = (binmap_t*)&chunk->m_elem_free_binmap;
                for (u32 i = 0; i < bm->num_levels(); i++)
                {
                    m_fsa->deallocptr(bm->m_l[i]);
                    bm->m_l[i] = nullptr;
                }
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
                m_segment_list_active[chunk_info_index].insert(m_segment_list_data, chunk->m_segment_index);
            }

            m_block_count -= chunk->m_physical_pages;

            // TODO: We need to limit the number of cached chunks
            segment->m_count_chunks_cached += 1;

            // Release the tracking array that was allocated for this chunk
            deinitialize_chunk(*m_fsa, chunk, m_superbins[chunk->m_bin_index]);

            // See if this segment is now empty, if so we need to release it
            segment->m_count_chunks_used -= 1;
            if (segment->m_count_chunks_used == 0)
            {
                m_segment_list_active[segment->m_chunk_config.m_chunk_info_index].remove_item(m_segment_list_data, chunk->m_segment_index);

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

                m_segment_list_free.insert(m_segment_list_data, chunk->m_segment_index);
            }

            // Release the chunk structure back to the fsa
            m_fsa->deallocptr(chunk);
            segment->m_chunks_array[chunk->m_segment_chunk_index] = llnode_t::NIL;
        }

        void set_assoc(void* ptr, u32 assoc, chunk_t* chunk, superbin_t const& bin)
        {
            segment_t* segment              = &m_segment_array[chunk->m_segment_index];
            u32*       chunk_tracking_array = (u32*)m_fsa->idx2ptr(chunk->m_alloc_tracking_iptr);

            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            if (bin.use_binmap())
            {
                void* const chunk_address              = chunk_index_to_address(segment, chunk->m_segment_chunk_index);
                u32 const   chunk_item_index           = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
                chunk_tracking_array[chunk_item_index] = assoc;
            }
            else
            {
                chunk_tracking_array[0] = assoc;
            }
        }

        u32 get_assoc(void* ptr, chunk_t const* chunk, superbin_t const& bin) const
        {
            segment_t* segment              = &m_segment_array[chunk->m_segment_index];
            u32* const chunk_tracking_array = (u32*)m_fsa->idx2ptr(chunk->m_alloc_tracking_iptr);

            ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);
            if (bin.use_binmap())
            {
                void* const chunk_address       = chunk_index_to_address(segment, chunk->m_segment_chunk_index);
                u32 const   chunk_element_index = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
                return chunk_tracking_array[chunk_element_index];
            }
            return chunk_tracking_array[0];
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

        inline void* chunk_index_to_address(segment_t* segment, u32 segment_chunk_index) const
        {
            u64 const segment_offset = ((u64)(segment - m_segment_array) << m_segment_shift);
            return toaddress(m_address_base, segment_offset + ((u64)segment_chunk_index << segment->m_chunk_config.m_chunk_size_shift));
        }
    };

    struct superalloc_t
    {
        superspace_t* m_superspace;
        lldata_t      m_chunk_list_data;
        llhead_t*     m_active_chunk_list_per_alloc_size;

        superalloc_t()
            : m_superspace(nullptr)
            , m_chunk_list_data()
            , m_active_chunk_list_per_alloc_size(nullptr)
        {
        }

        void  initialize(superheap_t& heap, superfsa_t& fsa, superspace_t* superspace);
        void* allocate(superfsa_t* fsa, u32 size, superbin_t const& bin);
        u32   deallocate(superfsa_t* fsa, void* ptr, superspace_t::chunk_t* chunk, superbin_t const& bin);

        void set_assoc(void* ptr, u32 assoc, superspace_t::chunk_t* chunk, superbin_t const& bin);
        u32  get_assoc(void* ptr, superspace_t::chunk_t const* chunk, superbin_t const& bin) const;
    };

    void superalloc_t::initialize(superheap_t& heap, superfsa_t& fsa, superspace_t* superspace)
    {
        m_superspace = superspace;

        m_chunk_list_data.m_data     = fsa.baseptr(sizeof(superspace_t::chunk_t));
        m_chunk_list_data.m_itemsize = fsa.sizeof_alloc(sizeof(superspace_t::chunk_t));
        m_chunk_list_data.m_pagesize = fsa.blocksize(sizeof(superspace_t::chunk_t));
    }

    void* superalloc_t::allocate(superfsa_t* fsa, u32 alloc_size, superbin_t const& bin)
    {
        u32 const              c          = bin.m_chunk_config.m_chunk_info_index;
        llhead_t               chunk_list = m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index];
        superspace_t::chunk_t* chunk      = nullptr;
        if (chunk_list.is_nil())
        {
            chunk                                                     = m_superspace->checkout_chunk(bin);
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index] = fsa->ptr2idx(chunk);
        }
        else
        {
            chunk = (superspace_t::chunk_t*)fsa->idx2ptr(chunk_list.m_index);
        }

        ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);

        superspace_t::segment_t* segment = &m_superspace->m_segment_array[chunk->m_segment_index];
        void*                    ptr     = m_superspace->chunk_index_to_address(segment, chunk->m_segment_chunk_index);
        if (bin.use_binmap())
        {
            // If we have elements in the binmap, we can use it to get a free element.
            // If not, we need to use free_index as the free element.
            u32 elem_index;
            if (chunk->m_elem_used_count == chunk->m_elem_free_index)
            {  // seems all elements are used, lazy initialize binmap
                if ((chunk->m_elem_free_index & 0x1F) == 0)
                {
                    chunk->m_elem_free_binmap.lazy_init_0(chunk->m_elem_free_index);
                }
                elem_index = chunk->m_elem_free_index++;
            }
            else
            {
                elem_index = chunk->m_elem_free_binmap.findandset();
            }
            ASSERT(elem_index < bin.m_max_alloc_count);
            ptr = toaddress(ptr, (u64)elem_index * bin.m_alloc_size);

            // Initialize the assoc value for this element
            u32* chunk_tracking_array        = (u32*)fsa->idx2ptr(chunk->m_alloc_tracking_iptr);
            chunk_tracking_array[elem_index] = 0;
        }
        else
        {
            chunk->m_physical_pages = (alloc_size + (m_superspace->m_block_size - 1)) >> m_superspace->m_page_shift;
        }

        chunk->m_elem_used_count += 1;
        if (bin.m_max_alloc_count == chunk->m_elem_used_count)
        {
            // Chunk is full, so remove it from the list of active chunks
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].remove_head(m_chunk_list_data);
        }
        return ptr;
    }

    u32 superalloc_t::deallocate(superfsa_t* fsa, void* ptr, superspace_t::chunk_t* chunk, superbin_t const& bin)
    {
        ASSERT(chunk->m_bin_index == bin.m_alloc_bin_index);

        u32 alloc_size;
        if (bin.use_binmap())
        {
            superspace_t::segment_t* segment       = &m_superspace->m_segment_array[chunk->m_segment_index];
            void* const              chunk_address = m_superspace->chunk_index_to_address(segment, chunk->m_segment_chunk_index);
            u32 const                i             = (u32)(todistance(chunk_address, ptr) / bin.m_alloc_size);
            ASSERT(i < bin.m_max_alloc_count);
            chunk->m_elem_free_binmap.clr(i);
            u32* chunk_tracking_array = (u32*)fsa->idx2ptr(chunk->m_alloc_tracking_iptr);
            ASSERT(chunk_tracking_array[i] != 0xFEFEFEFE);  // Double freeing this element ?
            chunk_tracking_array[i] = 0xFEFEFEFE;           // Clear the assoc value for this element (mark it as freed)
            alloc_size              = bin.m_alloc_size;
        }
        else
        {
            alloc_size = chunk->m_physical_pages * m_superspace->m_block_size;
        }

        const bool chunk_was_full = (bin.m_max_alloc_count == chunk->m_elem_used_count);

        chunk->m_elem_used_count -= 1;
        if (0 == chunk->m_elem_used_count)
        {
            if (!chunk_was_full)
            {
                m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].remove_item(m_chunk_list_data, fsa->ptr2idx(chunk));
            }
            m_superspace->release_chunk(chunk);
        }
        else if (chunk_was_full)
        {
            // Ok, this chunk can be used to allocate, so add it to the list of active chunks
            m_active_chunk_list_per_alloc_size[bin.m_alloc_bin_index].insert(m_chunk_list_data, fsa->ptr2idx(chunk));
        }
        return alloc_size;
    }

    void superalloc_t::set_assoc(void* ptr, u32 assoc, superspace_t::chunk_t* chunk, superbin_t const& bin) { m_superspace->set_assoc(ptr, assoc, chunk, bin); }
    u32  superalloc_t::get_assoc(void* ptr, superspace_t::chunk_t const* chunk, superbin_t const& bin) const { return m_superspace->get_assoc(ptr, chunk, bin); }

    /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    /// The following is a strict data-drive initialization of the bins and allocators, please know what you are doing when modifying any of this.

    struct superallocator_config_t
    {
        superallocator_config_t()
            : m_total_address_size(0)
            , m_segment_address_size(0)
            , m_segment_address_range_shift(0)
            , m_num_superbins(0)
            , m_asuperbins(nullptr)
            , m_internal_heap_address_range(0)
            , m_internal_heap_pre_size(0)
            , m_internal_fsa_address_range(0)
            , m_internal_fsa_pre_size(0)
        {
        }

        superallocator_config_t(const superallocator_config_t& other)
            : m_total_address_size(other.m_total_address_size)
            , m_segment_address_size(other.m_segment_address_size)
            , m_num_superbins(other.m_num_superbins)
            , m_asuperbins(other.m_asuperbins)
            , m_internal_heap_address_range(other.m_internal_heap_address_range)
            , m_internal_heap_pre_size(other.m_internal_heap_pre_size)
            , m_internal_fsa_address_range(other.m_internal_fsa_address_range)
            , m_internal_fsa_pre_size(other.m_internal_fsa_pre_size)
        {
        }

        superallocator_config_t(u64 space_address_range, u64 segment_address_range, s32 const num_superbins, superbin_t const* asuperbins, u32 const internal_heap_address_range, u32 const internal_heap_pre_size, u32 const internal_fsa_address_range,
                                u32 const internal_fsa_pre_size)
            : m_total_address_size(space_address_range)
            , m_segment_address_size(segment_address_range)
            , m_num_superbins(num_superbins)
            , m_asuperbins(asuperbins)
            , m_internal_heap_address_range(internal_heap_address_range)
            , m_internal_heap_pre_size(internal_heap_pre_size)
            , m_internal_fsa_address_range(internal_fsa_address_range)
            , m_internal_fsa_pre_size(internal_fsa_pre_size)
        {
            m_segment_address_range_shift = math::ilog2(segment_address_range);
        }

        u64               m_total_address_size;
        u64               m_segment_address_size;
        s8                m_segment_address_range_shift;
        u32               m_num_superbins;
        superbin_t const* m_asuperbins;
        u32               m_internal_heap_address_range;
        u32               m_internal_heap_pre_size;
        u32               m_internal_fsa_address_range;
        u32               m_internal_fsa_pre_size;
    };

    // 25% allocation waste (based on empirical data)
    namespace superallocator_config_windows_desktop_app_25p_t
    {
        // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly

        // clang-format off
        // superbin_t(alloc-size, bin redir index, chunk-config)
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
            const u32 c_internal_fsa_pre_size       = 16 * cMB;
            return superallocator_config_t(c_total_address_space, c_segment_address_range, c_num_bins, c_asbins, c_internal_heap_address_range, c_internal_heap_pre_size, c_internal_fsa_address_range, c_internal_fsa_pre_size);
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
        // superbin_t(alloc-size, bin redir index, chunk-config)
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
            const u32 c_internal_fsa_pre_size       = 16 * cMB;
            return superallocator_config_t(c_total_address_space, c_segment_address_range, c_num_bins, c_asbins, c_internal_heap_address_range, c_internal_heap_pre_size, c_internal_fsa_address_range, c_internal_fsa_pre_size);
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
        superspace_t*           m_superspace;
        superalloc_t*           m_allocator;
        void*                   m_vmem_membase;
        vmem_t*                 m_vmem;
        superheap_t             m_internal_heap;
        superfsa_t              m_internal_fsa;
        superallocator_config_t m_config;

    public:
        superallocator_t()
            : m_vmem_membase(0)
            , m_superspace(nullptr)
            , m_allocator(nullptr)
            , m_vmem(nullptr)
            , m_internal_heap()
            , m_internal_fsa()
            , m_config()
        {
        }

        void initialize(vmem_t* vmem, superallocator_config_t const& config);
        void deinitialize();

        virtual void* v_allocate(u32 size, u32 align);
        virtual u32   v_deallocate(void* p);
        virtual void  v_release() { deinitialize(); }

        virtual u32  v_get_size(void* ptr) const;
        virtual void v_set_tag(void* ptr, u32 assoc);
        virtual u32  v_get_tag(void* ptr) const;
    };

    void superallocator_t::initialize(vmem_t* vmem, superallocator_config_t const& config)
    {
        m_config = config;
        m_vmem   = vmem;

        m_internal_heap.initialize(m_vmem, m_config.m_internal_heap_address_range, m_config.m_internal_heap_pre_size);
        m_internal_fsa.initialize(m_internal_heap, m_vmem, m_config.m_internal_fsa_address_range);

        m_superspace = (superspace_t*)m_internal_heap.allocate(sizeof(superspace_t));
        m_superspace->initialize(vmem, config.m_total_address_size, config.m_segment_address_range_shift, &m_internal_heap, &m_internal_fsa, cNumChunkConfigs, cChunkInfoArray);
        m_allocator = (superalloc_t*)m_internal_heap.allocate(sizeof(superalloc_t));
        m_allocator->initialize(m_internal_heap, m_internal_fsa, m_superspace);

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
        m_internal_heap.deinitialize();
        m_superspace = nullptr;
        m_allocator  = nullptr;
        m_vmem       = nullptr;
    }

    void* superallocator_t::v_allocate(u32 size, u32 alignment)
    {
        size                = math::alignUp(size, alignment);
        u32 const sbinindex = m_config.m_asuperbins[superallocator_config::size2bin(size)].m_alloc_bin_index;
        ASSERT(size <= m_config.m_asuperbins[sbinindex].m_alloc_size);
        ASSERT(m_config.m_asuperbins[sbinindex].m_alloc_bin_index == sbinindex);
        void* ptr = m_allocator->allocate(&m_internal_fsa, size, m_config.m_asuperbins[sbinindex]);
        ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
        return ptr;
    }

    u32 superallocator_t::v_deallocate(void* ptr)
    {
        if (ptr == nullptr)
            return 0;
        ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
        superspace_t::chunk_t* pchunk    = m_superspace->address_to_chunk(ptr);
        u32 const              sbinindex = pchunk->m_bin_index;
        u32 const              size      = m_allocator->deallocate(&m_internal_fsa, ptr, pchunk, m_config.m_asuperbins[sbinindex]);
        ASSERT(size <= m_config.m_asuperbins[sbinindex].m_alloc_size);
        return size;
    }

    u32 superallocator_t::v_get_size(void* ptr) const
    {
        if (ptr == nullptr)
            return 0;
        ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
        superspace_t::chunk_t* pchunk    = m_superspace->address_to_chunk(ptr);
        u32 const              sbinindex = pchunk->m_bin_index;
        if (m_config.m_asuperbins[sbinindex].use_binmap())
        {
            return m_config.m_asuperbins[sbinindex].m_alloc_size;
        }
        else
        {
            return pchunk->m_physical_pages * m_superspace->m_block_size;
        }
    }

    void superallocator_t::v_set_tag(void* ptr, u32 assoc)
    {
        if (ptr != nullptr)
        {
            ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
            superspace_t::chunk_t* pchunk    = m_superspace->address_to_chunk(ptr);
            u32 const              sbinindex = pchunk->m_bin_index;
            m_allocator->set_assoc(ptr, assoc, pchunk, m_config.m_asuperbins[sbinindex]);
        }
    }

    u32 superallocator_t::v_get_tag(void* ptr) const
    {
        if (ptr == nullptr)
            return 0xffffffff;
        ASSERT(ptr >= m_superspace->m_address_base && ptr < ((u8*)m_superspace->m_address_base + m_superspace->m_address_range));
        superspace_t::chunk_t* pchunk    = m_superspace->address_to_chunk(ptr);
        u32 const              sbinindex = pchunk->m_bin_index;
        return m_allocator->get_assoc(ptr, pchunk, m_config.m_asuperbins[sbinindex]);
    }

    valloc_t* gCreateVmAllocator(alloc_t* main_heap, vmem_t* vmem)
    {
        superallocator_t* superalloc = (superallocator_t*)main_heap->allocate(sizeof(superallocator_t));
        superalloc->initialize(vmem, superallocator_config::get_config());
        return superalloc;
    }

}  // namespace ncore
