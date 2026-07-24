/* ============================================================================
 *  ble_compat_stubs.h — no-op BLE public API for the BLE-less Maix build.
 *
 *  Included (instead of ble_provision.h / ble_ancs.h / …) when BOARD_HAS_BLE==0.
 *  The heavy NimBLE clients (ANCS/AMS/Gadgetbridge/provisioning) are deferred to a
 *  later BlueZ port; meanwhile the UI (watchface BLE indicator, the WiFi/BLE
 *  settings app, Find-My-Phone) calls only this small public surface, which here
 *  reports "no BLE / not connected" so those screens render and behave inertly.
 *
 *  `static` matches the originals (the firmware is one translation unit).
 *  Signatures mirror the real declarations in ble_provision.h / ble_ancs.h.
 * ========================================================================== */
#pragma once
#include <cstdint>
#include <cstddef>

/* ---- BLE-UI data symbols the WiFi/BLE settings app reads directly --------- */
#define BLE_BOND_MAX 3   /* mirrors CONFIG_NIMBLE_MAX_BONDS on the ESP build */
enum BleToast { BLE_TOAST_NONE = 0, BLE_TOAST_PAIRED, BLE_TOAST_SAVED, BLE_TOAST_DUP,
                BLE_TOAST_FULL, BLE_TOAST_FORGETFAIL };
static volatile BleToast s_ble_toast = BLE_TOAST_NONE;  /* set by the app; never consumed here */
static bool              s_ble_up    = false;           /* stack never comes up on Maix */

/* ---- connection / status (ble_provision.h) ------------------------------- */
static inline bool ble_is_up(void)            { return false; }
static inline bool ble_phone_connected(void)  { return false; }
static inline bool ble_overlay_active(void)   { return false; }
static inline bool ble_take_dirty(void)       { return false; }
static inline bool ble_take_bond_dirty(void)  { return false; }
static inline bool ble_take_find_watch_req(void) { return false; }

/* ---- lifecycle ----------------------------------------------------------- */
static inline void ble_begin(void)         {}
static inline void ble_end(void)           {}
static inline void ble_apply_enabled(void) {}
static inline void ble_ui_tick(void)       {}

/* ---- find-my-phone / battery report -------------------------------------- */
static inline bool ble_ping_phone(void)        { return false; }
static inline bool ble_stop_phone_ring(void)   { return false; }
static inline void ble_report_battery(int /*pct*/, bool /*charging*/) {}

/* ---- bonds (Settings > WiFi/BLE paired-device list) ---------------------- */
static inline int  ble_bond_count(void)                       { return 0; }
static inline bool ble_bond_label(int, char *buf, size_t cap) { if (buf && cap) buf[0] = 0; return false; }
static inline bool ble_bond_forget(int)                       { return false; }

/* ---- ANCS (iPhone notifications) public hooks (ble_ancs.h) --------------- */
static inline bool ancs_take_removed(void)  { return false; }
static inline bool ancs_take_ui_dirty(void) { return false; }
static inline void ancs_dismiss_all(void)   {}
static inline void ancs_dismiss_id(uint64_t /*id*/) {}
static inline bool ancs_background_check(int /*timeout_ms*/) { return false; }  // deep-sleep fetch path
