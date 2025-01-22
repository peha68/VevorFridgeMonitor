ESP32 BLE Client for Alpicool / Vevor / WT-0001 Car Fridge

ESP32 BLE Client for Alpicool / Vevor / WT-0001 Car Fridge
==================================================

This project demonstrates how to **scan**, **bind**, **query**, and **decode status** from an Alpicool / Vevor / WT-0001 BLE-enabled car fridge (also sold under various rebrands and different names). It uses an ESP32 running Arduino or PlatformIO to communicate with the fridge via Bluetooth Low Energy (BLE).

> **Note**: Much of the reverse-engineering details about the BLE protocol and data structures come from [klightspeed/BrassMonkeyFridgeMonitor](https://github.com/klightspeed/BrassMonkeyFridgeMonitor). Big thanks to that project for providing the technical reference!

Features
--------

*   **Auto-scan** for a BLE device named `WT-0001`
*   **Bind command** (`FEFE03010200FF`) to associate with the fridge (sometimes optional, but included in case your fridge requires it)
*   **Query command** (`FEFE03010200` or a variant with a checksum, depending on your firmware) sent **every 60 seconds** to retrieve current temperature, battery status, etc.
*   **Notify callback** to receive and decode the fridge’s status
*   **Example** decoding of locked state, power state, run mode (ECO/MAX), target temperature, battery voltage, etc.

Getting Started
---------------

### Hardware & Software Requirements

*   **ESP32 development board** (e.g., ESP32 DevKitC, NodeMCU-32S, etc.)
*   **Arduino IDE** or [PlatformIO](https://platformio.org/) in Visual Studio Code
*   A **WT-0001** (Alpicool / Vevor / or rebranded) BLE fridge that advertises its name as `WT-0001` or other (You have to find)
*   A working **BLE** environment (ESP32 firmware supports BLEClient)

### Cloning and Building

1.  **Clone this repository** or download the ZIP:
    
        git clone https://github.com/peha68/VevorFridgeMonitor.git
        cd VevorFridgeMonitor
                    
    
2.  **Open** it in the Arduino IDE or VS Code (PlatformIO).
3.  **Select** the ESP32 board in Arduino IDE or set `board = esp32dev` in `platformio.ini`.
4.  **Compile and upload** the sketch to your ESP32.

BLE Process Overview
--------------------

1.  **Scan**: The code looks for a device named `WT-0001` in my case.
2.  **Connect**: Once found, it connects to the fridge’s BLE service (`0x1234`).
3.  **Bind**: Sends the bind command (`FEFE03010200FF`) which may or may not be required by your fridge’s firmware.
4.  **Query**: Sends the query command (`FEFE03010200`) **every 60 seconds**, receiving status notifications in response.
5.  **Decode**: The notification callback saves the raw bytes, and the main loop decodes them into fields like locked, run mode, current temperature, battery level, etc.

How to Query the Fridge Manually
--------------------------------

The code already sends a _query_ once per minute. If you need to trigger an extra query:

1.  Call the function to build the query packet (e.g., `buildQueryCommand()`).
2.  Write that packet to the fridge’s Write characteristic (`0x1235`).
3.  Wait for notifications on characteristic `0x1236`.
4.  Decode the returned data in the callback or main loop.

> Different firmware versions may require a slightly different command format or a checksum.

Changing Fridge Settings
------------------------

Beyond querying, many Alpicool-like / Vevor fridges can be controlled by sending specific “Set” commands:

*   **Set Left** (0x05) – set the target temperature for the main (left) zone. Use this for one zone fridges
*   **Set Right** (0x06) – for dual-zone fridges
*   Possibly `0x02` (“setOther”) or `0x04` (“reset\`) for advanced control

Each command typically has a format like:

    FE FE [length] [0x05 / 0x06] [payload with new temp, lock state, run mode, etc.] [2-byte checksum?]
        

Check the [BrassMonkeyFridgeMonitor docs](https://github.com/klightspeed/BrassMonkeyFridgeMonitor) for the structure. You’d create a `buildSetCommand()` similar to `buildQueryCommand()` and write that to `0x1235`.

References
----------

*   [klightspeed/BrassMonkeyFridgeMonitor](https://github.com/klightspeed/BrassMonkeyFridgeMonitor)  
    This repository provided most of the reverse-engineered BLE protocol details for the WT-0001 fridge (bind command, query, data structures, etc.).
*   [ESP32 BLE Arduino](https://github.com/espressif/arduino-esp32/tree/master/libraries/BLE)  
    The Arduino BLE library used for scanning, connecting, reading, and writing characteristics.