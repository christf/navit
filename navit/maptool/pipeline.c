#include "pipeline.h"
#include <glib.h>

struct pipeline_stage {
    pipeline_consumer_fn process;
    pipeline_flush_fn flush;
    void *userdata;
    struct pipeline_stage *next;
};

void pipeline_init(struct pipeline *p) {
    p->head = NULL;
    p->tail = NULL;
}

void pipeline_add(struct pipeline *p, pipeline_consumer_fn process, pipeline_flush_fn flush, void *userdata) {
    struct pipeline_stage *s = g_malloc(sizeof(*s));
    s->process = process;
    s->flush = flush;
    s->userdata = userdata;
    s->next = NULL;
    if (p->tail)
        p->tail->next = s;
    else
        p->head = s;
    p->tail = s;
}

void pipeline_emit(struct pipeline *p, struct pipeline_item *item) {
    struct pipeline_stage *s;
    for (s = p->head; s; s = s->next)
        s->process(item, s->userdata);
}

void pipeline_flush(struct pipeline *p) {
    struct pipeline_stage *s;
    for (s = p->head; s; s = s->next) {
        if (s->flush)
            s->flush(s->userdata);
    }
}

void pipeline_destroy(struct pipeline *p) {
    struct pipeline_stage *s = p->head;
    while (s) {
        struct pipeline_stage *next = s->next;
        g_free(s);
        s = next;
    }
    p->head = NULL;
    p->tail = NULL;
}
