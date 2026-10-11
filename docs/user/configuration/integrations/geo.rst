Geo links
=========

Navit can be used as the handler for ``geo:`` links, so that clicking a
location link in a mail client or web browser opens Navit centered on
that position.

How it works
------------

Navit's desktop entry declares the MIME type ``x-scheme-handler/geo`` and
passes the link to the ``-p`` command line option::

    Exec=navit -p %u

A link such as ``geo:52.8153,11.3807`` therefore starts Navit as if it
had been invoked as ``navit -p 52.8153,11.3807``, which centers the map
on the position and marks it with a circle. When a ``geo:`` link carries
no coordinates, the argument ``-p`` has no value and is ignored so that
Navit starts normally.

Setup
-----

After installing Navit, update the desktop entry database so that other
programs offer Navit as a handler for ``geo:`` links:

.. code:: bash

    update-desktop-database /usr/local/share/applications

Run this command for every directory the desktop entry was installed to
(the installation prefix configured during build, e.g.
``CMAKE_INSTALL_PREFIX=/usr/local`` maps to
``/usr/local/share/applications``).
For a system-wide installation it may require administrator privileges.

To make Navit the default handler for ``geo:`` links:

.. code:: bash

    xdg-mime default navit.desktop x-scheme-handler/geo

To check which applications are registered for ``geo:`` links:

.. code:: bash

    gio mime x-scheme-handler/geo

.. note::

   A desktop entry in ``~/.local/share/applications`` takes precedence
   over the system-wide one. Remove or update it if it shadows the
   installed entry.
