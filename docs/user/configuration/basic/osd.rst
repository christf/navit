.. _osd:

On Screen Display (OSD)
=======================

The On Screen Display (OSD) provides status information and controls blended directly onto the map. These can be implemented using the ``<osd ... />`` tag inside navit.xml.

You might try NavitConfigurator as a WYSIWYG testing environment for customizing your screen setup.


Examples
--------

To see example layouts for a variety of platforms and screen sizes, see :doc:`osd_layouts`.


Enable/Disable
--------------

An osd item can be enabled/disabled using the following:

.. code-block:: xml

   <osd enabled="yes" />
   <osd enabled="no"  />


Position
--------

The position of an element is specified in x and y **pixels** or **percent** of screen height/width from the top-left hand corner of the screen. Negative values will position the element with respect to the bottom-right hand corner of the screen (pixels only - this does not work with percent).

For example:

.. code-block:: xml

   <osd enabled="yes" x="0"   y="0"   />
   <osd enabled="yes" x="10"  y="10"  />
   <osd enabled="yes" x="-10" y="-10" />
   <osd enabled="yes" x="10"  y="-10" />
   <osd enabled="yes" x="10%" y="30%" />

- The first OSD item is placed in the top left hand corner of the screen.
- The second OSD item is placed 10 pixels right from the left-hand-side of the screen and 10 pixels down from the top of the screen (i.e. top left of the screen).
- The third OSD item is placed 10 pixels left from the right-hand-side of the screen and 10 pixels up from the bottom of the screen (i.e. bottom right of the screen).
- The fourth OSD item is placed 10 pixels right from the left-hand-side of the screen and 10 pixels up from the bottom of the screen (i.e. bottom left of the screen).
- The fifth OSD item is placed 10% of the screen width right from the left-hand-side of the screen and 30% of the screen height down from the top of the screen.


Size
----

The sizes of each item can be explicitly set in **pixels** or **percent** (of screen width/height) using the ``w`` and ``h`` attributes.

Example 1:

.. code-block:: xml

   <osd enabled="yes" x="0" y="0" w="100" h="50" />

This will create an item of width 100 and height 50 pixels from the top-left corner of the item.

Example 2:

.. code-block:: xml

   <osd enabled="yes" x="0" y="0" w="50%" h="10%" />

This will create an item of width 50% of the screen width and height 10% of the screen height from the top-left corner of the item.


Alignment
---------

Certain osd items may be aligned. For example, text may be aligned centrally within an item. Alignment is specified using:

.. code-block:: xml

   <osd enabled="yes" x="0" y="0" w="100" h="50" align="ALIGN_NUMBER"/>

Where the alignment number can be any of the following:

- "1": Align to the top
- "2": Align to the bottom
- "0" or "3": Align to the center (vertical)
- "4": Align to the left
- "8": Align to the right
- "0" or "12": Align to the center (horizontal)

To get a combination of alignment you have to sum vertical and horizontal alignment, so ``align="5"`` would give top left alignment.


Color
-----

Background Color
^^^^^^^^^^^^^^^^

The osd item's background color can be changed using the ``background_color`` attribute. For example:

.. code-block:: xml

   <osd enabled="yes" x="0" y="0" w="100" h="50" align="0" background_color="#000000c8" />

The color is specified in standard 6-figure hexadecimal, with the last two figures specifying amount of transparency/opacity (00 = fully transparent, FF = fully opaque). The above color is a translucent black.


Text Color
^^^^^^^^^^

The color of osd text items can be changed using the ``text_color`` attribute. For example:

.. code-block:: xml

   <osd enabled="yes" type="text" x="90"  y="0" w="110" h="45" align="4" font_size="400"  text_color="#ff0000"  label="${vehicle.position_speed}" />

The color is specified in standard 6-figure hexadecimal, red in the example above. Default seems to be white (#ffffff).


Icons
-----

When OSD types use icons, they are usually specified in one of the following two attributes:

- ``src``: Filename for a static image.
- ``icon_src``: Filename for a dynamic image. The string contains a ``%s`` placeholder, which will be replaced with an appropriate string (depending on the OSD type) at run time.

If these are specified without a path, Navit will look in its default image dir (platform-dependent) for a matching image. If you specify a path, Navit will look in that path.


OSD Types
---------

Text
^^^^

Display text on the screen. The text can contain variables that are replaced at runtime.

.. code-block:: xml

   <osd type="text" x="0" y="0" w="100" h="50" label="${vehicle.position_speed}" font_size="400" />

Navigation Next Turn
^^^^^^^^^^^^^^^^^^^^

Display the next turn instruction.

.. code-block:: xml

   <osd type="navigation_next_turn" x="0" y="0" w="80" h="80" />

Navigation Status
^^^^^^^^^^^^^^^^^

Display the current navigation status.

.. code-block:: xml

   <osd type="navigation_status" x="0" y="0" w="100" h="50" />

GPS Status
^^^^^^^^^^

Display the current GPS status.

.. code-block:: xml

   <osd type="gps_status" x="0" y="0" w="100" h="50" />

Button
^^^^^^

Display a clickable button.

.. code-block:: xml

   <osd type="button" x="0" y="0" w="100" h="50" command="gui.menu()" label="Menu" />


Navit Commands
--------------

OSD items can execute Navit commands when clicked. Common commands include:

- ``gui.menu()`` - Open the main menu
- ``gui.back()`` - Go back in the menu
- ``gui_quit()`` - Quit Navit
- ``set_center_cursor()`` - Center map on cursor
- ``follow=0`` - Disable map following
- ``follow=1`` - Enable map following
