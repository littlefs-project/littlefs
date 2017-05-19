## The little filesystem

A little fail-safe filesystem designed for embedded systems.

```
   | | |     .---._____
  .-----.   |          |
--|o    |---| littlefs |
--|     |---|          |
  '-----'   '----------'
   | | |
```

**Fail-safe** - The littlefs is designed to work consistently with random
power failures. During filesystem operations the storage on disk is always
kept in a valid state. The filesystem also has strong copy-on-write garuntees.
When updating a file, the original file will remain unmodified until the
file is closed, or sync is called.

**Wear awareness** - While the littlefs does not implement static wear
leveling, the littlefs takes into account write errors reported by the
underlying block device and uses a limited form of dynamic wear leveling
to manage blocks that go bad during the lifetime of the filesystem.

**Bounded ram/rom** - The littlefs is designed to work in a
limited amount of memory, recursion is avoided, and dynamic memory is kept
to a minimum. The littlefs allocates two fixed-size buffers for general
operations, and one fixed-size buffer per file. If there is only ever one file
in use, all memory can be provided statically and the littlefs can be used
in a system without dynamic memory.

## Example

Here's a simple example that updates a file named `boot_count` every time
main runs. The program can be interrupted at any time without losing track
of how many times it has been booted and without corrupting the filesystem:

``` c
#include "lfs.h"

// variables used by the filesystem
lfs_t lfs;
lfs_file_t file;

// configuration of the filesystem is provided by this struct
const struct lfs_config cfg = {
    // block device operations
    .read  = user_provided_block_device_read,
    .prog  = user_provided_block_device_prog,
    .erase = user_provided_block_device_erase,
    .sync  = user_provided_block_device_sync,

    // block device configuration
    .read_size = 16,
    .prog_size = 16,
    .block_size = 4096,
    .block_count = 128,
    .lookahead = 128,
};

// entry point
int main(void) {
    // mount the filesystem
    int err = lfs_mount(&lfs, &cfg);

    // reformat if we can't mount the filesystem
    // this should only happen on the first boot
    if (err) {
        lfs_format(&lfs, &cfg);
        lfs_mount(&lfs, &cfg);
    }

    // read current count
    uint32_t boot_count = 0;
    lfs_file_open(&lfs, &file, "boot_count", LFS_O_RDWR | LFS_O_CREAT);
    lfs_file_read(&lfs, &file, &boot_count, sizeof(boot_count));

    // update boot count
    boot_count += 1;
    printf("boot_count: %ld\n", boot_count);
    lfs_file_rewind(&lfs, &file);
    lfs_file_write(&lfs, &file, &boot_count, sizeof(boot_count));

    // remember the storage is not updated until the file is closed successfully
    lfs_file_close(&lfs, &file);

    // release any resources we were using
    lfs_unmount(&lfs);
}
```

## Usage

Detailed documentation (or at least as much detail as is currently available)
can be cound in the comments in [lfs.h](lfs.h).

As you may have noticed, the littlefs takes in a configuration structure that
defines how the filesystem operates. The configuration struct provides the
filesystem with the block device operations and dimensions, tweakable
parameters that tradeoff memory usage for performance, and optional
static buffers if the user wants to avoid dynamic memory.

The state of the littlefs is stored in the `lfs_t` type which is left up
to the user to allocate, allowing multiple filesystems to be in use
simultaneously. With the `lfs_t` and configuration struct, a user can either
format a block device or mount the filesystem.

Once mounted, the littlefs provides a full set of posix-like file and
directory functions, with the deviation that the allocation of filesystem
structures must be provided by the user. An important addition is that
no file updates will actually be written to disk until a sync or close
is called.

## Other notes

All littlefs have the potential to return a negative error code. The errors
can be either one of those found in the `enum lfs_error` in [lfs.h](lfs.h),
or an error returned by the user's block device operations.

It should also be noted that the littlefs does not do anything to insure
that the data written to disk is machine portable. It should be fine as
long as the machines involved share endianness and don't have really
strange padding requirements. If the question does come up, the littlefs
metadata should be stored on disk in little-endian format.

## Design

the littlefs was developed with the goal of learning more about filesystem
design by tackling the relative unsolved problem of managing a robust
filesystem resilient to power loss on devices with limited RAM and ROM.
More detail on the solutions and tradeoffs incorporated into this filesystem
can be found in [DESIGN.md](DESIGN.md).

## Testing

The littlefs comes with a test suite designed to run on a pc using the
[emulated block device](emubd/lfs_emubd.h) found in the emubd directory.
The tests assume a linux environment and can be started with make:

``` bash
make test
```
