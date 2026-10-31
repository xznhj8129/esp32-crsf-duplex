# esp32-crsf-duplex

Half-duplex to full-duplex CRSF converters using ESP32 boards.

## ESP32 Dev Module USB adapter

`crsf_hdtofd_usbcdc/crsf_hdtofd_usbcdc.ino` uses the development board's USB
serial connector as the computer-facing full-duplex CRSF port. It needs only
one signal wire between ESP32 GPIO4 and the transmitter module's S.port, plus a
common ground. Both sides run at 921600 baud.

On a classic ESP32 Dev Module, the USB connector is implemented by the board's
built-in USB-to-UART bridge and reaches ESP32 UART0 on GPIO1/GPIO3. It appears
as a USB CDC serial device on the computer and eliminates the separate external
CP2102 used by `crsf_hdtofd`.

### Wiring

| ESP32 Dev Module | ELRS transmitter module |
|---|---|
| GPIO4 | S.port |
| GND | GND |

Connect the ESP32 development board to the computer through its USB connector.
Do not connect GPIO1 or GPIO3 externally; the board already connects them to
its USB serial bridge.

The USB serial stream is binary CRSF data. The sketch deliberately produces no
debug output because any text written to UART0 would corrupt that stream. The
ESP32 ROM may emit boot text when it resets, so the computer-side program
should discard input accumulated before opening the CRSF session.

### Other variants

- `crsf_hdtofd` is the original classic ESP32, two-external-UART version at
  115200 baud.
- `crsf_hd_fd_c3` is the ESP32-C3 GPIO UART experiment at 115200 baud.
