.. _layout_examples:

Layout Examples
===============

The way a map and cursor (the thing that shows your current location) is rendered in Navit is controlled by the particular Layout which has been selected. As with almost everything else in Navit, Layouts are highly configurable. Below are user-submitted examples of Navit Layouts.


Adding a Layout
---------------

If you want to share a layout with other Navit users, please do so using this page. Leave this page as an introduction to your layout (use the other layout descriptions as a template), and link to a new page where you can include extra images and the relevant xml code.


Alternate Layouts
-----------------

Mapnik
~~~~~~

This layout tries to closely mimic the Mapnik rendering style used by default over at `OpenStreetMap <http://www.openstreetmap.org>`_. It even uses the same icon styles where available (this means you will have to download the relevant icons - a link is provided).

**Features:**

- Cycle ways are more prominently displayed in purple
- Routing is shown with a bright green line which is overlaid onto the road, rather than a fat blue line underneath it. Road names appear above the routing line.
- Zoom settings for various POIs have been changed. For example, fuel station POIs are shown out to quite a far zoom level.
  - POIs which will be most important to *navigating drivers* are prominently displayed, whilst those which are perhaps interesting but not very useful when navigating are less noticeable and/or only show up when zooming in closer. This is so that unhelpful POIs do not clutter up the map view.
- Bus stops are shown with a blue ring, until zoomed in quite close when a proper icon is used.
  - There are a lot of bus stops everywhere, and the POI icon was cluttering up the map. The unobtrusive blue ring is still noticeable, but less annoying!
- Mini-roundabout icons have been removed, and are now shown by black rings.
- A few POI types which do not appear in maptool's osm.c (i.e. don't actually get converted from OSM and won't currently appear in the Navit data) have been removed.


Mapnik for small screens
~~~~~~~~~~~~~~~~~~~~~~~~

Based upon the original Mapnik style, these map layouts are optimised for devices with smaller screens. There are two layouts available:

- **HDPI** - for high definition small screens
- **MDPI** - for medium definition small screens

**Features:**

- Only navigation-important POIs are shown
- Reduced the number of visible elements at higher zoom levels
- Increased font sizes for town and street names


Other Layouts
-------------

Various other layouts are available for different use cases:

- **Car** - Default layout for car navigation
- **Bike** - Layout optimized for bicycle navigation (simple layout, no dashed lines, optimized for broken winCE renderer)
- **Pedestrian** - Layout for walking navigation
- **Winter/Snow** - Layout with high contrast for winter conditions
- **Hi-Vis** - High visibility layout for outdoor use
- **Detailed Camping Bike** - Detailed layout for camping cyclists
- **Mapnik for HDPI** - Mapnik style for high-density small screens
- **Mapnik for MDPI** - Mapnik style for medium-density small screens


Customizing Layouts
-------------------

Layouts can be customized by modifying the layout section in navit.xml. Each layout consists of:

- **Layers** - Different rendering layers for various map elements
- **Itemgra** - Rules for when and how to draw items
- **Colors** - Customizable colors for roads, areas, and other elements
- **Icons** - Different icons for POIs and navigation elements

For detailed information on layout configuration, see the :doc:`layout` page.
