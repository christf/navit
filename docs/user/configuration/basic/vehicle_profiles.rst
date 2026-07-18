.. _vehicle_profiles:

Vehicle Profiles
================

Vehicle profiles define routing rules for Navit. This page contains example profiles for different use cases.


Bike Profiles
-------------

Bike Cycleway
^^^^^^^^^^^^^

A profile designed for cyclists who prefer cycleways.

.. code-block:: xml

   <vehicleprofile name="Bike prefered Cycleways" flags="0x80000000" flags_forward_mask="0x80000000" flags_reverse_mask="0x80000000" maxspeed_handling="1" route_mode="0" static_speed="5" static_distance="25">
       <roadprofile item_types="steps" speed="2">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_pedestrian,footway" speed="18">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="path,track_ground,hiking,track_grass,hiking_mountain" speed="12">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="track_gravelled,track_unpaved" speed="17">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="track_paved,street_service,street_parking_lane" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="cycleway" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_0,street_1_city,living_street" speed="20">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_2_city,street_1_land,street_2_land,street_3_city,street_4_city,ramp,street_3_land,street_4_land" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="roundabout" speed="20"/>
       <roadprofile item_types="ferry" speed="40"/>
   </vehicleprofile>


Bike on Asphalt
^^^^^^^^^^^^^^^

A profile for racing cyclists who prefer asphalt surfaces.

.. code-block:: xml

   <vehicleprofile name="Bike on Asphalt" flags="0x40000000" flags_forward_mask="0x40000000" flags_reverse_mask="0x40000000" maxspeed_handling="1" route_mode="0" static_speed="5" static_distance="25">
       <roadprofile item_types="steps" speed="2">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_pedestrian,footway" speed="5">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="path,track_ground,track_gravelled" speed="12">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="track_paved,cycleway,street_service,street_parking_lane" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_0,street_1_city,living_street" speed="20">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_2_city,street_1_land,street_2_land" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_3_city,street_4_city,ramp" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_3_land,street_4_land" speed="20">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2" distance_metric="1000"/>
       </roadprofile>
       <roadprofile item_types="roundabout" speed="20"/>
       <roadprofile item_types="ferry" speed="40"/>
   </vehicleprofile>


Car Profiles
------------

Car (no Highway)
^^^^^^^^^^^^^^^^

A profile for cars that avoids highways (useful for avoiding tolls).

.. code-block:: xml

   <vehicleprofile name="Car (no Highway)" flags="0x4000000" flags_forward_mask="0x4000002" flags_reverse_mask="0x4000001" maxspeed_handling="0" route_mode="0" static_speed="5" static_distance="25">
       <roadprofile item_types="street_0,street_1_city,living_street,street_service,track_gravelled,track_unpaved,street_parking_lane" speed="10">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_2_city,track_paved" speed="30">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_3_city" speed="40">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_4_city" speed="50">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="highway_city" speed="80">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2" distance_metric="1000"/>
       </roadprofile>
       <roadprofile item_types="street_1_land" speed="60">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2" distance_metric="1000"/>
       </roadprofile>
       <roadprofile item_types="street_2_land" speed="65">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2" distance_metric="1000"/>
       </roadprofile>
       <roadprofile item_types="street_3_land" speed="70">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2" distance_metric="1000"/>
       </roadprofile>
       <roadprofile item_types="street_4_land" speed="80">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2" distance_metric="1000"/>
       </roadprofile>
       <roadprofile item_types="street_n_lanes" speed="120">
           <announcement level="0"/>
           <announcement level="1" distance_metric="1000"/>
           <announcement level="2" distance_metric="2000"/>
       </roadprofile>
       <roadprofile item_types="ramp" speed="40">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="roundabout" speed="10"/>
       <roadprofile item_types="ferry" speed="40"/>
   </vehicleprofile>


Hike & Bike Profiles
---------------------

Hike & Bike hard
^^^^^^^^^^^^^^^^^

A profile for hikers and cyclists who use tracks, paths, and other small ways.

.. code-block:: xml

   <vehicleprofile name="Hike & Bike hard" flags="0x80000000" flags_forward_mask="0x80000000" flags_reverse_mask="0x80000000" maxspeed_handling="1" route_mode="0" static_speed="5" static_distance="25">
       <roadprofile item_types="steps" speed="2">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_pedestrian,footway" speed="5">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="path,track_ground,hiking,track_grass,hiking_mountain" speed="12">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="track_gravelled,track_unpaved" speed="17">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="track_paved,cycleway,street_service,street_parking_lane" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_0,street_1_city,living_street" speed="20">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_2_city,street_1_land,street_2_land" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_3_city" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_4_city,ramp" speed="22"/>
       <roadprofile item_types="street_3_land,street_4_land" speed="20"/>
       <roadprofile item_types="roundabout" speed="20"/>
       <roadprofile item_types="ferry" speed="40"/>
   </vehicleprofile>


Hike & Bike on Ground & Gravel
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A profile for hikers and cyclists on ground and gravel surfaces.

.. code-block:: xml

   <vehicleprofile name="Hike & Bike on Ground & Gravel" flags="0x40000000" flags_forward_mask="0x40000000" flags_reverse_mask="0x40000000" maxspeed_handling="1" route_mode="0" static_speed="5" static_distance="25">
       <roadprofile item_types="steps" speed="2">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_pedestrian,footway" speed="5">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="path,track_ground,hiking,track_grass,hiking_mountain" speed="12">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="track_gravelled,track_unpaved" speed="17">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="track_paved,cycleway,street_service,street_parking_lane" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_0,street_1_city,living_street" speed="20">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_2_city,street_1_land,street_2_land" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
       <roadprofile item_types="street_4_city,ramp,street_3_land,street_4_land,street_3_city" speed="22">
           <announcement level="0"/>
           <announcement level="1"/>
           <announcement level="2"/>
       </roadprofile>
   </vehicleprofile>
