#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"

#include "csuperalloc/private/c_items.h"

namespace ncore
{
    items_t::items_t(byte* array, u32 sizeof_item, u32 capacity)
        : m_item_capacity(capacity)
        , m_item_count(0)
        , m_item_size(sizeof_item)
        , m_item_free_index(0)
        , m_array(array)
    {
    }

    u32 items_t::alloc()
    {
        if (m_item_free_index >= m_item_capacity)
            return D_U32_MAX;
        const u32 index = m_item_free_index++;
        m_item_count++;
        return index;
    }

    void items_t::dealloc(u32 index)
    {
        ASSERT(index < m_item_capacity);
        ASSERT(m_item_count > 0);
        m_item_count--;
    }

}  // namespace ncore
