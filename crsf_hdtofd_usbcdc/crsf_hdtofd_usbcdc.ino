#include <Arduino.h>

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "rom/gpio.h"
#include "soc/gpio_sig_map.h"

/*
  ESP32 Dev Module CRSF duplex adapter

  Computer USB serial <-> UART0 <-> ESP32 <-> UART1 <-> GPIO4 <-> ELRS S.port

  The computer-facing USB serial stream is binary CRSF data. Do not print debug
  output to Serial because Serial is the full-duplex side of this bridge.
*/

static const uart_port_t HD_UART = UART_NUM_1;
static const uart_port_t USB_UART = UART_NUM_0;

static const int HD_PIN = 4;
static const int USB_TX_PIN = 1;
static const int USB_RX_PIN = 3;

static const int BAUD = 921600;

static const int UART_RX_BUF_SIZE = 8192;
static const int UART_TX_BUF_SIZE = 4096;
static const int UART_RX_FULL_THRESHOLD = 16;

static const int IO_BUF_SIZE = 256;
static const int MAX_CHUNKS_PER_PASS = 8;

static const uint32_t UART_BYTE_US = ((1000000UL * 10UL) + BAUD - 1UL) / BAUD;

/*
  Pure bus-idle guard, not CRSF frame parsing.
  At 921600 baud, four byte times round up to 44 us.
*/
static const uint32_t HD_IDLE_US = UART_BYTE_US * 4UL;

/*
  ESP32 GPIO matrix constant used to detach UART1 RX while GPIO4 is transmitting.
  This is the same technique used by ExpressLRS CRSFHandset::duplex_set_TX().
*/
static const uint8_t MATRIX_DETACH_IN_LOW = 0x30;

static volatile bool hdTxActive = false;
static volatile uint32_t lastHdActivityUs = 0;

static uint8_t hdToUsbBuf[IO_BUF_SIZE];
static uint8_t usbToHdBuf[IO_BUF_SIZE];

static uint32_t nowUs()
{
  return (uint32_t)esp_timer_get_time();
}

static void IRAM_ATTR hdEdgeIsr(void *arg)
{
  (void)arg;

  if (!hdTxActive) {
    lastHdActivityUs = (uint32_t)esp_timer_get_time();
  }
}

static void stopHere(esp_err_t err)
{
  if (err == ESP_OK) {
    return;
  }

  while (true) {
    delay(1000);
  }
}

static void setupUartBase(uart_port_t port)
{
  uart_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  cfg.baud_rate = BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.rx_flow_ctrl_thresh = 0;

  stopHere(uart_driver_install(port, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, nullptr, 0));
  stopHere(uart_param_config(port, &cfg));
  stopHere(uart_set_mode(port, UART_MODE_UART));

  /*
    Drain sustained traffic in batches, then use the one-byte-time RX timeout
    to forward the tail of each short CRSF frame with about 11 us latency.
  */
  stopHere(uart_set_rx_full_threshold(port, UART_RX_FULL_THRESHOLD));
  stopHere(uart_set_rx_timeout(port, 1));

  uart_flush_input(port);
}

static void setupUsbUart()
{
  setupUartBase(USB_UART);

  stopHere(uart_set_pin(USB_UART,
                        USB_TX_PIN,
                        USB_RX_PIN,
                        UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE));

  uart_flush_input(USB_UART);
}

static void hdSetRx()
{
  /*
    The JR-bay CRSF S.port is inverted: physical idle low, UART logical idle
    high. The GPIO matrix performs the input inversion.
  */
  stopHere(gpio_set_direction((gpio_num_t)HD_PIN, GPIO_MODE_INPUT));

  gpio_matrix_in((gpio_num_t)HD_PIN, U1RXD_IN_IDX, true);

  gpio_pulldown_en((gpio_num_t)HD_PIN);
  gpio_pullup_dis((gpio_num_t)HD_PIN);
}

static void hdSetTx()
{
  /* Put the pad at the inverted UART idle level before enabling its output. */
  stopHere(gpio_set_pull_mode((gpio_num_t)HD_PIN, GPIO_FLOATING));
  stopHere(gpio_set_level((gpio_num_t)HD_PIN, 0));
  stopHere(gpio_set_direction((gpio_num_t)HD_PIN, GPIO_MODE_OUTPUT));

  /* Detach RX to prevent self-echo while UART1 owns the one-wire bus. */
  gpio_matrix_in(MATRIX_DETACH_IN_LOW, U1RXD_IN_IDX, false);

  /* Invert UART1 TX for the physical S.port signal. */
  gpio_matrix_out((gpio_num_t)HD_PIN, U1TXD_OUT_IDX, true, false);
}

static void setupHdUart()
{
  setupUartBase(HD_UART);

  hdSetRx();
  uart_flush_input(HD_UART);
}

static bool hdBusIdle()
{
  return (uint32_t)(nowUs() - lastHdActivityUs) >= HD_IDLE_US;
}

static bool hdLineLooksIdle()
{
  return gpio_get_level((gpio_num_t)HD_PIN) == 0;
}

static size_t uartPending(uart_port_t port)
{
  size_t pending = 0;
  uart_get_buffered_data_len(port, &pending);
  return pending;
}

static bool hdTxDone()
{
  return uart_wait_tx_done(HD_UART, 0) == ESP_OK;
}

static bool hdTryStartTx()
{
  if (hdTxActive) {
    return true;
  }

  if (!hdBusIdle() || uartPending(HD_UART) > 0 || !hdLineLooksIdle()) {
    return false;
  }

  gpio_intr_disable((gpio_num_t)HD_PIN);

  if (!hdBusIdle() || uartPending(HD_UART) > 0 || !hdLineLooksIdle()) {
    gpio_intr_enable((gpio_num_t)HD_PIN);
    lastHdActivityUs = nowUs();
    return false;
  }

  hdTxActive = true;
  hdSetTx();
  return true;
}

static void hdReleaseTxIfDone()
{
  if (!hdTxActive || uartPending(USB_UART) > 0 || !hdTxDone()) {
    return;
  }

  uart_flush_input(HD_UART);

  hdSetRx();
  lastHdActivityUs = nowUs();
  hdTxActive = false;

  gpio_intr_enable((gpio_num_t)HD_PIN);
}

static void writeAll(uart_port_t port, const uint8_t *buf, int len)
{
  int offset = 0;

  while (offset < len) {
    int written = uart_write_bytes(port, (const char *)(buf + offset), len - offset);

    if (written <= 0) {
      return;
    }

    offset += written;
  }
}

static void serviceHdToUsb()
{
  if (hdTxActive) {
    return;
  }

  for (int chunk = 0; chunk < MAX_CHUNKS_PER_PASS; chunk++) {
    int n = uart_read_bytes(HD_UART, hdToUsbBuf, sizeof(hdToUsbBuf), 0);

    if (n <= 0) {
      return;
    }

    lastHdActivityUs = nowUs();
    writeAll(USB_UART, hdToUsbBuf, n);
  }
}

static void serviceUsbToHd()
{
  for (int chunk = 0; chunk < MAX_CHUNKS_PER_PASS; chunk++) {
    size_t pending = uartPending(USB_UART);

    if (pending == 0) {
      return;
    }

    if (!hdTxActive && !hdTryStartTx()) {
      return;
    }

    int toRead = (int)pending;

    if (toRead > IO_BUF_SIZE) {
      toRead = IO_BUF_SIZE;
    }

    int n = uart_read_bytes(USB_UART, usbToHdBuf, toRead, 0);

    if (n <= 0) {
      return;
    }

    writeAll(HD_UART, usbToHdBuf, n);
  }
}

void setup()
{
  /*
    Never call Serial.begin() or print to Serial in this sketch. UART0 is the
    binary CRSF stream presented by the ESP32 development board's USB port.
  */
  setupHdUart();
  setupUsbUart();

  stopHere(gpio_set_intr_type((gpio_num_t)HD_PIN, GPIO_INTR_ANYEDGE));

  esp_err_t isrErr = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);

  if (isrErr != ESP_OK && isrErr != ESP_ERR_INVALID_STATE) {
    stopHere(isrErr);
  }

  stopHere(gpio_isr_handler_add((gpio_num_t)HD_PIN, hdEdgeIsr, nullptr));

  lastHdActivityUs = nowUs();
}

void loop()
{
  if (hdTxActive) {
    serviceUsbToHd();
    hdReleaseTxIfDone();
    return;
  }

  serviceHdToUsb();
  serviceUsbToHd();
  hdReleaseTxIfDone();
}
