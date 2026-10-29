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

#include "shapefil.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static SAFile sa_open(const char *filename, const char *access) {
    return (SAFile)fopen(filename, access);
}

static SAOffset sa_read(void *p, SAOffset size, SAOffset nmemb, SAFile file) {
    return fread(p, size, nmemb, (FILE *)file);
}

static SAOffset sa_write(void *p, SAOffset size, SAOffset nmemb, SAFile file) {
    return fwrite(p, size, nmemb, (FILE *)file);
}

static SAOffset sa_seek(SAFile file, SAOffset offset, int whence) {
    return fseek((FILE *)file, offset, whence);
}

static SAOffset sa_tell(SAFile file) {
    return ftell((FILE *)file);
}

static int sa_flush(SAFile file) {
    return fflush((FILE *)file);
}

static int sa_close(SAFile file) {
    return fclose((FILE *)file);
}

static int sa_remove(const char *filename) {
    return unlink(filename);
}

static void sa_error(const char *message) {
    fprintf(stderr, "shapefile: %s\n", message);
}

void SASetupDefaultHooks(SAHooks *hooks) {
    hooks->FOpen = sa_open;
    hooks->FRead = sa_read;
    hooks->FWrite = sa_write;
    hooks->FSeek = sa_seek;
    hooks->FTell = sa_tell;
    hooks->FFlush = sa_flush;
    hooks->FClose = sa_close;
    hooks->Remove = sa_remove;
    hooks->Atof = atof;
    hooks->Error = sa_error;
}
