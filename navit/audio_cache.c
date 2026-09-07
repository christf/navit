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
#include "debug.h"
#include "file.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <sys/stat.h>
#    include <utime.h>
#    include <unistd.h>
#endif

struct audio_cache {
    char *synthetic_dir;
    char *manual_dir;
};

static int is_safe_filename_char(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
        || c == '~') {
        return 1;
    }
    return 0;
}

char *audio_cache_name_encode(const char *text) {
    GString *s = g_string_sized_new(strlen(text) * 3);
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (is_safe_filename_char(*p)) {
            g_string_append_c(s, *p);
        } else if (*p == ' ') {
            g_string_append_c(s, '_');
        } else {
            g_string_append_printf(s, "%%%02X", *p);
        }
        p++;
    }
    return g_string_free(s, FALSE);
}

char *audio_cache_name_decode(const char *name) {
    GString *s = g_string_sized_new(strlen(name));
    const char *p = name;
    while (*p) {
        if (*p == '%' && p[1] && p[2]) {
            int val;
            char c1 = p[1], c2 = p[2];
            if (c1 >= '0' && c1 <= '9')
                val = (c1 - '0') << 4;
            else if (c1 >= 'a' && c1 <= 'f')
                val = (c1 - 'a' + 10) << 4;
            else if (c1 >= 'A' && c1 <= 'F')
                val = (c1 - 'A' + 10) << 4;
            else
                goto literal;
            if (c2 >= '0' && c2 <= '9')
                val |= (c2 - '0');
            else if (c2 >= 'a' && c2 <= 'f')
                val |= (c2 - 'a' + 10);
            else if (c2 >= 'A' && c2 <= 'F')
                val |= (c2 - 'A' + 10);
            else
                goto literal;
            g_string_append_c(s, (char)val);
            p += 3;
            continue;
        literal:
            ;
        } else if (*p == '_') {
            g_string_append_c(s, ' ');
            p++;
            continue;
        }
        g_string_append_c(s, *p);
        p++;
    }
    return g_string_free(s, FALSE);
}

struct audio_cache *audio_cache_new(const char *cache_dir) {
    struct audio_cache *ac;
    ac = g_new0(struct audio_cache, 1);
    ac->synthetic_dir = g_build_filename(cache_dir, "synthetic", NULL);
    ac->manual_dir = g_build_filename(cache_dir, "manual", NULL);
    file_mkdir(ac->synthetic_dir, 1);
    file_mkdir(ac->manual_dir, 1);
    dbg(lvl_debug, "audio cache dirs: %s, %s", ac->synthetic_dir, ac->manual_dir);
    return ac;
}

void audio_cache_destroy(struct audio_cache *ac) {
    if (!ac)
        return;
    g_free(ac->synthetic_dir);
    g_free(ac->manual_dir);
    g_free(ac);
}

char *audio_cache_synthetic_dir(struct audio_cache *ac) {
    return ac->synthetic_dir;
}

char *audio_cache_manual_dir(struct audio_cache *ac) {
    return ac->manual_dir;
}

static GList *scan_directory(const char *dir, const char *suffix) {
    GList *files = NULL;
    void *handle;
    int suffix_len = suffix ? strlen(suffix) : 0;

    handle = file_opendir((char *)dir);
    if (!handle) {
        dbg(lvl_debug, "cannot open directory: %s", dir);
        return NULL;
    }
    char *name;
    while ((name = file_readdir(handle))) {
        int len = strlen(name);
        if (len > suffix_len) {
            if (!suffix || !strcmp(name + len - suffix_len, suffix)) {
                char *full = g_build_filename(dir, name, NULL);
                struct stat st;
                if (g_stat(full, &st) == 0 && st.st_size == 0) {
                    g_free(full);
                    continue;
                }
                files = g_list_prepend(files, full);
            }
        }
    }
    file_closedir(handle);
    return files;
}

static int longest_prefix_match(const char *text, const char *sample_text) {
    int len = 0;
    while (*text && *sample_text && g_ascii_strncasecmp(text, sample_text, 1) == 0) {
        text++;
        sample_text++;
        len++;
    }
    return len;
}

static GList *find_longest_match(GList *files, const char *text, const char *suffix) {
    GList *f;
    const char *best_file = NULL;
    char *best_decoded = NULL;
    int best_len = 0;
    int suffix_len = suffix ? strlen(suffix) : 0;

    for (f = files; f; f = g_list_next(f)) {
        char *path = (char *)f->data;
        char *basename = g_path_get_basename(path);
        char *decoded = NULL;

        if (suffix && suffix_len > 0) {
            int blen = strlen(basename);
            if (blen > suffix_len) {
                basename[blen - suffix_len] = '\0';
            }
        }
        decoded = audio_cache_name_decode(basename);
        int match_len = longest_prefix_match(text, decoded);
        if (match_len > best_len) {
            best_len = match_len;
            best_file = path;
            if (best_decoded)
                g_free(best_decoded);
            best_decoded = g_strdup(decoded);
        }
        g_free(decoded);
        g_free(basename);
    }
    if (best_file && best_len > 0) {
        char *result = g_strdup(best_file);
        g_free(best_decoded);
        return g_list_prepend(NULL, result);
    }
    g_free(best_decoded);
    return NULL;
}

GList *audio_cache_lookup(struct audio_cache *ac, const char *text,
                          const char *suffix) {
    GList *all_files = NULL;
    GList *result = NULL;
    GList *synthetic_files = NULL;
    GList *manual_files = NULL;
    const char *remaining = text;

    synthetic_files = scan_directory(ac->synthetic_dir, suffix);
    manual_files = scan_directory(ac->manual_dir, suffix);
    all_files = g_list_concat(synthetic_files, manual_files);
    if (!all_files) {
        dbg(lvl_debug, "no cached files found for '%s'", text);
        return NULL;
    }

    while (*remaining) {
        while (*remaining == ' ' || *remaining == ',')
            remaining++;
        if (!*remaining)
            break;

        GList *match = find_longest_match(all_files, remaining, suffix);
        if (!match) {
            dbg(lvl_debug, "no match for remaining '%s'", remaining);
            g_list_free_full(all_files, g_free);
            g_list_free_full(result, g_free);
            return NULL;
        }
        char *matched_path = (char *)match->data;
        char *basename = g_path_get_basename(matched_path);
        char *decoded = NULL;

        if (suffix) {
            int blen = strlen(basename);
            int suffix_len = strlen(suffix);
            if (blen > suffix_len)
                basename[blen - suffix_len] = '\0';
        }
        decoded = audio_cache_name_decode(basename);
        int match_len = longest_prefix_match(remaining, decoded);
        remaining += match_len;
        result = g_list_append(result, g_strdup(matched_path));
        g_list_free_full(match, g_free);
        g_free(decoded);
        g_free(basename);
    }

    g_list_free_full(all_files, g_free);
    return result;
}

int audio_cache_put(struct audio_cache *ac, const char *text,
                    const void *data, size_t len,
                    const char *suffix, int synthetic) {
    char *dir = synthetic ? ac->synthetic_dir : ac->manual_dir;
    char *encoded = audio_cache_name_encode(text);
    char *filename = g_strdup_printf("%s%s", encoded, suffix ? suffix : "");
    char *path = g_build_filename(dir, filename, NULL);
    char *tmp_path = g_strdup_printf("%s.tmp", path);
    int ret = -1;

    FILE *f = g_fopen(tmp_path, "wb");
    if (!f) {
        dbg(lvl_error, "cannot write to %s", tmp_path);
        goto cleanup;
    }
    if (fwrite(data, 1, len, f) != len) {
        dbg(lvl_error, "short write to %s", tmp_path);
        fclose(f);
        g_unlink(tmp_path);
        goto cleanup;
    }
    fclose(f);

    if (g_rename(tmp_path, path) != 0) {
        dbg(lvl_error, "rename %s -> %s failed", tmp_path, path);
        g_unlink(tmp_path);
        goto cleanup;
    }
    ret = 0;

cleanup:
    g_free(encoded);
    g_free(filename);
    g_free(path);
    g_free(tmp_path);
    return ret;
}

void audio_cache_segment_free(struct cache_segment *seg) {
    if (!seg)
        return;
    g_free(seg->text);
    g_free(seg->path);
    g_free(seg);
}

GList *audio_cache_decompose(struct audio_cache *ac, const char *text,
                             const char *suffix) {
    GList *all_files = NULL;
    GList *synthetic_files = NULL;
    GList *manual_files = NULL;
    GList *result = NULL;
    const char *remaining = text;
    const char *blank_start = text;

    synthetic_files = scan_directory(ac->synthetic_dir, suffix);
    manual_files = scan_directory(ac->manual_dir, suffix);
    all_files = g_list_concat(synthetic_files, manual_files);

    while (*remaining) {
        while (*remaining == ' ' || *remaining == ',')
            remaining++;
        if (!*remaining)
            break;
        blank_start = remaining;

        GList *match = find_longest_match(all_files, remaining, suffix);
        if (!match)
            break;

        char *matched_path = (char *)match->data;
        char *basename = g_path_get_basename(matched_path);
        if (suffix) {
            int blen = strlen(basename);
            int suffix_len = strlen(suffix);
            if (blen > suffix_len)
                basename[blen - suffix_len] = '\0';
        }
        char *decoded = audio_cache_name_decode(basename);
        int match_len = longest_prefix_match(remaining, decoded);

        {
            struct cache_segment *seg = g_new0(struct cache_segment, 1);
            seg->type = CACHE_SEGMENT_MATCHED;
            seg->text = g_strndup(remaining, match_len);
            seg->path = g_strdup(matched_path);
            result = g_list_append(result, seg);
        }

        remaining += match_len;
        g_free(decoded);
        g_free(basename);
        g_list_free_full(match, g_free);
    }

    if (blank_start < remaining) {
        struct cache_segment *seg = g_new0(struct cache_segment, 1);
        seg->type = CACHE_SEGMENT_BLANK;
        seg->text = g_strndup(blank_start, remaining - blank_start);
        result = g_list_append(result, seg);
    }
    if (*remaining) {
        struct cache_segment *seg = g_new0(struct cache_segment, 1);
        seg->type = CACHE_SEGMENT_BLANK;
        seg->text = g_strdup(remaining);
        result = g_list_append(result, seg);
    }

    g_list_free_full(all_files, g_free);
    return result;
}

void audio_cache_touch(const char *path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        SetFileTime(h, NULL, NULL, &ft);
        CloseHandle(h);
    }
#else
    struct stat st;
    if (g_stat(path, &st) == 0) {
        struct utimbuf ut;
        ut.actime = st.st_atime;
        ut.modtime = time(NULL);
        utime(path, &ut);
    }
#endif
}

#define STALE_TEMP_AGE 3600

struct file_info {
    char *path;
    time_t mtime;
    long long size;
};

static void cleanup_stale_temps(const char *dir, GHashTable *regulars,
                                GList *temps) {
    time_t now = time(NULL);
    GList *l;

    for (l = temps; l; l = l->next) {
        char *tmp_name = l->data;
        char *tmp_mark = strstr(tmp_name, ".tmp.");
        int base_len;
        char *base_name;
        char *full_tmp;
        char *full_base;
        struct stat st;
        int del_tmp;
        int del_base;

        base_len = tmp_mark - tmp_name;
        base_name = g_strndup(tmp_name, base_len);
        full_tmp = g_build_filename(dir, tmp_name, NULL);
        full_base = g_hash_table_lookup(regulars, base_name);
        del_tmp = 0;
        del_base = 0;

        if (!full_base) {
            if (g_stat(full_tmp, &st) == 0 && now - st.st_mtime > STALE_TEMP_AGE)
                del_tmp = 1;
        } else if (g_stat(full_base, &st) == 0 && st.st_size == 0) {
            if (g_stat(full_tmp, &st) == 0 && now - st.st_mtime > STALE_TEMP_AGE) {
                del_base = 1;
                del_tmp = 1;
            }
        } else if (g_stat(full_base, &st) == 0 && st.st_size > 0) {
            del_tmp = 1;
        }

        if (del_base) {
            dbg(lvl_debug, "removing stale lock %s", full_base);
            g_unlink(full_base);
            g_hash_table_remove(regulars, base_name);
        }
        if (del_tmp) {
            dbg(lvl_debug, "removing stale temp %s", full_tmp);
            g_unlink(full_tmp);
        }

        g_free(base_name);
        g_free(full_tmp);
    }
}

static void cleanup_orphan_locks(GHashTable *regulars) {
    GHashTableIter iter;
    gpointer key, value;
    struct stat st;

    g_hash_table_iter_init(&iter, regulars);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        char *path = value;
        if (g_stat(path, &st) == 0 && st.st_size == 0) {
            dbg(lvl_debug, "removing stale lock (no temp) %s", path);
            g_unlink(path);
            g_hash_table_iter_remove(&iter);
        }
    }
}

static void lru_evict(GHashTable *regulars, size_t max_bytes) {
    GList *remaining;
    long long total;
    int count;
    struct file_info *info;
    int idx;
    GList *f;
    struct stat st;
    int sorted;
    int i;

    remaining = g_hash_table_get_values(regulars);
    if (!remaining)
        return;

    total = 0;
    count = g_list_length(remaining);
    info = g_new0(struct file_info, count);
    idx = 0;

    for (f = remaining; f; f = g_list_next(f), idx++) {
        info[idx].path = g_strdup((char *)f->data);
        if (g_stat(info[idx].path, &st) == 0) {
            info[idx].mtime = st.st_mtime;
            info[idx].size = st.st_size;
        }
        total += info[idx].size;
    }

    if (total > (long long)max_bytes) {
        sorted = 0;
        while (!sorted) {
            sorted = 1;
            for (i = 0; i < count - 1; i++) {
                if (info[i].mtime > info[i + 1].mtime) {
                    struct file_info tmp = info[i];
                    info[i] = info[i + 1];
                    info[i + 1] = tmp;
                    sorted = 0;
                }
            }
        }

        for (i = 0; i < count && total > (long long)max_bytes; i++) {
            if (g_unlink(info[i].path) == 0) {
                total -= info[i].size;
                dbg(lvl_debug, "evicted %s (size %lld)", info[i].path,
                    (long long)info[i].size);
            }
        }
    }

    for (i = 0; i < count; i++)
        g_free(info[i].path);
    g_free(info);
    g_list_free(remaining);
}

void audio_cache_cleanup(struct audio_cache *ac, size_t max_bytes,
                         const char *suffix) {
    char *dir = ac->synthetic_dir;
    GHashTable *regulars;
    GList *temps;
    void *handle;
    char *name;
    int suffix_len;

    regulars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    temps = NULL;
    handle = file_opendir(dir);
    if (!handle) {
        g_hash_table_destroy(regulars);
        return;
    }
    suffix_len = suffix ? strlen(suffix) : 0;
    while ((name = file_readdir(handle))) {
        int len = strlen(name);
        char *tmp_mark = strstr(name, ".tmp.");
        if (tmp_mark) {
            temps = g_list_prepend(temps, g_strdup(name));
        } else if (len > suffix_len && (!suffix || !strcmp(name + len - suffix_len, suffix))) {
            char *full = g_build_filename(dir, name, NULL);
            g_hash_table_insert(regulars, g_strdup(name), full);
        }
    }
    file_closedir(handle);

    cleanup_stale_temps(dir, regulars, temps);
    g_list_free_full(temps, g_free);

    cleanup_orphan_locks(regulars);

    lru_evict(regulars, max_bytes);

    g_hash_table_destroy(regulars);
}
