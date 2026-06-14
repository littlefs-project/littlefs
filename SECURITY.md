### littlefs/lfs.c invalid pointer dereference in lfs_dir_commitattr via lfs_migrate

#### Description:
A crafted littlefs v1 filesystem image passed to `lfs_migrate()` can drive
littlefs into an invalid metadata traversal state. Probably during migration, stale or
corrupted metadata can be interpreted as the reserved pseudo-tag
`LFS_FROM_USERATTRS`.

When this happens in `lfs_dir_traverse()`, the function can enter the
`LFS_FROM_USERATTRS` branch while its active `buffer` pointer still refers to a
stack-local `struct lfs_diskoff disk`, not to an array of `struct lfs_attr`.
The code then casts that stack object to `const struct lfs_attr *` and reads
attribute fields past the end of the stack object.

In the attached reproducer, this path is reached from the commit callback
flow. A fake `a[i].buffer` value is passed to `lfs_dir_commitattr()`, where it
is interpreted as `const struct lfs_diskoff *` and dereferenced. In a
release-style non-sanitized build this produces a native `SIGSEGV` at
`lfs.c:1651`.

#### Impact:
Potential attacker who can provide a filesystem image that is later passed to
`lfs_migrate()` can cause an out-of-bounds stack read and a process crash.
This is a denial-of-service issue for applications that migrate untrusted or
externally supplied littlefs v1 images.
The native crash was reproduced without Asan/UBSan. Asan additionally
reports the earlier stack out-of-bounds read in `lfs_dir_traverse()` at
`lfs.c:1059:32`.

#### Affected component:
- Project: `littlefs`
- Repository: `https://github.com/littlefs-project/littlefs`
- Tested commit: `6cb4e86540eca0d9ba62500a298385c9d863c8be`
- Entry point: `lfs_migrate()`
- Build condition: `LFS_MIGRATE` enabled
- Affected source locations:
  - `lfs.c:927`: `buffer` may refer to stack-local `struct lfs_diskoff disk`
  - `lfs.c:1056-1060`: `LFS_FROM_USERATTRS` casts `buffer` to `const struct lfs_attr *`
  - `lfs.c:1651`: release-style native `SIGSEGV` in `lfs_dir_commitattr()`
  - `lfs.c:5829`: migration commits attacker-influenced metadata

#### Proof-of-concept files:
Please attach the following files with this report:
- `poc3_lfs_dir_traverse_commit_userattrs.bin`
  - Size: `1662` bytes
  - SHA-256: `b82f5dacaaebee0bc0ce83285772002fe4bdeb4b3c17ec173a177014fbc2a40d`
- `fuzz_migrate.c`
  - SHA-256: `0a9508a9c6a289e87ccfc2a40c3aae0641cf736720307fb84db64361b910d03f`

The tested upstream checkout had no local modifications to tracked project
files (`git diff` and `git diff --cached` were empty). The fuzzer harness and
PoC are external reproducer artifacts.

#### To Reproduce:
Place `fuzz_migrate.c` and `poc3_lfs_dir_traverse_commit_userattrs.bin` in a
directory under the littlefs checkout, for example `fuzz/migrate/poc/`.

Build a release-style non-sanitized reproducer:
```bash
cd fuzz/migrate/poc ;
clang -I../../.. -DLFS_MIGRATE -DNDEBUG \
  -DLFS_NO_ERROR -DLFS_NO_WARN -DLFS_NO_DEBUG \
  -std=c99 -O2 -g -fno-omit-frame-pointer -fno-common \
  -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
  fuzz_migrate.c ../../../lfs.c ../../../lfs_util.c \
  -o migrate_fuzz_o2_ndebug -Wl,--gc-sections
```

Run the PoC:
```bash
./migrate_fuzz_o2_ndebug poc3_lfs_dir_traverse_commit_userattrs.bin
```

#### Asan reproduction:
Build an ASan reproducer:
```bash
cd fuzz/migrate/poc ;

clang -I../../.. -DLFS_MIGRATE \
  -DLFS_NO_ERROR -DLFS_NO_WARN -DLFS_NO_DEBUG \
  -std=c99 -O1 -g -fno-omit-frame-pointer -fno-common \
  -fsanitize=address,undefined \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
  fuzz_migrate.c ../../../lfs.c ../../../lfs_util.c \
  -o migrate_fuzz_asan
```

Run:
```bash
ASAN_OPTIONS=handle_abort=1:print_stacktrace=1 \
  ./migrate_fuzz_asan poc3_lfs_dir_traverse_commit_userattrs.bin
```

#### Asan output:
```bash
6640262d/littlefs/littlefs/fuzz/migrate/poc/poc3_lfs_dir_traverse_commit_userattrs.bin
../../lfs.c:1384:error: Corrupted dir pair at {0x1, 0x0}
=================================================================
==711778==ERROR: AddressSanitizer: stack-buffer-overflow on address 0x7b5d13201330 at pc 0x55e99cd17e63 bp 0x7fff23aa3890 sp 0x7fff23aa3888
READ of size 4 at 0x7b5d13201330 thread T0                                                                                                                  
    #0 0x55e99cd17e62 in lfs_dir_traverse /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:1059:32
    #1 0x55e99cd08f9d in lfs_dir_splittingcompact /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:2139:23
    #2 0x55e99cd08f9d in lfs_dir_relocatingcommit /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:2348:13
    #3 0x55e99cd032a6 in lfs_dir_orphaningcommit /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:2432:17
    #4 0x55e99ccf5271 in lfs_dir_commit /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:2604:19
    #5 0x55e99ccf5271 in lfs_migrate_ /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:5829:23
    #6 0x55e99ccf5271 in lfs_migrate /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:6551:11
    #7 0x55e99cce982b in run_one_input /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/poc/fuzz_migrate.c:306:9
    #8 0x55e99cce982b in LLVMFuzzerTestOneInput /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/poc/fuzz_migrate.c:336:5
    #9 0x55e99cce9ba0 in main /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/poc/fuzz_migrate.c:431:8
    #10 0x7f5d1518ef74 in __libc_start_call_main csu/../sysdeps/nptl/libc_start_call_main.h:58:16
    #11 0x7f5d1518f026 in __libc_start_main csu/../csu/libc-start.c:360:3
    #12 0x55e99cc0b340 in _start (/run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/build_asan/migrate_fuzz+0x50340) (BuildId: 301346b4f9742af4e7bdf7bd478503cd8146cbdc)

Address 0x7b5d13201330 is located in stack of thread T0 at offset 304 in frame
    #0 0x55e99cd15d77 in lfs_dir_traverse /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:917

  This frame has 3 object(s):
    [32, 208) 'stack' (line 920)
    [272, 276) 'tag' (line 925)
    [288, 296) 'disk' (line 927) <== Memory access at offset 304 overflows this variable
HINT: this may be a false positive if your program uses some custom stack unwind mechanism, swapcontext or vfork
      (longjmp and C++ exceptions *are* supported)
SUMMARY: AddressSanitizer: stack-buffer-overflow /run/media/user/8ed8205b-4114-4c2a-b2d0-e2ad6640262d/littlefs/littlefs/fuzz/migrate/../../lfs.c:1059:32 in lfs_dir_traverse
Shadow bytes around the buggy address:
  0x7b5d13201080: f8 f8 f8 f2 f2 f2 f2 f2 00 00 f2 f2 f8 f8 f2 f2
  0x7b5d13201100: 00 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3
  0x7b5d13201180: f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3
  0x7b5d13201200: f1 f1 f1 f1 00 00 00 00 00 00 00 00 00 00 00 00
  0x7b5d13201280: 00 00 00 00 00 00 00 00 00 00 f2 f2 f2 f2 f2 f2
=>0x7b5d13201300: f2 f2 04 f2 00 f3[f3]f3 f3 f3 f3 f3 f3 f3 f3 f3
  0x7b5d13201380: f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3 f3
  0x7b5d13201400: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x7b5d13201480: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x7b5d13201500: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x7b5d13201580: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
Shadow byte legend (one shadow byte represents 8 application bytes):
  Addressable:           00
  Partially addressable: 01 02 03 04 05 06 07 
  Heap left redzone:       fa
  Freed heap region:       fd
  Stack left redzone:      f1
  Stack mid redzone:       f2
  Stack right redzone:     f3
  Stack after return:      f5
  Stack use after scope:   f8
  Global redzone:          f9
  Global init order:       f6
  Poisoned by user:        f7
  Container overflow:      fc
  Array cookie:            ac
  Intra object redzone:    bb
  ASan internal:           fe
  Left alloca redzone:     ca
  Right alloca redzone:    cb
==711778==ABORTING
```

#### Environment:
OS: Linux rack1 6.19.14+kali-amd64 #1 SMP PREEMPT_DYNAMIC Kali 6.19.14-1+kali1 (2026-05-05) x86_64 GNU/Linux
Compiler: Debian clang version 21.1.8 (3+b1)
CPU: x86_64
ASan build opts: -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
Non-sanitized build opts: -O2 -g -DNDEBUG -DLFS_MIGRATE -DLFS_NO_ERROR -DLFS_NO_WARN -DLFS_NO_DEBUG

#### Notes:
With assertions enabled, this input may hit an earlier `LFS_ASSERT` and abort
with `SIGABRT`. The native `SIGSEGV` is reproducible in release-style builds
with assertions disabled through `-DNDEBUG`.

#### Suggested fix direction:
Do not allow on-disk/stale metadata tags to be interpreted as
`LFS_FROM_USERATTRS` unless `buffer` is known to point to a valid caller-owned
`struct lfs_attr` array. The `LFS_FROM_USERATTRS` pseudo-tag should be accepted
only from the synthetic attr traversal path, or the traversal state should
carry explicit provenance/type information for `buffer` before casting it.

#### Screenshots:

![screen](https://github.com/sigdevel/pocs/blob/main/res/littlefs/1/1_asan.png?raw=true "screen")
