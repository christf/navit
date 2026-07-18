.. _osd_layouts:

OSD Layouts
===========

This page is intended for users to display the OSD layouts that they have designed and provide a way to share those layouts to other users. Feel free to add your own completed layout to this page. As the number of layouts expands this page will be broken into several sub sections for each device.

Note: For an explanation of how to modify the OSD layouts reference :doc:`osd` section. Many of the layouts on this page were borrowed from the examples on that page.


Notes
-----

- If you would like instructions on how to modify OSD layouts you can refer to the :doc:`osd` page.
- If you would like to share your own layout please contact us on the Discord or IRC channel and let us know.


Tip
---

To make configuring Navit simpler it is recommended that you copy the navit.xml from "/usr/share/navit" to your home directory "/home/user/.navit" where "user" is the username you log into your computer with. Then to make changing OSD layouts, you can replace the OSD entries in navit.xml with an xi:include statement.

Then create a new file navitOSD.xml in which you place all the OSD items. This means you can create and share layouts by providing just the navitOSD.xml file and people can drop them into place without having to hand edit their navit.xml files. The same trick will work for any subset part of the navit.xml file. Remember to begin your file with ``<config>`` and end it with ``</config>``, otherwise Navit won't be able to parse it properly.


Layout scaler for different screen sizes
----------------------------------------

This is a small perl script scale.pl that makes use of imagemagick (convert) to quickly convert any OSD layout for a different screen resolution.

Known issues:

- svg images are displayed in fixed size by navit (might be fixed soon)
- some elements don't scale that nicely, you might want to edit the resulting xml for one or two fontsizes.
- no proper xml parsing, just regexp stuff

Features:

- converts xml and png in one go
- does not touch original files

Usage: Create scaled layout with:

.. code::

   ./scale.pl 50 ~/.navit/nibbler01/

then include the -scaled-XX xml file instead of the original.


Example Layouts
---------------

The following are example OSD layouts for various platforms:

Desktop Layout
~~~~~~~~~~~~~~

A basic layout for desktop use with common OSD elements.

.. code-block:: xml

   <osd type="text" x="0" y="0" w="100" h="50" label="${vehicle.position_speed}" font_size="400" />
   <osd type="navigation_next_turn" x="0" y="60" w="80" h="80" />
   <osd type="gps_status" x="0" y="150" w="100" h="50" />

Mobile Layout
~~~~~~~~~~~~~

A compact layout for mobile devices with touch screen.

.. code-block:: xml

   <osd type="text" x="-100" y="0" w="100" h="50" label="${vehicle.position_speed}" font_size="350" />
   <osd type="navigation_next_turn" x="0" y="-100" w="80" h="80" />
   <osd type="button" x="0" y="0" w="100" h="50" command="gui.menu()" label="Menu" />

Netbook Layout
~~~~~~~~~~~~~~

A layout optimized for netbook screens.

.. code-block:: xml

   <osd type="text" x="0" y="0" w="80" h="40" label="${vehicle.position_speed}" font_size="300" />
   <osd type="navigation_next_turn" x="0" y="50" w="60" h="60" />
   <osd type="button" x="-100" y="0" w="100" h="50" command="gui.menu()" label="Menu" />
