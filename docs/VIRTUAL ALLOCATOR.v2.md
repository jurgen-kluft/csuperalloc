# Virtual Allocator v2.0

This allocator can handle a maximum of 1024 segments (1 TiB of virtual address space).
A segment is 1 GiB in size, and contains up to 32 regions of 32 MiB each, or
4 regions of 256 MiB each, or 1 region of 1 GiB, etc..
A region consists of chunks or blocks, depending on the allocation size.
Chunks are used for allocations up to 32 KiB, and blocks are used for allocations
larger than 32 KiB. Regions have two binmaps, one marking free chunks, and another
marking active chunks.

The allocator has a pre-allocated array of segment, while regions are allocated
from an array of regions that can grow dynamically using virtual memory. But this
virtual memory is part of the total virtual address space managed by the allocator.
The allocator does make use of a fixed size allocator (fsa, power-of-two sizes only), 
to be able to allocate binmap level 1 (free and active) for chunks dynamically.
The fsa is also used to allocate the region index arrays for segments, since each
segment might require a different number of regions depending on the region size.

```c++
struct segment_t
{
    u8          m_regions_in_use;        // number of regions in use in this segment
    u8          m_region_size_shift;     // size shift of each region in this segment
    u32         m_region_free_bin0;      // free regions binmap, level 0
    u16         m_next;                  // next segment in linked list
    u16         m_prev;                  // previous segment in linked list
};
```

Example: 256 GiB of virtual address space for allocations:
  - 16 KiB
    - allocator_t struct, 128 bytes
    - alloc_config_t[128], 128 * 8 bytes = 1 KiB allocation config array
    - segment_t*[16], list heads for active segments per region size, 128 bytes
    - u32[128], a region list per 'index' for access to a region with free chunks/blocks, 128 * 4 bytes = 512 bytes
    - u32[128], a region list per 'index' for access to a region with active chunks/blocks, 128 * 4 bytes = 512 bytes
    - segment_t[1024], pre-allocated segment array for book-keeping data (12 KiB), 1024 segments * 12 bytes = 12 KiB
  - region_t[], virtual region array for allocating region_t structs from (32 B * 65536 = 2 MiB, 2 MiB of virtual address space)
  - chunk_t[], virtual chunk array for allocating chunk_t structs from (16 B * 1024*1024 = 16 MiB, 16 MiB of virtual address space)
  - 256 GiB for the actual virtual address space for allocations

Active Region Sizes:
- 32 MiB
- 256 MiB
- 512 MiB
- 1 GiB

# Dynamic Allocation

Who?:

- segment; allocate an array of region indices, depending on the region size.
- chunk; allocate binmap level 1 for free items

Sizes:

- 8 bytes
- 16 bytes
- 32 bytes
- 64 bytes
- 128 bytes
- 256 bytes
- 512 bytes
- 1 KiB
- 2 KiB

Notes:

- The array of region indices per segment could come from the first region book-keeping data since
  there is still some space left there.
  Current usage is like this: 
    free chunks bin1 + active chunks bin1 + region indices + 512 * sizeof(chunk_t) =
     64 + 64 + 32*4 + 512 * 16 bytes = 128 B + 128 B + 8 KiB = 256 B + 8 KiB 

# Region (32 MiB) with Chunks

A separate address space that contains the book-keeping data for each region. 
Each region has 16 KiB (16 KiB pages, or 4 * 4 KiB pages) of book-keeping data associated with it.

        - Region = 8 MiB
        - Chunk = 16 KiB
        - 16 <= Allocation Size < 32 B
        - Max Items per Chunk <= 1024
        - Max Chunks per Region = 512 
        - Segment = 1 GiB
        - Max Regions per Segment = 128

        - Region = 16 MiB
        - Chunk = 32 KiB
        - 32 <= Allocation Size < 64 B
        - Max Items per Chunk <= 1024
        - Max Chunks per Region = 512 
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 64 <= Allocation Size < 128 B
        - Max Items per Chunk = 1024
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 128 <= Allocation Size < 256 B
        - Max Items per Chunk = 512
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 256 <= Allocation Size < 512 B
        - Max Items per Chunk = 256
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 512 <= Allocation Size < 1 KiB
        - Max Items per Chunk = 128
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 1 KiB <= Allocation Size < 2 KiB
        - Max Items per Chunk = 64
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 2 KiB <= Allocation Size < 4 KiB
        - Max Items per Chunk = 32
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 4 KiB <= Allocation Size < 8 KiB
        - Max Items per Chunk = 16
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 8 KiB <= Allocation Size < 16 KiB
        - Max Items per Chunk = 8
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - 16 KiB <= Allocation Size < 32 KiB
        - Max Items per Chunk = 4
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

# Region (32 MiB) with Blocks

        - Region = 32 MiB
        - Block = 64 KiB
        - 32 KiB <= Allocation Size <= 64 KiB
        - Max Blocks per Region = 512 (* 4 bytes = 2 KiB)
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Block = 128 KiB
        - 64 KiB < Allocation Size <= 128 KiB
        - Max Blocks per Region = 256
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Block = 256 KiB
        - 128 KiB < Allocation Size <= 256 KiB
        - Max Blocks per Region = 128
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Block = 512 KiB
        - 256 KiB < Allocation Size <= 512 KiB
        - Max Blocks per Region = 64
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Block = 1 MiB
        - 512 KiB < Allocation Size <= 1 MiB
        - Max Blocks per Region = 32
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Block = 2 MiB
        - 1 MiB < Allocation Size <= 2 MiB
        - Max Blocks per Region = 16
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Block = 4 MiB
        - 2 MiB < Allocation Size <= 4 MiB
        - Max Blocks per Region = 8
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Block = 8 MiB
        - 4 MiB < Allocation Size <= 8 MiB
        - Max Blocks per Region = 4
        - Segment = 1 GiB
        - Max Regions per Segment = 32

# Region (256 MiB) with Blocks

        - Region = 256 MiB
        - Block = 16 MiB
        - 8 MiB < Allocation Size <= 16 MiB
        - Max Blocks per Region = 16
        - Segment = 1 GiB
        - Max Regions per Segment = 4

        - Region = 256 MiB
        - Block = 32 MiB
        - 16 MiB < Allocation Size <= 32 MiB
        - Max Blocks per Region = 8
        - Segment = 1 GiB
        - Max Regions per Segment = 4

# Region (1 GiB) with Blocks

        - Region = 256 MiB
        - Block = 64 MiB
        - 32 MiB < Allocation Size <= 64 MiB
        - Max Blocks per Region = 4
        - Segment = 1 GiB
        - Max Regions per Segment = 4

        - Region = 256 MiB
        - Block = 128 MiB
        - 64 MiB < Allocation Size <= 128 MiB
        - Max Blocks per Region = 2
        - Segment = 1 GiB
        - Max Regions per Segment = 4

        - Region = 512 MiB
        - Block = 256 MiB
        - 128 MiB < Allocation Size <= 256 MiB
        - Max Blocks per Region = 2
        - Segment = 1 GiB
        - Max Regions per Segment = 2

        - Region = 1 GiB
        - Block = 512 MiB
        - 256 MiB < Allocation Size <= 512 MiB
        - Max Blocks per Region = 2
        - Segment = 1 GiB
        - Max Regions per Segment = 1

        - Region = 1 GiB
        - 512 MiB < Allocation Size <= 1 GiB
        - Max Blocks per Region = 1
        - Segment = 1 GiB
        - Max Regions per Segment = 1

# Allocations larger than 1 GiB

TODO, any allocation size larger than 1 GiB

