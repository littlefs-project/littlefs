// littlefs bench runner defines


#ifdef BENCH_INCLUDE
    #ifndef BENCH_DEFINES_H
    #define BENCH_DEFINES_H

    // DISK_GEOMETRY controls which simulation we use
    // 0 => NOR flash (the default)
    // 1 => NAND flash
    #define DISK_MAP(define) \
            ((DISK_GEOMETRY == 0) ? NOR_##define \
                                  : NAND_##define)

    #endif
#endif

// preconfigured defines that control how benches run
#ifdef BENCH_DEFINE
    //           name                   value (overridable)
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
    BENCH_DEFINE(GC_FLAGS,              LFS3_GC_GC                          )
    BENCH_DEFINE(GC_STEPS,              0                                   )
    BENCH_DEFINE(GC_LOOKAHEAD_THRESH,   -1                                  )
    BENCH_DEFINE(GC_LOOKGBMAP_THRESH,   -1                                  )
    BENCH_DEFINE(GC_PREERASE_COUNT,     -1                                  )
    BENCH_DEFINE(GC_COMPACT_THRESH,     0                                   )
    BENCH_DEFINE(SHRUB_SIZE,            BLOCK_SIZE/4                        )
    BENCH_DEFINE(FRAGMENT_SIZE,         LFS3_MIN(BLOCK_SIZE/16, 512)        )
    BENCH_DEFINE(CRYSTAL_THRESH,        BLOCK_SIZE/16                       )
    BENCH_DEFINE(LOOKGBMAP_THRESH,      BLOCK_COUNT/4                       )
    // don't bother simulating erases, this may be less realistic, but
    // it's certainly faster!
    BENCH_DEFINE(ERASE_VALUE,           -1                                  )
    BENCH_DEFINE(READ_WIDTH,            DISK_MAP(READ_WIDTH)                )
    BENCH_DEFINE(PROG_WIDTH,            DISK_MAP(PROG_WIDTH)                )
    BENCH_DEFINE(ERASE_WIDTH,           DISK_MAP(ERASE_WIDTH)               )
    BENCH_DEFINE(READ_TIMING,           DISK_MAP(READ_TIMING)               )
    BENCH_DEFINE(PROG_TIMING,           DISK_MAP(PROG_TIMING)               )
    BENCH_DEFINE(ERASE_TIMING,          DISK_MAP(ERASE_TIMING)              )
    BENCH_DEFINE(READED_TIMING,         DISK_MAP(READED_TIMING)             )
    BENCH_DEFINE(PROGGED_TIMING,        DISK_MAP(PROGGED_TIMING)            )
    BENCH_DEFINE(ERASED_TIMING,         DISK_MAP(ERASED_TIMING)             )

    // NOR flash (DISK_GEOMETRY=0)
    //
    // based on w25q64jv:
    // https://www.winbond.com/resource-files/
    //         W25Q64JV%20RevM%2012242024%20Plus.pdf
    //
    // note one thing unique to NOR flash is the extreme erase cost
    //
    // FR=104 MHz, quad prog (9.6 ns * 8/4)
    // => +~19 ns for bus (not read!)
    //
    // simple per-byte sim:
    // readed=40ns/B fR=50 MHz, quad read (20 ns * 8/4)
    // progged=1582ns/B tPP=0.4 ms, page=256 (0.4 ms / 256 + bus)
    // erased=10986ns/B tSE=45 ms, sector=4096 (45 ms / 4096)
    //
    // less-simple bus+buffer sim:
    // read=0ns/B (no transaction cost)
    // prog=1563ns/B tPP=0.4 ms, page=256 (0.4 ms / 256)
    // erase=10986ns/B tSE=45 ms, sector=4096 (45 ms / 4096)
    // readed=40ns/B fR=50 MHz, quad read (20 ns * 8/4)
    // progged=19ns/B (bus)
    // erased=0ns/B (no bus cost)
    //
    BENCH_DEFINE(NOR_READ_SIZE,         1                                   )
    BENCH_DEFINE(NOR_PROG_SIZE,         1                                   )
    BENCH_DEFINE(NOR_ERASE_SIZE,        4096                                )
    BENCH_DEFINE(NOR_READ_WIDTH,        (DISK_SIM == 0)
                                            ? 1
                                            : BLOCK_SIZE                    )
    BENCH_DEFINE(NOR_PROG_WIDTH,        (DISK_SIM == 0)
                                            ? LFS3_MIN(256, BLOCK_SIZE)
                                            : BLOCK_SIZE                    )
    BENCH_DEFINE(NOR_ERASE_WIDTH,       (DISK_SIM == 0)
                                            ? LFS3_MIN(ERASE_SIZE, BLOCK_SIZE)
                                            : BLOCK_SIZE                    )
    BENCH_DEFINE(NOR_READ_TIMING,       (DISK_SIM == 0) ? 0     : 0         )
    BENCH_DEFINE(NOR_PROG_TIMING,       (DISK_SIM == 0) ? 1563  : 0         )
    BENCH_DEFINE(NOR_ERASE_TIMING,      (DISK_SIM == 0) ? 10986 : 0         )
    BENCH_DEFINE(NOR_READED_TIMING,     (DISK_SIM == 0) ? 40    : 40        )
    BENCH_DEFINE(NOR_PROGGED_TIMING,    (DISK_SIM == 0) ? 19    : 1582      )
    BENCH_DEFINE(NOR_ERASED_TIMING,     (DISK_SIM == 0) ? 0     : 10986     )

    // NAND flash (DISK_GEOMETRY=1)
    //
    // based on w25n01gv:
    // https://www.winbond.com/resource-files/W25N01GV%20Rev%20R%20070323.pdf
    //
    // FR=104 MHz, quad read/prog (9.6 ns * 8/4)
    // => +~19 ns for bus
    //
    // simple per-byte sim:
    // readed=31ns/B tRD1=25 us, p=2048, s=512 (25 us / 2048 + bus)
    // progged=141ns/B tPP=250 us, p=2048, s=512 (250 us / 2048 + bus)
    // erased=15ns/B tBE=2 ms, block=131072 (2 ms / 131072)
    //
    // less-simple bus+buffer sim:
    // read=12ns/B tRD1=25 us, p=2048, s=512 (25 us / 2048)
    // prog=122ns/B tPP=250 us, p=2048, s=512 (250 us / 2048)
    // erase=15ns/B tBE=2 ms, block=131072 (2 ms / 131072)
    // readed=19ns/B (bus)
    // progged=19ns/B (bus)
    // erased=0ns/B (no bus cost)
    //
    BENCH_DEFINE(NAND_READ_SIZE,        1                                   )
    BENCH_DEFINE(NAND_PROG_SIZE,        512                                 )
    BENCH_DEFINE(NAND_ERASE_SIZE,       131072                              )
    BENCH_DEFINE(NAND_READ_WIDTH,       (DISK_SIM == 0)
                                            ? LFS3_MIN(2048, BLOCK_SIZE)
                                            : BLOCK_SIZE                    )
    BENCH_DEFINE(NAND_PROG_WIDTH,       (DISK_SIM == 0)
                                            ? LFS3_MIN(2048, BLOCK_SIZE)
                                            : BLOCK_SIZE                    )
    BENCH_DEFINE(NAND_ERASE_WIDTH,      (DISK_SIM == 0)
                                            ? LFS3_MIN(ERASE_SIZE, BLOCK_SIZE)
                                            : BLOCK_SIZE                    )
    BENCH_DEFINE(NAND_READ_TIMING,      (DISK_SIM == 0) ? 12  : 0           )
    BENCH_DEFINE(NAND_PROG_TIMING,      (DISK_SIM == 0) ? 122 : 0           )
    BENCH_DEFINE(NAND_ERASE_TIMING,     (DISK_SIM == 0) ? 15  : 0           )
    BENCH_DEFINE(NAND_READED_TIMING,    (DISK_SIM == 0) ? 19  : 31          )
    BENCH_DEFINE(NAND_PROGGED_TIMING,   (DISK_SIM == 0) ? 19  : 141         )
    BENCH_DEFINE(NAND_ERASED_TIMING,    (DISK_SIM == 0) ? 0   : 15          )
#endif


// struct lfs3_cfg definition
#ifdef BENCH_CFG
    struct lfs3_cfg _cfg = {
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
        .gc_lookgbmap_thresh            = GC_LOOKGBMAP_THRESH,
        .lookgbmap_thresh               = LOOKGBMAP_THRESH,
        #endif
        #ifdef LFS3_PREERASE
        .gc_preerase_count              = GC_PREERASE_COUNT,
        #endif
        #ifdef LFS3_GC
        .gc_flags                       = GC_FLAGS,
        .gc_steps                       = GC_STEPS,
        #endif
        .gc_lookahead_thresh            = GC_LOOKAHEAD_THRESH,
        .gc_compact_thresh              = GC_COMPACT_THRESH,
        .shrub_size                     = SHRUB_SIZE,
        .fragment_size                  = FRAGMENT_SIZE,
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
        .readed_timing                  = READED_TIMING,
        .progged_timing                 = PROGGED_TIMING,
        .erased_timing                  = ERASED_TIMING,
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
        .readed_timing                  = READED_TIMING,
        .progged_timing                 = PROGGED_TIMING,
        .erased_timing                  = ERASED_TIMING,
    };
    struct lfs3_kiwibd_cfg *BENCH_BDCFG = &_bdcfg;
    #endif
#endif

