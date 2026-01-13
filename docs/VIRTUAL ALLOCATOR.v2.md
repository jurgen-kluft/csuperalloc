# Virtual Allocator v2.0

The allocator has a pre-allocated array of segments and regions.

For a 256 GiB virtual address space:
- 256 GiB / 1 GiB = 256 segments
  - u8[256], this value indicates how many regions for each segment
  - u16[256], a next segment index for a linked list of segments with free regions
  - u32[256], this value is a binmap of free regions per segment  
- Group A
  - 256 segments * 32 regions = 8192 regions
    -> region_t[8192] = 8192 * 32 bytes = 256 KiB

8192 * 32 KiB = 256 MiB virtual address space for book-keeping data

A certain amount of segments are meant for Group A, then Group B, and finally Group C.

So, to summarize, the address space layout looks like this:

- one page ?
  - allocator_t struct
  - u8 [256] byte array for each segment indicating how many regions are in use
  - u32 [256] binmap for each segment indicating which regions are free
  - region_t* [32], a region list per bin for quick access to regions with free chunks/blocks
- + 256 MiB virtual address space for book-keeping data
- + 256 GiB virtual 

# Group A 

A separate address space that contains the book-keeping data for each region. 
Each region has 32 KiB (2 * 16 KiB pages, or 8 * 4 KiB pages) of book-keeping data associated with it.

        - Region = 32 MiB
        - Chunk = 64 KiB
        - Minimum Allocation Size <= 16 B
        - Max Items per Chunk = 4096
        - Max Chunks per Region = 512 (* 32 bytes = 16 KiB)
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

# Group B

- Region = 1 GiB, 16384 * Block =  64 KiB
- Region = 1 GiB,  8192 * Block = 128 KiB
- Region = 1 GiB,  4096 * Block = 256 KiB
- Region = 1 GiB,  2048 * Block = 512 KiB
- Region = 1 GiB,  1024 * Block =   1 MiB
- Region = 1 GiB,   512 * Block =   2 MiB
- Region = 1 GiB,   256 * Block =   4 MiB
- Region = 1 GiB,   128 * Block =   8 MiB
- Region = 1 GiB,    64 * Block =  16 MiB
- Region = 1 GiB,    32 * Block =  32 MiB
- Region = 1 GiB,    16 * Block =  64 MiB
- Region = 1 GiB,     8 * Block = 128 MiB
- Region = 1 GiB,     4 * Block = 256 MiB
- Region = 1 GiB,     2 * Block = 512 MiB
- Region = 1 GiB,     1 * Block =   1 GiB

# Group C

TODO

