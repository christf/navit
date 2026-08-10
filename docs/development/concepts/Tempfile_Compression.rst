.. _tempfile_compression:

Tempfile compression (block codec, parallel)
============================================

The maptool pipeline keeps almost all intermediate data in temporary
files that live in the working directory (``maptool_<pid>.tmp/``). At
planet scale these add up to several hundred GB: the node table
(``coords.tmp``) alone is ≈ 140 GB for ≈ 9 B nodes at 16 B/node, and the
way files (``ways``, ``ways_split``, ``ways_split_index``) dominate the
rest. Those files are internal to a single run and read back only by
maptool itself, so they do not need to be stored uncompressed.

This document describes an optional, block-based compression layer for
those temp files: how the bytes are laid out on disk, how compression is
parallelised by reusing the phase-13/14 tile-compression infrastructure,
and how it plugs into the existing ``FILE*`` API without touching the
hundreds of read/write call sites.

.. contents:: Contents
   :depth: 2
   :local:

Motivation and measured baseline
--------------------------------

A measured niedersachsen build (4 threads, ``-T 4 -k -e 4``) writes:

.. list-table::
   :header-rows: 1

   * - File
     - Uncompressed
   * - ``coords.tmp`` (node table)
     - 728 MB
   * - ``ways_``
     - 770 MB
   * - ``ways_split_``
     - 896 MB
   * - ``ways_split_index_``
     - 128 MB
   * - ``poly2poi_resolved``
     - 247 MB
   * - ``nodes_``
     - 145 MB
   * - ``way2poi_result``
     - 152 MB
   * - smaller files
     - ≈ 50 MB
   * - **total**
     - ≈ 3.1 GB

The ratio stays roughly the same at planet scale, where the estimate is
≈ 450-600 GB. The way item files are highly compressible (≈ 4-5x with
zstd -3); the node table is high-entropy 16 B records and compresses only
≈ 1.5-2.5x. Even so, compressing everything gets a planet build from
≈ 500 GB down to ≈ 120-200 GB, which fits the kind of 256 GB NVMe an 8 GB
laptop can hold.

Why block compression and not a streaming codec
-----------------------------------------------

Temp files have two access patterns:

- **Sequential streams.** ``ways``, ``ways_split``, ``nodes``,
  ``way2poi_result`` and the relation outputs are written once and read
  back sequentially.
- **Slice-addressed.** ``coords.tmp`` is read and rewritten in 1 GB
  slices (``load_buffer`` / ``save_buffer`` at ``i * slice_size``).

A classic streaming codec (one continuous dictionary per file) is
sequential on the write side and cannot jump to a slice without decoding
everything before it. A **block codec** - the file is a sequence of
independent, fixed-size *uncompressed* blocks, each compressed separately
- has three decisive properties:

1. **Blocks are independent**, so the write side can compress in
   parallel. The phases that write temp files (1-4) are single-threaded
   and leave all cores idle, so parallel block compression costs almost
   no wall time.
2. **Random access.** Reading slice ``i`` only needs the blocks that
   cover it, and rewriting slice ``i`` only recompresses those blocks.
3. **One codec path** for every file, instead of a separate streaming
   implementation per codec.

The only cost is a few percent of ratio (no cross-block dictionary),
which is dwarfed by the 3-5x win on the way files.

Reusing the phase-13/14 compression infrastructure
--------------------------------------------------

Phase 13/14 already compresses tile buffers in parallel with a small
worker pool (``misc.c`` ``tile_worker_pool_*``):

- a pair of ``GAsyncQueue``s (one for work, one for completed blocks),
- one worker thread per core, each with its own reusable scratch buffer
  and (for LZMA) its own ``lzma_allocator`` arena,
- a consumer that re-orders completed blocks by sequence number, so the
  output is byte-identical regardless of scheduling,
- the one-shot compressor ``compress_for_zip`` (``zip.c``), which takes a
  whole buffer in and produces a whole compressed buffer out.

Tempfile compression reuses all four pieces:

- The pool is extracted into a generic ``struct compress_pool``
  (``tempfile_compress.c``) whose workers call a shared
  ``compress_block`` primitive. Phase 13/14 becomes one client of that
  pool (via ``compress_for_zip``-style jobs); the tempfile writer is the
  other. The two never run at the same time, so they are two instances of
  the same code.
- Block compression is exactly the one-shot model ``compress_for_zip``
  implements, so the same per-worker scratch/allocator strategy applies.
- Tempfile blocks are written in submission order by a single writer
  thread per file, mirroring the phase-13 reorder logic; each compressed
  block's ``(usize, csize, offset)`` is recorded for the footer.

On-disk format
--------------

A compressed temp file is::

    [block 0][block 1]...[block n-1][footer]

- Every block holds at most ``block_size`` uncompressed bytes; the last
  block may be short.
- Each block is stored as its *compressed* bytes, possibly followed by
  nothing else; the footer knows each block's compressed size.
- A block whose compressed size is not smaller than ``block_size`` is
  stored **stored** (uncompressed), so incompressible slices never bloat.

The footer, written at close time and read on open, is::

    magic       "MTC1"             4 bytes
    version     1                  4
    codec       (zlib/lzma/zstd)   4
    level       compressor level   4
    block_size                      4
    block_count                     4
    logical_size                    8
    per block: usize (4), csize (4), file_off (8)   16 * block_count
    compressed_end (offset of footer in the file)    8
    tail magic  "MTC1"             4

On open the tail magic is found by reading the last 4 bytes; the footer
length is derived from ``block_count``. A file that does not carry the
magic is treated as a plain, uncompressed file - so the same open helper
works for compressed and plain files, and ``-Q none`` runs keep exactly
today's on-disk layout.

The ``FILE*`` layer
-------------------

The entire pipeline works on ``FILE*`` handles obtained from
``tempfile()`` and the coordinate helpers ``load_buffer`` /
``save_buffer`` / ``sizeof_buffer``. Rather than change any of the
hundreds of call sites to a new handle type, a compressed file is exposed
as a **glibc ``fopencookie`` stream**: the cookie functions
(``read``/``write``/``seek``/``close``) translate standard stdio calls
into the block codec. All existing code - ``item_bin_write``,
``read_item``, ``read_node_item``, ``fseeko``/``ftello``/``ftell``,
``fscanf``/``fprintf`` where used - keeps working unmodified; a
compressed file behaves like an ordinary byte stream with the logical
(uncompressed) size.

The cookie state keeps:

- the block map loaded from the footer (read mode) or built up (write
  mode),
- the current logical position,
- one decompressed block buffer for reads,
- the in-progress block buffer and the in-flight counter for writes.

Write path
~~~~~~~~~~

``write(cookie, buf, n)``:

- **Append** (position == logical end): copy into the current block
  buffer; when it fills, submit the block to the compression pool and
  continue. ``write`` returns immediately; the pool compresses while the
  single-threaded caller keeps producing. A per-file writer thread writes
  completed blocks to the physical file in order. To bound memory, the
  writer stalls once more than a few blocks are in flight.
- **Overwrite** (position < logical end, used by ``save_buffer`` for the
  node table): if the range is block-aligned (the normal case, since
  slices are 1 GB), each covered block is recompressed from the new bytes
  and appended; the block-map entries are updated to point at the new
  copies and the old bytes become garbage. Unaligned ranges fall back to
  read-merge-recompress. ``coords.tmp`` is compacted once after the
  phase-2 rewrite loop to reclaim the garbage.
- Errors during physical I/O (e.g. disk full) surface through the cookie
  and route through ``fatal_file_error`` like the rest of the pipeline.

Read path
~~~~~~~~~

``read(cookie, buf, n)``: after draining any in-flight writes, load the
block at the current position (seek the physical file to its offset, read
its compressed bytes, decompress into the block buffer) and serve bytes
from it. ``seek`` moves the logical position and invalidates the cached
block; ``SEEK_END`` returns ``logical_size``.

Lifecycle and per-file policy
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``tempfile(suffix, name, mode)`` returns a compressed cookie stream for
write modes (and read modes, auto-detected from the footer) whenever
tempfile compression is enabled, except for a small denylist of files
that are used in ways a compressed stream cannot serve:

- ``tiles_data`` - mmap'd tile buffers (phase 13/14),
- ``sgr``, ``ddsg_coords`` - mmap'd in ``ch.c``.

Path-based opens that must auto-detect compressed files
(``coords.tmp`` in ``buffer.c`` / ``osm_process_turn_restrictions``, and
the aux-tile country parts in ``write_aux_tiles``) go through the same
open helper. Country index files and sort inputs are written with raw
``fopen`` and never compressed.

Command line
------------

- ``-Q`` / ``--tempfile-compression <none|zlib|lzma|zstd>`` (default
  ``none``). Independent of the map output compression (``-C`` / ``-z``).
- ``-q`` / ``--tempfile-level <n>`` (defaults: zlib 6, lzma 6, zstd 3).
- ``--tempfile-block-size <bytes>`` (default 32 MB; uncompressed block
  size). Larger blocks compress slightly better but cost more scratch and
  in-flight memory (scratch ≈ block size * 4/3 per worker).

With ``-Q none`` (the default) every temp file is written exactly as
today and the on-disk tmp layout is unchanged.

Why byte-identity is preserved
------------------------------

The produced ``.bin`` never contains temp-file bytes; it only depends on
the *logical* content of the temp streams. The cookie layer is
byte-exact (it stores and reproduces exactly the bytes written), so the
final map is byte-identical whether the run used ``-Q none`` or a
compressed codec. Compressed temp files are themselves deterministic per
run only if the same codec/level is used; that does not matter for the
output.

Verification
------------

- Byte-identical ``.bin`` between ``-Q none`` and ``-Q zstd``/``-Q lzma``
  runs at ``-T 1`` on the regression grids (relmix, test, reltest, grid)
  and niedersachsen, using the comparison helpers in ``/tmp/opencode``.
- The node-table rewrite path is exercised by every build (phase 2
  ``save_buffer``); compare a ``-Q zstd`` vs ``-Q none`` niedersachsen
  build with a slice size that is *not* a multiple of the block size to
  cover the unaligned overwrite path.
- ``-k`` runs: the kept ``.tmp`` files are codec-specific; a later run
  must auto-detect them (they carry the footer magic).
- Disk: run niedersachsen with ``-Q zstd`` and confirm peak temp usage
  drops roughly 3-4x.

Sequencing and effort
---------------------

.. list-table::
   :header-rows: 1

   * - Step
     - Measure
     - Est. effort
     - Rationale
   * - 1
     - Generic ``compress_pool`` + ``compress_block`` + ``decompress_block``
     - ~ 1 day
     - shared by tiles and tempfiles
   * - 2
     - Cookie ``FILE*`` layer, footer format, write/read/seek/overwrite
     - ~ 2-3 days
     - the bulk of the work
   * - 3
     - CLI (``-Q``/``-q``/``--tempfile-block-size``) and integration into
       ``tempfile()``/``buffer.c``/``write_aux_tiles``
     - ~ 1 day
     - mechanical, small blast radius
   * - 4
     - Verification (byte-identity matrix, node-table overwrite, disk)
     - ~ 1 day
     - reuses existing comparison helpers

Risks and mitigations
---------------------

- **stdio interop with ``fopencookie``.** All temp-file access is stdio;
  glibc handles buffered read/write switching on cookie streams. The
  ``-Q none`` path bypasses the cookie layer entirely, so any regression
  is confined to compressed runs.
- **Node-table rewrite.** ``save_buffer`` rewrites whole slices in place;
  appending recompressed blocks and updating the map is correct but
  leaves garbage. The one-shot compaction after phase 2 bounds
  ``coords.tmp`` to ≈ its compressed size.
- **Portability.** ``fopencookie`` is a glibc extension; on other
  platforms the feature degrades to plain files (no compression).
- **LZMA memory.** Level 9 needs ≈ 674 MB per worker; levels are capped
  by default and users are warned about high levels.
