############### Add include ###################
# Order matters: compat/ FIRST so the firmware's `#include <Arduino.h>`, <Wire.h>,
# <Preferences.h>, <esp_sleep.h>, … resolve to our shims rather than a real Arduino
# core. Then the firmware source dir, then MaixCDK's lvgl conf (its 32-bit lv_conf.h)
# so the firmware's `#include "lv_conf.h"` is the MaixCDK one, NOT the ESP libraries/ one.
list(APPEND ADD_INCLUDE "include"
                        "${CMAKE_CURRENT_SOURCE_DIR}/../compat"
                        "${CMAKE_CURRENT_SOURCE_DIR}/../../../OpenWatchFace"
                        "${CMAKE_CURRENT_SOURCE_DIR}/../../MaixCDK/components/3rd_party/lvgl/conf"
    )
list(APPEND ADD_PRIVATE_INCLUDE "")
###############################################

############ Add source files #################
append_srcs_dir(ADD_SRCS "src")     # main.cpp + firmware_tu.cpp (which #includes the .ino)
list(APPEND ADD_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../compat/arduino_shim.cpp"
                     "${CMAKE_CURRENT_SOURCE_DIR}/../compat/arduino_time_maix.cpp"
                     "${CMAKE_CURRENT_SOURCE_DIR}/../compat/maix_wifi_bridge.cpp"
                     "${CMAKE_CURRENT_SOURCE_DIR}/../compat/maix_sys_bridge.cpp")
# LVGL font/icon C arrays — separate translation units the firmware references via
# LV_FONT_DECLARE (extern). Compiled as C (C linkage matches the firmware's extern "C" decls).
set(OWF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../OpenWatchFace")
list(APPEND ADD_SRCS "${OWF_DIR}/icons14.c"
                     "${OWF_DIR}/icons22.c"
                     "${OWF_DIR}/icons28.c"
                     "${OWF_DIR}/icons34.c"
                     "${OWF_DIR}/montserrat_clock_110.c")
###############################################

###### Add required/dependent components ######
list(APPEND ADD_REQUIREMENTS basic vision lvgl peripheral ext_dev network)
###############################################

#### Compile options / definitions ############
# Select the MaixCam-Pro (Linux) board target in the firmware's board.h.
# BOARD_ID_MAIX == 3 (see OpenWatchFace/board.h).
list(APPEND ADD_DEFINITIONS -DBOARD_SELECT=3)
###############################################

register_component()
