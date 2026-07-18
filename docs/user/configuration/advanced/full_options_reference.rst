.. _full_options_reference:

Full Options Reference
======================

This page provides a complete reference of all configuration options available in navit.xml.


XML Structure
-------------

The image below details the complete xml tag structure for the default navit.xml. The colour scheme of the image is designed to give an impression of what is and what isn't required (both for Navit to run successfully in the first place, and to have a functional navigation system) - those tags in blue are required, whilst those in green can be regarded as optional.


config
------

+-----------+-------+--------------+--------------+---------------------+
| Attribute | Units | Values       | Notes        | Example             |
+===========+=======+==============+==============+=====================+
| language  |       | "en_US",     | Enables      | ``language="en_US"``|
|           |       | "en_GB",     | manual       |                     |
|           |       | "pl_PL",     | setting of   |                     |
|           |       | ...          | the locale.  |                     |
+-----------+-------+--------------+--------------+---------------------+


debug
-----

+-----------+-------+--------------+--------------+---------------------+
| Attribute | Units | Values       | Notes        | Example             |
+===========+=======+==============+==============+=====================+
| name      |       | "navigation" | Debug        | ``name="navigation"``|
|           |       | "gui"        | categories   |                     |
|           |       | "vehicle"    |              |                     |
|           |       | "map"        |              |                     |
|           |       | "speech"     |              |                     |
|           |       | "plugin"     |              |                     |
|           |       | "misc"       |              |                     |
|           |       | "arm"        |              |                     |
|           |       | "graphics"   |              |                     |
|           |       | "zomm"       |              |                     |
|           |       | "geo"        |              |                     |
|           |       | "search"     |              |                     |
|           |       | "routing"    |              |                     |
|           |       | "iso8583"    |              |                     |
|           |       | "osd"        |              |                     |
|           |       | "speech_cmdline" |           |                     |
+-----------+-------+--------------+--------------+---------------------+
| enabled   |       | "yes", "no"  | Enable/disable| ``enabled="yes"``  |
|           |       |              | debug output |                     |
+-----------+-------+--------------+--------------+---------------------+


gui
---

+-----------+-------+--------------+--------------+---------------------+
| Attribute | Units | Values       | Notes        | Example             |
+===========+=======+==============+==============+=====================+
| name      |       | "internal",  | GUI type     | ``name="internal"`` |
|           |       | "gtk",       |              |                     |
|           |       | "sdl",       |              |                     |
|           |       | "qml2",      |              |                     |
|           |       | "win32",     |              |                     |
|           |       | "framebuffer",|              |                     |
|           |       | "maemo",     |              |                     |
+-----------+-------+--------------+--------------+---------------------+
| enabled   |       | "yes", "no"  | Enable/disable| ``enabled="yes"``  |
|           |       |              | this GUI     |                     |
+-----------+-------+--------------+--------------+---------------------+
| menubar   |       | "yes", "no"  | Show/hide    | ``menubar="yes"``  |
|           |       |              | menu bar     |                     |
+-----------+-------+--------------+--------------+---------------------+
| toolbar   |       | "yes", "no"  | Show/hide    | ``toolbar="yes"``  |
|           |       |              | toolbar      |                     |
+-----------+-------+--------------+--------------+---------------------+
| statusbar |       | "yes", "no"  | Show/hide    | ``statusbar="yes"``|
|           |       |              | status bar   |                     |
+-----------+-------+--------------+--------------+---------------------+
| window    |       | "yes", "no"  | Show/hide    | ``window="yes"``   |
|           |       |              | window       |                     |
+-----------+-------+--------------+--------------+---------------------+


vehicle
-------

+-----------+-------+--------------+--------------+---------------------+
| Attribute | Units | Values       | Notes        | Example             |
+===========+=======+==============+==============+=====================+
| name      |       | string       | Vehicle name | ``name="Car"``      |
+-----------+-------+--------------+--------------+---------------------+
| enabled   |       | "yes", "no"  | Enable/disable| ``enabled="yes"``  |
|           |       |              | this vehicle|                     |
+-----------+-------+--------------+--------------+---------------------+
| active    |       | "yes", "no"  | Set as active| ``active="yes"``    |
|           |       |              | vehicle      |                     |
+-----------+-------+--------------+--------------+---------------------+
| language  |       | "en_US",     | Enables      | ``language="en_US"``|
|           |       | "en_GB",     | manual       |                     |
|           |       | "pl_PL",     | setting of   |                     |
|           |       | ...          | the locale.  |                     |
+-----------+-------+--------------+--------------+---------------------+
| lang_pref |       | comma-sep    | Comma-       | ``lang_pref="de,en"``|
|           |       | language     | separated    |                     |
|           |       | codes        | language     |                     |
|           |       |              | priority     |                     |
|           |       |              | list for     |                     |
|           |       |              | map label    |                     |
|           |       |              | display      |                     |
+-----------+-------+--------------+--------------+---------------------+
| gpsd_host |       | hostname     | GPSD server  | ``gpsd_host="localhost"``|
+-----------+-------+--------------+--------------+---------------------+
| gpsd_port |       | number       | GPSD port    | ``gpsd_port="2947"``|
+-----------+-------+--------------+--------------+---------------------+


map
---

+-----------+-------+--------------+--------------+---------------------+
| Attribute | Units | Values       | Notes        | Example             |
+===========+=======+==============+==============+=====================+
| enabled   |       | "yes", "no"  | Enable/disable| ``enabled="yes"``  |
|           |       |              | this map     |                     |
+-----------+-------+--------------+--------------+---------------------+
| type      |       | "binfile",   | Map format   | ``type="binfile"``  |
|           |       | "csv",       |              |                     |
|           |       | "gpx",       |              |                     |
|           |       | "json",      |              |                     |
|           |       | "navit",     |              |                     |
|           |       | "osm",       |              |                     |
|           |       | "poly",      |              |                     |
|           |       | "textfile",  |              |                     |
+-----------+-------+--------------+--------------+---------------------+
| data      |       | file path    | Map file     | ``data="/path/to/map.bin"``|
|           |       |              | path         |                     |
+-----------+-------+--------------+--------------+---------------------+


layout
------

+-----------+-------+--------------+--------------+---------------------+
| Attribute | Units | Values       | Notes        | Example             |
+===========+=======+==============+==============+=====================+
| name      |       | string       | Layout name  | ``name="Car"``      |
+-----------+-------+--------------+--------------+---------------------+
| enabled   |       | "yes", "no"  | Enable/disable| ``enabled="yes"``  |
|           |       |              | this layout |                     |
+-----------+-------+--------------+--------------+---------------------+
| active    |       | "yes", "no"  | Set as active| ``active="yes"``    |
|           |       |              | layout       |                     |
+-----------+-------+--------------+--------------+---------------------+


osd
---

+-----------+-------+--------------+--------------+---------------------+
| Attribute | Units | Values       | Notes        | Example             |
+===========+=======+==============+==============+=====================+
| type      |       | "text",      | OSD element  | ``type="text"``     |
|           |       | "image",     | type         |                     |
|           |       | "nav_turn_arrow", |          |                     |
|           |       | "gps_status",|              |                     |
|           |       | "navigation_next_turn", |    |                     |
|           |       | "navigation_status", |       |                     |
|           |       | "button",    |              |                     |
+-----------+-------+--------------+--------------+---------------------+
| enabled   |       | "yes", "no"  | Enable/disable| ``enabled="yes"``  |
|           |       |              | this OSD item|                     |
+-----------+-------+--------------+--------------+---------------------+
| x         | pixels| number       | X position   | ``x="0"``           |
|           | or %  |              |              |                     |
+-----------+-------+--------------+--------------+---------------------+
| y         | pixels| number       | Y position   | ``y="0"``           |
|           | or %  |              |              |                     |
+-----------+-------+--------------+--------------+---------------------+
| w         | pixels| number       | Width        | ``w="100"``         |
|           | or %  |              |              |                     |
+-----------+-------+--------------+--------------+---------------------+
| h         | pixels| number       | Height       | ``h="50"``          |
|           | or %  |              |              |                     |
+-----------+-------+--------------+--------------+---------------------+
| align     |       | number       | Alignment    | ``align="0"``       |
+-----------+-------+--------------+--------------+---------------------+
| label     |       | string       | Text label   | ``label="${vehicle.position_speed}"``|
+-----------+-------+--------------+--------------+---------------------+
| font_size |       | number       | Font size    | ``font_size="400"`` |
+-----------+-------+--------------+--------------+---------------------+
| command   |       | string       | Click        | ``command="gui.menu()"``|
|           |       |              | command      |                     |
+-----------+-------+--------------+--------------+---------------------+


For the complete list of options with detailed descriptions, please refer to the `Navit XML Configuration <https://wiki.navit-project.org/index.php/Configuration/Full_list_of_options>`_ wiki page.
