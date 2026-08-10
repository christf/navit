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
#include "debug.h"
#include "maptool.h"
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifndef _MSC_VER
#    include <dirent.h>
#    include <unistd.h>
#endif
#include <string.h>

char *tempfile_obtain_prefix() {
    static char *tmpfile_prefix = NULL;

    if (!tmpfile_prefix) {
#define tmpfile_prefix_size 64
        tmpfile_prefix = calloc(tmpfile_prefix_size, 1);

        snprintf(tmpfile_prefix, tmpfile_prefix_size, "maptool_%d.tmp", getpid());
        if (mkdir(tmpfile_prefix, 0755)) {
            perror("Error creating directory");
            exit(1);
        }
    }
    return tmpfile_prefix;
}

void tempfile_cleanup() {
    char *prefix = tempfile_obtain_prefix();
    DIR *dir = opendir(prefix);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.')
                continue;
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", prefix, entry->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    rmdir(prefix);
}

char *tempfile_name(char *suffix, char *name) {
    return g_strdup_printf("%s/%s_%s.tmp", tempfile_obtain_prefix(), name, suffix);
}
FILE *tempfile(char *suffix, char *name, int mode) {
    char *buffer = tempfile_name(suffix, name);
    char *fmode;
    FILE *ret = NULL;
    int compressible = 1;
    switch (mode) {
    case 0:
        fmode = "rb";
        break;
    case 1:
        fmode = "wb+";
        break;
    case 2:
        fmode = "ab";
        break;
    default:
        fmode = "rb";
        break;
    }
    /* Files used through mmap need their raw bytes on disk. */
    if (!strcmp(name, "tiles_data") || !strcmp(name, "sgr") || !strcmp(name, "ddsg_coords"))
        compressible = 0;
    ret = tf_fopen(buffer, fmode, compressible);
    if (!ret && mode != 0) {
        fprintf(stderr, "maptool: cannot create temp file %s: %s\n", buffer, strerror(errno));
        tempfile_cleanup();
        exit(1);
    }
    g_free(buffer);
    return ret;
}

void tempfile_unlink(char *suffix, char *name) {
    char buffer[4096];
    sprintf(buffer, "%s/%s_%s.tmp", tempfile_obtain_prefix(), name, suffix);
    tf_cache_drop(buffer);
    unlink(buffer);
}

void tempfile_rename(char *suffix, char *from, char *to) {
    char buffer_from[4096], buffer_to[4096];
    sprintf(buffer_from, "%s/%s_%s.tmp", tempfile_obtain_prefix(), from, suffix);
    sprintf(buffer_to, "%s/%s_%s.tmp", tempfile_obtain_prefix(), to, suffix);
    tf_cache_rename(buffer_from, buffer_to);
    dbg_assert(rename(buffer_from, buffer_to) == 0);
}
