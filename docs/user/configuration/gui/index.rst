.. _gui_index:

GUI Engines
===========

Navit's GUI controls user interaction and map display. Choose a GUI in
the ``gui`` tag of ``navit.xml``; not every GUI works with every graphics
driver. See :doc:`/user/configuration/basic/display` for the generic
GUI and graphics-driver options.

Available GUI modules include:

* :doc:`/user/configuration/Internal_GUI` - the default, embedded GUI.
* :doc:`qt5_qml` - the Qt 5 based touch interface.
* :doc:`qml` - a legacy Qt 4 based interface.

The old SDL GUI has been removed; use the Internal GUI instead. The
former QML2 design notes were unpublished work in progress and are no
longer maintained.

.. toctree::
   :maxdepth: 1

   qt5_qml
   qml
