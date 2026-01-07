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
    namespace nfsa
    {
        // One large address space, the first page contains the fsa_t struct
        // as well as the array of sections.
        // The first page of the section address range contains the array of
        // blocks.
        // That's it, during runtime, no more allocations, which also means
        // we can free an empty section back to the fsa allocator.
        //
        // Furthermore, we have been able to reduce the size of the structs,
        // fsa_t, block_t and section_t significantly.
        // Both section_t and block_t are now 16 bytes, and fsa_t is now just
        // 8 bytes.

        static inline void* toaddress(void* base, u64 offset) { return (void*)((ptr_t)base + offset); }
        static inline ptr_t todistance(void const* base, void const* ptr) { return (ptr_t)((ptr_t)ptr - (ptr_t)base); }

        const u32 MAX_SECTIONS           = 1024 - 1;
        const u32 MAX_BLOCKS_PER_SECTION = 1024 - 1;
        const u32 MAX_ITEMS_PER_BLOCK    = 4096;

        struct bin_config_t
        {
            s8 m_alloc_sizeshift;
            s8 m_block_sizeshift;
        };

        // block size shift
        static const u8 c4KB   = 12;
        static const u8 c8KB   = 13;
        static const u8 c16KB  = 14;
        static const u8 c32KB  = 15;
        static const u8 c64KB  = 16;
        static const u8 c128KB = 17;
        static const u8 c256KB = 18;
        static const u8 c512KB = 19;
        static const u8 c1MB   = 20;
        static const u8 c2MB   = 21;
        static const u8 c4MB   = 22;
        static const u8 c8MB   = 23;

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
          {8, c64KB},    //      256
          {9, c64KB},    //      512
          {10, c64KB},   //     1024
          {11, c64KB},   //     2048
          {12, c64KB},   //     4096
          {13, c64KB},   //      8192
          {14, c64KB},   //     16384
          {15, c256KB},  //     32768
          {16, c256KB},  //  64 * cKB
          {17, c1MB},    // 128 * cKB
          {18, c1MB},    // 256 * cKB
          {19, c4MB},    // 512 * cKB
          {20, c4MB},    //   1 * cMB
          {21, c4MB},    //   2 * cMB
        };

        // Maximum number of items in a block is 32768 items
        // This struct is 16 bytes on a 64-bit system
        struct block_t
        {
            u32 m_next;            // high 16 bits = section index, low 16 bits = block index
            u32 m_prev;            // high 16 bits = section index, low 16 bits = block index
            u16 m_item_freeindex;  // index of the next free item if freelist is empty
            u16 m_item_count;      // current number of allocated items
            u16 m_item_count_max;  // maximum number of items in this block
            u16 m_item_freelist;   // index of the first free item in the freelist, D_NILL_U16 if none
        };

        namespace nblock
        {
            void initialize(block_t* self, bin_config_t const& bincfg)
            {
                ASSERT((1 << (bincfg.m_block_sizeshift - bincfg.m_alloc_sizeshift)) <= 32768);
                self->m_next           = D_NILL_U32;
                self->m_prev           = D_NILL_U32;
                self->m_item_freeindex = 0;
                self->m_item_count_max = (1 << (bincfg.m_block_sizeshift - bincfg.m_alloc_sizeshift));
                self->m_item_freelist  = D_NILL_U16;
            }

            inline bool  is_full(block_t* self) { return self->m_item_count == self->m_item_count_max; }
            inline bool  is_empty(block_t* self) { return self->m_item_count == 0; }
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
#ifdef SUPERALLOC_DEBUG
                nmem::memset(item, 0xCDCDCDCD, ((u64)1 << bincfg.m_sizeshift));
#endif
                return item;
            }

            void deallocate_item(bin_config_t const& bincfg, block_t* self, byte* block_address, u16 item_index)
            {
                ASSERT(self->m_item_count > 0);
                ASSERT(item_index < self->m_item_freeindex);
                u16* const pelem = (u16*)((byte*)block_address + ((u64)item_index << bincfg.m_alloc_sizeshift));
#ifdef SUPERALLOC_DEBUG
                nmem::memset(pelem, 0xFEFEFEFE, ((u64)1 << bincfg.m_sizeshift));
#endif
                pelem[0]              = self->m_item_freelist;
                self->m_item_freelist = item_index;
                self->m_item_count--;
            }
        }  // namespace nblock

        byte* fsa_base_address(fsa_t* fsa) { return (byte*)fsa + ((int_t)1 << fsa->m_page_size_shift); }

        static inline bool ptr_is_inside_fsa(fsa_t* fsa, void const* ptr) { return ptr >= (byte*)fsa && ptr < toaddress((byte*)fsa, ((u64)fsa->m_sections_max_index << fsa->m_section_size_shift)); }
        static inline u16  ptr_to_section_index(fsa_t* fsa, void const* ptr) { return ((const byte*)ptr - fsa_base_address(fsa)) >> fsa->m_section_size_shift; }

        // A section contains multiple blocks of the same size defined by bin config
        // This struct is 16 bytes on a 64-bit system
        // The address range of this section starts with N pages that are committed and
        // contains the array of blocks.
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
            void initialize(section_t* self)
            {
                self->m_bin_config       = {0, 0};
                self->m_block_free_index = 0;
                self->m_block_max_count  = 0;
            }

            void checkout(fsa_t* fsa, section_t* self, u32 index, bin_config_t const& bincfg)
            {
                self->m_bin_config       = bincfg;
                self->m_block_free_index = 0;
                self->m_block_max_count  = 1 << (fsa->m_section_size_shift - self->m_bin_config.m_block_sizeshift);
                self->m_block_count      = 0;
                v_alloc_commit(fsa_base_address(fsa) + ((int_t)index << fsa->m_section_size_shift), (int_t)1 << fsa->m_page_size_shift);
            }

            void deinitialize(fsa_t* fsa, section_t* self)
            {
                // TODO, for every block, decommit the memory used by the block
            }

            bool is_full(section_t* self) { return self->m_block_count == self->m_block_max_count; }
            bool is_empty(section_t* self) { return self->m_block_count == 0; }

            byte* get_section_address(fsa_t* fsa, u32 index)
            {
                byte* section_address = fsa_base_address(fsa) + ((u64)index << fsa->m_section_size_shift);
                section_address += ((u64)1 << fsa->m_page_size_shift);
                return section_address;
            }

            block_t* get_block_array(section_t* self) { return (block_t*)((byte*)self + sizeof(section_t)); }

            block_t* get_block(section_t* self, u32 index)
            {
                ASSERT(index < self->m_block_max_count);
                block_t* block_array = get_block_array(self);
                return &block_array[index];
            }

            byte* get_block_address(fsa_t* fsa, section_t* section, u32 block_index) { return get_section_address(fsa, section->m_section_index) + ((u64)block_index << section->m_bin_config.m_block_sizeshift); }

            u32 get_block_index(fsa_t* fsa, section_t* self, byte const* ptr)
            {
                const const byte* section_address = get_section_address(fsa, self->m_section_index);
                return (u32)((ptr - ((byte*)fsa_base_address(fsa) + ((u64)self->m_section_index << fsa->m_section_size_shift))) >> self->m_bin_config.m_block_sizeshift);
            }

            u32 checkout_block(fsa_t* fsa, section_t* self, bin_config_t const& bincfg, block_t*& out_block)
            {
                block_t* block       = nullptr;
                u32      block_index = 0;

#ifdef SUPERALLOC_DEBUG
                nmem::memset(address_of_block(free_block_index), 0xCDCDCDCD, (int_t)1 << self->m_bin_config.m_sizeshift);
#endif
                self->m_block_count++;
                nblock::initialize(block, bincfg);

                out_block = block;
                return block_index;
            }

            void release_block(fsa_t* fsa, section_t* self, block_t* block)
            {
#ifdef SUPERALLOC_DEBUG
                nmem::memset(address_of_block(block->m_section_block_index), 0xFEFEFEFE, (int_t)1 << self->m_bin_config.m_sizeshift);
#endif
                self->m_block_count--;
                // add to free list
            }
        }  // namespace nsection

        // ------------------------------------------------------------------------------
        // ------------------------------------------------------------------------------
        // fsa functions
        inline section_t* get_section(fsa_t* fsa, u16 index) { return (section_t*)((byte*)fsa + sizeof(fsa_t) + index * sizeof(section_t)); }

        void* idx2ptr(fsa_t* fsa, u32 i)
        {
            if (i == D_NILL_U32)
                return nullptr;
            u32 const section_index = (i >> 22) & 0x3FF;
            ASSERT(section_index < fsa->m_sections_max_index);
            section_t* section     = get_section(fsa, section_index);
            u32 const  block_index = (i >> 12) & 0x3FF;
            ASSERT(block_index < section->m_block_max_count);
            block_t*  block         = nsection::get_block(section, block_index);
            byte*     block_address = nsection::get_block_address(fsa, section, block_index);
            const u32 element_index = i & 0xFFF;
            return nblock::idx2ptr(section->m_bin_config, block_address, element_index);
        }

        u32 ptr2idx(fsa_t* fsa, void* ptr)
        {
            if (ptr == nullptr)
                return D_NILL_U32;
            u32 const section_index = ((byte const*)ptr - fsa_base_address(fsa)) >> fsa->m_section_size_shift;
            ASSERT(section_index < fsa->m_sections_max_index);
            section_t*  section       = get_section(fsa, section_index);
            u32 const   block_index   = nsection::get_block_index(fsa, section, (byte const*)ptr);
            block_t*    block         = nsection::get_block(section, block_index);
            byte const* block_address = nsection::get_block_address(fsa, section, block_index);
            u32 const   item_index    = nblock::ptr2idx(section->m_bin_config, block_address, (byte const*)ptr);
            u32 const   idx           = ((section_index & 0x3FF) << 22) | ((block_index & 0x3FF) << 12) | (item_index & 0xFFF);
            return idx;
        }

        static inline bin_config_t alloc_size_to_alloc_config(u32 alloc_size)
        {
            alloc_size = math::ceilpo2(alloc_size);
            s8 const c = math::countTrailingZeros(alloc_size);
            ASSERT(c < (s8)DARRAYSIZE(c_bin_configs));
            return c_bin_configs[c];
        }

        // These 2 arrays follow immediately after this struct in memory
        // u16*  m_active_section_list_per_bincfg;  // sections that still have available free blocks
        // u32*  m_active_block_list_per_bincfg;    // blocks that still have available free items, a list per bin config
        u16* active_section_list_per_bincfg(fsa_t* fsa)
        {
            byte* base = (byte*)fsa + sizeof(fsa_t);
            return (u16*)base;
        }

        section_t* active_section_for_bincfg(fsa_t* fsa, bin_config_t const& bincfg)
        {
            u16* sections_list_heads = active_section_list_per_bincfg(fsa);
            u16  list_head           = sections_list_heads[bincfg.m_alloc_sizeshift];
            if (list_head == D_NILL_U16)
                return nullptr;
            return get_section(fsa, list_head);
        }

        // blocks that still have available free items, a list per bin config
        // u32 here means: upper 16 bits = section index, lower 16 bits = block index
        u32* active_block_list_per_bincfg(fsa_t* fsa)
        {
            byte* base = (byte*)fsa + sizeof(fsa_t);
            base += 32 * sizeof(u16);  // skip active section list
            return (u32*)base;
        }

        bool active_block_for_bincfg(fsa_t* fsa, bin_config_t const& bincfg, section_t*& out_section, block_t*& out_block, u32& out_block_index)
        {
            u32* blocks_list_heads = active_block_list_per_bincfg(fsa);
            u32  list_head         = blocks_list_heads[bincfg.m_alloc_sizeshift];
            if (list_head == D_NILL_U32)
            {
                out_section = nullptr;
                out_block   = nullptr;
                return false;
            }
            u32 const  section_index = (list_head >> 16) & 0xFFFF;
            u32 const  block_index   = (list_head & 0xFFFF);
            section_t* section       = get_section(fsa, (u16)section_index);
            out_section              = section;
            out_block                = nsection::get_block(section, block_index);
            out_block_index          = block_index;
            return true;
        }

        void add_active_block_for_bincfg(fsa_t* fsa, bin_config_t const& bincfg, section_t* section, block_t* block, u32 block_index)
        {
            u32* blocks_list_heads = active_block_list_per_bincfg(fsa);
            u32  block_idx         = ((u32)section->m_section_index << 16) | block_index;
            // u32  old_head                               = blocks_list_heads[bincfg.m_alloc_sizeshift];
            // blocks_list_heads[bincfg.m_alloc_sizeshift] = block_idx | old_head;
            //
            // TODO
            //
        }

        void rem_active_block_for_bincfg(fsa_t* fsa, bin_config_t const& bincfg, section_t* section, block_t* block)
        {
            u32* blocks_list_heads = active_block_list_per_bincfg(fsa);
            // TODO
        }

        section_t* section_array(fsa_t* fsa)
        {
            byte* base = (byte*)fsa + sizeof(fsa_t);
            base += 32 * sizeof(u16);  // skip active section list
            base += 32 * sizeof(u32);  // skip active block list
            // Note: A section is a 16 byte struct, even with a 4KiB page, we can
            // fit 256 - 13 sections in the first page.
            // On MacOS, the page size is 16KiB, so we can fit enough sections in
            // the first page.
            return (section_t*)base;
        }
        section_t* section_at_index(fsa_t* fsa, u32 index)
        {
            ASSERT(index < fsa->m_sections_max_index);
            section_t* sections = section_array(fsa);
            return &sections[index];
        }

        static inline u32 sizeof_alloc(u32 alloc_size) { return math::ceilpo2((alloc_size + (8 - 1)) & ~(8 - 1)); }

        bool checkout_block(fsa_t* fsa, bin_config_t const& bincfg, section_t*& out_section, block_t*& out_block, u32& out_block_index)
        {
            section_t* section = active_section_for_bincfg(fsa, bincfg);
            if (section == nullptr)
            {
                if (!is_nil(fsa->m_sections_free_list))
                {
                    const u16 section_index   = fsa->m_sections_free_list;
                    section                   = section_at_index(fsa, section_index);
                    fsa->m_sections_free_list = section->m_next;
                    nsection::checkout(fsa, section, section_index, bincfg);
                }
                else if (fsa->m_sections_free_index < fsa->m_sections_max_index)
                {
                    const u16 section_index = fsa->m_sections_free_index++;
                    section                 = section_at_index(fsa, section_index);
                    nsection::checkout(fsa, section, section_index, bincfg);
                }
                else
                {
                    // panic, no more sections available
                    return false;
                }
            }

            block_t* block;
            u32      block_index = nsection::checkout_block(fsa, section, bincfg, block);
            if (nsection::is_full(section))
            {
                // TODO this section doesn't have any more free blocks, remove it from the active list
            }

            out_section     = section;
            out_block       = block;
            out_block_index = block_index;
            return true;
        }

        void* allocate(fsa_t* fsa, u32 alloc_size)
        {
            const bin_config_t bincfg = alloc_size_to_alloc_config(alloc_size);
            ASSERT(alloc_size <= ((u32)1 << bincfg.m_alloc_sizeshift));

            section_t* section;
            block_t*   block;
            u32        block_index;
            if (!active_block_for_bincfg(fsa, bincfg, section, block, block_index))
            {
                if (!checkout_block(fsa, bincfg, section, block, block_index))
                    return nullptr;  // panic
                add_active_block_for_bincfg(fsa, bincfg, section, block, block_index);
            }

            byte* block_address = nsection::get_block_address(fsa, section, block_index);
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

            u32 const section_index = ((byte const*)ptr - fsa_base_address(fsa)) >> fsa->m_section_size_shift;
            ASSERT(section_index < fsa->m_sections_max_index);
            section_t*  section       = get_section(fsa, section_index);
            u32 const   block_index   = nsection::get_block_index(fsa, section, (byte const*)ptr);
            block_t*    block         = nsection::get_block(section, block_index);
            byte const* block_address = nsection::get_block_address(fsa, section, block_index);
            u32 const   item_index    = nblock::ptr2idx(section->m_bin_config, block_address, (byte const*)ptr);

            const bool was_full = nblock::is_full(block);
            nblock::deallocate_item(section->m_bin_config, block, (byte*)block_address, (u16)item_index);
            if (nblock::is_empty(block))
            {
                // TODO, release block
                nsection::release_block(fsa, section, block);

                // Is section now empty, if so release section and add to free list
            }
            else if (was_full)
            {
                add_active_block_for_bincfg(fsa, section->m_bin_config, section, block, block_index);
            }
        }

        fsa_t* new_fsa(u64 address_range, u32 section_size)
        {
            // const u32 internal_fsa_address_range  = 256 * cMB;
            // const u32 internal_fsa_section_size   = 8 * cMB;
            // const u32 internal_fsa_pre_size       = 16 * cMB;
            void*     base_address       = v_alloc_reserve(address_range);
            const u32 page_size          = v_alloc_get_page_size();
            const u8  page_size_shift    = v_alloc_get_page_size_shift();
            const u8  section_size_shift = math::ilog2(section_size);
            v_alloc_commit(base_address, (int_t)page_size);
            fsa_t* fsa                 = (fsa_t*)base_address;
            fsa->m_sections_free_index = 0;
            fsa->m_sections_free_list  = D_NILL_U16;
            fsa->m_sections_max_index  = (u16)(address_range >> section_size_shift);
            fsa->m_section_size_shift  = section_size_shift;
            fsa->m_page_size_shift     = page_size_shift;

            u16* active_sections = active_section_list_per_bincfg(fsa);
            u32* active_blocks   = active_block_list_per_bincfg(fsa);
            for (u32 i = 0; i < 32; ++i)
            {
                active_sections[i] = D_NILL_U16;
                active_blocks[i]   = D_NILL_U32;
            }
            return fsa;
        }

        void destroy(fsa_t* fsa)
        {
            // TODO, decommit all sections
        }

    }  // namespace nfsa
}  // namespace ncore
