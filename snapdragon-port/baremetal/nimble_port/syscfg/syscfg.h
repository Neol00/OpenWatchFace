/* syscfg.h — NimBLE host configuration for the snapdragon-port (msm8909w / WCN3620).
 * Host only; the controller is the Bluetooth core inside WCNSS, reached over
 * HCI on SMD (nimble_port/nimble_transport_smd.c). Values set here win; the
 * rest come from the upstream "linux" example defaults copied alongside. */
#pragma once

/* transport: our own LL glue, host native */
#define MYNEWT_VAL_BLE_TRANSPORT_LL__socket (0)
#define MYNEWT_VAL_BLE_TRANSPORT_LL__custom (1)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_COUNT (16)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_DISCARDABLE_COUNT (16)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_SIZE (257)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_HS_COUNT (12)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_LL_COUNT (12)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_SIZE (255)
#define MYNEWT_VAL_BLE_MONITOR_UART (0)
#define MYNEWT_VAL_BLE_MONITOR_RTT (0)

/* roles: peripheral + central (ANCS/AMS client), no ext adv / iso / eatt */
#define MYNEWT_VAL_BLE_ROLE_BROADCASTER (1)
#define MYNEWT_VAL_BLE_ROLE_CENTRAL (1)
#define MYNEWT_VAL_BLE_ROLE_OBSERVER (1)
#define MYNEWT_VAL_BLE_ROLE_PERIPHERAL (1)
#define MYNEWT_VAL_BLE_MAX_CONNECTIONS (2)
#define MYNEWT_VAL_BLE_MAX_PERIODIC_SYNCS (0)
#define MYNEWT_VAL_BLE_EXT_ADV (0)
#define MYNEWT_VAL_BLE_PERIODIC_ADV (0)
#define MYNEWT_VAL_BLE_ISO (0)
#define MYNEWT_VAL_BLE_EATT_CHAN_NUM (0)
#define MYNEWT_VAL_BLE_L2CAP_COC_MAX_NUM (0)
#define MYNEWT_VAL_BLE_HCI_VS (0)
#define MYNEWT_VAL_BLE_VERSION (50)
#define MYNEWT_VAL_BLE_PHY_2M (0)
#define MYNEWT_VAL_BLE_POWER_CONTROL (0)

/* security: LE Secure Connections + legacy, MITM via display-only passkey, bonding */
#define MYNEWT_VAL_BLE_SM_LEGACY (1)
#define MYNEWT_VAL_BLE_SM_SC (1)
#define MYNEWT_VAL_BLE_SM_BONDING (1)
#define MYNEWT_VAL_BLE_SM_MITM (1)
#define MYNEWT_VAL_BLE_SM_IO_CAP (BLE_HS_IO_DISPLAY_ONLY)
#define MYNEWT_VAL_BLE_SM_OUR_KEY_DIST (3)
#define MYNEWT_VAL_BLE_SM_THEIR_KEY_DIST (3)
#define MYNEWT_VAL_BLE_SM_MAX_PROCS (1)
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS (3)
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS (16)
#define MYNEWT_VAL_BLE_STORE_CONFIG_PERSIST (0)   /* our own persistence in nimble_glue.c */
#define MYNEWT_VAL_TRNG (0)
#define MYNEWT_VAL_SELFTEST (0)

/* host */
#define MYNEWT_VAL_BLE_HS_AUTO_START (0)         /* BLEDevice::init starts it after the transport is up */
#define MYNEWT_VAL_BLE_HS_LOG_LVL (1)
#define MYNEWT_VAL_BLE_HS_DEBUG (0)
#define MYNEWT_VAL_BLE_HS_FLOW_CTRL (0)
#define MYNEWT_VAL_BLE_HS_STOP_ON_SHUTDOWN (0)
#define MYNEWT_VAL_BLE_ATT_PREFERRED_MTU (247)
#define MYNEWT_VAL_BLE_GATT_MAX_PROCS (4)
#define MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME "WatchFace"
#define MYNEWT_VAL_BLE_SVC_GAP_APPEARANCE (0x00C2)   /* generic watch */
#define MYNEWT_VAL_MSYS_1_BLOCK_COUNT (32)
#define MYNEWT_VAL_MSYS_1_BLOCK_SIZE (292)
#define MYNEWT_VAL_OS_TICKS_PER_SEC (1000)         /* FreeRTOS tick = 1 ms on this port */
#define MYNEWT_VAL_OS_CPUTIME_FREQ (1000000)

#include "syscfg_linux_defaults.h"
