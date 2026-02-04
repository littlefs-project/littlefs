// littlefs test runner defines


// preconfigured defines that control how tests run
#ifdef TEST_DEFINE
    //          name                    value (overridable)
    TEST_DEFINE(DISK_SIZE,              1024*1024                           )
    TEST_DEFINE(DISK_GEOMETRY,          0                                   )
    TEST_DEFINE(READ_SIZE,              1                                   )
    TEST_DEFINE(PROG_SIZE,              1                                   )
    TEST_DEFINE(ERASE_SIZE,             4096                                )
    TEST_DEFINE(BLOCK_SIZE,             LFS3_MAX(ERASE_SIZE, 512)           )
    TEST_DEFINE(BLOCK_COUNT,            DISK_SIZE/LFS3_MAX(BLOCK_SIZE, 1)   )
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
    TEST_DEFINE(FRAGMENT_SIZE,          LFS3_MIN(BLOCK_SIZE/16, 512)        )
    TEST_DEFINE(CRYSTAL_THRESH,         BLOCK_SIZE/16                       )
    TEST_DEFINE(LOOKGBMAP_THRESH,       BLOCK_COUNT/4                       )
    TEST_DEFINE(ERASE_VALUE,            0xff                                )
    #ifndef TEST_KIWIBD
    TEST_DEFINE(ERASE_CYCLES,           0                                   )
    TEST_DEFINE(BADBLOCK_BEHAVIOR,      LFS3_EMUBD_BADBLOCK_PROGERROR       )
    TEST_DEFINE(POWERLOSS_BEHAVIOR,     LFS3_EMUBD_POWERLOSS_ATOMIC         )
    TEST_DEFINE(BD_SEED,                0                                   )
    #endif
#endif


// struct lfs3_cfg definition
#ifdef TEST_CFG
    struct lfs3_cfg _cfg = {
        #ifdef TEST_CFG_CFG
        TEST_CFG_CFG
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
    struct lfs3_cfg *TEST_CFG = &_cfg;
#endif


// struct lfs3_*bd_cfg definition
#ifdef TEST_BDCFG
    #ifndef TEST_KIWIBD
    struct lfs3_emubd_cfg _bdcfg = {
        #ifdef TEST_BDCFG_CFG
        TEST_BDCFG_CFG
        #endif
        .erase_value                    = ERASE_VALUE,
        .erase_cycles                   = ERASE_CYCLES,
        .badblock_behavior              = BADBLOCK_BEHAVIOR,
        .powerloss_behavior             = POWERLOSS_BEHAVIOR,
        .seed                           = BD_SEED,
    };
    struct lfs3_emubd_cfg *TEST_BDCFG = &_bdcfg;
    #else
    struct lfs3_kiwibd_cfg _bdcfg = {
        #ifdef TEST_BDCFG_CFG
        TEST_BDCFG_CFG
        #endif
        .erase_value                    = ERASE_VALUE,
    };
    struct lfs3_kiwibd_cfg *TEST_BDCFG = &_bdcfg;
    #endif
#endif

