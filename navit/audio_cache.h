/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2024 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#ifndef NAVIT_AUDIO_CACHE_H
#define NAVIT_AUDIO_CACHE_H

#include <glib.h>
#include <stddef.h>

struct audio_cache;

enum cache_segment_type {
    CACHE_SEGMENT_MATCHED,
    CACHE_SEGMENT_BLANK,
};

struct cache_segment {
    enum cache_segment_type type;
    char *text;
    char *path;
};

struct audio_cache *audio_cache_new(const char *cache_dir);
void audio_cache_destroy(struct audio_cache *ac);

GList *audio_cache_lookup(struct audio_cache *ac, const char *text, const char *suffix);

GList *audio_cache_decompose(struct audio_cache *ac, const char *text, const char *suffix);

void audio_cache_segment_free(struct cache_segment *seg);

int audio_cache_put(struct audio_cache *ac, const char *text, const void *data, size_t len, const char *suffix,
                    int synthetic);

void audio_cache_touch(const char *path);

void audio_cache_cleanup(struct audio_cache *ac, size_t max_bytes, const char *suffix);

char *audio_cache_synthetic_dir(struct audio_cache *ac);
char *audio_cache_manual_dir(struct audio_cache *ac);

char *audio_cache_name_encode(const char *text);
char *audio_cache_name_decode(const char *name);

#endif
