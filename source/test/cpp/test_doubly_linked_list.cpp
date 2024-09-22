#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"

#include "csuperalloc/private/c_doubly_linked_list.h"
#include "csuperalloc/test_allocator.h"

#include "cunittest/cunittest.h"

using namespace ncore;

class list_dexer_t : public dexer_t
{
public:
    list_dexer_t() {}

    virtual void* v_idx2ptr(u32 i) { return &m_data[i]; }
    virtual u32   v_ptr2idx(void const* obj) const { return (llindex_t)((llnode_t*)obj - m_data); }

    DCORE_CLASS_PLACEMENT_NEW_DELETE

    llnode_t* m_data;
};

dexer_t* gCreateList(alloc_t* alloc, u32 count)
{
    list_dexer_t* list = alloc->construct<list_dexer_t>();
    list->m_data       = (llnode_t*)alloc->allocate(sizeof(llnode_t) * count);
    return list;
}

void gDestroyList(alloc_t* alloc, dexer_t* list)
{
    list_dexer_t* l = (list_dexer_t*)list;
    alloc->deallocate(l->m_data);
    alloc->destruct(l);
}

UNITTEST_SUITE_BEGIN(doubly_linked_list)
{
    UNITTEST_FIXTURE(main)
    {
        UNITTEST_ALLOCATOR;

        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(init)
        {
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));
        }

        UNITTEST_TEST(insert_1)
        {
            dexer_t* lldata = gCreateList(Allocator, 1024);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            list.insert(lldata, 0);

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(1, list.size());
            CHECK_FALSE(ll_is_nil(list.m_head));

            llnode_t* node = list.idx2node(lldata, 0);
            CHECK_EQUAL(0, node->m_next);
            CHECK_EQUAL(0, node->m_prev);

            gDestroyList(Allocator, lldata);
        }

        UNITTEST_TEST(insert_1_remove_head)
        {
            dexer_t* lldata = gCreateList(Allocator, 1024);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            list.insert(lldata, 0);

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(1, list.size());
            CHECK_FALSE(ll_is_nil(list.m_head));

            llnode_t* node = list.remove_head(lldata);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            CHECK_TRUE(node->m_next == llnode_t::NIL);
            CHECK_TRUE(node->m_prev == llnode_t::NIL);

            gDestroyList(Allocator, lldata);
        }

        UNITTEST_TEST(insert_N_remove_head)
        {
            dexer_t* lldata = gCreateList(Allocator, 1024);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            const s32 count = 256;
            for (s32 i = 0; i < count; ++i)
            {
                list.insert(lldata, i);
            }

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(count, list.size());
            CHECK_FALSE(ll_is_nil(list.m_head));

            for (s32 i = 0; i < count; ++i)
            {
                llnode_t* node = list.remove_head(lldata);

                CHECK_TRUE(node->m_next == llnode_t::NIL);
                CHECK_TRUE(node->m_prev == llnode_t::NIL);
            }

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            gDestroyList(Allocator, lldata);
        }

        UNITTEST_TEST(insert_N_remove_tail)
        {
            dexer_t* lldata = gCreateList(Allocator, 1024);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            const s32 count = 256;
            for (s32 i = 0; i < count; ++i)
            {
                list.insert(lldata, i);
            }

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(count, list.size());
            CHECK_FALSE(ll_is_nil(list.m_head));

            for (s32 i = 0; i < count; ++i)
            {
                llnode_t* node = list.remove_tail(lldata);

                CHECK_TRUE(node->m_next == llnode_t::NIL);
                CHECK_TRUE(node->m_prev == llnode_t::NIL);
            }

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            gDestroyList(Allocator, lldata);
        }

        UNITTEST_TEST(insert_N_remove_item)
        {
            dexer_t* lldata = gCreateList(Allocator, 1024);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            const s32 count = 256;
            for (s32 i = 0; i < count; ++i)
            {
                list.insert(lldata, i);
            }

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(count, list.size());
            CHECK_FALSE(ll_is_nil(list.m_head));

            for (s32 i = 0; i < count; ++i)
            {
                llnode_t* node = list.remove_item(lldata, i);

                CHECK_TRUE(node->m_next == llnode_t::NIL);
                CHECK_TRUE(node->m_prev == llnode_t::NIL);
            }

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(ll_is_nil(list.m_head));

            gDestroyList(Allocator, lldata);
        }
    }
}
UNITTEST_SUITE_END
