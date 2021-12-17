---
marp: true
theme: gaia
_class: lead
paginate: true
backgroundColor: #f0f
backgroundImage: url('superalloc-background.jpg')
---

# **SuperAlloc**

Virtual Memory Allocator for 64-bit Games or Applications

---

# Background

Current well known memory allocators like DLMalloc are mainly based on handling small size requests using a simple FSA approach and for medium/large requests to use a coalesce best-fit approach.

---

# Problems

* Waste a lot of memory
* Suffers from fragmentation
* Easy to crash when intrusive bookkeeping data is overwritten making it hard to find the cause
* Not able to use it for GPU memory, need another implementation to handle GPU memory

---

# Memory Fragmentation

* Heap fragmented in small non-contigues blocks
* Allocations can fail despite enough memory
* Caused by mixed allocation lifetimes

```md
               - Allocation 128 KB                                                                   
              /                                                                                     
        +---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---+       
  0 B   | F | X | F | X | X | X | X | X | X | X | X | F | X | X | X | X | X | F | X | X | X |   1 MB
        +---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---+       
                  \                                                                                 
                   -Free                                                                           
```

---

# Design Goals

* Low fragmentation
* High utilisation
* Configurable
* Support many platforms (PC, Mac, Linux, Playstation, Xbox, Nintendo)
* Support GPU and texture streaming
* Debugging support

---

# Virtual Memory

* Process uses virtual addresses
* Virtual addresses mapped to physical addresses
* CPU looks up physical address
* Requires OS and hardware support

---

# Benefits of Virtual Memory

* Reduced memory fragmentation
  * Fragmentation is address fragmentation
  * We use virtual addresses
  * Virtual address space is larger than physical
  * Contiguous virtual memory not contiguous in physical memory
* Able to release back unused 'memory'

---

# Virtual Address Space

```md

 Virtual Address Space

 +-------------------------------------------------------------------------------------------------+       
 |                                                                                                 | 944 GB
 +-------------------------------------------------------------------------------------------------+       

 Physical Memory                                                                                           

 +-+
 | | 8 GB
 +-+
```

---

# Memory Pages

* Mapped in pages
* x64 supports:
  * 4 KB and 2 MB pages
* PlayStation 4 OS uses:
  * 16 KB (4x4 KB) and 2 MB
* GPU has more sizes

---

# Page Sizes

* 2 MB pages are the fastest
* 16 KB pages wastes less memory
* We use 64 KB (4 x 16 KB pages on PlayStation 4)
  * Smallest optimal size for PlayStation 4 GPU
* Also able to use 4 or 16 KB for special cases

---

# PlayStation Onion Bus & Garlic Bus

* CPU & GPU can access both
  * But at different bandwidths
* Onion = fast CPU access
* Garlic = fast GPU access

---

# SuperAllocator

* Splits up large virtual address space into `blocks`
* Every `block` is dedicated to one specific `chunk` size
* All bookkeeping data is outside of the managed memory
* A superalloc allocator manages a range of allocation sizes
* Only uses 2 data structures:
  * Doubly Linked List (260 lines)
  * BinMap (3 level hierarchical bitmap) (180 lines)
* Number of code lines = 1200, excluding the 2 data structures

---

# Allocator

```c++
class alloc_t
{
public:
    virtual void* allocate(u32 size, u32 align) = 0;
    virtual u32   deallocate(void* ptr) = 0;

    virtual u32   get_size(void* ptr) = 0;      // Easy to add
    virtual void* get_gpu(void* cpuptr) = 0;    // Requires some code to support this
};
```

---

# SuperAllocator Virtual Address Space

Address space is divided into blocks of 1 GB.
(X = Used, F = Free)

```md
                -> Block 1 GB                                                                           
               /                                                                                      
              /                                                                                       
        +---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---+       
        |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |       
 0 GB   | X | X | F | F | F | F | F | F | F | F | F | F | F | F | F | F | F | F | 128 GB
        |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |       
        +---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---+       

```

---

# Block

A block of 1 GB is divided into chunks of a specific size.

```md
         Chunk (64 KB)
         /
     +++/+++++++++++++++++++++++++++++++
0 GB | X | F | F |                     | 1 GB
     +---+---+---+---------------------+

```

Chunk size is a power-of-2 multiple of 64 KB, some chunk sizes are 64 KB, 128 KB, 256 KB, 512 KB, 1 MB, 2 MB, 4 MB, 8 MB ... 256 MB.

---

# Chunk

The smallest chunk-size of 64 KB, handled by the first super alloc, is holding allocation sizes of 8 B to 256 B. The next super alloc has a chunk-size of 512 KB and handles allocation sizes between 256 B and 64 KB. Chunks are managed in two distinct ways, when the allocation size does not need to track the actual number of pages used for an allocation we can use a binmap. Otherwise a chunk is used for a single allocation and the actual number of physical pages committed is stored.

---

# Binmap

The main purpose of Binmap is to quickly give you the index of a '0' bit. The implementation uses 3 levels of bit arrays. Binmap has a `findandset` function which can give you a 'free' element quickly.

```md
                           +--------------------------------+                                                                                
          32-bit, level 0  |                               0|                                                                                
                           +--------------------------------+                                                                                
                                                           \----------|                                                                 
                16-bit words                                          |                                                                 
                    +----------------+   +----------------+   +-------|--------+                                                        
            level 1 |                |   |                |   |              01|                                                        
                    +----------------+   +----------------+   +--------------||+ First bit '1' means that                               
                                                                             /\  16-bit word on level 2 is full                         
           level 2                                             /------------+  |                                                        
           +----------------+   +----------------+   +--------|-------+    +---|------------+                                           
           |                |   |                |   |0001000001111111|    |1111111111111111|                                           
           +----------------+   +----------------+   +----------------+    +----------------+                                           
```

---

# SuperAlloc

In total for the desktop application configuration there are 14 super allocators that make up the main allocator. In the implementation you can find the configuration for desktop applications.

Internally superallocator uses it's own simple (virtual memory) heap allocator and a (virtual memory) FSA allocator.

---

# Simulated Test

A test run of 60 million allocations/deallocations has shown that the bookkeeping data uses around 5 MB of memory.
The amount of waste from chunks not totally full using a naive configuration is ~8% of the actual requested memory.

---
