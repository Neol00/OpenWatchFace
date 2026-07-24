#pragma once

#include "Arduino_DataBus.h"

#if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)
#include <driver/spi_master.h>
#if (ESP_ARDUINO_VERSION_MAJOR >= 3)
#include <esp_memory_utils.h>
#endif

#ifndef ESP32SPIDMA_MAX_PIXELS_AT_ONCE
#define ESP32SPIDMA_MAX_PIXELS_AT_ONCE 1024
#endif
#ifndef ESP32SPIDMA_DMA_CHANNEL
#define ESP32SPIDMA_DMA_CHANNEL SPI_DMA_CH_AUTO
#endif

class Arduino_ESP32SPIDMA : public Arduino_DataBus
{
public:
#if CONFIG_IDF_TARGET_ESP32
  Arduino_ESP32SPIDMA(int8_t dc = GFX_NOT_DEFINED, int8_t cs = GFX_NOT_DEFINED, int8_t sck = GFX_NOT_DEFINED, int8_t mosi = GFX_NOT_DEFINED, int8_t miso = GFX_NOT_DEFINED, uint8_t spi_num = VSPI, bool is_shared_interface = false); // Constructor
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
  Arduino_ESP32SPIDMA(int8_t dc = GFX_NOT_DEFINED, int8_t cs = GFX_NOT_DEFINED, int8_t sck = GFX_NOT_DEFINED, int8_t mosi = GFX_NOT_DEFINED, int8_t miso = GFX_NOT_DEFINED, uint8_t spi_num = HSPI, bool is_shared_interface = false); // Constructor
#else
  Arduino_ESP32SPIDMA(int8_t dc = GFX_NOT_DEFINED, int8_t cs = GFX_NOT_DEFINED, int8_t sck = GFX_NOT_DEFINED, int8_t mosi = GFX_NOT_DEFINED, int8_t miso = GFX_NOT_DEFINED, uint8_t spi_num = FSPI, bool is_shared_interface = false); // Constructor
#endif

  bool begin(int32_t speed = GFX_NOT_DEFINED, int8_t dataMode = SPI_MODE0) override;
  void beginWrite() override;
  void endWrite() override;
  void writeCommand(uint8_t) override;
  void writeCommand16(uint16_t) override;
  void writeCommandBytes(uint8_t *data, uint32_t len) override;
  void write(uint8_t) override;
  void write16(uint16_t) override;

  void writeC8D8(uint8_t c, uint8_t d) override;
  void writeC8D16(uint8_t c, uint16_t d) override;
  void writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2) override;

  void writeRepeat(uint16_t p, uint32_t len) override;
  void writePixels(uint16_t *data, uint32_t len) override;

  void writeBytes(uint8_t *data, uint32_t len) override;

  void writeIndexedPixels(uint8_t *data, uint16_t *idx, uint32_t len) override;
  void writeIndexedPixelsDouble(uint8_t *data, uint16_t *idx, uint32_t len) override;
  void writeYCbCrPixels(uint8_t *yData, uint8_t *cbData, uint8_t *crData, uint16_t w, uint16_t h) override;

  // LOCAL ADDITION (OpenWatchFace C6-1.47): ASYNC tile write for the JD9853 flush.
  // Queues the pixel data as <=ASYNC_MAX_SEG DMA transactions and returns as soon as
  // they're queued (~us) — the CPU is then free to let LVGL render the next tile while
  // the wire drains. `data` MUST already be in panel byte order (the LVGL flush swaps
  // it) and live in DMA-capable RAM. CS is taken LOW here and raised from the post_cb
  // ISR, which then calls done_cb(done_arg) (i.e. lv_disp_flush_ready). The caller must
  // have set the address window (sync) immediately before. DC stays HIGH (data).
  // Returns false if the area is too big for ASYNC_MAX_SEG segments -> caller falls
  // back to the blocking draw path. NOTE: requires the device NOT hold the bus via
  // spi_device_acquire_bus (begin() drops the permanent acquire), because the queued
  // API is incompatible with a held bus.
  bool writePixelsAsync(const uint8_t *data, uint32_t len, void (*done_cb)(void *), void *done_arg);
  void waitAsync(); // block until no async write is in flight (reaps queued results)

  // LOCAL ADDITION (OpenWatchFace C6-1.47): HYBRID async/sync mode. The microSD shares this
  // SPI host; the async path (queued DMA + raw-GPIO CS) corrupts the panel when the SD device
  // coexists on the bus. While SD I/O is in progress the SD layer sets sync_only(true): then
  // writePixelsAsync() returns false so the LVGL flush takes its BLOCKING path (one full
  // synchronous transaction, no queued DMA in flight), keeping the bus single-owner. Cleared
  // (false) when SD is idle to restore the async FPS win. Caller drains in-flight DMA before
  // setting true. */
  void set_sync_only(bool on) { _sync_only = on; }
  bool sync_only(void) const { return _sync_only; }

  // LOCAL ADDITION (OpenWatchFace C6-1.47): re-latch this device's bus config after ANOTHER
  // device (the microSD) used the shared host. The IDF spi_master reconfigures the bus
  // registers (clock divider 20->80 MHz, timing) at the START of the first transaction after
  // a device switch — but this display's CS is RAW GPIO, pulled LOW in beginWrite() BEFORE
  // that first transaction, so a reconfig glitch on SCLK is clocked into the panel and shifts
  // its command stream (=> corrupted CASET/RASET => sliced/garbled lines). This sends ONE
  // dummy byte on this device while every CS on the bus is HIGH (panel and SD both ignore it),
  // so the reconfig happens during a transaction nobody hears; the next real CS bracket then
  // starts with the bus already configured for the display. Call after each SD bus release.
  void resyncBus(void);

  // LOCAL ADDITION (OpenWatchFace C6-1.47): force the panel's raw-GPIO CS HIGH. Belt-and-
  // braces before SD bus use: if ANY code path ever left the panel CS low, every SD byte
  // would also be clocked into the panel (ESP-IDF sdspi_share rule #1: all other devices'
  // CS must be deasserted during SD traffic). Idempotent, ~ns cost.
  void forceCsIdle(void);

protected:
  void flush_data_buf();
  GFX_INLINE void WRITE8BIT(uint8_t d);
  GFX_INLINE void WRITE9BIT(uint32_t d);
  GFX_INLINE void DC_HIGH(void);
  GFX_INLINE void DC_LOW(void);
  GFX_INLINE void CS_HIGH(void);
  GFX_INLINE void CS_LOW(void);
  GFX_INLINE void POLL_START();
  GFX_INLINE void POLL_END();

private:
  int8_t _dc, _cs;
  int8_t _sck, _mosi, _miso;
  uint8_t _spi_num;
  bool _is_shared_interface;
  uint32_t _div = 0;

  PORTreg_t _dcPortSet; ///< PORT register for data/command SET
  PORTreg_t _dcPortClr; ///< PORT register for data/command CLEAR
  PORTreg_t _csPortSet; ///< PORT register for chip select SET
  PORTreg_t _csPortClr; ///< PORT register for chip select CLEAR
  uint32_t _dcPinMask;  ///< Bitmask for data/command
  uint32_t _csPinMask;  ///< Bitmask for chip select

  spi_device_handle_t _handle;
  spi_transaction_t _spi_tran;
  uint8_t _bitOrder = SPI_MSBFIRST;

  union
  {
    uint8_t *_buffer;
    uint16_t *_buffer16;
    uint32_t *_buffer32;
  };

  union
  {
    uint8_t *_2nd_buffer;
    uint16_t *_2nd_buffer16;
    uint32_t *_2nd_buffer32;
  };

  uint16_t _data_buf_bit_idx = 0;

  // LOCAL ADDITION (OpenWatchFace C6-1.47): async tile write state. Up to
  // ASYNC_MAX_SEG queued DMA segments sharing ONE CS bracket; pre_cb drops CS on the
  // first segment, post_cb raises it on the last + fires the done callback. .user ==
  // NULL on every sync transaction so the callbacks no-op for the normal paths.
  struct AsyncSeg
  {
    Arduino_ESP32SPIDMA *bus;
    bool first;
    bool last;
  };
  static void _async_pre_cb(spi_transaction_t *t);
  static void _async_post_cb(spi_transaction_t *t);
  // A full-height C6 tile (172 x BOARD_PARTIAL_BUF_LINES px) at 2 bytes/px, split into
  // ESP32SPIDMA_MAX_PIXELS_AT_ONCE*2-byte (2 KB) segments: 172*70*2 = 24080 B / 2048
  // ~= 12 segments worst case. 16 gives headroom; the array cost is tiny.
  static const int ASYNC_MAX_SEG = 16;
  spi_transaction_t _async_tran[ASYNC_MAX_SEG];
  AsyncSeg _async_seg[ASYNC_MAX_SEG];
  volatile uint8_t _async_pending = 0; // queued-but-unreaped async transactions
  volatile bool _sync_only = false;    // HYBRID: true while SD I/O owns the bus -> no async
  bool _bus_acquired = false;          // did beginWrite take spi_device_acquire_bus? (release in endWrite)
  void (*_async_done_cb)(void *) = nullptr;
  void *_async_done_arg = nullptr;
};

#endif // #if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)
