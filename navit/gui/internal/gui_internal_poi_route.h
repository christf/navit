
/**
 * @file
 *
 * @brief POI search along the remaining route for the internal GUI.
 *
 * Provides the "POIs along Route" feature: searches POIs within a configurable
 * perpendicular distance of the remaining route (current position to destination),
 * displays them sorted by distance along route or detour cost, and allows adding
 * them as waypoints at the correct geographic position along the route.
 */

#ifndef NAVIT_GUI_INTERNAL_POI_ROUTE_H
#define NAVIT_GUI_INTERNAL_POI_ROUTE_H

struct gui_priv;
struct widget;

/* prototypes */
void gui_internal_cmd_pois_along_route(struct gui_priv *this, struct widget *wm, void *data);
/* end of prototypes */

#endif
