.. _icons:

Icons
=====

Navit uses several icons for marking POIs on the map and for its GUI.

Icons are provided with the source tree into the ``navit/xpm/`` directory. Once installed, icons are stored usually in ``/usr/share/navit/xpm/``. Originally they were .xpm (bitmap) files sized 16x16 pixels, but they will be eventually replaced by scalable vector graphics (.svg) files. At compile time, .svg files are converted to .png bitmaps, scaled at the required sizes. You can control the sizes generated with the following ``./configure`` options:

.. code::

   --enable-svg2png-scaling=8,16,32,48,96
   --enable-svg2png-scaling-flag=8 16 32 48 96
   --enable-svg2png-scaling-nav=8,16,32,48,96

The first option refers to standard icons used into the map, the second option refers to country flag icons and the third is for special icons used for navigation (turns, roundabouts, etc.).

For each .svg file, a .png file will be created with the size specified into the .svg itself; this is usually **22x22** pixels. Besides that, for each size given at configure time, a .png file named **icon_XX_XX.png** will be created, where XX is the size in pixels.

Navit will use the icons as specified in the layout tag in the configuration file. For example, a fuel station will be displayed starting at zoom 12 using fuel.png icon:

.. code-block:: xml

   <itemgra item_types="poi_fuel" order="12-">
       <icon src="fuel.png"/>
   </itemgra>

Navit does not scale icons on the map automatically, it's up to the style to use different sizes at different zoom levels.

Navit instead scales **GUI icons** on the fly using the .svg files. You can specify the size into the gui tag of navit.xml:

.. code-block:: xml

   <gui type="internal" icon_xs="32" icon_s="96" icon_l="96" />

There are three types of icons:

``icon_xs``
   The size that extra-small style icons should be scaled to (e.g. country flag on town search).

``icon_s``
   The size that small style icons should be scaled to (e.g. icons of internal GUI toolbar).

``icon_l``
   The size that large style icons should be scaled to (e.g. icons of internal GUI menu).
