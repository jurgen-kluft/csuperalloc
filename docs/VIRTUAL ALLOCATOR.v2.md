# Virtual Allocator v2.0

Each allocation size has a region type to obtain a chunk from, that is all we need, this information can be stored in a simple array.

We can create a 'segment' allocator which can own the address space and give us a power-of-two region upon request.
- min-size = 4 MiB, shift = 22
- max-size = 1 GiB, shift = 30

Allocations larger than a certain size, like 128 KiB, are handled differently, and they are handled directly by a region.

```c++
struct cchunk_t
{
    i8      m_region_index;     // index into region array (back reference)
    i8      m_chunk_index;      // index of the chunk in the region (back reference)
    i8      m_shift_size;       // shift size of the chunk (1 << shift_size = chunk size)
    i8      m_bin_levels;       // number of bin levels in the binmap for free elements
    u16     m_chunk_prev;       // previous chunk in the list
    u16     m_chunk_next;       // next chunk in the list
    u32     m_pages_committed;  // number of committed pages in the chunk
    u32     m_element_size;     // size of each element in the chunk
    u32     m_element_count;    // number of elements in the chunk
    u32     m_element_index;    // current element index
    u64     m_bin0;             // binmap, level 0 (6) 
    u64*    m_bin1;             // binmap, level 1 (6)
};

const u8 c_region_type_chunks   = 0;

// We introduce an intermediate structure called region to reduce the impact of small chunks to the higher level.
// A region has a variable size (4 MiB to 1 GiB) and consists of N same size chunks, the following types are defined:
//   - 4 MiB, 64 chunks of 64 KiB
//   - 8 MiB, 64 chunks of 128 KiB
//   - 16 MiB, 64 chunks of 256 KiB
//   - 32 MiB, 64 chunks of 512 KiB
//   - 64 MiB, 64 chunks of 1 MiB
//   - 128 MiB, 64 chunks of 2 MiB
//   - 256 MiB, 64 chunks of 4 MiB
//   - 512 MiB, 64 chunks of 8 MiB
//   - 1 GiB, 64 chunks of 16 MiB

const u8 c_region_type_blocks    = 1;  

// A region can also be configured to handle large allocations (m_region_type = c_region_type_blocks).
// Allocation sizes handled are for example:
//   region size,        block size,                  allocation sizes
//     16 MiB,             256 KiB,                 128 KiB upto 256 KiB
//     32 MiB,             512 KiB,                 256 KiB upto 512 KiB
//     64 MiB,             1 MiB,                   512 KiB upto 1 MiB
//    128 MiB,             2 MiB,                   1 MiB upto 2 MiB
//    256 MiB,             4 MiB,                   2 MiB upto 4 MiB
//    512 MiB,             8 MiB,                   4 MiB upto 8 MiB
//      1 GiB,            16 MiB / 512 MiB,         8 MiB upto 16 MiB / 512 MiB

struct cregion_t
{
    byte*        m_base_address;                  // base address of the region
    u64          m_free_map;                      // bit map of free chunks / blocks in this region
    u32*         m_array;                         // array of chunks or pages per block
    u8           m_region_config_index;           // region configuration
    u16          m_region_prev;                   // previous region in the list
    u16          m_region_next;                   // next region in the list
};

struct cregion_config_t
{
    u8    m_index;                 // global index of region
    u8    m_mode;                  // region mode, chunks / blocks
    i8    m_region_size_shift;     // shift size of this region
    i8    m_chunk_size_shift;      // shift size of chunks in this region
};

static cregion_config_t s_region_config[] =
{
    // index,         mode,          region,  chunk/block
    {0,        c_region_type_chunks,   22,      16},     // 4 MiB, 64 KiB
    {1,        c_region_type_chunks,   23,      17},     // 8 MiB, 128 KiB
    {2,        c_region_type_chunks,   24,      18},     // 16 MiB, 256 KiB
    {3,        c_region_type_chunks,   25,      19},     // 32 MiB, 512 KiB
    {4,        c_region_type_chunks,   26,      20},     // 64 MiB, 1 MiB
    {5,        c_region_type_chunks,   27,      21},     // 128 MiB, 2 MiB
    {6,        c_region_type_chunks,   28,      22},     // 256 MiB, 4 MiB
    {7,        c_region_type_chunks,   29,      23},     // 512 MiB, 8 MiB
    {8,        c_region_type_chunks,   30,      24},     // 1 GiB, 16 MiB
    {9,        c_region_type_blocks,   22,      16},     // 4 MiB, 64 KiB
    {10,       c_region_type_blocks,   23,      17},     // 8 MiB, 128 KiB
    {11,       c_region_type_blocks,   24,      18},     // 16 MiB, 256 KiB
    {12,       c_region_type_blocks,   25,      19},     // 32 MiB, 512 KiB
    {13,       c_region_type_blocks,   26,      20},     // 64 MiB, 1 MiB
    {14,       c_region_type_blocks,   27,      21},     // 128 MiB, 2 MiB
    {15,       c_region_type_blocks,   28,      22},     // 256 MiB, 4 MiB
    {16,       c_region_type_blocks,   28,      23},     // 256 MiB, 8 MiB
    {17,       c_region_type_blocks,   28,      24},     // 512 MiB, 16 MiB
    {18,       c_region_type_blocks,   28,      25},     // 512 MiB, 32 MiB
};

struct cbin_config_t
{
    u32   m_element_size;      // size of elements in this bin
    u8    m_index;             // bin index
    u8    m_region_config;     // region config index for this bin
};

struct calloc_t
{
    byte*             m_address_base;                          // base address of the superallocator
    int_t             m_address_size;                          // size of the address space
    u16*              m_address_regions;                       // accross the whole address space
    u16*              m_active_region_per_index;               // active region-lists per region index
    u16*              m_active_chunk_per_alloc_size;           // active chunk-lists per allocation size
    segment_alloc_t   m_region_allocator;                      // region allocator
    i32               m_page_size;                             // system page size
    i32               m_chunk_capacity;                        // maximum number of chunks
    i32               m_chunk_count;                           // current number of chunks
    i32               m_region_capacity;                       // maximum number of chunks
    i32               m_region_count;                          // current number of chunks
    cchunk_t*         m_chunk_free_list;                       // free list of chunks
    cchunk_t*         m_chunk_array;                           // array of chunks
    cregion_t*        m_region_free_list;                      // free list of regions
    cregion_t*        m_region_array;                          // array of chunks
};

// Huge allocations are handled by this structure
//  bin(0), 4 -> 8 MiB 
//  bin(1), 8 -> 16 MiB 
//  bin(2), 16 -> 32 MiB 
//  bin(3), 32 -> 64 MiB 
//  bin(4), 64 -> 128 MiB 
//  bin(5), 128 -> 256 MiB 
//  bin(6), 256 -> 512 MiB 
//  bin(7), 512 MiB -> 1 GB
struct chuge_t
{
    void* m_bins[8]; 

};

// Chunk-Size = 4 MiB (1 << 22)
// 64 GiB / 4 MiB = 16384 chunks = 16384 * sizeof(u32) = 64 KiB

// Chunk-Size = 256 KiB (1 << 18)
// 64 GiB / 256 KiB = 262144 chunks = 262144 * sizeof(u32) = 1 MiB
```

