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

#include "extsort.h"
#include "debug.h"
#include "maptool.h"
#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#    include <unistd.h>
#endif

/** Minimum in-memory buffer before records spill to a run file. */
#define EXTSORT_MIN_BUDGET (32 * 1024 * 1024)
/** Records are length-prefixed by an int32 in memory, in runs and in output. */
#define EXTSORT_LEN_HEADER 4

/** Descriptor of one record inside the in-memory buffer. */
struct ext_sort_desc {
    long long off;
    int len;
};

struct ext_sort {
    long long budget;
    ext_sort_cmp_func cmp;
    void *user;
    char *base; /* in-memory record buffer */
    long long buf_len;
    long long buf_cap;
    struct ext_sort_desc *desc;
    int desc_count;
    int desc_cap;
    GPtrArray *runs; /* char* run file names */
    int run_number;
    long long record_count;
};

static void ext_sort_rec_from_desc(const struct ext_sort_desc *d, char *base, struct ext_sort_rec *rec) {
    rec->data = base + d->off + EXTSORT_LEN_HEADER;
    rec->len = d->len;
}

static int ext_sort_desc_cmp(const void *a, const void *b, void *user) {
    struct ext_sort *s = user;
    struct ext_sort_rec ra, rb;
    ext_sort_rec_from_desc(a, s->base, &ra);
    ext_sort_rec_from_desc(b, s->base, &rb);
    return s->cmp(&ra, &rb, s->user);
}

static void ext_sort_write_run(FILE *out, const void *data, int len) {
    if (fwrite(&len, sizeof(len), 1, out) != 1 || (len && fwrite(data, len, 1, out) != 1))
        fatal_file_error("writing sort run data failed");
}

struct ext_sort *ext_sort_new(long long mem_budget, ext_sort_cmp_func cmp, void *user) {
    struct ext_sort *s = g_new0(struct ext_sort, 1);
    s->budget = mem_budget > EXTSORT_MIN_BUDGET ? mem_budget : EXTSORT_MIN_BUDGET;
    s->cmp = cmp;
    s->user = user;
    s->runs = g_ptr_array_new();
    s->buf_cap = 1024 * 1024;
    s->base = g_malloc(s->buf_cap);
    s->desc_cap = 1024;
    s->desc = g_malloc(sizeof(struct ext_sort_desc) * s->desc_cap);
    return s;
}

static void ext_sort_flush(struct ext_sort *s) {
    int i;
    char *name;
    FILE *run;
    if (s->desc_count == 0)
        return;
    g_qsort_with_data(s->desc, s->desc_count, sizeof(struct ext_sort_desc), ext_sort_desc_cmp, s);
    name = g_strdup_printf("%s/extsort_%p_%d.run", tempfile_obtain_prefix(), (void *)s, s->run_number++);
    run = fopen(name, "wb");
    if (!run)
        fatal_file_error(name);
    for (i = 0; i < s->desc_count; i++) {
        struct ext_sort_desc *d = &s->desc[i];
        ext_sort_write_run(run, s->base + d->off + EXTSORT_LEN_HEADER, d->len);
    }
    if (fclose(run))
        fatal_file_error("closing sort run file failed");
    g_ptr_array_add(s->runs, name);
    s->buf_len = 0;
    s->desc_count = 0;
}

void ext_sort_add(struct ext_sort *s, const void *data, int len) {
    long long need;
    if (len < 0)
        len = 0;
    need = s->buf_len + EXTSORT_LEN_HEADER + len;
    if (need > s->buf_cap) {
        while (need > s->buf_cap)
            s->buf_cap *= 2;
        s->base = g_realloc(s->base, s->buf_cap);
    }
    *(int *)(s->base + s->buf_len) = len;
    if (len)
        memcpy(s->base + s->buf_len + EXTSORT_LEN_HEADER, data, len);
    if (s->desc_count == s->desc_cap) {
        s->desc_cap *= 2;
        s->desc = g_realloc(s->desc, sizeof(struct ext_sort_desc) * s->desc_cap);
    }
    s->desc[s->desc_count].off = s->buf_len;
    s->desc[s->desc_count].len = len;
    s->desc_count++;
    s->buf_len = need;
    s->record_count++;
    if (s->buf_len >= s->budget)
        ext_sort_flush(s);
}

/** One in-flight record of a run during the k-way merge. */
struct ext_sort_merge_run {
    FILE *f;
    char *buf;
    int cap;
    int len;
    int active;
};

static int ext_sort_merge_run_read(struct ext_sort_merge_run *r) {
    int len = 0;
    if (fread(&len, sizeof(len), 1, r->f) != 1) {
        r->active = 0;
        return 0;
    }
    if (len < 0)
        fatal_file_error("corrupt sort run record");
    if (r->cap < len) {
        r->cap = len ? len : 1;
        r->buf = g_realloc(r->buf, r->cap);
    }
    if (len && fread(r->buf, len, 1, r->f) != 1)
        fatal_file_error("reading sort run data failed");
    r->len = len;
    r->active = 1;
    return 1;
}

/** Binary min-heap over active runs, ordered by the user comparator. */
struct ext_sort_heap {
    int *idx; /* run indices */
    int count;
    int cap;
    struct ext_sort *s;
    struct ext_sort_merge_run *runs;
};

static int ext_sort_heap_less(struct ext_sort_heap *h, int a, int b) {
    struct ext_sort_merge_run *ra = &h->runs[h->idx[a]];
    struct ext_sort_merge_run *rb = &h->runs[h->idx[b]];
    struct ext_sort_rec reca, recb;
    int c;
    reca.data = ra->buf;
    reca.len = ra->len;
    recb.data = rb->buf;
    recb.len = rb->len;
    c = h->s->cmp(&reca, &recb, h->s->user);
    if (c != 0)
        return c < 0;
    /* stable: earlier run wins on equal keys */
    return h->idx[a] < h->idx[b];
}

static void ext_sort_heap_sift_down(struct ext_sort_heap *h, int i) {
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, m = i;
        if (l < h->count && ext_sort_heap_less(h, l, m))
            m = l;
        if (r < h->count && ext_sort_heap_less(h, r, m))
            m = r;
        if (m == i)
            return;
        int tmp = h->idx[i];
        h->idx[i] = h->idx[m];
        h->idx[m] = tmp;
        i = m;
    }
}

static long long ext_sort_merge(struct ext_sort *s, FILE *out) {
    int i, k = s->runs->len;
    long long written = 0;
    struct ext_sort_heap heap;
    memset(&heap, 0, sizeof(heap));
    heap.s = s;
    heap.runs = g_new0(struct ext_sort_merge_run, k);
    for (i = 0; i < k; i++) {
        char *name = g_ptr_array_index(s->runs, i);
        heap.runs[i].f = fopen(name, "rb");
        if (!heap.runs[i].f)
            fatal_file_error(name);
        if (ext_sort_merge_run_read(&heap.runs[i]))
            heap.count++;
    }
    if (heap.count > 0) {
        heap.cap = heap.count;
        heap.idx = g_malloc(sizeof(int) * heap.cap);
        /* build heap bottom-up */
        for (i = 0; i < heap.count; i++)
            heap.idx[i] = i;
        for (i = heap.count / 2 - 1; i >= 0; i--)
            ext_sort_heap_sift_down(&heap, i);
        while (heap.count > 0) {
            int run = heap.idx[0];
            struct ext_sort_merge_run *r = &heap.runs[run];
            ext_sort_write_run(out, r->buf, r->len);
            written++;
            if (ext_sort_merge_run_read(r)) {
                ext_sort_heap_sift_down(&heap, 0);
            } else {
                heap.idx[0] = heap.idx[--heap.count];
                if (heap.count > 0)
                    ext_sort_heap_sift_down(&heap, 0);
            }
        }
    }
    for (i = 0; i < k; i++) {
        fclose(heap.runs[i].f);
        g_free(heap.runs[i].buf);
    }
    g_free(heap.runs);
    g_free(heap.idx);
    return written;
}

long long ext_sort_finish(struct ext_sort *s, FILE *out) {
    long long written;
    ext_sort_flush(s);
    if (s->runs->len == 0)
        return 0;
    written = ext_sort_merge(s, out);
    return written;
}

void ext_sort_destroy(struct ext_sort *s) {
    guint i;
    for (i = 0; i < s->runs->len; i++) {
        char *name = g_ptr_array_index(s->runs, i);
        unlink(name);
        g_free(name);
    }
    g_ptr_array_free(s->runs, TRUE);
    g_free(s->base);
    g_free(s->desc);
    g_free(s);
}
