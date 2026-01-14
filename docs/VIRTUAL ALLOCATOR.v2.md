# Virtual Allocator v2.0

This allocator can handle a maximum of 1024 segments (1 TiB of virtual address space).
A segment is 1 GiB in size, and contains up to 32 regions of 32 MiB each, or
4 regions of 256 MiB each, or 1 region of 1 GiB, etc..
A region consists of chunks or blocks, depending on the allocation size.
Chunks are used for allocations up to 32 KiB, and blocks are used for allocations
larger than 32 KiB. Chunks have two binmaps, one marking all free chunks, and 
another one marking all active chunks.

The allocator has a pre-allocated array of segments and regions.
The allocator makes use of a fixed size allocator (fsa, power-of-two sizes only), 
to be able to allocate binmap level 1 (free and active) for chunks dynamically.

```c++
struct segment_t
{
    u8          m_regions_in_use;        // number of regions in use in this segment
    u8          m_region_size_shift;     // size shift of each region in this segment
    u32         m_region_free_bin0;      // free regions binmap, level 0
    segment_t*  m_next;                  // next segment in linked list
    segment_t*  m_prev;                  // previous segment in linked list
};
```

Example: 256 GiB of virtual address space for allocations:
  - 32 KiB
    - allocator_t struct
    - u32 segments_free_bin0, u32 segments_free_bin1[32] for free segments binmap
    - segment_t*[16], list heads for active segments per region size 
    - region_t*[32], a region list per bin for access to a region with free chunks/blocks
    - region_t*[32], a region list per bin for access to a region with ee chunks/blocks
    - segment_t[1024], pre-allocated segment array for book-keeping data (24 KiB)
  - region_t[256 * 32], pre-allocated region array for book-keeping data (256 KiB)
  - 256 segments * 32 regions * 16 KiB = 128 MiB virtual address space for book-keeping data
  - 256 GiB for the actual virtual address space for allocations

# Region (32 MiB) with Chunks

A separate address space that contains the book-keeping data for each region. 
Each region has 16 KiB (16 KiB pages, or 4 * 4 KiB pages) of book-keeping data associated with it.

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 16 B
        - Max Items per Chunk = 4096
        - Max Chunks per Region = 512 (* 24 bytes = 12 KiB)
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 32 B
        - Max Items per Chunk = 2048
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 64 B
        - Max Items per Chunk = 1024
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 128 B
        - Max Items per Chunk = 512
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 256 B
        - Max Items per Chunk = 256
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 512 B
        - Max Items per Chunk = 128
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 1 KiB
        - Max Items per Chunk = 64
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 2 KiB
        - Max Items per Chunk = 32
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 4 KiB
        - Max Items per Chunk = 16
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 8 KiB
        - Max Items per Chunk = 8
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 16 KiB
        - Max Items per Chunk = 4
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 32 KiB
        - Max Items per Chunk = 2
        - Max Chunks per Region = 512
        - Segment = 1 GiB
        - Max Regions per Segment = 32

# Region (32 MiB) with Blocks

        - Region = 32 MiB
        - Minimum Allocation Size <= 64 KiB
        - Max Blocks per Region = 512 (* 4 bytes = 2 KiB)
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Minimum Allocation Size <= 128 KiB
        - Max Blocks per Region = 256
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Minimum Allocation Size <= 256 KiB
        - Max Blocks per Region = 128
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Minimum Allocation Size <= 512 KiB
        - Max Blocks per Region = 64
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Minimum Allocation Size <= 1 MiB
        - Max Blocks per Region = 32
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Minimum Allocation Size <= 2 MiB
        - Max Blocks per Region = 16
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Minimum Allocation Size <= 4 MiB
        - Max Blocks per Region = 8
        - Segment = 1 GiB
        - Max Regions per Segment = 32

        - Region = 32 MiB
        - Minimum Allocation Size <= 8 MiB
        - Max Blocks per Region = 4
        - Segment = 1 GiB
        - Max Regions per Segment = 32

# Region (256 MiB) with Blocks

        - Region = 256 MiB
        - Minimum Allocation Size <= 16 MiB
        - Max Blocks per Region = 16
        - Segment = 1 GiB
        - Max Regions per Segment = 4

        - Region = 256 MiB
        - Minimum Allocation Size <= 32 MiB
        - Max Blocks per Region = 8
        - Segment = 1 GiB
        - Max Regions per Segment = 4

# Region (1 GiB) with Blocks

        - Region = 1 GiB
        - Minimum Allocation Size <= 64 MiB
        - Max Blocks per Region = 16
        - Segment = 1 GiB
        - Max Regions per Segment = 1

        - Region = 1 GiB
        - Minimum Allocation Size <= 128 MiB
        - Max Blocks per Region = 8
        - Segment = 1 GiB
        - Max Regions per Segment = 1

        - Region = 1 GiB
        - Minimum Allocation Size <= 256 MiB
        - Max Blocks per Region = 4
        - Segment = 1 GiB
        - Max Regions per Segment = 1

        - Region = 1 GiB
        - Minimum Allocation Size <= 512 MiB
        - Max Blocks per Region = 2
        - Segment = 1 GiB
        - Max Regions per Segment = 1

        - Region = 1 GiB
        - Minimum Allocation Size <= 1 GiB
        - Max Blocks per Region = 1
        - Segment = 1 GiB
        - Max Regions per Segment = 1

# Allocations larger than 1 GiB

TODO, any allocation size larger than 1 GiB

