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

#include "synthesizer.h"
#include "debug.h"
#include "item.h"
#include "plugin.h"
#include "xmlconfig.h"
#include <glib.h>
#include <string.h>

struct synthesizer {
    NAVIT_OBJECT;
    struct synthesizer_priv *priv;
    struct synthesizer_methods meth;
};

struct synthesizer *synthesizer_new(struct attr *parent, struct attr **attrs) {
    struct synthesizer *this_;
    struct synthesizer_priv *(*synthesizer_new)(struct synthesizer_methods *meth, struct attr **attrs,
                                                struct attr *parent);
    struct attr *attr;

    attr = attr_search(attrs, attr_type);
    if (!attr) {
        dbg(lvl_error, "type missing");
        return NULL;
    }
    dbg(lvl_debug, "type='%s'", attr->u.str);
    synthesizer_new = plugin_get_category_synthesizer(attr->u.str);
    dbg(lvl_debug, "new=%p", synthesizer_new);
    if (!synthesizer_new) {
        dbg(lvl_error, "wrong type '%s'", attr->u.str);
        return NULL;
    }
    this_ = (struct synthesizer *)navit_object_new(attrs, &synthesizer_func, sizeof(struct synthesizer));
    this_->priv = synthesizer_new(&this_->meth, this_->attrs, parent);
    dbg(lvl_debug, "synthesize=%p priv=%p", this_->meth.synthesize, this_->priv);
    if (!this_->priv) {
        synthesizer_destroy(this_);
        return NULL;
    }
    return this_;
}

void synthesizer_destroy(struct synthesizer *this_) {
    if (this_->priv)
        this_->meth.destroy(this_->priv);
    navit_object_destroy((struct navit_object *)this_);
}

int synthesizer_synthesize(struct synthesizer *this_, const char *text, const char *output_path,
                           synthesizer_batch_id batch) {
    dbg(lvl_debug, "this_=%p text='%s' output='%s' batch=%llu", this_, text, output_path, batch);
    return (this_->meth.synthesize)(this_->priv, text, output_path, batch);
}

synthesizer_batch_id synthesizer_batch_begin(struct synthesizer *this_) {
    if (this_->meth.batch_begin)
        return (this_->meth.batch_begin)(this_->priv);
    return 0;
}

int synthesizer_check_status(struct synthesizer *this_) {
    if (this_->meth.check_status)
        return (this_->meth.check_status)(this_->priv);
    return -1;
}

int synthesizer_get_attr(struct synthesizer *this_, enum attr_type type, struct attr *attr, struct attr_iter *iter) {
    return attr_generic_get_attr(this_->attrs, NULL, type, attr, iter);
}

int synthesizer_set_attr(struct synthesizer *this_, struct attr *attr) {
    this_->attrs = attr_generic_set_attr(this_->attrs, attr);
    return 1;
}

struct object_func synthesizer_func = {
    attr_synthesizer,
    (object_func_new)synthesizer_new,
    (object_func_get_attr)synthesizer_get_attr,
    (object_func_iter_new)navit_object_attr_iter_new,
    (object_func_iter_destroy)navit_object_attr_iter_destroy,
    (object_func_set_attr)synthesizer_set_attr,
    (object_func_add_attr)navit_object_add_attr,
    (object_func_remove_attr)navit_object_remove_attr,
    (object_func_init)NULL,
    (object_func_destroy)synthesizer_destroy,
    (object_func_dup)NULL,
    (object_func_ref)navit_object_ref,
    (object_func_unref)navit_object_unref,
};
