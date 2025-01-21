/***************************************************************
 * COMPLETE EXAMPLE (IN ENGLISH)
 *
 * 1) Scans BLE for "WT-0001"
 * 2) Connects to service 0x1234
 *    - Write characteristic:  0x1235
 *    - Notify characteristic: 0x1236
 * 3) Sends BIND "FEFE03010200FF" right after connecting
 * 4) Sends QUERY (command FEFE03010200) every minute
 * 5) Receives responses via NOTIFY -> callback only stores data
 * 6) Decodes and displays the data in loop()
 * 
 * Based on https://github.com/klightspeed/BrassMonkeyFridgeMonitor
 ***************************************************************/

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Arduino.h>

/** -------------------------
 * CONFIGURATION
 * ------------------------- */

// The name advertised by the fridge's BLE module
#define TARGET_DEVICE_NAME "WT-0001"

// Service and characteristic UUIDs
static BLEUUID serviceUUID((uint16_t)0x1234);     // 0x1234
static BLEUUID charUUID_Write((uint16_t)0x1235);  // 0x1235 (Write)
static BLEUUID charUUID_Notify((uint16_t)0x1236); // 0x1236 (Notify)

// We will send a "query" command every 60 seconds
const unsigned long QUERY_INTERVAL_MS = 60000;

/** -------------------------
 * GLOBAL VARIABLES
 * ------------------------- */

BLEScan* pBLEScan;
BLEAddress* pServerAddress = nullptr;

bool doConnect = false;
bool connected = false;

BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pRemoteCharacteristicWrite = nullptr;
BLERemoteCharacteristic* pRemoteCharacteristicNotify = nullptr;

// Last time we sent a query
unsigned long lastQueryMillis = 0;

// Buffer for the last notification and a flag for "new data available"
static std::vector<uint8_t> g_lastNotificationData; 
static bool g_newDataAvailable = false;

/** --------------------------------------------------
 * Data structure for a single-zone fridge query result
 * (after decoding the BLE response).
 * -------------------------------------------------- */
struct FridgeStatus_t {
  bool locked;
  bool poweredOn;
  uint8_t runMode;     // 0=MAX, 1=ECO
  uint8_t batSaver;    // 0=Low, 1=Mid, 2=High
  int8_t  leftTarget;
  int8_t  tempMax;
  int8_t  tempMin;
  uint8_t leftRetDiff;
  uint8_t startDelay;
  uint8_t unit;        // 0 = Celsius, 1 = Fahrenheit
  int8_t  leftTCHot;
  int8_t  leftTCMid;
  int8_t  leftTCCold;
  int8_t  leftTCHalt;
  int8_t  leftCurrent;
  uint8_t batPercent;
  uint8_t batVolInt;
  uint8_t batVolDec;
};

/** --------------------------------------------------
 * Function: Calculates a simple checksum for standard
 * "FE FE" frames.
 * -------------------------------------------------- */
static uint16_t calculateChecksum(const uint8_t* buf, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum += buf[i];
  }
  return (uint16_t)(sum & 0xFFFF);
}

/** --------------------------------------------------
 * Function: Decodes a "query response" frame (0x01)
 *   FE FE [length] [0x01] [payload] [2-byte checksum]
 * For a single-zone fridge (18 bytes payload).
 * -------------------------------------------------- */
bool decodeFridgeQuerySingleZone(const uint8_t* data, size_t length, FridgeStatus_t &status) {
  // Minimum length ~24 bytes: FE FE + length + code + 18 payload + 2 checksum
  if (length < 24) return false;
  if (data[0] != 0xFE || data[1] != 0xFE) return false;

  uint8_t declaredLen = data[2];
  uint8_t cmd = data[3];
  if (cmd != 0x01) {
    // Not a "query response" frame
    return false;
  }

  // Last 2 bytes = checksum
  if (length < 24) return false;
  uint16_t offsetSum = length - 2;
  uint16_t sumPacket = (data[offsetSum] << 8) | data[offsetSum + 1];
  uint16_t sumCalc = calculateChecksum(data, offsetSum);
  if (sumCalc != sumPacket) {
    // Checksum mismatch
    return false;
  }

  // Payload: bytes 4..21 (18 bytes)
  const uint8_t* payload = &data[4];

  status.locked       = (payload[0] == 1);
  status.poweredOn    = (payload[1] == 1);
  status.runMode      = payload[2];
  status.batSaver     = payload[3];
  status.leftTarget   = (int8_t)payload[4];
  status.tempMax      = (int8_t)payload[5];
  status.tempMin      = (int8_t)payload[6];
  status.leftRetDiff  = payload[7];
  status.startDelay   = payload[8];
  status.unit         = payload[9];
  status.leftTCHot    = (int8_t)payload[10];
  status.leftTCMid    = (int8_t)payload[11];
  status.leftTCCold   = (int8_t)payload[12];
  status.leftTCHalt   = (int8_t)payload[13];
  status.leftCurrent  = (int8_t)payload[14];
  status.batPercent   = payload[15];
  status.batVolInt    = payload[16];
  status.batVolDec    = payload[17];

  return true;
}

/** --------------------------------------------------
 * Function: buildQueryCommand
 *   In my scenario: FEFE03010200
 * -------------------------------------------------- */
void buildQueryCommand(std::vector<uint8_t> &packet) {
  // Using the literal bytes: FE FE 03 01 02 00
  packet.clear();
  packet.push_back(0xFE);
  packet.push_back(0xFE);
  packet.push_back(0x03);
  packet.push_back(0x01);
  packet.push_back(0x02);
  packet.push_back(0x00);
}

/** --------------------------------------------------
 * Function: buildBindCommand
 *   In my scenario: "FEFE03010200FF"
 * -------------------------------------------------- */
void buildBindCommand(std::vector<uint8_t> &packet) {
  // Using the literal bytes: FE FE 03 01 02 00 FF
  packet.clear();
  packet.push_back(0xFE);
  packet.push_back(0xFE);
  packet.push_back(0x03);
  packet.push_back(0x01);
  packet.push_back(0x02);
  packet.push_back(0x00);
  packet.push_back(0xFF);
}

/** --------------------------------------------------
 * NOTIFY CALLBACK:
 *  Only saves the raw data into a global buffer
 *  and sets a "new data" flag.
 * -------------------------------------------------- */
static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) 
{
  // Clear previous data
  g_lastNotificationData.clear();
  
  // Copy new bytes
  for (size_t i = 0; i < length; i++) {
    g_lastNotificationData.push_back(pData[i]);
  }
  
  // Flag that new data is available
  g_newDataAvailable = true;
}

/** --------------------------------------------------
 * BLE SCAN CALLBACK
 * -------------------------------------------------- */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.print("Found device: ");
    Serial.println(advertisedDevice.toString().c_str());
    
    if (advertisedDevice.haveName() && advertisedDevice.getName() == TARGET_DEVICE_NAME) {
      Serial.println("-> This is our fridge, stopping scan and connecting...");
      pServerAddress = new BLEAddress(advertisedDevice.getAddress());
      doConnect = true;
      pBLEScan->stop();
    }
  }
};

/** --------------------------------------------------
 * BLE CLIENT CALLBACK
 * -------------------------------------------------- */
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    Serial.println("[BLEClient] Connected to BLE server");
  }
  void onDisconnect(BLEClient* pclient) {
    Serial.println("[BLEClient] Disconnected from BLE server");
    connected = false;
  }
};

/** --------------------------------------------------
 * connectToServer:
 *  - Connects to the BLE server
 *  - Finds service 0x1234
 *  - Finds char Write=0x1235, char Notify=0x1236
 *  - Registers notify
 *  - Sends BIND (FEFE03010200FF)
 * -------------------------------------------------- */
bool connectToServer(BLEAddress pAddress) {
  Serial.print("Connecting to: ");
  Serial.println(pAddress.toString().c_str());

  pClient = BLEDevice::createClient();
  Serial.println("-> Created BLE client");
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(pAddress)) {
    Serial.println("-> Connection failed");
    return false;
  }
  Serial.println("-> Connected to BLE server");

  // Find service 0x1234
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.println("-> Service 0x1234 not found");
    pClient->disconnect();
    return false;
  }
  Serial.println("-> Found service 0x1234");

  // Find characteristic Write=0x1235
  pRemoteCharacteristicWrite = pRemoteService->getCharacteristic(charUUID_Write);
  if (pRemoteCharacteristicWrite == nullptr) {
    Serial.println("-> Characteristic 0x1235 not found");
    pClient->disconnect();
    return false;
  }
  Serial.println("-> Found Write characteristic (0x1235)");

  // Find characteristic Notify=0x1236
  pRemoteCharacteristicNotify = pRemoteService->getCharacteristic(charUUID_Notify);
  if (pRemoteCharacteristicNotify == nullptr) {
    Serial.println("-> Characteristic 0x1236 not found");
    pClient->disconnect();
    return false;
  }
  Serial.println("-> Found Notify characteristic (0x1236)");

  // Register notify callback
  if (pRemoteCharacteristicNotify->canNotify()) {
    pRemoteCharacteristicNotify->registerForNotify(notifyCallback);
    Serial.println("-> Notify callback set");
  } else {
    Serial.println("-> WARNING: 0x1236 does not support NOTIFY!");
  }

  connected = true;

  // Send BIND command
  {
    std::vector<uint8_t> bindCmd;
    buildBindCommand(bindCmd);
    Serial.println("[BIND] Sending FEFE03010200FF...");
    pRemoteCharacteristicWrite->writeValue(bindCmd.data(), bindCmd.size(), false);
  }

  // Prepare to send the first query soon
  lastQueryMillis = millis() - QUERY_INTERVAL_MS;
  
  return true;
}

/** --------------------------------------------------
 * setup()
 * -------------------------------------------------- */
void setup() {
  Serial.begin(115200);
  Serial.println("----- [Start] Alpicool BLE Client (English) -----");

  BLEDevice::init("ESP32-Alpicool-Client");

  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

/** --------------------------------------------------
 * loop()
 * -------------------------------------------------- */
void loop() {
  // 1) If not connected and not set to connect -> Scan for 5s
  if (!connected && !doConnect) {
    Serial.println("[SCAN] Starting BLE scan (5s)...");
    pBLEScan->start(5);
    pBLEScan->clearResults();
  }

  // 2) If doConnect -> connect to the server
  if (doConnect) {
    connectToServer(*pServerAddress);
    doConnect = false;
  }

  // 3) If connected, send a "query" every minute
  if (connected && pRemoteCharacteristicWrite != nullptr) {
    unsigned long now = millis();
    if (now - lastQueryMillis >= QUERY_INTERVAL_MS) {
      lastQueryMillis = now;

      std::vector<uint8_t> queryCmd;
      buildQueryCommand(queryCmd);

      Serial.println("[QUERY] Sending command  (query)...");
      pRemoteCharacteristicWrite->writeValue(queryCmd.data(), queryCmd.size(), false);
    }
  }

  // 4) If new notification data has arrived, decode and display it here
  if (g_newDataAvailable) {
    g_newDataAvailable = false; // reset the flag

    Serial.println("[LOOP] New notification data received. Decoding...");

    FridgeStatus_t st;
    bool ok = decodeFridgeQuerySingleZone(g_lastNotificationData.data(), g_lastNotificationData.size(), st);

    if (!ok) {
      Serial.print("[DECODE] Error decoding or not a query response. Raw bytes: ");
      for (size_t i = 0; i < g_lastNotificationData.size(); i++) {
        Serial.printf("%02X ", g_lastNotificationData[i]);
      }
      Serial.println();
    } else {
      // Display the decoded fridge status in a human-readable form
      Serial.println("[DECODE] Single-zone fridge status:");

      // locked / poweredOn
      Serial.print(" -> locked: ");
      Serial.println(st.locked ? "YES" : "NO");

      Serial.print(" -> poweredOn: ");
      Serial.println(st.poweredOn ? "ON" : "OFF");

      // runMode (0=MAX, 1=ECO)
      String runModeStr = "UNKNOWN";
      if (st.runMode == 0) runModeStr = "MAX";
      else if (st.runMode == 1) runModeStr = "ECO";
      Serial.print(" -> runMode: ");
      Serial.println(runModeStr);

      // batSaver (0=Low,1=Mid,2=High)
      String saverStr = "Unknown";
      if (st.batSaver == 0) saverStr = "Low";
      if (st.batSaver == 1) saverStr = "Mid";
      if (st.batSaver == 2) saverStr = "High";
      Serial.print(" -> batSaver: ");
      Serial.println(saverStr);

      // Temperature unit
      String tempUnit = (st.unit == 0) ? "°C" : "°F";

      Serial.print(" -> leftTarget: ");
      Serial.print(st.leftTarget);
      Serial.println(tempUnit);

      Serial.print(" -> leftCurrent: ");
      Serial.print(st.leftCurrent);
      Serial.println(tempUnit);

      Serial.print(" -> batPercent: ");
      Serial.print(st.batPercent);
      Serial.println("%");

      // battery voltage: assuming batVolDec is tenths
      float batVoltage = st.batVolInt + (st.batVolDec / 10.0f);
      Serial.print(" -> batVoltage: ");
      Serial.print(batVoltage, 2);
      Serial.println(" V");
    }
  }

  delay(100);
}
