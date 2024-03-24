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

    virtual void* v_idx2ptr(u32 i) const { return &m_data[i]; }
    virtual u32   v_ptr2idx(void* obj) const { return (llindex_t)((llnode_t*)obj - m_data); }

    DCORE_CLASS_PLACEMENT_NEW_DELETE

    mutable llnode_t m_data[1024];
};

dexer_t* gCreateList(alloc_t* alloc, u32 count, lldata_t& lldata)
{
    list_dexer_t* list = new (alloc) list_dexer_t();
    lldata.m_dexer     = list;
    return list;
}

void gDestroyList(alloc_t* alloc, dexer_t* list) { alloc->deallocate(list); }

UNITTEST_SUITE_BEGIN(doubly_linked_list)
{
    UNITTEST_FIXTURE(main)
    {
        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_ALLOCATOR;

        UNITTEST_TEST(init)
        {
            lldata_t lldata;
            dexer_t* list_data = gCreateList(Allocator, 1024, lldata);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            gDestroyList(Allocator, list_data);
        }

        UNITTEST_TEST(insert_1)
        {
            lldata_t lldata;
            dexer_t* list_data = gCreateList(Allocator, 1024, lldata);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            list.insert(lldata, 0);

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(1, list.size());
            CHECK_FALSE(list.m_head.is_nil());

            llnode_t* node = list.idx2node(lldata, 0);
            CHECK_EQUAL(0, node->m_next);
            CHECK_EQUAL(0, node->m_prev);

            gDestroyList(Allocator, list_data);
        }

        UNITTEST_TEST(insert_1_remove_head)
        {
            lldata_t lldata;
            dexer_t* list_data = gCreateList(Allocator, 1024, lldata);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            list.insert(lldata, 0);

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(1, list.size());
            CHECK_FALSE(list.m_head.is_nil());

            llnode_t* node = list.remove_head(lldata);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            CHECK_TRUE(node->m_next == llnode_t::NIL);
            CHECK_TRUE(node->m_prev == llnode_t::NIL);

            gDestroyList(Allocator, list_data);
        }

        UNITTEST_TEST(insert_N_remove_head)
        {
            lldata_t lldata;
            dexer_t* list_data = gCreateList(Allocator, 1024, lldata);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            const s32 count = 256;
            for (s32 i = 0; i < count; ++i)
            {
                list.insert(lldata, i);
            }

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(count, list.size());
            CHECK_FALSE(list.m_head.is_nil());

            for (s32 i = 0; i < count; ++i)
            {
                llnode_t* node = list.remove_head(lldata);

                CHECK_TRUE(node->m_next == llnode_t::NIL);
                CHECK_TRUE(node->m_prev == llnode_t::NIL);
            }

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            gDestroyList(Allocator, list_data);
        }

        UNITTEST_TEST(insert_N_remove_tail)
        {
            lldata_t lldata;
            dexer_t* list_data = gCreateList(Allocator, 1024, lldata);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            const s32 count = 256;
            for (s32 i = 0; i < count; ++i)
            {
                list.insert(lldata, i);
            }

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(count, list.size());
            CHECK_FALSE(list.m_head.is_nil());

            for (s32 i = 0; i < count; ++i)
            {
                llnode_t* node = list.remove_tail(lldata);

                CHECK_TRUE(node->m_next == llnode_t::NIL);
                CHECK_TRUE(node->m_prev == llnode_t::NIL);
            }

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            gDestroyList(Allocator, list_data);
        }

        UNITTEST_TEST(insert_N_remove_item)
        {
            lldata_t lldata;
            dexer_t* list_data = gCreateList(Allocator, 1024, lldata);
            llist_t  list(0, 1024);

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            const s32 count = 256;
            for (s32 i = 0; i < count; ++i)
            {
                list.insert(lldata, i);
            }

            CHECK_FALSE(list.is_empty());
            CHECK_EQUAL(count, list.size());
            CHECK_FALSE(list.m_head.is_nil());

            for (s32 i = 0; i < count; ++i)
            {
                llnode_t* node = list.remove_item(lldata, i);

                CHECK_TRUE(node->m_next == llnode_t::NIL);
                CHECK_TRUE(node->m_prev == llnode_t::NIL);
            }

            CHECK_TRUE(list.is_empty());
            CHECK_EQUAL(0, list.size());
            CHECK_TRUE(list.m_head.is_nil());

            gDestroyList(Allocator, list_data);
        }
    }
}
UNITTEST_SUITE_END
