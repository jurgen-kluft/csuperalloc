package main

import "fmt"

// Compute Bins
// This will generate an array of bins according to some parameters.
// - page size: the size of the page in bytes.
// - list of chunk sizes that can be used, a chunk size is always a multiple of the page size.

// Objective
// It will compute the best chunk size, reducing allocation waste.
// Also compute the maximum number of allocations of each bin.

type chunkconfig_t struct {
	Index int16 // The index of this chunk config in the chunk config array
	Size  int32 // The shift of the chunk size (e.g. 12 for 4KB)
}

type binconfig_t struct {
	Index       int16         // The index of this bin
	AllocSize   int32         // The size of the allocation that this bin is managing
	AllocCount  int32         // The maximum number of allocations that can be made from a single chunk
	ChunkConfig chunkconfig_t // The index of the chunk size that this bin requires

	PercentageOfWaste float32 // The percentage of waste per allocation
}

// Example output:
/*
   chunk sizes: 16KB, 32KB, 48KB, 64KB, 80KB, ...
   static const binconfig_t c_abinconfigs[] = {
       {0,  8,  cKB},               {1,     8, c64KB},
       {2,  8,  c64KB},             {3,     8, c64KB},
       {4,  8,  c64KB},             {5,     8, c64KB},
       {6,  8,  c64KB},             {7,     8, c64KB},
       {8,  8,  c64KB},             {9,    16, c64KB},
       {10, 16, c64KB},             {11,   16, c64KB},
       {12, 16, c64KB},             {13,   24, c64KB},
       {14, 24, c64KB},             {15,   32, c64KB},
       {16, 32, c64KB},             {17,   40, c64KB},
       ...
       ...
       ...
   };
*/

var cKB int32 = 1024
var cMB int32 = 1024 * 1024

var c_page_size = 16 * cKB

var c_chunk_sizes = []chunkconfig_t{
	{0, 16 * cKB},
	{1, 32 * cKB},
	{2, 48 * cKB},
	{3, 64 * cKB},
	{4, 80 * cKB},
	{5, 96 * cKB},
	{6, 112 * cKB},
	{7, 128 * cKB},
	{8, 144 * cKB},
	{9, 160 * cKB},
	{10, 176 * cKB},
	{11, 192 * cKB},
	{12, 208 * cKB},
	{13, 224 * cKB},
	{14, 256 * cKB},
	{15, 384 * cKB},
	{16, 512 * cKB},
	{17, 768 * cKB},
	{18, 1 * cMB},
	{19, 2 * cMB},
	{20, 4 * cMB},
	{21, 8 * cMB},
	{22, 32 * cMB},
	{23, 64 * cMB},
	{24, 256 * cMB},
	{25, 512 * cMB},
}

var c_alloc_sizes = []int32{
	8,
	12,
	16,
	20,
	24,
	28,
	32,
	36,
	40,
	44,
	48,
	56,
	64,
	80,
	88,
	96,
	112,
	128,
	160,
	192,
	224,
	256,
	288,
	320,
	352,
	384,
	448,
	512,
	640,
	768,
	896,
	960,
	1 * cKB,
	1*cKB + 128,
	1*cKB + 256,
	1*cKB + 384,
	1*cKB + 512,
	1*cKB + 640,
	1*cKB + 768,
	1*cKB + 896,
	2 * cKB,
	2*cKB + 256,
	2*cKB + 512,
	2*cKB + 768,
	3 * cKB,
	3*cKB + 256,
	3*cKB + 512,
	3*cKB + 768,
	4 * cKB,
	4*cKB + 512,
	5 * cKB,
	5*cKB + 512,
	6 * cKB,
	6*cKB + 512,
	7 * cKB,
	7*cKB + 512,
	8 * cKB,
	9 * cKB,
	10 * cKB,
	11 * cKB,
	12 * cKB,
	13 * cKB,
	14 * cKB,
	15 * cKB,
	16 * cKB,
	18 * cKB,
	20 * cKB,
	22 * cKB,
	24 * cKB,
	26 * cKB,
	28 * cKB,
	30 * cKB,
	32 * cKB,
	36 * cKB,
	40 * cKB,
	44 * cKB,
	48 * cKB,
	52 * cKB,
	56 * cKB,
	60 * cKB,
	64 * cKB,
	72 * cKB,
	80 * cKB,
	88 * cKB,
	96 * cKB,
	104 * cKB,
	112 * cKB,
	120 * cKB,
	128 * cKB,
	144 * cKB,
	160 * cKB,
	176 * cKB,
	192 * cKB,
	208 * cKB,
	224 * cKB,
	240 * cKB,
	256 * cKB,
	288 * cKB,
	320 * cKB,
	352 * cKB,
	384 * cKB,
	416 * cKB,
	448 * cKB,
	480 * cKB,
	512 * cKB,
	576 * cKB,
	640 * cKB,
	704 * cKB,
	768 * cKB,
	832 * cKB,
	896 * cKB,
	960 * cKB,
	1 * cMB,
	1*cMB + 128*cKB,
	1*cMB + 256*cKB,
	1*cMB + 384*cKB,
	1*cMB + 512*cKB,
	1*cMB + 640*cKB,
	1*cMB + 768*cKB,
	1*cMB + 896*cKB,
	2 * cMB,
	2*cMB + 256*cKB,
	2*cMB + 512*cKB,
	2*cMB + 768*cKB,
	3 * cMB,
	3*cMB + 256*cKB,
	3*cMB + 512*cKB,
	3*cMB + 768*cKB,
	4 * cMB,
	4*cMB + 512*cKB,
	5 * cMB,
	5*cMB + 512*cKB,
	6 * cMB,
	6*cMB + 512*cKB,
	7 * cMB,
	7*cMB + 512*cKB,
	8 * cMB,
	9 * cMB,
	11 * cMB,
	12 * cMB,
	13 * cMB,
	14 * cMB,
	15 * cMB,
	16 * cMB,
	18 * cMB,
	22 * cMB,
	24 * cMB,
	26 * cMB,
	28 * cMB,
	32 * cMB,
	36 * cMB,
	44 * cMB,
	48 * cMB,
	52 * cMB,
	56 * cMB,
	64 * cMB,
	72 * cMB,
	88 * cMB,
	96 * cMB,
	104 * cMB,
	112 * cMB,
	120 * cMB,
	128 * cMB,
	144 * cMB,
	160 * cMB,
	176 * cMB,
	192 * cMB,
	208 * cMB,
	224 * cMB,
	256 * cMB,
	288 * cMB,
	320 * cMB,
	352 * cMB,
	384 * cMB,
	416 * cMB,
	448 * cMB,
}

func compute_best_chunk_size(page_size int32, chunk_config []chunkconfig_t, alloc_size int32, acceptable_waste_percentage float32) binconfig_t {

	var bin binconfig_t

	// anything greater or equal to 8 * cMB will take the chunk size that is just above the alloc size
	if alloc_size >= 128*cKB {
		for _, chunk := range chunk_config {
			if chunk.Size >= alloc_size {
				bin.AllocSize = alloc_size
				bin.AllocCount = 1
				bin.ChunkConfig = chunk
				return bin
			}
		}
	} else {

		for _, chunk := range chunk_config {
			if alloc_size > chunk.Size {
				continue
			}
			if alloc_size > 8*chunk.Size {
				continue
			}

			// waste per allocation, example:
			// chunk_size = 16KB, alloc_size = 24
			// allocation count = (16KB / 24) = 682
			// waste per allocation = (16384.0 - (682.0 * 24.0)) / 682.0 = 0.0234
			// anything under 0.1 is immediately considered

			size_of_chunk := chunk.Size
			for size_of_chunk >= page_size && size_of_chunk >= alloc_size {
				var count int32 = size_of_chunk / alloc_size
				var waste_bytes_per_allocation float32 = float32(size_of_chunk-(count*alloc_size)) / float32(count)
				var waste_percent float32 = (waste_bytes_per_allocation / float32(alloc_size)) * 100
				if waste_percent < acceptable_waste_percentage {
					bin.AllocSize = alloc_size
					bin.AllocCount = count
					bin.ChunkConfig = chunk
					bin.PercentageOfWaste = waste_percent
					return bin
				}
				size_of_chunk -= page_size
			}
		}
	}

	return bin
}

func compute_bins(page_size int32, chunk_config []chunkconfig_t, alloc_sizes []int32, acceptable_waste_percentage float32) []binconfig_t {
	var bins []binconfig_t

	for _, alloc_size := range alloc_sizes {
		bin := compute_best_chunk_size(page_size, chunk_config, alloc_size, acceptable_waste_percentage)
		bin.Index = int16(len(bins))
		bins = append(bins, bin)
	}

	return bins
}

func chunk_size_to_string(chunk_size int32) string {
	// chunk size is always a power of 2
	if chunk_size < 1*cMB {
		kb := chunk_size / cKB
		return fmt.Sprintf("c%vKB", kb)
	} else {
		mb := chunk_size / cMB
		return fmt.Sprintf("c%vMB", mb)
	}
}

func main() {
	acceptable_waste_percentage := float32(10)
	bins := compute_bins(c_page_size, c_chunk_sizes, c_alloc_sizes, acceptable_waste_percentage)

	// Output in the example format
	println("static const binconfig_t c_abinconfigs[] = {")
	for _, bin := range bins {
		println("    {", bin.Index, ", ", bin.AllocSize, ", ", chunk_size_to_string(c_chunk_sizes[bin.ChunkConfig.Index].Size), ", ", bin.AllocCount, "},  // ", fmt.Sprintf("%0.2f", bin.PercentageOfWaste), "%")
	}
	println("};")
}
