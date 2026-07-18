.. _menu_configurations:

Menu Configurations
===================

The menu used in the Internal GUI is defined by a html-like syntax inside the gui tags within navit.xml. As a result, the menu offers a variety of configuration options to better suit the user.


Installing Alternative Configurations
-------------------------------------

If you want to try one of the configurations shown below, simply copy the code for the configuration and paste over the top of the default configuration within the gui tags in ``navit.xml``. Make sure that all traces of a previous configuration are wiped out, as only one configuration can be present in ``navit.xml`` at one time.


Default Configuration
---------------------

Below is the default menu configuration found in a freshly-installed version of navit.xml.

.. code-block:: xml

   <![CDATA[
   <html>
       <a name='Main Menu'><text>Main menu</text>
            <img cond='button' src='gui_map'><script>position(click_coord_geo,_("Map Point"),8|16|32|64|256|1024)</script></img>
            <a href='#Actions'><img src='gui_actions'>Actions</img></a>
           <img cond='flags&amp;2' src='gui_map' onclick='back_to_map()'><text>Show Map</text></img>
           <a href='#Settings'><img src='gui_settings'><text>Settings</text></img></a>
           <a href='#Tools'><img src='gui_tools'><text>Tools</text></img></a>
           <a href='#Route'><img src='gui_settings'><text>Route</text></img></a>
                   <img src='gui_about'  onclick='about()'><text>About</text></img>
       </a>

       <a name='Actions'><text>Actions</text>
           <img src='gui_bookmark' onclick='bookmarks()'><text>Bookmarks</text></img>
           <img cond='click_coord_geo' src='gui_map' onclick='position(click_coord_geo,_("Map Point"),8|16|32|64|256)'>
                   <script>write(click_coord_geo)</script></img>
           <img cond='position_coord_geo' src='gui_vehicle' onclick='position(position_coord_geo,_("Vehicle Position"),8|32|64|128|256)'>
                   <script>write(position_coord_geo)</script></img>
           <img src='gui_town' onclick='town()'><text>Town</text></img>
           <img src='gui_quit' onclick='quit()'><text>Quit</text></img>
           <img cond='navit.route.route_status&amp;52' src='gui_stop' onclick='abort_navigation()'><text>Stop Navigation</text></img>
       </a>

       <a name='Settings'><text>Settings</text>
           <a href='#Settings Display'><img src='gui_display'><text>Display</text></img></a>
           <img src='gui_maps' onclick='setting_maps()'><text>Maps</text></img>
           <img src='gui_vehicle' onclick='setting_vehicle()'><text>Vehicle</text></img>
           <img src='gui_rules' onclick='setting_rules()'><text>Rules</text></img>
       </a>

       <a name='Settings Display'><text>Display</text>
           <img src='gui_display' onclick='setting_layout()'><text>Layout</text></img>
           <img cond='fullscreen==0' src='gui_fullscreen' onclick='fullscreen=1'><text>Fullscreen</text></img>
           <img cond='fullscreen==1' src='gui_leave_fullscreen' onclick='fullscreen=0'><text>Window Mode</text></img>
           <img cond='navit.pitch==0' src='gui_map' onclick='navit.pitch=pitch;redraw_map();back_to_map()'><text>3D</text></img>
           <img cond='navit.pitch!=0' src='gui_map' onclick='navit.pitch=0;redraw_map();back_to_map()'><text>2D</text></img>
       </a>

       <a name='Tools'><text>Tools</text>
           <img src='gui_actions' onclick='locale()'><text>Show Locale</text></img>
       </a>

       <a name='Route'><text>Route</text>
           <img src='gui_actions' onclick='route_description()'><text>Description</text></img>
           <img src='gui_actions' onclick='route_height_profile()'><text>Height Profile</text></img>
       </a>

   </html>
   ]]>


Menu Structure
--------------

The default menu structure consists of:

- **Main Menu** - The root menu with access to all sub-menus
- **Actions** - Bookmarks, map point, vehicle position, town search, quit
- **Settings** - Display, maps, vehicle, rules
- **Display** - Layout, fullscreen, 3D/2D mode
- **Tools** - Show locale
- **Route** - Description, height profile


Customizing the Menu
--------------------

You can customize the menu by modifying the html-like syntax in the gui tags. The menu supports:

- **Conditional display** using ``cond`` attribute
- **Scripts** using ``script`` tags
- **Images** using ``img`` tags
- **Links** using ``a`` tags
- **Text** using ``text`` tags


Common Customizations
---------------------

Adding a Bookmark Menu Item
^^^^^^^^^^^^^^^^^^^^^^^^^^^

To add a bookmark menu item to the main menu:

.. code-block:: xml

   <img src='gui_bookmark' onclick='bookmarks()'><text>Bookmarks</text></img>

Adding a Navigation Stop Button
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

To add a stop navigation button:

.. code-block:: xml

   <img cond='navit.route.route_status&amp;52' src='gui_stop' onclick='abort_navigation()'><text>Stop Navigation</text></img>

Adding a Fullscreen Toggle
^^^^^^^^^^^^^^^^^^^^^^^^^^

To add a fullscreen toggle button:

.. code-block:: xml

   <img cond='fullscreen==0' src='gui_fullscreen' onclick='fullscreen=1'><text>Fullscreen</text></img>
   <img cond='fullscreen==1' src='gui_leave_fullscreen' onclick='fullscreen=0'><text>Window Mode</text></img>
