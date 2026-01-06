#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_arena.h"

#include "csuperalloc/c_superheap.h"

namespace ncore
{

#define SUPERALLOC_DEBUG

    namespace nsuperheap
    {
        void initialize(superheap_t* sh, u64 memory_range, u64 size_to_pre_allocate) { sh->m_arena = narena::new_arena(memory_range, size_to_pre_allocate); }
        void deinitialize(superheap_t* sh) { narena::destroy(sh->m_arena); }

        void* allocate(superheap_t* sh, u32 size, u32 alignment)
        {
            if (size == 0)
                return nullptr;

            void* ptr = narena::alloc(sh->m_arena, size, alignment);
#ifdef SUPERALLOC_DEBUG
            nmem::memset(ptr, 0xCDCDCDCD, (u64)size);
#endif
            return ptr;
        }

        void deallocate(superheap_t* sh, void* ptr)
        {
            if (ptr == nullptr)
                return;
            ASSERT(narena::within_committed(sh->m_arena, ptr));
        }
    }  // namespace nsuperheap
}  // namespace ncore
