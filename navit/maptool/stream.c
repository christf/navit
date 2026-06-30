#include "stream.h"
#include "maptool.h"
#include <glib.h>
#include <stdio.h>
#include <string.h>

/* --- Relation member parsing (mirrors osm.c's static helpers) --- */
struct rel_member {
    enum relation_member_type type;
    osmid id;
    char *role;
};

static int parse_member_str(char *raw, struct rel_member *m) {
    int type_numeric;
    int len;
    if (sscanf(raw, RELATION_MEMBER_PARSE_FORMAT, &type_numeric, &m->id, &len) < 2)
        return 0;
    m->type = (enum relation_member_type)type_numeric;
    m->role = raw + len;
    return 1;
}

static int find_member_by_role(struct item_bin *ib, const char *role,
                                struct rel_member *m, int *min_count) {
    char *str = NULL;
    int count = 0;
    while ((str = item_bin_get_attr(ib, attr_osm_member, str))) {
        struct rel_member cur;
        if (!parse_member_str(str, &cur))
            continue;
        count++;
        if (!g_strcmp0(cur.role, role) && (!min_count || *min_count < count)) {
            if (min_count)
                *min_count = count;
            *m = cur;
            return 1;
        }
    }
    return 0;
}

/* --- Associated street index --- */
struct assoc_street_index {
    GHashTable *way_to_name; /* (gpointer)(osmid) → g_strdup'd name or "" */
};

static struct assoc_street_index *assoc_street_index_new(FILE *in) {
    struct assoc_street_index *idx;
    struct item_bin *ib;

    if (!in)
        return NULL;
    idx = g_malloc0(sizeof(*idx));
    idx->way_to_name = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    fseek(in, 0, SEEK_SET);
    while ((ib = read_item(in))) {
        char *name = osm_tag_value(ib, "name");
        int roles_done = 0;
        const char *roles[] = {"house", "addr:houselink", "address", NULL};
        int ri;
        for (ri = 0; roles[ri]; ri++) {
            struct rel_member m;
            int mc = 0;
            while (find_member_by_role(ib, roles[ri], &m, &mc)) {
                char *prev;
                if (m.type != rel_member_way)
                    continue;
                prev = g_hash_table_lookup(idx->way_to_name, (gpointer)(long)m.id);
                if (!prev || prev[0] == '\0')
                    g_hash_table_insert(idx->way_to_name, (gpointer)(long)m.id,
                                        g_strdup(name && name[0] ? name : ""));
                roles_done = 1;
            }
        }
        if (!roles_done) {
            char *str = NULL;
            while ((str = item_bin_get_attr(ib, attr_osm_member, str))) {
                struct rel_member m;
                char *prev;
                if (!parse_member_str(str, &m))
                    continue;
                if (m.type != rel_member_way)
                    continue;
                prev = g_hash_table_lookup(idx->way_to_name, (gpointer)(long)m.id);
                if (!prev || prev[0] == '\0')
                    g_hash_table_insert(idx->way_to_name, (gpointer)(long)m.id,
                                        g_strdup(name && name[0] ? name : ""));
            }
        }
    }
    return idx;
}

static void assoc_street_index_destroy(struct assoc_street_index *idx) {
    if (!idx)
        return;
    g_hash_table_destroy(idx->way_to_name);
    g_free(idx);
}

static char *assoc_street_lookup(struct assoc_street_index *idx, osmid wayid) {
    if (!idx)
        return NULL;
    return g_hash_table_lookup(idx->way_to_name, (gpointer)(long)wayid);
}

/* --- House-number interpolation index --- */
struct hn_entry {
    osmid first_node;
    osmid last_node;
};

struct hn_index {
    GHashTable *way_to_entry; /* osmid → struct hn_entry* */
};

static struct hn_index *hn_index_new(FILE *in) {
    struct hn_index *idx;
    struct item_bin *ib;

    if (!in)
        return NULL;
    idx = g_malloc0(sizeof(*idx));
    idx->way_to_entry = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    fseek(in, 0, SEEK_SET);
    while ((ib = read_item(in))) {
        struct hn_entry *e = g_malloc0(sizeof(*e));
        osmid wayid = item_bin_get_wayid(ib);
        if (!wayid) {
            g_free(e);
            continue;
        }
        osmid *nodeid = item_bin_get_attr(ib, attr_osm_nodeid_first_node, NULL);
        if (nodeid)
            e->first_node = *nodeid;
        nodeid = item_bin_get_attr(ib, attr_osm_nodeid_last_node, NULL);
        if (nodeid)
            e->last_node = *nodeid;
        g_hash_table_insert(idx->way_to_entry, (gpointer)(long)wayid, e);
    }
    return idx;
}

static void hn_index_destroy(struct hn_index *idx) {
    if (!idx)
        return;
    g_hash_table_destroy(idx->way_to_entry);
    g_free(idx);
}

static struct hn_entry *hn_entry_lookup(struct hn_index *idx, osmid wayid) {
    if (!idx)
        return NULL;
    return g_hash_table_lookup(idx->way_to_entry, (gpointer)(long)wayid);
}

/* --- Stream state --- */
struct stream_state {
    struct assoc_street_index *assoc_idx;
    struct hn_index *hn_idx;
    struct tile_info tile_info;
    GList *tiles_list;
    struct zip_info *zip_info;
    char *suffix;
};

struct stream_state *stream_new(struct zip_info *zip_info, char *suffix,
                                struct tile_info *tile_info) {
    struct stream_state *ss;
    FILE *f;

    ss = g_malloc0(sizeof(*ss));
    ss->tile_info = *tile_info;
    ss->tile_info.tiles_list = &ss->tiles_list;
    ss->zip_info = zip_info;
    ss->suffix = suffix;

    f = tempfile(suffix, "associated_streets", 0);
    ss->assoc_idx = assoc_street_index_new(f);
    if (f)
        fclose(f);

    f = tempfile(suffix, "house_number_interpolations", 0);
    ss->hn_idx = hn_index_new(f);
    if (f)
        fclose(f);

    if (!tile_hash)
        tile_hash = g_hash_table_new(g_str_hash, g_str_equal);
    create_tile_hash();
    return ss;
}

void stream_feed(struct stream_state *ss, struct item_bin *ib) {
    osmid wayid;
    char *street_name;

    wayid = 0;

    /* Associated street enrichment */
    if (ss->assoc_idx) {
        enum item_type t = ib->type;
        int is_interp = (t >= type_house_number_interpolation_even
                         && t <= type_house_number_interpolation_alphabetic);
        if (is_interp) {
            wayid = item_bin_get_wayid(ib);
            street_name = assoc_street_lookup(ss->assoc_idx, wayid);
            if (street_name && street_name[0]
                && !item_bin_get_attr(ib, attr_street_name, NULL)) {
                item_bin_add_attr_string(ib, attr_street_name, street_name);
            }
        }
    }

    /* House-number interpolation enrichment */
    if (ss->hn_idx) {
        if (!wayid)
            wayid = item_bin_get_wayid(ib);
        if (wayid) {
            struct hn_entry *e = hn_entry_lookup(ss->hn_idx, wayid);
            if (e) {
                item_bin_add_attr_longlong(ib, attr_osm_nodeid_first_node, e->first_node);
                item_bin_add_attr_longlong(ib, attr_osm_nodeid_last_node, e->last_node);
            }
        }
    }

    /* Route to tile */
    tile_write_item_minmax(&ss->tile_info, ib, NULL, 0,
                           item_order_by_type(ib->type));
}

void stream_flush(struct stream_state *ss) {
    int tile_count = 0;
    struct tile_head *th;

    for (th = tile_head_root; th; th = th->next) {
        if (th->process && th->name[0] && th->total_size > 0)
            tile_count++;
    }

    tile_write_index_tiles(ss->zip_info);

    if (tile_count == 0)
        return;

    if (ss->tile_info.tile_compress_queue) {
        tile_push_all_tiles(&ss->tile_info);
        tile_consume_done_queue(ss->zip_info, tile_count);
    } else {
        int maxnamelen = zip_get_maxnamelen(ss->zip_info);
        for (th = tile_head_root; th; th = th->next) {
            if (!th->process || !th->name[0] || th->total_size <= 0)
                continue;
            if (th->total_size != th->total_size_used) {
                fprintf(stderr, "Size error '%s': %d vs %d\n", th->name, th->total_size, th->total_size_used);
                exit(1);
            }
            th->zipnum = zip_get_zipnum(ss->zip_info);
            write_zipmember(ss->zip_info, th->name, maxnamelen, th->zip_data, th->total_size);
            zip_add_member(ss->zip_info);
        }
    }
    /* Write type_submap index entries so navit can find tiles by coordinate */
    for (th = tile_head_root; th; th = th->next) {
        if (!th->process || !th->name[0] || th->total_size <= 0)
            continue;
        struct rect r;
        int tlen = tile_len(th->name);
        tile_bbox(th->name, &r, overlap);
        struct item_bin *ib = init_item(type_submap);
        item_bin_add_coord_rect(ib, &r);
        item_bin_add_attr_range(ib, attr_order, (tlen > 4) ? tlen - 4 : 0, 255);
        item_bin_add_attr_int(ib, attr_zipfile_ref, th->zipnum);
        item_bin_write(ib, zip_get_index(ss->zip_info));
    }
}

void stream_destroy(struct stream_state *ss) {
    assoc_street_index_destroy(ss->assoc_idx);
    hn_index_destroy(ss->hn_idx);
    g_free(ss);
}
