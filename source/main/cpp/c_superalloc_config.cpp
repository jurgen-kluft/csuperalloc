#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "cbase/c_integer.h"

#include "csuperalloc/private/c_binmap.h"
#include "csuperalloc/c_superalloc.h"
#include "csuperalloc/c_superalloc_config.h"

#include "cvmem/c_virtual_memory.h"

namespace ncore
{
#define SUPERALLOC_DEBUG

    /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    /// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    /// The following is a strict data-drive initialization of the bins and allocators, please know what you are doing when modifying any of this.

    static const chunkconfig_t c4KB               = {12, 0};
    static const chunkconfig_t c16KB              = {14, 1};
    static const chunkconfig_t c32KB              = {15, 2};
    static const chunkconfig_t c64KB              = {16, 3};
    static const chunkconfig_t c128KB             = {17, 4};
    static const chunkconfig_t c256KB             = {18, 5};
    static const chunkconfig_t c512KB             = {19, 6};
    static const chunkconfig_t c1MB               = {20, 7};
    static const chunkconfig_t c2MB               = {21, 8};
    static const chunkconfig_t c4MB               = {22, 9};
    static const chunkconfig_t c8MB               = {23, 10};
    static const chunkconfig_t c16MB              = {24, 11};
    static const chunkconfig_t c32MB              = {25, 12};
    static const chunkconfig_t c64MB              = {26, 13};
    static const chunkconfig_t c128MB             = {27, 14};
    static const chunkconfig_t c256MB             = {28, 15};
    static const chunkconfig_t c512MB             = {29, 16};
    static const chunkconfig_t c_achunkconfigs[]  = {c4KB, c16KB, c32KB, c64KB, c128KB, c256KB, c512KB, c1MB, c2MB, c4MB, c8MB, c16MB, c32MB, c64MB, c128MB, c256MB, c512MB};
    static const u32           c_num_chunkconfigs = sizeof(c_achunkconfigs) / sizeof(chunkconfig_t);

    static void initialize(binconfig_t& bin, binconfig_t const& src)
    {
        bin.m_alloc_size      = src.m_alloc_size;
        bin.m_chunk_config    = src.m_chunk_config;
        bin.m_alloc_bin_index = src.m_alloc_bin_index;
        bin.m_max_alloc_count = (((u32)1 << bin.m_chunk_config.m_chunk_size_shift) / bin.m_alloc_size);
    }

    // clang-format off
    // binconfig_t(bin-index or remap, alloc-size, chunk-config)
    static const s32        c_num_binconfigs_25p           = 112;
    static const binconfig_t c_abinconfigs_25p[c_num_binconfigs_25p] = {
        {8,         8,  c64KB},                    {8,    8,  c64KB},                        // 8, 8
        {8,         8,  c64KB},                    {8,    8,  c64KB},                        // 8, 8
        {8,         8,  c64KB},                    {8,    8,  c64KB},                        // 8, 8
        {8,         8,  c64KB},                    {8,    8,  c64KB},                        // 8, 8
        {8,         8,  c64KB},                    {10,   10, c64KB},                        // 8, 12
        {10,        12, c64KB},                    {12,   14, c64KB},                        // 12, 16
        {12,        16, c64KB},                    {13,   20, c64KB},                        // 16, 20
        {14,        24, c64KB},                    {15,   28, c64KB},                        // 24, 28
        {16,        32, c64KB},                    {17,   40, c64KB},                        // 32, 40
        {18,        48, c64KB},                    {19,   56, c64KB},                        // 48, 56
        {20,        64, c64KB},                    {21,   80, c64KB},                        //
        {22,        96, c64KB},                    {23,   112, c64KB},                       //
        {24,       128, c64KB},                    {25,   160, c64KB},                       //
        {26,       192, c64KB},                    {27,   224, c64KB},                       //
        {28,       256, c64KB},                    {29,   320, c64KB},                       //
        {30,       384, c64KB},                    {31,   448, c64KB},                       //
        {32,       512, c64KB},                    {33,   640, c64KB},                       //
        {34,       768, c64KB},                    {35,   896, c64KB},                       //
        {36,   1 * cKB, c64KB},                    {37,  1*cKB + 256, c128KB},               //
        {38,   1 * cKB + 512, c128KB},             {39,  1*cKB + 768, c128KB},               //
        {40,   2 * cKB, c128KB},                   {41,  2*cKB + 512, c128KB},               //
        {42,   3 * cKB, c128KB},                   {43,  3*cKB + 512, c128KB},               //
        {44,   4 * cKB, c128KB},                   {45,  5*cKB, c128KB},                     //
        {46,   6 * cKB, c128KB},                   {47,  7*cKB, c128KB},                     //
        {48,   8 * cKB, c128KB},                   {49,  10*cKB, c128KB},                    //
        {50,  12 * cKB, c128KB},                   {51,  14*cKB, c128KB},                    //
        {52,  16 * cKB, c128KB},                   {53,  20*cKB, c128KB},                    //
        {54,  24 * cKB, c128KB},                   {55,  28*cKB, c128KB},                    //
        {56,  32 * cKB, c128KB},                   {57,  40*cKB, c512KB},                    //
        {58,  48 * cKB, c512KB},                   {59,  56*cKB, c512KB},                    //
        {60,  64 * cKB, c512KB},                   {61,  80*cKB, c512KB},                    //
        {62,  96 * cKB, c512KB},                   {63,  112*cKB, c512KB},                   //
        {64, 128 * cKB, c512KB},                   {65,  160*cKB, c1MB},                     //
        {66, 192 * cKB, c1MB},                     {67,  224*cKB, c1MB},                     //
        {68, 256 * cKB, c1MB},                     {69,  320*cKB, c1MB},                     //
        {70, 384 * cKB, c2MB},                     {71,  448*cKB, c2MB},                     //
        {72, 512 * cKB, c2MB},                     {73,  640*cKB, c8MB},                     //
        {74, 768 * cKB, c8MB},                     {75,  896*cKB, c8MB},                     //
        {76,   1 * cMB, c8MB},                     {77, 1*cMB + 256*cKB, c8MB},              //
        {78,   1 * cMB + 512 * cKB, c8MB},         {79, 1*cMB + 768*cKB, c8MB},              //
        {80,   2 * cMB, c32MB},                    {81, 2*cMB + 512*cKB, c32MB},             //
        {82,   3 * cMB, c32MB},                    {83, 3*cMB + 512*cKB, c32MB},             //
        {84,   4 * cMB, c32MB},                    {85, 5*cMB, c32MB},                       //
        {86,   6 * cMB, c32MB},                    {87, 7*cMB, c32MB},                       //
        {88,   8 * cMB, c32MB},                    {89, 10*cMB, c32MB},                      //
        {90,  12 * cMB, c32MB},                    {91, 14*cMB, c32MB},                      //
        {92,  16 * cMB, c32MB},                    {93, 20*cMB, c32MB},                      //
        {94,  24 * cMB, c32MB},                    {95, 28*cMB, c32MB},                      //
        {96,  32 * cMB, c32MB},                    {97, 40*cMB, c128MB},                     //
        {98,  48 * cMB, c128MB},                   {99, 56*cMB, c128MB},                     //
        {100,  64 * cMB, c128MB},                  {101, 80*cMB, c128MB},                    //
        {102,  96 * cMB, c128MB},                  {103, 112*cMB, c128MB},                   //
        {104, 128 * cMB, c128MB},                  {105, 160*cMB, c512MB},                   //
        {106, 192 * cMB, c512MB},                  {107, 224*cMB, c512MB},                   //
        {108, 256 * cMB, c512MB},                  {109, 320*cMB, c512MB},                   //
        {110, 384 * cMB, c512MB},                  {111, 448*cMB, c512MB},                   //
    };
    // clang-format on

    // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly
    // 25% allocation waste (based on empirical data), however based on the allocation behaviour of the application, the waste can
    // be a lot more or a lot less than 25%.
    class superalloc_config_windows_desktop_app_25p_t : public superalloc_config_t
    {
    public:
        binconfig_t const& size2bin(u32 alloc_size) const override final
        {
            const s32 w = math::countLeadingZeros(alloc_size);
            const u32 f = (u32)0x80000000 >> w;
            const u32 r = 0xFFFFFFFF << (29 - w);
            const u32 t = ((f - 1) >> 2);
            alloc_size  = (alloc_size + t) & ~t;
            int i       = (int)((alloc_size & r) >> (29 - w)) + ((29 - w) * 4);
            i           = m_abinconfigs_25p[i].m_alloc_bin_index;
            ASSERT(alloc_size <= m_abinconfigs_25p[i].m_alloc_size);
            return m_abinconfigs_25p[i];
        }

        binconfig_t m_abinconfigs_25p[c_num_binconfigs_25p];
    };

    static superalloc_config_windows_desktop_app_25p_t s_config_25p;

    superalloc_config_t const* gGetSuperAllocConfigWindowsDesktopApp25p()
    {
        const u64 c_total_address_space         = 256 * cGB;
        const u64 c_segment_address_range       = 1 * cGB;
        const u32 c_internal_heap_address_range = 16 * cMB;
        const u32 c_internal_heap_pre_size      = 2 * cMB;
        const u32 c_internal_fsa_address_range  = 256 * cMB;
        const u32 c_internal_fsa_segment_size   = 8 * cMB;
        const u32 c_internal_fsa_pre_size       = 16 * cMB;

        superalloc_config_windows_desktop_app_25p_t* config = &s_config_25p;

        config->m_total_address_size          = c_total_address_space;
        config->m_segment_address_range       = c_segment_address_range;
        config->m_internal_heap_address_range = c_internal_heap_address_range;
        config->m_internal_heap_pre_size      = c_internal_heap_pre_size;
        config->m_internal_fsa_address_range  = c_internal_fsa_address_range;
        config->m_internal_fsa_segment_size   = c_internal_fsa_segment_size;
        config->m_internal_fsa_pre_size       = c_internal_fsa_pre_size;
        config->m_segment_address_range_shift = math::ilog2(c_segment_address_range);
        config->m_num_binconfigs              = c_num_binconfigs_25p;
        config->m_num_chunkconfigs            = c_num_chunkconfigs;
        config->m_abinconfigs                 = config->m_abinconfigs_25p;
        config->m_achunkconfigs               = c_achunkconfigs;
        for (u32 s = 0; s < config->m_num_binconfigs; s++)
            initialize(config->m_abinconfigs_25p[s], c_abinconfigs_25p[s]);

#ifdef SUPERALLOC_DEBUG
        // sanity check on the binconfig_t config
        for (u32 s = 0; s < config->m_num_binconfigs; s++)
        {
            ASSERT(config->m_abinconfigs[s].m_max_alloc_count >= 1);
            u32 const          rs   = config->m_abinconfigs[s].m_alloc_bin_index;
            u32 const          size = config->m_abinconfigs[rs].m_alloc_size;
            binconfig_t const& bin  = config->size2bin(size);
            ASSERT(size <= bin.m_alloc_size);
            ASSERT(bin.m_max_alloc_count >= 1);
        }
#endif
        return config;
    }

    // clang-format off
    // binconfig_t(bin-index or remap, alloc-size, chunk-config)
    static const s32        c_num_binconfigs_10p = 216;
    static const binconfig_t c_abinconfigs_10p[c_num_binconfigs_10p] = {
        {8,   8, c64KB},                              {8,   8, c64KB},                       //
        {8,   8, c64KB},                              {8,   8, c64KB},                       //
        {8,   8, c64KB},                              {8,   8, c64KB},                       //
        {8,   8, c64KB},                              {8,   8, c64KB},                       //
        {8,   8, c64KB},                              {12,   9, c64KB},                      //
        {12,   10, c64KB},                            {12,   11, c64KB},                     //
        {12,   12, c64KB},                            {16,   13, c64KB},                     //
        {16,   14, c64KB},                            {16,   15, c64KB},                     //
        {16,   16, c64KB},                            {18,   18, c64KB},                     //
        {18,   20, c64KB},                            {20,   22, c64KB},                     //
        {20,   24, c64KB},                            {22,   26, c64KB},                     //
        {22,   28, c64KB},                            {24,   30, c64KB},                     //
        {24,   32, c64KB},                            {25,   36, c64KB},                     //
        {26,   40, c64KB},                            {27,   44, c64KB},                     //
        {28,   48, c64KB},                            {29,   52, c64KB},                     //
        {30,   56, c64KB},                            {31,   60, c64KB},                     //
        {32,   64, c64KB},                            {33,   72, c64KB},                     //
        {34,   80, c64KB},                            {35,   88, c64KB},                     //
        {36,   96, c64KB},                            {37,   104, c64KB},                    //
        {38,   112, c64KB},                           {39,   120, c64KB},                    //
        {40,   128, c64KB},                           {41,   144, c64KB},                    //
        {42,   160, c64KB},                           {43,   176, c64KB},                    //
        {44,   192, c64KB},                           {45,   208, c64KB},                    //
        {46,   224, c64KB},                           {47,   240, c64KB},                    //
        {48,   256, c64KB},                           {49,   288, c64KB},                    //
        {50,   320, c64KB},                           {51,   352, c64KB},                    //
        {52,   384, c64KB},                           {53,   416, c64KB},                    //
        {54,   448, c64KB},                           {55,   480, c64KB},                    //
        {56,   512, c64KB},                           {57,   576, c64KB},                    //
        {58,   640, c64KB},                           {59,   704, c64KB},                    //
        {60,   768, c64KB},                           {61,   832, c64KB},                    //
        {62,   896, c64KB},                           {63,   960, c64KB},                    //
        {64,  1*cKB, c64KB},                          {65,  1*cKB + 128, c64KB},             //
        {66,  1*cKB + 256, c64KB},                    {67,  1*cKB + 384, c64KB},             //
        {68,  1*cKB + 512, c64KB},                    {69,  1*cKB + 640, c64KB},             //
        {70,  1*cKB + 768, c64KB},                    {71,  1*cKB + 896, c64KB},             //
        {72,  2*cKB, c64KB},                          {73,  2*cKB + 256, c64KB},             //
        {74,  2*cKB + 512, c64KB},                    {75,  2*cKB + 768, c64KB},             //
        {76,  3*cKB, c64KB},                          {77,  3*cKB + 256, c64KB},             //
        {78,  3*cKB + 512, c64KB},                    {79,  3*cKB + 768, c64KB},             //
        {80,  4*cKB, c64KB},                          {81,  4*cKB + 512, c64KB},             //
        {82,  5*cKB, c64KB},                          {83,  5*cKB + 512, c64KB},             //
        {84,  6*cKB, c64KB},                          {85,  6*cKB + 512, c64KB},             //
        {86,  7*cKB, c64KB},                          {87,  7*cKB + 512, c64KB},             //
        {88,  8*cKB, c64KB},                          {89,  9*cKB, c64KB},                   //
        {90,  10*cKB, c64KB},                         {91,  11*cKB, c64KB},                  //
        {92,  12*cKB, c64KB},                         {93,  13*cKB, c64KB},                  //
        {94,  14*cKB, c64KB},                         {95,  15*cKB, c64KB},                  //
        {96,  16*cKB, c64KB},                         {97,  18*cKB, c64KB},                  //
        {98,  20*cKB, c64KB},                         {99,  22*cKB, c64KB},                  //
        {100,  24*cKB, c64KB},                        {101,  26*cKB, c64KB},                 //
        {102,  28*cKB, c64KB},                        {103,  30*cKB, c64KB},                 //
        {104,  32*cKB, c64KB},                        {105,  36*cKB, c64KB},                 //
        {106,  40*cKB, c64KB},                        {107,  44*cKB, c64KB},                 //
        {108,  48*cKB, c64KB},                        {109,  52*cKB, c64KB},                 //
        {110,  56*cKB, c64KB},                        {111,  60*cKB, c64KB},                 //
        {112,  64*cKB, c64KB},                        {113,  72*cKB, c64KB},                 //
        {114,  80*cKB, c64KB},                        {115,  88*cKB, c64KB},                 //
        {116,  96*cKB, c64KB},                        {117,  104*cKB, c64KB},                //
        {118,  112*cKB, c64KB},                       {119,  120*cKB, c64KB},                //
        {120,  128*cKB, c64KB},                       {121,  144*cKB, c64KB},                //
        {122,  160*cKB, c64KB},                       {123,  176*cKB, c64KB},                //
        {124,  192*cKB, c64KB},                       {125,  208*cKB, c64KB},                //
        {126,  224*cKB, c64KB},                       {127,  240*cKB, c64KB},                //
        {128,  256*cKB, c64KB},                       {129,  288*cKB, c64KB},                //
        {130,  320*cKB, c64KB},                       {131,  352*cKB, c64KB},                //
        {132,  384*cKB, c64KB},                       {133,  416*cKB, c64KB},                //
        {134,  448*cKB, c64KB},                       {135,  480*cKB, c64KB},                //
        {136,  512*cKB, c64KB},                       {137,  576*cKB, c64KB},                //
        {138,  640*cKB, c64KB},                       {139,  704*cKB, c64KB},                //
        {140,  768*cKB, c64KB},                       {141,  832*cKB, c64KB},                //
        {142,  896*cKB, c64KB},                       {143,  960*cKB, c64KB},                //
        {144, 1*cMB, c64KB},                          {145, 1*cMB + 128*cKB, c64KB},         //
        {146, 1*cMB + 256*cKB, c64KB},                {147, 1*cMB + 384*cKB, c64KB},         //
        {148, 1*cMB + 512*cKB, c64KB},                {149, 1*cMB + 640*cKB, c64KB},         //
        {150, 1*cMB + 768*cKB, c64KB},                {151, 1*cMB + 896*cKB, c64KB},         //
        {152, 2*cMB, c64KB},                          {153, 2*cMB + 256*cKB, c64KB},         //
        {154, 2*cMB + 512*cKB, c64KB},                {155, 2*cMB + 768*cKB, c64KB},         //
        {156, 3*cMB, c64KB},                          {157, 3*cMB + 256*cKB, c64KB},         //
        {158, 3*cMB + 512*cKB, c64KB},                {159, 3*cMB + 768*cKB, c64KB},         //
        {160, 4*cMB, c64KB},                          {161, 4*cMB + 512*cKB, c64KB},         //
        {162, 5*cMB, c64KB},                          {163, 5*cMB + 512*cKB, c64KB},         //
        {164, 6*cMB, c64KB},                          {165, 6*cMB + 512*cKB, c64KB},         //
        {166, 7*cMB, c64KB},                          {167, 7*cMB + 512*cKB, c64KB},         //
        {168, 8*cMB, c64KB},                          {169, 9*cMB, c64KB},                   //
        {170, 10*cKB, c64KB},                         {171, 11*cMB, c64KB},                  //
        {172, 12*cMB, c64KB},                         {173, 13*cMB, c64KB},                  //
        {174, 14*cMB, c64KB},                         {175, 15*cMB, c64KB},                  //
        {176, 16*cMB, c64KB},                         {177, 18*cMB, c64KB},                  //
        {178, 20*cKB, c64KB},                         {179, 22*cMB, c64KB},                  //
        {180, 24*cMB, c64KB},                         {181, 26*cMB, c64KB},                  //
        {182, 28*cMB, c64KB},                         {183, 30*cKB, c64KB},                  //
        {184, 32*cMB, c64KB},                         {185, 36*cMB, c64KB},                  //
        {186, 40*cKB, c64KB},                         {187, 44*cMB, c64KB},                  //
        {188, 48*cMB, c64KB},                         {189, 52*cMB, c64KB},                  //
        {190, 56*cMB, c64KB},                         {191, 60*cKB, c64KB},                  //
        {192, 64*cMB, c64KB},                         {193, 72*cMB, c64KB},                  //
        {194, 80*cKB, c64KB},                         {195, 88*cMB, c64KB},                  //
        {196, 96*cMB, c64KB},                         {197, 104*cMB, c64KB},                 //
        {198, 112*cMB, c64KB},                        {199, 120*cKB, c64KB},                 //
        {200, 128*cMB, c64KB},                        {201, 144*cMB, c64KB},                 //
        {202, 160*cKB, c64KB},                        {203, 176*cMB, c64KB},                 //
        {204, 192*cMB, c64KB},                        {205, 208*cMB, c64KB},                 //
        {206, 224*cMB, c64KB},                        {207, 240*cKB, c64KB},                 //
        {208, 256*cMB, c64KB},                        {209, 288*cMB, c64KB},                 //
        {210, 320*cKB, c64KB},                        {211, 352*cMB, c64KB},                 //
        {212, 384*cMB, c64KB},                        {213, 416*cMB, c64KB},                 //
        {214, 448*cMB, c64KB},                        {215, 480*cKB, c64KB},                 //
    };
    // clang-format on

    // Note: It is preferable to analyze the memory usage of the application and adjust the superallocator configuration accordingly
    // 10% allocation waste (based on empirical data), however based on the allocation behaviour of the application, the waste can
    // be a lot more or a lot less than 10%.
    class superalloc_config_windows_desktop_app_10p_t : public superalloc_config_t
    {
    public:
        binconfig_t const& size2bin(u32 alloc_size) const override final
        {
            const s32 w = math::countLeadingZeros(alloc_size);
            const u32 f = (u32)0x80000000 >> w;
            const u32 r = 0xFFFFFFFF << (28 - w);
            const u32 t = ((f - 1) >> 3);
            alloc_size  = (alloc_size + t) & ~t;
            int i       = (int)((alloc_size & r) >> (28 - w)) + ((28 - w) * 8);
            i           = m_abinconfigs[i].m_alloc_bin_index;
            ASSERT(alloc_size <= m_abinconfigs[i].m_alloc_size);
            return m_abinconfigs[i];
        }

        binconfig_t m_abinconfigs_10p[c_num_binconfigs_10p];
    };

    static superalloc_config_windows_desktop_app_10p_t s_config_10p;

    superalloc_config_t const* gGetSuperAllocConfigWindowsDesktopApp10p()
    {
        const u64 c_total_address_space         = 128 * cGB;
        const u64 c_segment_address_range       = 1 * cGB;
        const u32 c_internal_heap_address_range = 16 * cMB;
        const u32 c_internal_heap_pre_size      = 2 * cMB;
        const u32 c_internal_fsa_address_range  = 256 * cMB;
        const u32 c_internal_fsa_segment_size   = 8 * cMB;
        const u32 c_internal_fsa_pre_size       = 16 * cMB;

        superalloc_config_windows_desktop_app_10p_t* config = &s_config_10p;

        config->m_total_address_size          = c_total_address_space;
        config->m_segment_address_range       = c_segment_address_range;
        config->m_internal_heap_address_range = c_internal_heap_address_range;
        config->m_internal_heap_pre_size      = c_internal_heap_pre_size;
        config->m_internal_fsa_address_range  = c_internal_fsa_address_range;
        config->m_internal_fsa_segment_size   = c_internal_fsa_segment_size;
        config->m_internal_fsa_pre_size       = c_internal_fsa_pre_size;
        config->m_segment_address_range_shift = math::ilog2(c_segment_address_range);
        config->m_num_binconfigs              = c_num_binconfigs_10p;
        config->m_num_chunkconfigs            = c_num_chunkconfigs;
        config->m_abinconfigs                 = config->m_abinconfigs_10p;
        config->m_achunkconfigs               = c_achunkconfigs;
        for (u32 s = 0; s < config->m_num_binconfigs; s++)
            initialize(config->m_abinconfigs_10p[s], c_abinconfigs_25p[s]);

#ifdef SUPERALLOC_DEBUG
        // sanity check on the binconfig_t config
        for (u32 s = 0; s < config->m_num_binconfigs; s++)
        {
            u32 const          rs   = config->m_abinconfigs[s].m_alloc_bin_index;
            u32 const          size = config->m_abinconfigs[rs].m_alloc_size;
            binconfig_t const& bin  = config->size2bin(size);
            ASSERT(size <= bin.m_alloc_size);
        }
#endif
        return config;
    }

}  // namespace ncore