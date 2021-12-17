using System;
using System.Collections.Generic;

namespace SuperAlloc
{
    public class Program
    {
        public enum EWasteTarget : int
		{
            PERCENT_10 = 8,
            PERCENT_25 = 4,
		};

        public static int AllocSizeToBin(UInt64 size, EWasteTarget target)
        {
            if (target == EWasteTarget.PERCENT_10)
            {
                int w = CountLeadingZeros(size);
                UInt64 f = (UInt64)0x8000000000000000 >> w;
                UInt64 r = (UInt64)0xFFFFFFFFFFFFFFFF >> (60 - w);
                UInt64 t = ((f - 1) >> 3);
                size = (size + t) & ~t;
                int i = (int)((size & r) >> (60 - w)) + ((60 - w) * 8);
                return i;
            }
			else if (target == EWasteTarget.PERCENT_25)
			{
                int w = CountLeadingZeros(size);
                UInt64 f = (UInt64)0x8000000000000000 >> w;
                UInt64 r = (UInt64)0xFFFFFFFFFFFFFFFF >> (61 - w);
                UInt64 t = ((f - 1) >> 2);
                size = (size + t) & ~t;
                int i = (int)((size & r) >> (61 - w)) + ((61 - w) * 4);
                return i;
            }
            return -1;
        }

        public class BinMapConfig
        {
            public UInt64 Count { get; set; }
            public UInt64 L1Len { get; set; }
            public UInt64 L2Len { get; set; }
        }

        public class SuperBin_t
		{
            public SuperBin_t(UInt64 size, int index)
			{
                Size = (UInt32)size;
                BinIndex = index;
                NumPages = 1;
			}
            public UInt32 Size { get; set; }
            public int BinIndex { get; set; }
            public int AllocIndex { get; set; }
            public UInt32 NumPages { get; set; }
            public UInt32 Waste { get; set; }
            public UInt32 AllocCount { get; set; }
            public UInt32 BmL1Len { get; set; }
            public UInt32 BmL2Len { get; set; }
		}
        public class SuperAlloc_t
        {
            public List<SuperBin_t> AllocSizes { get; set; } = new List<SuperBin_t>();
            public UInt64 ChunkSize { get; set; } = 0;
            public UInt64 ChunkCount { get; set; } = 0;
        };

        public static BinMapConfig CalcBinMap(UInt64 allocCount, UInt64 chunksize)
        {
            BinMapConfig bm = new BinMapConfig();
            bm.Count = (uint)(allocCount);
            if (bm.Count <= 32)
            {
                bm.L1Len = 0;
                bm.L2Len = 0;
            }
            else
            {
                int l2_len = (int)CeilPo2((uint)((allocCount + 15) / 16));
                int l1_len = (int)((l2_len + 15) / 16);
                l1_len = Math.Max(l1_len, 2);
                bm.L1Len = (uint)l1_len;
                bm.L2Len = (uint)l2_len;
            }

            bm.L1Len = CeilPo2(bm.L1Len);
            bm.L2Len = CeilPo2(bm.L2Len);
            return bm;
        }

        public static void Main()
        {
            try
            {
                EWasteTarget wt = EWasteTarget.PERCENT_10;

                // This is the place to override certain sizes and map them to a higher size
                Dictionary<UInt32, UInt32> allocSizeRemap = new Dictionary<uint, uint>();
                // remap size     from -> to
                if (wt == EWasteTarget.PERCENT_10)
                {
                    allocSizeRemap.Add(9, 12);
                    allocSizeRemap.Add(10, 12);
                    allocSizeRemap.Add(11, 12);
                    allocSizeRemap.Add(13, 16);
                    allocSizeRemap.Add(14, 16);
                    allocSizeRemap.Add(15, 16);
                    allocSizeRemap.Add(18, 20);
                    allocSizeRemap.Add(22, 24);
                    allocSizeRemap.Add(26, 28);
                    allocSizeRemap.Add(30, 32);
                }
                else if (wt == EWasteTarget.PERCENT_25)
				{
                    allocSizeRemap.Add(10, 12);
                    allocSizeRemap.Add(14, 16);
				}
				List<SuperBin_t> AllocSizes = new List<SuperBin_t>();

                UInt32 maxAllocSize = (UInt32)MB(256);
                for (UInt32 b = 8; b <= maxAllocSize; )
				{
                    UInt32 d = b / (uint)wt;
                    UInt32 s = b;
                    while (s < (b<<1))
                    {
                        int bin = AllocSizeToBin(s, wt);
						//Console.WriteLine("AllocSize: {0}, Bin: {1}", s, bin);
						SuperBin_t sbin = new SuperBin_t(s, bin);
						while (AllocSizes.Count <= bin)
                        {
							AllocSizes.Add(sbin);
                        }
                        s += d;
                    }
                    b = s;
                }

                // Remap certain sizes to another (higher) bin
                foreach(var s2s in allocSizeRemap)
				{
                    int fbin = AllocSizeToBin(s2s.Key, wt);
                    int tbin = AllocSizeToBin(s2s.Value, wt);
                    AllocSizes[fbin].BinIndex = tbin;
				}                    

                // Go over all the power-of-2 chunk sizes and determine which allocation sizes to add
                // to each SuperAlloc.
                UInt32 pageSize = (UInt32)KB(64);
                HashSet<UInt32> allocSizesToDo = new HashSet<UInt32>();
				foreach (SuperBin_t allocSize in AllocSizes)
				{
                    allocSizesToDo.Add(allocSize.Size);
				}
				List<SuperAlloc_t> Allocators = new List<SuperAlloc_t>();
				for (UInt64 chunkSize = KB(64); chunkSize <= MB(512); chunkSize *= 2)
                {
                    SuperAlloc_t allocator = new SuperAlloc_t();
                    allocator.ChunkSize = chunkSize;
                    foreach (SuperBin_t allocSize in AllocSizes)
                    {
                        if (!allocSizesToDo.Contains(allocSize.Size))
                            continue;
                        if (allocSize.Size > chunkSize)
                            continue;
                        //if (allocSize > pageSize)
                        //{
                        //   allocator.AllocSizes.Add(allocSize);
                        //  allocSizesToDo.Remove(allocSize);
                        // continue;
                        //}
                        // Figure out if this size can be part of this Chunk Size
                        // Go down in chunksize until page-size to try and fit the allocation size
                        bool addToAllocator = false;
                        UInt32 numPages = 1;
                        UInt32 lowestWaste = pageSize;
                        for (UInt64 cs = chunkSize; cs >= pageSize; cs -= pageSize)
                        {
                            UInt32 chunkWaste = (UInt32)(cs % allocSize.Size);
                            if ((chunkWaste <= (UInt32)(cs / 100)) && chunkWaste < lowestWaste)
                            {
                                numPages = (UInt32)(cs / pageSize);
                                lowestWaste = chunkWaste;
                                addToAllocator = true;
                                break;
                            }
                        }
                        if (addToAllocator)
						{
                            allocSizesToDo.Remove(allocSize.Size);
                            allocSize.AllocCount = (pageSize * numPages) / allocSize.Size;
                            allocSize.NumPages = numPages;
                            allocSize.Waste = lowestWaste;
                            allocSize.AllocIndex = (int)Allocators.Count;
							allocator.AllocSizes.Add(allocSize);
						}
					}
                    Allocators.Add(allocator);
                }

                UInt64 totalChunkCount = 0;
                int allocatorIndex = 0;
                foreach (SuperAlloc_t am in Allocators)
                {
                    foreach (SuperBin_t allocSize in am.AllocSizes)
                    { 
                        UInt64 allocCountPerChunk = am.ChunkSize / allocSize.Size;
                        UInt64 chunkSize = am.ChunkSize;
                        int bin = AllocSizeToBin(allocSize.Size, wt);

                        Console.Write("{0}:", allocatorIndex);
                        Console.Write("{0} AllocSize:{1}, AllocCount:{2}, ChunkSize:{3}, UsedPagesPerChunk:{4}", bin, allocSize.Size.ToByteSize(), allocCountPerChunk, chunkSize.ToByteSize(), allocSize.NumPages);
                        
                        if (allocSize.AllocCount > 1)
                        {
                            BinMapConfig bm = CalcBinMap(allocCountPerChunk, am.ChunkSize);
                            allocSize.BmL1Len = (UInt32)bm.L1Len;
                            allocSize.BmL2Len = (UInt32)bm.L2Len;
                            Console.Write(", BinMap({0},{1}):{2}", bm.L1Len, bm.L2Len, 4 + 2 * (bm.L1Len + bm.L2Len));
                        }
                        Console.WriteLine();
                    }

                    totalChunkCount += (UInt64)am.ChunkCount;
                    allocatorIndex += 1;
                }
                Console.WriteLine();

                // Generate C++ Code
                Console.WriteLine("static const s32        c_num_bins = {0};", AllocSizes.Count);
                Console.WriteLine("static const superbin_t c_asbins[c_num_bins] = {");
                foreach(SuperBin_t bin in AllocSizes)
				{
                    int MB = (int)(bin.Size >> 20) & 0x3FF;
                    int KB = (int)(bin.Size >> 10) & 0x3FF;
                    int B = (int)(bin.Size & 0x3FF);
                    int BinIndex = bin.BinIndex;
                    int AllocatorIndex = bin.AllocIndex;
                    int UseBinMap = (bin.AllocCount > 1) ? 1 : 0;
                    int AllocationCount = (int)bin.AllocCount;
                    int BinMapL1Len = (int)bin.BmL1Len;
                    int BinMapL2Len = (int)bin.BmL2Len;
                    Console.WriteLine("superbin_t({0},{1},{2},{3},{4},{5},{6},{7},{8}){9}", MB, KB, B, BinIndex, AllocatorIndex, UseBinMap, AllocationCount, BinMapL1Len, BinMapL2Len, ",");
				}
                Console.WriteLine("};");

                Console.WriteLine("static const s32    c_num_allocators = {0};", Allocators.Count);
				Console.WriteLine("static superalloc_t c_allocators[c_num_allocators] = {");
				foreach (SuperAlloc_t am in Allocators)
                {
                    Console.WriteLine("    superalloc_t({0}),", am.ChunkSize); 
                }
                Console.WriteLine("};");
            }
            catch (Exception e)
            {
                Console.WriteLine("Exception: {0}", e);
            }
            Console.WriteLine("Done...");
        }

        // Targetting 10% allocation waste, this is hard for a specific range of allocation sizes
        // that are close to the page-size of 64 KB. For this we would like to re-route to a region
        // that can deal with 4 KB or 16 KB pages.
        // For example, sizes between 64 KB and 128 KB like 80 KB is automatically wasting 48 KB at
        // the tail. We can reduce this only by going for a page-size of 4 KB / 16 KB.
        // 

        ///  ----------------------------------------------------------------------------------------------------------
        ///  ----------------------------------------------------------------------------------------------------------
        ///  ----------------------------------------------------------------------------------------------------------
        ///                                               UTILITY FUNCTIONS
        ///  ----------------------------------------------------------------------------------------------------------
        ///  ----------------------------------------------------------------------------------------------------------
        ///  ----------------------------------------------------------------------------------------------------------


        static UInt64 KB(int mb) { return (UInt64)mb * 1024; }
        static UInt64 MB(int mb) { return (UInt64)mb * 1024 * 1024; }
        static UInt64 GB(int mb) { return (UInt64)mb * 1024 * 1024 * 1024; }

        public static UInt64 CeilPo2(UInt64 v)
        {
            int w = CountLeadingZeros(v);
            UInt64 l = (UInt64)0x8000000000000000 >> w;
            if (l == v) return v;
            return l << 1;
        }

        public static UInt64 FloorPo2(UInt64 v)
        {
            int w = CountLeadingZeros(v);
            UInt64 l = (UInt64)0x8000000000000000 >> w;
            if (l == v) return v;
            return l;
        }

        public static int CountLeadingZeros(UInt64 integer)
        {
            if (integer == 0)
                return 64;

            int count = 0;
            if ((integer & 0xFFFFFFFF00000000UL) == 0)
            {
                count += 32;
                integer <<= 32;
            }
            if ((integer & 0xFFFF000000000000UL) == 0)
            {
                count += 16;
                integer <<= 16;
            }
            if ((integer & 0xFF00000000000000UL) == 0)
            {
                count += 8;
                integer <<= 8;
            }
            if ((integer & 0xF000000000000000UL) == 0)
            {
                count += 4;
                integer <<= 4;
            }
            if ((integer & 0xC000000000000000UL) == 0)
            {
                count += 2;
                integer <<= 2;
            }
            if ((integer & 0x8000000000000000UL) == 0)
            {
                count += 1;
                integer <<= 1;
            }
            if ((integer & 0x8000000000000000UL) == 0)
            {
                count += 1;
            }
            return count;
        }

        public static int CountTrailingZeros(UInt64 integer)
        {
            int count = 0;
            if ((integer & 0xFFFFFFFF) == 0)
            {
                count += 32;
                integer >>= 32;
            }
            if ((integer & 0x0000FFFF) == 0)
            {
                count += 16;
                integer >>= 16;
            }
            if ((integer & 0x000000FF) == 0)
            {
                count += 8;
                integer >>= 8;
            }
            if ((integer & 0x0000000F) == 0)
            {
                count += 4;
                integer >>= 4;
            }
            if ((integer & 0x00000003) == 0)
            {
                count += 2;
                integer >>= 2;
            }
            if ((integer & 0x00000001) == 0)
            {
                count += 1;
                integer >>= 1;
            }
            if ((integer & 0x00000001) == 1)
            {
                return count;
            }
            return 0;
        }

        public static bool IsPowerOf2(UInt64 v)
        {
            return (v & (v - 1)) == 0;
        }

        public static UInt64 AlignTo(UInt64 v, UInt64 a)
        {
            return (v + (a - 1)) & ~((UInt64)a - 1);
        }
        public static UInt64 AlignTo8(UInt64 v)
        {
            return AlignTo(v, 8);
        }
        public static UInt64 AlignTo16(UInt64 v)
        {
            return AlignTo(v, 16);
        }
        public static UInt64 AlignTo32(UInt64 v)
        {
            return AlignTo(v, 32);
        }
    }

    public static class IntExtensions
    {
        public static string ToByteSize(this int size)
        {
            return String.Format(new FileSizeFormatProvider(), "{0:fs}", size);
        }

        public static string ToByteSize(this Int64 size)
        {
            return String.Format(new FileSizeFormatProvider(), "{0:fs}", size);
        }
        public static string ToByteSize(this UInt32 size)
        {
            return String.Format(new FileSizeFormatProvider(), "{0:fs}", size);
        }

        public static string ToByteSize(this UInt64 size)
        {
            return String.Format(new FileSizeFormatProvider(), "{0:fs}", size);
        }

        public struct FileSize : IFormattable
        {
            private readonly ulong _value;

            private const int DEFAULT_PRECISION = 2;

            private readonly static IList<string> Units = new List<string>() { " B", " KB", " MB", " GB", " TB" };

            public FileSize(ulong value)
            {
                _value = value;
            }

            public static explicit operator FileSize(ulong value)
            {
                return new FileSize(value);
            }

            override public string ToString()
            {
                return ToString(null, null);
            }

            public string ToString(string format)
            {
                return ToString(format, null);
            }

            public string ToString(string format, IFormatProvider formatProvider)
            {
                int precision;

                if (String.IsNullOrEmpty(format))
                    return ToString(DEFAULT_PRECISION);
                else if (int.TryParse(format, out precision))
                    return ToString(precision);
                else
                    return _value.ToString(format, formatProvider);
            }

            /// <summary>
            /// Formats the FileSize using the given number of decimals.
            /// </summary>
            public string ToString(int precision)
            {
                double pow = Math.Floor((_value > 0 ? Math.Log(_value) : 0) / Math.Log(1024));
                pow = Math.Min(pow, Units.Count - 1);
                double value = (double)_value / Math.Pow(1024, pow);
                string str = value.ToString(pow == 0 ? "F0" : "F" + precision.ToString());
                if (str.EndsWith(".00"))
                    str = str.Substring(0, str.Length - 3);
                return str + Units[(int)pow];

            }
        }

        public class FileSizeFormatProvider : IFormatProvider, ICustomFormatter
        {
            public object GetFormat(Type formatType)
            {
                if (formatType == typeof(ICustomFormatter)) return this;
                return null;
            }

            /// <summary>
            /// Usage Examples:
            ///		Console2.WriteLine(String.Format(new FileSizeFormatProvider(), "File size: {0:fs}", 100));
            /// </summary>

            private const string fileSizeFormat = "fs";
            private const Decimal OneKiloByte = 1024M;
            private const Decimal OneMegaByte = OneKiloByte * 1024M;
            private const Decimal OneGigaByte = OneMegaByte * 1024M;

            public string Format(string format, object arg, IFormatProvider formatProvider)
            {
                if (format == null || !format.StartsWith(fileSizeFormat))
                {
                    return defaultFormat(format, arg, formatProvider);
                }

                if (arg is string)
                {
                    return defaultFormat(format, arg, formatProvider);
                }

                Decimal size;

                try
                {
                    size = Convert.ToDecimal(arg);
                }
                catch (InvalidCastException)
                {
                    return defaultFormat(format, arg, formatProvider);
                }

                string suffix;
                if (size >= OneGigaByte)
                {
                    size /= OneGigaByte;
                    suffix = " GB";
                }
                else if (size >= OneMegaByte)
                {
                    size /= OneMegaByte;
                    suffix = " MB";
                }
                else if (size >= OneKiloByte)
                {
                    size /= OneKiloByte;
                    suffix = " kB";
                }
                else
                {
                    suffix = " B";
                }

                string precision = format.Substring(2);
                if (String.IsNullOrEmpty(precision)) precision = "2";
                if (size == Decimal.Floor(size))
                    precision = "0";
                return String.Format("{0:N" + precision + "}{1}", size, suffix);
            }

            private static string defaultFormat(string format, object arg, IFormatProvider formatProvider)
            {
                IFormattable formattableArg = arg as IFormattable;
                if (formattableArg != null)
                {
                    return formattableArg.ToString(format, formatProvider);
                }
                return arg.ToString();
            }

        }
    }
}
