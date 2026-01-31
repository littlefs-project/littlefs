// littlefs bench runner defines


// preconfigured defines that control how benches run
#ifdef BENCH_DEFINE
    //          name                    value (overridable)
    BENCH_DEFINE(READ_SIZE,             1                                   )
    BENCH_DEFINE(PROG_SIZE,             1                                   )
    BENCH_DEFINE(BLOCK_SIZE,            4096                                )
    BENCH_DEFINE(BLOCK_COUNT,           DISK_SIZE/BLOCK_SIZE                )
    BENCH_DEFINE(DISK_SIZE,             1024*1024                           )
    BENCH_DEFINE(BLOCK_RECYCLES,        -1                                  )
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
    BENCH_DEFINE(FRAGMENT_SIZE,         LFS3_MIN(BLOCK_SIZE/8, 512)         )
    BENCH_DEFINE(CRYSTAL_THRESH,        BLOCK_SIZE/8                        )
    BENCH_DEFINE(LOOKGBMAP_THRESH,      BLOCK_COUNT/4                       )
    BENCH_DEFINE(ERASE_VALUE,           0xff                                )
    #ifndef BENCH_NAND
    // default timings for NOR flash, based on w25q64jv:
    // https://www.winbond.com/resource-files/
    //         W25Q64JV%20RevM%2012242024%20Plus.pdf
    //
    // note one thing unique to NOR flash is the extreme erase cost
    //
    // FR=104 MHz, quad prog (9.6 ns * 8/4)
    // => +~19 ns for bus (not read!)
    //
    // simple:
    // readed=40ns/B fR=50 MHz, quad read (20 ns * 8/4)
    // progged=1582ns/B tPP=0.4 ms, page=256 (0.4 ms / 256 + bus)
    // erased=10986ns/B tSE=45 ms, sector=4096 (45 ms / 4096)
    //
    // less-simple:
    // reads=0ns (no transaction cost)
    // progs=400000ns tPP=0.4 ms, page=256
    // erases=0ns (no transaction cost)
    // readed=40ns/B fR=50 MHz, quad read (20 ns * 8/4)
    // progged=1484ns/B tPP=0.4 ms (((4096/256)*0.4ms - 0.4ms)/4096 + bus)
    // erased=10986ns/B tSE=45 ms, sector=4096 (45 ms / 4096)
    //
    // note we always treat erases as per-byte to simplify benchmarking
    // across different block sizes
    //
    #ifdef BENCH_SIMPLE
    BENCH_DEFINE(READS_TIMING,          0                                   )
    BENCH_DEFINE(PROGS_TIMING,          0                                   )
    BENCH_DEFINE(ERASES_TIMING,         0                                   )
    BENCH_DEFINE(READED_TIMING,         40                                  )
    BENCH_DEFINE(PROGGED_TIMING,        1582                                )
    BENCH_DEFINE(ERASED_TIMING,         10986                               )
    #else
    BENCH_DEFINE(READS_TIMING,          0                                   )
    BENCH_DEFINE(PROGS_TIMING,          400000                              )
    BENCH_DEFINE(ERASES_TIMING,         0                                   )
    BENCH_DEFINE(READED_TIMING,         40                                  )
    BENCH_DEFINE(PROGGED_TIMING,        1484                                )
    BENCH_DEFINE(ERASED_TIMING,         10986                               )
    #endif
    #else
    // default timings for NAND flash, based on w25n01gv:
    // https://www.winbond.com/resource-files/W25N01GV%20Rev%20R%20070323.pdf
    //
    // FR=104 MHz, quad read/prog (9.6 ns * 8/4)
    // => +~19 ns for bus
    //
    // simple:
    // readed=31ns/B tRD1=25 us, p=2048, s=512 (25 us / 2048 + bus)
    // progged=141ns/B tPP=250 us, p=2048, s=512 (250 us / 2048 + bus)
    // erased=15ns/B tBE=2 ms, block=131072 (2 ms / 131072)
    //
    // less-simple:
    // reads=25000ns tRD1=25 us, p=2048, s=512
    // progs=250000ns tPP=250 us, p=2048, s=512
    // erases=0ns (no transaction cost)
    // readed=31ns/B tRD1=25 us (((131072/2048)*25us - 25us)/131072 + bus)
    // progged=139ns/B tPP=250 us (((131072/2048)*250us - 250us)/131072 + bus)
    // erased=15ns/B tBE=2 ms, block=131072 (2 ms / 131072)
    //
    // note we always treat erases as per-byte to simplify benchmarking
    // across different block sizes
    //
    #ifdef BENCH_SIMPLE
    BENCH_DEFINE(READS_TIMING,          0                                   )
    BENCH_DEFINE(PROGS_TIMING,          0                                   )
    BENCH_DEFINE(ERASES_TIMING,         0                                   )
    BENCH_DEFINE(READED_TIMING,         31                                  )
    BENCH_DEFINE(PROGGED_TIMING,        141                                 )
    BENCH_DEFINE(ERASED_TIMING,         15                                  )
    #else
    BENCH_DEFINE(READS_TIMING,          25000                               )
    BENCH_DEFINE(PROGS_TIMING,          250000                              )
    BENCH_DEFINE(ERASES_TIMING,         0                                   )
    BENCH_DEFINE(READED_TIMING,         31                                  )
    BENCH_DEFINE(PROGGED_TIMING,        139                                 )
    BENCH_DEFINE(ERASED_TIMING,         15                                  )
    #endif
    #endif
    #ifndef BENCH_KIWIBD
    BENCH_DEFINE(ERASE_CYCLES,          0                                   )
    BENCH_DEFINE(BADBLOCK_BEHAVIOR,     LFS3_EMUBD_BADBLOCK_PROGERROR       )
    BENCH_DEFINE(POWERLOSS_BEHAVIOR,    LFS3_EMUBD_POWERLOSS_ATOMIC         )
    BENCH_DEFINE(BD_SEED,               0                                   )
    #endif
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
        .reads_timing                   = READS_TIMING,
        .progs_timing                   = PROGS_TIMING,
        .erases_timing                  = ERASES_TIMING,
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
        .reads_timing                   = READS_TIMING,
        .progs_timing                   = PROGS_TIMING,
        .erases_timing                  = ERASES_TIMING,
        .readed_timing                  = READED_TIMING,
        .progged_timing                 = PROGGED_TIMING,
        .erased_timing                  = ERASED_TIMING,
    };
    struct lfs3_kiwibd_cfg *BENCH_BDCFG = &_bdcfg;
    #endif
#endif

