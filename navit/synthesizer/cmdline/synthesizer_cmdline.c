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
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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

    for (iter = g_sequence_get_begin_iter(this->queue);
         iter != g_sequence_get_end_iter(this->queue);
         iter = g_sequence_iter_next(iter)) {
        struct pending_synth *ps = g_sequence_get(iter);
        if (ps->spi)
            running++;
    }

    for (iter = g_sequence_get_begin_iter(this->queue);
         iter != g_sequence_get_end_iter(this->queue) && running < MAX_CONCURRENT;
         iter = g_sequence_iter_next(iter)) {
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

static void synthesizer_cmdline_reap_done(struct synthesizer_priv *this) {
    GSequenceIter *iter = g_sequence_get_begin_iter(this->queue);
    while (iter != g_sequence_get_end_iter(this->queue)) {
        struct pending_synth *ps = g_sequence_get(iter);
        GSequenceIter *next = g_sequence_iter_next(iter);
        if (ps->spi) {
            int st = spawn_process_check_status(ps->spi, 0);
            if (st >= 0) {
                spawn_process_info_free(ps->spi);
                if (ps->tmp_output && g_file_test(ps->tmp_output, G_FILE_TEST_EXISTS))
                    g_rename(ps->tmp_output, ps->output);
                pending_synth_free(ps);
                g_sequence_remove(iter);
            }
        }
        iter = next;
    }
    synthesizer_cmdline_fill_slots(this);
}

static int synthesizer_cmdline_synthesize(struct synthesizer_priv *this,
                                          const char *text,
                                          const char *output_path,
                                          synthesizer_batch_id batch) {
    if (!text || !*text || !output_path)
        return -1;

    dbg(lvl_debug, "synthesize text='%s' output='%s' batch=%llu", text, output_path, batch);

    if (g_file_test(output_path, G_FILE_TEST_EXISTS)) {
        dbg(lvl_debug, "output '%s' already exists, skipping", output_path);
        return 0;
    }

    {
        GSequenceIter *iter;
        for (iter = g_sequence_get_begin_iter(this->queue);
             iter != g_sequence_get_end_iter(this->queue);
             iter = g_sequence_iter_next(iter)) {
            struct pending_synth *ps = g_sequence_get(iter);
            if (!g_strcmp0(ps->output, output_path)) {
                dbg(lvl_debug, "duplicate output_path '%s', skipping", output_path);
                return 0;
            }
        }
    }

    {
        int fd = open(output_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd != -1) {
            close(fd);
        } else if (errno == EEXIST) {
            dbg(lvl_debug, "lock file '%s' already exists", output_path);
        } else {
            dbg(lvl_error, "cannot create lock file '%s'", output_path);
        }
    }

    struct pending_synth *ps = g_new0(struct pending_synth, 1);
    ps->text = g_strdup(text);
    ps->output = g_strdup(output_path);
    ps->priority = batch;
    g_sequence_insert_sorted(this->queue, ps, pending_synth_compare, NULL);

    synthesizer_cmdline_reap_done(this);

    return 0;
}

static unsigned long long synthesizer_cmdline_batch_begin(struct synthesizer_priv *this) {
    GSequenceIter *iter = g_sequence_get_begin_iter(this->queue);
    while (iter != g_sequence_get_end_iter(this->queue)) {
        struct pending_synth *ps = g_sequence_get(iter);
        GSequenceIter *next = g_sequence_iter_next(iter);
        if (!ps->spi) {
            dbg(lvl_debug, "discard pending batch=%llu '%s'", ps->priority, ps->output);
            pending_synth_free(ps);
            g_sequence_remove(iter);
        }
        iter = next;
    }
    this->next_batch_id++;
    dbg(lvl_debug, "new batch id=%llu", this->next_batch_id);
    return this->next_batch_id;
}

static int synthesizer_cmdline_check_status(struct synthesizer_priv *this) {
    synthesizer_cmdline_reap_done(this);
    if (g_sequence_get_length(this->queue) == 0)
        return 255;
    return -1;
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
    g_sequence_foreach(this->queue, pending_synth_destroy_wrapper, NULL);
    g_sequence_free(this->queue);
    g_free(this->cmdline);
    g_free(this);
}

static struct synthesizer_methods synthesizer_cmdline_meth = {
    synthesizer_cmdline_destroy,
    synthesizer_cmdline_synthesize,
    synthesizer_cmdline_check_status,
    synthesizer_cmdline_batch_begin,
};

static struct synthesizer_priv *synthesizer_cmdline_new(struct synthesizer_methods *meth,
                                                        struct attr **attrs,
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

    *meth = synthesizer_cmdline_meth;
    return this;
}

void plugin_init(void) {
    plugin_register_category_synthesizer("cmdline", synthesizer_cmdline_new);
}
