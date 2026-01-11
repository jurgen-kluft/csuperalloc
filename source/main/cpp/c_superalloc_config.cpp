#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "ccore/c_math.h"

#include "csuperalloc/c_superalloc_config.h"

namespace ncore
{
#define SUPERALLOC_DEBUG

    namespace nsuperalloc
    {

        /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        /// The following is a strict data-drive initialization of the bins and allocators, please know what you are doing when modifying any of this.

        static const s8 sSectionSize_64MB  = 26;  // 2^26 = 64MB
        static const s8 sSectionSize_128MB = 27;  //
        static const s8 sSectionSize_256MB = 28;  //
        static const s8 sSectionSize_512MB = 29;  //
        static const s8 sSectionSize_1GB   = 30;  //

        static const s8 sSectionSize_Min = sSectionSize_64MB;  // Minimum section size is 64MB (1 << 26)
        static const s8 sSectionSize_Max = sSectionSize_1GB;   // Maximum section size is 1GB  (1 << 30)

        static const chunkconfig_t c64KB              = {16, 0, 4, sSectionSize_64MB};
        static const chunkconfig_t c128KB             = {17, 1, 2, sSectionSize_64MB};
        static const chunkconfig_t c256KB             = {18, 2, 1, sSectionSize_64MB};
        static const chunkconfig_t c512KB             = {19, 3, 0, sSectionSize_128MB};
        static const chunkconfig_t c2MB               = {21, 4, -1, sSectionSize_256MB};
        static const chunkconfig_t c8MB               = {23, 5, -1, sSectionSize_512MB};
        static const chunkconfig_t c32MB              = {25, 6, -1, sSectionSize_512MB};
        static const chunkconfig_t c128MB             = {27, 7, -1, sSectionSize_512MB};
        static const chunkconfig_t c512MB             = {29, 8, -1, sSectionSize_1GB};
        static const chunkconfig_t c_achunkconfigs[]  = {c64KB, c128KB, c256KB, c512KB, c2MB, c8MB, c32MB, c128MB, c512MB};
        static const u32           c_num_chunkconfigs = sizeof(c_achunkconfigs) / sizeof(chunkconfig_t);

        namespace nsuperalloc_config_25p
        {
            // clang-format off
            // binconfig_t (alloc-size, chunk-config)
            static const binconfig_t c_abinconfigs[] = {
                binconfig_t(        16, c64KB),                    binconfig_t(   16, c64KB),                        // 16, 16
                binconfig_t(        16, c64KB),                    binconfig_t(   16, c64KB),                        // 16, 16
                binconfig_t(        16, c64KB),                    binconfig_t(   16, c64KB),                        // 16, 16
                binconfig_t(        16, c64KB),                    binconfig_t(   16, c64KB),                        // 16, 16
                binconfig_t(        16, c64KB),                    binconfig_t(   16, c64KB),                        // 16, 16
                binconfig_t(        16, c64KB),                    binconfig_t(   16, c64KB),                        // 16, 16
                binconfig_t(        16, c64KB),                    binconfig_t(   32, c64KB),                        // 16, 32
                binconfig_t(        32, c64KB),                    binconfig_t(   32, c64KB),                        // 32, 32
                binconfig_t(        32, c64KB),                    binconfig_t(   48, c64KB),                        // 32, 48
                binconfig_t(        48, c64KB),                    binconfig_t(   64, c64KB),                        // 48, 64
                binconfig_t(        64, c64KB),                    binconfig_t(   80, c64KB),                        // 64, 80
                binconfig_t(        96, c64KB),                    binconfig_t(   112, c64KB),                       // 96, 112
                binconfig_t(       128, c64KB),                    binconfig_t(   160, c64KB),                       // 128, 160
                binconfig_t(       192, c64KB),                    binconfig_t(   224, c64KB),                       // 192, 224
                binconfig_t(       256, c64KB),                    binconfig_t(   320, c64KB),                       // 256, 320
                binconfig_t(       384, c64KB),                    binconfig_t(   448, c64KB),                       // 384, 448
                binconfig_t(       512, c64KB),                    binconfig_t(   640, c64KB),                       // 512, 640
                binconfig_t(       768, c64KB),                    binconfig_t(   896, c64KB),                       // 768, 896
                binconfig_t(   1 * cKB, c64KB),                    binconfig_t(  1*cKB + 256, c64KB),                //   1KB, 1KB + 256
                binconfig_t(   1 * cKB + 512, c64KB),              binconfig_t(  1*cKB + 768, c64KB),                //   1KB, 1KB + 768
                binconfig_t(   2 * cKB, c64KB),                    binconfig_t(  2*cKB + 512, c64KB),                //   2KB, 2KB + 512
                binconfig_t(   3 * cKB, c64KB),                    binconfig_t(  3*cKB + 512, c64KB),                //   3KB, 3KB + 512
                binconfig_t(   4 * cKB, c64KB),                    binconfig_t(  5*cKB, c128KB),                     //   4KB, 5KB
                binconfig_t(   6 * cKB, c128KB),                   binconfig_t(  7*cKB, c128KB),                     //   6KB, 7KB
                binconfig_t(   8 * cKB, c64KB),                    binconfig_t(  10*cKB, c128KB),                    //   8KB, 10KB
                binconfig_t(  12 * cKB, c128KB),                   binconfig_t(  14*cKB, c128KB),                    //  12KB, 14KB
                binconfig_t(  16 * cKB, c64KB),                    binconfig_t(  20*cKB, c128KB),                    //  16KB, 20KB
                binconfig_t(  24 * cKB, c128KB),                   binconfig_t(  28*cKB, c128KB),                    //  24KB, 28KB
                binconfig_t(  32 * cKB, c64KB),                    binconfig_t(  40*cKB, c512KB),                    //  32KB, 40KB
                binconfig_t(  48 * cKB, c512KB),                   binconfig_t(  56*cKB, c512KB),                    //  48KB, 56KB
                binconfig_t(  64 * cKB, c512KB),                   binconfig_t(  80*cKB, c512KB),                    //  64KB, 80KB
                binconfig_t(  96 * cKB, c512KB),                   binconfig_t(  112*cKB, c512KB),                   //  96KB, 112KB
                binconfig_t( 128 * cKB, c512KB),                   binconfig_t(  160*cKB, c2MB),                     // 128KB, 160KB
                binconfig_t( 192 * cKB, c2MB),                     binconfig_t(  224*cKB, c2MB),                     // 192KB, 224KB
                binconfig_t( 256 * cKB, c2MB),                     binconfig_t(  320*cKB, c2MB),                     // 256KB, 320KB
                binconfig_t( 384 * cKB, c2MB),                     binconfig_t(  448*cKB, c2MB),                     // 384KB, 448KB
                binconfig_t( 512 * cKB, c2MB),                     binconfig_t(  640*cKB, c8MB),                     // 512KB, 640KB
                binconfig_t( 768 * cKB, c8MB),                     binconfig_t(  896*cKB, c8MB),                     // 768KB, 896KB
                binconfig_t(   1 * cMB, c8MB),                     binconfig_t( 1*cMB + 256*cKB, c8MB),              //   1MB, 1MB + 256*cKB
                binconfig_t(   1 * cMB + 512 * cKB, c8MB),         binconfig_t( 1*cMB + 768*cKB, c8MB),              //   1MB, 1MB + 768*cKB
                binconfig_t(   2 * cMB, c32MB),                    binconfig_t( 2*cMB + 512*cKB, c32MB),             //   2MB, 2MB + 512*cKB
                binconfig_t(   3 * cMB, c32MB),                    binconfig_t( 3*cMB + 512*cKB, c32MB),             //   3MB, 3MB + 512*cKB
                binconfig_t(   4 * cMB, c32MB),                    binconfig_t( 5*cMB, c32MB),                       //   4MB, 5MB
                binconfig_t(   6 * cMB, c32MB),                    binconfig_t( 7*cMB, c32MB),                       //   6MB, 7MB
                binconfig_t(   8 * cMB, c32MB),                    binconfig_t( 10*cMB, c32MB),                      //   8MB, 10MB
                binconfig_t(  12 * cMB, c32MB),                    binconfig_t( 14*cMB, c32MB),                      //  12MB, 14MB
                binconfig_t(  16 * cMB, c32MB),                    binconfig_t( 20*cMB, c32MB),                      //  16MB, 20MB
                binconfig_t(  24 * cMB, c32MB),                    binconfig_t( 28*cMB, c32MB),                      //  24MB, 28MB
                binconfig_t(  32 * cMB, c32MB),                    binconfig_t( 40*cMB, c128MB),                     //  32MB, 40MB
                binconfig_t(  48 * cMB, c128MB),                   binconfig_t( 56*cMB, c128MB),                     //  48MB, 56MB
                binconfig_t(  64 * cMB, c128MB),                  binconfig_t( 80*cMB, c128MB),                    //  64MB, 80MB
                binconfig_t(  96 * cMB, c128MB),                  binconfig_t( 112*cMB, c128MB),                   //  96MB, 112MB
                binconfig_t( 128 * cMB, c128MB),                  binconfig_t( 160*cMB, c512MB),                   // 128MB, 160MB
                binconfig_t( 192 * cMB, c512MB),                  binconfig_t( 224*cMB, c512MB),                   // 192MB, 224MB
                binconfig_t( 256 * cMB, c512MB),                  binconfig_t( 320*cMB, c512MB),                   // 256MB, 320MB
                binconfig_t( 384 * cMB, c512MB),                  binconfig_t( 448*cMB, c512MB),                   // 384MB, 448MB
                binconfig_t( 512 * cMB, c512MB),                                                                       // 512MB
            };
            static const s32        c_num_binconfigs = sizeof(c_abinconfigs) / sizeof(binconfig_t);
            // clang-format on

            // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly
            // 25% allocation waste (based on empirical data), however based on the allocation behaviour of the application, the waste can
            // be a lot more or a lot less than 25%.
            class config_25p_t : public config_t
            {
            public:
                u8 size2bin(u32 alloc_size) const override final
                {
                    const s32 w   = math::countLeadingZeros(alloc_size);
                    const u32 f   = (u32)0x80000000 >> w;
                    const u32 r   = 0xFFFFFFFF << (29 - w);
                    const u32 t   = ((f - 1) >> 2);
                    alloc_size    = (alloc_size + t) & ~t;
                    const s32 bin = (s32)((alloc_size & r) >> (29 - w)) + ((29 - w) * 4);
                    ASSERT(alloc_size <= m_abinconfigs[bin].m_alloc_size);
                    return (u8)bin;
                }
            };

            static config_25p_t s_config;
        }  // namespace nsuperalloc_config_25p

        config_t const* gConfigWindowsDesktopApp25p()
        {
            const u64 c_total_address_space         = 256 * cGB;
            const u64 c_section_address_range       = sSectionSize_Max;
            const u32 c_internal_heap_address_range = 32 * cMB;
            const u32 c_internal_heap_pre_size      = 4 * cMB;
            const u32 c_internal_fsa_address_range  = 256 * cMB;  // Note: max 256 segments
            const u32 c_internal_fsa_section_size   = 8 * cMB;    // Note: max 256 blocks (smallest block is 64KB)
            const u32 c_internal_fsa_pre_size       = 16 * cMB;

            nsuperalloc_config_25p::config_25p_t* config = &nsuperalloc_config_25p::s_config;

            config->m_total_address_size          = c_total_address_space;
            config->m_section_address_range       = c_section_address_range;
            config->m_internal_heap_address_range = c_internal_heap_address_range;
            config->m_internal_heap_pre_size      = c_internal_heap_pre_size;
            config->m_internal_fsa_address_range  = c_internal_fsa_address_range;
            config->m_internal_fsa_section_size   = c_internal_fsa_section_size;
            config->m_internal_fsa_pre_size       = c_internal_fsa_pre_size;
            config->m_section_minsize_shift       = sSectionSize_Min;
            config->m_section_maxsize_shift       = sSectionSize_Max;
            config->m_num_chunkconfigs            = c_num_chunkconfigs;
            config->m_num_binconfigs              = nsuperalloc_config_25p::c_num_binconfigs;
            config->m_achunkconfigs               = c_achunkconfigs;
            config->m_abinconfigs                 = nsuperalloc_config_25p::c_abinconfigs;

#ifdef SUPERALLOC_DEBUG
            // sanity check the configuration
            for (s16 s = 0; s < config->m_num_binconfigs; s++)
            {
                ASSERT(config->m_abinconfigs[s].m_max_alloc_count >= 1);
                u32 const          size      = config->m_abinconfigs[s].m_alloc_size;
                u8 const           bin_index = config->size2bin(size);
                binconfig_t const& bin       = config->m_abinconfigs[bin_index];
                ASSERT(size <= bin.m_alloc_size);
                ASSERT(bin.m_max_alloc_count >= 1);
                ASSERT(bin.m_max_alloc_count <= 4096);  // The binmap we use can only handle max 4096 bits
            }
#endif
            return config;
        }

        namespace nsuperalloc_config_10p
        {
            // clang-format off
            // binconfig_t (alloc-size, chunk-config)
            static const binconfig_t c_abinconfigs[] = {
                binconfig_t(   8, c64KB),                 binconfig_t(   8, c64KB),                 // 0, 1
                binconfig_t(   8, c64KB),                 binconfig_t(   8, c64KB),                 // 2, 3
                binconfig_t(   8, c64KB),                 binconfig_t(   8, c64KB),                 // 4, 5
                binconfig_t(   8, c64KB),                 binconfig_t(   8, c64KB),                 // 6, 7
                binconfig_t(   8, c64KB),                 binconfig_t(   16, c64KB),                // 8, 9
                binconfig_t(   16, c64KB),               binconfig_t(   16, c64KB),               // 10, 11
                binconfig_t(   16, c64KB),               binconfig_t(   16, c64KB),               // 12, 13
                binconfig_t(   16, c64KB),               binconfig_t(   16, c64KB),               // 14, 15
                binconfig_t(   16, c64KB),               binconfig_t(   24, c64KB),               // 16, 17
                binconfig_t(   24, c64KB),               binconfig_t(   24, c64KB),               // 18, 19
                binconfig_t(   24, c64KB),               binconfig_t(   28, c64KB),               // 20, 21
                binconfig_t(   32, c64KB),               binconfig_t(   32, c64KB),               // 22, 23
                binconfig_t(   32, c64KB),               binconfig_t(   40, c64KB),               // 24, 25
                binconfig_t(   40, c64KB),               binconfig_t(   48, c64KB),               // 26, 27
                binconfig_t(   48, c64KB),               binconfig_t(   56, c64KB),               // 28, 29
                binconfig_t(   56, c64KB),               binconfig_t(   64, c64KB),               // 30, 31
                binconfig_t(   64, c64KB),               binconfig_t(   80, c64KB),               // 32, 33
                binconfig_t(   80, c64KB),               binconfig_t(   88, c64KB),               // 34, 35
                binconfig_t(   96, c64KB),               binconfig_t(   112, c64KB),              // 36, 37
                binconfig_t(   112, c64KB),              binconfig_t(   128, c64KB),              // 38, 39
                binconfig_t(   128, c64KB),              binconfig_t(   160, c64KB),              // 40, 41
                binconfig_t(   160, c64KB),              binconfig_t(   192, c64KB),              // 42, 43
                binconfig_t(   192, c64KB),              binconfig_t(   224, c64KB),              // 44, 45
                binconfig_t(   224, c64KB),              binconfig_t(   256, c64KB),              // 46, 47
                binconfig_t(   256, c64KB),              binconfig_t(   288, c64KB),              // 48, 49
                binconfig_t(   320, c64KB),              binconfig_t(   352, c64KB),              // 50, 51
                binconfig_t(   384, c64KB),              binconfig_t(   448, c64KB),              // 52, 53
                binconfig_t(   448, c64KB),              binconfig_t(   512, c64KB),              // 54, 55
                binconfig_t(   512, c64KB),              binconfig_t(   640, c64KB),              // 56, 57
                binconfig_t(   640, c64KB),              binconfig_t(   768, c64KB),              // 58, 59
                binconfig_t(   768, c64KB),              binconfig_t(   896, c64KB),              // 60, 61
                binconfig_t(   896, c64KB),              binconfig_t(   960, c64KB),              // 62, 63
                binconfig_t(  1*cKB, c64KB),             binconfig_t(  1*cKB + 128, c64KB),       // 64, 65
                binconfig_t(  1*cKB + 256, c128KB),      binconfig_t(  1*cKB + 384, c128KB),      // 66, 67
                binconfig_t(  1*cKB + 512, c128KB),      binconfig_t(  1*cKB + 640, c128KB),      // 68, 69
                binconfig_t(  1*cKB + 768, c128KB),      binconfig_t(  1*cKB + 896, c128KB),      // 70, 71
                binconfig_t(  2*cKB, c128KB),            binconfig_t(  2*cKB + 256, c128KB),      // 72, 73
                binconfig_t(  2*cKB + 512, c128KB),      binconfig_t(  2*cKB + 768, c128KB),      // 74, 75
                binconfig_t(  3*cKB, c128KB),            binconfig_t(  3*cKB + 256, c128KB),      // 76, 77
                binconfig_t(  3*cKB + 512, c128KB),      binconfig_t(  3*cKB + 768, c128KB),      // 78, 79
                binconfig_t(  4*cKB, c128KB),            binconfig_t(  4*cKB + 512, c128KB),      // 80, 81
                binconfig_t(  5*cKB, c128KB),            binconfig_t(  5*cKB + 512, c128KB),      // 82, 83
                binconfig_t(  6*cKB, c128KB),            binconfig_t(  6*cKB + 512, c128KB),      // 84, 85
                binconfig_t(  7*cKB, c128KB),            binconfig_t(  7*cKB + 512, c128KB),      // 86, 87
                binconfig_t(  8*cKB, c128KB),            binconfig_t(  9*cKB, c128KB),            // 88, 89
                binconfig_t(  10*cKB, c128KB),           binconfig_t(  11*cKB, c128KB),           // 90, 91
                binconfig_t(  12*cKB, c128KB),           binconfig_t(  13*cKB, c128KB),           // 92, 93
                binconfig_t(  14*cKB, c128KB),           binconfig_t(  15*cKB, c128KB),           // 94, 95
                binconfig_t(  16*cKB, c128KB),           binconfig_t(  18*cKB, c128KB),           // 96, 97
                binconfig_t(  20*cKB, c128KB),           binconfig_t(  22*cKB, c128KB),           // 98, 99
                binconfig_t(  24*cKB, c128KB),          binconfig_t(  26*cKB, c128KB),          // 100, 101
                binconfig_t(  28*cKB, c128KB),          binconfig_t(  30*cKB, c128KB),          // 102, 103
                binconfig_t(  32*cKB, c128KB),          binconfig_t(  36*cKB, c512KB),          // 104, 105
                binconfig_t(  40*cKB, c512KB),          binconfig_t(  44*cKB, c512KB),          // 106, 107
                binconfig_t(  48*cKB, c512KB),          binconfig_t(  52*cKB, c512KB),          // 108, 109
                binconfig_t(  56*cKB, c512KB),          binconfig_t(  60*cKB, c512KB),          // 110, 111
                binconfig_t(  64*cKB, c512KB),          binconfig_t(  72*cKB, c512KB),          // 112, 113
                binconfig_t(  80*cKB, c512KB),          binconfig_t(  88*cKB, c512KB),          // 114, 115
                binconfig_t(  96*cKB, c512KB),          binconfig_t(  104*cKB, c512KB),         // 116, 117
                binconfig_t(  112*cKB, c512KB),         binconfig_t(  120*cKB, c512KB),         // 118, 119
                binconfig_t(  128*cKB, c512KB),         binconfig_t(  144*cKB, c512KB),         // 120, 121
                binconfig_t(  160*cKB, c2MB),           binconfig_t(  176*cKB, c2MB),           // 122, 123
                binconfig_t(  192*cKB, c2MB),           binconfig_t(  208*cKB, c2MB),           // 124, 125
                binconfig_t(  224*cKB, c2MB),           binconfig_t(  240*cKB, c2MB),           // 126, 127
                binconfig_t(  256*cKB, c2MB),           binconfig_t(  288*cKB, c2MB),           // 128, 129
                binconfig_t(  320*cKB, c2MB),           binconfig_t(  352*cKB, c2MB),           // 130, 131
                binconfig_t(  384*cKB, c2MB),           binconfig_t(  416*cKB, c2MB),           // 132, 133
                binconfig_t(  448*cKB, c2MB),           binconfig_t(  480*cKB, c2MB),           // 134, 135
                binconfig_t(  512*cKB, c2MB),           binconfig_t(  576*cKB, c8MB),           // 136, 137
                binconfig_t(  640*cKB, c8MB),           binconfig_t(  704*cKB, c8MB),           // 138, 139
                binconfig_t(  768*cKB, c8MB),           binconfig_t(  832*cKB, c8MB),           // 140, 141
                binconfig_t(  896*cKB, c8MB),           binconfig_t(  960*cKB, c8MB),           // 142, 143
                binconfig_t( 1*cMB, c8MB),              binconfig_t( 1*cMB + 128*cKB, c8MB),    // 144, 145
                binconfig_t( 1*cMB + 256*cKB, c8MB),    binconfig_t( 1*cMB + 384*cKB, c8MB),    // 146, 147
                binconfig_t( 1*cMB + 512*cKB, c8MB),    binconfig_t( 1*cMB + 640*cKB, c8MB),    // 148, 149
                binconfig_t( 1*cMB + 768*cKB, c8MB),    binconfig_t( 1*cMB + 896*cKB, c8MB),    // 150, 151
                binconfig_t( 2*cMB, c32MB),             binconfig_t( 2*cMB + 256*cKB, c32MB),   // 152, 153
                binconfig_t( 2*cMB + 512*cKB, c32MB),   binconfig_t( 2*cMB + 768*cKB, c32MB),   // 154, 155
                binconfig_t( 3*cMB, c32MB),             binconfig_t( 3*cMB + 256*cKB, c32MB),   // 156, 157
                binconfig_t( 3*cMB + 512*cKB, c32MB),   binconfig_t( 3*cMB + 768*cKB, c32MB),   // 158, 159
                binconfig_t( 4*cMB, c32MB),             binconfig_t( 4*cMB + 512*cKB, c32MB),   // 160, 161
                binconfig_t( 5*cMB, c32MB),             binconfig_t( 5*cMB + 512*cKB, c32MB),   // 162, 163
                binconfig_t( 6*cMB, c32MB),             binconfig_t( 6*cMB + 512*cKB, c32MB),   // 164, 165
                binconfig_t( 7*cMB, c32MB),             binconfig_t( 7*cMB + 512*cKB, c32MB),   // 166, 167
                binconfig_t( 8*cMB, c32MB),             binconfig_t( 9*cMB, c32MB),             // 168, 169
                binconfig_t( 10*cKB, c32MB),            binconfig_t( 11*cMB, c32MB),            // 170, 171
                binconfig_t( 12*cMB, c32MB),            binconfig_t( 13*cMB, c32MB),            // 172, 173
                binconfig_t( 14*cMB, c32MB),            binconfig_t( 15*cMB, c32MB),            // 174, 175
                binconfig_t( 16*cMB, c32MB),            binconfig_t( 18*cMB, c32MB),            // 176, 177
                binconfig_t( 20*cKB, c32MB),            binconfig_t( 22*cMB, c32MB),            // 178, 179
                binconfig_t( 24*cMB, c32MB),            binconfig_t( 26*cMB, c32MB),            // 180, 181
                binconfig_t( 28*cMB, c32MB),            binconfig_t( 30*cKB, c32MB),            // 182, 183
                binconfig_t( 32*cMB, c32MB),            binconfig_t( 36*cMB, c128MB),           // 184, 185
                binconfig_t( 40*cKB, c128MB),           binconfig_t( 44*cMB, c128MB),           // 186, 187
                binconfig_t( 48*cMB, c128MB),           binconfig_t( 52*cMB, c128MB),           // 188, 189
                binconfig_t( 56*cMB, c128MB),           binconfig_t( 60*cKB, c128MB),           // 190, 191
                binconfig_t( 64*cMB, c128MB),           binconfig_t( 72*cMB, c128MB),           // 192, 193
                binconfig_t( 80*cKB, c128MB),           binconfig_t( 88*cMB, c128MB),           // 194, 195
                binconfig_t( 96*cMB, c128MB),           binconfig_t( 104*cMB, c128MB),          // 196, 197
                binconfig_t( 112*cMB, c128MB),          binconfig_t( 120*cMB, c128MB),          // 198, 199
                binconfig_t( 128*cMB, c128MB),          binconfig_t( 144*cMB, c512MB),          // 200, 201
                binconfig_t( 160*cMB, c512MB),          binconfig_t( 176*cMB, c512MB),          // 202, 203
                binconfig_t( 192*cMB, c512MB),          binconfig_t( 208*cMB, c512MB),          // 204, 205
                binconfig_t( 224*cMB, c512MB),          binconfig_t( 240*cKB, c512MB),          // 206, 207
                binconfig_t( 256*cMB, c512MB),          binconfig_t( 288*cMB, c512MB),          // 208, 209
                binconfig_t( 320*cMB, c512MB),          binconfig_t( 352*cMB, c512MB),          // 210, 211
                binconfig_t( 384*cMB, c512MB),          binconfig_t( 416*cMB, c512MB),          // 212, 213
                binconfig_t( 448*cMB, c512MB),          binconfig_t( 480*cKB, c512MB),          // 214, 215
            };
            static const s32 c_num_binconfigs = sizeof(c_abinconfigs) / sizeof(binconfig_t);
            // clang-format on

            // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly
            // 10% allocation waste (based on empirical data), however based on the allocation behaviour of the application, the waste can
            // be a lot more or a lot less than 10%.
            class config_10p_t : public config_t
            {
            public:
                u8 size2bin(u32 alloc_size) const override final
                {
                    const s32 w   = math::countLeadingZeros(alloc_size);
                    const u32 f   = (u32)0x80000000 >> w;
                    const u32 r   = 0xFFFFFFFF << (28 - w);
                    const u32 t   = ((f - 1) >> 3);
                    alloc_size    = (alloc_size + t) & ~t;
                    const s32 bin = (s32)((alloc_size & r) >> (28 - w)) + ((28 - w) * 8);
                    ASSERT(alloc_size <= m_abinconfigs[bin].m_alloc_size);
                    return (u8)bin;
                }
            };

            static config_10p_t s_config;

        }  // namespace nsuperalloc_config_10p

        config_t const* gConfigWindowsDesktopApp10p()
        {
            const u64 c_total_address_space         = 256 * cGB;
            const u64 c_section_address_range       = sSectionSize_Max;
            const u32 c_internal_heap_address_range = 32 * cMB;
            const u32 c_internal_heap_pre_size      = 4 * cMB;
            const u32 c_internal_fsa_address_range  = 256 * cMB;
            const u32 c_internal_fsa_section_size   = 8 * cMB;
            const u32 c_internal_fsa_pre_size       = 16 * cMB;

            nsuperalloc_config_10p::config_10p_t* config = &nsuperalloc_config_10p::s_config;

            config->m_total_address_size          = c_total_address_space;
            config->m_section_address_range       = c_section_address_range;
            config->m_internal_heap_address_range = c_internal_heap_address_range;
            config->m_internal_heap_pre_size      = c_internal_heap_pre_size;
            config->m_internal_fsa_address_range  = c_internal_fsa_address_range;
            config->m_internal_fsa_section_size   = c_internal_fsa_section_size;
            config->m_internal_fsa_pre_size       = c_internal_fsa_pre_size;
            config->m_section_minsize_shift       = sSectionSize_Min;
            config->m_section_maxsize_shift       = sSectionSize_Max;
            config->m_num_chunkconfigs            = c_num_chunkconfigs;
            config->m_num_binconfigs              = nsuperalloc_config_10p::c_num_binconfigs;
            config->m_achunkconfigs               = c_achunkconfigs;
            config->m_abinconfigs                 = nsuperalloc_config_10p::c_abinconfigs;

#ifdef SUPERALLOC_DEBUG
            // sanity check the configuration
            for (s16 s = 0; s < config->m_num_binconfigs; s++)
            {
                u32 const          size      = config->m_abinconfigs[s].m_alloc_size;
                u8 const           bin_index = config->size2bin(size);
                binconfig_t const& bin       = config->m_abinconfigs[bin_index];
                ASSERT(size <= bin.m_alloc_size);
                ASSERT(bin.m_max_alloc_count >= 1);     // Zero is not allowed
                ASSERT(bin.m_max_alloc_count <= 4096);  // The binmap we use can only handle max 4096 bits
            }
#endif
            return config;
        }
    }  // namespace nsuperalloc
}  // namespace ncore
