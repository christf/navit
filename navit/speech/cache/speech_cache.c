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

#include "audio_cache.h"
#include "config.h"
#include "debug.h"
#include "file.h"
#include "navit.h"
#include "plugin.h"
#include "speech.h"
#include "synthesizer.h"
#include "util.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum cache_mode {
    MODE_FULL_PHRASE,
    MODE_FILL_IN_THE_BLANK,
};

struct speech_priv {
    char *play_cmdline;
    char *cache_dir;
    char *sample_suffix;
    size_t cache_size;
    enum cache_mode mode;
    struct audio_cache *ac;
    struct synthesizer *synth;
    struct spawn_process_info *spi;
};

static void playback_files(struct speech_priv *this, GList *files) {
    char **cmdv;
    int var_idx;
    int n;
    int cmdv_len;
    int argc;
    char **argv;
    int i, j;

    if (!files)
        return;

    if (this->spi) {
        spawn_process_check_status(this->spi, 1);
        spawn_process_info_free(this->spi);
    }

    cmdv = g_strsplit(this->play_cmdline, " ", -1);
    var_idx = -1;
    for (i = 0; cmdv[i]; i++) {
        if (strstr(cmdv[i], "%s")) {
            var_idx = i;
            break;
        }
    }

    n = g_list_length(files);
    cmdv_len = g_strv_length(cmdv);
    argc = cmdv_len + n - (var_idx >= 0 ? 1 : 0);
    argv = g_new(char *, argc + 1);

    for (i = 0, j = 0; j < argc;) {
        if (i == var_idx) {
            GList *l = files;
            while (l) {
                argv[j++] = g_strdup_printf(cmdv[i], (char *)l->data);
                audio_cache_touch((char *)l->data);
                l = g_list_next(l);
            }
            i++;
        } else {
            argv[j++] = g_strdup(cmdv[i++]);
        }
    }
    argv[j] = NULL;

    this->spi = spawn_process(argv);
    g_strfreev(argv);
    g_strfreev(cmdv);
}

static int synthesis_sync(struct speech_priv *this, const char *out_path, const char *label) {
    struct stat st;
    int waited = 0;
    while (stat(out_path, &st) == 0 && st.st_size == 0 && waited < 600) {
        synthesizer_check_status(this->synth);
        g_usleep(100000);
        waited++;
    }
    if (waited)
        dbg(lvl_debug, "waited %dms for '%s'", waited * 100, label);
    return (stat(out_path, &st) == 0 && st.st_size > 0) ? 0 : -1;
}

static int synthesize_text(struct speech_priv *this, const char *text, int wait, unsigned long long batch_id) {
    if (!this->synth)
        return -1;

    if (this->mode == MODE_FILL_IN_THE_BLANK) {
        GList *segs = audio_cache_decompose(this->ac, text, this->sample_suffix);
        GList *l;
        int ok = 0;
        for (l = segs; l; l = l->next) {
            struct cache_segment *seg = l->data;
            if (seg->type != CACHE_SEGMENT_BLANK)
                continue;
            dbg(lvl_debug, "synthesize blank: '%s'", seg->text);
            char *encoded = audio_cache_name_encode(seg->text);
            char *filename = g_strdup_printf("%s%s", encoded, this->sample_suffix ? this->sample_suffix : "");
            char *out_path = g_build_filename(this->cache_dir, "synthetic", filename, NULL);
            g_free(filename);
            g_free(encoded);
            synthesizer_synthesize(this->synth, seg->text, out_path, batch_id);
            if (wait)
                synthesis_sync(this, out_path, seg->text);
            g_free(out_path);
            ok = 1;
        }
        g_list_free_full(segs, (GDestroyNotify)audio_cache_segment_free);
        return ok ? 0 : -1;
    }

    char *encoded = audio_cache_name_encode(text);
    char *filename = g_strdup_printf("%s%s", encoded, this->sample_suffix ? this->sample_suffix : "");
    char *out_path = g_build_filename(this->cache_dir, "synthetic", filename, NULL);
    g_free(filename);
    g_free(encoded);
    synthesizer_synthesize(this->synth, text, out_path, batch_id);
    if (wait)
        synthesis_sync(this, out_path, text);
    g_free(out_path);
    return 0;
}

static int speech_cache_say(struct speech_priv *this, const char *text) {
    if (!text || !*text)
        return 0;

    dbg(lvl_debug, "say: '%s'", text);

    GList *matches = audio_cache_lookup(this->ac, text, this->sample_suffix);
    if (matches) {
        dbg(lvl_debug, "cache HIT: %d file(s)", g_list_length(matches));
        playback_files(this, matches);
        g_list_free_full(matches, g_free);
        return 0;
    }

    unsigned long long batch = synthesizer_batch_begin(this->synth);
    dbg(lvl_debug, "cache MISS, synthesizing '%s' batch=%llu", text, batch);
    synthesize_text(this, text, 1, batch);

    matches = audio_cache_lookup(this->ac, text, this->sample_suffix);
    if (matches) {
        dbg(lvl_debug, "after synthesis cache HIT: %d file(s)", g_list_length(matches));
        playback_files(this, matches);
        g_list_free_full(matches, g_free);
    } else {
        dbg(lvl_error, "synthesis produced no file for '%s'", text);
    }

    return 0;
}

static int speech_cache_prepare(struct speech_priv *this, const char *text, unsigned long long batch_id) {
    if (!text || !*text)
        return 0;

    GList *matches = audio_cache_lookup(this->ac, text, this->sample_suffix);
    if (matches) {
        g_list_free_full(matches, g_free);
        return 0;
    }

    dbg(lvl_debug, "prepare: synthesizing '%s' batch=%llu", text, batch_id);
    synthesize_text(this, text, 0, batch_id);
    return 0;
}

static void speech_cache_destroy(struct speech_priv *this) {
    if (!this)
        return;
    if (this->spi) {
        spawn_process_check_status(this->spi, 0);
        spawn_process_info_free(this->spi);
    }
    g_free(this->play_cmdline);
    g_free(this->cache_dir);
    g_free(this->sample_suffix);
    if (this->synth)
        synthesizer_destroy(this->synth);
    audio_cache_destroy(this->ac);
    g_free(this);
}

static unsigned long long speech_cache_batch_begin(struct speech_priv *this) {
    if (this->synth)
        return synthesizer_batch_begin(this->synth);
    return 0;
}

static struct speech_methods speech_cache_meth = {
    speech_cache_destroy,
    speech_cache_say,
    speech_cache_prepare,
    speech_cache_batch_begin,
};

static struct speech_priv *speech_cache_new(struct speech_methods *meth, struct attr **attrs, struct attr *parent) {
    struct speech_priv *this;
    struct attr *attr;

    this = g_new0(struct speech_priv, 1);

    attr = attr_search(attrs, attr_data);
    if (attr)
        this->play_cmdline = g_strdup(attr->u.str);
    else
        this->play_cmdline = g_strdup("navit-speech-cache-playback.sh %s");

    attr = attr_search(attrs, attr_sample_dir);
    if (attr)
        this->cache_dir = g_strdup(attr->u.str);
    else {
        char *user_dir = navit_get_user_data_directory(TRUE);
        if (user_dir)
            this->cache_dir = g_build_filename(user_dir, "audio_cache", NULL);
        else
            this->cache_dir = g_build_filename(g_get_home_dir(), ".navit", "audio_cache", NULL);
    }

    attr = attr_search(attrs, attr_sample_suffix);
    if (attr)
        this->sample_suffix = g_strdup(attr->u.str);
    else
        this->sample_suffix = g_strdup(".opus");

    attr = attr_search(attrs, attr_cache_size);
    if (attr)
        this->cache_size = attr->u.num;
    else
        this->cache_size = 50 * 1024 * 1024;

    attr = attr_search(attrs, attr_subtype);
    if (attr) {
        if (!strcmp("fill-in-the-blank", attr->u.str))
            this->mode = MODE_FILL_IN_THE_BLANK;
        else if (!strcmp("full-phrase", attr->u.str))
            this->mode = MODE_FULL_PHRASE;
        else
            dbg(lvl_warning, "unknown mode '%s', using full-phrase", attr->u.str);
    }

    this->ac = audio_cache_new(this->cache_dir);
    if (!this->ac) {
        speech_cache_destroy(this);
        return NULL;
    }

    struct attr *synth_attrs[3];
    struct attr synth_type;
    struct attr synth_data;
    synth_type.type = attr_type;
    synth_type.u.str = "cmdline";
    synth_attrs[0] = &synth_type;

    attr = attr_search(attrs, attr_data_synth);
    if (attr) {
        synth_data.type = attr_data;
        synth_data.u.str = attr->u.str;
        synth_attrs[1] = &synth_data;
        synth_attrs[2] = NULL;
    } else {
        synth_attrs[1] = NULL;
    }
    this->synth = synthesizer_new(NULL, synth_attrs);
    if (!this->synth) {
        dbg(lvl_warning, "no synthesizer available, cache misses will fail");
    }

    *meth = speech_cache_meth;
    return this;
}

void plugin_init(void) {
    plugin_register_category_speech("cache", speech_cache_new);
}
