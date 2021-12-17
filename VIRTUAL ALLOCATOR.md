# Virtual Memory Allocator

These are my current brain-dump on the virtual memory allocator

1. CPU Fixed-Size-Allocator (FSA), 8 B <= SIZE <= 4 KB
2. Virtual Memory: Alloc, Cache and Free pages back to system

Let's say an APP has 640 GB of address space and it has the following behaviour:

1. Many small allocations (FSA Heap)
2. CPU and GPU allocations (different calls, different page settings, VirtualAlloc/XMemAlloc)
3. Categories of GPU resources have different min/max size, alignment requirements, count and frequency

## Implementations

- Fixed Size Allocator (intrusive) :white_check_mark:
- Coalesce Strategy :white_check_mark:
- Segregated Strategy :white_check_mark:
- Large Strategy :white_check_mark:
- Temporal Strategy :soon:

## Fixed Size Allocator [Ok]

This is an intrusive fixed size allocator targetted at small size allocations.

### Multi Threading

Not too hard to make multi-thread safe using atomics where the only hard multi-threading
problem is page commit/decommit.

### FSA

- RegionSize = 512 x 1024 x 1024
- PageSize = 4 KB (if possible), otherwise 64 KB
- Min >= 8 (this is the first allocator so it should also include this minimum)
- Max <= 4096
- Min/Max/Step = 8/64/8, 64/512/16, 512/1024/64, 1024/2048/128, 2048/4096/256

#### Pros / Cons

- Tiny implementation [+]
- Very low wastage [+]
- Can make use of flexible memory [+]
- Fast [+]
- Can cache a certain amount of free pages [+]
- Difficult to detect memory corruption [-]

## Coalesce Allocator Direct 1 (512 MB)

- Can use more than one instance
- Size range: 4 KB < Size <= 64 KB
  - Size alignment: 256
  - Size-DB: 256 entries
- A memory range of 512 MB
  - Addr-DB: 4096 entries
  - 512 MB / 4096 = 128 KB per address node
- Best-Fit strategy
- Suitable for GPU memory

## Coalesce Allocator Direct 2 (512 MB)

- Can use more than one instance
- Size range: 64 KB < Size <= 512 KB
- Size alignment: 4096
- Size-DB: 128 entries
- A memory range of 512 MB
  - Addr-DB: 512 entries
  - 512 MB / 512 = 1 MB per address node
- Best-Fit strategy
- Suitable for GPU memory

## Segregated Allocator [WIP]

- Segregated:
  - Upon allocation the necessary pages are committed
  - Upon deallocation the pages are decommitted
  - The allocation is 'size' but will only commit used pages
- A reserved memory range (16GB) of virtual pages
- Can use more than one instance
- Sizes to use are multiple of 64KB (page-size)
  Sizes; 512 KB, ..., 16 MB, 32 MB (Also can handle 64 KB, 128 KB and 256 KB)
- Size-Alignment = Page-Size (64 KB)
- Suitable for GPU memory

## Large Size Allocator

- 32 MB < Size < 512 MB
- Size alignment is page size
- Small number of allocations (<32)
- Allocation tracking is done with blocks (kinda like FSA)
- Reserves huge virtual address space (~128 GB)
- Maps and unmaps pages on demand
- Guarantees contiguous memory
- 128 GB / 512 MB = 256 maximum number of allocations

Pros and Cons:

- No headers [+]
- Relatively simple implementation (~200 lines of code) [+]
- Will have no fragmentation [+]
- Size rounded up to page size [-]
- Kernel calls relatively slow [-]

## Proxy allocators for 'commit/decommit' of virtual memory

1. Direct
   Upon allocation virtual memory is committed
   Upon deallocation virtual memory is decommitted
2. Regions
   Upon allocation, newly intersecting regions are committed
   Upon deallocation, intersecting regions that become non-intersected are decommitted
3. Regions with caching
   A region is not directly committed or decommitted but it is first added to a list
   When the list reaches its maximum the oldest ones are decommitted

## Temporal Allocator [WIP]

- For requests that have a very similar life-time (1 or more frames based allocations)
- Contiguous virtual pages
- Moves forward when allocating and wraps around (this is an optimization)
- Large address space (~32 GB)
- Tracked with external bookkeeping
- Suitable for GPU memory
- Configured with a maximum number of allocations

```C++
class xtemporal : public xalloc
{
public:
    void initialize(xalloc* internal_heap, u32 max_num_allocs, xvirtual_memory* vmem, u64 mem_range);

    virtual void* allocate(u32 size, u32 alignment);
    virtual void  deallocate(void* addr);

protected:
    struct xentry
    {
        void* m_address;
        u32   m_size;
        u32   m_state;      // Free / Allocated
    };
    xalloc*          m_internal_heap;
    xvirtual_memory* m_vmem;
    void*            m_mem_base;
    u64              m_mem_range;
    u32              m_entry_write;
    u32              m_entry_read;
    u32              m_entry_max;
    xentry*          m_entry_array;
};
```

## Clear Values (1337 speak)

- memset to byte value
  - Keep it memorable
- 0xF5A10C8E – Fixed-Size (FSA) Memory Allocated
- 0xF5ADE1E7 – Fixed-Size (FSA) Memory Free
- 0xD3A10C8E – Direct Memory Allocated
- 0xD3ADE1E7 – Direct Memory Free
- 0x3A10C8ED – Memory ALlocated
- 0x3DE1E7ED – Memory DEallocated
- 0x96A10C8E – GPU(96U) Memory Allocated
- 0x96DE1E7E – GPU(96U) Memory Free

## Allocation management for `Coalesce Allocator` using array's and lists

### Address and Size Tables

- Min/Max-Alloc-Size, Heap 1 =   8 KB / 128 KB
  - Size-Alignment = 1024 B
  - Find Size is using a size-array of (128K-8K)/1024 = 120 -> 128 entries

- Min/Max-Alloc-Size, Heap 2 = 128 KB / 1024 KB
  - Size-Alignment = 8192 B
  - Find Size is using a size-array of (1024K-128K)/8K = 112 -> 128
  - 1 MB per address node, 256 nodes = covering 256 MB
  - NOTE: This heap configuration may be better of using the Segregated strategy

- Address-Range = 32MB
  We define average size as 2 \* Min-Alloc-Size = 2 \* 8KB = 16KB
  Targetting 8 nodes per block means 32M / (8 \* 16KB) = 256 blocks
  Maximum number of allocations is 32 MB / Min-Alloc-Size = 32K/8 = 4K

- Allocate: Find free node with best size in size-array and remove from size-db
            If size needs to be split add the left-over back to the size-db.
- Deallocate: Find pointer in address-db, check prev-next for coalesce.
  When removing a node we need to figure out if this is the last 'size' entry for
  that block, if it is the last one we need to tag it in the size entry block db.

The size-db for every size entry only needs to store a bitset indicating at which block
we have one or more 'free' nodes of that size. We do have to search it in the list.

Per size we have 256 b = 32 B = 8 W
Size-db is 128 * 32 B = 4 KB

The only downside of using array's and lists is that the size-db is NOT sorted by default.
We can solve this for every size entry by also introducing an address-db and bitset.

For the size entry that stores the larger than Max-Alloc-Size it at least will have an
address bias. However there will still be many different sizes.

Problem: If you do not align the size by Min-Alloc-Size then you can get size fragments
         that are smaller than Min-Alloc-Size.

### Notes 1

Small/Medium Heap Region Size = 256 MB
Medium/Large Heap Region Size = 256 MB

Coalesce Heap Region Size = Small/Medium Heap Region Size
Coalesce Heap Min-Size = 4 KB
Coalesce Heap Max-Size = 64 KB
Coalesce Heap Step-Size = 256 B

Coalesce Heap Region Size = Medium/Large Heap Region Size
Coalesce Heap Min-Size = 64 KB
Coalesce Heap Max-Size = 512 KB
Coalesce Heap Step-Size = 2 KB

### Notes 2

<http://twvideo01.ubm-us.net/o1/vault/gdc2016/Presentations/MacDougall_Aaron_Building_A_Low.pdf>
<http://supertech.csail.mit.edu/papers/Kuszmaul15.pdf>

### Notes Address Space

PS4 = 994 GB address space

### Notes 3

For allocations that are under but close to sizes like 4K, 8/12/16/20 we could allocate them
in a separate allocator. These sizes are very efficient and could benefit from a fast allocator.

### Notes 4

For GPU resources it is best to analyze the resource constraints, for example; On Nintendo
Switch shaders should be allocated with an alignment of 256 bytes and the size should also
be a multiple of 256.

### Notes 5

Memory Debugging:

- Memory Corruption
- Memory Leaks
- Memory Tracking

Writing allocators to be able to debug allocations. We can do this by writing allocators that
are proxy classes that do some extra work/tracking.

### Notes 6

2, 4, 8, 16, 32, 64, 128, 256

{0, 64, 8}, 7

 8   = 65536 / 8  = (8192 + 31) / 32 = 256, 8, 1
16   = 65536 / 16 = (4096 + 31) / 32 = 128, 4, 1
24   = 65536 / 24 = (2730 + 31) / 32 = 86,  4, 1
32   = 65536 / 32 = (2048 + 31) / 32 = 64,  2, 1
40   = 65536 / 40 = (1638 + 31) / 32 = 52,  2, 1
48   = 65536 / 48 = (1365 + 31) / 32 = 43,  2, 1
56   = 65536 / 56 = (1170 + 31) / 32 = 37,  2, 1

{64, 512, 16}, 19

64   = 65536 / 64  = (1024 + 31) / 32 = 32, 1
80   = 65536 / 80  = (819  + 31) / 32 = 26, 1
96   = 65536 / 96  = (682  + 31) / 32 = 22, 1
112  = 65536 / 112 = (585  + 31) / 32 = 19, 1
128  = 65536 / 128 = (512  + 31) / 32 = 16, 1
144  = 65536 / 144 = (455  + 31) / 32 = 15, 1
160  = 65536 / 160 = (409  + 31) / 32 = 13, 1
176  = 65536 / 176 = (372  + 31) / 32 = 12, 1
192  = 65536 / 192 = (341  + 31) / 32 = 11, 1
208  = 65536 / 208 = (315  + 31) / 32 = 10, 1
224  = 65536 / 224 = (292  + 31) / 32 = 10, 1
256 = 65536 / 256 = 256, 8, 1
288 = 65536 / 288 = 227, 8, 1
320 = 65536 / 320 = 204, 8, 1
352 = 65536 / 352 = 186, 8, 1
384 = 65536 / 384 = 170, 8, 1
448 = 65536 / 448 = 146, 8, 1
480 = 65536 / 480 = 136, 8, 1
512 = 65536 / 512 = 128, 4, 1

{512, 1024, 64}, 9

512  = 65536 / 512  = 128, 4, 1
576  = 65536 / 576  = 113, 4, 1
640  = 65536 / 640  = 102, 4, 1
704  = 65536 / 704  = 93,  4, 1
768  = 65536 / 768  = 85,  4, 1
832  = 65536 / 832  = 78,  4, 1
896  = 65536 / 896  = 73,  4, 1
960  = 65536 / 960  = 68,  4, 1
1024 = 65536 / 1024 = 64,  2, 1

{1024, 2048, 128}, 9

1024 = 2, 1
1152 = 2, 1
1280 = 2, 1
1408 = 2, 1
1536 = 2, 1
1664 = 2, 1
1792 = 2, 1
1920 = 2, 1
2048 = 1

{2048, 4096, 256}, 9

2048 = 1
2304 = 1
2560 = 1
2816 = 1
3072 = 1
3328 = 1
3584 = 1
3840 = 1
4096 = 1

### Notes 7

GPU Allocator

Actually for GPU (or others) allocations we also would like to easily store associated data like a CPU write pointer, ref count etc..
So for this it is best if every FSA size gets an address range with an additional address range to store an N byte data block.

e.g FSA Size = 256, |--- FSA element 1 ---|--- FSA element 2 ---|--- FSA element 3 ---|--- FSA element 4 ---|--- FSA element 5 ---| .....
Data Size    =   8, |-Data 1-|-Data 2-|-Data 3-|-Data 4-|-Data 5-|...

With this setup we can quickly get the associated data belonging to an FSA element.

### Notes 8

Chunk  = 2 MB.
Region = 8192 x 2 MB = 16 GB (address range).
Array  = 8192 x sizeof(Chunk) = 8192 x 8 B = 64 KB.
Arena  = Region[32] x 16 GB = 512 GB (address range).
Array  = 32 x 64 KB = 2 MB.

```c++
struct Chunk
{
    static const u64 SIZE = 2 * 1024 * 1024;

    u16 m_next;   // used/free list
    u16 m_prev;   // ..
};

// Small Size allocations
// Can NOT manage GPU memory
// Book-Keeping per page
// 8/16/24/32/../../1024/1280/1536
struct Chunk_Small
{
    static const u64 PAGE = 64 * 1024;
    static const u64 PAGES = Chunk::SIZE / PAGE;

    u16     m_free_list[PAGES];
    u16     m_free_index[PAGES];
    u16     m_elem_used[PAGES];
    u16     m_elem_max[PAGES];
};

// Medium sized allocations
// Can manage GPU memory
// Book-keeping is for chunk
// At this size an allocation can overlap 2 pages
// The amount of allocations is less/equal to 1024 (requires a 2-level bitmap)
// 1792/2048/2560/3072/3584/4096/5120/6144/7168/8192/10240/12288/14336/16384/20480/24576
struct Chunk_Medium
{
    u16     m_max_allocs;
    u16     m_num_allocs;
    u32     m_elem_bitmap0;
    u32     m_elem_bitmap1[32];
};

// Large sized allocations
// Book-keeping is for chunk
// At this size an allocation can overlap 2 pages
// The amount of allocations is less/equal to 64 (single level bitmap)
// 28672\32768\36864\40960\49152\57344\65536\81920\98304\114688\131072\163840\196608\229376\262144
struct Chunk_Large
{
    u16     m_max_allocs;
    u16     m_num_allocs;
    u64     m_elem_bitmap;
};

// =======================================================================================
// Huge size allocations
// At this size, size alignment is page-size
// The following size-bins are managed: 512KB/1MB/2MB/4MB/8MB/16MB/32MB
struct Huge
{
    struct Block  // 32 MB
    {
        u16 m_next;
        u16 m_prev;
        u64 m_elem_bitmap;
    };

    u32     m_free_list;
    Block   m_blocks[4096];
};

struct Region
{
    static const u64 CHUNKS = 8192;

    Chunk   m_chunks[CHUNKS];
    u16     m_free_chunk_list;
    u16     m_used_chunk_list_per_size[];
};

struct Arena
{
    static const u64 ADDRESS_SPACE = (u64)512 * 1024 * 1024 * 1024;
    static const u32 REGIONS = ADDRESS_SPACE / (Chunk::SIZE * Region::CHUNKS);
    u32     m_region_bitmap;
    Region  m_regions[REGIONS];
};
```
