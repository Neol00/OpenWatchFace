/* firmware_tu.cpp — compiles the OpenWatchFace firmware as one C++ translation
 * unit. MaixCDK's build only compiles known source extensions (.c/.cpp/.cc), not
 * Arduino's .ino, so we pull the sketch in through a .cpp wrapper. The firmware
 * dir is on the include path (see main/CMakeLists.txt), and the sketch already
 * #includes all its own .h modules (it's a unity build). Provides setup()/loop(),
 * which main.cpp calls. */
#include "OpenWatchFace.ino"
