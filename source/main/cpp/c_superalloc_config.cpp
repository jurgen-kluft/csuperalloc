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

        static const s8 sSectionSize_Min = sSectionSize_64MB;  // Minimum section size is 64MB
        static const s8 sSectionSize_Max = sSectionSize_1GB;   // Maximum section size is 1GB

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
            // binconfig_t(bin-index, alloc-size, chunk-config)
            static const binconfig_t c_abinconfigs[] = {
                {12,        16, c64KB},                    {12,   16, c64KB},                        // 16, 16
                {12,        16, c64KB},                    {12,   16, c64KB},                        // 16, 16
                {12,        16, c64KB},                    {12,   16, c64KB},                        // 16, 16
                {12,        16, c64KB},                    {12,   16, c64KB},                        // 16, 16
                {12,        16, c64KB},                    {12,   16, c64KB},                        // 16, 16
                {12,        16, c64KB},                    {12,   16, c64KB},                        // 16, 16
                {12,        16, c64KB},                    {16,   32, c64KB},                        // 16, 32
                {16,        32, c64KB},                    {16,   32, c64KB},                        // 32, 32
                {16,        32, c64KB},                    {18,   48, c64KB},                        // 32, 48
                {18,        48, c64KB},                    {20,   64, c64KB},                        // 48, 64
                {20,        64, c64KB},                    {21,   80, c64KB},                        // 64, 80
                {22,        96, c64KB},                    {23,   112, c64KB},                       // 96, 112
                {24,       128, c64KB},                    {25,   160, c64KB},                       // 128, 160
                {26,       192, c64KB},                    {27,   224, c64KB},                       // 192, 224
                {28,       256, c64KB},                    {29,   320, c64KB},                       // 256, 320
                {30,       384, c64KB},                    {31,   448, c64KB},                       // 384, 448
                {32,       512, c64KB},                    {33,   640, c64KB},                       // 512, 640
                {34,       768, c64KB},                    {35,   896, c64KB},                       // 768, 896
                {36,   1 * cKB, c64KB},                    {37,  1*cKB + 256, c64KB},                //   1KB, 1KB + 256
                {38,   1 * cKB + 512, c64KB},              {39,  1*cKB + 768, c64KB},                //   1KB, 1KB + 768
                {40,   2 * cKB, c64KB},                    {41,  2*cKB + 512, c64KB},                //   2KB, 2KB + 512
                {42,   3 * cKB, c64KB},                    {43,  3*cKB + 512, c64KB},                //   3KB, 3KB + 512
                {44,   4 * cKB, c64KB},                    {45,  5*cKB, c128KB},                     //   4KB, 5KB
                {46,   6 * cKB, c128KB},                   {47,  7*cKB, c128KB},                     //   6KB, 7KB
                {48,   8 * cKB, c64KB},                    {49,  10*cKB, c128KB},                    //   8KB, 10KB
                {50,  12 * cKB, c128KB},                   {51,  14*cKB, c128KB},                    //  12KB, 14KB
                {52,  16 * cKB, c64KB},                    {53,  20*cKB, c128KB},                    //  16KB, 20KB
                {54,  24 * cKB, c128KB},                   {55,  28*cKB, c128KB},                    //  24KB, 28KB
                {56,  32 * cKB, c64KB},                    {57,  40*cKB, c512KB},                    //  32KB, 40KB
                {58,  48 * cKB, c512KB},                   {59,  56*cKB, c512KB},                    //  48KB, 56KB
                {60,  64 * cKB, c512KB},                   {61,  80*cKB, c512KB},                    //  64KB, 80KB
                {62,  96 * cKB, c512KB},                   {63,  112*cKB, c512KB},                   //  96KB, 112KB
                {64, 128 * cKB, c512KB},                   {65,  160*cKB, c2MB},                     // 128KB, 160KB
                {66, 192 * cKB, c2MB},                     {67,  224*cKB, c2MB},                     // 192KB, 224KB
                {68, 256 * cKB, c2MB},                     {69,  320*cKB, c2MB},                     // 256KB, 320KB
                {70, 384 * cKB, c2MB},                     {71,  448*cKB, c2MB},                     // 384KB, 448KB
                {72, 512 * cKB, c2MB},                     {73,  640*cKB, c8MB},                     // 512KB, 640KB
                {74, 768 * cKB, c8MB},                     {75,  896*cKB, c8MB},                     // 768KB, 896KB
                {76,   1 * cMB, c8MB},                     {77, 1*cMB + 256*cKB, c8MB},              //   1MB, 1MB + 256*cKB
                {78,   1 * cMB + 512 * cKB, c8MB},         {79, 1*cMB + 768*cKB, c8MB},              //   1MB, 1MB + 768*cKB
                {80,   2 * cMB, c32MB},                    {81, 2*cMB + 512*cKB, c32MB},             //   2MB, 2MB + 512*cKB
                {82,   3 * cMB, c32MB},                    {83, 3*cMB + 512*cKB, c32MB},             //   3MB, 3MB + 512*cKB
                {84,   4 * cMB, c32MB},                    {85, 5*cMB, c32MB},                       //   4MB, 5MB
                {86,   6 * cMB, c32MB},                    {87, 7*cMB, c32MB},                       //   6MB, 7MB
                {88,   8 * cMB, c32MB},                    {89, 10*cMB, c32MB},                      //   8MB, 10MB
                {90,  12 * cMB, c32MB},                    {91, 14*cMB, c32MB},                      //  12MB, 14MB
                {92,  16 * cMB, c32MB},                    {93, 20*cMB, c32MB},                      //  16MB, 20MB
                {94,  24 * cMB, c32MB},                    {95, 28*cMB, c32MB},                      //  24MB, 28MB
                {96,  32 * cMB, c32MB},                    {97, 40*cMB, c128MB},                     //  32MB, 40MB
                {98,  48 * cMB, c128MB},                   {99, 56*cMB, c128MB},                     //  48MB, 56MB
                {100,  64 * cMB, c128MB},                  {101, 80*cMB, c128MB},                    //  64MB, 80MB
                {102,  96 * cMB, c128MB},                  {103, 112*cMB, c128MB},                   //  96MB, 112MB
                {104, 128 * cMB, c128MB},                  {105, 160*cMB, c512MB},                   // 128MB, 160MB
                {106, 192 * cMB, c512MB},                  {107, 224*cMB, c512MB},                   // 192MB, 224MB
                {108, 256 * cMB, c512MB},                  {109, 320*cMB, c512MB},                   // 256MB, 320MB
                {110, 384 * cMB, c512MB},                  {112, 448*cMB, c512MB},                   // 384MB, 448MB
                {112, 512 * cMB, c512MB},                                                            // 512MB
            };
            static const s32        c_num_binconfigs = sizeof(c_abinconfigs) / sizeof(binconfig_t);
            // clang-format on

            // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly
            // 25% allocation waste (based on empirical data), however based on the allocation behaviour of the application, the waste can
            // be a lot more or a lot less than 25%.
            class config_25p_t : public config_t
            {
            public:
                binconfig_t const& size2bin(u32 alloc_size) const override final
                {
                    const s32 w   = math::countLeadingZeros(alloc_size);
                    const u32 f   = (u32)0x80000000 >> w;
                    const u32 r   = 0xFFFFFFFF << (29 - w);
                    const u32 t   = ((f - 1) >> 2);
                    alloc_size    = (alloc_size + t) & ~t;
                    const s32 bin = (s32)((alloc_size & r) >> (29 - w)) + ((29 - w) * 4);
                    ASSERT(alloc_size <= m_abinconfigs[bin].m_alloc_size);
                    return m_abinconfigs[bin];
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
                u32 const          size = config->m_abinconfigs[s].m_alloc_size;
                binconfig_t const& bin  = config->size2bin(size);
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
            // binconfig_t(bin-index (or remap), alloc-size, chunk-config)
            static const binconfig_t c_abinconfigs[] = {
                {8,   8, c64KB},                 {8,   8, c64KB},                 // 0, 1
                {8,   8, c64KB},                 {8,   8, c64KB},                 // 2, 3
                {8,   8, c64KB},                 {8,   8, c64KB},                 // 4, 5
                {8,   8, c64KB},                 {8,   8, c64KB},                 // 6, 7
                {8,   8, c64KB},                 {12,   16, c64KB},                // 8, 9
                {12,   16, c64KB},               {12,   16, c64KB},               // 10, 11
                {12,   16, c64KB},               {16,   16, c64KB},               // 12, 13
                {16,   16, c64KB},               {16,   16, c64KB},               // 14, 15
                {16,   16, c64KB},               {18,   24, c64KB},               // 16, 17
                {18,   24, c64KB},               {20,   24, c64KB},               // 18, 19
                {20,   24, c64KB},               {22,   28, c64KB},               // 20, 21
                {22,   32, c64KB},               {24,   32, c64KB},               // 22, 23
                {24,   32, c64KB},               {25,   40, c64KB},               // 24, 25
                {26,   40, c64KB},               {27,   48, c64KB},               // 26, 27
                {28,   48, c64KB},               {29,   56, c64KB},               // 28, 29
                {30,   56, c64KB},               {31,   64, c64KB},               // 30, 31
                {32,   64, c64KB},               {33,   80, c64KB},               // 32, 33
                {34,   80, c64KB},               {35,   88, c64KB},               // 34, 35
                {36,   96, c64KB},               {37,   112, c64KB},              // 36, 37
                {38,   112, c64KB},              {39,   128, c64KB},              // 38, 39
                {40,   128, c64KB},              {41,   160, c64KB},              // 40, 41
                {42,   160, c64KB},              {43,   192, c64KB},              // 42, 43
                {44,   192, c64KB},              {45,   224, c64KB},              // 44, 45
                {46,   224, c64KB},              {47,   256, c64KB},              // 46, 47
                {48,   256, c64KB},              {49,   288, c64KB},              // 48, 49
                {50,   320, c64KB},              {51,   352, c64KB},              // 50, 51
                {52,   384, c64KB},              {53,   448, c64KB},              // 52, 53
                {54,   448, c64KB},              {55,   512, c64KB},              // 54, 55
                {56,   512, c64KB},              {57,   640, c64KB},              // 56, 57
                {58,   640, c64KB},              {59,   768, c64KB},              // 58, 59
                {60,   768, c64KB},              {61,   896, c64KB},              // 60, 61
                {62,   896, c64KB},              {63,   960, c64KB},              // 62, 63
                {64,  1*cKB, c64KB},             {65,  1*cKB + 128, c64KB},       // 64, 65
                {66,  1*cKB + 256, c128KB},      {67,  1*cKB + 384, c128KB},      // 66, 67
                {68,  1*cKB + 512, c128KB},      {69,  1*cKB + 640, c128KB},      // 68, 69
                {70,  1*cKB + 768, c128KB},      {71,  1*cKB + 896, c128KB},      // 70, 71
                {72,  2*cKB, c128KB},            {73,  2*cKB + 256, c128KB},      // 72, 73
                {74,  2*cKB + 512, c128KB},      {75,  2*cKB + 768, c128KB},      // 74, 75
                {76,  3*cKB, c128KB},            {77,  3*cKB + 256, c128KB},      // 76, 77
                {78,  3*cKB + 512, c128KB},      {79,  3*cKB + 768, c128KB},      // 78, 79
                {80,  4*cKB, c128KB},            {81,  4*cKB + 512, c128KB},      // 80, 81
                {82,  5*cKB, c128KB},            {83,  5*cKB + 512, c128KB},      // 82, 83
                {84,  6*cKB, c128KB},            {85,  6*cKB + 512, c128KB},      // 84, 85
                {86,  7*cKB, c128KB},            {87,  7*cKB + 512, c128KB},      // 86, 87
                {88,  8*cKB, c128KB},            {89,  9*cKB, c128KB},            // 88, 89
                {90,  10*cKB, c128KB},           {91,  11*cKB, c128KB},           // 90, 91
                {92,  12*cKB, c128KB},           {93,  13*cKB, c128KB},           // 92, 93
                {94,  14*cKB, c128KB},           {95,  15*cKB, c128KB},           // 94, 95
                {96,  16*cKB, c128KB},           {97,  18*cKB, c128KB},           // 96, 97
                {98,  20*cKB, c128KB},           {99,  22*cKB, c128KB},           // 98, 99
                {100,  24*cKB, c128KB},          {101,  26*cKB, c128KB},          // 100, 101
                {102,  28*cKB, c128KB},          {103,  30*cKB, c128KB},          // 102, 103
                {104,  32*cKB, c128KB},          {105,  36*cKB, c512KB},          // 104, 105
                {106,  40*cKB, c512KB},          {107,  44*cKB, c512KB},          // 106, 107
                {108,  48*cKB, c512KB},          {109,  52*cKB, c512KB},          // 108, 109
                {110,  56*cKB, c512KB},          {111,  60*cKB, c512KB},          // 110, 111
                {112,  64*cKB, c512KB},          {113,  72*cKB, c512KB},          // 112, 113
                {114,  80*cKB, c512KB},          {115,  88*cKB, c512KB},          // 114, 115
                {116,  96*cKB, c512KB},          {117,  104*cKB, c512KB},         // 116, 117
                {118,  112*cKB, c512KB},         {119,  120*cKB, c512KB},         // 118, 119
                {120,  128*cKB, c512KB},         {121,  144*cKB, c512KB},         // 120, 121
                {122,  160*cKB, c2MB},           {123,  176*cKB, c2MB},           // 122, 123
                {124,  192*cKB, c2MB},           {125,  208*cKB, c2MB},           // 124, 125
                {126,  224*cKB, c2MB},           {127,  240*cKB, c2MB},           // 126, 127
                {128,  256*cKB, c2MB},           {129,  288*cKB, c2MB},           // 128, 129
                {130,  320*cKB, c2MB},           {131,  352*cKB, c2MB},           // 130, 131
                {132,  384*cKB, c2MB},           {133,  416*cKB, c2MB},           // 132, 133
                {134,  448*cKB, c2MB},           {135,  480*cKB, c2MB},           // 134, 135
                {136,  512*cKB, c2MB},           {137,  576*cKB, c8MB},           // 136, 137
                {138,  640*cKB, c8MB},           {139,  704*cKB, c8MB},           // 138, 139
                {140,  768*cKB, c8MB},           {141,  832*cKB, c8MB},           // 140, 141
                {142,  896*cKB, c8MB},           {143,  960*cKB, c8MB},           // 142, 143
                {144, 1*cMB, c8MB},              {145, 1*cMB + 128*cKB, c8MB},    // 144, 145
                {146, 1*cMB + 256*cKB, c8MB},    {147, 1*cMB + 384*cKB, c8MB},    // 146, 147
                {148, 1*cMB + 512*cKB, c8MB},    {149, 1*cMB + 640*cKB, c8MB},    // 148, 149
                {150, 1*cMB + 768*cKB, c8MB},    {151, 1*cMB + 896*cKB, c8MB},    // 150, 151
                {152, 2*cMB, c32MB},             {153, 2*cMB + 256*cKB, c32MB},   // 152, 153
                {154, 2*cMB + 512*cKB, c32MB},   {155, 2*cMB + 768*cKB, c32MB},   // 154, 155
                {156, 3*cMB, c32MB},             {157, 3*cMB + 256*cKB, c32MB},   // 156, 157
                {158, 3*cMB + 512*cKB, c32MB},   {159, 3*cMB + 768*cKB, c32MB},   // 158, 159
                {160, 4*cMB, c32MB},             {161, 4*cMB + 512*cKB, c32MB},   // 160, 161
                {162, 5*cMB, c32MB},             {163, 5*cMB + 512*cKB, c32MB},   // 162, 163
                {164, 6*cMB, c32MB},             {165, 6*cMB + 512*cKB, c32MB},   // 164, 165
                {166, 7*cMB, c32MB},             {167, 7*cMB + 512*cKB, c32MB},   // 166, 167
                {168, 8*cMB, c32MB},             {169, 9*cMB, c32MB},             // 168, 169
                {170, 10*cKB, c32MB},            {171, 11*cMB, c32MB},            // 170, 171
                {172, 12*cMB, c32MB},            {173, 13*cMB, c32MB},            // 172, 173
                {174, 14*cMB, c32MB},            {175, 15*cMB, c32MB},            // 174, 175
                {176, 16*cMB, c32MB},            {177, 18*cMB, c32MB},            // 176, 177
                {178, 20*cKB, c32MB},            {179, 22*cMB, c32MB},            // 178, 179
                {180, 24*cMB, c32MB},            {181, 26*cMB, c32MB},            // 180, 181
                {182, 28*cMB, c32MB},            {183, 30*cKB, c32MB},            // 182, 183
                {184, 32*cMB, c32MB},            {185, 36*cMB, c128MB},           // 184, 185
                {186, 40*cKB, c128MB},           {187, 44*cMB, c128MB},           // 186, 187
                {188, 48*cMB, c128MB},           {189, 52*cMB, c128MB},           // 188, 189
                {190, 56*cMB, c128MB},           {191, 60*cKB, c128MB},           // 190, 191
                {192, 64*cMB, c128MB},           {193, 72*cMB, c128MB},           // 192, 193
                {194, 80*cKB, c128MB},           {195, 88*cMB, c128MB},           // 194, 195
                {196, 96*cMB, c128MB},           {197, 104*cMB, c128MB},          // 196, 197
                {198, 112*cMB, c128MB},          {199, 120*cMB, c128MB},          // 198, 199
                {200, 128*cMB, c128MB},          {201, 144*cMB, c512MB},          // 200, 201
                {202, 160*cMB, c512MB},          {203, 176*cMB, c512MB},          // 202, 203
                {204, 192*cMB, c512MB},          {205, 208*cMB, c512MB},          // 204, 205
                {206, 224*cMB, c512MB},          {207, 240*cKB, c512MB},          // 206, 207
                {208, 256*cMB, c512MB},          {209, 288*cMB, c512MB},          // 208, 209
                {210, 320*cMB, c512MB},          {211, 352*cMB, c512MB},          // 210, 211
                {212, 384*cMB, c512MB},          {213, 416*cMB, c512MB},          // 212, 213
                {214, 448*cMB, c512MB},          {215, 480*cKB, c512MB},          // 214, 215
            };
            static const s32 c_num_binconfigs = sizeof(c_abinconfigs) / sizeof(binconfig_t);
            // clang-format on

            // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly
            // 10% allocation waste (based on empirical data), however based on the allocation behaviour of the application, the waste can
            // be a lot more or a lot less than 10%.
            class config_10p_t : public config_t
            {
            public:
                binconfig_t const& size2bin(u32 alloc_size) const override final
                {
                    const s32 w   = math::countLeadingZeros(alloc_size);
                    const u32 f   = (u32)0x80000000 >> w;
                    const u32 r   = 0xFFFFFFFF << (28 - w);
                    const u32 t   = ((f - 1) >> 3);
                    alloc_size    = (alloc_size + t) & ~t;
                    const s32 bin = (s32)((alloc_size & r) >> (28 - w)) + ((28 - w) * 8);
                    ASSERT(alloc_size <= m_abinconfigs[bin].m_alloc_size);
                    return m_abinconfigs[bin];
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
                u32 const          size = config->m_abinconfigs[s].m_alloc_size;
                binconfig_t const& bin  = config->size2bin(size);
                ASSERT(size <= bin.m_alloc_size);
                ASSERT(bin.m_max_alloc_count >= 1);     // Zero is not allowed
                ASSERT(bin.m_max_alloc_count <= 4096);  // The binmap we use can only handle max 4096 bits
            }
#endif
            return config;
        }
    }  // namespace nsuperalloc
}  // namespace ncore
