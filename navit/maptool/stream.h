#ifndef NAVIT_MAPTOOL_STREAM_H
#define NAVIT_MAPTOOL_STREAM_H

#include "maptool.h"

struct stream_state;

struct stream_state *stream_new(struct zip_info *zip_info, char *suffix,
                                struct tile_info *tile_info);
void stream_feed(struct stream_state *ss, struct item_bin *ib);
void stream_flush(struct stream_state *ss);
void stream_destroy(struct stream_state *ss);

#endif
