


// static inline void heap_lock_acquire(void)
// {
// 	uintptr_t lock = 0;
// 	uintptr_t this_lock = get_thread_id();
// 	while (!atomic_compare_exchange_strong(&global_heap_lock, &lock, this_lock))
//     {
// 		lock = 0;
// 		wait_spin();
// 	}
// }

// static inline void heap_lock_release(void)
// {
// 	rpmalloc_assert((uintptr_t)atomic_load_explicit(&global_heap_lock, memory_order_relaxed) == get_thread_id(), "Bad heap lock");
// 	atomic_store_explicit(&global_heap_lock, 0, memory_order_release);
// }
