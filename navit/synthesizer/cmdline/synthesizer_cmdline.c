/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2024 Navit Team
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

#include "config.h"
#include "debug.h"
#include "plugin.h"
#include "synthesizer.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CONCURRENT 1
#define MAX_BATCH 16

struct pending_synth {
    struct spawn_process_info *spi;
    char *text;
    char *output;
    char *tmp_output;
    unsigned long long priority;
};

struct synthesizer_priv {
    char *cmdline;
    GSequence *queue;
    unsigned long long next_batch_id;
    GMutex mutex;
    GCond cond;
    GThread *thread;
    int shutdown;
};

static void pending_synth_free(struct pending_synth *ps) {
    if (!ps)
        return;
    g_free(ps->text);
    g_free(ps->output);
    g_free(ps->tmp_output);
    g_free(ps);
}

static gint pending_synth_compare(gconstpointer a, gconstpointer b, gpointer user_data) {
    const struct pending_synth *pa = a;
    const struct pending_synth *pb = b;
    if (pa->priority > pb->priority)
        return -1;
    if (pa->priority < pb->priority)
        return 1;
    return 0;
}

static void synthesizer_cmdline_spawn(struct synthesizer_priv *this, struct pending_synth *ps) {
    char **cmdv;
    int cmdv_len;
    char **argv;
    int i;

    ps->tmp_output = g_strdup_printf("%s.tmp.%d", ps->output, getpid());

    cmdv = g_strsplit(this->cmdline, " ", -1);
    cmdv_len = g_strv_length(cmdv);
    argv = g_new(char *, cmdv_len + 3);
    for (i = 0; cmdv[i]; i++)
        argv[i] = g_strdup(cmdv[i]);
    argv[i++] = g_strdup(ps->text);
    argv[i++] = g_strdup(ps->tmp_output);
    argv[i] = NULL;

    ps->spi = spawn_process(argv);
    g_strfreev(argv);
    g_strfreev(cmdv);
}

static void synthesizer_cmdline_spawn_batch(struct synthesizer_priv *this, GList *batch) {
    char *batch_file;
    char **cmdv;
    int cmdv_len;
    char **argv;
    int i;
    GList *l;
    FILE *f;
    struct spawn_process_info *spi;

    batch_file = g_strdup_printf("/tmp/navit-synth-batch.%d.txt", getpid());
    f = fopen(batch_file, "w");
    if (!f) {
        dbg(lvl_error, "failed to create batch file '%s': %s", batch_file, g_strerror(errno));
        g_free(batch_file);
        return;
    }

    for (l = batch; l; l = l->next) {
        struct pending_synth *ps = l->data;
        ps->tmp_output = g_strdup_printf("%s.tmp.%d", ps->output, getpid());
        fprintf(f, "\"%s\" \"%s\"\n", ps->text, ps->tmp_output);
    }
    fclose(f);

    cmdv = g_strsplit(this->cmdline, " ", -1);
    cmdv_len = g_strv_length(cmdv);
    argv = g_new(char *, cmdv_len + 3);
    for (i = 0; cmdv[i]; i++)
        argv[i] = g_strdup(cmdv[i]);
    argv[i++] = g_strdup("--batch");
    argv[i++] = g_strdup(batch_file);
    argv[i] = NULL;

    spi = spawn_process(argv);

    for (l = batch; l; l = l->next) {
        struct pending_synth *ps = l->data;
        ps->spi = spi;
    }

    g_strfreev(argv);
    g_strfreev(cmdv);
    g_free(batch_file);
}

static void synthesizer_cmdline_fill_slots(struct synthesizer_priv *this) {
    int running = 0;
    int pending = 0;
    GSequenceIter *iter;
    GList *batch = NULL;

    for (iter = g_sequence_get_begin_iter(this->queue); iter != g_sequence_get_end_iter(this->queue);
         iter = g_sequence_iter_next(iter)) {
        struct pending_synth *ps = g_sequence_get(iter);
        if (ps->spi)
            running++;
        else
            pending++;
    }

    if (running >= MAX_CONCURRENT || pending == 0)
        return;

    if (pending == 1) {
        for (iter = g_sequence_get_begin_iter(this->queue); iter != g_sequence_get_end_iter(this->queue);
             iter = g_sequence_iter_next(iter)) {
            struct pending_synth *ps = g_sequence_get(iter);
            if (!ps->spi) {
                synthesizer_cmdline_spawn(this, ps);
                break;
            }
        }
    } else {
        int count = 0;
        for (iter = g_sequence_get_begin_iter(this->queue); iter != g_sequence_get_end_iter(this->queue);
             iter = g_sequence_iter_next(iter)) {
            struct pending_synth *ps = g_sequence_get(iter);
            if (!ps->spi) {
                batch = g_list_prepend(batch, ps);
                count++;
                if (count >= MAX_BATCH)
                    break;
            }
        }
        batch = g_list_reverse(batch);
        synthesizer_cmdline_spawn_batch(this, batch);
        g_list_free(batch);
    }
}

static void synthesizer_cmdline_reap_entry(struct pending_synth *ps, int st) {
    if (st >= 0 && ps->tmp_output && g_file_test(ps->tmp_output, G_FILE_TEST_EXISTS)) {
        struct stat fst;
        if (stat(ps->tmp_output, &fst) == 0 && fst.st_size > 0) {
            g_rename(ps->tmp_output, ps->output);
        } else {
            dbg(lvl_warning, "synthesis failed for '%s', removing empty output", ps->text);
            g_unlink(ps->tmp_output);
        }
    } else if (ps->tmp_output) {
        g_unlink(ps->tmp_output);
    }
}

static gpointer synthesizer_cmdline_worker(gpointer data) {
    struct synthesizer_priv *this = data;

    g_mutex_lock(&this->mutex);
    while (!this->shutdown) {
        GSequenceIter *running_iter = NULL;
        GSequenceIter *iter;

        synthesizer_cmdline_fill_slots(this);

        for (iter = g_sequence_get_begin_iter(this->queue); iter != g_sequence_get_end_iter(this->queue);
             iter = g_sequence_iter_next(iter)) {
            struct pending_synth *ps = g_sequence_get(iter);
            if (ps->spi) {
                running_iter = iter;
                break;
            }
        }

        if (running_iter) {
            struct pending_synth *ps = g_sequence_get(running_iter);
            struct spawn_process_info *spi = ps->spi;
            int st;
            GSequenceIter *inner;

            g_mutex_unlock(&this->mutex);
            st = spawn_process_check_status(spi, 1);
            g_mutex_lock(&this->mutex);

            /* Remove all entries sharing this spi (batch or single) */
            inner = g_sequence_get_begin_iter(this->queue);
            while (inner != g_sequence_get_end_iter(this->queue)) {
                struct pending_synth *entry = g_sequence_get(inner);
                if (entry->spi == spi) {
                    synthesizer_cmdline_reap_entry(entry, st);
                    pending_synth_free(entry);
                    g_sequence_remove(inner);
                    inner = g_sequence_get_begin_iter(this->queue);
                } else {
                    inner = g_sequence_iter_next(inner);
                }
            }
            spawn_process_info_free(spi);
        } else if (g_sequence_get_length(this->queue) > 0) {
            g_usleep(50000);
        } else {
            g_cond_wait(&this->cond, &this->mutex);
        }
    }
    g_mutex_unlock(&this->mutex);
    return NULL;
}

static int synthesizer_cmdline_synthesize(struct synthesizer_priv *this, const char *text, const char *output_path,
                                          synthesizer_batch_id batch) {
    if (!text || !*text || !output_path)
        return -1;

    dbg(lvl_debug, "synthesize text='%s' output='%s' batch=%llu", text, output_path, batch);

    if (g_file_test(output_path, G_FILE_TEST_EXISTS)) {
        struct stat st;
        if (stat(output_path, &st) == 0 && st.st_size > 0) {
            dbg(lvl_debug, "output '%s' already exists (%ld bytes), skipping", output_path, (long)st.st_size);
            return 0;
        }
        dbg(lvl_debug, "output '%s' exists but is empty/stale, removing and re-synthesizing", output_path);
        g_unlink(output_path);
    }

    {
        GSequenceIter *iter;
        for (iter = g_sequence_get_begin_iter(this->queue); iter != g_sequence_get_end_iter(this->queue);
             iter = g_sequence_iter_next(iter)) {
            struct pending_synth *ps = g_sequence_get(iter);
            if (!g_strcmp0(ps->output, output_path)) {
                dbg(lvl_debug, "duplicate output_path '%s', skipping", output_path);
                return 0;
            }
        }
    }

    struct pending_synth *ps = g_new0(struct pending_synth, 1);
    ps->text = g_strdup(text);
    ps->output = g_strdup(output_path);
    ps->priority = batch;

    g_mutex_lock(&this->mutex);
    g_sequence_insert_sorted(this->queue, ps, pending_synth_compare, NULL);
    g_cond_signal(&this->cond);
    g_mutex_unlock(&this->mutex);

    return 0;
}

static unsigned long long synthesizer_cmdline_batch_begin(struct synthesizer_priv *this) {
    g_mutex_lock(&this->mutex);
    this->next_batch_id++;
    dbg(lvl_debug, "new batch id=%llu", this->next_batch_id);
    g_mutex_unlock(&this->mutex);
    return this->next_batch_id;
}

static int synthesizer_cmdline_check_status(struct synthesizer_priv *this) {
    int len;

    g_mutex_lock(&this->mutex);
    len = g_sequence_get_length(this->queue);
    g_mutex_unlock(&this->mutex);

    if (len == 0)
        return 255;
    return -1;
}

static int synthesizer_cmdline_wait_done(struct synthesizer_priv *this) {
    g_mutex_lock(&this->mutex);
    while (g_sequence_get_length(this->queue) > 0 && !this->shutdown)
        g_cond_wait(&this->cond, &this->mutex);
    g_mutex_unlock(&this->mutex);

    if (this->shutdown)
        return -1;
    return 255;
}

static void synthesizer_cmdline_destroy(struct synthesizer_priv *this) {
    GSequenceIter *iter;
    GList *freed_spis = NULL;

    g_mutex_lock(&this->mutex);
    this->shutdown = 1;
    g_cond_signal(&this->cond);
    g_mutex_unlock(&this->mutex);

    if (this->thread)
        g_thread_join(this->thread);

    iter = g_sequence_get_begin_iter(this->queue);
    while (iter != g_sequence_get_end_iter(this->queue)) {
        struct pending_synth *ps = g_sequence_get(iter);
        if (ps->spi && !g_list_find(freed_spis, ps->spi)) {
            spawn_process_check_status(ps->spi, 1);
            spawn_process_info_free(ps->spi);
            freed_spis = g_list_prepend(freed_spis, ps->spi);
        }
        if (ps->tmp_output && g_file_test(ps->tmp_output, G_FILE_TEST_EXISTS))
            unlink(ps->tmp_output);
        pending_synth_free(ps);
        g_sequence_remove(iter);
        iter = g_sequence_get_begin_iter(this->queue);
    }
    g_list_free(freed_spis);
    g_sequence_free(this->queue);
    g_mutex_clear(&this->mutex);
    g_cond_clear(&this->cond);
    g_free(this->cmdline);
    g_free(this);
}

static struct synthesizer_methods synthesizer_cmdline_meth = {
    synthesizer_cmdline_destroy,   synthesizer_cmdline_synthesize,  synthesizer_cmdline_check_status,
    synthesizer_cmdline_wait_done, synthesizer_cmdline_batch_begin,
};

static struct synthesizer_priv *synthesizer_cmdline_new(struct synthesizer_methods *meth, struct attr **attrs,
                                                        struct attr *parent) {
    struct synthesizer_priv *this;
    struct attr *attr;

    this = g_new0(struct synthesizer_priv, 1);

    attr = attr_search(attrs, attr_data);
    if (attr)
        this->cmdline = g_strdup(attr->u.str);
    else
        this->cmdline = g_strdup("navit-speech-cache-synthesize.sh");

    this->queue = g_sequence_new(NULL);
    g_mutex_init(&this->mutex);
    g_cond_init(&this->cond);
    this->thread = g_thread_new("navit-synth", synthesizer_cmdline_worker, this);

    *meth = synthesizer_cmdline_meth;
    return this;
}

void plugin_init(void) {
    plugin_register_category_synthesizer("cmdline", synthesizer_cmdline_new);
}
