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
    BENCH_DEFINE(ERASE_CYCLES,          0                                   )
    BENCH_DEFINE(BADBLOCK_BEHAVIOR,     LFS3_EMUBD_BADBLOCK_PROGERROR       )
    BENCH_DEFINE(POWERLOSS_BEHAVIOR,    LFS3_EMUBD_POWERLOSS_ATOMIC         )
    BENCH_DEFINE(EMUBD_SEED,            0                                   )
#endif


// struct lfs3_cfg fields
#ifdef BENCH_CFG
    BENCH_CFG(read_size,                READ_SIZE                           )
    BENCH_CFG(prog_size,                PROG_SIZE                           )
    BENCH_CFG(block_size,               BLOCK_SIZE                          )
    BENCH_CFG(block_count,              BLOCK_COUNT                         )
    BENCH_CFG(block_recycles,           BLOCK_RECYCLES                      )
    BENCH_CFG(rcache_size,              RCACHE_SIZE                         )
    BENCH_CFG(pcache_size,              PCACHE_SIZE                         )
    BENCH_CFG(fcache_size,              FCACHE_SIZE                         )
    BENCH_CFG(lookahead_size,           LOOKAHEAD_SIZE                      )
    #ifdef LFS3_GBMAP
    BENCH_CFG(gc_lookgbmap_thresh,      GC_LOOKGBMAP_THRESH                 )
    BENCH_CFG(lookgbmap_thresh,         LOOKGBMAP_THRESH                    )
    #endif
    #ifdef LFS3_PREERASE
    BENCH_CFG(gc_preerase_count,        GC_PREERASE_COUNT                   )
    #endif
    #ifdef LFS3_GC
    BENCH_CFG(gc_flags,                 GC_FLAGS                            )
    BENCH_CFG(gc_steps,                 GC_STEPS                            )
    #endif
    BENCH_CFG(gc_lookahead_thresh,      GC_LOOKAHEAD_THRESH                 )
    BENCH_CFG(gc_compact_thresh,        GC_COMPACT_THRESH                   )
    BENCH_CFG(shrub_size,               SHRUB_SIZE                          )
    BENCH_CFG(fragment_size,            FRAGMENT_SIZE                       )
    BENCH_CFG(crystal_thresh,           CRYSTAL_THRESH                      )
#endif


// struct lfs3_*bd_cfg fields
#ifdef BENCH_BDCFG
    BENCH_BDCFG(erase_value,            ERASE_VALUE                         )
    BENCH_BDCFG(erase_cycles,           ERASE_CYCLES                        )
    BENCH_BDCFG(badblock_behavior,      BADBLOCK_BEHAVIOR                   )
    BENCH_BDCFG(powerloss_behavior,     POWERLOSS_BEHAVIOR                  )
    BENCH_BDCFG(seed,                   EMUBD_SEED                          )
#endif

