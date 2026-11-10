#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_arena.h"
#include "ccore/c_bitvec.h"
#include "ccore/c_statevec.h"
#include "ccore/c_debug.h"
#include "ccore/c_limits.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"

namespace ncore
{
    namespace nsuperallocv3
    {
        enum
        {
            c4KiB   = 12,
            c8KiB   = 13,
            c16KiB  = 14,
            c32KiB  = 15,
            c64KiB  = 16,
            c128KiB = 17,
            c256KiB = 18,
            c512KiB = 19,
            c1MiB   = 20,
            c2MiB   = 21,
            c4MiB   = 22,
            c8MiB   = 23,
            c16MiB  = 24,
            c32MiB  = 25,
            c64MiB  = 26,
            c128MiB = 27,
            c256MiB = 28,
            c512MiB = 29,
            c1GiB   = 30,
            c2GiB   = 31,
            c4GiB   = 32,
        };

        struct region_config_t
        {
            u8 m_region_type;        // chunk or block based
            u8 m_region_size_shift;  // region size (1 << region_size_shift)
            u8 m_chunk_size_shift;   // chunk/block size shift
        };

        struct alloc_config_t
        {
            u32 m_alloc_size;           // size of the allocation
            u8  m_alloc_range_index;    // index of the allocation range
            u8  m_alloc_range_min;      // index to the minimum allocation config
            u8  m_alloc_range_max;      // index to the maximum allocation config
            u8  m_region_config_index;  // index into region configuration array
        };

    }  // namespace nsuperallocv3

}  // namespace ncore
