# RSS placement report

`rssmap` combines a NIC's RSS indirection table, iflib RX queue CPU bindings,
and the kernel's software RSS bucket map. It only reads configuration.
It does not change interface settings or require `options RSS`.

Build from this source tree:

```
make -C tools/tools/rssmap
```

Run `rssmap aq0` for a compact summary with one row per RX queue. Use
`rssmap -b aq0` to expand it to individual hash indices. The tool is built
separately from the base system and does not add output to ifconfig.

The running kernel and driver must implement `SIOCGIFRSSTABLE`. A kernel
predating that ioctl, or a driver without a table getter, produces an error.
CPU bindings are currently available for iflib drivers. The tool resolves
the original driver name through the interface MIB, so renamed interfaces
work too. Missing or unbound queue CPUs appear as `-`.

## Reading the report

The header reports the hardware hash function and active type mask, compares
the NIC key with the software key when readable, and shows netisr's global
dispatch mode, worker count, and thread binding setting. Reading the software
key normally requires root; an unprivileged report continues with
`key comparison unavailable`. Raw keys are not printed. Equal keys alone do
not establish matching hash algorithms or input fields.

The default columns are:

- `QUEUE`: RX queue number.
- `RX-CPU`: CPU to which iflib binds the queue's receive task.
- `ENTRIES`: number of hardware indirection table entries selecting that queue.
- `SAME`, `DIFFERENT`, `UNKNOWN`: counts of hash indices whose software RSS
  target matches, differs from, or cannot be compared with the queue CPU.
- `RSS-CPUS`: the software RSS target CPUs for those indices, shown as a
  sorted list with consecutive CPU numbers grouped into ranges.

The last four columns appear only when the software bucket map is available.
An interface can therefore be inspected on a kernel without `options RSS`.
The software hash algorithm and key can still exist on such a kernel.

With `-b`, `HASH` identifies a class of hashes sharing the same low bits.
`ENTRY` is the hardware table index, `RXQ` its selected queue, `RSS-BUCKET`
the software bucket, and `RSS-CPU` that bucket's configured CPU target.
`PLACEMENT` compares the two CPU numbers.

For a hardware table of length H and a software table of length S, the
report enumerates max(H, S) hash indices. Hardware entries use
`hash & (H - 1)` and software buckets use `hash & (S - 1)`. Both lengths
must be powers of two. When S exceeds H, one hardware entry can correspond
to several software buckets, so the comparison counts can exceed `ENTRIES`.
These are configuration counts, not packet counts or traffic percentages.

The comparison uses the same numerical hash on both sides. Software
rehashing can change it, depending on the key and packet fields, including
fragmentation. CPU numbers are configured bindings and targets, not observed
execution CPUs. Direct netisr dispatch bypasses CPU selection; worker
configuration and protocol dispatch settings can also affect execution.
The reads are not an atomic snapshot, so avoid changing steering settings
while collecting a report.

## Tests

The `tests` directory contains ATF tests that run the built utility with
synthetic ioctl and sysctl responses. They require neither root nor a NIC.
Build both directories before querying their object paths:

```sh
make -C tools/tools/rssmap
make -C tools/tools/rssmap/tests
env RSSMAP="$(make -C tools/tools/rssmap -V .OBJDIR)/rssmap" \
    kyua test -k "$(make -C tools/tools/rssmap/tests -V .OBJDIR)/Kyuafile"
```

The ATF `rssmap` configuration variable can also select the binary to test.
