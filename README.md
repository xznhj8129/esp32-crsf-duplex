# esp32-crsf-duplex

Half-duplex to full-duplex CRSF converters on ESP32 boards.

An ExpressLRS transmitter module in a JR/handset bay talks CRSF on **one
inverted signal wire** (the S.port pin): the handset and the module take turns
on the same pad. A computer serial port, or a program such as
[crsfproxy](https://github.com/xznhj8129/crsfproxy), wants a normal
**full-duplex** UART: separate TX and RX, idle high. These sketches sit between
the two and forward bytes both ways, so a computer can drive the module as if
it were the handset.

```
computer / USB-UART  ==full duplex==  ESP32  --one wire, inverted--  ELRS TX module S.port
```

## Variants

| Sketch | Board | Computer side | Module side | Debug output |
|---|---|---|---|---|---|
| [`crsf_hdtofd_usbcdc`](crsf_hdtofd_usbcdc/crsf_hdtofd_usbcdc.ino) | ESP32 Dev Module (classic ESP32) | the board's own USB port (UART0, GPIO1/GPIO3) | UART1 on GPIO4 | none |
| [`crsf_hdtofd`](crsf_hdtofd/crsf_hdtofd.ino) | classic ESP32 | external USB-UART (e.g. CP2102) on UART2, TX GPIO17 / RX GPIO16 | UART1 on GPIO4 | 1 Hz counters on the board's USB port at 921600 |
| [`crsf_hd_fd_c3`](crsf_hd_fd_c3/crsf_hd_fd_c3.ino) | ESP32-C3 | external USB-UART on UART1, TX GPIO21 / RX GPIO20 | UART0 on GPIO3 | none |

Both sides of a converter run at the same baud, set by `BAUD` in the sketch.
The ELRS module locks to the baud of the first CRSF it receives after
power-up, so start the converter at the rate you want before powering the
module, or power-cycle the module after changing rate.

## How it works

All three sketches use the same mechanism. None of them parses CRSF: they
forward raw bytes and only arbitrate the single wire.

- **Inversion.** The S.port idles low; a UART idles high. The ESP32 GPIO matrix
  inverts the module-side UART's RX input and TX output, so no inverter
  transistor is needed. In receive, the pad has a pull-down.
- **Receive (module → computer).** The pad is a UART RX input. Whatever arrives
  is copied to the computer-side UART at once. An edge interrupt on the pad
  timestamps the last bus activity.
- **Bus-idle guard.** Bytes from the computer may only go on the wire once the
  bus has been quiet for **four byte times** (about 348 µs at 115200, 44 µs at
  921600). Until then they wait in the computer-side UART's driver buffer. The
  USB-port and C3 sketches also require the pad to read idle (low) and the
  module-side RX buffer to be empty, and check again with the edge interrupt
  disabled before taking the wire.
- **Transmit (computer → module).** The pad is set to the inverted idle level,
  switched to output, and given to the UART's TX signal (inverted). The UART's
  RX is detached from the pad (fed a constant from the GPIO matrix) so the
  converter does not hear its own bytes. This is the technique ExpressLRS uses
  in `CRSFHandset::duplex_set_TX()`.
- **Release.** The wire is kept while more computer bytes are already queued,
  so one burst is not split. When the queue is empty and the UART has finished
  shifting out, the RX buffer is flushed and the pad goes back to receive.

The module answers about 160 µs after the last stop bit. A converter has to be
back in receive by then, so keep the main loop free of other work.

## `crsf_hdtofd_usbcdc`: ESP32 Dev Module on its own USB port

The computer-facing port is the development board's USB connector, so no
separate USB-UART adapter is needed. On a classic ESP32 Dev Module that
connector is the board's on-board USB-to-UART bridge wired to UART0
(GPIO1/GPIO3); the computer sees an ordinary USB serial port.

| ESP32 Dev Module | ELRS transmitter module |
|---|---|
| GPIO4 | S.port |
| GND | GND |

Connect the board to the computer by USB. Do not connect GPIO1 or GPIO3
externally; the board already wires them to its USB bridge.

The USB serial stream is binary CRSF, so the sketch prints nothing: any text
on UART0 would corrupt the stream. The ESP32 ROM can still print boot text
when the board resets, so the computer-side program should discard input
received before it starts its CRSF session.

Tuning: 8 KB RX buffers; the UART wakes the driver after 16 bytes or after one
idle byte time (about 11 µs), so sustained traffic moves in batches and the
tail of a short frame is not held back.

Build with PlatformIO (`platformio.ini`: `espressif32@6.12.0`, board
`esp32dev`, Arduino framework):

```bash
cd crsf_hdtofd_usbcdc
pio run -t upload
```

or open the `.ino` in the Arduino IDE with the board "ESP32 Dev Module".

## `crsf_hdtofd`: classic ESP32 with an external USB-UART

The first version. The computer side is an external USB-UART adapter on UART2;
the board's own USB port stays free for diagnostics.

| ESP32 | Connects to |
|---|---|
| GPIO4 | module S.port |
| GPIO17 (UART2 TX) | adapter RX |
| GPIO16 (UART2 RX) | adapter TX |
| GND | module GND and adapter GND |

Once a second the sketch prints one line on the board's USB serial port at
921600: pad level, whether it owns the wire, edges per second from the module
(`ext_edges`) and from its own transmissions (`self_edges`), bytes in and out
on each side (`HD_IN`, `FD_IN`, `HD_OUT`, `FD_OUT`), read bursts, wire
takeovers (`TX_START`), releases (`TX_RELEASE`), sends deferred because the
bus was busy (`TX_DEFER`), and bytes waiting in each driver buffer. Set
`DEBUG_STATS = false` to silence it.

Build with the Arduino IDE, board "ESP32 Dev Module".

## `crsf_hd_fd_c3`: ESP32-C3

The same converter on an ESP32-C3, which has only two UARTs. UART0 becomes the
module side on GPIO3 through the GPIO matrix, and UART1 is the computer side
on GPIO21/GPIO20. GPIO20/21 are the C3's native UART0 pins, so the sketch
resets them and routes UART1 onto them explicitly.

| ESP32-C3 | Connects to |
|---|---|
| GPIO3 | module S.port |
| GPIO21 (UART1 TX) | adapter RX |
| GPIO20 (UART1 RX) | adapter TX |
| GND | module GND and adapter GND |

There is no debug output: both UARTs carry the bridge, and `Serial` would land
on one of them. Both UARTs wake the driver on every byte (RX threshold 1,
one-byte-time timeout) for the lowest latency.

Build with the Arduino IDE, board "ESP32C3 Dev Module".

## Using it

Point the computer-side program at the converter's serial port and baud, for
example crsfproxy with `--device /dev/ttyUSB0 --baud 921600` for the USB-port
variant. crsfproxy's README covers its options.