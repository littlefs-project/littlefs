// littlefs bench runner defines


#ifdef BENCH_INCLUDE
    #ifndef BENCH_DEFINES_H
    #define BENCH_DEFINES_H

    // DISK_GEOMETRY controls which simulation we use
    // 0 => NOR flash (the default)
    // 1 => NAND flash
    // 2 => NAND+FTL
    // 3 => SD/eMMC
    // 4 => FRAM
    #define DISK_MAP(define) \
            ((DISK_GEOMETRY == 0)      ? NOR_##define     \
                : (DISK_GEOMETRY == 1) ? NAND_##define    \
                : (DISK_GEOMETRY == 2) ? NANDFTL_##define \
                : (DISK_GEOMETRY == 3) ? EMMC_##define    \
                : (DISK_GEOMETRY == 4) ? FRAM_##define    \
                                       : 0)
    #endif
#endif

// preconfigured defines that control how benches run
#ifdef BENCH_DEFINE
    //           name                   value (overridable)
    BENCH_DEFINE(CFG_FLAGS,             0                                   )
    BENCH_DEFINE(DISK_SIZE,             128*1024*1024                       )
    BENCH_DEFINE(DISK_GEOMETRY,         0                                   )
    // simulation mode
    // 0 => full bus+buffer sim
    // 1 => simple per-byte sim
    BENCH_DEFINE(DISK_SIM,              0                                   )
    BENCH_DEFINE(READ_SIZE,             DISK_MAP(READ_SIZE)                 )
    BENCH_DEFINE(PROG_SIZE,             DISK_MAP(PROG_SIZE)                 )
    BENCH_DEFINE(ERASE_SIZE,            DISK_MAP(ERASE_SIZE)                )
    BENCH_DEFINE(BLOCK_SIZE,            LFS3_MAX(ERASE_SIZE, 512)           )
    BENCH_DEFINE(BLOCK_COUNT,           DISK_SIZE/LFS3_MAX(BLOCK_SIZE, 1)   )
    BENCH_DEFINE(BLOCK_RECYCLES,        100                                 )
    BENCH_DEFINE(RCACHE_SIZE,           LFS3_MAX(16, READ_SIZE)             )
    BENCH_DEFINE(PCACHE_SIZE,           LFS3_MAX(16, PROG_SIZE)             )
    BENCH_DEFINE(FCACHE_SIZE,           16                                  )
    BENCH_DEFINE(LOOKAHEAD_SIZE,        16                                  )
    BENCH_DEFINE(LOOKGBMAP_THRESH,      BLOCK_COUNT/8                       )
    BENCH_DEFINE(EVICTQUEUE_COUNT,      2                                   )
    // report estimated buffer usage
    BENCH_DEFINE(BUF_WATERMARK,         RCACHE_SIZE
                                            + PCACHE_SIZE
                                            + FCACHE_SIZE
                                            + LOOKAHEAD_SIZE
                                            + LFS3_IFDEF_EVICT(
                                                EVICTQUEUE_COUNT
                                                    * sizeof(lfs3_evict_t),
                                                0)                          )
    BENCH_DEFINE(GC_FLAGS,              LFS3_GC_GC                          )
    BENCH_DEFINE(GC_STEPS,              0                                   )
    BENCH_DEFINE(GC_LOOKAHEAD_THRESH,   BLOCK_COUNT                         )
    BENCH_DEFINE(GC_LOOKGBMAP_THRESH,   BLOCK_COUNT - BLOCK_COUNT/2         )
    BENCH_DEFINE(GC_PREERASE_COUNT,     BLOCK_COUNT                         )
    BENCH_DEFINE(GC_COMPACTMETA_THRESH, BLOCK_SIZE - BLOCK_SIZE/8           )
    BENCH_DEFINE(GC_COMPACTBTREE_THRESH,
                                        0                                   )
    BENCH_DEFINE(SHRUB_SIZE,            BLOCK_SIZE/8                        )
    BENCH_DEFINE(GRAIN_SIZE,            LFS3_MIN(BLOCK_SIZE/16, 512)        )
    BENCH_DEFINE(CRYSTAL_THRESH,        BLOCK_SIZE/16                       )
    // don't bother simulating erases, this may be less realistic, but
    // it's certainly faster!
    BENCH_DEFINE(ERASE_VALUE,           -1                                  )
    BENCH_DEFINE(READ_WIDTH,            DISK_MAP(READ_WIDTH)                )
    BENCH_DEFINE(PROG_WIDTH,            DISK_MAP(PROG_WIDTH)                )
    BENCH_DEFINE(ERASE_WIDTH,           DISK_MAP(ERASE_WIDTH)               )
    BENCH_DEFINE(READ_TIMING,           DISK_MAP(READ_TIMING)               )
    BENCH_DEFINE(PROG_TIMING,           DISK_MAP(PROG_TIMING)               )
    BENCH_DEFINE(ERASE_TIMING,          DISK_MAP(ERASE_TIMING)              )
    BENCH_DEFINE(READ_WTIMING,          DISK_MAP(READ_WTIMING)              )
    BENCH_DEFINE(PROG_WTIMING,          DISK_MAP(PROG_WTIMING)              )
    BENCH_DEFINE(ERASE_WTIMING,         DISK_MAP(ERASE_WTIMING)             )
    BENCH_DEFINE(READ_UTIMING,          DISK_MAP(READ_UTIMING)              )
    BENCH_DEFINE(PROG_UTIMING,          DISK_MAP(PROG_UTIMING)              )
    BENCH_DEFINE(ERASE_UTIMING,         DISK_MAP(ERASE_UTIMING)             )

    // NOR flash (DISK_GEOMETRY=0)
    //
    // based on w25q64jv:
    // https://www.winbond.com/resource-files/
    //         W25Q64JV%20RevM%2012242024%20Plus.pdf
    //
    // note one thing unique to NOR flash is the extreme erase cost
    //
    // FR = 104MHz, quad prog (not read!)
    // fR = 50MHz, quad read
    // sector = 4096
    // tSE = 45ms
    // page = 256
    // tPP = 0.4ms
    //
    // main bus = 104MHz * quad prog
    //          = ~9.6ns * 8/4
    //          = ~19ns/B
    // read bus = 50MHz * quad read
    //          = 20ns * 8/4
    //          = 40ns/B
    //
    // read cmd = 8op + 6addr + 2mode + 4dummy
    //          = 20 * ~19ns/B (main bus)
    //          = ~380ns
    // prog cmd = 8op + 24addr
    //          = 32 * ~19ns/B (main bus)
    //          = ~608ns
    // erase cmd = 8op + 24addr
    //           = 32 * ~19ns/B (main bus)
    //           = ~608ns
    //
    // simple per-byte sim:
    // readed = 40ns/B (read bus)
    // progged = tPP/page + main bus
    //         = 0.4ms/256 + ~19ns/B
    //         = ~1582ns/B
    // erased = tSE/sector
    //        = 45ms/4096
    //        = ~10986ns/B
    //
    // less-simple bus+buffer sim:
    // read = ~380ns (read cmd)
    // prog = ~608ns (prog cmd)
    // erase = ~608ns (erase cmd)
    // wread = 0ns/B (no transaction cost)
    // wprog = tPP/page
    //       = 0.4ms/256
    //       = ~1563ns/B
    // werase = tSE/sector
    //        = ~10986ns/B
    // readed = 40ns/B (read bus)
    // progged = ~19ns/B (main bus)
    // erased = 0ns/B (no bus cost)
    //
    BENCH_DEFINE(NOR_READ_SIZE,         1                                   )
    BENCH_DEFINE(NOR_PROG_SIZE,         1                                   )
    BENCH_DEFINE(NOR_ERASE_SIZE,        4096                                )
    BENCH_DEFINE(NOR_READ_WIDTH,        1                                   )
    BENCH_DEFINE(NOR_PROG_WIDTH,        256                                 )
    BENCH_DEFINE(NOR_ERASE_WIDTH,       LFS3_MIN(ERASE_SIZE, BLOCK_SIZE)    )
    BENCH_DEFINE(NOR_READ_TIMING,       (DISK_SIM == 0) ? 380   : 0         )
    BENCH_DEFINE(NOR_PROG_TIMING,       (DISK_SIM == 0) ? 608   : 0         )
    BENCH_DEFINE(NOR_ERASE_TIMING,      (DISK_SIM == 0) ? 608   : 0         )
    BENCH_DEFINE(NOR_READ_WTIMING,      0                                   )
    BENCH_DEFINE(NOR_PROG_WTIMING,      (DISK_SIM == 0)
                                            ? 1563*NOR_PROG_WIDTH
                                            : 0                             )
    BENCH_DEFINE(NOR_ERASE_WTIMING,     (DISK_SIM == 0)
                                            ? 10986*NOR_ERASE_WIDTH
                                            : 0                             )
    BENCH_DEFINE(NOR_READ_UTIMING,      40                                  )
    BENCH_DEFINE(NOR_PROG_UTIMING,      (DISK_SIM == 0) ? 19    : 1582      )
    BENCH_DEFINE(NOR_ERASE_UTIMING,     (DISK_SIM == 0) ? 0     : 10986     )

    // NAND flash (DISK_GEOMETRY=1)
    //
    // based on w25n01gv:
    // https://www.winbond.com/resource-files/W25N01GV%20Rev%20R%20070323.pdf
    //
    // FR = 104MHz, quad read/prog
    // block = 131072
    // tBE = 2ms
    // page = 2048
    // sector = 512
    // tPP = 250us
    // tRD1 = 25us
    //
    // bus = 104MHz * quad read/prog
    //     = ~9.6ns * 8/4
    //     = ~19ns/B
    //
    // read cmd = read page + read col
    //            (read page = 8op + 8dummy + 16addr)
    //            (          = 32 * bus             )
    //            (read col = 8op + 4addr + 10dummy)
    //            (         = 22 * bus             )
    //          = (32 + 22) * bus
    //          = 54 * ~19ns/B (bus)
    //          = ~1026ns
    // prog cmd = prog col + prog page
    //            (prog col = 8op + 16addr)
    //            (         = 24 * bus    )
    //            (prog page = 8op + 8dummy + 16addr)
    //            (          = 32 * bus             )
    //          = (24 + 32) * bus
    //          = 56 * ~19ns/B (bus)
    //          = ~1064ns
    // erase cmd = 8op + 8dummy + 16addr
    //           = 32 * ~19ns/B (bus)
    //           = ~608ns
    //
    // simple per-byte sim:
    // readed = tRD1/page + bus
    //        = 25us/2048 + ~19ns/B
    //        = ~31ns/B
    // progged = tPP/page + bus
    //         = 250us/2048 + ~19ns/B
    //         = ~141ns/B
    // erased = tBE/block
    //        = 2ms/131072
    //        = ~15ns/B
    //
    // less-simple bus+buffer sim:
    // read = ~1026ns (read cmd)
    // prog = ~1064ns (prog cmd)
    // erase = ~608ns (erase cmd)
    // wread = tRD1/page
    //       = 25us/2048
    //       = ~12ns/B
    // wprog = tPP/page
    //       = 250us/2048
    //       = ~122ns/B
    // werase = tBE/block
    //        = 2ms/131072
    //        = ~15ns/B
    // readed = ~19ns/B (bus)
    // progged = ~19ns/B (bus)
    // erased = 0ns/B (no bus cost)
    //
    BENCH_DEFINE(NAND_READ_SIZE,        1                                   )
    BENCH_DEFINE(NAND_PROG_SIZE,        512                                 )
    BENCH_DEFINE(NAND_ERASE_SIZE,       131072                              )
    BENCH_DEFINE(NAND_READ_WIDTH,       2048                                )
    BENCH_DEFINE(NAND_PROG_WIDTH,       2048                                )
    BENCH_DEFINE(NAND_ERASE_WIDTH,      LFS3_MIN(ERASE_SIZE, BLOCK_SIZE)    )
    BENCH_DEFINE(NAND_READ_TIMING,      (DISK_SIM == 0) ? 1026  : 0         )
    BENCH_DEFINE(NAND_PROG_TIMING,      (DISK_SIM == 0) ? 1064  : 0         )
    BENCH_DEFINE(NAND_ERASE_TIMING,     (DISK_SIM == 0) ? 608   : 0         )
    BENCH_DEFINE(NAND_READ_WTIMING,     (DISK_SIM == 0)
                                            ? 12*NAND_READ_WIDTH
                                            : 0                             )
    BENCH_DEFINE(NAND_PROG_WTIMING,     (DISK_SIM == 0)
                                            ? 122*NAND_PROG_WIDTH
                                            : 0                             )
    BENCH_DEFINE(NAND_ERASE_WTIMING,    (DISK_SIM == 0)
                                            ? 15*NAND_ERASE_WIDTH
                                            : 0                             )
    BENCH_DEFINE(NAND_READ_UTIMING,     (DISK_SIM == 0) ? 19    : 31        )
    BENCH_DEFINE(NAND_PROG_UTIMING,     (DISK_SIM == 0) ? 19    : 141       )
    BENCH_DEFINE(NAND_ERASE_UTIMING,    (DISK_SIM == 0) ? 0     : 15        )

    // NAND+FTL (DISK_GEOMETRY=2)
    //
    // this just uses the above NAND flash (w25n01gv) and assumes a
    // perfect FTL
    //
    // FR = 104MHz, quad read/prog
    // block = 131072
    // tBE = 2ms
    // page = 2048
    // sector = 512
    // tPP = 250us
    // tRD1 = 25us
    //
    // bus = 104MHz * quad read/prog
    //     = ~9.6ns * 8/4
    //     = ~19ns/B
    //
    // read cmd = read page + read col
    //            (read page = 8op + 8dummy + 16addr)
    //            (          = 32 * bus             )
    //            (read col = 8op + 4addr + 10dummy)
    //            (         = 22 * bus             )
    //          = (32 + 22) * bus
    //          = 54 * ~19ns/B (bus)
    //          = ~1026ns
    // prog cmd = prog col + prog page
    //            (prog col = 8op + 16addr)
    //            (         = 24 * bus    )
    //            (prog page = 8op + 8dummy + 16addr)
    //            (          = 32 * bus             )
    //          = (24 + 32) * bus
    //          = 56 * ~19ns/B (bus)
    //          = ~1064ns
    // erase cmd = 8op + 8dummy + 16addr
    //           = 32 * ~19ns/B (bus)
    //           = ~608ns
    //
    // erase = erase cmd + tBE
    //       = ~608ns + 2ms
    //       = ~2000608ns
    //
    // simple per-byte sim:
    // readed = tRD1/page + bus
    //        = 25us/2048 + ~19ns/B
    //        = ~31ns/B
    // progged = tPP/page + bus + erase/block
    //         = 250us/2048 + ~19ns/B + ~2000608ns/131072
    //         = ~156ns/B
    // erased = 0ns/B (noop)
    //
    // less-simple bus+buffer sim:
    // read = ~1026ns (read cmd)
    // prog = ~1064ns (prog cmd)
    // erase = 0ns (noop)
    // wread = tRD1/page
    //       = 25us/2048
    //       = ~12ns/B
    // wprog = tPP/page + erase/block
    //       = 250us/2048 + ~2000608ns/131072
    //       = ~137ns/B
    // werase = 0ns/B (noop)
    // readed = ~19ns/B (bus)
    // progged = ~19ns/B (bus)
    // erased = 0ns/B (no bus cost)
    //
    BENCH_DEFINE(NANDFTL_READ_SIZE,     1                                   )
    BENCH_DEFINE(NANDFTL_PROG_SIZE,     512                                 )
    BENCH_DEFINE(NANDFTL_ERASE_SIZE,    512                                 )
    BENCH_DEFINE(NANDFTL_READ_WIDTH,    2048                                )
    BENCH_DEFINE(NANDFTL_PROG_WIDTH,    2048                                )
    BENCH_DEFINE(NANDFTL_ERASE_WIDTH,   LFS3_MIN(ERASE_SIZE, BLOCK_SIZE)    )
    BENCH_DEFINE(NANDFTL_READ_TIMING,   (DISK_SIM == 0) ? 1026  : 0         )
    BENCH_DEFINE(NANDFTL_PROG_TIMING,   (DISK_SIM == 0) ? 1064  : 0         )
    BENCH_DEFINE(NANDFTL_ERASE_TIMING,  0                                   )
    BENCH_DEFINE(NANDFTL_READ_WTIMING,  (DISK_SIM == 0)
                                            ? 12*EMMC_READ_WIDTH
                                            : 0                             )
    BENCH_DEFINE(NANDFTL_PROG_WTIMING,  (DISK_SIM == 0)
                                            ? 137*EMMC_PROG_WIDTH
                                            : 0                             )
    BENCH_DEFINE(NANDFTL_ERASE_WTIMING, 0                                   )
    BENCH_DEFINE(NANDFTL_READ_UTIMING,  (DISK_SIM == 0) ? 19    : 31        )
    BENCH_DEFINE(NANDFTL_PROG_UTIMING,  (DISK_SIM == 0) ? 19    : 156       )
    BENCH_DEFINE(NANDFTL_ERASE_UTIMING, 0                                   )

    // SD/eMMC (DISK_GEOMETRY=3)
    //
    // this just uses the above NAND flash (w25n01gv) and assumes a
    // perfect FTL
    //
    // unlike NAND+FTL, this also limits reads to a single sector (progs
    // were already limited since we can't prog a sub-sector)
    //
    // FR = 104MHz, quad read/prog
    // block = 131072
    // tBE = 2ms
    // page = 2048
    // sector = 512
    // tPP = 250us
    // tRD1 = 25us
    //
    // bus = 104MHz * quad read/prog
    //     = ~9.6ns * 8/4
    //     = ~19ns/B
    //
    // read cmd = read page + read col
    //            (read page = 8op + 8dummy + 16addr)
    //            (          = 32 * bus             )
    //            (read col = 8op + 4addr + 10dummy)
    //            (         = 22 * bus             )
    //          = (32 + 22) * bus
    //          = 54 * ~19ns/B (bus)
    //          = ~1026ns
    // prog cmd = prog col + prog page
    //            (prog col = 8op + 16addr)
    //            (         = 24 * bus    )
    //            (prog page = 8op + 8dummy + 16addr)
    //            (          = 32 * bus             )
    //          = (24 + 32) * bus
    //          = 56 * ~19ns/B (bus)
    //          = ~1064ns
    // erase cmd = 8op + 8dummy + 16addr
    //           = 32 * ~19ns/B (bus)
    //           = ~608ns
    //
    // erase = erase cmd + tBE
    //       = ~608ns + 2ms
    //       = ~2000608ns
    //
    // simple per-byte sim:
    // readed = tRD1/page + bus
    //        = 25us/2048 + ~19ns/B
    //        = ~31ns/B
    // progged = tPP/page + bus + erase/block
    //         = 250us/2048 + ~19ns/B + ~2000608ns/131072
    //         = ~156ns/B
    // erased = 0ns/B (noop)
    //
    // less-simple bus+buffer sim:
    // read = ~1026ns (read cmd)
    // prog = ~1064ns (prog cmd)
    // erase = 0ns (noop)
    // wread = tRD1/page
    //       = 25us/2048
    //       = ~12ns/B
    // wprog = tPP/page + erase/block
    //       = 250us/2048 + ~2000608ns/131072
    //       = ~137ns/B
    // werase = 0ns/B (noop)
    // readed = ~19ns/B (bus)
    // progged = ~19ns/B (bus)
    // erased = 0ns/B (no bus cost)
    //
    BENCH_DEFINE(EMMC_READ_SIZE,        512                                 )
    BENCH_DEFINE(EMMC_PROG_SIZE,        512                                 )
    BENCH_DEFINE(EMMC_ERASE_SIZE,       512                                 )
    BENCH_DEFINE(EMMC_READ_WIDTH,       2048                                )
    BENCH_DEFINE(EMMC_PROG_WIDTH,       2048                                )
    BENCH_DEFINE(EMMC_ERASE_WIDTH,      LFS3_MIN(ERASE_SIZE, BLOCK_SIZE)    )
    BENCH_DEFINE(EMMC_READ_TIMING,      (DISK_SIM == 0) ? 1026  : 0         )
    BENCH_DEFINE(EMMC_PROG_TIMING,      (DISK_SIM == 0) ? 1064  : 0         )
    BENCH_DEFINE(EMMC_ERASE_TIMING,     0                                   )
    BENCH_DEFINE(EMMC_READ_WTIMING,     (DISK_SIM == 0)
                                            ? 12*EMMC_READ_WIDTH
                                            : 0                             )
    BENCH_DEFINE(EMMC_PROG_WTIMING,     (DISK_SIM == 0)
                                            ? 137*EMMC_PROG_WIDTH
                                            : 0                             )
    BENCH_DEFINE(EMMC_ERASE_WTIMING,    0                                   )
    BENCH_DEFINE(EMMC_READ_UTIMING,     (DISK_SIM == 0) ? 19    : 31        )
    BENCH_DEFINE(EMMC_PROG_UTIMING,     (DISK_SIM == 0) ? 19    : 156       )
    BENCH_DEFINE(EMMC_ERASE_UTIMING,    0                                   )

    // FRAM (DISK_GEOMETRY=4)
    //
    // based on cy15b102qsn:
    // https://www.infineon.com/assets/row/public/documents/10/49/
    //         infineon-cy15b102qsn-cy15v102qsn-excelon-ultra-2-mbit-
    //         256k-x-8-quad-spi-f-ram-datasheet-en.pdf
    //
    // fSCK = 108MHz, quad read/write
    //
    // bus = 108MHz * quad read/write
    //     = ~9.3ns * 8/4
    //     = ~19ns/B
    //
    // read cmd = 8op + 6addr + 2mode + 7dummy
    //          = 15 * ~19ns/B (bus)
    //          = ~285ns
    // prog cmd = 8op + 6addr + 2mode
    //          = 16 * ~19ns/B (bus)
    //          = ~304ns
    //
    // simple per-byte sim:
    // readed = 19ns/B (bus)
    // progged = 19ns/B (bus)
    // erased = 0ns/B (noop)
    //
    // less-simple bus+buffer sim:
    // read = 285ns (read cmd)
    // prog = 304ns (prog cmd)
    // erase = 0ns (noop)
    // wread = 0ns/B (no transaction cost)
    // wprog = 0ns/B (no transaction cost)
    // werase = 0ns/B (noop)
    // readed = 19ns/B (bus)
    // progged = 19ns/B (bus)
    // erased = 0ns/B (noop)
    //
    BENCH_DEFINE(FRAM_READ_SIZE,        1                                   )
    BENCH_DEFINE(FRAM_PROG_SIZE,        1                                   )
    BENCH_DEFINE(FRAM_ERASE_SIZE,       1                                   )
    BENCH_DEFINE(FRAM_READ_WIDTH,       1                                   )
    BENCH_DEFINE(FRAM_PROG_WIDTH,       1                                   )
    BENCH_DEFINE(FRAM_ERASE_WIDTH,      LFS3_MIN(ERASE_SIZE, BLOCK_SIZE)    )
    BENCH_DEFINE(FRAM_READ_TIMING,      (DISK_SIM == 0) ? 285 : 0           )
    BENCH_DEFINE(FRAM_PROG_TIMING,      (DISK_SIM == 0) ? 304 : 0           )
    BENCH_DEFINE(FRAM_ERASE_TIMING,     0                                   )
    BENCH_DEFINE(FRAM_READ_WTIMING,     0                                   )
    BENCH_DEFINE(FRAM_PROG_WTIMING,     0                                   )
    BENCH_DEFINE(FRAM_ERASE_WTIMING,    0                                   )
    BENCH_DEFINE(FRAM_READ_UTIMING,     19                                  )
    BENCH_DEFINE(FRAM_PROG_UTIMING,     19                                  )
    BENCH_DEFINE(FRAM_ERASE_UTIMING,    0                                   )
#endif


// struct lfs3_cfg definition
#ifdef BENCH_CFG
    struct lfs3_cfg _cfg = {
        .flags                          = CFG_FLAGS,
        #ifdef BENCH_CFG_CFG
        BENCH_CFG_CFG
        #endif
        .read_size                      = READ_SIZE,
        .prog_size                      = PROG_SIZE,
        .block_size                     = BLOCK_SIZE,
        .block_count                    = BLOCK_COUNT,
        .block_recycles                 = BLOCK_RECYCLES,
        .rcache_size                    = RCACHE_SIZE,
        .pcache_size                    = PCACHE_SIZE,
        .fcache_size                    = FCACHE_SIZE,
        .lookahead_size                 = LOOKAHEAD_SIZE,
        #ifdef LFS3_GBMAP
        .lookgbmap_thresh               = LOOKGBMAP_THRESH,
        .gc_lookgbmap_thresh            = GC_LOOKGBMAP_THRESH,
        #endif
        #ifdef LFS3_PREERASE
        .gc_preerase_count              = GC_PREERASE_COUNT,
        #endif
        #ifdef LFS3_EVICT
        .evictqueue_count               = EVICTQUEUE_COUNT,
        #endif
        #ifdef LFS3_GC
        .gc_flags                       = GC_FLAGS,
        .gc_steps                       = GC_STEPS,
        #endif
        .gc_lookahead_thresh            = GC_LOOKAHEAD_THRESH,
        .gc_compactmeta_thresh          = GC_COMPACTMETA_THRESH,
        .gc_compactbtree_thresh         = GC_COMPACTBTREE_THRESH,
        .shrub_size                     = SHRUB_SIZE,
        .grain_size                     = GRAIN_SIZE,
        .crystal_thresh                 = CRYSTAL_THRESH,
    };
    struct lfs3_cfg *BENCH_CFG = &_cfg;
#endif


// struct lfs3_*bd_cfg definition
#ifdef BENCH_BDCFG
    #ifndef BENCH_KIWIBD
    struct lfs3_emubd_cfg _bdcfg = {
        #ifdef BENCH_BDCFG_CFG
        BENCH_BDCFG_CFG
        #endif
        .erase_value                    = ERASE_VALUE,
        .read_width                     = READ_WIDTH,
        .prog_width                     = PROG_WIDTH,
        .erase_width                    = ERASE_WIDTH,
        .read_timing                    = READ_TIMING,
        .prog_timing                    = PROG_TIMING,
        .erase_timing                   = ERASE_TIMING,
        .read_wtiming                   = READ_WTIMING,
        .prog_wtiming                   = PROG_WTIMING,
        .erase_wtiming                  = ERASE_WTIMING,
        .read_utiming                   = READ_UTIMING,
        .prog_utiming                   = PROG_UTIMING,
        .erase_utiming                  = ERASE_UTIMING,
        .erase_cycles                   = ERASE_CYCLES,
        .badblock_behavior              = BADBLOCK_BEHAVIOR,
        .powerloss_behavior             = POWERLOSS_BEHAVIOR,
        .seed                           = BD_SEED,
    };
    struct lfs3_emubd_cfg *BENCH_BDCFG = &_bdcfg;
    #else
    struct lfs3_kiwibd_cfg _bdcfg = {
        #ifdef BENCH_BDCFG_CFG
        BENCH_BDCFG_CFG
        #endif
        .erase_value                    = ERASE_VALUE,
        .read_width                     = READ_WIDTH,
        .prog_width                     = PROG_WIDTH,
        .erase_width                    = ERASE_WIDTH,
        .read_timing                    = READ_TIMING,
        .prog_timing                    = PROG_TIMING,
        .erase_timing                   = ERASE_TIMING,
        .read_wtiming                   = READ_WTIMING,
        .prog_wtiming                   = PROG_WTIMING,
        .erase_wtiming                  = ERASE_WTIMING,
        .read_utiming                   = READ_UTIMING,
        .prog_utiming                   = PROG_UTIMING,
        .erase_utiming                  = ERASE_UTIMING,
    };
    struct lfs3_kiwibd_cfg *BENCH_BDCFG = &_bdcfg;
    #endif
#endif

