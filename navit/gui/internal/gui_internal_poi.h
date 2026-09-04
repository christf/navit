
/**
 * POI search/filtering parameters.
 *
 */

#ifndef _NAVIT_INTERNAL_POI_H
#define _NAVIT_INTERNAL_POI_H

#include "route.h"
#include <glib.h>

/**
 * A POI category selector entry, used by the POI category grid.
 *
 */
struct selector {
    char *icon;
    char *name;
    enum item_type *types;  // even items are start, odd ones are end of selection
};

/**
 * The list of POI category selectors offered by the category grid.
 */
extern struct selector selectors[];

/**
 * The number of entries in selectors[].
 */
extern const int selector_count;

struct poi_param {

    /**
     * =1 if selnb is defined, 0 otherwize.
     */
    unsigned char sel;

    /**
     * Index to struct selector selectors[], shows what type of POIs is defined.
     */
    unsigned char selnb;
    /**
     * Page number to display.
     */
    unsigned char pagenb;
    /**
     * Radius (number of 10-kilometer intervals) to search for POIs.
     */
    unsigned char dist;
    /**
     * Should filter phrase be compared to postal address of the POI.
     * =0 - name filter, =1 - address filter, =2 - address filter, including postal code
     */
    unsigned char AddressFilterType;
    /**
     * Filter string, casefold()ed and divided into substrings at the spaces, which are replaced by ASCII 0*.
     */
    char *filterstr;
    /**
     * list of pointers to individual substrings of filterstr.
     */
    GList *filter;
    /**
     * Number of POIs in this list
     */
    int count;
};

/* prototypes */
struct coord;
struct graphics_image;
struct gui_priv;
struct item;
struct poi_param;
struct widget;
void gui_internal_poi_param_free(void *p);
struct poi_param *gui_internal_poi_param_clone(struct poi_param *p);
void gui_internal_poi_param_set_filter(struct poi_param *param, char *text);
int gui_internal_pois_item_selected(struct poi_param *param, struct item *item);
struct graphics_image *gui_internal_poi_icon(struct gui_priv *this, struct item *item);
struct widget *gui_internal_cmd_pois_item(struct gui_priv *this, struct coord *center, struct item *item,
                                          struct coord *c, struct route *route, int dist, char *name);
char *gui_internal_compose_item_address_string(struct item *item, int prependPostal);
void gui_internal_cmd_pois_filter(struct gui_priv *this, struct widget *wm, void *data);
void gui_internal_cmd_pois(struct gui_priv *this, struct widget *wm, void *data);
/* end of prototypes */

#endif
