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

/** @file synthesizer.h
 * @brief Synthesizer plugin type for background audio generation.
 *
 * The synthesizer plugin type generates audio files from text, running
 * asynchronously so navigation is not blocked. Plugins are registered via
 * plugin_register_category_synthesizer() and looked up by the @ref attr_type
 * attribute in XML config.
 *
 * Built-in implementation:
 * - @b cmdline: runs an external command to produce audio. The command receives
 *   the text and output path as arguments. Synthesis requests are queued and
 *   at most one runs concurrently.
 *
 * Batch priority:
 *   When synthesizer_batch_begin() is called, a new batch ID (monotonically
 *   increasing timestamp) is issued. All subsequent synthesize() calls with
 *   that batch ID get higher priority than older batches. Pending (not yet
 *   running) entries from previous batches are discarded. This lets route
 *   recalculations preempt synthesis of outdated instructions.
 *
 * Pause/Resume:
 *   synthesizer_pause() prevents fill_slots from spawning new synthesis
 *   processes. Currently running processes continue until they finish.
 *   synthesizer_resume() re-enables spawning and wakes the worker thread.
 *   This allows on-demand (ad-hoc) synthesis to run without competing with
 *   background pre-synthesis.
 *
 * XML configuration for the cmdline synthesizer:
 * @code
 *   <synthesizer type="cmdline" data="navit-speech-cache-synthesize.sh"/>
 * @endcode
 * Attributes:
 *   - type (string, required): plugin type, e.g. "cmdline"
 *   - data (string, required): external command template. The text and output
 *     path are appended as arguments.
 */

#ifndef NAVIT_SYNTHESIZER_H
#define NAVIT_SYNTHESIZER_H

#include "attr.h"
#include "attr_type_def.h"

struct synthesizer_priv;
struct attr_iter;

typedef unsigned long long synthesizer_batch_id;

struct synthesizer_methods {
    void (*destroy)(struct synthesizer_priv *this_);
    int (*synthesize)(struct synthesizer_priv *this_, const char *text, const char *output_path,
                      synthesizer_batch_id batch);
    int (*check_status)(struct synthesizer_priv *this_);
    int (*wait_done)(struct synthesizer_priv *this_);
    synthesizer_batch_id (*batch_begin)(struct synthesizer_priv *this_);
    void (*pause)(struct synthesizer_priv *this_);
    void (*resume)(struct synthesizer_priv *this_);
};

struct synthesizer *synthesizer_new(struct attr *parent, struct attr **attrs);
int synthesizer_synthesize(struct synthesizer *this_, const char *text, const char *output_path,
                           synthesizer_batch_id batch);
synthesizer_batch_id synthesizer_batch_begin(struct synthesizer *this_);
int synthesizer_check_status(struct synthesizer *this_);
int synthesizer_wait_done(struct synthesizer *this_);
void synthesizer_destroy(struct synthesizer *this_);
void synthesizer_pause(struct synthesizer *this_);
void synthesizer_resume(struct synthesizer *this_);
int synthesizer_get_attr(struct synthesizer *this_, enum attr_type type, struct attr *attr, struct attr_iter *iter);
int synthesizer_set_attr(struct synthesizer *this_, struct attr *attr);

#endif
