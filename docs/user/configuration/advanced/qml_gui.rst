.. _qml_gui:

QML GUI
=======

The QML GUI is designed to be a modern and flexible replacement of the internal GUI. It is based on `Qt's modelling language UI framework <http://en.wikipedia.org/wiki/QML>`_, therefore it can run on any Qt platform, including mobile platforms with touchscreens.

.. warning::

   The QML GUI is currently unmaintained and buggy. Feel free to test it, but be prepared to fix issues.


Building
--------

Qt Libraries
^^^^^^^^^^^^

Navit requires Qt 4.7 or 4.8. You can either install Qt from your distribution's package manager or build from source.

For Debian/Ubuntu:

.. code-block:: bash

   apt-get install qt4-default libqt4-dev

For building from source, download from http://download.qt.io/archive/qt/ and follow the Qt build instructions.


Build Navit
^^^^^^^^^^^

Once Qt is installed, build Navit with CMake. The build system should automatically detect Qt and enable Qt support:

.. code-block:: bash

   mkdir build && cd build
   cmake ..
   make
   sudo make install

Look for these lines in the CMake output:

::

   Enabled   qt_qpainter ( Qt libraries found )
   Enabled   qml ( Qt Declarative found )

Both ``qt_qpainter`` and ``qml`` are required for the QML GUI.


Configuration
-------------

navit.xml
^^^^^^^^^

Enable the QML GUI in your ``navit.xml``:

.. code-block:: xml

   <graphics type="qt_qpainter"/>
   <gui type="qml" enabled="yes"/>

All other GUI modules must be disabled by setting ``enabled="no"`` in their configuration stanzas.


QML GUI Parameters
^^^^^^^^^^^^^^^^^^

The QML GUI supports the following parameters:

fullscreen
""""""""""

- **"0"** - Start Navit in windowed mode (default)
- "1" - Start Navit in fullscreen mode

.. code-block:: xml

   <gui type="qml" enabled="yes" fullscreen="1"/>

menu_on_map_click
"""""""""""""""""

- "0" - Menu GUI is switched by a command (not yet implemented)
- **"1"** - Single click on map will bring up the menu GUI (default)

.. code-block:: xml

   <gui type="qml" enabled="yes" menu_on_map_click="1"/>

signal_on_map_click
"""""""""""""""""""

- **"0"** - When single clicking on the map, processing to be controlled by menu_on_map_click (default)
- "1" - When single clicking on the map, send a DBus signal

radius
""""""

This takes a number, which is the distance in kilometres which POI search will restrict to.

- **"10"** - Default POI search radius, in kilometres (default)

.. code-block:: xml

   <gui type="qml" enabled="yes" radius="5"/>

skin
""""

- **"skin/stadiamap"** - Stadia Maps skin (default)

.. code-block:: xml

   <gui type="qml" enabled="yes" skin="skin/stadiamap"/>


Troubleshooting
---------------

- If the QML GUI doesn't start, check that both ``qt_qpainter`` and ``qml`` are enabled in the CMake output
- Make sure all other GUI modules are disabled
- Check that Qt libraries are properly installed and accessible
