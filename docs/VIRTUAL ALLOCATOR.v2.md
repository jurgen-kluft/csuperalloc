# Virtual Allocator v2.0

Each allocation size has a region type to obtain a chunk from, that is all we need, this information can be stored in a simple array.

We can create a 'segment' allocator which can own the address space and give us a power-of-two region upon request.
- min-size = 4 MiB, shift = 22
- max-size = 1 GiB, shift = 30

Allocations larger than a certain size, like 128 KiB, are handled differently, and they are handled directly by a region.

The allocator has 2 internal heaps, one is an arena used for allocations that happen during initialization, the other is used for metadata allocations during runtime.
The one used during runtime is a custom FSA (fixed-size allocations) that allocates items of a fixed (small) size, the runtime allocations
are infrequent and of the following sizes:
- binmap, bin1: <= 512 bytes
- region; array, <= 256 bytes