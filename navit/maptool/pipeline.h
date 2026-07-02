#ifndef NAVIT_MAPTOOL_PIPELINE_H
#define NAVIT_MAPTOOL_PIPELINE_H

#include "item.h"

struct pipeline_item {
    struct item_bin *ib;
};

struct pipeline_stage;

typedef void (*pipeline_consumer_fn)(struct pipeline_item *item, void *userdata);
typedef void (*pipeline_flush_fn)(void *userdata);
typedef void (*pipeline_destroy_fn)(void *userdata);

struct pipeline {
    struct pipeline_stage *head;
    struct pipeline_stage *tail;
    struct pipeline_stage *flush_stage;
};

void pipeline_init(struct pipeline *p);
void pipeline_add(struct pipeline *p, pipeline_consumer_fn process, pipeline_flush_fn flush,
                  pipeline_destroy_fn destroy, void *userdata);
void pipeline_emit(struct pipeline *p, struct pipeline_item *item);
void pipeline_emit_downstream(struct pipeline *p, struct pipeline_item *item);
void pipeline_flush(struct pipeline *p);
void pipeline_destroy(struct pipeline *p);

#endif
