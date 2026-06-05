# Virtual Allocator v2.0

This allocator can handle a maximum of 1024 segments (1 TiB of virtual address space).
A segment is locked to 1 GiB in size, and contains N regions (1 <= N <= 128), e.g.
4 regions of 256 MiB each, or 2 regions of 512 MiB each, etc..
A region consists of N chunks or blocks, depending on the allocation size.
Chunks are used for allocations up to 32 KiB, and blocks are used for allocations
larger than 32 KiB. Regions have two binmaps, one marking free chunks, and another
marking active chunks.

The allocator has a pre-allocated array of segments, while regions are allocated
from an array of regions that can grow dynamically using virtual memory. 

In `ccore` we now have `block bin`, `chunk bin`, and `index bin`, that can be used to take over a lot of the infra-structure that is need for this allocator. 

# Active Region Sizes

- 8 MiB
- 16 MiB
- 32 MiB
- 256 MiB
- 512 MiB
- 1 GiB

# Dynamic Allocation

- segment; allocate an array of region indices
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

---

# Memory Architecture Specifications



## Core System Constraints
* **Region Size:** 8 MiB up to 1 GiB
* **Segment Size:** fixed at 1 GiB
* **Max Regions per Segment:** 128

---

## 1. Sub-Page Chunk Allocations (Small Objects)
For allocations under 16 KiB, the allocator uses **Chunks** to manage small allocations. Each chunk is a contiguous memory block used to allocate from. The allocator maintains two binmaps for each region: one for tracking free chunks and another for tracking active chunks. The chunk sizes and their corresponding configurations are as follows:


| Allocation Size Range | Region Size | Strategy | Chunk Size | Items/Chunk | Chunks/Region |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 16 B ≤ Size < 32 B | 8 MiB | Chunk | 16 KiB | ≤ 1024 | 512 |
| 32 B < Size ≤ 64 B | 8 MiB | Chunk | 32 KiB | < 1024 | 256 |
| 64 B < Size ≤ 128 B | 8 MiB | Chunk | 64 KiB | < 1024 | 128 |
| 128 B < Size ≤ 256 B | 8 MiB | Chunk | 64 KiB | < 512 | 128 |
| 256 B < Size ≤ 512 B | 8 MiB | Chunk | 64 KiB | < 256 | 128 |
| 512 B < Size ≤ 1 KiB | 8 MiB | Chunk | 64 KiB | < 128 | 128 |
| 1 KiB < Size ≤ 2 KiB | 8 MiB | Chunk | 64 KiB | < 64 | 128 |
| 2 KiB < Size ≤ 4 KiB | 8 MiB | Chunk | 64 KiB | < 32 | 128 |
| 4 KiB < Size ≤ 8 KiB | 8 MiB | Chunk | 64 KiB | < 16 | 128 |
| 8 KiB < Size ≤ 16 KiB | 8 MiB | Chunk | 64 KiB | < 8 | 128 |

---

## 2. Page Scale Allocations (Medium Objects)
From 16 KiB up to 512 MiB, the allocator uses a `block` based strategy instead of the `chunk` strategy. It allocates sizes calculated by the number of pages, managed by what we call a `block`, which tracks the amount of committed pages.

Note: On MacOS a virtual page is 16 KiB, while on Windows and Linux it is 4 KiB.


| Allocation Size Range | Region Size | Strategy | Block Size | Blocks/Region | Regions/Segment |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 16 KiB < Size ≤ 32 KiB | 8 MiB | Block | 32 KiB | 256 | 128 |
| 32 KiB < Size ≤ 64 KiB | 8 MiB | Block | 64 KiB | 128 | 128 |
| 64 KiB < Size ≤ 128 KiB | 16 MiB | Block | 128 KiB | 128 | 64 |
| 128 KiB < Size ≤ 256 KiB | 32 MiB | Block | 256 KiB | 128 | 32 |
| 256 KiB < Size ≤ 512 KiB | 32 MiB | Block | 512 KiB | 64 | 32 |
| 512 KiB < Size ≤ 1 MiB | 32 MiB | Block | 1 MiB | 32 | 32 |
| 1 MiB < Size ≤ 2 MiB | 32 MiB | Block | 2 MiB | 16 | 32 |
| 2 MiB < Size ≤ 4 MiB | 32 MiB | Block | 4 MiB | 8 | 32 |
| 4 MiB < Size ≤ 8 MiB | 32 MiB | Block | 8 MiB | 4 | 32 |
| 8 MiB < Size ≤ 16 MiB | 256 MiB | Block | 16 MiB | 16 | 4 |
| 16 MiB < Size ≤ 32 MiB | 256 MiB | Block | 32 MiB | 8 | 4 |
| 32 MiB < Size ≤ 64 MiB | 256 MiB | Block | 64 MiB | 4 | 4 |
| 64 MiB < Size ≤ 128 MiB | 512 MiB | Block | 128 MiB | 4 | 2 |
| 128 MiB < Size ≤ 256 MiB | 512 MiB | Block | 256 MiB | 2 | 2 |
| 256 MiB < Size ≤ 512 MiB | 1 GiB | Block | 512 MiB | 2 | 1 |
| 512 MiB < Size ≤ 1 GiB | 1 GiB | Block | 1 GiB | 1 | 1 |


# Allocations larger than 1 GiB

Any allocation size larger than 1 GiB will be allocated from a separate virtual address space or directly from the OS.

