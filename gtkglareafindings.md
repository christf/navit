* smooth scrolling in 3d mode does not work because there is no compensation
  for the "camera tilting"
* when multiple vehicles are shown on the map, the map scrolls with the primary
  active vehicle, this is good, BUT the other vehicle(s) have different trajectory / speeds / destinations and their positions therefore is entirely wrong. For non-primary vehicles, their cursor must move on the map, while the map should scroll underneath the primary vehicle just as it is
* concave shapes look weird. When we have a moon-shaped object (close to
  new-moon), then the polygon closes by connecting the tips such that a half-circle is created, not a sichel.
* even in smooth scrolling, there are sudden jumps in the map. What is their
  root cause?
