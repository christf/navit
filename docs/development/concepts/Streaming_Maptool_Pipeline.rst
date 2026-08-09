.. _streaming_maptool_pipeline:

Streaming maptool pipeline
==========================

``maptool`` converts OSM data into a Navit ``.bin`` map. Today it is a
single-process, multi-phase pipeline in which every phase except the last
uses exactly one CPU core, and all LZMA compression happens in one burst at
the very end. This document explains why it behaves that way, what has
already been done to move it toward true streaming, and a concrete roadmap
for the remaining work.

.. contents:: Contents
   :depth: 2
   :local:

Current state and measurements
------------------------------

Reference run (≈ 45M-node regional extract, 4-core laptop, ``-C lzma -z6``):

.. list-table::
   :header-rows: 1

   * - Phase
     - Wall time
     - Cores used
     - Note
   * - 1 read + map input
     - ~ 2:00
     - 1
     - 20 % of total
   * - 2 count refs / resolve ways
     - ~ 0:04
     - 1
     - slice loop over node table
   * - 3 convert ways to POIs
     - ~ 0:02
     - 1
     - ``way2poi``
   * - 4 split at intersections
     - ~ 0:33
     - 1
     - produces ``ways_split`` (≈ 900 MB)
   * - 5 coastlines
     - ~ 0:06
     - 1
   * - 6 towns → countries
     - ~ 0:00
     - 1
   * - 7 sort countries
     - ~ 0:00
     - 1
   * - 8 turn restrictions
     - ~ 0:22
     - 1
   * - 9 multipolygons
     - ~ 0:11
     - 1
   * - 10 associated streets
     - ~ 0:10
     - 1
     - rewrites ``ways_split``/``nodes``/``way2poi_result``
   * - 11 house number interpolations
     - ~ 0:11
     - 1
     - rewrites the same three files again
   * - 12 generate tiles
     - ~ 0:16
     - 1
     - mostly removed by the sizing work (see below)
   * - 13 assemble map
     - ~ 5:30
     - 1 + N
     - single-core read/dispatch, threaded compression burst
   * - 14 done
     -
     -

Total ≈ 9.5 minutes, of which phase 13 is ≈ 60 % and phase 1 ≈ 20 %.
This is the "congestion at the end" the current implementation exhibits.

Data flow and hard constraints
------------------------------

The pipeline is not a straight chain; it is a DAG that converges on the
tile generator::

    read OSM
      ├──► nodes ───────────────────────────────┐
      │    └──► way2poi_result ────────────────► │
      ├──► ways ─► resolve coords ─► split ─────►│   tiles + assemble
      │                            (ways_split)  │   + LZMA ──► .bin
      ├──► towns / coastlines / relations ──────►│
      └──► turn restrictions / multipolygons ───►│

Three constraints make a plain Unix-style pipeline impossible:

1. **Coordinate dependency.** Ways reference node IDs; they can only be
   tiled after the full node coordinate table is loaded (``coords.tmp`` +
   the slice-based node buffer). This is a hard barrier between input and
   everything downstream.
2. **Rewrite dependency.** ``ways_split`` (the dominant tile source,
   ≈ 900 MB) is only *final* after the phase 10/11 rewrites. Most tiles
   therefore cannot complete before the end of phase 11, no matter how the
   pipeline is scheduled.
3. **Single ZIP without data descriptors.** The Navit binfile is a classic
   ZIP: each member's local header carries its compressed size, so every
   tile must be fully assembled before it can be written. There is no way
   to stream a tile's *content* into the output before it is complete.
   Combined with (2) this places the compression burst structurally at the
   end for a single-binfile output.

Within these constraints "true streaming" means: **keep all cores busy from
start to finish by overlapping stages and parallelizing each CPU-heavy
stage**, and overlap the compression burst with the tail of production as
much as (2) and (3) allow.

What is already in place
------------------------

The following work is already committed to the working tree and is the
baseline for this roadmap:

- **Single-pass phase 4 → 5.** Tile sizes and per-tile completion positions
  are collected while the item files are produced (``tile_sizing_*`` in
  ``misc.c``). Phase 12 no longer re-reads the item files to compute sizes;
  phase 13 is the only read of the final files.
- **Incremental tile dispatch.** ``tile_check_complete`` (``tile.c``)
  dispatches a tile to the compression pool as soon as its completion point
  passes, and ``stream_write_completed`` (``misc.c``) writes members in
  zipnum order as they complete. Compression already overlaps with the tail
  of the phase-13 read loop.
- **Threaded compression pool.** ``tile_worker_pool_*`` runs
  ``thread_count`` workers, each with its own LZMA arena and scratch
  buffer.
- **Disk-backed tile buffers.** ``tile_data_map_*`` pre-sizes one temp file
  and mmaps it, so per-tile buffers live in the page cache instead of heap
  memory.
- **Robust I/O error handling.** All critical writes route through
  ``fatal_file_error()`` (``misc.c``), which prints ``maptool: <what>:
  <strerror>`` (e.g. "No space left on device"), removes temp files and
  exits cleanly instead of calling ``abort()`` on an assertion. Applied to
  ``item_bin_write``, all of ``zip.c``, the way-subsection writers, tile
  reference writes and ``tempfile()`` creation failures.

Roadmap
-------

The measures below are ordered by value/risk ratio. Items 1-4 are the four
measures; item 5 is the structural endgame that truly parallelizes the
compression burst.

.. _mtp_measure_fuse_10_11:

Measure 1 — fuse phases 10 and 11 (rewrites)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Problem.** Associated streets and house-number interpolations each do a
full streaming rewrite of ``ways_split`` (≈ 900 MB), ``nodes`` and
``way2poi_result``, back to back. That is two full ~1.5 GB write passes
plus the corresponding reads, and two rounds of ``tile_sizing_reset_file``.

**Design.** Both phases are "relations processing over the same inputs"
(``process_associated_streets`` and ``process_house_number_interpolations``
in ``osm.c``). Run both relation sets against ``ways_split``/``nodes``/
``way2poi_result`` in a **single read pass**, applying the two transforms
in order, and emit each output file once (one ``_relproc_tmp`` → final
rename). The ``tile_sizing_reset_file`` + ``tile_sizing_set_file`` pairs
collapse to one per output file.

**Ordering risk.** Phase 11 currently consumes phase 10's output, so fusion
is only byte-identical if (a) the two transforms are independent for the
final bytes, or (b) the combined pass applies them in a sub-pass structure
that reproduces the sequential result. This must be proven by the
byte-identical verification below, not assumed.

**Files.** ``maptool.c`` (phase orchestration), ``osm.c`` (the two
``process_*`` functions), ``osm_relations.c`` (relation funcs / write-through
semantics).

**Status: implemented and verified.** A single combined function
``process_associated_streets_and_house_number_interpolations``
(``osm.c``) runs the two relation sets against each rewritten file in one
pass: a collect pass assigns the associated street names and then the
house-number interpolation attributes (so the interpolation pass sees the
same street names the sequential flow produced), followed by a write pass
that emits each item once per associated-street membership and, for each
such copy, once per interpolation membership — exactly the byte sequence
the two sequential passes produced. ``relations_member_lookup``
(``osm_relations.c``) exposes the membership lists so both transforms can
run against the same ``item_bin``. ``tile_sizing_reset_file`` +
``tile_sizing_set_file`` collapse to one pair per output file.

**Ordering note.** The byte-identical check on the full niedersachsen map
must be run with ``-T 1``: ``process_multipolygons_setup`` and
``process_turn_restrictions_setup`` feed *one* shared async queue to
``thread_count`` workers, so the relation→thread assignment — and therefore
the item order in ``multipolygons_out`` — is nondeterministic run-to-run.
The ~42 % of tiles sourced from multipolygons differ between any two
default-threaded runs, fused or not. With ``-T 1`` the pipeline is
deterministic and the fusion output is byte-identical.

**Value.** Removes ~22 s (phase 10+11: 40 s → 18 s on the niedersachsen
map, ``-T 1``) and one full rewrite pass over each of the three files;
measured total wall time 313.4 s → 292.7 s.

**Verify.** Byte-identical output on the grid regression and the
niedersachsen map (run with ``-T 1``); phase 10+11 wall time roughly
halves.

.. _mtp_measure_parallel_phase13:

Measure 2 — parallelize the phase-13 read/dispatch
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Problem.** Phase 13's read loop is single-core: it parses ≈ 1.5 GB of
item files, computes each item's tile(s) and appends to the mmap'd tile
buffers. The compression pool only fills during the tail of that loop.

**Design.** Process the seven item files concurrently while preserving
deterministic per-tile item order (currently: file-loop order). Two options:

- *Per-file reader threads + per-tile ordered merge.* One thread per input
  file reads items and computes tiles; each produces ``(tile, serialized
  item)`` into per-tile queues. A single dispatcher merges per-tile in file
  order, appends to the tile buffer and calls ``tile_check_complete``.
- *Range-parallel within a file.* Split each file into ranges; workers
  parse each range into per-worker staging buffers; ordered concatenation
  per tile.

**Determinism requirement.** Output must stay byte-identical run-to-run,
which currently holds. Any parallel scheme must reproduce item order within
each tile exactly.

**Note.** ``read_item`` (``itembin_buffer.c``) reads into a single shared
20 MB static buffer; parallel readers need their own buffers.

**Files.** ``misc.c`` (``phase5``), ``itembin_buffer.c``, ``tile.c``.

**Value.** Removes the ~1 min single-core parse loop; also lets the
compression pool saturate earlier within phase 13.

**Verify.** Byte-identical output; measure phase-13 read-loop duration and
worker-queue occupancy.

.. _mtp_measure_overlap_tail:

Measure 3 — start compression during the tail of production
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Problem.** The ≈ 5 min LZMA burst is still a burst: it begins only when
phase 13 starts, and compression workers are idle while phases 1-11 run on
one core.

**Design.**

1. **Fold phase 12 into phase 13.** Phase 12 now only writes
   ``tilesdir``/``tilesdir_pos`` (the sizing pass made the file reads
   unnecessary). Remove the separate phase boundary so assembly starts the
   moment the last input file is finalized.
2. **Consume in production order.** The assembler currently reads files in
   a fixed order. Consuming files in *production order* lets tiles whose
   last contributing file is produced early (``coastline_result``,
   ``towns_poly``, ``relations``, ``multipolygons_out``) be dispatched to
   the compression pool immediately, overlapping LZMA with the remaining
   production phases (10/11). This requires the completion tracking
   (``completion_file``/``completion_pos``) to be defined over the
   production order, which the sizing machinery already computes.
3. **Honest ceiling.** Because most tiles complete only when ``ways_split``
   is final (constraint 2), the burst can be moved earlier by at most the
   duration of the phases that produce the *last* big file — a modest gain
   for a single-binfile output. The decisive step is Measure 5.

**Files.** ``maptool.c`` (phase 12/13 orchestration), ``misc.c``
(``phase4``/``phase5``), ``tile.c``.

**Value.** Shorter idle tail; compression overlaps with the last rewrites.

**Verify.** Byte-identical output; wall-time comparison; worker-pool
utilization trace.

.. _mtp_measure_parallel_phase1:

Measure 4 — parallelize phase 1 (input read + mapping)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Problem.** Phase 1 is the longest single-core phase (≈ 2 min, 20 % of
total). libosmium's ``Reader`` already overlaps decompression in a
background thread, but ``NavitHandler`` (``osm_libosmium.cpp``) feeds the
single-threaded ``osm_add_*``/``osm_end_*`` state machine in ``osm.c``,
which converts and writes items serially.

**Design.**

- Read PBF blobs with ``reader.read()`` and process each blob on a
  ``osmium::threads::ThreadPool`` worker.
- Split each entity's processing into a **convert** step (tag → attribute
  resolution, item construction — CPU-bound, thread-safe once isolated from
  the global state machine) and an **emit** step (ordered writes to
  ``coords.tmp``, the node buffer and the item temp files).
- Workers produce per-batch serialized item payloads; an ordered flusher
  emits them in blob order, keeping ``node_buffer`` and ``coords.tmp``
  strictly ordered (they are position-addressed).
- The osm.c ``osm_add_*``/``osm_end_*`` cycle must be made re-entrant for
  the duration of a single entity (it already is: the cycle ends in
  ``osm_end_*``); the global ``node_buffer``/``attr_strings`` state moves
  into the batch context or the ordered flusher.

**Files.** ``osm_libosmium.cpp``, ``osm.c`` (entity handling),
``maptool.c``.

**Value.** Largest single-phase speedup (2:00 → 0:30-1:00 expected on 4
cores). Highest refactor risk — done last.

**Verify.** Byte-identical output; phase-1 wall time; deterministic
``coords.tmp``/``nodes`` content (check the byte-identical zip comparison).

.. _mtp_measure_multi_bin:

Measure 5 (endgame) — parallel multi-binfile assembly
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Problem.** For a single output ZIP, the compression burst is structurally
serial (constraints 2 and 3).

**Design.** Assemble per-country (or per-submap) binfile members
concurrently. ``maptool`` already splits data by country
(``country_*.tmp``); if each country produced its own binfile (Navit
supports mapsets of several binfiles), each country's tiles could be
assembled and compressed by a dedicated worker thread/process starting the
moment that country's tiles complete. The 5-minute burst becomes an
N-core-wide pipeline whose wall time is bounded by the largest country, and
it can start while other countries are still being produced.

**Files.** ``maptool.c`` (output staging), ``zip.c`` (per-map zip
instances), ``misc.c`` (phase 5 split per country), plus Navit-side
verification of multi-map mapsets.

**Value.** The only design that genuinely parallelizes the compression
burst and moves it into the production pipeline.

**Verify.** Mapset loads and renders in Navit; per-country output compared
against the single-file baseline.

Sequencing and effort
---------------------

.. list-table::
   :header-rows: 1

   * - Step
     - Measure
     - Est. effort
     - Rationale
   * - 1
     - Fuse phases 10+11 (Measure 1)
     - ~ 1 day
     - smallest, safe, removes 2×900 MB I/O
   * - 2
     - Parallel phase-13 read (Measure 2)
     - ~ 1-2 days
     - bounded risk, removes single-core parse loop
   * - 3
     - Overlap compression with tail (Measure 3)
     - ~ 1-2 days
     - plumbing of existing sizing machinery
   * - 4
     - Parallel phase 1 (Measure 4)
     - ~ 2-3 days
     - biggest win, biggest refactor
   * - 5
     - Multi-binfile assembly (Measure 5)
     - separate initiative
     - structural, changes map delivery

Testing strategy
----------------

- **Byte-identical verification.** Every change must keep the produced
  ``.bin`` byte-identical (excluding the ZIP timestamp fields) to the
  pre-change build, checked on the small grid regression *and* the
  niedersachsen map. The existing comparison helpers in
  ``/tmp/opencode`` (``compare_zips.py``, ``compare_zip_items.py``) cover
  this.
- **Determinism.** Run-to-run output must be identical (same timestamp
  argument). The parallel designs in Measures 2 and 4 explicitly preserve
  item order within each tile/file.

  .. note:: **Current nondeterminism in the relation-splitting phases.**
     ``process_multipolygons_setup`` and ``process_turn_restrictions_setup``
     distribute relations to workers through a single shared async queue,
     so the relation→thread assignment (and the item order of
     ``multipolygons_out`` / the turn-restriction files) varies run-to-run
     whenever ``-T > 1``. Any two full-map runs therefore differ in the
     multipolygon-sourced tiles. Byte-identity checks on the full map must
     use ``-T 1`` (deterministic), and the grid regression is used for the
     default-threaded case. Fixing this race is a prerequisite for
     run-to-run reproducible multi-threaded builds (and would make
     Measure 2/3/4 verification trivially comparable).
- **Performance.** Phase wall-times are visible in the ``PROGRESS`` output.
  Track phase 1, phase 4, phase 13, total wall time, peak RSS and temp-dir
  disk usage before/after each measure.
- **Failure paths.** After Measure on error handling, verify a full-disk
  scenario reports "No space left on device" and cleans up temp files
  instead of aborting.
