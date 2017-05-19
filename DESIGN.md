## The design of the little filesystem

The littlefs is a little fail-safe filesystem designed for embedded systems.

```
   | | |     .---._____
  .-----.   |          |
--|o    |---| littlefs |
--|     |---|          |
  '-----'   '----------'
   | | |
```

For a bit of backstory, the littlefs was developed with the goal of learning
more about filesystem design by tackling the relative unsolved problem of
managing a robust filesystem resilient to power loss on devices
with limited RAM and ROM.

The embedded systems the littlefs is targeting are usually 32bit
microcontrollers with around 32Kbytes of RAM and 512Kbytes of ROM. These are
often paired with SPI NOR flash chips with about 4Mbytes of flash storage.

Flash itself is a very interesting piece of technology with quite a bit of
nuance. Unlike most other forms of storage, writing to flash requires two
operations: erasing and programming. The programming operation is relatively
cheap, and can be very granular. For NOR flash specifically, byte-level
programs are quite common. Erasing, however, requires an expensive operation
that forces the state of large blocks of memory to reset in a destructive
reaction that gives flash its name. The [Wikipedia entry](https://en.wikipedia.org/wiki/Flash_memory)
has more information if you are interesting in how this works.

This leaves us with an interesting set of limitations that can be simplified
to three strong requirements:

1. **Fail-safe** - This is actually the main goal of the littlefs and the focus
   of this project. Embedded systems are usually designed without a shutdown
   routine and a notable lack of user interface for recovery, so filesystems
   targeting embedded systems should be prepared to lose power an any given
   time.

   Despite this state of things, there are very few embedded filesystems that
   handle power loss in a reasonable manner, and can become corrupted if the
   user is unlucky enough.

2. **Wear awareness** - Due to the destructive nature of flash, most flash
   chips have a limited number of erase cycles, usually in the order of around
   100,000 erases per block for NOR flash. Filesystems that don't take wear
   into account can easily burn through blocks used to store frequently updated
   metadata.

   Consider the [FAT filesystem](https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system),
   which stores a file allocation table (FAT) at a specific offset from the
   beginning of disk. Every block allocation will update this table, and after
   100,000 updates, the block will likely go bad, rendering the filesystem
   unusable even if there are many more erase cycles available on the storage.

3. **Bounded RAM/ROM** - Even with the design difficulties presented by the
   previous two limitations, we have already seen several flash filesystems
   developed on PCs that handle power loss just fine, such as the
   logging filesystems. However, these filesystems take advantage of the
   relatively cheap access to RAM, and use some rather... opportunistic...
   techniques, such as reconstructing the entire directory structure in RAM.
   These operations make perfect sense when the filesystem's only concern is
   erase cycles, but the idea is a bit silly on embedded systems.

   To cater to embedded systems, the littlefs has the simple limitation of
   using only a bounded amount of RAM and ROM. That is, no matter what is
   written to the filesystem, and no matter how large the underlying storage
   is, the littlefs will always use the same amount of RAM and ROM. This
   presents a very unique challenge, and makes presumably simple operations,
   such as iterating through the directory tree, surprisingly difficult.

## Existing designs?

There are of course, many different existing filesystem. Heres a very rough
summary of the general ideas behind some of them.

Most of the existing filesystems fall into the one big category of filesystem
designed in the early days of spinny magnet disks. While there is a vast amount
of interesting technology and ideas in this area, the nature of spinny magnet
disks encourage properties such as grouping writes near each other, that don't
make as much sense on recent storage types. For instance, on flash, write
locality is not as important and can actually increase wear destructively.

One of the most popular designs for flash filesystems is called the
[logging filesystem](https://en.wikipedia.org/wiki/Log-structured_file_system).
The flash filesystems [jffs](https://en.wikipedia.org/wiki/JFFS)
and [yaffs](https://en.wikipedia.org/wiki/YAFFS) are good examples. In
logging filesystem, data is not store in a data structure on disk, but instead
the changes to the files are stored on disk. This has several neat advantages,
such as the fact that the data is written in a cyclic log format naturally
wear levels as a side effect. And, with a bit of error detection, the entire
filesystem can easily be designed to be resilient to power loss. The
journalling component of most modern day filesystems is actually a reduced
form of a logging filesystem. However, logging filesystems have a difficulty
scaling as the size of storage increases. And most filesystems compensate by
caching large parts of the filesystem in RAM, a strategy that is unavailable
for embedded systems.

Another interesting filesystem design technique that the littlefs borrows the
most from, is the [copy-on-write (COW)](https://en.wikipedia.org/wiki/Copy-on-write).
A good example of this is the [btrfs](https://en.wikipedia.org/wiki/Btrfs)
filesystem. COW filesystems can easily recover from corrupted blocks and have
natural protection against power loss. However, if they are not designed with
wear in mind, a COW filesystem could unintentionally wear down the root block
where the COW data structures are synchronized.

## Metadata pairs

The core piece of technology that provides the backbone for the littlefs is
the concept of metadata pairs. The key idea here, is that any metadata that
needs to be updated atomically is stored on a pair of blocks tagged with
a revision count and checksum. Every update alternates between these two
pairs, so that at any time there is always a backup containing the previous
state of the metadata.

Consider a small example where each metadata pair has a revision count,
a number as data, and the xor of the block as a quick checksum. If
we update the data to a value of 9, and then to a value of 5, here is
what the pair of blocks may look like after each update:
```
  block 1   block 2        block 1   block 2        block 1   block 2
.---------.---------.    .---------.---------.    .---------.---------.
| rev: 1  | rev: 0  |    | rev: 1  | rev: 2  |    | rev: 3  | rev: 2  |
| data: 3 | data: 0 | -> | data: 3 | data: 9 | -> | data: 5 | data: 9 |
| xor: 2  | xor: 0  |    | xor: 2  | xor: 11 |    | xor: 6  | xor: 11 |
'---------'---------'    '---------'---------'    '---------'---------'
                 let data = 9             let data = 5
```

After each update, we can find the most up to date value of data by looking
at the revision count.

Now consider what the blocks may look like if we suddenly loss power while
changing the value of data to 5:
```
  block 1   block 2        block 1   block 2        block 1   block 2
.---------.---------.    .---------.---------.    .---------.---------.
| rev: 1  | rev: 0  |    | rev: 1  | rev: 2  |    | rev: 3  | rev: 2  |
| data: 3 | data: 0 | -> | data: 3 | data: 9 | -x | data: 3 | data: 9 |
| xor: 2  | xor: 0  |    | xor: 2  | xor: 11 |    | xor: 2  | xor: 11 |
'---------'---------'    '---------'---------'    '---------'---------'
                 let data = 9             let data = 5
                                          powerloss!!!
```

In this case, block 1 was partially written with a new revision count, but
the littlefs hadn't made it to updating the value of data. However, if we
check our checksum we notice that block 1 was corrupted. So we fall back to
block 2 and use the value 9.

Using this concept, the littlefs is able to update metadata blocks atomically.
There are a few other tweaks, such as using a 32bit crc and using sequence
arithmetic to handle revision count overflow, but the basic concept
is the same. These metadata pairs define the backbone of the littlefs, and the
rest of the filesystem is built on top of these atomic updates.

## Files

Now, the metadata pairs do come with some drawbacks. Most notably, each pair
requires two blocks for each block of data. I'm sure users would be very
unhappy if their storage was suddenly cut in half! Instead of storing
everything in these metadata blocks, the littlefs uses a COW data structure
for files which is in turn pointed to by a metadata block. When
we update a file, we create a copies of any blocks that are modified until
the metadata blocks are updated with the new copy. Once the metadata block
points to the new copy, we deallocate the old blocks that are no longer in use.

Here is what updating a one-block file may look like:
```
  block 1   block 2        block 1   block 2        block 1   block 2
.---------.---------.    .---------.---------.    .---------.---------.
| rev: 1  | rev: 0  |    | rev: 1  | rev: 0  |    | rev: 1  | rev: 2  |
| file: 4 | file: 0 | -> | file: 4 | file: 0 | -> | file: 4 | file: 5 |
| xor: 5  | xor: 0  |    | xor: 5  | xor: 0  |    | xor: 5  | xor: 7  |
'---------'---------'    '---------'---------'    '---------'---------'
    |                        |                                  |
    v                        v                                  v
 block 4                  block 4    block 5       block 4    block 5
.--------.               .--------. .--------.    .--------. .--------.
| old    |               | old    | | new    |    | old    | | new    |
| data   |               | data   | | data   |    | data   | | data   |
|        |               |        | |        |    |        | |        |
'--------'               '--------' '--------'    '--------' '--------'
            update data in file        update metadata pair
```

It doesn't matter if we lose power while writing block 5 with the new data,
since the old data remains unmodified in block 4. This example also
highlights how the atomic updates of the metadata blockss provide a
synchronization barrier for the rest of the littlefs.

At this point, it may look like we are wasting an awfully large amount
of space on the metadata. Just looking at that example, we are using
three blocks to represent a file that fits comfortably in one! So instead
of giving each file a metadata pair, we actually store the metadata for
all files contained in a single directory in a single metadata block.

Now we could just leave files here, copying the entire file on write
provides the synchronization without the duplicated memory requirements
of the metadata blocks. However, we can do a bit better.

## CTZ linked-lists

There are many different data structures for representing the actual
files in filesystems. Of these, the littlefs uses a rather unique [COW](https://upload.wikimedia.org/wikipedia/commons/0/0c/Cow_female_black_white.jpg)
data structure that allows the filesystem to reuse unmodified parts of the
file without additional metadata pairs.

First lets consider storing files in a simple linked-list. What happens when
append a block? We have to change the last block in the linked-list to point
to this new block, which means we have to copy out the last block, and change
the second-to-last block, and then the third-to-last, and so on until we've
copied out the entire file.

```
Exhibit A: A linked-list
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |->| data 1 |->| data 2 |->| data 4 |->| data 5 |->| data 6 |
|        |  |        |  |        |  |        |  |        |  |        |
|        |  |        |  |        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```

To get around this, the littlefs, at its heart, stores files backwards. Each
block points to its predecessor, with the first block containing no pointers.
If you think about this, it makes a bit of sense. Appending blocks just point
to their predecessor and no other blocks need to be updated. If we update
a block in the middle, we will need to copy out the blocks that follow,
but can reuse the blocks before the modified block. Since most file operations
either reset the file each write or append to files, this design avoids
copying the file in the most common cases.

```
Exhibit B: A backwards linked-list
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |<-| data 1 |<-| data 2 |<-| data 4 |<-| data 5 |<-| data 6 |
|        |  |        |  |        |  |        |  |        |  |        |
|        |  |        |  |        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```

However, a backwards linked-list does come with a rather glaring problem.
Iterating over a file _in order_ has a runtime of O(n^2). Gah! A quadratic
runtime to just _read_ a file? That's awful. Keep in mind reading files are
usually the most common filesystem operation.

To avoid this problem, the littlefs uses a multilayered linked-list. For
every block that is divisible by a power of two, the block contains an
additional pointer that points back by that power of two. Another way of
thinking about this design is that there are actually many linked-lists
threaded together, with each linked-lists skipping an increasing number
of blocks. If you're familiar with data-structures, you may have also
recognized that this is a deterministic skip-list.

To find the power of two factors efficiently, we can use the instruction
[count trailing zeros (CTZ)](https://en.wikipedia.org/wiki/Count_trailing_zeros),
which is where this linked-list's name comes from.

```
Exhibit C: A backwards CTZ linked-list
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |<-| data 1 |<-| data 2 |<-| data 3 |<-| data 4 |<-| data 5 |
|        |<-|        |--|        |<-|        |--|        |  |        |
|        |<-|        |--|        |--|        |--|        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```

Taking exhibit C for example, here is the path from data block 5 to data
block 1. You can see how data block 3 was completely skipped:
```
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |  | data 1 |<-| data 2 |  | data 3 |  | data 4 |<-| data 5 |
|        |  |        |  |        |<-|        |--|        |  |        |
|        |  |        |  |        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```

The path to data block 0 is even more quick, requiring only two jumps:
```
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |  | data 1 |  | data 2 |  | data 3 |  | data 4 |<-| data 5 |
|        |  |        |  |        |  |        |  |        |  |        |
|        |<-|        |--|        |--|        |--|        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```

The CTZ linked-list has quite a few interesting properties. All of the pointers
in the block can be found by just knowing the index in the list of the current
block, and, with a bit of math, the amortized overhead for the linked-list is
only two pointers per block.  Most importantly, the CTZ linked-list has a
worst case lookup runtime of O(logn), which brings the runtime of reading a
file down to O(n logn). Given that the constant runtime is divided by the
amount of data we can store in a block, this is pretty reasonable.

Here is what it might look like to update a file stored with a CTZ linked-list:
```
                                      block 1   block 2
                                    .---------.---------.
                                    | rev: 1  | rev: 0  |
                                    | file: 6 | file: 0 |
                                    | size: 4 | xor: 0  |
                                    | xor: 3  | xor: 0  |
                                    '---------'---------'
                                        |
                                        v
  block 3     block 4     block 5     block 6
.--------.  .--------.  .--------.  .--------.
| data 0 |<-| data 1 |<-| data 2 |<-| data 3 |
|        |<-|        |--|        |  |        |
|        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'

|  update data in file
v

                                      block 1   block 2
                                    .---------.---------.
                                    | rev: 1  | rev: 0  |
                                    | file: 6 | file: 0 |
                                    | size: 4 | size: 0 |
                                    | xor: 3  | xor: 0  |
                                    '---------'---------'
                                        |
                                        v
  block 3     block 4     block 5     block 6
.--------.  .--------.  .--------.  .--------.
| data 0 |<-| data 1 |<-| old    |<-| old    |
|        |<-|        |--| data 2 |  | data 3 |
|        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'
     ^ ^           ^
     | |           |      block 7     block 8     block 9    block 10
     | |           |    .--------.  .--------.  .--------.  .--------.
     | |           '----| new    |<-| new    |<-| new    |<-| new    |
     | '----------------| data 2 |<-| data 3 |--| data 4 |  | data 5 |
     '------------------|        |--|        |--|        |  |        |
                        '--------'  '--------'  '--------'  '--------'

|  update metadata pair
v

                                                   block 1   block 2
                                                 .---------.---------.
                                                 | rev: 1  | rev: 2  |
                                                 | file: 6 | file: 10|
                                                 | size: 4 | size: 6 |
                                                 | xor: 3  | xor: 14 |
                                                 '---------'---------'
                                                                |
                                                                |
  block 3     block 4     block 5     block 6                   |
.--------.  .--------.  .--------.  .--------.                  |
| data 0 |<-| data 1 |<-| old    |<-| old    |                  |
|        |<-|        |--| data 2 |  | data 3 |                  |
|        |  |        |  |        |  |        |                  |
'--------'  '--------'  '--------'  '--------'                  |
     ^ ^           ^                                            v
     | |           |      block 7     block 8     block 9    block 10
     | |           |    .--------.  .--------.  .--------.  .--------.
     | |           '----| new    |<-| new    |<-| new    |<-| new    |
     | '----------------| data 2 |<-| data 3 |--| data 4 |  | data 5 |
     '------------------|        |--|        |--|        |  |        |
                        '--------'  '--------'  '--------'  '--------'
```

## Block allocation

So those two ideas provide the grounds for the filesystem. The metadata pairs
give us directories, and the CTZ linked-lists give us files. But this leaves
one big [elephant](https://upload.wikimedia.org/wikipedia/commons/3/37/African_Bush_Elephant.jpg)
of a question. How do we get those blocks in the first place?

One common strategy is to store unallocated blocks in a big free list, and
initially the littlefs was designed with this in mind. By storing a reference
to the free list in every single metadata pair, additions to the free list
could be updated atomically at the same time the replacement blocks were
stored in the metadata pair. During boot, every metadata pair had to be
scanned to find the most recent free list, but once the list was found the
state of all free blocks becomes known.

However, this approach had several issues:
- There was a lot of nuanced logic for adding blocks to the free list without
  modifying the blocks, since the blocks remain active until the metadata is
  updated.
- The free list had to support both additions and removals in fifo order while
  minimizing block erases.
- The free list had to handle the case where the file system completely ran
  out of blocks and may no longer be able to add blocks to the free list.
- If we used a revision count to track the most recently updated free list,
  metadata blocks that were left unmodified were ticking time bombs that would
  cause the system to go haywire if the revision count overflowed
- Every single metadata block wasted space to store these free list references.

Actually, to simplify, this approach had one massive glaring issue: complexity.

> Complexity leads to fallibility.  
> Fallibility leads to unmaintainability.  
> Unmaintainability leads to suffering.  

Or at least, complexity leads to increased code size, which is a problem
for embedded systems.

In the end, the littlefs adopted more of a "drop it on the floor" strategy.
That is, the littlefs doesn't actually store information about which blocks
are free on the storage. The littlefs already stores which files _are_ in
use, so to find a free block, the littlefs just takes all of the blocks that
exist and subtract the blocks that are in use.

Of course, it's not quite that simple. Most filesystems that adopt this "drop
it on the floor" strategy either rely on some properties inherent to the
filesystem, such as the cyclic-buffer structure of logging filesystems,
or use a bitmap or table stored in RAM to track free blocks, which scales
with the size of storage and is problematic when you have limited RAM. You
could iterate through every single block in storage and check it against
every single block in the filesystem on every single allocation, but that
would have an abhorrent runtime.

So the littlefs compromises. It doesn't store a bitmap the size of the storage,
but it does store a little bit-vector that contains a fixed set lookahead
for block allocations. During a block allocation, the lookahead vector is
checked for any free blocks, if there are none, the lookahead region jumps
forward and the entire filesystem is scanned for free blocks.

Here's what it might look like to allocate 4 blocks on a decently busy
filesystem with a 32bit lookahead and a total of
128 blocks (512Kbytes of storage if blocks are 4Kbyte):
```
boot...         lookahead:
                fs blocks: fffff9fffffffffeffffffffffff0000
scanning...     lookahead: fffff9ff
                fs blocks: fffff9fffffffffeffffffffffff0000
alloc = 21      lookahead: fffffdff
                fs blocks: fffffdfffffffffeffffffffffff0000
alloc = 22      lookahead: ffffffff
                fs blocks: fffffffffffffffeffffffffffff0000
scanning...     lookahead:         fffffffe
                fs blocks: fffffffffffffffeffffffffffff0000
alloc = 63      lookahead:         ffffffff
                fs blocks: ffffffffffffffffffffffffffff0000
scanning...     lookahead:         ffffffff
                fs blocks: ffffffffffffffffffffffffffff0000
scanning...     lookahead:                 ffffffff
                fs blocks: ffffffffffffffffffffffffffff0000
scanning...     lookahead:                         ffff0000
                fs blocks: ffffffffffffffffffffffffffff0000
alloc = 112     lookahead:                         ffff8000
                fs blocks: ffffffffffffffffffffffffffff8000
```

While this lookahead approach still has an asymptotic runtime of O(n^2) to
scan all of storage, the lookahead reduces the practical runtime to a
reasonable amount. Bit-vectors are surprisingly compact, given only 16 bytes,
the lookahead could track 128 blocks. For a 4Mbyte flash chip with 4Kbyte
blocks, the littlefs would only need 8 passes to scan the entire storage.

The real benefit of this approach is just how much it simplified the design
of the littlefs. Deallocating blocks is as simple as simply forgetting they
exist, and there is absolutely no concern of bugs in the deallocation code
causing difficult to detect memory leaks.

## Directories

Now we just need directories to store our files. Since we already have
metadata blocks that store information about files, lets just use these
metadata blocks as the directories. Maybe turn the directories into linked
lists of metadata blocks so it isn't limited by the number of files that fit
in a single block. Add entries that represent other nested directories.
Drop "." and ".." entries, cause who needs them. Dust off our hands and
we now have a directory tree.

```
            .--------.
            |root dir|
            | pair 0 |
            |        |
            '--------'
            .-'    '-------------------------.
           v                                  v
      .--------.        .--------.        .--------.
      | dir A  |------->| dir A  |        | dir B  |
      | pair 0 |        | pair 1 |        | pair 0 |
      |        |        |        |        |        |
      '--------'        '--------'        '--------'
      .-'    '-.            |             .-'    '-.
     v          v           v            v          v
.--------.  .--------.  .--------.  .--------.  .--------.
| file C |  | file D |  | file E |  | file F |  | file G |
|        |  |        |  |        |  |        |  |        |
|        |  |        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'
```

Unfortunately it turns out it's not that simple. See, iterating over a
directory tree isn't actually all that easy, especially when you're trying
to fit in a bounded amount of RAM, which rules out any recursive solution.
And since our block allocator involves iterating over the entire filesystem
tree, possibly multiple times in a single allocation, iteration needs to be
efficient.

So, as a solution, the littlefs adopted a sort of threaded tree. Each
directory not only contains pointers to all of its children, but also a
pointer to the next directory. These pointers create a linked-list that
is threaded through all of the directories in the filesystem. Since we
only use this linked list to check for existance, the order doesn't actually
matter. As an added plus, we can repurpose the pointer for the individual
directory linked-lists and avoid using any additional space.

```
            .--------.
            |root dir|-.
            | pair 0 | |
   .--------|        |-'
   |        '--------'
   |        .-'    '-------------------------.
   |       v                                  v
   |  .--------.        .--------.        .--------.
   '->| dir A  |------->| dir B  |------->| dir B  |
      | pair 0 |        | pair 1 |        | pair 0 |
      |        |        |        |        |        |
      '--------'        '--------'        '--------'
      .-'    '-.            |             .-'    '-.
     v          v           v            v          v
.--------.  .--------.  .--------.  .--------.  .--------.
| file C |  | file D |  | file E |  | file F |  | file G |
|        |  |        |  |        |  |        |  |        |
|        |  |        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'
```

This threaded tree approach does come with a few tradeoffs. Now, anytime we
want to manipulate the directory tree, we find ourselves having to update two
pointers instead of one. For anyone familiar with creating atomic data
structures this should set off a whole bunch of red flags.

But unlike the data structure guys, we can update a whole block atomically! So
as long as we're really careful (and cheat a little bit), we can still
manipulate the directory tree in a way that is resilient to power loss.

Consider how we might add a new directory. Since both pointers that reference
it can come from the same directory, we only need a single atomic update to
finagle the directory into the filesystem:
```
   .--------.
   |root dir|-.
   | pair 0 | |
.--|        |-'
|  '--------'
|      |
|      v
|  .--------.
'->| dir A  |
   | pair 0 |
   |        |
   '--------'

|  create the new directory block
v

               .--------.
               |root dir|-.
               | pair 0 | |
            .--|        |-'
            |  '--------'
            |      |
            |      v
            |  .--------.
.--------.  '->| dir A  |
| dir B  |---->| pair 0 |
| pair 0 |     |        |
|        |     '--------'
'--------'

|  update root to point to directory B
v

         .--------.
         |root dir|-.
         | pair 0 | |
.--------|        |-'
|        '--------'
|        .-'    '-.
|       v          v
|  .--------.  .--------.
'->| dir B  |->| dir A  |
   | pair 0 |  | pair 0 |
   |        |  |        |
   '--------'  '--------'
```

Note that even though directory B was added after directory A, we insert
directory B before directory A in the linked-list because it is convenient.

Now how about removal:
```
         .--------.        .--------.
         |root dir|------->|root dir|-.
         | pair 0 |        | pair 1 | |
.--------|        |--------|        |-'
|        '--------'        '--------'
|        .-'    '-.            |
|       v          v           v
|  .--------.  .--------.  .--------.
'->| dir A  |->| dir B  |->| dir C  |
   | pair 0 |  | pair 0 |  | pair 0 |
   |        |  |        |  |        |
   '--------'  '--------'  '--------'

|  update root to no longer contain directory B
v

   .--------.              .--------.
   |root dir|------------->|root dir|-.
   | pair 0 |              | pair 1 | |
.--|        |--------------|        |-'
|  '--------'              '--------'
|      |                       |
|      v                       v
|  .--------.  .--------.  .--------.
'->| dir A  |->| dir B  |->| dir C  |
   | pair 0 |  | pair 0 |  | pair 0 |
   |        |  |        |  |        |
   '--------'  '--------'  '--------'

|  remove directory B from the linked-list
v

   .--------.  .--------.
   |root dir|->|root dir|-.
   | pair 0 |  | pair 1 | |
.--|        |--|        |-'
|  '--------'  '--------'
|      |           |
|      v           v
|  .--------.  .--------.
'->| dir A  |->| dir C  |
   | pair 0 |  | pair 0 |
   |        |  |        |
   '--------'  '--------'
```

Wait, wait, wait, wait, wait, that's not atomic at all! If power is lost after
removing directory B from the root, directory B is still in the linked-list.
We've just created a memory leak!

And to be honest, I don't have a clever solution for this case. As a
side-effect of using multiple pointers in the threaded tree, the littlefs
can end up with orphan blocks that have no parents and should have been
removed.

To keep these orphan blocks from becoming a problem, the littlefs has a
deorphan step that simply iterates through every directory in the linked-list
and checks it against every directory entry in the filesystem to see if it
has a parent. The deorphan step occurs on the first block allocation after
boot, so orphans should never cause the littlefs to run out of storage
prematurely.

And for my final trick, moving a directory:
```
         .--------.
         |root dir|-.
         | pair 0 | |
.--------|        |-'
|        '--------'
|        .-'    '-.
|       v          v
|  .--------.  .--------.
'->| dir A  |->| dir B  |
   | pair 0 |  | pair 0 |
   |        |  |        |
   '--------'  '--------'

|  update directory B to point to directory A
v

         .--------.
         |root dir|-.
         | pair 0 | |
.--------|        |-'
|        '--------'
|    .-----'    '-.
|    |             v
|    |           .--------.
|    |        .->| dir B  |
|    |        |  | pair 0 |
|    |        |  |        |
|    |        |  '--------'
|    |     .-------'
|    v    v   |
|  .--------. |
'->| dir A  |-'
   | pair 0 |
   |        |
   '--------'

|  update root to no longer contain directory A
v
     .--------.
     |root dir|-.
     | pair 0 | |
.----|        |-'
|    '--------'
|        |
|        v
|    .--------.
| .->| dir B  |
| |  | pair 0 |
| '--|        |-.
|    '--------' |
|        |      |
|        v      |
|    .--------. |
'--->| dir A  |-'
     | pair 0 |
     |        |
     '--------'
```

Note that once again we don't care about the ordering of directories in the
linked-list, so we can simply leave directories in their old positions. This
does make the diagrams a bit hard to draw, but the littlefs doesn't really
care.

It's also worth noting that once again we have an operation that isn't actually
atomic. After we add directory A to directory B, we could lose power, leaving
directory A as a part of both the root directory and directory B. However,
there isn't anything inherent to the littlefs that prevents a directory from
having multiple parents, so in this case, we just allow that to happen. Extra
care is taken to only remove a directory from the linked-list if there are
no parents left in the filesystem.

## Wear awareness

So now that we have all of the pieces of a filesystem, we can look at a more
subtle attribute of embedded storage: The wear down of flash blocks.

The first concern for the littlefs, is that prefectly valid blocks can suddenly
become unusable. As a nice side-effect of using a COW data-structure for files,
we can simply move on to a different block when a file write fails. All
modifications to files are performed in copies, so we will only replace the
old file when we are sure none of the new file has errors. Directories, on
the other hand, need a different strategy.

The solution to directory corruption in the littlefs relies on the redundant
nature of the metadata pairs. If an error is detected during a write to one
of the metadata pairs, we seek out a new block to take its place. Once we find
a block without errors, we iterate through the directory tree, updating any
references to the corrupted metadata pair to point to the new metadata block.
Just like when we remove directories, we can lose power during this operation
and end up with a desynchronized metadata pair in our filesystem. And just like
when we remove directories, we leave the possibility of a desynchronized
metadata pair up to the deorphan step to clean up.

Here's what encountering a directory error may look like with all of
the directories and directory pointers fully expanded:
```
         root dir
         block 1   block 2
       .---------.---------.
       | rev: 1  | rev: 0  |--.
       |         |         |-.|
.------|         |         |-|'
|.-----|         |         |-'
||     '---------'---------'
||       |||||'--------------------------------------------------.
||       ||||'-----------------------------------------.         |
||       |||'-----------------------------.            |         |
||       ||'--------------------.         |            |         |
||       |'-------.             |         |            |         |
||       v         v            v         v            v         v
||    dir A                  dir B                  dir C
||    block 3   block 4      block 5   block 6      block 7   block 8
||  .---------.---------.  .---------.---------.  .---------.---------.
|'->| rev: 1  | rev: 0  |->| rev: 1  | rev: 0  |->| rev: 1  | rev: 0  |
'-->|         |         |->|         |         |->|         |         |
    |         |         |  |         |         |  |
    |         |         |  |         |         |  |         |         |
    '---------'---------'  '---------'---------'  '---------'---------'

|  update directory B
v

         root dir
         block 1   block 2
       .---------.---------.
       | rev: 1  | rev: 0  |--.
       |         |         |-.|
.------|         |         |-|'
|.-----|         |         |-'
||     '---------'---------'
||       |||||'--------------------------------------------------.
||       ||||'-----------------------------------------.         |
||       |||'-----------------------------.            |         |
||       ||'--------------------.         |            |         |
||       |'-------.             |         |            |         |
||       v         v            v         v            v         v
||    dir A                  dir B                  dir C
||    block 3   block 4      block 5   block 6      block 7   block 8
||  .---------.---------.  .---------.---------.  .---------.---------.
|'->| rev: 1  | rev: 0  |->| rev: 1  | rev: 2  |->| rev: 1  | rev: 0  |
'-->|         |         |->|         | corrupt!|->|         |         |
    |         |         |  |         | corrupt!|  |         |         |
    |         |         |  |         | corrupt!|  |         |         |
    '---------'---------'  '---------'---------'  '---------'---------'

|  oh no! corruption detected
v  allocate a replacement block

         root dir
         block 1   block 2
       .---------.---------.
       | rev: 1  | rev: 0  |--.
       |         |         |-.|
.------|         |         |-|'
|.-----|         |         |-'
||     '---------'---------'
||       |||||'----------------------------------------------------.
||       ||||'-------------------------------------------.         |
||       |||'-----------------------------.              |         |
||       ||'--------------------.         |              |         |
||       |'-------.             |         |              |         |
||       v         v            v         v              v         v
||    dir A                  dir B                    dir C
||    block 3   block 4      block 5   block 6        block 7   block 8
||  .---------.---------.  .---------.---------.    .---------.---------.
|'->| rev: 1  | rev: 0  |->| rev: 1  | rev: 2  |--->| rev: 1  | rev: 0  |
'-->|         |         |->|         | corrupt!|--->|         |         |
    |         |         |  |         | corrupt!| .->|         |         |
    |         |         |  |         | corrupt!| |  |         |         |
    '---------'---------'  '---------'---------' |  '---------'---------'
                                       block 9   |
                                     .---------. |
                                     | rev: 2  |-'
                                     |         |
                                     |         |
                                     |         |
                                     '---------'

|  update root directory to contain block 9
v

        root dir
        block 1   block 2
      .---------.---------.
      | rev: 1  | rev: 2  |--.
      |         |         |-.|
.-----|         |         |-|'
|.----|         |         |-'
||    '---------'---------'
||       .--------'||||'----------------------------------------------.
||       |         |||'-------------------------------------.         |
||       |         ||'-----------------------.              |         |
||       |         |'------------.           |              |         |
||       |         |             |           |              |         |
||       v         v             v           v              v         v
||    dir A                   dir B                      dir C
||    block 3   block 4       block 5     block 9        block 7   block 8
||  .---------.---------.   .---------. .---------.    .---------.---------.
|'->| rev: 1  | rev: 0  |-->| rev: 1  |-| rev: 2  |--->| rev: 1  | rev: 0  |
'-->|         |         |-. |         | |         |--->|         |         |
    |         |         | | |         | |         | .->|         |         |
    |         |         | | |         | |         | |  |         |         |
    '---------'---------' | '---------' '---------' |  '---------'---------'
                          |               block 6   |
                          |             .---------. |
                          '------------>| rev: 2  |-'
                                        | corrupt!|
                                        | corrupt!|
                                        | corrupt!|
                                        '---------'

|  remove corrupted block from linked-list
v

        root dir
        block 1   block 2
      .---------.---------.
      | rev: 1  | rev: 2  |--.
      |         |         |-.|
.-----|         |         |-|'
|.----|         |         |-'
||    '---------'---------'
||       .--------'||||'-----------------------------------------.
||       |         |||'--------------------------------.         |
||       |         ||'--------------------.            |         |
||       |         |'-----------.         |            |         |
||       |         |            |         |            |         |
||       v         v            v         v            v         v
||    dir A                  dir B                  dir C
||    block 3   block 4      block 5   block 9      block 7   block 8
||  .---------.---------.  .---------.---------.  .---------.---------.
|'->| rev: 1  | rev: 2  |->| rev: 1  | rev: 2  |->| rev: 1  | rev: 0  |
'-->|         |         |->|         |         |->|         |         |
    |         |         |  |         |         |  |         |         |
    |         |         |  |         |         |  |         |         |
    '---------'---------'  '---------'---------'  '---------'---------'
```

Also one question I've been getting is, what about the root directory?
It can't move so wouldn't the filesystem die as soon as the root blocks
develop errors? And you would be correct. So instead of storing the root
in the first few blocks of the storage, the root is actually pointed to
by the superblock. The superblock contains a few bits of static data, but
outside of when the filesystem is formatted, it is only updated when the root
develops errors and needs to be moved.

## Wear leveling

The second concern for the littlefs, is that blocks in the filesystem may wear
unevenly. In this situation, a filesystem may meet an early demise where
there are no more non-corrupted blocks that aren't in use. It may be entirely
possible that files were written once and left unmodified, wasting the
potential erase cycles of the blocks it sits on.

Wear leveling is a term that describes distributing block writes evenly to
avoid the early termination of a flash part. There are typically two levels
of wear leveling:
1. Dynamic wear leveling - Blocks are distributed evenly during blocks writes.
   Note that the issue with write-once files still exists in this case.
2. Static wear leveling - Unmodified blocks are evicted for new block writes.
   This provides the longest lifetime for a flash device.

Now, it's possible to use the revision count on metadata pairs to approximate
the wear of a metadata block. And combined with the COW nature of files, the
littlefs could provide a form of dynamic wear leveling.

However, the littlefs does not. This is for a few reasons. Most notably, even
if the littlefs did implement dynamic wear leveling, this would still not
handle the case of write-once files, and near the end of the lifetime of a
flash device, you would likely end up with uneven wear on the blocks anyways.

As a flash device reaches the end of its life, the metadata blocks will
naturally be the first to go since they are updated most often. In this
situation, the littlefs is designed to simply move on to another set of
metadata blocks. This travelling means that at the end of a flash device's
life, the filesystem will have worn the device down as evenly as a dynamic
wear leveling filesystem could anyways. Simply put, if the lifetime of flash
is a serious concern, static wear leveling is the only valid solution.

This is a very important takeaway to note. If your storage stack uses highly
sensitive storage such as NAND flash. In most cases you are going to be better
off just using a [flash translation layer (FTL)](https://en.wikipedia.org/wiki/Flash_translation_layer).
NAND flash already has many limitations that make it poorly suited for an
embedded system: low erase cycles, very large blocks, errors that can develop
even during reads, errors that can develop during writes of neighboring blocks.
Managing sensitive storage such as NAND flash is out of scope for the littlefs.
The littlefs does have some properties that may be beneficial on top of a FTL,
such as limiting the number of writes where possible. But if you have the
storage requirements that necessitate the need of NAND flash, you should have
the RAM to match and just use an FTL or flash filesystem.

## Summary

So, to summarize:

1. The littlefs is composed of directory blocks
2. Each directory is a linked-list of metadata pairs
3. These metadata pairs can be updated atomically by alternative which
   metadata block is active
4. Directory blocks contain either references to other directories or files
5. Files are represented by copy-on-write CTZ linked-lists
6. The CTZ linked-lists support appending in O(1) and reading in O(n logn)
7. Blocks are allocated by scanning the filesystem for used blocks in a
   fixed-size lookahead region is that stored in a bit-vector
8. To facilitate scanning the filesystem, all directories are part of a
   linked-list that is threaded through the entire filesystem
9. If a block develops an error, the littlefs allocates a new block, and
   moves the data and references of the old block to the new.
10. Any case where an atomic operation is not possible, it is taken care of
   by a deorphan step that occurs on the first allocation after boot

Welp, that's the little filesystem. Thanks for reading!

