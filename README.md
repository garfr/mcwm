# mcwm

This is a fork of mcwm a minimalist, floating window manager written using XCB, the X
protocol C binding.

## Compiling

### Prereqs
* libxcb1-dev, libxcb-atom1-dev, libxcb-keysyms1-dev, libxcb-icccm1-dev, libxcb-randr0-dev
* meson 
* ninja

### Steps 

``mkdir build``

``meson setup build``

``cd build/``

``ninja``
