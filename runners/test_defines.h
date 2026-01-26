// littlefs test runner defines


// preconfigured defines that control how tests run
#ifdef TEST_DEFINE
    //          name                    value (overridable)
    TEST_DEFINE(READ_SIZE,              1                                   )
    TEST_DEFINE(PROG_SIZE,              1                                   )
    TEST_DEFINE(BLOCK_SIZE,             4096                                )
    TEST_DEFINE(BLOCK_COUNT,            DISK_SIZE/BLOCK_SIZE                )
    TEST_DEFINE(DISK_SIZE,              1024*1024                           )
    TEST_DEFINE(BLOCK_RECYCLES,         -1                                  )
    TEST_DEFINE(RCACHE_SIZE,            LFS3_MAX(16, READ_SIZE)             )
    TEST_DEFINE(PCACHE_SIZE,            LFS3_MAX(16, PROG_SIZE)             )
    TEST_DEFINE(FCACHE_SIZE,            16                                  )
    TEST_DEFINE(LOOKAHEAD_SIZE,         16                                  )
    TEST_DEFINE(GC_FLAGS,               LFS3_GC_GC                          )
    TEST_DEFINE(GC_STEPS,               0                                   )
    TEST_DEFINE(GC_LOOKAHEAD_THRESH,    -1                                  )
    TEST_DEFINE(GC_LOOKGBMAP_THRESH,    -1                                  )
    TEST_DEFINE(GC_PREERASE_COUNT,      -1                                  )
    TEST_DEFINE(GC_COMPACT_THRESH,      0                                   )
    TEST_DEFINE(SHRUB_SIZE,             BLOCK_SIZE/4                        )
    TEST_DEFINE(FRAGMENT_SIZE,          LFS3_MIN(BLOCK_SIZE/8, 512)         )
    TEST_DEFINE(CRYSTAL_THRESH,         BLOCK_SIZE/8                        )
    TEST_DEFINE(LOOKGBMAP_THRESH,       BLOCK_COUNT/4                       )
    TEST_DEFINE(ERASE_VALUE,            0xff                                )
    #ifndef TEST_KIWIBD
    TEST_DEFINE(ERASE_CYCLES,           0                                   )
    TEST_DEFINE(BADBLOCK_BEHAVIOR,      LFS3_EMUBD_BADBLOCK_PROGERROR       )
    TEST_DEFINE(POWERLOSS_BEHAVIOR,     LFS3_EMUBD_POWERLOSS_ATOMIC         )
    TEST_DEFINE(BD_SEED,                0                                   )
    #endif
#endif


// struct lfs3_cfg fields
#ifdef TEST_CFG
    TEST_CFG(read_size,                 READ_SIZE                           )
    TEST_CFG(prog_size,                 PROG_SIZE                           )
    TEST_CFG(block_size,                BLOCK_SIZE                          )
    TEST_CFG(block_count,               BLOCK_COUNT                         )
    TEST_CFG(block_recycles,            BLOCK_RECYCLES                      )
    TEST_CFG(rcache_size,               RCACHE_SIZE                         )
    TEST_CFG(pcache_size,               PCACHE_SIZE                         )
    TEST_CFG(fcache_size,               FCACHE_SIZE                         )
    TEST_CFG(lookahead_size,            LOOKAHEAD_SIZE                      )
    #ifdef LFS3_GBMAP
    TEST_CFG(gc_lookgbmap_thresh,       GC_LOOKGBMAP_THRESH                 )
    TEST_CFG(lookgbmap_thresh,          LOOKGBMAP_THRESH                    )
    #endif
    #ifdef LFS3_PREERASE
    TEST_CFG(gc_preerase_count,         GC_PREERASE_COUNT                   )
    #endif
    #ifdef LFS3_GC
    TEST_CFG(gc_flags,                  GC_FLAGS                            )
    TEST_CFG(gc_steps,                  GC_STEPS                            )
    #endif
    TEST_CFG(gc_lookahead_thresh,       GC_LOOKAHEAD_THRESH                 )
    TEST_CFG(gc_compact_thresh,         GC_COMPACT_THRESH                   )
    TEST_CFG(shrub_size,                SHRUB_SIZE                          )
    TEST_CFG(fragment_size,             FRAGMENT_SIZE                       )
    TEST_CFG(crystal_thresh,            CRYSTAL_THRESH                      )
#endif


// struct lfs3_*bd_cfg fields
#ifdef TEST_BDCFG
    TEST_BDCFG(erase_value,             ERASE_VALUE                         )
    #ifndef TEST_KIWIBD
    TEST_BDCFG(erase_cycles,            ERASE_CYCLES                        )
    TEST_BDCFG(badblock_behavior,       BADBLOCK_BEHAVIOR                   )
    TEST_BDCFG(powerloss_behavior,      POWERLOSS_BEHAVIOR                  )
    TEST_BDCFG(seed,                    BD_SEED                             )
    #endif
#endif

