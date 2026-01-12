# Virtual Allocator v2.0

Each allocation size has a region type to obtain a chunk from, that is all we need, this information can be stored in a simple array.

The allocator has 2 internal heaps, one is an arena used for allocations that happen during initialization, the other is used for metadata allocations during runtime.
The one used during runtime is a custom FSA (fixed-size allocations) that allocates items of a fixed (small) size, the runtime allocations
are infrequent and of the following sizes:
- binmap, bin1: <= 512 bytes
- region; array, <= 256 bytes


Each chunk can hold a maximum of 2048 elements of size 16 bytes, so we have a 
minimum chunk size of 32 KiB (16 * 2048).

  4 KiB * 256 =   1 MiB    one block holds <= 256 chunks                         CHUNK
  1 MiB * 256 = 256 MiB    256 regions, each region containing 1 MiB blocks      REGION
256 MiB * 256 =  64 GiB   256 segments, each segment containing 256 MiB regions  SEGMENT

So are we able to get all each structures as small as possible?
If we make a chunk 4 KiB, we can hold a maximum of 256 elements of size 16 bytes.
Let's see what happens when we use this value, 256 as our magic key.

// A chunk of 4 KiB, can hold a maximum of 256 elements of 16 bytes each
// 8 bytes
struct chunk_t
{
    u8  m_next;                // next/prev for linked list of active chunks
    u8  m_prev;                // next/prev for linked list
    u8  m_capacity;            // maximum number of elements in the chunk
    u8  m_count;               // number of elements in the chunk
    u8  m_free_index;          // current element free index
    // u32 m_bin[8];           // where do we store the binmap
};

// A block of 1 MiB, can hold a maximum of 256 chunks of 4 KiB each
// 8 bytes
struct block_t
{
    u16 m_next;                // next/prev for linked list of active blocks
    u16 m_prev;                // next/prev for linked list
    u8  m_capacity;            // maximum number of chunks in the block
    u8  m_count;               // number of chunks in the block
    u8  m_free_index;          // current chunk free index
    // u32 m_bin[8];          // where do we store the binmap 
};

1
2
4
8
16
32
64
128 


