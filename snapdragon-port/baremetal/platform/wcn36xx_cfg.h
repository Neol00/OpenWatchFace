/* wcn36xx_cfg.h -- GENERATED from mainline v6.6 wcn36xx/smd.c wcn36xx_cfg_vals[]
 * + hal.h WCN36XX_HAL_CFG_* ids. The config TLVs HAL_START_REQ carries. */
#pragma once
#include <stdint.h>
static const struct { uint16_t id; uint32_t val; } k_wcn36xx_cfg[] = {
    {   1,      1 },  /* CURRENT_TX_ANTENNA */
    {   2,      1 },  /* CURRENT_RX_ANTENNA */
    {   3,      0 },  /* LOW_GAIN_OVERRIDE */
    {   4,    785 },  /* POWER_STATE_PER_CHAIN */
    {   5,      5 },  /* CAL_PERIOD */
    {   6,      1 },  /* CAL_CONTROL */
    {   7,      0 },  /* PROXIMITY */
    {   8,      3 },  /* NETWORK_DENSITY */
    {   9,   6000 },  /* MAX_MEDIUM_TIME */
    {  10,     64 },  /* MAX_MPDUS_IN_AMPDU */
    {  11,   2347 },  /* RTS_THRESHOLD */
    {  12,     15 },  /* SHORT_RETRY_LIMIT */
    {  13,     15 },  /* LONG_RETRY_LIMIT */
    {  14,   8000 },  /* FRAGMENTATION_THRESHOLD */
    {  15,      5 },  /* DYNAMIC_THRESHOLD_ZERO */
    {  16,     10 },  /* DYNAMIC_THRESHOLD_ONE */
    {  17,     15 },  /* DYNAMIC_THRESHOLD_TWO */
    {  18,      0 },  /* FIXED_RATE */
    {  19,      4 },  /* RETRYRATE_POLICY */
    {  20,      0 },  /* RETRYRATE_SECONDARY */
    {  21,      0 },  /* RETRYRATE_TERTIARY */
    {  22,      5 },  /* FORCE_POLICY_PROTECTION */
    {  23,      1 },  /* FIXED_RATE_MULTICAST_24GHZ */
    {  24,      5 },  /* FIXED_RATE_MULTICAST_5GHZ */
    {  26,      5 },  /* DEFAULT_RATE_INDEX_5GHZ */
    {  27,     40 },  /* MAX_BA_SESSIONS */
    {  28,    200 },  /* PS_DATA_INACTIVITY_TIMEOUT */
    {  29,      1 },  /* PS_ENABLE_BCN_FILTER */
    {  30,      1 },  /* PS_ENABLE_RSSI_MONITOR */
    {  31,     20 },  /* NUM_BEACON_PER_RSSI_AVERAGE */
    {  32,     10 },  /* STATS_PERIOD */
    {  33,  30000 },  /* CFP_MAX_DURATION */
    {  34,      0 },  /* FRAME_TRANS_ENABLED */
    {  40,    128 },  /* BA_THRESHOLD_HIGH */
    {  41,   2560 },  /* MAX_BA_BUFFERS */
    {  57,      0 },  /* DYNAMIC_PS_POLL_VALUE */
    {  64,      1 },  /* TX_PWR_CTRL_ENABLE */
    {  75,      1 },  /* ENABLE_CLOSE_LOOP */
    {  99,      0 },  /* ENABLE_LPWR_IMG_TRANSITION */
    {  87, 120000 },  /* BTC_STATIC_LEN_LE_BT */
    {  91,  30000 },  /* BTC_STATIC_LEN_LE_WLAN */
    {  98,     10 },  /* MAX_ASSOC_LIMIT */
    { 100,      0 },  /* ENABLE_MCC_ADAPTIVE_SCHEDULER */
    { 210,    133 },  /* ENABLE_DYNAMIC_RA_START_RATE */
    { 215,   1000 },  /* LINK_FAIL_TX_CNT */
};
