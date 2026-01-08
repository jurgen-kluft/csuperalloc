#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_arena.h"
#include "ccore/c_binmap1.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"
#include "ccore/c_limits.h"

#include "csuperalloc/c_fsa.h"

namespace ncore
{
    struct fsa_t
    {
        u32 m_base_offset;   // offset to the base address
        u16 m_header_pages;  // number of pages used by fsa header
        u16 m_sections_free_index;
        u16 m_sections_free_list;
        u16 m_sections_max_index;
        u8  m_section_block_array_size_in_pages;
        u8  m_section_size_shift;
        u8  m_page_size_shift;
        u8  m_padding0;
    };

    namespace nfsa
    {

// Constraints
#define D_MAX_SECTIONS           256   // maximum number of sections is 256 sections
#define D_MAX_BLOCKS_PER_SECTION 1024  // maximum number of blocks in a section is 256 blocks
#define D_MAX_ITEMS_PER_BLOCK    8192  // maximum number of items in a block is 8192 items

        // Uncomment to enable debug fills
        // #define FSA_DEBUG

        // One large address space, the first page(s) contain the fsa_t struct
        // as well as the array of active section lists, active block lists per section
        // and the array of sections.
        //
        // Since a page is 4KiB, 16KiB or 64KiB depending on platform, we have
        // the array of sections right after the fsa_t struct.
        //           N sections * 16 bytes
        // We will also place the array of block list heads:
        //           N sections * (32 * sizeof(u16) = 64 bytes)
        // TODO this can be reduced to (16 * sizeof(u16) = 32 bytes) if we limit
        // the bin configurations to maximum 16 different block sizes.
        //
        // It is then page aligned and followed by N * 16KiB, each 16KiB containing the
        // the array of blocks (block_t[]) for that section.
        //
        // That's it, during runtime, no more allocations, only page commits, which
        // also means we can free an empty block and section back to the fsa allocator.
        //

        static inline void* toaddress(void* base, u64 offset) { return (void*)((ptr_t)base + offset); }
        static inline ptr_t todistance(void const* base, void const* ptr) { return (ptr_t)((ptr_t)ptr - (ptr_t)base); }

        struct bin_config_t
        {
            s8 m_alloc_sizeshift;
            s8 m_block_sizeshift;
        };

        // block size shift
        //static const u8 c16KB  = 14;
        //static const u8 c32KB  = 15;
        static const u8 c64KB  = 16;
        //static const u8 c128KB = 17;
        static const u8 c256KB = 18;
        //static const u8 c512KB = 19;
        //static const u8 c1MB   = 20;
        //static const u8 c2MB   = 21;
        static const u8 c4MB   = 22;
        //static const u8 c8MB   = 23;

        static const bin_config_t c_bin_configs[] = {
          // alloc-size { allocation sizeshift, block sizeshift}
          {3, c64KB},    //        1
          {3, c64KB},    //        2
          {3, c64KB},    //        4
          {3, c64KB},    //        8
          {4, c64KB},    //       16
          {5, c64KB},    //       32
          {6, c64KB},    //       64
          {7, c64KB},    //      128
          {8, c256KB},   //      256
          {9, c256KB},   //      512
          {10, c256KB},  //     1024
          {11, c256KB},  //     2048
          {12, c256KB},  //     4096
          {13, c256KB},  //      8192
          {14, c256KB},  //     16384
          {15, c256KB},  //     32768
          {16, c4MB},    //  64 * cKB
          {17, c4MB},    // 128 * cKB
          {18, c4MB},    // 256 * cKB
          {19, c4MB},    // 512 * cKB
          {20, c4MB},    //   1 * cMB
          {21, c4MB},    //   2 * cMB
        };

        // Maximum number of items in a block is 32768 items
        // This struct is 16 bytes on a 64-bit system
        struct block_t
        {
            u16 m_next;            //
            u16 m_prev;            //
            u16 m_block_index;     // index of the block in the section
            u16 m_item_freeindex;  // index of the next free item if freelist is empty
            u16 m_item_count;      // current number of allocated items
            u16 m_item_count_max;  // maximum number of items in this block
            u16 m_item_freelist;   // index of the first free item in the freelist, D_NILL_U16 if none
            u8  m_active;          // is block active
            u8  m_padding;         // padding
        };

        namespace nblock
        {
            inline bool is_full(block_t* self) { return self->m_item_count == self->m_item_count_max; }
            inline bool is_empty(block_t* self) { return self->m_item_count == 0; }

            inline u32   ptr2idx(bin_config_t const& bincfg, const byte* block_address, const byte* elem) { return (u32)(((u64)elem - (u64)block_address) >> bincfg.m_alloc_sizeshift); }
            inline void* idx2ptr(bin_config_t const& bincfg, byte* block_address, u32 index) { return toaddress(block_address, ((u64)index << bincfg.m_alloc_sizeshift)); }

            void* allocate_item(bin_config_t const& bincfg, block_t* self, byte* block_address)
            {
                u16* item = nullptr;
                if (self->m_item_freelist != D_NILL_U16)
                {
                    const u32 item_index  = self->m_item_freelist;
                    item                  = (u16*)idx2ptr(bincfg, block_address, item_index);
                    self->m_item_freelist = item[0];
                }
                else if (self->m_item_freeindex < self->m_item_count_max)
                {
                    const u32 item_index = self->m_item_freeindex++;
                    item                 = (u16*)idx2ptr(bincfg, block_address, item_index);
                }
                else
                {
                    ASSERT(false);  // panic
                    return item;
                }

                self->m_item_count++;
#ifdef FSA_DEBUG
                nmem::memset(item, 0xCDCDCDCD, ((u64)1 << bincfg.m_sizeshift));
#endif
                return item;
            }

            void deallocate_item(bin_config_t const& bincfg, block_t* self, byte* block_address, u16 item_index)
            {
                ASSERT(self->m_item_count > 0);
                ASSERT(item_index < self->m_item_freeindex);
                u16* const item = (u16*)idx2ptr(bincfg, block_address, item_index);
#ifdef FSA_DEBUG
                nmem::memset(item, 0xFEFEFEFE, ((u64)1 << bincfg.m_sizeshift));
#endif
                item[0]               = self->m_item_freelist;
                self->m_item_freelist = item_index;
                self->m_item_count--;
            }

            void activate(fsa_t* fsa, block_t* self, bin_config_t const& bincfg)
            {
                ASSERT((1 << (bincfg.m_block_sizeshift - bincfg.m_alloc_sizeshift)) <= D_MAX_ITEMS_PER_BLOCK);
                self->m_next           = D_NILL_U16;
                self->m_prev           = D_NILL_U16;
                self->m_item_freeindex = 0;
                self->m_item_count_max = (1 << (bincfg.m_block_sizeshift - bincfg.m_alloc_sizeshift));
                self->m_item_freelist  = D_NILL_U16;
                self->m_active         = 1;
            }

            void deactivate(fsa_t* fsa, block_t* block)
            {
                // TODO uncommit the pages used by this block
            }
        }  // namespace nblock

        static inline byte* base_address(fsa_t* fsa) { return (byte*)fsa + fsa->m_base_offset; }
        static inline bool  ptr_is_inside_fsa(fsa_t* fsa, void const* ptr) { return ptr >= base_address(fsa) && ptr < toaddress(base_address(fsa), ((u64)fsa->m_sections_max_index << fsa->m_section_size_shift)); }
        static inline u16   ptr_to_section_index(fsa_t* fsa, void const* ptr) { return ((const byte*)ptr - base_address(fsa)) >> fsa->m_section_size_shift; }

        // A section contains multiple blocks of the same size defined by bin config
        // This struct is 16 bytes on a 64-bit system
        struct section_t
        {
            u16          m_section_index;
            bin_config_t m_bin_config;
            u16          m_block_count;
            u16          m_block_max_count;
            u16          m_block_free_index;
            u16          m_block_free_list;
            u16          m_next;
            u16          m_prev;
        };

        namespace nsection
        {
            static inline bool is_full(section_t* self) { return self->m_block_count == self->m_block_max_count; }
            static inline bool is_empty(section_t* self) { return self->m_block_count == 0; }

            static inline u32 get_section_index(fsa_t* fsa, void const* ptr)
            {
                const u32 section_index = (u32)(((byte const*)ptr - base_address(fsa)) >> fsa->m_section_size_shift);
                ASSERT(section_index < fsa->m_sections_max_index);
                return section_index;
            }

            static inline section_t* get_section_array(fsa_t* fsa)
            {
                byte* base = (byte*)fsa + sizeof(fsa_t) + (32 * sizeof(u16));
                return (section_t*)base;
            }

            static inline section_t* get_section_at_index(fsa_t* fsa, u32 index)
            {
                ASSERT(index < fsa->m_sections_max_index);
                section_t* sections = get_section_array(fsa);
                return &sections[index];
            }

            static inline byte* get_section_address(fsa_t* fsa, u32 section_index)
            {
                byte* section_address = base_address(fsa) + ((u64)section_index << fsa->m_section_size_shift);
                return section_address;
            }

            static inline u16*     get_blocklist_per_bincfg(section_t* self) { return (u16*)((byte*)self + sizeof(section_t)); }
            static inline block_t* get_block_array(fsa_t* fsa, u32 section_index)
            {
                int_t offset = (int_t)fsa->m_header_pages << fsa->m_page_size_shift;
                offset += ((int_t)fsa->m_section_block_array_size_in_pages << fsa->m_page_size_shift);
                return (block_t*)((byte*)fsa + offset);
            }

            static inline block_t* get_block_at_index(fsa_t* fsa, section_t* self, u32 index)
            {
                ASSERT(index < self->m_block_max_count);
                block_t* block_array = get_block_array(fsa, self->m_section_index);
                return &block_array[index];
            }

            static inline byte* get_block_address(fsa_t* fsa, section_t* section, u32 block_index) { return get_section_address(fsa, section->m_section_index) + ((u64)block_index << section->m_bin_config.m_block_sizeshift); }
            static inline u32   get_block_index(fsa_t* fsa, section_t* self, byte const* ptr)
            {
                const byte* section_address = get_section_address(fsa, self->m_section_index);
                return (u32)((ptr - section_address) >> self->m_bin_config.m_block_sizeshift);
            }

            void allocate_block(fsa_t* fsa, section_t* self, bin_config_t const& bincfg, block_t*& out_block)
            {
                block_t* block = nullptr;
                nblock::activate(fsa, block, bincfg);

#ifdef FSA_DEBUG
                nmem::memset(nsection::get_block_address(fsa, self, block_index), 0xCDCDCDCD, (int_t)1 << self->m_bin_config.m_sizeshift);
#endif
                self->m_block_count++;
                out_block = block;
            }

            void deallocate_block(fsa_t* fsa, section_t* self, block_t* block)
            {
#ifdef FSA_DEBUG
                nmem::memset(nsection::get_block_address(fsa, self, block->m_block_index), 0xFEFEFEFE, (int_t)1 << self->m_bin_config.m_sizeshift);
#endif
                // TODO uncommit the pages used by this block

                self->m_block_count--;
                // add to free list
            }

            void initialize(section_t* self)
            {
                self->m_bin_config       = {0, 0};
                self->m_block_free_index = 0;
                self->m_block_max_count  = 0;
            }

            void activate(fsa_t* fsa, section_t* self, u32 index, bin_config_t const& bincfg)
            {
                self->m_bin_config       = bincfg;
                self->m_block_free_index = 0;
                self->m_block_max_count  = 1 << (fsa->m_section_size_shift - self->m_bin_config.m_block_sizeshift);
                self->m_block_count      = 0;

                // TODO we could handle this more gracefully by committing pages as needed.
                // currently we just commit one page for the block array, but if the block array
                // grows beyond that, we should commit more pages.
                byte* block_array = (byte*)get_block_array(fsa, index);
                v_alloc_commit(block_array, (int_t)1 << fsa->m_page_size_shift);
            }

            void deactivate(fsa_t* fsa, section_t* self)
            {
                // TODO, for every block, decommit the memory used by the block array
            }
        }  // namespace nsection

        // ------------------------------------------------------------------------------
        // ------------------------------------------------------------------------------
        // fsa functions

        static inline bin_config_t alloc_size_to_alloc_config(u32 alloc_size)
        {
            alloc_size = math::ceilpo2(alloc_size);
            s8 const c = math::countTrailingZeros(alloc_size);
            ASSERT(c < (s8)DARRAYSIZE(c_bin_configs));
            return c_bin_configs[c];
        }

        // These 2 arrays follow immediately after this struct in memory
        // u16*  m_active_section_list_per_bincfg;  // sections that still have available free blocks
        u16* active_section_list_per_bincfg(fsa_t* fsa)
        {
            byte* base = (byte*)fsa + sizeof(fsa_t);
            return (u16*)base;
        }

        section_t* active_section_for_bincfg(fsa_t* fsa, bin_config_t const& bincfg)
        {
            u16* sections_list_heads = active_section_list_per_bincfg(fsa);
            u16  list_head           = sections_list_heads[bincfg.m_block_sizeshift];
            if (list_head == D_NILL_U16)
                return nullptr;
            return nsection::get_section_at_index(fsa, list_head);
        }

        // blocks that still have available free items, a list per bin config
        // u16*  m_active_block_list_per_bincfg;    // blocks that still have available free items, a list per bin config
        u16* active_block_list_per_bincfg(fsa_t* fsa, u32 section_index)
        {
            byte* base = (byte*)fsa + sizeof(fsa_t);
            base += 32 * sizeof(u16);                                 // skip active section list
            base += (sizeof(section_t) * fsa->m_sections_max_index);  // skip section array
            base += ((u32)section_index * 32 * sizeof(u16));          // skip to block list for this section
            return (u16*)base;
        }

        bool get_active_block_for_bincfg(fsa_t* fsa, bin_config_t const& bincfg, section_t*& out_section, block_t*& out_block)
        {
            section_t* section = active_section_for_bincfg(fsa, bincfg);
            if (section == nullptr)
            {
                // get a new section

                return false;
            }

            u16* blocks_list_heads = active_block_list_per_bincfg(fsa, section->m_section_index);
            u16  block_list_head   = blocks_list_heads[bincfg.m_alloc_sizeshift];
            if (block_list_head == D_NILL_U16)
            {
                // get a new block from this section and make it the active one for this bincfg
            }

            const u32 block_index = block_list_head;

            out_section = section;
            out_block   = nsection::get_block_at_index(fsa, section, block_index);
            return true;
        }

        void add_active_block_for_bincfg(fsa_t* fsa, bin_config_t const& bincfg, section_t* section, block_t* block)
        {
            u16* blocks_list_heads = active_block_list_per_bincfg(fsa, section->m_section_index);
            u16& head              = blocks_list_heads[bincfg.m_alloc_sizeshift];

            const u16 block_index = block->m_block_index;
            if (head == D_NILL_U16)
            {
                head          = block_index;
                block->m_next = D_NILL_U16;
                block->m_prev = D_NILL_U16;
            }
            else
            {
                block_t* head_block = nsection::get_block_at_index(fsa, section, head);
                head_block->m_prev  = block_index;
                block->m_next       = head;
                block->m_prev       = D_NILL_U16;
                head                = block_index;
            }
        }

        void rem_active_block_for_bincfg(fsa_t* fsa, bin_config_t const& bincfg, section_t* section, block_t* block)
        {
            u16* blocks_list_heads = active_block_list_per_bincfg(fsa, section->m_section_index);
            u16& head              = blocks_list_heads[bincfg.m_alloc_sizeshift];

            const u16 block_index = block->m_block_index;
            if (block_index == head)
            {
                head = block->m_next;
                if (head != D_NILL_U16)
                {
                    block_t* new_head_block = nsection::get_block_at_index(fsa, section, head);
                    new_head_block->m_prev  = D_NILL_U16;
                }
            }
            else
            {
                if (block->m_prev != D_NILL_U16)
                {
                    block_t* prev_block = nsection::get_block_at_index(fsa, section, block->m_prev);
                    prev_block->m_next  = block->m_next;
                }
                if (block->m_next != D_NILL_U16)
                {
                    block_t* next_block = nsection::get_block_at_index(fsa, section, block->m_next);
                    next_block->m_prev  = block->m_prev;
                }
            }
        }

        static inline u32 sizeof_alloc(u32 alloc_size) { return math::ceilpo2((alloc_size + (8 - 1)) & ~(8 - 1)); }

        bool allocate_block(fsa_t* fsa, bin_config_t const& bincfg, section_t*& out_section, block_t*& out_block)
        {
            section_t* section = active_section_for_bincfg(fsa, bincfg);
            if (section == nullptr)
            {
                if (!is_nil(fsa->m_sections_free_list))
                {
                    const u16 section_index   = fsa->m_sections_free_list;
                    section                   = nsection::get_section_at_index(fsa, section_index);
                    fsa->m_sections_free_list = section->m_next;
                    nsection::activate(fsa, section, section_index, bincfg);
                }
                else if (fsa->m_sections_free_index < fsa->m_sections_max_index)
                {
                    const u16 section_index = fsa->m_sections_free_index++;
                    section                 = nsection::get_section_at_index(fsa, section_index);
                    nsection::activate(fsa, section, section_index, bincfg);
                }
                else
                {
                    // panic, no more sections available
                    return false;
                }
            }

            block_t* block;
            nsection::allocate_block(fsa, section, bincfg, block);
            if (nsection::is_full(section))
            {
                // TODO this section doesn't have any more free blocks, remove it from the active list
            }

            out_section = section;
            out_block   = block;
            return true;
        }

        fsa_t* new_fsa(u64 address_range)
        {
            // example from superalloc config
            // const u32 internal_fsa_address_range  =  1 * cGB;
            // const u32 internal_fsa_section_size   = 16 * cMB;
            // const u32 internal_fsa_pre_size       = 16 * cMB;
            const u32 page_size          = v_alloc_get_page_size();
            const u8  page_size_shift    = v_alloc_get_page_size_shift();
            const u8  section_size_shift = 24;  // 16 * cMB

            const u16 sections_max_index = (u16)(address_range >> section_size_shift);

            int_t fsa_size = sizeof(fsa_t);
            fsa_size += 32 * sizeof(u16);                                                                   // active section list
            fsa_size += sizeof(section_t) * sections_max_index;                                             // section_t[N]
            fsa_size += (32 * sizeof(u16)) * sections_max_index;                                            // active block list for each section
            const i32 fsa_size_in_pages         = (fsa_size + (page_size - 1)) >> page_size_shift;          // round up to pages
            int_t     block_array_size          = (D_MAX_BLOCKS_PER_SECTION * sizeof(block_t));             // block_t[N]
            const i32 block_array_size_in_pages = (block_array_size + (page_size - 1)) >> page_size_shift;  // round up to pages

            address_range = ((u64)sections_max_index << section_size_shift);
            address_range += (u64)(fsa_size_in_pages) << page_size_shift;
            address_range += (u64)(block_array_size_in_pages * sections_max_index) << page_size_shift;
            void* base_address = v_alloc_reserve(address_range);
            v_alloc_commit(base_address, (int_t)fsa_size_in_pages << page_size_shift);

            fsa_t* fsa                               = (fsa_t*)base_address;
            fsa->m_sections_free_index               = 0;
            fsa->m_sections_free_list                = D_NILL_U16;
            fsa->m_sections_max_index                = sections_max_index;
            fsa->m_section_size_shift                = section_size_shift;
            fsa->m_page_size_shift                   = page_size_shift;
            fsa->m_header_pages                      = (u16)(fsa_size_in_pages);
            fsa->m_base_offset                       = (u32)((fsa_size_in_pages + (block_array_size_in_pages * sections_max_index)) << page_size_shift);
            fsa->m_section_block_array_size_in_pages = (u8)(block_array_size_in_pages);

            section_t* section_array   = nsection::get_section_array(fsa);
            u16*       active_sections = active_section_list_per_bincfg(fsa);
            for (u32 i = 0; i < 32; ++i)
            {
                active_sections[i]       = D_NILL_U16;
                section_t* section       = &section_array[i];
                u16*       active_blocks = nsection::get_blocklist_per_bincfg(section);
                for (u32 j = 0; j < 32; ++j)
                    active_blocks[j] = D_NILL_U16;
            }

            return fsa;
        }

        void destroy(fsa_t* fsa)
        {
            // TODO, decommit all sections
        }

        void* allocate(fsa_t* fsa, u32 alloc_size)
        {
            const bin_config_t bincfg = alloc_size_to_alloc_config(alloc_size);
            ASSERT(alloc_size <= ((u32)1 << bincfg.m_alloc_sizeshift));

            section_t* section;
            block_t*   block;
            if (!get_active_block_for_bincfg(fsa, bincfg, section, block))
            {
                if (!allocate_block(fsa, bincfg, section, block))
                    return nullptr;  // panic
                add_active_block_for_bincfg(fsa, bincfg, section, block);
            }

            byte* block_address = nsection::get_block_address(fsa, section, block->m_block_index);
            void* item          = nblock::allocate_item(bincfg, block, block_address);
            if (nblock::is_full(block))
            {
                rem_active_block_for_bincfg(fsa, bincfg, section, block);
            }
            return item;
        }

        void deallocate(fsa_t* fsa, void* ptr)
        {
            if (ptr == nullptr)
                return;

            u32 const   section_index = nsection::get_section_index(fsa, ptr);
            section_t*  section       = nsection::get_section_at_index(fsa, section_index);
            u32 const   block_index   = nsection::get_block_index(fsa, section, (byte const*)ptr);
            block_t*    block         = nsection::get_block_at_index(fsa, section, block_index);
            byte const* block_address = nsection::get_block_address(fsa, section, block_index);
            u32 const   item_index    = nblock::ptr2idx(section->m_bin_config, block_address, (byte const*)ptr);

            const bool was_full = nblock::is_full(block);
            nblock::deallocate_item(section->m_bin_config, block, (byte*)block_address, (u16)item_index);
            if (nblock::is_empty(block))
            {
                nsection::deallocate_block(fsa, section, block);
                // TODO Is section now empty?, if so release section and add to free list
            }
            else if (was_full)
            {
                add_active_block_for_bincfg(fsa, section->m_bin_config, section, block);
            }
        }

        u32 get_size(fsa_t* fsa, void* ptr)
        {
            if (ptr == nullptr)
                return 0;
            u32 const  section_index = nsection::get_section_index(fsa, ptr);
            section_t* section       = nsection::get_section_at_index(fsa, section_index);
            return (u32)((u64)1 << section->m_bin_config.m_alloc_sizeshift);
        }

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
            const u64   dist = (u64)((byte const*)ptr - base);
            // The maximum address range of the fsa is 32 GB, and since the smallest
            // allocation size is 8 bytes, we can shift right 3 bits and fit in u32.
            return (u32)(dist >> 3);
        }
    }  // namespace nfsa
}  // namespace ncore
