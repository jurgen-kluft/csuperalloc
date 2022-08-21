#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"

#include "cvmem/private/c_doubly_linked_list.h"

#include "cunittest/cunittest.h"

using namespace ncore;

extern alloc_t* gTestAllocator;

llnode_t* gCreateList(u32 count, lldata_t& lldata)
{
	llnode_t* list = (llnode_t*)gTestAllocator->allocate(sizeof(llnode_t) * count);
	lldata.m_data = list;
	lldata.m_itemsize = sizeof(llnode_t);
	lldata.m_pagesize = sizeof(llnode_t) * count;
	return list;
}

void gDestroyList(llnode_t* list)
{
	gTestAllocator->deallocate(list);
}

UNITTEST_SUITE_BEGIN(doubly_linked_list)
{
    UNITTEST_FIXTURE(main)
    {
        UNITTEST_FIXTURE_SETUP() {}

        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(init) 
		{
			lldata_t lldata;
			llnode_t* list_data = gCreateList(1024, lldata);
			llist_t list(0, 1024);

			CHECK_TRUE(list.is_empty());
			CHECK_EQUAL(0, list.size());
			CHECK_TRUE(list.m_head.is_nil());

			gDestroyList(list_data);
		}

        UNITTEST_TEST(insert_1) 
		{
			lldata_t lldata;
			llnode_t* list_data = gCreateList(1024, lldata);
			llist_t list(0, 1024);

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

			gDestroyList(list_data);
		}

        UNITTEST_TEST(insert_1_remove_head) 
		{
			lldata_t lldata;
			llnode_t* list_data = gCreateList(1024, lldata);
			llist_t list(0, 1024);

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

			CHECK_TRUE(node->m_next==llnode_t::NIL);
			CHECK_TRUE(node->m_prev==llnode_t::NIL);

			gDestroyList(list_data);
		}

        UNITTEST_TEST(insert_N_remove_head) 
		{
			lldata_t lldata;
			llnode_t* list_data = gCreateList(1024, lldata);
			llist_t list(0, 1024);

			CHECK_TRUE(list.is_empty());
			CHECK_EQUAL(0, list.size());
			CHECK_TRUE(list.m_head.is_nil());

			const s32 count = 256;
			for (s32 i=0; i<count; ++i)
			{
				list.insert(lldata, i);
			}

			CHECK_FALSE(list.is_empty());
			CHECK_EQUAL(count, list.size());
			CHECK_FALSE(list.m_head.is_nil());

			for (s32 i=0; i<count; ++i)
			{
				llnode_t* node = list.remove_head(lldata);

				CHECK_TRUE(node->m_next == llnode_t::NIL);
				CHECK_TRUE(node->m_prev == llnode_t::NIL);
			}

			CHECK_TRUE(list.is_empty());
			CHECK_EQUAL(0, list.size());
			CHECK_TRUE(list.m_head.is_nil());

			gDestroyList(list_data);
		}

        UNITTEST_TEST(insert_N_remove_tail) 
		{
			lldata_t lldata;
			llnode_t* list_data = gCreateList(1024, lldata);
			llist_t list(0, 1024);

			CHECK_TRUE(list.is_empty());
			CHECK_EQUAL(0, list.size());
			CHECK_TRUE(list.m_head.is_nil());

			const s32 count = 256;
			for (s32 i=0; i<count; ++i)
			{
				list.insert(lldata, i);
			}

			CHECK_FALSE(list.is_empty());
			CHECK_EQUAL(count, list.size());
			CHECK_FALSE(list.m_head.is_nil());

			for (s32 i=0; i<count; ++i)
			{
				llnode_t* node = list.remove_tail(lldata);

				CHECK_TRUE(node->m_next == llnode_t::NIL);
				CHECK_TRUE(node->m_prev == llnode_t::NIL);
			}

			CHECK_TRUE(list.is_empty());
			CHECK_EQUAL(0, list.size());
			CHECK_TRUE(list.m_head.is_nil());

			gDestroyList(list_data);
		}

        UNITTEST_TEST(insert_N_remove_item) 
		{
			lldata_t lldata;
			llnode_t* list_data = gCreateList(1024, lldata);
			llist_t list(0, 1024);

			CHECK_TRUE(list.is_empty());
			CHECK_EQUAL(0, list.size());
			CHECK_TRUE(list.m_head.is_nil());

			const s32 count = 256;
			for (s32 i=0; i<count; ++i)
			{
				list.insert(lldata, i);
			}

			CHECK_FALSE(list.is_empty());
			CHECK_EQUAL(count, list.size());
			CHECK_FALSE(list.m_head.is_nil());

			for (s32 i=0; i<count; ++i)
			{
				llnode_t* node = list.remove_item(lldata, i);
				
				CHECK_TRUE(node->m_next == llnode_t::NIL);
				CHECK_TRUE(node->m_prev == llnode_t::NIL);
			}

			CHECK_TRUE(list.is_empty());
			CHECK_EQUAL(0, list.size());
			CHECK_TRUE(list.m_head.is_nil());

			gDestroyList(list_data);
		}

    }
}
UNITTEST_SUITE_END
