## The design of littlefs
A little fail-safe filesystem designed for microcontrollers.

```
             .---._____
  .-----.   |          |
--|o    |---| littlefs |
--|     |---|          |
  '-----'   '----------'
```

littlefs was originally built as an experiment to learn about filesystem design
in the context of microcontrollers. The question was: How would you build a
filesystem that is resilient to power-loss and flash wear without using
unbounded memory?
This document covers the high-level design of littlefs, how it is different
than other filesystems, and the design decisions that got us here. For the
low-level details covering every bit on disk, check out [SPEC.md](SPEC.md).
## The problem
The embedded systems littlefs targets are usually 32-bit microcontrollers with
around 32 KiB of RAM and 512 KiB of ROM. These are often paired with SPI NOR
flash chips with about 4 MiB of flash storage. These devices are too small for
Linux and most existing filesystems, requiring code written specifically with
size in mind.
Flash itself is an interesting piece of technology with its own quirks and
nuance. Unlike other forms of storage, writing to flash requires two
operations: erasing and programming. Programming (setting bits to 0) is
relatively cheap and can be very granular. Erasing however (setting bits to 1),
requires an expensive and destructive operation which gives flash its name.
[Wikipedia][wikipedia-flash] has more information on how exactly flash works.
To make the situation more annoying, it's very common for these embedded
systems to lose power at any time. Usually, microcontroller code is simple and
reactive, with no concept of a shutdown routine. This presents a big challenge
for persistent storage, where an unlucky power loss can corrupt the storage and
leave a device unrecoverable.
This leaves us with three major requirements for an embedded filesystem.
1. **Power-loss resilience** - On these systems, power can be lost at any time.
   If a power loss corrupts any persistent data structures, this can cause the
   device to become unrecoverable. An embedded filesystem must be designed to
   recover from a power loss during any write operation.
1. **Wear leveling** - Writing to flash is destructive. If a filesystem
   repeatedly writes to the same block, eventually that block will wear out.
   Filesystems that don't take wear into account can easily burn through blocks
   used to store frequently updated metadata and cause a device's early death.
	@@ -443,89 +443,94 @@
   more space is available, but because we have small logs, overflowing a log
   isn't really an error condition.
   Instead, we split our original metadata pair into two metadata pairs, each
   containing half of the entries, connected by a tail pointer. Instead of
   increasing the size of the log and dealing with the scalability issues
   associated with larger logs, we form a linked list of small bounded logs.
   This is a tradeoff as this approach does use more storage space, but at the
   benefit of improved scalability.
   Despite writing to two metadata pairs, we can still maintain power
   resilience during this split step by first preparing the new metadata pair,
   and then inserting the tail pointer during the commit to the original
   metadata pair.
   ```
                         commit C and D, need to split
   .----------------.----------------.    .----------------.----------------.
   |   revision 1   |   revision 2   | => |   revision 3   |   revision 2   |
   |----------------|----------------|    |----------------|----------------|
   |       A        |       A'       |    |       A'       |       A'       |
   |----------------|----------------|    |----------------|----------------|
   |    checksum    |       B'       |    |       B'       |       B'       |
   |----------------|----------------|    |----------------|----------------|
   |       B        |    checksum    |    |      tail    ---------------------.
   |----------------|----------------|    |----------------|----------------| |
   |       A'       |       |        |    |    checksum    |                | |
   |----------------|       v        |    |----------------|                | |
   |    checksum    |                |    |       |        |                | |
   |----------------|                |    |       v        |                | |
   '----------------'----------------'    '----------------'----------------' |
                                                   .----------------.---------'
                                                  v                v
                                          .----------------.----------------.
                                          |   revision 1   |   revision 0   |
                                          |----------------|----------------|
                                          |       C        |                |
                                          |----------------|                |
                                          |       D        |                |
                                          |----------------|                |
                                          |    checksum    |                |
                                          |----------------|                |
                                          |       |        |                |
                                          |       v        |                |
                                          |                |                |
                                          |                |                |
                                          '----------------'----------------'
   ```
There is another complexity the crops up when dealing with small logs. The
amortized runtime cost of garbage collection is not only dependent on its
one time cost (_O(n&sup2;)_ for littlefs), but also depends on how often
garbage collection occurs.
Consider two extremes:
1. Log is empty, garbage collection occurs once every _n_ updates
2. Log is full, garbage collection occurs **every** update
Clearly we need to be more aggressive than waiting for our metadata pair to
be full. As the metadata pair approaches fullness the frequency of compactions
grows very rapidly.

Looking at the problem generically, consider a log with `n` bytes for each
entry, `d`dynamic entries (entries that are outdated during garbage
collection), and `s` static entries (entries that need to be copied during
garbage collection). If we look at the amortized runtime complexity of updating
this log we get this formula:

![cost = n + n (s / d+1)][metadata-formula1]

If we let `r` be the ratio of static space to the size of our log in bytes, we
find an alternative representation of the number of static and dynamic entries:

$$
s = r (size/n)
$$

$$
d = (1 - r) (size/n)
$$

Substituting these in for `d` and `s` gives us a nice formula for the cost of
updating an entry given how full the log is:

$$
cost = n + n (r (size/n) / ((1-r) (size/n) + 1))
$$

Assuming 100 byte entries in a 4 KiB log, we can graph this using the entry
size to find a multiplicative cost:
![Metadata pair update cost graph][metadata-cost-graph]

So at 50% usage, we're seeing an average of 2x cost per update, and at 75%
usage, we're already at an average of 4x cost per update.
To avoid this exponential growth, instead of waiting for our metadata pair
to be full, we split the metadata pair once we exceed 50% capacity. We do this
lazily, waiting until we need to compact before checking if we fit in our 50%
limit. This limits the overhead of garbage collection to 2x the runtime cost,
giving us an amortized runtime complexity of _O(1)_.
---
If we look at metadata pairs and linked-lists of metadata pairs at a high
level, they have fairly nice runtime costs. Assuming _n_ metadata pairs,
each containing _m_ metadata entries, the _lookup_ cost for a specific
entry has a worst case runtime complexity of _O(nm)_. For _updating_ a specific
entry, the worst case complexity is _O(nm&sup2;)_, with an amortized complexity
of only _O(nm)_.
However, splitting at 50% capacity does mean that in the best case our
metadata pairs will only be 1/2 full. If we include the overhead of the second
block in our metadata pair, each metadata entry has an effective storage cost
of 4x the original size. I imagine users would not be happy if they found
that they can only use a quarter of their original storage. Metadata pairs
provide a mechanism for performing atomic updates, but we need a separate
mechanism for storing the bulk of our data.
## CTZ skip-lists
Metadata pairs provide efficient atomic updates but unfortunately have a large
storage cost. But we can work around this storage cost by only using the
metadata pairs to store references to more dense, copy-on-write (COW) data
structures.
[Copy-on-write data structures][wikipedia-cow], also called purely functional
data structures, are a category of data structures where the underlying
elements are immutable.  Making changes to the data requires creating new
elements containing a copy of the updated data and replacing any references
with references to the new elements. Generally, the performance of a COW data
structure depends on how many old elements can be reused after replacing parts
of the data.
littlefs has several requirements of its COW structures. They need to be
efficient to read and write, but most frustrating, they need to be traversable
with a constant amount of RAM. Notably this rules out
[B-trees][wikipedia-B-tree], which can not be traversed with constant RAM, and
[B+-trees][wikipedia-B+-tree], which are not possible to update with COW
operations.
---
So, what can we do? First let's consider storing files in a simple COW
linked-list. Appending a block, which is the basis for writing files, means we
have to update the last block to point to our new block. This requires a COW
operation, which means we need to update the second-to-last block, and then the
third-to-last, and so on until we've copied out the entire file.
```
A linked-list
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |->| data 1 |->| data 2 |->| data 4 |->| data 5 |->| data 6 |
|        |  |        |  |        |  |        |  |        |  |        |
|        |  |        |  |        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```
To avoid a full copy during appends, we can store the data backwards. Appending
blocks just requires adding the new block and no other blocks need to be
updated. If we update a block in the middle, we still need to copy the
following blocks, but can reuse any blocks before it. Since most file writes
are linear, this design gambles that appends are the most common type of data
update.
```
A backwards linked-list
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |<-| data 1 |<-| data 2 |<-| data 4 |<-| data 5 |<-| data 6 |
|        |  |        |  |        |  |        |  |        |  |        |
|        |  |        |  |        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```
However, a backwards linked-list does have a rather glaring problem. Iterating
over a file _in order_ has a runtime cost of _O(n&sup2;)_. A quadratic runtime
just to read a file! That's awful.
Fortunately we can do better. Instead of a singly linked list, littlefs
uses a multilayered linked-list often called a
[skip-list][wikipedia-skip-list]. However, unlike the most common type of
skip-list, littlefs's skip-lists are strictly deterministic built around some
interesting properties of the count-trailing-zeros (CTZ) instruction.
The rules CTZ skip-lists follow are that for every _n_&zwj;th block where _n_
is divisible by 2&zwj;_&#739;_, that block contains a pointer to block
_n_-2&zwj;_&#739;_. This means that each block contains anywhere from 1 to
log&#8322;_n_ pointers that skip to different preceding elements of the
skip-list.
The name comes from heavy use of the [CTZ instruction][wikipedia-ctz], which
lets us calculate the power-of-two factors efficiently. For a given block _n_,
that block contains ctz(_n_)+1 pointers.
```
A backwards CTZ skip-list
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |<-| data 1 |<-| data 2 |<-| data 3 |<-| data 4 |<-| data 5 |
|        |<-|        |--|        |<-|        |--|        |  |        |
|        |<-|        |--|        |--|        |--|        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```
The additional pointers let us navigate the data-structure on disk much more
efficiently than in a singly linked list.
Consider a path from data block 5 to data block 1. You can see how data block 3
was completely skipped:
```
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |  | data 1 |<-| data 2 |  | data 3 |  | data 4 |<-| data 5 |
|        |  |        |  |        |<-|        |--|        |  |        |
|        |  |        |  |        |  |        |  |        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```
The path to data block 0 is even faster, requiring only two jumps:
```
.--------.  .--------.  .--------.  .--------.  .--------.  .--------.
| data 0 |  | data 1 |  | data 2 |  | data 3 |  | data 4 |<-| data 5 |
|        |  |        |  |        |  |        |  |        |  |        |
|        |<-|        |--|        |--|        |--|        |  |        |
'--------'  '--------'  '--------'  '--------'  '--------'  '--------'
```
We can find the runtime complexity by looking at the path to any block from
the block containing the most pointers. Every step along the path divides
the search space for the block in half, giving us a runtime of _O(log n)_.
To get _to_ the block with the most pointers, we can perform the same steps
backwards, which puts the runtime at _O(2 log n)_ = _O(log n)_. An interesting
note is that this optimal path occurs naturally if we greedily choose the
pointer that covers the most distance without passing our target.
So now we have a [COW] data structure that is cheap to append with a runtime
of _O(1)_, and can be read with a worst case runtime of _O(n log n)_. Given
that this runtime is also divided by the amount of data we can store in a
block, this cost is fairly reasonable.
---
This is a new data structure, so we still have several questions. What is the
storage overhead? Can the number of pointers exceed the size of a block? How do
we store a CTZ skip-list in our metadata pairs?
To find the storage overhead, we can look at the data structure as multiple
linked-lists. Each linked-list skips twice as many blocks as the previous,
or from another perspective, each linked-list uses half as much storage as
the previous. As we approach infinity, the storage overhead forms a geometric
series. Solving this tells us that on average our storage overhead is only
2 pointers per block.

$$
lim,n->inf((1/n)sum,i,0->n(ctz(i)+1)) = sum,i,0->inf(1/2^i) = 2
$$

Because our file size is limited the word width we use to store sizes, we can
also solve for the maximum number of pointers we would ever need to store in a
block. If we set the overhead of pointers equal to the block size, we get the
following equation. Note that both a smaller block size (`B`) and larger
word width (`w`) result in more storage overhead.

$$
B = (w/8)ceil(log2(2^w / (B-2w/8)))
$$

Solving the equation for `B` gives us the minimum block size for some
common word widths:

1. 32-bit CTZ skip-list => minimum block size of 104 bytes
2. 64-bit CTZ skip-list => minimum block size of 448 bytes
littlefs uses a 32-bit word width, so our blocks can only overflow with
pointers if they are smaller than 104 bytes. This is an easy requirement, as
in practice, most block sizes start at 512 bytes. As long as our block size
is larger than 104 bytes, we can avoid the extra logic needed to handle
pointer overflow.
This last question is how do we store CTZ skip-lists? We need a pointer to the
head block, the size of the skip-list, the index of the head block, and our
offset in the head block. But it's worth noting that each size maps to a unique
index + offset pair. So in theory we can store only a single pointer and size.

However, calculating the index + offset pair from the size is a bit
complicated. We can start with a summation that loops through all of the blocks
up until our given size. Let `B` be the block size in bytes, `w` be the
word width in bits, `n` be the index of the block in the skip-list, and
`N` be the file size in bytes:

$$
N = sum,i,0->n(B-(w/8)(ctz(i)+1))
$$

This works quite well, but requires _O(n)_ to compute, which brings the full
runtime of reading a file up to _O(n&sup2; log n)_. Fortunately, that summation
doesn't need to touch the disk, so the practical impact is minimal.
However, despite the integration of a bitwise operation, we can actually reduce
this equation to a _O(1)_ form.  While browsing the amazing resource that is
the [On-Line Encyclopedia of Integer Sequences (OEIS)][oeis], I managed to find
[A001511], which matches the iteration of the CTZ instruction,
and [A005187], which matches its partial summation. Much to my
surprise, these both result from simple equations, leading us to a rather
unintuitive property that ties together two seemingly unrelated bitwise
instructions:

$$
sum,i,0->n(ctz(i)+1) = 2n-popcount(n)
$$

where:

1. ctz(`x`) = the number of trailing bits that are 0 in `x`
2. popcount(`x`) = the number of bits that are 1 in `x`

Initial tests of this surprising property seem to hold. As `n` approaches
infinity, we end up with an average overhead of 2 pointers, which matches our
assumption from earlier. During iteration, the popcount function seems to
handle deviations from this average. Of course, just to make sure I wrote a
quick script that verified this property for all 32-bit integers.
Now we can substitute into our original equation to find a more efficient
equation for file size:

$$
N = Bn - (w/8)(2n-popcount(n))
$$

Unfortunately, the popcount function is non-injective, so we can't solve this
equation for our index. But what we can do is solve for an `n'` index that
is greater than `n` with error bounded by the range of the popcount function.
We can repeatedly substitute `n'` into the original equation until the error
is smaller than our integer resolution. As it turns out, we only need to
perform this substitution once, which gives us this formula for our index:

$$
n = floor((N-(w/8)popcount(N/(B-2w/8))) / (B-2w/8))
$$

Now that we have our index `n`, we can just plug it back into the above
equation to find the offset. We run into a bit of a problem with integer
overflow, but we can avoid this by rearranging the equation a bit:

$$
off = N - (B-2w/8)n - (w/8)popcount(n)
$$

Our solution requires quite a bit of math, but computers are very good at math.
Now we can find both our block index and offset from a size in _O(1)_, letting
	@@ -2124,50 +2143,26 @@
[wikipedia-hamming-bound]: https://en.wikipedia.org/wiki/Hamming_bound
[wikipedia-dynamic-wear-leveling]: https://en.wikipedia.org/wiki/Wear_leveling#Dynamic_wear_leveling
[wikipedia-static-wear-leveling]: https://en.wikipedia.org/wiki/Wear_leveling#Static_wear_leveling
[wikipedia-ftl]: https://en.wikipedia.org/wiki/Flash_translation_layer
[oeis]: https://oeis.org
[A001511]: https://oeis.org/A001511
[A005187]: https://oeis.org/A005187
[fat]: https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system
[ext2]: http://e2fsprogs.sourceforge.net/ext2intro.html
[jffs]: https://www.sourceware.org/jffs2/jffs2-html
[yaffs]: https://yaffs.net/documents/how-yaffs-works
[spiffs]: https://github.com/pellepl/spiffs/blob/master/docs/TECH_SPEC
[ext4]: https://ext4.wiki.kernel.org/index.php/Ext4_Design
[ntfs]: https://en.wikipedia.org/wiki/NTFS
[btrfs]: https://btrfs.wiki.kernel.org/index.php/Btrfs_design
[zfs]: https://en.wikipedia.org/wiki/ZFS
[cow]: https://upload.wikimedia.org/wikipedia/commons/0/0c/Cow_female_black_white.jpg
[elephant]: https://upload.wikimedia.org/wikipedia/commons/3/37/African_Bush_Elephant.jpg
[ram]: https://upload.wikimedia.org/wikipedia/commons/9/97/New_Mexico_Bighorn_Sheep.JPG

[metadata-cost-graph]: https://raw.githubusercontent.com/geky/littlefs/gh-images/metadata-cost.svg?sanitize=true
[wear-distribution-graph]: https://raw.githubusercontent.com/geky/littlefs/gh-images/wear-distribution.svg?sanitize=true
[file-cost-graph]: https://raw.githubusercontent.com/geky/littlefs/gh-images/file-cost.svg?sanitize=true
