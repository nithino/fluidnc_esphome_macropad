# FluidNC ESPHome Macro Keypad
This piece of software is written for ESP32 Microcontroller, running ESPHome (https://github.com/esphome/esphome) to connect to FluidNC (https://github.com/bdring/FluidNC) firmware.  
**Please note that this is vibe-coded. Even though I tested this software, and it works, use it at your own risk.** 
A laser cutter, especially high powered ones are extremely dangerous. I have built safety controls into the system to reduce risks, but please exercise caution.  
A macro-keypad running FluidNC using GRBL serial line protocol to jog, and control a FluidNC-based laser.  
This software, along with diagrams, electronics suggestions etc. is provided "as is, without any warranty" Refer to the license for more details.  

  
3D Printed file for the Pendant (macro keypad) is posted here: LINK.  

# Features:
1. Jog controls - Left, Right, Top, Bottom
2. Home - Press and hold home button for 1 second.
3. Laser pulse - Fires laser beam as long as the button is pressed - Currently sets at 17.5% - sets as per your desired need.
4. Pause or Freehold - Only works during a job. Pressing it once pauses the job, and pressing the button again resumes the job.


# Safety Features:
1. Always sends M5 command once the laser button is released.
2. Only allows laser pulse in idle state (not in error, not while moving).
3. Keeps jog distance limited to avoid uncontrollable movement of the laser head if connection looses. Make sure to exercise this pattern.
4. Keypad becomes non-responsive in alarm state - you should home the laser head in such instances.

# Hardware
1. ESP32 MCU. I used ESP32 S2 Mini, but it should work for other ESP32 boards as well. Make sure to map your pins properly.
2. 7 Macro Keypad pins.
3. UART Connection with the main board running FluidNC.
4. Planned ---
     - Relay controls
     - OLED Display
     - Water temperature monitoring using DS18B20.
     - WLED status monitoring using WS2812B led strip.
     - LED Strip (white) controls.

# How to flash
Follow the sample yaml file in the repo. 

```yaml
api:
  reboot_timeout: 0s

logger:
  hardware_uart: USB_CDC
  level: DEBUG

external_components:
  - source:
      type: git
      url: https://github.com/nithino/fluidnc_esphome_macropad
      ref: main
    components:
      - fluidnc_pendant
  refresh: 0s

uart:
  id: fnc_uart
  tx_pin: GPIO4      # pendant TX -> FluidNC RX (gpio.4)
  rx_pin: GPIO2      # pendant RX <- FluidNC TX (gpio.0)
  baud_rate: 115200

fluidnc_pendant:
  uart_id: fnc_uart
  dry_run: false             # true = log "would send" lines but transmit nothing for buttons
  log_rx: false              # true = log every line received (status reports, ok)
  buttons:
    # ---- Button No. 1 ----
    - name: "Home"
      pin: GPIO9
      hold_time: 1s
      on_press: HOME
      require_state: [Idle, Alarm]
    # Remaining buttons here ---
```
This is the basic code structure and blocks. Refer to the full YAML file uploaded here.  

Additionally, you should create a UART configuration in your FluidNC firmware.  
In your FluidNC config.yaml:
```yaml
uart1:
  txd_pin: gpio.2
  rxd_pin: gpio.4
  baud: 115200
  mode: 8N1
  rts_pin: NO_PIN
uart_channel1:
  uart_num: 1
  report_interval_ms: 400
```

Setting Speed of Jogging:
```yaml
on_press: "jog X-9 F2000"
# Change F value to change feed rate. 
```
Setting Laser Power on test fire. 
```yaml
on_press: "M3 S170.5 G1 F100"
# Change S value (170.5 here), depending on your defined values in FluidNC Config. 
lockout: 1s
# Lock out time is the time delay between each press allowed. 
```
If you are using LightBurn, you could test safer limits by testing in the software for test firing laser. 
