.. _qml2_gui:

QML2 GUI
========

The QML2 UI is a new UI being currently developed to provide a more modern look and feel to Navit.


Screenshots
-----------

.. only:: html

   ===================== ====================================
   Tablet / PC           Mobile UI
   ===================== ====================================
   .. figure:: ../images/Qml2.gif
      :alt: Qml2.gif

      Qml2.gif

   .. figure:: ../images/Gui_qml_drawer_popup.gif
      :alt: Gui_qml_drawer_popup.gif

      Gui_qml_drawer_popup.gif
   ===================== ====================================


Prebuilt Image
--------------

A prebuilt image is available for Raspberry Pi 2/3 (preview):

- Download: https://cloud.kazer.org/index.php/s/QkSgrHoARq2BZC8
- Flash to card: ``dd if=rpi3-sdcard.img of=/dev/sdX``
- Log in as root (no password)
- Start Navit

Tweaks (default config file is in ``/usr/share/navit/navit.xml``):

- You might want to tweak the default zoom setting (in this image it's 32)
- You can disable the qt5_qml GUI and switch back to internal, it'll still use EGL


Prerequisites
-------------

The QML2 UI is currently developed against Qt 5.7.

The easiest way to install Qt 5.7 (or greater) is probably to use the Qt online installer:

- Linux: https://download.qt.io/archive/online_installers/2.0/
- Other platforms: https://download.qt.io/archive/online_installers/2.0/


Building
--------

If you have Qt5 installation in standard paths, simply ``cmake`` will create the Makefile and you can proceed with it.

When the Qt5 installation is in non-standard paths, you have to use ``CMAKE_INSTALL_PREFIX``:

.. code-block:: bash

   cmake -DCMAKE_INSTALL_PREFIX=$QT_INSTALL_PREFIX_PATH $NAVIT_SOURCE_PATH
   make
   sudo make install


Enabling the QML2 UI
---------------------

Once you compiled Navit, you can enable the QML2 UI from navit.xml:

1. Change your graphics driver to qt5:

   .. code-block:: xml

      <graphics type="qt5"/>

2. Enable the QML2 UI:

   .. code-block:: xml

      <gui type="qt5_qml" enabled="yes"/>

3. Disable the internal UI:

   .. code-block:: xml

      <gui type="internal" enabled="no"/>


Pages
-----

Homepage
^^^^^^^^

When you tap the screen, this page comes up. It shows:

- Header: "Back to map"
- Content: 4 common actions
- Footer: Current position with street name

Search
^^^^^^

Search for addresses and points of interest.

Bookmarks
^^^^^^^^^

Access your saved bookmarks.

POI Around Me
^^^^^^^^^^^^^

Find points of interest near your current location.

Settings
^^^^^^^^

Configure Navit settings including:

- Routing Profile
- Map
- Layout
- Personalize (Set Home, Bookmarks, OSD)

Drawer
^^^^^^

The drawer on the left side shows:

- POI Around Me
- Vehicle
- Settings

Popup
^^^^^

Popup dialogs for:

- About
- Team
