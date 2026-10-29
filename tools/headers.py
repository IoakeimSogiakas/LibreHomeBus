HOST = """\
esphome:
  name: {name}

host:
  mac_address: "02:A1:B2:C3:D4:E5"

logger:

api:

uart:
  id: uart_bus
  port: {port}
  baud_rate: 115200
  rx_buffer_size: 256

modbus:
  id: modbus_bus
  uart_id: uart_bus
  send_wait_time: {send_wait}
  turnaround_time: {turnaround}
"""

# Ethernet settings are placeholders -- verify against the WT32-ETH01 pinout.
WT32 = """\
esphome:
  name: {name}

esp32:
  board: esp32dev
  framework:
    type: esp-idf

logger:

api:

ota:
  - platform: esphome

ethernet:
  type: LAN8720
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk_mode: GPIO0_IN
  phy_addr: 1
  power_pin: GPIO16

uart:
  id: uart_bus
  tx_pin: GPIO17
  rx_pin: GPIO32
  flow_control_pin: GPIO33
  baud_rate: 115200
  rx_buffer_size: 256
  rx_timeout: 15

modbus:
  id: modbus_bus
  uart_id: uart_bus
  send_wait_time: {send_wait}
  turnaround_time: {turnaround}
"""

S3ZERO = """\
esphome:
  name: {name}
  friendly_name: {name}
  on_boot:
    priority: -100
    then:
      - light.turn_on:
          id: board_led
          brightness: 60%
          red: 100%
          green: 0%
          blue: 0%

esp32:
  variant: esp32s3
  flash_size: 4MB
  framework:
    type: esp-idf

logger:

api:

ota:
  - platform: esphome
    encryption:

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  fast_connect: true
  ap:
    ssid: {name} Fallback Hotspot
  on_connect:
    - light.turn_on:
        id: board_led
        brightness: 60%
        red: 0%
        green: 0%
        blue: 100%
  on_disconnect:
    - light.turn_on:
        id: board_led
        brightness: 60%
        red: 100%
        green: 0%
        blue: 0%

captive_portal:

uart:
  - id: uart_bus
    baud_rate: 115200
    rx_pin: 44
    tx_pin: 43
    rx_buffer_size: 256

modbus:
  - id: modbus_bus
    uart_id: uart_bus
    flow_control_pin: GPIO6
    send_wait_time: {send_wait}
    turnaround_time: {turnaround}
"""

BOARD_LED_S3ZERO = """\
  - platform: esp32_rmt_led_strip
    id: board_led
    pin: GPIO21
    num_leds: 1
    chipset: WS2812
    rgb_order: RGB
    name: "Board LED"
"""

ALL = {
    "host":   HOST,
    "wt32":   WT32,
    "s3zero": S3ZERO,
}
