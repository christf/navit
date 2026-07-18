.. _menu_overview:

Menu Overview
=============

The Menu of Navit has the following main views and settings for navigation and trips. This article highlights the main use cases and possible future application scenarios of an OpenSource navigation system for Offline usage especially with a Crowd Sourcing approach and Open Maps.


Navigation View
---------------

Seeing the Map and Current Location of GPS-Receiver. If a destination is set, the route is displayed in the map.


Reality View
------------

`Augmented Reality <https://github.com/navit-gps/navit/wiki/Augmented-Reality>`_ can be used to display routing support in a smartphone camera image. The Reality View Project is designed as a Navit extension.


Trips
-----

A trip consists of a start point and a destination point. For a trip the estimated time can be calculated. Trips are useful for trip planning prior to the navigation. Without having the notion of trips explicitly in Navit the calculation of the duration and the distance for a trip is currently implemented from the current GPS location to destination. This is shown in the Navigation view if the GUI element is defined in the config file of Navit.


Contacts/Bookmarks
~~~~~~~~~~~~~~~~~~

The bookmarks are used for start or destination point of routing.


Trip Bookmarks
~~~~~~~~~~~~~~

Trips cannot be saved like bookmarks currently in Navit. A trip consists of Start and Destination Point. Together with the vehicle setting a trip has a property of average duration and distance. The average duration could be modified by real traffic data of the user stored locally on the mobile device for a more accurate tailored estimation for Weekdays, Month and Start time of the trip.


Return Trip
~~~~~~~~~~~

A return trip button swaps Start Point and Destination Point of a trip. This is not implemented in current version of Navit, but it can be realized with bookmarks.


Settings of Navit
-----------------

Maps
~~~~

Different pre-defined maps can be loaded on the Mobile Device like on a SD-card on a smart phone for Offline usage. A list of loaded maps is currently not visible. Update button of maps e.g. from OpenStreetMap is not implemented (could increase the traffic on the map server).


2D/3D-Views
~~~~~~~~~~~

Views are the setting of the navigation view of Navit.


Vehicle Properties
~~~~~~~~~~~~~~~~~~

The properties of the vehicle determines the calculation of the duration of the trip. Furthermore the vehicle setting/pedestrian for reality view determines the camera perspective.
