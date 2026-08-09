.. _streaming_maptool_relations:

Streaming maptool relation processing (memory-bounded)
=======================================================

The multipolygon and turn-restriction phases of ``maptool`` are the only
pipeline stages whose memory use grows with the *whole input*, not with a
slice or a single tile. At planet.pbf scale they hold multiple GB in heap
and are the reason a full-planet build dies on an 8 GB machine. This
document describes the current structure, why a mmap'd hash is the wrong
fix, and a sorted-stream merge-join design that makes both phases
memory-bounded without changing the produced map.

.. contents:: Contents
   :depth: 2
   :local:

Current structure and why it is unbounded
-----------------------------------------

Both phases share the same skeleton (``osm.c`` ``process_multipolygons`` /
``process_turn_restrictions``, using ``osm_relations.c``):

1. **Setup.** ``process_*_setup`` reads the relation file and, for every
   member of every relation, inserts one ``relations_member`` into a
   ``GHashTable`` keyed by member id, whose value is a per-id ``GList`` of
   memberships (``relations_add_relation_member_entry``,
   ``osm_relations.c``). One hash is built **per worker thread**.
2. **Process.** ``relations_process`` / ``relations_process_multi`` scan
   ``ways_split`` (and for turn restrictions also the node coordinate file)
   once per thread, and do a hash lookup per item.
3. **Finish.** The relation payloads accumulated during processing are
   finalized into the ``multipolygons_out`` / ``relations`` output files.

The resident data that scales with the planet:

- **Membership hash.** ≈ 50 M entries for the planet's multipolygon way
  members alone. Each entry costs roughly a ``relations_member``
  (32 B) + a ``GList`` node (24 B) + a hash bucket (≈ 40 B) ≈ **96 B**,
  i.e. ≈ **5 GB**, and it exists once **per thread**.
- **Member way copies.** ``process_multipolygons_member`` does
  ``item_bin_dup(member)`` for every outer/inner way
  (``osm.c``:3416/3422), so all multipolygon member ways - full coordinate
  lists included - are resident at once. This is the single largest
  consumer (tens of GB at planet scale).
- **Relation payloads.** One ``struct multipolygon`` per relation
  (including a dup of the whole relation item) and one ``struct
  turn_restriction`` per restriction, with `g_renew`-grown coordinate
  arrays.

Net effect: peak RSS during the multipolygon phase is on the order of
8-15 GB for a planet build, before the tile buffer and node table even
start.

Why not a mmap'd hash
---------------------

The obvious reflex - "the node table and tile buffers are mmap'd, do the
same here" - does not transfer. The membership structure is a growable,
open-addressing ``GHashTable`` whose values are variable-length,
pointer-linked ``GList`` chains. A file-backed mmap region is a fixed
address range that cannot be rehashed/relocated in place, and the hash
uses C pointers rather than offsets. Rewriting it as a disk structure
means replacing the hash with offsets and sorted storage, i.e. a B-tree,
with strictly more complexity than a sorted merge-join and no memory
advantage.

The sorted-stream design below does nevertheless use mmap where it helps:
all intermediate tables are plain files read through mmap, so they live in
the page cache and RSS stays bounded.

Design overview
---------------

Replace the membership hash with **two sorted streams and a merge-join**:

::

    relations file (multipolygons / turn_restrictions)
       │  setup pass (streaming)
       ├──► relp.tmp        payloads, in input order (= seq order)
       └──► memb.tmp        (type, id, seq, role) records
              │ external sort by (type, id)
              ▼
          memb_sorted ──┐
                        │ merge-join (sequential)
    ways_split ──► ways_sorted ──┘  ──► parts.tmp  (seq, offset, role, item)
              (sort by wayid,           │ external sort by (seq, offset)
               original offset)         ▼
                                   parts_sorted ──► streaming assembly
                                                        (one relation at a
                                                         time) ──► output

All four files are streams; nothing is keyed in RAM. The join is a single
sequential pass over the two sorted inputs (no random seeks), and the
assembly is a sequential scan over the sorted parts.

The phase entry points choose between this sorted path and the existing
in-RAM hash path adaptively: a cheap counting pre-pass over the (small)
relations file estimates the membership size, and if ``members × 96 B +
member payload`` stays below the ``-S`` memory budget the existing hash
implementation runs unchanged (keeping regional builds as fast as today);
above it, the external-sort path is used. ``MAPTOOL_RELATIONS_MODE``
forces either implementation for testing.

Records
-------

Fixed-size records where possible:

- **Membership** ``(osmid id; int32 type; int64 seq; int32 role)``, 24 B.
  ``seq`` is the relation's position in the input file (its payload index).
- **Sorted ways** ``(osmid wayid; int64 original_offset; int32 len;
  char item[])`` - the full way item plus its byte offset in the original
  ``ways_split``. Sorted by ``(wayid, original_offset)``.
- **Part** ``(int64 seq; int64 original_offset; int32 role; int32 len;
  char item[])`` - the item bytes a membership matched. Sorted by
  ``(seq, original_offset)``.

The ``original_offset`` field is the byte-identity keystone: it preserves,
inside a relation, the exact order in which the current code appends
members as it scans ``ways_split``. ``find_loops`` builds its sequences
from that order, so the polygon output is byte-identical.

Pipeline
--------

1. **Setup** (streaming over the relations file): assign ``seq``, write
   the relation payload to ``relp.tmp`` (already in ``seq`` order), emit
   one membership record per member with its role (outer/inner, or
   from/to/via).
2. **External sort** ``memb.tmp`` by ``(type, id)``.
3. **External sort** the way items of ``ways_split`` by ``(wayid,
   original_offset)``. This is built once and reused by both the
   turn-restriction (phase 8) and multipolygon (phase 9) phases, since
   both consume the same pre-rewrite ``ways_split``.
4. **Merge-join** ``memb_sorted`` and ``ways_sorted``: for every way that
   is a member of a relation, emit a part record (respecting the existing
   ``attr_duplicate`` skip from ``process_multipolygons_member``). A way
   shared by several relations yields one part per membership, exactly
   like today's per-membership GList entries.
5. **External sort** parts by ``(seq, original_offset)``.
6. **Streaming assembly**: read parts grouped by ``seq``; rebuild that one
   relation's inner/outer ``item_bin`` lists; run the existing
   ``process_multipolygons_find_loops`` / finish logic; write its polygons
   to ``multipolygons_out`` with ``item_bin_write`` +
   ``tile_sizing_write_file``; free. Read ``relp.tmp`` in lockstep (it is
   in ``seq`` order, so this is sequential).

Memory is now bounded by the **largest single relation**, not the planet:
worst case is a few hundred MB even for the biggest multipolygons.

Turn restrictions
-----------------

Per-relation state is tiny (``struct turn_restriction`` holds a handful of
coordinates), so at planet scale all payloads together are ≈ 300 MB. The
way-membership side uses the same sorted mechanism above. Via **node**
members are the only extra wrinkle; instead of sorting the ≈ 120 GB
``coords.tmp``, keep a small in-RAM hash for via-node members only (≈
2-5 M entries, ≈ 200-400 MB). This makes the phase fit comfortably while
avoiding a full external sort of the node table.

Determinism
-----------

Today the relation→thread assignment is a shared async queue, so with
``-T > 1`` the output of ``multipolygons_out`` (and the turn-restriction
files) is nondeterministic run-to-run, which is why the existing pipeline
document forces byte-identity checks onto ``-T 1``. The sorted design has
no thread assignment: every sort is total on its full key, so the output
is byte-identical for any ``-T``. This removes the caveat.

Parallelism
-----------

The merge-join and the assembly are sequential single-stream passes; like
the phase-13 read loop they are expected to be I/O bound. The external
sorts can use the existing ``thread_count`` worker pool for parallel
run-building and k-way merge, which is where the CPU goes.

Shared external sorter
----------------------

A small, reusable external merge sort (~250 lines) is needed for the
fixed-size membership/part keys and the length-prefixed way items. Navit
already links libosmium, but ``osmium::sort`` operates on osmium entity
buffers, not raw records, so a purpose-built sorter is the pragmatic
choice. It is unit-testable in isolation (feed records in, get the sorted
file out) and should accept a memory budget and thread count.

Memory and disk budget (planet, ≈ estimates)
--------------------------------------------

.. list-table::
   :header-rows: 1

   * - Resource
     - Current
     - Design
   * - Peak RSS, multipolygon phase
     - ≈ 8-15 GB (crashes 8 GB)
     - ≈ largest single relation (+ page cache)
   * - Peak RSS, turn-restriction phase
     - ≈ 1-2 GB × threads
     - ≈ 0.3-0.7 GB
   * - Disk, membership file
     - n/a (RAM)
     - ≈ 1.2 GB
   * - Disk, parts file
     - n/a (RAM)
     - ≈ 10-50 GB
   * - Disk, sorted ways
     - n/a
     - ≈ 30-60 GB
   * - Extra wall time
     - n/a
     - a few extra sequential passes over the way items

The cost is disk and wall time, not RAM.

Measured baseline (niedersachsen, current code)
-----------------------------------------------

Measurements of the current (pre-rewrite) pipeline on the niedersachsen
extract (4 threads, 20.25 M ways) put the cost of both phases in
perspective:

.. list-table::
   :header-rows: 1

   * - Quantity
     - Measured
   * - Full build wall time (fused relation pipeline)
     - 292.7 s
   * - Turn-restriction phase (phase 8)
     - ≈ 20 s
   * - Multipolygon phase (phase 9)
     - ≈ 10 s
   * - Multipolygon memberships (hash entries)
     - ≈ 129 k (≈ 14 MB in the current per-thread hash)
   * - Way split / attribute phase (phase 4)
     - ≈ 37 s

So the two relation phases are only ≈ 10 % of a regional build's wall
time, and their in-RAM membership hashes are tiny at this scale. That is
exactly the regime the adaptive fast path (below) must keep on the
existing hash implementation. The external-sort path only becomes the
chosen implementation once the estimated membership size crosses the
memory budget (``-S``/``slice_size``), i.e. at planet scale, where the
current code grows to multiple GB per thread and dies on an 8 GB machine.

Verification
------------

- **Byte-identical output.** The produced ``.bin`` must be byte-identical
  (minus ZIP timestamps) to the pre-change build on the grid regression
  and the niedersachsen map, using the existing comparison helpers in
  ``/tmp/opencode`` (``compare_zips.py``, ``compare_zip_items.py``).
- **Determinism.** Two runs with identical timestamp must be identical at
  every ``-T``, not only ``-T 1``.
- **Memory.** Track peak RSS of the multipolygon and turn-restriction
  phases before/after on a region that currently peaks above the machine's
  budget; verify the RSS is flat as input size grows.
- **Failure paths.** Sort/join I/O must route through ``fatal_file_error``
  like the rest of the pipeline.

Sequencing and effort
---------------------

.. list-table::
   :header-rows: 1

   * - Step
     - Measure
     - Est. effort
     - Rationale
   * - 1
     - External sorter + multipolygon rewrite
     - ~ 3-5 days
     - the OOM culprit; self-contained
   * - 2
     - Turn-restriction rewrite (reuse sorter + sorted ways)
     - ~ 1-2 days
     - smaller; reuses step 1 machinery
   * - 3
     - Adaptive in-RAM fast path for small inputs
     - ~ 0.5 days
     - keeps regional builds fast

Risks and mitigations
---------------------

- **Byte-order risk.** Member order inside a relation is order-sensitive
  for ``find_loops``; the ``original_offset`` tie-breaker preserves it. If
  a divergence is ever found, the fallback is per-polygon (canonical)
  comparison rather than byte identity.
- **I/O cost.** Sorting the way items adds sequential passes over a
  30-60 GB file. Mitigation: build the sorted ways file once and reuse it
  across both relation phases.
- **Largest-relation spike.** A single pathological multipolygon (tens of
  thousands of ways) bounds the assembly window; it is a fixed constant
  regardless of planet size, so it does not reopen the memory problem.
