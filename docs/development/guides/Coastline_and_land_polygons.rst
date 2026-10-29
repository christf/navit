.. _coastline_land_polygons:

Coastline and land polygons
===========================

``maptool`` derives the ocean fill from the ``natural=coastline`` ways in
its input. In a regional extract the coastline is cut at the extract
border and may contain gaps, so the fill can leak onto land. The ``-L``
(``--land-polygons``) option supplies an independent, authoritative land
mask: the finest ocean-fill tiles whose centre falls on land are
suppressed, while coarser tiles keep their normal propagation. The same
mask also fills ocean areas that contain no coastline: level-14 tiles
whose centre is not land become water, and those tiles do not expand
into coarser tiles.

The fill is limited to the intersection of the mask bounding box and the
input PBF header bounding box. If the input has no header bounding box,
the mask bounding box is used; a mask covering the whole world is
rejected because it would enumerate global tiles. Clip the mask or use an
input with a bounded header for cutouts.

The mask is optional; without ``-L`` ``maptool`` behaves as before.

Where to obtain the shapefiles
------------------------------

The `OSMData project <https://osmdata.openstreetmap.de/>`__ publishes
daily global extracts derived from OpenStreetMap:

-  `Land polygons <https://osmdata.openstreetmap.de/data/land-polygons.html>`__
   -- polygonized land, produced with ``osmcoastline``. This is what
   ``-L`` expects.
-  `Coastlines <https://osmdata.openstreetmap.de/data/coastlines.html>`__
   -- coastline linestrings, if you need the raw geometry.

Download the WGS84 complete land polygons:

.. code:: bash

   wget https://osmdata.openstreetmap.de/download/land-polygons-complete-4326.zip
   unzip land-polygons-complete-4326.zip

This produces ``land-polygons-complete-4326/land_polygons.shp`` together
with ``.shx``, ``.dbf``, ``.prj`` and ``.cpg``. The 2026-10-27 build has
833,680 polygons; the shapefile is about 1.22 GiB unpacked.

Use the **4326** (WGS84, longitude/latitude) variant. ``maptool``
interprets the shapefile coordinates as longitude/latitude, so the 3857
variants will not work. The ``complete`` variant is a single shapefile;
``maptool -L`` takes one shapefile, so use ``complete`` or merge the
``split`` tiles first.

Building a map using a global shapefile
---------------------------------------

It is possible to pass the global file directly to maptool

.. code:: bash

   maptool -P -i region.osm.pbf \
     -L /path/to/land-polygons-complete-4326/land_polygons.shp \
     region.bin

This works, but ``maptool`` loads all polygons into memory and
scans them for every candidate tile. For repeated regional builds, clip the
mask:

.. code:: bash

   ogr2ogr -f "ESRI Shapefile" \
     -clipsrc LON_MIN LAT_MIN LON_MAX LAT_MAX \
     region_land.shp \
     /path/to/land-polygons-complete-4326/land_polygons.shp

Example for Niedersachsen (5.5°E--11.5°E, 51.5°N--54.5°N):

.. code:: bash

   ogr2ogr -f "ESRI Shapefile" -clipsrc 5.5 51.5 11.5 54.5 \
     ~/nobackup/osm/land/niedersachsen_land.shp \
     ~/nobackup/osm/land/land-polygons-complete-4326/land_polygons.shp

``-clipsrc`` takes ``xmin ymin xmax ymax``; use it rather than
``-spat`` so that polygons crossing the border are actually clipped.
Keep the generated ``.shp``, ``.shx`` and ``.dbf`` files together.

Building a map
--------------

.. code:: bash

   maptool -C lzma -z6 -P -i region.osm.pbf \
     -L region_land.shp region.bin

On stderr ``maptool`` reports how many polygons it loaded:

.. code:: text

   Loaded 111 land polygons from '/path/to/region_land.shp'

If this line is absent, or reports 0 polygons, the path is wrong,
the shapefile is incomplete, or it contains no polygon features. ``-L`` may appear anywhere before the output file; the long
form is ``--land-polygons``.

Identifying the map extent
~~~~~~~~~~~~~~~~~~~~~~~~~~

``maptool`` limits the mask fill to the intersection of the land-mask
bounding box and the PBF header bounding box. Read both with ``osmium``:

.. code:: bash

   osmium fileinfo -e -g header.boxes region.osm.pbf
   osmium fileinfo -e -g data.bbox region.osm.pbf

The header box is the intended extract extent and is what ``maptool``
uses. The data box can be larger because complete ways and relations
cross the extract boundary. If the header box is empty, set it when
creating the cutout:

.. code:: bash

   osmium extract --set-bounds -b LON_MIN LAT_MIN LON_MAX LAT_MAX \
     region.osm.pbf -o cutout.osm.pbf

Cutouts
-------

A cutout should carry its own coastline or a land mask clipped to its
area. Without a coastline, a cutout has no land or island to anchor the
ocean fill, so provide the coastline in the extract as well. Bounds in
the PBF header are also used to limit the fill; set them as described
above. If the cutout has no header bounds, clip the land mask to the same
extent; otherwise the fill covers the mask's full bounding box.

Regenerating the data
---------------------

The OSMData files are rebuilt daily. To generate the same data, you need a
planet PBF (currently on the order of 80 GB) and ``osmium`` plus
``osmcoastline``:

.. code:: bash

   osmium tags-filter planet-latest.osm.pbf w/natural=coastline -o coastline.osm.pbf
   mkdir land
   osmcoastline -f -g "ESRI Shapefile" -o land -p land coastline.osm.pbf

``osmcoastline`` holds the global coastline graph in memory; expect
several gigabytes of RAM and tens of minutes. The result is a single
WGS84 land-polygon shapefile at ``land/land_polygons.shp``, which can be
used directly or clipped as above.

See also
--------

-  `OSMData land polygons <https://osmdata.openstreetmap.de/data/land-polygons.html>`__
-  `OSMData coastline processing <https://osmdata.openstreetmap.de/processing/coastline.html>`__
-  :doc:`/development/concepts/Working_with_map_data`
