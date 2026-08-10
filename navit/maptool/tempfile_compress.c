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

/* Temporary-file block compression.
 *
 * With -Q (tempfile compression) every temp file is stored as a sequence of
 * independent, fixed-size uncompressed blocks, each compressed separately and
 * appended to the physical file. A footer at the end maps logical block
 * numbers to (usize, csize, physical offset). Compressed files are exposed to
 * the rest of the pipeline as ordinary FILE* streams through a glibc
 * fopencookie shim, so none of the existing read/write call sites change.
 *
 * The compression itself reuses the phase-13/14 tile infrastructure: a worker
 * pool (one thread per core, per-worker scratch buffer and lzma allocator,
 * GAsyncQueue in/out) plus the one-shot compress_for_zip primitive from
 * zip.c, wrapped as compress_block(). A per-file writer thread consumes the
 * completed blocks and writes them to disk in any order (the footer records
 * offsets, so physical order is irrelevant).
 *
 * The only temp file rewritten in place is coords.tmp (the node table), whose
 * 1 GB slices are overwritten by save_buffer(). Overwritten ranges are
 * recompressed and appended, the block map entries updated, and the old bytes
 * become garbage; tf_compact() reclaims that garbage once after phase 2.
 *
 * The state needed to serve a stream (current position, read cache) lives in
 * a per-open tf_handle, while the compressed file itself is kept as a single
 * cached tf_file entry per path. Entries stay alive across open/close cycles
 * until they are finalized (tf_compact, tempfile_unlink/rename, or
 * tf_compress_fini at exit), so repeated open/close of the same file - the
 * per-slice save_buffer/load_buffer pattern on coords.tmp - no longer pays
 * for a writer-thread create+join, a full footer read, or an O(block_count)
 * block-map walk. The footer is only (re)written when an entry is finalized;
 * with -k, crash left-overs therefore may lack a valid footer.
 *
 * The cache and all tf_fopen/tf_compact/tempfile_* calls are used only from
 * the main thread (the compression pool and per-file writer threads only touch
 * the pool queues, the entry locks and the shared phys FILE*), so the cache
 * list itself needs no lock.
 */

#define _GNU_SOURCE 1 /* exposes fopencookie in <stdio.h> */

#include "config.h"
#include "debug.h"
#include "maptool.h"
#include "zipfile.h"
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef HAVE_LZMA
#    include <lzma.h>
#endif
#ifdef HAVE_ZSTD
#    include <zstd.h>
#endif
#ifndef _MSC_VER
#    include <unistd.h>
#endif

int tf_compression_method = TF_CODEC_NONE;
int tf_compression_level = 0;
long long tf_block_size = 32 * 1024 * 1024;

#define TF_MAGIC 0x4d544331u /* "MTC1" */
#define TF_VERSION 1
/* Logical blocks mapped to their physical location. */
struct tf_block {
    unsigned int usize;
    unsigned int csize;
    long long file_off;
};

/* ---------------- codecs ---------------- */

int tf_codec_from_zip(int zipmthd) {
    switch (zipmthd) {
    case ZIP_COMPRESSION_DEFLATE:
        return TF_CODEC_ZLIB;
    case ZIP_COMPRESSION_LZMA:
        return TF_CODEC_LZMA;
    default:
        return TF_CODEC_NONE;
    }
}

int tf_codec_to_zipmthd(int codec) {
    switch (codec) {
    case TF_CODEC_ZLIB:
        return ZIP_COMPRESSION_DEFLATE;
    case TF_CODEC_LZMA:
        return ZIP_COMPRESSION_LZMA;
    default:
        return ZIP_COMPRESSION_STORED;
    }
}

/* One-shot block compressor. Reuses compress_for_zip (zip.c) for zlib and
 * lzma so the output is byte-identical to the phase-13/14 tile compression;
 * zstd is handled here. Returns a malloc'd buffer, or NULL (and
 * *out_method = TF_CODEC_NONE) when the block is not compressible. */
char *compress_block(char *input, int input_size, int level, int method, int *out_size, int *out_method,
                     char **reuse_buf, size_t *reuse_size, void *lzma_alloc) {
    if (method == TF_CODEC_ZSTD) {
#ifdef HAVE_ZSTD
        if (level <= 0)
            level = 3;
        size_t cap = ZSTD_compressBound((size_t)input_size);
        if (*reuse_size < cap) {
            *reuse_buf = g_realloc(*reuse_buf, cap);
            *reuse_size = cap;
        }
        size_t r = ZSTD_compress(*reuse_buf, cap, input, (size_t)input_size, level);
        if (!ZSTD_isError(r) && r < (size_t)input_size) {
            char *result = g_malloc(r);
            memcpy(result, *reuse_buf, r);
            *out_size = (int)r;
            *out_method = TF_CODEC_ZSTD;
            return result;
        }
#endif
        *out_size = input_size;
        *out_method = TF_CODEC_NONE;
        return NULL;
    }

    int zip_method;
    switch (method) {
    case TF_CODEC_ZLIB:
        zip_method = ZIP_COMPRESSION_DEFLATE;
        if (level <= 0)
            level = 6;
        break;
    case TF_CODEC_LZMA:
        zip_method = ZIP_COMPRESSION_LZMA;
        if (level <= 0)
            level = 6;
        break;
    default:
        *out_size = input_size;
        *out_method = TF_CODEC_NONE;
        return NULL;
    }
    int zipmthd = 0;
    char *comp = compress_for_zip(input, input_size, level, zip_method, out_size, &zipmthd, reuse_buf, reuse_size,
                                  lzma_alloc);
    *out_method = comp ? method : TF_CODEC_NONE;
    return comp;
}

/* One-shot block decompressor for the codecs above. Returns 0 on success. */
int decompress_block(char *src, int csize, char *dst, int usize, int method) {
    switch (method) {
    case TF_CODEC_ZLIB: {
        z_stream stream;
        memset(&stream, 0, sizeof(stream));
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
            return -1;
        stream.next_in = (Bytef *)src;
        stream.avail_in = (uInt)csize;
        stream.next_out = (Bytef *)dst;
        stream.avail_out = (uInt)usize;
        int err = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        return (err == Z_STREAM_END && stream.total_out == (uLong)usize) ? 0 : -1;
    }
#ifdef HAVE_LZMA
    case TF_CODEC_LZMA: {
        uint64_t memlimit = UINT64_MAX;
        size_t in_pos = 0;
        size_t out_pos = 0;
        size_t out_size = usize;
        lzma_ret err = lzma_stream_buffer_decode(&memlimit, 0, NULL, (const uint8_t *)src, &in_pos, (size_t)csize,
                                                 (uint8_t *)dst, &out_pos, out_size);
        return (err == LZMA_OK && out_pos == out_size) ? 0 : -1;
    }
#endif
#ifdef HAVE_ZSTD
    case TF_CODEC_ZSTD: {
        size_t r = ZSTD_decompress(dst, (size_t)usize, src, (size_t)csize);
        return (ZSTD_isError(r) || r != (size_t)usize) ? -1 : 0;
    }
#endif
    default:
        return -1;
    }
}

/* ---------------- compression pool ---------------- */

struct pool_worker {
    struct compress_pool *pool;
    GThread *thread;
    char *scratch;
    size_t scratch_size;
    void *allocator; /* lzma allocator when the pool codec is LZMA */
};

struct compress_pool {
    GAsyncQueue *in; /* compress_job* */
    int thread_count;
    int level;
    int method;
    struct pool_worker *workers;
};

/* Sentinel job that tells pool workers to exit (g_async_queue_push refuses
 * NULL data, so a real sentinel pointer is used). */
static struct compress_job tf_pool_stop_job;

static gpointer compress_pool_worker_fn(gpointer data) {
    struct pool_worker *w = (struct pool_worker *)data;
    struct compress_job *job;

    while ((job = (struct compress_job *)g_async_queue_pop(w->pool->in)) != NULL) {
        if (job == &tf_pool_stop_job)
            break;
        job->comp = compress_block(job->data, job->size, w->pool->level, w->pool->method, &job->comp_size,
                                   &job->comp_method, &w->scratch, &w->scratch_size, w->allocator);
        if (!job->comp) {
            job->comp = job->data;
            job->comp_method = TF_CODEC_NONE;
            job->comp_size = job->size;
        }
        if (job->crc_out)
            *job->crc_out = crc32(0, (const Bytef *)job->data, (uInt)job->size);
        g_async_queue_push(job->done_q, job);
    }
    g_thread_exit(NULL);
    return NULL;
}

struct compress_pool *compress_pool_new(int threads, int level, int method) {
    struct compress_pool *pool = g_new0(struct compress_pool, 1);
    int i;
    if (threads <= 0)
        threads = 1;
    pool->thread_count = threads;
    pool->level = level;
    pool->method = method;
    pool->in = g_async_queue_new();
    pool->workers = g_new0(struct pool_worker, threads);
    for (i = 0; i < threads; i++) {
        struct pool_worker *w = &pool->workers[i];
        w->pool = pool;
#ifdef HAVE_LZMA
        if (method == TF_CODEC_LZMA)
            w->allocator = lzma_allocator_create(level > 0 ? level : 6);
#endif
        w->thread = g_thread_new("compress-pool", compress_pool_worker_fn, w);
    }
    return pool;
}

void compress_pool_destroy(struct compress_pool *pool) {
    int i;
    if (!pool)
        return;
    for (i = 0; i < pool->thread_count; i++)
        g_async_queue_push(pool->in, &tf_pool_stop_job);
    for (i = 0; i < pool->thread_count; i++) {
        g_thread_join(pool->workers[i].thread);
        g_free(pool->workers[i].scratch);
#ifdef HAVE_LZMA
        lzma_allocator_destroy(pool->workers[i].allocator);
#endif
    }
    g_free(pool->workers);
    g_async_queue_unref(pool->in);
    g_free(pool);
}

void compress_pool_submit(struct compress_pool *pool, struct compress_job *job) {
    g_async_queue_push(pool->in, job);
}

/* ---------------- cookie FILE* layer ---------------- */

#if defined(__GLIBC__)

/* State shared by every open handle of one compressed temp file. Entries are
 * cached per path and finalized only when the file is replaced, deleted, or
 * at exit, so the block table, logical_size and phys_end stay authoritative in
 * memory across open/close cycles. */
struct tf_file {
    FILE *phys;
    int writable;
    int codec;
    int level;
    long long block_size;
    int block_count;
    long long logical_size;
    struct tf_block *blocks;
    /* logical_offs[i] = logical start offset of block i; the sentinel entry
     * logical_offs[block_count] == logical_size. Grows with every append, so
     * tf_pos_to_block can binary-search instead of walking the block table. */
    long long *logical_offs;

    /* append state */
    char *w_buf;
    long long w_len;

    GAsyncQueue *done_queue;
    struct compress_pool *pool;
    int pending;   /* jobs submitted but not yet written */
    int writer_stop;
    int io_error;
    int io_errno;
    char *dbg_path;
    GMutex lock;
    GCond cond;
    GThread *writer_thread;

    /* Serializes every operation on tf->phys: the writer thread's
     * fseeko+fwrite and the main thread's reads (tf_load_block) and footer
     * emission all share one FILE* and its position. */
    GMutex phys_lock;
    /* Logical end of the physical data stream. The writer writes at this
     * offset explicitly and advances it; it must not use ftello(tf->phys)
     * because tf_load_block on the main thread moves the shared FILE*
     * position while reading. */
    long long phys_end;

    /* cache linkage */
    char *path;
    int refcount;
    int in_cache;
    struct tf_file *next;
};

/* Per-open stream state. pos and the read cache are private to one handle;
 * the underlying tf_file is shared through the cache. */
struct tf_handle {
    struct tf_file *tf;
    long long pos;
    /* read cache; r_file_off/r_usize record which block revision the buffer
     * holds, so a block overwritten by another handle is reloaded. */
    int r_block;
    unsigned int r_usize;
    long long r_file_off;
    char *r_buf;
};

/* Maximum number of compressed blocks in flight per file before the write
 * path starts waiting for the writer thread. */
#define TF_MAX_INFLIGHT 12

static struct compress_pool *tf_pool = NULL;

static struct compress_pool *tf_pool_get(void) {
    if (!tf_pool)
        tf_pool = compress_pool_new(thread_count > 0 ? thread_count : 1, tf_compression_level, tf_compression_method);
    return tf_pool;
}

/* ---------------- per-path entry cache ---------------- */

static struct tf_file *tf_cache_head = NULL;

static struct tf_file *tf_cache_find(char *path) {
    struct tf_file *tf;
    for (tf = tf_cache_head; tf; tf = tf->next)
        if (!strcmp(tf->path, path))
            return tf;
    return NULL;
}

static void tf_cache_add(struct tf_file *tf) {
    tf->in_cache = 1;
    tf->next = tf_cache_head;
    tf_cache_head = tf;
}

static void tf_cache_remove(struct tf_file *tf) {
    struct tf_file **pp = &tf_cache_head;
    while (*pp && *pp != tf)
        pp = &(*pp)->next;
    if (*pp)
        *pp = tf->next;
    tf->next = NULL;
    tf->in_cache = 0;
}

/* On-disk footer. Header is 6 ints + long long = 32 bytes, followed by
 * block_count * struct tf_block (16 bytes each), then compressed_end (8) and
 * the tail magic (4). */
struct tf_footer {
    int magic;
    int version;
    int codec;
    int level;
    int block_size;
    int block_count;
    long long logical_size;
};

/* Submit one logical block to the pool; the writer thread writes it to the
 * physical file and records its index entry. seq >= 0 identifies the block's
 * logical index (both for appends, which are assigned their index at
 * submission time so out-of-order pool completion cannot remap blocks, and
 * for in-place overwrites of an existing block). */
static int tf_submit(struct tf_file *tf, int seq, char *data, int size) {
    struct compress_job *job = g_new0(struct compress_job, 1);
    job->seq = seq;
    job->data = data;
    job->size = size;
    job->done_q = tf->done_queue;
    g_mutex_lock(&tf->lock);
    while (!tf->io_error && tf->pending >= TF_MAX_INFLIGHT)
        g_cond_wait(&tf->cond, &tf->lock);
    if (tf->io_error) {
        int e = tf->io_errno;
        g_mutex_unlock(&tf->lock);
        g_free(job);
        g_free(data);
        errno = e;
        return -1;
    }
    tf->pending++;
    g_cond_broadcast(&tf->cond);
    g_mutex_unlock(&tf->lock);
    compress_pool_submit(tf->pool, job);
    return 0;
}

/* Append one logical block: assign it the next block index up front and make
 * room in the block table, so the writer thread can record its entry at that
 * index regardless of pool completion order. logical_size already includes
 * the appended bytes, so the block's logical start is logical_size - size. */
static int tf_submit_append(struct tf_file *tf, char *data, int size) {
    g_mutex_lock(&tf->lock);
    int idx = tf->block_count++;
    tf->blocks = g_realloc(tf->blocks, (size_t)tf->block_count * sizeof(struct tf_block));
    tf->logical_offs = g_realloc(tf->logical_offs, (size_t)(tf->block_count + 1) * sizeof(long long));
    tf->logical_offs[idx] = tf->logical_size - size;
    tf->logical_offs[idx + 1] = tf->logical_size;
    g_mutex_unlock(&tf->lock);
    return tf_submit(tf, idx, data, size);
}

static gpointer tf_writer_thread_fn(gpointer data) {
    struct tf_file *tf = (struct tf_file *)data;
    for (;;) {
        g_mutex_lock(&tf->lock);
        while (!tf->io_error && tf->pending <= 0 && !tf->writer_stop)
            g_cond_wait(&tf->cond, &tf->lock);
        if (tf->io_error || (tf->writer_stop && tf->pending <= 0)) {
            g_mutex_unlock(&tf->lock);
            break;
        }
        g_mutex_unlock(&tf->lock);

        struct compress_job *job = (struct compress_job *)g_async_queue_pop(tf->done_queue);
        if (!job)
            break;
        if (tf->io_error) {
            if (job->comp != job->data)
                g_free(job->comp);
            g_free(job->data);
            g_free(job);
            continue;
        }
        g_mutex_lock(&tf->phys_lock);
        long long off = tf->phys_end;
        if (getenv("TF_DEBUG"))
            fprintf(stderr, "[writer] seq=%d off=%lld usize=%d csize=%d\n", job->seq, off, job->size, job->comp_size);
        if (off < 0 || fseeko(tf->phys, off, SEEK_SET) || fwrite(job->comp, 1, job->comp_size, tf->phys) != (size_t)job->comp_size) {
            g_mutex_unlock(&tf->phys_lock);
            g_mutex_lock(&tf->lock);
            tf->io_error = 1;
            tf->io_errno = errno;
            g_cond_broadcast(&tf->cond);
            g_mutex_unlock(&tf->lock);
            if (job->comp != job->data)
                g_free(job->comp);
            g_free(job->data);
            g_free(job);
            g_mutex_lock(&tf->lock);
            tf->pending--;
            g_cond_broadcast(&tf->cond);
            g_mutex_unlock(&tf->lock);
            continue;
        }
        tf->phys_end = off + job->comp_size;
        g_mutex_unlock(&tf->phys_lock);
        g_mutex_lock(&tf->lock);
        tf->blocks[job->seq].usize = (unsigned int)job->size;
        tf->blocks[job->seq].csize = (unsigned int)job->comp_size;
        tf->blocks[job->seq].file_off = off;
        g_mutex_unlock(&tf->lock);
        if (job->comp != job->data)
            g_free(job->comp);
        g_free(job->data);
        g_free(job);
        g_mutex_lock(&tf->lock);
        tf->pending--;
        g_cond_broadcast(&tf->cond);
        g_mutex_unlock(&tf->lock);
    }
    g_thread_exit(NULL);
    return NULL;
}

/* Flush buffered append data and wait until the writer thread has written
 * everything, so reads and seeks see a consistent file. */
static int tf_sync_writes(struct tf_file *tf) {
    if (tf->writable && tf->w_len > 0) {
        char *data = g_malloc(tf->w_len);
        memcpy(data, tf->w_buf, tf->w_len);
        if (tf_submit_append(tf, data, tf->w_len))
            return -1;
        tf->w_len = 0;
    }
    g_mutex_lock(&tf->lock);
    while (!tf->io_error && tf->pending > 0)
        g_cond_wait(&tf->cond, &tf->lock);
    if (tf->io_error) {
        int e = tf->io_errno;
        g_mutex_unlock(&tf->lock);
        errno = e;
        return -1;
    }
    g_mutex_unlock(&tf->lock);
    return 0;
}

/* Decompress block into the handle's read cache. The block-table entry is
 * snapshotted under tf->lock (the writer thread updates it); the cached
 * buffer is only reused while the block's usize and file_off are unchanged,
 * so an overwrite performed through any handle forces a reload. */
static int tf_load_block(struct tf_handle *h, int block) {
    struct tf_file *tf = h->tf;
    struct tf_block b;
    g_mutex_lock(&tf->lock);
    b = tf->blocks[block];
    g_mutex_unlock(&tf->lock);
    if (getenv("TF_DEBUG"))
        fprintf(stderr, "[read] block=%d usize=%u csize=%u off=%lld codec=%d pos=%lld\n", block, b.usize, b.csize,
                b.file_off, tf->codec, h->pos);
    if (h->r_block == block && h->r_usize == b.usize && h->r_file_off == b.file_off)
        return 0;
    if (!h->r_buf)
        h->r_buf = g_malloc(tf->block_size);
    g_mutex_lock(&tf->phys_lock);
    int rc = 0;
    if (b.csize == (unsigned int)b.usize) {
        /* stored block */
        if (fseeko(tf->phys, b.file_off, SEEK_SET) || fread(h->r_buf, 1, b.csize, tf->phys) != b.csize)
            rc = -1;
    } else {
        char *cbuf = g_malloc(b.csize);
        if (fseeko(tf->phys, b.file_off, SEEK_SET) || fread(cbuf, 1, b.csize, tf->phys) != b.csize) {
            rc = -1;
        } else {
            rc = decompress_block(cbuf, b.csize, h->r_buf, b.usize, tf->codec);
        }
        g_free(cbuf);
    }
    g_mutex_unlock(&tf->phys_lock);
    if (rc)
        return -1;
    h->r_block = block;
    h->r_usize = b.usize;
    h->r_file_off = b.file_off;
    return 0;
}

/* Map a logical position to the block that contains it and the offset within
 * that block. Blocks are laid out contiguously in logical space, so this is a
 * binary search over logical_offs (blocks may be partial anywhere: a
 * mid-stream fseek/ftell on a writable stream flushes a partial block). */
static int tf_pos_to_block(struct tf_file *tf, long long pos, long long *start_out) {
    int lo = 0, hi = tf->block_count;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (tf->logical_offs[mid] <= pos)
            lo = mid;
        else
            hi = mid - 1;
    }
    *start_out = tf->logical_offs[lo];
    return lo;
}

static ssize_t tf_cookie_read(void *cookie, char *buf, size_t size) {
    struct tf_handle *h = (struct tf_handle *)cookie;
    struct tf_file *tf = h->tf;
    size_t done = 0;
    if (tf->writable && tf_sync_writes(tf))
        return -1;
    while (done < size && h->pos < tf->logical_size) {
        long long start;
        int block = tf_pos_to_block(tf, h->pos, &start);
        if (block >= tf->block_count)
            break;
        size_t off_in_block = (size_t)(h->pos - start);
        if (tf_load_block(h, block))
            return done ? (ssize_t)done : -1;
        size_t avail = (size_t)(tf->logical_offs[block + 1] - start) - off_in_block;
        size_t take = size - done < avail ? size - done : avail;
        memcpy(buf + done, h->r_buf + off_in_block, take);
        done += take;
        h->pos += take;
    }
    return done;
}

/* Overwrite a range that lies fully within one block. Returns the number of
 * bytes consumed (> 0), or -1 on error. */
static ssize_t tf_overwrite_one(struct tf_handle *h, const char *buf, size_t size) {
    struct tf_file *tf = h->tf;
    long long start;
    int block = tf_pos_to_block(tf, h->pos, &start);
    if (block >= tf->block_count)
        return -1;
    long long end = tf->logical_offs[block + 1];
    unsigned int usize = (unsigned int)(end - start);
    size_t take = size < (size_t)(end - h->pos) ? size : (size_t)(end - h->pos);
    if (take == 0)
        return -1;
    if (getenv("TF_DEBUG"))
        fprintf(stderr, "[ow] %s block=%d start=%lld usize=%u pos=%lld take=%zu %s\n", tf->dbg_path ? tf->dbg_path : "?",
                block, start, usize, h->pos, take, (h->pos == start && take == usize) ? "full" : "merge");
    if (h->pos == start && take == usize) {
        char *data = g_memdup2(buf, take);
        if (tf_submit(tf, block, data, take))
            return -1;
    } else {
        /* A partial overwrite must merge onto the block's latest content.
         * Earlier overwrites of this same block may still be queued in the
         * async pipeline, so flush all pending writes first, then read the
         * block at its (updated) physical location. */
        if (tf_sync_writes(tf))
            return -1;
        char *merged = g_malloc(usize);
        if (tf_load_block(h, block)) {
            g_free(merged);
            return -1;
        }
        memcpy(merged, h->r_buf, usize);
        memcpy(merged + (h->pos - start), buf, take);
        if (tf_submit(tf, block, merged, usize))
            return -1;
    }
    h->pos += take;
    return take;
}

/* Append raw bytes to the logical stream, buffering them into blocks and
 * submitting full blocks to the writer asynchronously. */
static int tf_append_bytes(struct tf_file *tf, struct tf_handle *h, const char *buf, size_t len) {
    while (len > 0) {
        if (!tf->w_buf)
            tf->w_buf = g_malloc(tf->block_size);
        size_t room = tf->block_size - tf->w_len;
        size_t take = len < room ? len : room;
        memcpy(tf->w_buf + tf->w_len, buf, take);
        tf->w_len += take;
        h->pos += take;
        tf->logical_size = h->pos;
        buf += take;
        len -= take;
        if (tf->w_len == tf->block_size) {
            char *data = tf->w_buf;
            tf->w_buf = NULL;
            if (tf_submit_append(tf, data, (int)tf->w_len))
                return -1;
            tf->w_buf = g_malloc(tf->block_size);
            tf->w_len = 0;
        }
    }
    return 0;
}

static ssize_t tf_cookie_write(void *cookie, const char *buf, size_t size) {
    struct tf_handle *h = (struct tf_handle *)cookie;
    struct tf_file *tf = h->tf;
    if (getenv("TF_DEBUG"))
        fprintf(stderr, "[write] %s pos=%lld lsize=%lld size=%zu\n", tf->dbg_path ? tf->dbg_path : "?", h->pos,
                tf->logical_size, size);
    if (h->pos > tf->logical_size) {
        /* Writing beyond the current end creates a gap, exactly as fseeko+write
         * on a plain file would. Fill it with zeros, appending them at the
         * current logical end so they land in [old_end, pos). */
        long long target = h->pos;
        h->pos = tf->logical_size;
        char zeros[4096];
        while (h->pos < target) {
            size_t take =
                (size_t)((target - h->pos) < (long long)sizeof(zeros) ? (target - h->pos) : (long long)sizeof(zeros));
            memset(zeros, 0, take);
            if (tf_append_bytes(tf, h, zeros, take))
                return -1;
        }
    }
    size_t done = 0;
    while (done < size) {
        if (h->pos == tf->logical_size) {
            if (tf_append_bytes(tf, h, buf + done, size - done))
                return done ? (ssize_t)done : -1;
            done = size;
        } else {
            ssize_t took = tf_overwrite_one(h, buf + done, size - done);
            if (took < 0)
                return done ? (ssize_t)done : -1;
            done += took;
        }
    }
    h->r_block = -1;
    return done;
}

static int tf_cookie_seek(void *cookie, off_t *offset, int whence) {
    struct tf_handle *h = (struct tf_handle *)cookie;
    struct tf_file *tf = h->tf;
    long long target;
    if (tf->writable && tf_sync_writes(tf))
        return -1;
    switch (whence) {
    case SEEK_SET:
        target = *offset;
        break;
    case SEEK_CUR:
        target = h->pos + *offset;
        break;
    case SEEK_END:
        target = tf->logical_size + *offset;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (target < 0) {
        errno = EINVAL;
        return -1;
    }
    h->pos = target;
    h->r_block = -1;
    *offset = target;
    return 0;
}

static int tf_write_footer(struct tf_file *tf) {
    struct tf_footer hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = TF_MAGIC;
    hdr.version = TF_VERSION;
    hdr.codec = tf->codec;
    hdr.level = tf->level;
    hdr.block_size = (int)tf->block_size;
    hdr.block_count = tf->block_count;
    hdr.logical_size = tf->logical_size;
    g_mutex_lock(&tf->phys_lock);
    if (getenv("TF_DEBUG"))
        fprintf(stderr, "[footer] %s bc=%d lsize=%lld pos=%lld blocks=%p\n", tf->dbg_path ? tf->dbg_path : "?",
                tf->block_count, tf->logical_size, tf->phys_end, (void *)tf->blocks);
    long long compressed_end = tf->phys_end;
    if (compressed_end < 0 || fseeko(tf->phys, compressed_end, SEEK_SET)) {
        g_mutex_unlock(&tf->phys_lock);
        return -1;
    }
    if (fwrite(&hdr, sizeof(hdr), 1, tf->phys) != 1) {
        g_mutex_unlock(&tf->phys_lock);
        return -1;
    }
    if (fwrite(tf->blocks, sizeof(struct tf_block), tf->block_count, tf->phys) != (size_t)tf->block_count) {
        g_mutex_unlock(&tf->phys_lock);
        return -1;
    }
    if (fwrite(&compressed_end, sizeof(compressed_end), 1, tf->phys) != 1) {
        g_mutex_unlock(&tf->phys_lock);
        return -1;
    }
    int tail = TF_MAGIC;
    if (fwrite(&tail, sizeof(tail), 1, tf->phys) != 1) {
        g_mutex_unlock(&tf->phys_lock);
        return -1;
    }
    int flush_rc = fflush(tf->phys);
    g_mutex_unlock(&tf->phys_lock);
    return flush_rc ? -1 : 0;
}

/* Read the footer from f into tf. Returns 1 on a compressed file, 0 on a
 * plain file, -1 on a corrupt/truncated footer. On success the per-block
 * logical offsets are rebuilt and validated against logical_size. */
static int tf_read_footer(struct tf_file *tf, FILE *f) {
    int tail;
    if (fseeko(f, -4, SEEK_END) || fread(&tail, sizeof(tail), 1, f) != 1)
        return 0; /* too small for a footer: treat as a plain (empty) file */
    if (tail != (int)TF_MAGIC)
        return 0;
    long long compressed_end;
    if (fseeko(f, -12, SEEK_END) || fread(&compressed_end, sizeof(compressed_end), 1, f) != 1)
        return -1;
    if (compressed_end < 0 || fseeko(f, compressed_end, SEEK_SET))
        return -1;
    struct tf_footer hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (fread(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (hdr.magic != (int)TF_MAGIC || hdr.version != TF_VERSION || hdr.block_size <= 0 || hdr.block_count < 0)
        return -1;
    tf->codec = hdr.codec;
    tf->level = hdr.level;
    tf->block_size = hdr.block_size;
    tf->block_count = hdr.block_count;
    tf->logical_size = hdr.logical_size;
    tf->blocks = g_new0(struct tf_block, tf->block_count);
    if (tf->block_count && fread(tf->blocks, sizeof(struct tf_block), tf->block_count, f) != (size_t)tf->block_count)
        return -1;
    tf->logical_offs = g_new0(long long, tf->block_count + 1);
    long long acc = 0;
    int i;
    for (i = 0; i < tf->block_count; i++) {
        tf->logical_offs[i] = acc;
        acc += tf->blocks[i].usize;
    }
    tf->logical_offs[tf->block_count] = acc;
    if (acc != tf->logical_size)
        return -1;
    return 1;
}

/* Free the in-memory parts of an entry. The caller owns tf->phys and closes
 * it separately (entries created by tf_fopen and tf_compact all initialize
 * the mutexes; done_queue is NULL for plain-file / tf_compact structs). */
static void tf_file_free_inner(struct tf_file *tf) {
    g_free(tf->blocks);
    g_free(tf->logical_offs);
    g_free(tf->w_buf);
    g_free(tf->dbg_path);
    g_free(tf->path);
    if (tf->done_queue)
        g_async_queue_unref(tf->done_queue);
    g_mutex_clear(&tf->lock);
    g_cond_clear(&tf->cond);
    g_mutex_clear(&tf->phys_lock);
    g_free(tf);
}

/* Flush, stop the writer thread, write the footer and release the entry.
 * Called when a cached entry is replaced, deleted, compacted, or at exit. */
static int tf_file_finalize(struct tf_file *tf) {
    int rc = 0;
    if (tf->in_cache)
        tf_cache_remove(tf);
    if (tf->writable) {
        if (tf_sync_writes(tf))
            rc = -1;
        if (tf_write_footer(tf))
            rc = -1;
        g_mutex_lock(&tf->lock);
        tf->writer_stop = 1;
        g_cond_broadcast(&tf->cond);
        g_mutex_unlock(&tf->lock);
        g_thread_join(tf->writer_thread);
    }
    if (fclose(tf->phys))
        rc = -1;
    tf_file_free_inner(tf);
    return rc;
}

static int tf_cookie_close(void *cookie) {
    struct tf_handle *h = (struct tf_handle *)cookie;
    struct tf_file *tf = h->tf;
    int rc = 0;
    if (tf->writable && tf_sync_writes(tf))
        rc = -1;
    g_free(h->r_buf);
    g_free(h);
    tf->refcount--;
    if (tf->refcount == 0 && !tf->in_cache)
        tf_file_finalize(tf);
    return rc;
}

static cookie_io_functions_t tf_cookie_io = {
    .read = tf_cookie_read,
    .write = tf_cookie_write,
    .seek = tf_cookie_seek,
    .close = tf_cookie_close,
};

/* Open a temp file through the compression layer. */
FILE *tf_fopen(char *path, char *mode, int compressible) {
    int writable = strchr(mode, '+') || mode[0] == 'w' || mode[0] == 'a';
    FILE *f;

    if (tf_compression_method == TF_CODEC_NONE || !compressible) {
        f = fopen(path, mode);
        if (!f)
            return NULL;
        return f;
    }

    struct tf_file *tf = tf_cache_find(path);
    if (tf && mode[0] == 'w') {
        /* A fresh write replaces whatever is cached for this path. */
        tf_cache_remove(tf);
        if (tf->refcount == 0)
            tf_file_finalize(tf);
        tf = NULL;
    }

    if (!tf) {
        f = fopen(path, mode);
        if (!f)
            return NULL;
        tf = g_new0(struct tf_file, 1);
        tf->phys = f;
        tf->writable = writable;
        tf->path = g_strdup(path);
        if (getenv("TF_DEBUG"))
            tf->dbg_path = g_strdup(path);
        tf->block_size = tf_block_size > 0 ? tf_block_size : (32 * 1024 * 1024);
        g_mutex_init(&tf->lock);
        g_cond_init(&tf->cond);
        g_mutex_init(&tf->phys_lock);

        if (mode[0] == 'w') {
            tf->codec = tf_compression_method;
            tf->level = tf_compression_level;
            tf->pool = tf_pool_get();
            tf->done_queue = g_async_queue_new();
            tf->writer_thread = g_thread_new("tf-writer", tf_writer_thread_fn, tf);
        } else {
            int rc = tf_read_footer(tf, f);
            if (rc < 0) {
                fclose(f);
                tf_file_free_inner(tf);
                return NULL;
            }
            if (rc == 0) {
                /* plain file: hand the caller the raw stream */
                tf_file_free_inner(tf);
                return f;
            }
            if (writable) {
                tf->pool = tf_pool_get();
                tf->done_queue = g_async_queue_new();
                /* appends always go to the physical end */
                fseeko(f, 0, SEEK_END);
                tf->phys_end = ftello(f);
                if (tf->phys_end < 0)
                    tf->phys_end = 0;
                tf->writer_thread = g_thread_new("tf-writer", tf_writer_thread_fn, tf);
            }
        }
        tf_cache_add(tf);
    } else if (writable && !tf->writable) {
        /* Promote a cached read-only entry so it can be written to. */
        tf->writable = 1;
        tf->pool = tf_pool_get();
        tf->done_queue = g_async_queue_new();
        fseeko(tf->phys, 0, SEEK_END);
        tf->phys_end = ftello(tf->phys);
        if (tf->phys_end < 0)
            tf->phys_end = 0;
        tf->writer_thread = g_thread_new("tf-writer", tf_writer_thread_fn, tf);
    }

    struct tf_handle *h = g_new0(struct tf_handle, 1);
    h->tf = tf;
    h->r_block = -1;
    if (mode[0] == 'a')
        h->pos = tf->logical_size;
    tf->refcount++;
    return fopencookie(h, mode, tf_cookie_io);
}

/* Rewrite a compressed file once, moving every block's compressed bytes
 * contiguously to the front and re-emitting the footer, so overwrite garbage
 * is reclaimed. No-op for plain files and when compression is disabled. The
 * cached entry for the path (if any) is finalized first so its state is on
 * disk and the entry is out of the way. */
void tf_compact(char *path) {
    FILE *f, *out;
    char *tmp;
    struct tf_file *tf;
    int i, rc;

    if (tf_compression_method == TF_CODEC_NONE)
        return;
    struct tf_file *cached = tf_cache_find(path);
    if (cached) {
        if (cached->refcount == 0) {
            tf_file_finalize(cached);
        } else {
            dbg(lvl_warning, "tf_compact: %s still open, skipping cache finalize", path);
            return;
        }
    }
    f = fopen(path, "rb");
    if (!f)
        return;
    tf = g_new0(struct tf_file, 1);
    tf->phys = f;
    g_mutex_init(&tf->lock);
    g_cond_init(&tf->cond);
    g_mutex_init(&tf->phys_lock);
    rc = tf_read_footer(tf, f);
    if (rc != 1) {
        fclose(f);
        tf_file_free_inner(tf);
        return;
    }
    tmp = g_strdup_printf("%s.compact", path);
    out = fopen(tmp, "wb");
    if (!out) {
        fclose(f);
        tf_file_free_inner(tf);
        g_free(tmp);
        return;
    }
    for (i = 0; i < tf->block_count; i++) {
        struct tf_block *b = &tf->blocks[i];
        char *buf;
        if (fseeko(f, b->file_off, SEEK_SET) != 0) {
            fclose(out);
            fclose(f);
            unlink(tmp);
            tf_file_free_inner(tf);
            g_free(tmp);
            return;
        }
        buf = g_malloc(b->csize);
        if (fread(buf, 1, b->csize, f) != b->csize) {
            g_free(buf);
            fclose(out);
            fclose(f);
            unlink(tmp);
            tf_file_free_inner(tf);
            g_free(tmp);
            return;
        }
        b->file_off = ftello(out);
        if (b->file_off < 0 || fwrite(buf, 1, b->csize, out) != b->csize) {
            g_free(buf);
            fclose(out);
            fclose(f);
            unlink(tmp);
            tf_file_free_inner(tf);
            g_free(tmp);
            return;
        }
        g_free(buf);
    }
    tf->phys = out;
    tf->phys_end = ftello(out);
    if (tf->phys_end < 0)
        tf->phys_end = 0;
    tf_write_footer(tf);
    fclose(out);
    fclose(f);
    if (rename(tmp, path) != 0)
        fprintf(stderr, "maptool: tf_compact rename %s: %s\n", tmp, strerror(errno));
    g_free(tmp);
    tf_file_free_inner(tf);
}

/* Finalize every cached entry and tear down the compression pool. */
void tf_compress_fini(void) {
    while (tf_cache_head)
        tf_file_finalize(tf_cache_head);
    if (tf_pool) {
        compress_pool_destroy(tf_pool);
        tf_pool = NULL;
    }
}

/* Drop a cached entry for path, e.g. because the file is being deleted. If a
 * handle is still open the entry is kept until its last close, then
 * finalized; otherwise it is finalized (footer written, writer joined) here. */
void tf_cache_drop(char *path) {
    if (tf_compression_method == TF_CODEC_NONE)
        return;
    struct tf_file *tf = tf_cache_find(path);
    if (!tf)
        return;
    tf_cache_remove(tf);
    if (tf->refcount == 0)
        tf_file_finalize(tf);
}

/* Prepare for a rename(path_old -> path_new): finalize cached entries for
 * both names so the renamed file carries a valid footer and stale in-memory
 * state cannot be reused under either name. */
void tf_cache_rename(char *old_path, char *new_path) {
    if (tf_compression_method == TF_CODEC_NONE)
        return;
    struct tf_file *tf;
    tf = tf_cache_find(new_path);
    if (tf) {
        tf_cache_remove(tf);
        if (tf->refcount == 0)
            tf_file_finalize(tf);
    }
    tf = tf_cache_find(old_path);
    if (tf) {
        tf_cache_remove(tf);
        if (tf->refcount == 0)
            tf_file_finalize(tf);
    }
}

#else /* !__GLIBC__ */

FILE *tf_fopen(char *path, char *mode, int compressible) {
    (void)compressible;
    return fopen(path, mode);
}

void tf_compact(char *path) {
    (void)path;
}

void tf_compress_fini(void) {
}

void tf_cache_drop(char *path) {
    (void)path;
}

void tf_cache_rename(char *old_path, char *new_path) {
    (void)old_path;
    (void)new_path;
}

#endif /* __GLIBC__ */
