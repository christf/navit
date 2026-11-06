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
    int paused;
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

static void synthesizer_cmdline_fill_slots(struct synthesizer_priv *this) {
    int running = 0;
    GSequenceIter *iter;

    if (this->paused)
        return;

    for (iter = g_sequence_get_begin_iter(this->queue); iter != g_sequence_get_end_iter(this->queue);
         iter = g_sequence_iter_next(iter)) {
        struct pending_synth *ps = g_sequence_get(iter);
        if (ps->spi)
            running++;
    }

    for (iter = g_sequence_get_begin_iter(this->queue);
         iter != g_sequence_get_end_iter(this->queue) && running < MAX_CONCURRENT; iter = g_sequence_iter_next(iter)) {
        struct pending_synth *ps = g_sequence_get(iter);
        if (!ps->spi) {
            synthesizer_cmdline_spawn(this, ps);
            if (ps->spi)
                running++;
            else
                break;
        }
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
            int st;

            g_mutex_unlock(&this->mutex);
            st = spawn_process_check_status(ps->spi, 1);
            g_mutex_lock(&this->mutex);

            spawn_process_info_free(ps->spi);
            ps->spi = NULL;
            if (st >= 0) {
                synthesizer_cmdline_reap_entry(ps, st);
                pending_synth_free(ps);
                g_sequence_remove(running_iter);
                g_cond_broadcast(&this->cond);
            }
        } else if (g_sequence_get_length(this->queue) > 0) {
            if (this->paused)
                g_cond_wait(&this->cond, &this->mutex);
            else
                g_cond_wait_until(&this->cond, &this->mutex, g_get_monotonic_time() + 50 * 1000);
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

static void pending_synth_destroy_wrapper(gpointer data, gpointer user_data) {
    struct pending_synth *ps = data;
    if (ps->spi) {
        spawn_process_check_status(ps->spi, 1);
        spawn_process_info_free(ps->spi);
    }
    if (ps->tmp_output && g_file_test(ps->tmp_output, G_FILE_TEST_EXISTS))
        unlink(ps->tmp_output);
    pending_synth_free(ps);
}

static void synthesizer_cmdline_destroy(struct synthesizer_priv *this) {
    g_mutex_lock(&this->mutex);
    this->shutdown = 1;
    g_cond_signal(&this->cond);
    g_mutex_unlock(&this->mutex);

    if (this->thread)
        g_thread_join(this->thread);

    g_sequence_foreach(this->queue, pending_synth_destroy_wrapper, NULL);
    g_sequence_free(this->queue);
    g_mutex_clear(&this->mutex);
    g_cond_clear(&this->cond);
    g_free(this->cmdline);
    g_free(this);
}

static void synthesizer_cmdline_pause(struct synthesizer_priv *this) {
    g_mutex_lock(&this->mutex);
    this->paused = 1;
    g_mutex_unlock(&this->mutex);
}

static void synthesizer_cmdline_resume(struct synthesizer_priv *this) {
    g_mutex_lock(&this->mutex);
    this->paused = 0;
    g_cond_signal(&this->cond);
    g_mutex_unlock(&this->mutex);
}

static struct synthesizer_methods synthesizer_cmdline_meth = {
    synthesizer_cmdline_destroy,   synthesizer_cmdline_synthesize,  synthesizer_cmdline_check_status,
    synthesizer_cmdline_wait_done, synthesizer_cmdline_batch_begin, synthesizer_cmdline_pause,
    synthesizer_cmdline_resume,
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
