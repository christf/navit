/*
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2011 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#ifndef NAVIT_EXTSORT_H
#define NAVIT_EXTSORT_H

#include <stdio.h>

/** A record handed to the sort comparator.
 *
 *  Only valid during the comparator invocation, and only for reading.
 */
struct ext_sort_rec {
    const void *data;
    int len;
};

/** Comparator signature.
 *
 *  @returns <0, 0 or >0 like qsort's comparator.
 */
typedef int (*ext_sort_cmp_func)(const struct ext_sort_rec *a, const struct ext_sort_rec *b, void *user);

/** Opaque external merge sorter. */
struct ext_sort;

/** Create an external merge sorter.
 *
 *  Records added via ext_sort_add() are kept in memory until the buffer
 *  reaches @p mem_budget bytes, then flushed as a sorted run file. On
 *  ext_sort_finish() all runs are k-way merged into @p out, so memory use
 *  stays bounded regardless of how many records are added.
 *
 *  The sort is stable: records with equal sort keys come out in insertion
 *  order.
 *
 *  @param mem_budget  maximum bytes buffered in RAM before spilling.
 *  @param cmp         comparator deciding the sort order.
 *  @param user        passed through to @p cmp.
 */
struct ext_sort *ext_sort_new(long long mem_budget, ext_sort_cmp_func cmp, void *user);

/** Add a record to the sorter.
 *
 *  The data is copied; it does not have to stay valid afterwards.
 */
void ext_sort_add(struct ext_sort *s, const void *data, int len);

/** Flush remaining data and merge all runs into @p out.
 *
 *  The output file is a sequence of records in [int32 len][data] format.
 *  After this call the sorter is empty and may be destroyed or reused.
 *
 *  @returns number of records written, or -1 on error.
 */
long long ext_sort_finish(struct ext_sort *s, FILE *out);

/** Free all resources of the sorter. Run files are removed. */
void ext_sort_destroy(struct ext_sort *s);

#endif /* MAVIT_EXTSORT_H */
