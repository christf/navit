.. _internal_gui:

Internal GUI
============

The Internal GUI is designed to be used on touch screen devices, but also works very well on other devices such as netbooks and laptops. It is under continual development and as such features are constantly being added and improved upon. If you think that a particular feature is missing or poorly implemented, come talk to us in the IRC channel or open up a feature request on the GitHub issue tracker.


Configuring Internal GUI
------------------------


Enabling Internal GUI
~~~~~~~~~~~~~~~~~~~~~

The Internal GUI is configured as the default GUI for Navit, so if you're reading this after a first install no further configuration is required.

If the configuration has changed since first install, the Internal GUI can be chosen by setting the ``type`` attribute in the gui tag.

Ensure that any other ``gui`` tags are disabled by setting their ``enabled`` attribute to "no".


Keyboard Preferences
~~~~~~~~~~~~~~~~~~~~

Some options inside the Internal GUI menu require keyboard input - for example, Town search. By default, Navit provides a custom on-screen keyboard to enter text. If your device has its own keyboard which you'd prefer to use, and you'd like to conserve some screen space then set the ``keyboard`` attribute to "false" inside the gui tag.


Map-click Preferences
~~~~~~~~~~~~~~~~~~~~~

By default, the menu appears when the map is clicked. This can be disabled by adding the following to the gui tag:

``menu_on_map_click="0"``

This of course means that you now can't access the menu by clicking on the map. Instead you will have to add an OSD item with the command ``gui.menu()``.

Icon and font sizes
~~~~~~~~~~~~~~~~~~~

You can also configure the icon sizes used in the menu:

.. code-block:: xml

   <gui type="internal" font_size="250" icon_xs="32" icon_s="48" icon_l="64">

- ``font_size`` is the font size
- ``icon_xs`` is the size of extra-small icons (e.g. country flag on town search)
- ``icon_s`` is the size of small icons (e.g. icons in toolbar and POI selector)
- ``icon_l`` is the size of large icons (e.g. icons in menu)

The icon sizes need to be available as files in Navit (they should be by default).


Menu Configuration
~~~~~~~~~~~~~~~~~~

Using Internal GUI, the menu can be brought up by clicking (almost) anywhere on the map, or pressing the Enter (Return) key on the device's keyboard.

The menu is configured using a html-like syntax inside the gui tags. Alternative configurations can be found in :doc:`menu_configurations`.


Using the Internal GUI
----------------------


Basics and breadcrumbs
~~~~~~~~~~~~~~~~~~~~~~

The Internal GUI should be mostly self-explanatory (that's the idea, at least - if it is not, please file a bug). It basically consists of different screens which show icons that can be clicked / touched, lists (such as search results) and input fields. For text input, a **virtual keyboard** is available. Of course, a regular hardware keyboard can be used if available.

On all screens of the Internal GUI, there is a **breadcrumb trail** at the top of the screen, which shows the current position inside the screen hierarchy of the Internal GUI. The breadcrumbs are clickable, to return to an earlier screen.


Operation with keyboard or rotary encoder
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

While the Internal GUI is mainly designed to be used with a mouse or touch screen, it can also be operated using a (hardware) keyboard or even a `rotary encoder <https://en.wikipedia.org/wiki/Rotary_encoder>`_ (which only offers "forward", "backward" and "enter").

The GUI elements can be navigated using arrow keys, and activated using Enter, as usual. Additionally, all GUI elements can also be reached using only PgUp/PgDown - this allows the use of a rotary encoder, if its actions are mapped to these two keys.

When using a rotary encoder (or cursor keys), it may be useful to set option ``hide_impossible_next_keys`` to hide irrelevant keys when searching.


Main Menu
---------

The main menu is accessed by a single click (or tap for touch screen) anywhere on the map. From here all other sub-menus and actions are accessible. The sub menu items are:

1. **Actions** - Bookmarks, former destinations, map point, current location, town search
2. **Settings** - Display, rules, vehicle, map, about
3. **Tools** - Various tools and utilities
4. **Route** - Route management
5. **About** - Information about Navit


Actions
~~~~~~~

The Actions menu brings up several sub menus that are focused primarily on routing and location finding. The sub menu items are:

1. **Bookmarks** - Store and access frequently used destinations
2. **Former destinations** - Access previously used destinations
3. **Map Point** - Select a point on the map
4. **Current Location** - Show current GPS position
5. **Town** - Search for a town
6. **Quit** - Closes Navit


Bookmarks
^^^^^^^^^

Bookmarks provide a convenient way to store often used destinations. Since Navit does not fully support entering a complete address using OpenStreetMap maps, a user can locate some oft-used destinations on the map and then add that point as a bookmark. That way the next time the user would like a route for that particular destination the user only has to select it from the Bookmarks menu and does not have to go through the tedium of panning the map and zooming into the destination location.

Bookmarks can be arranged hierarchical using / as a separator - anything before the separator becomes the folder name; anything after the separator becomes the bookmark name. For example, if you name your bookmarks Friends/Joe and Friends/Bill, you will have a folder named Friends and the bookmarks Bill and Joe in there.

A fully functioning bookmark editor is currently not available, though some common edits can be performed from within the Bookmarks menu. Bookmarks are stored in a plain-text bookmarks file in your Navit directory (~/.navit on unix systems).


Former destinations
^^^^^^^^^^^^^^^^^^^

The former destinations menu provides access to previously used destinations. This is a convenient way to quickly select a destination that has been used before without having to search for it again.


Map Point
^^^^^^^^^

The Map Point option allows you to select a point directly on the map. This is useful when you don't know the exact address or name of a location, but you know where it is on the map.


Current Location
^^^^^^^^^^^^^^^^

The Current Location option shows your current GPS position on the map. This is useful for verifying that your GPS is working correctly and that Navit is receiving position data.


Town
^^^^

The Town search option allows you to search for a town by name. This is the most common way to set a destination when you know the name of the town you want to navigate to.


Settings
~~~~~~~~

The Settings menu provides access to various configuration options. The sub menu items are:

1. **Display** - Configure display options
2. **Rules** - Configure navigation rules
3. **Vehicle** - Configure vehicle options
4. **Map** - Configure map options
5. **About** - Information about Navit


Tools
~~~~~

The Tools menu provides access to various utilities. The sub menu items may vary depending on your Navit configuration.


Route
~~~~~

The Route menu provides access to route management options. From here you can view and modify the current route, set waypoints, and access other routing-related features.


About
~~~~~

The About menu displays information about Navit, including the version number and credits.
