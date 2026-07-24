#include "Arduino_ESP32SPIDMA.h"

#if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)

/**
 * @brief Arduino_ESP32SPIDMA
 *
 */
Arduino_ESP32SPIDMA::Arduino_ESP32SPIDMA(
    int8_t dc /* = GFX_NOT_DEFINED */, int8_t cs /* = GFX_NOT_DEFINED */, int8_t sck /* = GFX_NOT_DEFINED */, int8_t mosi /* = GFX_NOT_DEFINED */, int8_t miso /* = GFX_NOT_DEFINED */, uint8_t spi_num /* = VSPI for ESP32, HSPI for S2 & S3, FSPI for C3 */, bool is_shared_interface /* = true */)
    : _dc(dc), _spi_num(spi_num), _is_shared_interface(is_shared_interface)
{
#if CONFIG_IDF_TARGET_ESP32
  if (
      sck == GFX_NOT_DEFINED && miso == GFX_NOT_DEFINED && mosi == GFX_NOT_DEFINED && cs == GFX_NOT_DEFINED)
  {
    _sck = (_spi_num == VSPI) ? SCK : 14;
    _miso = (_spi_num == VSPI) ? MISO : 12;
    _mosi = (_spi_num == VSPI) ? MOSI : 13;
    _cs = (_spi_num == VSPI) ? SS : 15;
  }
  else
  {
    _sck = sck;
    _miso = miso;
    _mosi = mosi;
    _cs = cs;
  }
#else
  if (sck == GFX_NOT_DEFINED && miso == GFX_NOT_DEFINED && mosi == GFX_NOT_DEFINED && cs == GFX_NOT_DEFINED)
  {
    _sck = SCK;
    _miso = MISO;
    _mosi = MOSI;
    _cs = SS;
  }
  else
  {
    _sck = sck;
    _miso = miso;
    _mosi = mosi;
    _cs = cs;
  }
#endif
}

/**
 * @brief begin
 *
 * @param speed
 * @param dataMode
 * @return true
 * @return false
 */
bool Arduino_ESP32SPIDMA::begin(int32_t speed, int8_t dataMode)
{
  // set SPI parameters
  _speed = (speed == GFX_NOT_DEFINED) ? SPI_DEFAULT_FREQ : speed;
  _dataMode = (dataMode == GFX_NOT_DEFINED) ? SPI_MODE0 : dataMode;

  // Fix for ESP32 Arduino core 3.3.6+ compatibility
  // Ref: https://github.com/espressif/arduino-esp32/pull/12265
  // Note: _div is not used in DMA mode (speed is passed directly to ESP-IDF driver),
  // so we skip the call entirely for 3.3.6+ to avoid the changed function signature.
#if !defined(ESP_ARDUINO_VERSION) || (ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 6))
  if (!_div)
  {
    _div = spiFrequencyToClockDiv(_speed);
  }
#endif

  // set pin mode
  if (_dc != GFX_NOT_DEFINED)
  {
    pinMode(_dc, OUTPUT);
    digitalWrite(_dc, HIGH); // Data mode
  }
  if (_cs != GFX_NOT_DEFINED)
  {
    pinMode(_cs, OUTPUT);
    digitalWrite(_cs, HIGH); // disable chip select
  }

#if (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)
  // set fastIO variables
  if (_dc >= 32)
  {
    _dcPinMask = digitalPinToBitMask(_dc);
    _dcPortSet = (PORTreg_t)GPIO_OUT1_W1TS_REG;
    _dcPortClr = (PORTreg_t)GPIO_OUT1_W1TC_REG;
  }
  else if (_dc != GFX_NOT_DEFINED)
  {
    _dcPinMask = digitalPinToBitMask(_dc);
    _dcPortSet = (PORTreg_t)GPIO_OUT_W1TS_REG;
    _dcPortClr = (PORTreg_t)GPIO_OUT_W1TC_REG;
  }
#else
  if (_dc != GFX_NOT_DEFINED)
  {
    _dcPinMask = digitalPinToBitMask(_dc);
    _dcPortSet = (PORTreg_t)GPIO_OUT_W1TS_REG;
    _dcPortClr = (PORTreg_t)GPIO_OUT_W1TC_REG;
  }
#endif

#if (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3)
  if (_cs >= 32)
  {
    _csPinMask = digitalPinToBitMask(_cs);
    _csPortSet = (PORTreg_t)GPIO_OUT1_W1TS_REG;
    _csPortClr = (PORTreg_t)GPIO_OUT1_W1TC_REG;
  }
  else if (_cs != GFX_NOT_DEFINED)
  {
    _csPinMask = digitalPinToBitMask(_cs);
    _csPortSet = (PORTreg_t)GPIO_OUT_W1TS_REG;
    _csPortClr = (PORTreg_t)GPIO_OUT_W1TC_REG;
  }
#else
  if (_cs != GFX_NOT_DEFINED)
  {
    _csPinMask = digitalPinToBitMask(_cs);
    _csPortSet = (PORTreg_t)GPIO_OUT_W1TS_REG;
    _csPortClr = (PORTreg_t)GPIO_OUT_W1TC_REG;
  }
#endif

  spi_bus_config_t buscfg = {
      .mosi_io_num = _mosi,
      .miso_io_num = _miso,
      .sclk_io_num = _sck,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .data4_io_num = -1,
      .data5_io_num = -1,
      .data6_io_num = -1,
      .data7_io_num = -1,
      // LOCAL (OpenWatchFace): big enough for a whole async tile in ONE DMA transfer
      // (172 x up to ~120 lines x 2 B = ~41 KB). The stock value (ESP32SPIDMA_MAX_PIXELS
      // *16+8 ~= 16 KB) forced multi-segment, and multi-segment async tiles rendered as
      // a stale/black band the height of one buffer; one transfer per tile avoids it.
      .max_transfer_sz = 48 * 1024,
      .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
      .intr_flags = 0};
  // LOCAL FIX (OpenWatchFace): resolve the IDF host by IDENTITY, exactly like the
  // Arduino core (esp32-hal-spi.c: `host = (num == FSPI) ? SPI2_HOST : SPI3_HOST`).
  // The stock arithmetic `_spi_num - 1` is WRONG on the C6: Arduino FSPI == 0 there,
  // and IDF SPI1_HOST == 0 is the FLASH controller — so `(spi_host_device_t)0` grabbed
  // the flash bus (already initialized WITH the flash device attached), giving the
  // "SPI bus already initialized" abort and then a crash when freeing it. FSPI must map
  // to SPI2_HOST, HSPI to SPI3_HOST, regardless of the Arduino enum's numeric value.
#ifdef SPI3_HOST
  spi_host_device_t _spidma_host = (_spi_num == FSPI) ? SPI2_HOST : SPI3_HOST;
#else
  spi_host_device_t _spidma_host = SPI2_HOST;  // C6/C3/H2: single general-purpose SPI host
#endif
  esp_err_t ret = spi_bus_initialize(_spidma_host, &buscfg, ESP32SPIDMA_DMA_CHANNEL);
  if (ret != ESP_OK)
  {
    ESP_ERROR_CHECK(ret);
    return false;
  }

  spi_device_interface_config_t devcfg = {
      .command_bits = 0,
      .address_bits = 0,
      .dummy_bits = 0,
      .mode = (uint8_t)_dataMode,
      .duty_cycle_pos = 128,
      .cs_ena_pretrans = 0,
      .cs_ena_posttrans = 0,
      .clock_speed_hz = _speed,
      .input_delay_ns = 0,
      .spics_io_num = -1, // avoid use system CS control
      .flags = (_miso < 0) ? (uint32_t)SPI_DEVICE_NO_DUMMY : 0,
      // LOCAL ADDITION (OpenWatchFace C6-1.47): queue_size>1 so writePixelsAsync() can
      // hold several DMA tile segments in flight; pre/post callbacks drive CS + the
      // flush-ready callback for the async path. Both no-op for sync transactions
      // (.user == NULL), so all the normal blocking paths are unchanged.
      .queue_size = ASYNC_MAX_SEG,
      .pre_cb = Arduino_ESP32SPIDMA::_async_pre_cb,
      .post_cb = Arduino_ESP32SPIDMA::_async_post_cb};
  ret = spi_bus_add_device(_spidma_host, &devcfg, &_handle); // LOCAL FIX: resolved host (see above)
  if (ret != ESP_OK)
  {
    ESP_ERROR_CHECK(ret);
    return false;
  }

  // LOCAL ADDITION (OpenWatchFace C6-1.47): do NOT permanently acquire the bus, even
  // in the non-shared case. spi_device_acquire_bus() forbids the queued/interrupt API
  // (spi_device_queue_trans) for its whole duration — and writePixelsAsync() uses that
  // queued API. The IDF spi_master arbitrates per-transaction between this LCD device
  // and the microSD device that shares this host, so the permanent acquire isn't
  // needed for correctness; dropping it is what lets the async flush + a 2nd device
  // (SD) coexist. (Original: if (!_is_shared_interface) spi_device_acquire_bus(...);)

  memset(&_spi_tran, 0, sizeof(_spi_tran));

  _buffer = (uint8_t *)heap_caps_aligned_alloc(16, ESP32SPIDMA_MAX_PIXELS_AT_ONCE * 2, MALLOC_CAP_DMA);
  if (!_buffer)
  {
    return false;
  }
  _2nd_buffer = (uint8_t *)heap_caps_aligned_alloc(16, ESP32SPIDMA_MAX_PIXELS_AT_ONCE * 2, MALLOC_CAP_DMA);
  if (!_2nd_buffer)
  {
    return false;
  }

  return true;
}

/**
 * @brief beginWrite
 *
 */
void Arduino_ESP32SPIDMA::beginWrite()
{
  _data_buf_bit_idx = 0;
  _buffer[0] = 0;

  // LOCAL (OpenWatchFace C6): acquire the IDF bus across the WHOLE CS bracket while sync_only
  // (SD coexists on this host). The display drives DC/CS via raw GPIO across MANY transactions
  // per bracket; without holding the bus, the IDF lets a microSD transaction interleave between
  // them -> both CS asserted -> garbled panel. Holding the bus makes the IDF block SD until
  // endWrite releases. Tracked so endWrite releases iff we acquired. (Normal async mode does NOT
  // acquire — incompatible with the queued DMA API.)
  _bus_acquired = (_is_shared_interface || _sync_only);
  if (_bus_acquired)
  {
    spi_device_acquire_bus(_handle, portMAX_DELAY);
  }

  if (_dc != GFX_NOT_DEFINED)
  {
    DC_HIGH();
  }
  CS_LOW();
}

/**
 * @brief endWrite
 *
 */
void Arduino_ESP32SPIDMA::endWrite()
{
  if (_data_buf_bit_idx > 0)
  {
    flush_data_buf();
  }

  if (_bus_acquired)
  {
    spi_device_release_bus(_handle);
    _bus_acquired = false;
  }

  CS_HIGH();
}

/**
 * @brief writeCommand
 *
 * @param c
 */
void Arduino_ESP32SPIDMA::writeCommand(uint8_t c)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    WRITE9BIT(c);
  }
  else
  {
    if (_data_buf_bit_idx > 0)
    {
      flush_data_buf();
    }

    DC_LOW();

    _spi_tran.length = 8;
    _spi_tran.tx_data[0] = c;
    _spi_tran.flags = SPI_TRANS_USE_TXDATA;

    POLL_START();
    POLL_END();

    DC_HIGH();
  }
}

/**
 * @brief writeCommand16
 *
 * @param c
 */
void Arduino_ESP32SPIDMA::writeCommand16(uint16_t c)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    _data16.value = c;
    WRITE9BIT(_data16.msb);
    WRITE9BIT(_data16.lsb);
  }
  else
  {
    if (_data_buf_bit_idx > 0)
    {
      flush_data_buf();
    }

    DC_LOW();

    _spi_tran.length = 16;
    MSB_16_SET(_spi_tran.tx_data[0], c);
    _spi_tran.flags = SPI_TRANS_USE_TXDATA;

    POLL_START();
    POLL_END();

    DC_HIGH();
  }
}

/**
 * @brief
 *
 * @param data
 * @param len
 */
void Arduino_ESP32SPIDMA::writeCommandBytes(uint8_t *data, uint32_t len)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    while (len--)
    {
      WRITE9BIT(*data++);
    }
  }
  else
  {
    DC_LOW();
    while (len--)
    {
      WRITE8BIT(*data++);
    }
    DC_HIGH();
  }
}

/**
 * @brief write
 *
 * @param d
 */
void Arduino_ESP32SPIDMA::write(uint8_t d)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    WRITE9BIT(0x100 | d);
  }
  else
  {
    WRITE8BIT(d);
  }
}

/**
 * @brief write16
 *
 * @param d
 */
void Arduino_ESP32SPIDMA::write16(uint16_t d)
{
  _data16.value = d;
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    WRITE9BIT(0x100 | _data16.msb);
    WRITE9BIT(0x100 | _data16.lsb);
  }
  else
  {
    WRITE8BIT(_data16.msb);
    WRITE8BIT(_data16.lsb);
  }
}

/**
 * @brief writeC8D8
 *
 * @param c
 * @param d
 */
void Arduino_ESP32SPIDMA::writeC8D8(uint8_t c, uint8_t d)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    WRITE9BIT(c);
    WRITE9BIT(0x100 | d);
  }
  else
  {
    if (_data_buf_bit_idx > 0)
    {
      flush_data_buf();
    }

    DC_LOW();

    _spi_tran.length = 8;
    _spi_tran.tx_data[0] = c;
    _spi_tran.flags = SPI_TRANS_USE_TXDATA;

    POLL_START();
    POLL_END();

    DC_HIGH();

    _spi_tran.length = 8;
    _spi_tran.tx_data[0] = d;
    _spi_tran.flags = SPI_TRANS_USE_TXDATA;

    POLL_START();
    POLL_END();
  }
}

/**
 * @brief writeC8D16
 *
 * @param c
 * @param d
 */
void Arduino_ESP32SPIDMA::writeC8D16(uint8_t c, uint16_t d)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    WRITE9BIT(c);
    _data16.value = d;
    WRITE9BIT(0x100 | _data16.msb);
    WRITE9BIT(0x100 | _data16.lsb);
  }
  else
  {
    if (_data_buf_bit_idx > 0)
    {
      flush_data_buf();
    }

    DC_LOW();

    _spi_tran.length = 8;
    _spi_tran.tx_data[0] = c;
    _spi_tran.flags = SPI_TRANS_USE_TXDATA;

    POLL_START();
    POLL_END();

    DC_HIGH();

    _spi_tran.length = 16;
    _spi_tran.tx_data[0] = (d >> 8);
    _spi_tran.tx_data[1] = (d & 0xff);
    _spi_tran.flags = SPI_TRANS_USE_TXDATA;

    POLL_START();
    POLL_END();
  }
}

/**
 * @brief writeC8D16D16
 *
 * @param c
 * @param d1
 * @param d2
 */
void Arduino_ESP32SPIDMA::writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    WRITE9BIT(c);
    _data16.value = d1;
    WRITE9BIT(0x100 | _data16.msb);
    WRITE9BIT(0x100 | _data16.lsb);
    _data16.value = d2;
    WRITE9BIT(0x100 | _data16.msb);
    WRITE9BIT(0x100 | _data16.lsb);
  }
  else
  {
    if (_data_buf_bit_idx > 0)
    {
      flush_data_buf();
    }

    DC_LOW();

    _spi_tran.length = 8;
    _spi_tran.tx_data[0] = c;
    _spi_tran.flags = SPI_TRANS_USE_TXDATA;

    POLL_START();
    POLL_END();

    DC_HIGH();

    _spi_tran.length = 32;
    _spi_tran.tx_data[0] = (d1 >> 8);
    _spi_tran.tx_data[1] = (d1 & 0xff);
    _spi_tran.tx_data[2] = (d2 >> 8);
    _spi_tran.tx_data[3] = (d2 & 0xff);
    _spi_tran.flags = SPI_TRANS_USE_TXDATA;

    POLL_START();
    POLL_END();
  }
}

/**
 * @brief writeRepeat
 *
 * @param p
 * @param len
 */
void Arduino_ESP32SPIDMA::writeRepeat(uint16_t p, uint32_t len)
{
  if (_data_buf_bit_idx > 0)
  {
    flush_data_buf();
  }

  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    _data16.value = p;
    uint32_t hi = 0x100 | _data16.msb;
    uint32_t lo = 0x100 | _data16.lsb;
    uint16_t idx;
    uint8_t shift;
    uint16_t bufLen = (len <= 28) ? len : 28;
    int16_t xferLen;
    for (uint32_t t = 0; t < bufLen; t++)
    {
      idx = _data_buf_bit_idx >> 3;
      shift = (_data_buf_bit_idx % 8);
      if (shift)
      {
        _buffer[idx++] |= hi >> (shift + 1);
        _buffer[idx] = hi << (7 - shift);
      }
      else
      {
        _buffer[idx++] = hi >> 1;
        _buffer[idx] = hi << 7;
      }
      _data_buf_bit_idx += 9;

      idx = _data_buf_bit_idx >> 3;
      shift = (_data_buf_bit_idx % 8);
      if (shift)
      {
        _buffer[idx++] |= lo >> (shift + 1);
        _buffer[idx] = lo << (7 - shift);
      }
      else
      {
        _buffer[idx++] = lo >> 1;
        _buffer[idx] = lo << 7;
      }
      _data_buf_bit_idx += 9;
    }

    // Issue pixels in blocks from temp buffer
    while (len) // While pixels remain
    {
      xferLen = (bufLen < len) ? bufLen : len; // How many this pass?
      _data_buf_bit_idx = xferLen * 18;

      _spi_tran.tx_buffer = _buffer32;
      _spi_tran.length = _data_buf_bit_idx;
      _spi_tran.flags = 0;

      POLL_START();
      POLL_END();

      len -= xferLen;
    }
  }
  else // 8-bit SPI
  {
    uint16_t bufLen = (len >= ESP32SPIDMA_MAX_PIXELS_AT_ONCE) ? ESP32SPIDMA_MAX_PIXELS_AT_ONCE : len;
    int16_t xferLen, l;
    uint32_t c32;
    MSB_32_16_16_SET(c32, p, p);

    l = (bufLen + 1) / 2;
    for (uint32_t i = 0; i < l; i++)
    {
      _buffer32[i] = c32;
    }

    // Issue pixels in blocks from temp buffer
    while (len) // While pixels remain
    {
      xferLen = (bufLen <= len) ? bufLen : len; // How many this pass?

      _spi_tran.tx_buffer = _buffer32;
      _spi_tran.length = xferLen << 4;
      _spi_tran.flags = 0;

      POLL_START();
      POLL_END();

      len -= xferLen;
    }
  }

  _data_buf_bit_idx = 0;
}

/**
 * @brief writePixels
 *
 * @param data
 * @param len
 */
void Arduino_ESP32SPIDMA::writePixels(uint16_t *data, uint32_t len)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    while (len--)
    {
      write16(*data++);
    }
  }
  else // 8-bit SPI
  {
    if (_data_buf_bit_idx > 0)
    {
      flush_data_buf();
    }

    uint32_t l, l2;
    uint16_t p1, p2;
    while (len)
    {
      l = (len > ESP32SPIDMA_MAX_PIXELS_AT_ONCE) ? ESP32SPIDMA_MAX_PIXELS_AT_ONCE : len;
      l2 = (l + 1) >> 1;
      for (uint32_t i = 0; i < l2; ++i)
      {
        p1 = *data++;
        p2 = *data++;
        MSB_32_16_16_SET(_buffer32[i], p1, p2);
      }
      if (l & 1)
      {
        p1 = *data++;
        MSB_16_SET(_buffer16[l - 1], p1);
      }

      _spi_tran.tx_buffer = _buffer32;
      _spi_tran.length = l << 4;
      _spi_tran.flags = 0;

      POLL_START();
      POLL_END();

      len -= l;
    }
  }
}

/**
 * @brief writeBytes
 *
 * @param data
 * @param len
 */
void Arduino_ESP32SPIDMA::writeBytes(uint8_t *data, uint32_t len)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    while (len--)
    {
      write(*data++);
    }
  }
  else // 8-bit SPI
  {
    if (esp_ptr_dma_capable(data))
    {
      if (_data_buf_bit_idx > 0)
      {
        flush_data_buf();
      }

      uint32_t l;
      while (len)
      {
        l = (len >= (ESP32SPIDMA_MAX_PIXELS_AT_ONCE << 1)) ? (ESP32SPIDMA_MAX_PIXELS_AT_ONCE << 1) : len;

        _spi_tran.tx_buffer = data;
        _spi_tran.length = l << 3;
        _spi_tran.flags = 0;

        POLL_START();
        POLL_END();

        len -= l;
        data += l;
      }
    }
    else
    {
      if (_data_buf_bit_idx > 0)
      {
        flush_data_buf();
      }

      uint32_t l, l4;
      uint32_t *p;
      while (len)
      {
        l = (len > (ESP32SPIDMA_MAX_PIXELS_AT_ONCE << 1)) ? (ESP32SPIDMA_MAX_PIXELS_AT_ONCE << 1) : len;
        l4 = (l + 3) >> 2;
        p = (uint32_t *)data;
        for (uint32_t i = 0; i < l4; ++i)
        {
          _buffer32[i] = *p++;
        }

        _spi_tran.tx_buffer = _buffer32;
        _spi_tran.length = l << 3;
        _spi_tran.flags = 0;

        POLL_START();
        POLL_END();

        len -= l;
        data += l;
      }
    }
  }
}

/**
 * @brief writeIndexedPixels
 *
 * @param data
 * @param idx
 * @param len
 */
void Arduino_ESP32SPIDMA::writeIndexedPixels(uint8_t *data, uint16_t *idx, uint32_t len)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    while (len--)
    {
      write16(idx[*data++]);
    }
  }
  else // 8-bit SPI
  {
    if (_data_buf_bit_idx > 0)
    {
      flush_data_buf();
    }

    uint32_t l, l2;
    uint16_t p1, p2;
    while (len)
    {
      l = (len > ESP32SPIDMA_MAX_PIXELS_AT_ONCE) ? ESP32SPIDMA_MAX_PIXELS_AT_ONCE : len;
      l2 = l >> 1;
      for (uint32_t i = 0; i < l2; ++i)
      {
        p1 = idx[*data++];
        p2 = idx[*data++];
        MSB_32_16_16_SET(_buffer32[i], p1, p2);
      }
      if (l & 1)
      {
        p1 = idx[*data++];
        MSB_16_SET(_buffer16[l - 1], p1);
      }

      _spi_tran.tx_buffer = _buffer32;
      _spi_tran.length = l << 4;
      _spi_tran.flags = 0;

      POLL_START();
      POLL_END();

      len -= l;
    }
  }
}

/**
 * @brief writeIndexedPixelsDouble
 *
 * @param data
 * @param idx
 * @param len
 */
void Arduino_ESP32SPIDMA::writeIndexedPixelsDouble(uint8_t *data, uint16_t *idx, uint32_t len)
{
  if (_dc == GFX_NOT_DEFINED) // 9-bit SPI
  {
    uint16_t hi, lo;
    while (len--)
    {
      _data16.value = idx[*data++];
      hi = 0x100 | _data16.msb;
      lo = 0x100 | _data16.lsb;
      WRITE9BIT(hi);
      WRITE9BIT(lo);
      WRITE9BIT(hi);
      WRITE9BIT(lo);
    }
  }
  else // 8-bit SPI
  {
    if (_data_buf_bit_idx > 0)
    {
      flush_data_buf();
    }

    uint32_t l;
    uint16_t p;
    while (len)
    {
      l = (len > (ESP32SPIDMA_MAX_PIXELS_AT_ONCE >> 1)) ? (ESP32SPIDMA_MAX_PIXELS_AT_ONCE >> 1) : len;
      for (uint32_t i = 0; i < l; ++i)
      {
        p = idx[*data++];
        MSB_32_16_16_SET(_buffer32[i], p, p);
      }

      _spi_tran.tx_buffer = _buffer32;
      _spi_tran.length = l << 5;
      _spi_tran.flags = 0;

      POLL_START();
      POLL_END();

      len -= l;
    }
  }
}

void Arduino_ESP32SPIDMA::writeYCbCrPixels(uint8_t *yData, uint8_t *cbData, uint8_t *crData, uint16_t w, uint16_t h)
{
  if (w > (ESP32SPIDMA_MAX_PIXELS_AT_ONCE / 2))
  {
    Arduino_DataBus::writeYCbCrPixels(yData, cbData, crData, w, h);
  }
  else
  {
    int cols = w >> 1;
    int rows = h >> 1;
    uint8_t *yData2 = yData + w;
    uint16_t *dest = _buffer16;
    uint16_t *dest2 = dest + w;

    uint8_t pxCb, pxCr;
    int16_t pxR, pxG, pxB, pxY;

    uint16_t out_bits = w << 5;
    bool poll_started = false;
    for (int row = 0; row < rows; ++row)
    {
      for (int col = 0; col < cols; ++col)
      {
        pxCb = *cbData++;
        pxCr = *crData++;
        pxR = CR2R16[pxCr];
        pxG = -CB2G16[pxCb] - CR2G16[pxCr];
        pxB = CB2B16[pxCb];

        pxY = Y2I16[*yData++];
        *dest++ = CLIPRBE[pxY + pxR] | CLIPGBE[pxY + pxG] | CLIPBBE[pxY + pxB];
        pxY = Y2I16[*yData++];
        *dest++ = CLIPRBE[pxY + pxR] | CLIPGBE[pxY + pxG] | CLIPBBE[pxY + pxB];
        pxY = Y2I16[*yData2++];
        *dest2++ = CLIPRBE[pxY + pxR] | CLIPGBE[pxY + pxG] | CLIPBBE[pxY + pxB];
        pxY = Y2I16[*yData2++];
        *dest2++ = CLIPRBE[pxY + pxR] | CLIPGBE[pxY + pxG] | CLIPBBE[pxY + pxB];
      }
      yData += w;
      yData2 += w;

      if (poll_started)
      {
        POLL_END();
      }
      else
      {
        poll_started = true;
      }
      if (row & 1)
      {
        _spi_tran.tx_buffer = _2nd_buffer32;
        dest = _buffer16;
      }
      else
      {
        _spi_tran.tx_buffer = _buffer32;
        dest = _2nd_buffer16;
      }
      _spi_tran.length = out_bits;
      _spi_tran.flags = 0;

      POLL_START();
      dest2 = dest + w;
    }

    POLL_END();
  }
}

/**
 * @brief flush_data_buf
 *
 */
void Arduino_ESP32SPIDMA::flush_data_buf()
{
  _spi_tran.tx_buffer = _buffer32;
  _spi_tran.length = _data_buf_bit_idx;
  _spi_tran.flags = 0;

  POLL_START();
  POLL_END();

  _data_buf_bit_idx = 0;
}

/**
 * @brief WRITE8BIT
 *
 * @param d
 * @return GFX_INLINE
 */
GFX_INLINE void Arduino_ESP32SPIDMA::WRITE8BIT(uint8_t d)
{
  uint16_t idx = _data_buf_bit_idx >> 3;
  _buffer[idx] = d;
  _data_buf_bit_idx += 8;
  if (_data_buf_bit_idx >= (ESP32SPIDMA_MAX_PIXELS_AT_ONCE << 4))
  {
    flush_data_buf();
  }
}

/**
 * @brief WRITE9BIT
 *
 * @param d
 * @return GFX_INLINE
 */
GFX_INLINE void Arduino_ESP32SPIDMA::WRITE9BIT(uint32_t d)
{
  uint16_t idx = _data_buf_bit_idx >> 3;
  uint8_t shift = (_data_buf_bit_idx % 8);
  if (shift)
  {
    _buffer[idx++] |= d >> (shift + 1);
    _buffer[idx] = d << (7 - shift);
  }
  else
  {
    _buffer[idx++] = d >> 1;
    _buffer[idx] = d << 7;
  }
  _data_buf_bit_idx += 9;
  if (_data_buf_bit_idx >= 504) // 56 bytes * 9 bits
  {
    flush_data_buf();
  }
}

/******** low level bit twiddling **********/

/**
 * @brief DC_HIGH
 *
 * @return GFX_INLINE
 */
GFX_INLINE void Arduino_ESP32SPIDMA::DC_HIGH(void)
{
  *_dcPortSet = _dcPinMask;
}

/**
 * @brief DC_LOW
 *
 * @return GFX_INLINE
 */
GFX_INLINE void Arduino_ESP32SPIDMA::DC_LOW(void)
{
  *_dcPortClr = _dcPinMask;
}

/**
 * @brief CS_HIGH
 *
 * @return GFX_INLINE
 */
GFX_INLINE void Arduino_ESP32SPIDMA::CS_HIGH(void)
{
  if (_cs != GFX_NOT_DEFINED)
  {
    *_csPortSet = _csPinMask;
  }
}

/**
 * @brief CS_LOW
 *
 * @return GFX_INLINE
 */
GFX_INLINE void Arduino_ESP32SPIDMA::CS_LOW(void)
{
  if (_cs != GFX_NOT_DEFINED)
  {
    *_csPortClr = _csPinMask;
  }
}

/**
 * @brief POLL_START
 *
 * @return GFX_INLINE
 */
GFX_INLINE void Arduino_ESP32SPIDMA::POLL_START()
{
  spi_device_polling_start(_handle, &_spi_tran, portMAX_DELAY);
}

/**
 * @brief POLL_END
 *
 * @return GFX_INLINE
 */
GFX_INLINE void Arduino_ESP32SPIDMA::POLL_END()
{
  spi_device_polling_end(_handle, portMAX_DELAY);
}

// ====================================================================
// LOCAL ADDITION (OpenWatchFace C6-1.47): ASYNC tile write.
// The caller (my_disp_flush) has already set the address window with sync polling
// transactions and left DC HIGH (data) + the device's CS still HIGH. We take CS LOW
// in the pre_cb of the first segment, stream the (already panel-byte-order, DMA-capable)
// pixel buffer as up to ASYNC_MAX_SEG queued DMA transactions, and return immediately.
// The wire drains under DMA while LVGL renders the next tile; the last segment's post_cb
// raises CS and calls done_cb(done_arg) (lv_disp_flush_ready) from ISR context.
// ====================================================================
bool Arduino_ESP32SPIDMA::writePixelsAsync(const uint8_t *data, uint32_t len,
                                           void (*done_cb)(void *), void *done_arg)
{
  // HYBRID: while SD I/O owns the shared host, refuse async so the caller flushes synchronously
  // (a held bus / blocking transaction can't corrupt against the SD device). See set_sync_only.
  if (_sync_only)
  {
    return false;
  }
  if (!len)
  {
    if (done_cb)
    {
      done_cb(done_arg);
    }
    return true;
  }

  // One DMA transaction per tile when possible: SEG matches the bus max_transfer_sz
  // (48 KB), so a full partial tile goes out as a SINGLE queued transfer. Multi-segment
  // async tiles rendered as a stale band; keeping it to one segment avoids that and is
  // faster (one IRQ). Still a multiple of 2 for whole RGB565 pixels.
  const uint32_t SEG = 48 * 1024; // bytes (== bus max_transfer_sz)
  uint32_t nseg = (len + SEG - 1) / SEG;
  if (nseg > (uint32_t)ASYNC_MAX_SEG)
  {
    return false; // too big for one CS bracket — caller uses the blocking path
  }

  waitAsync(); // only one async tile may be in flight at a time
  _async_done_cb = done_cb;
  _async_done_arg = done_arg;

  for (uint32_t i = 0; i < nseg; i++)
  {
    spi_transaction_t *t = &_async_tran[i];
    AsyncSeg *s = &_async_seg[i];
    s->bus = this;
    s->first = (i == 0);
    s->last = (i == nseg - 1);

    uint32_t off = i * SEG;
    uint32_t l = len - off;
    if (l > SEG)
    {
      l = SEG;
    }

    memset(t, 0, sizeof(*t));
    t->tx_buffer = data + off;
    t->length = l << 3; // bits
    t->user = s;

    if (spi_device_queue_trans(_handle, t, portMAX_DELAY) != ESP_OK)
    {
      waitAsync(); // reap whatever queued, leave the bus clean
      return false;
    }
    _async_pending++;
  }
  return true;
}

// LOCAL ADDITION (OpenWatchFace C6-1.47): one dummy byte on THIS device with every CS on the
// bus HIGH, so the IDF's device-switch reconfig (SD 20 MHz -> LCD 80 MHz) happens during a
// transaction neither the panel nor the SD card is listening to. See header comment.
void Arduino_ESP32SPIDMA::resyncBus(void)
{
  _spi_tran.length = 8;
  _spi_tran.tx_data[0] = 0x00;
  _spi_tran.flags = SPI_TRANS_USE_TXDATA;
  POLL_START();
  POLL_END();
}

// LOCAL ADDITION (OpenWatchFace C6-1.47): raise the panel's raw-GPIO CS. See header comment.
void Arduino_ESP32SPIDMA::forceCsIdle(void)
{
  CS_HIGH();
}

void Arduino_ESP32SPIDMA::waitAsync()
{
  while (_async_pending)
  {
    spi_transaction_t *done;
    spi_device_get_trans_result(_handle, &done, portMAX_DELAY);
    _async_pending--;
  }
}

// Both callbacks run in the SPI interrupt for EVERY transaction on this device; sync
// transactions have .user == NULL and fall straight through. CS is written via the raw
// port registers (NOT CS_LOW/CS_HIGH — those are fine here but we keep it register-direct
// to stay ISR-safe and match the QSPI async path).
void Arduino_ESP32SPIDMA::_async_pre_cb(spi_transaction_t *t)
{
  AsyncSeg *s = (AsyncSeg *)t->user;
  if (s && s->first && s->bus->_cs != GFX_NOT_DEFINED)
  {
    *(s->bus->_csPortClr) = s->bus->_csPinMask; // CS LOW
  }
}

void Arduino_ESP32SPIDMA::_async_post_cb(spi_transaction_t *t)
{
  AsyncSeg *s = (AsyncSeg *)t->user;
  if (s && s->last)
  {
    Arduino_ESP32SPIDMA *b = s->bus;
    if (b->_cs != GFX_NOT_DEFINED)
    {
      *(b->_csPortSet) = b->_csPinMask; // CS HIGH
    }
    if (b->_async_done_cb)
    {
      b->_async_done_cb(b->_async_done_arg);
    }
  }
}

#endif // #if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5)
