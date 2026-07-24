/**
 * OpenWatchFace on MaixCam-Pro — entry point.
 *
 * Brings up the MaixCDK display + touchscreen, hands them to the bundled LVGL glue
 * (maix::lvgl_init, which wires flush/touch/tick), then runs the firmware: setup()
 * once, loop() forever. setup()/loop() live in OpenWatchFace.ino (compiled as a
 * separate C++ TU with the BOARD_ID_MAIX board); the .ino's Maix path adopts the
 * LVGL display we create here instead of bringing up an Arduino_GFX panel.
 *
 * This file deliberately does NOT include the firmware headers — it only needs the
 * two extern entry points — so the firmware stays a single translation unit.
 */
#include "maix_basic.hpp"
#include "maix_display.hpp"
#include "maix_touchscreen.hpp"
#include "maix_key.hpp"
#include "maix_lvgl.hpp"
#include "owf_maix_hooks.h"

using namespace maix;

/* Defined in OpenWatchFace.ino (Arduino sketch entry points). */
extern void setup();
extern void loop();

/* Map the MaixCam-Pro User button onto the firmware's BOOT button. The board has a
 * single user key, so ANY key event drives the BOOT level: pressed/long → LOW,
 * released → HIGH. The firmware's existing single-tap-menu / double-tap-sleep logic
 * (which polls digitalRead(BOOT_BTN_GPIO)) then works unchanged. */
static void pump_user_button(peripheral::key::Key &btn)
{
    int k, v;
    while (btn.read(k, v) == err::ERR_NONE && k != 0) {
        if (v == peripheral::key::State::KEY_RELEASED) g_owf_boot_level = 1;  // HIGH
        else                                           g_owf_boot_level = 0;  // LOW (pressed / long)
    }
}

int _main(int argc, char *argv[])
{
    display::Display screen = display::Display();
    log::info("OpenWatchFace: display %dx%d", screen.width(), screen.height());

    touchscreen::TouchScreen ts = touchscreen::TouchScreen();

    /* Physical User button. May have no input device on a desktop build — tolerate
     * that so the SDL build still runs (button just inactive there). */
    peripheral::key::Key *button = nullptr;
    try {
        button = new peripheral::key::Key();
    } catch (...) {
        log::warn("no user-button input device; BOOT button disabled");
    }

    lvgl_init(&screen, &ts);    // lv_init + display(monitor_flush) + indev + tick

    setup();                    // firmware boot: builds the watchface UI (Maix path).
                                // setup() kicks off WiFi auto-connect using WIFI_SSID/WIFI_PASS.

    while (!app::need_exit())
    {
        if (button) pump_user_button(*button);   // User button -> firmware BOOT button level
        loop();                                  // firmware tick: services lv_task_handler, self-paces
        if (!screen.is_opened())
            break;
    }
    delete button;

    lvgl_destroy();
    return 0;
}

int main(int argc, char *argv[])
{
    sys::register_default_signal_handle();
    CATCH_EXCEPTION_RUN_RETURN(_main, -1, argc, argv);
}
