/*
 * RFID Access Control System with Web Interface
 * 
 * Features:
 * - Non-blocking RFID card scanning
 * - Web interface for card management (add/delete/toggle)
 * - GPIO output activation for valid cards
 * - Secure file operations with error handling
 * - Buffer overflow protection
 * 
 * Pin Configuration:
 * - PN532 RFID (UART2): RX=16, TX=17
 * - W5500 Ethernet: SCK=18, MISO=19, MOSI=23, CS=5
 * - Output Pin: GPIO4 (Changeable, must be output-capable)
 * 
 * Network:
 * - Static IP: 192.168.1.177 (configurable)
 * - MAC: DE:AD:BE:EF:FE:ED (unique per device)
 */

#include <SPI.h>
#include <Ethernet.h>
#include "FS.h"
#include "SPIFFS.h"
#include <Adafruit_PN532.h>
#include <Wire.h>

// Configuration Constants
#define MAX_ROWS 200
#define MAX_REQUEST_LENGTH 512
#define OUTPUT_PIN 4         // Using GPIO4 which is a valid output pin
#define RFID_CHECK_INTERVAL 100  // ms between RFID checks

String fileRows[MAX_ROWS];
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 177);
EthernetServer server(80);

HardwareSerial PN532Serial(2);
Adafruit_PN532 nfc(PN532Serial);

String String_ID = "";
const char* dataPath = "/data.txt";

// Web server variables
char currentRequest[MAX_REQUEST_LENGTH];
unsigned currentRequestPos = 0;
EthernetClient client;
unsigned long lastClientCheck = 0;
const unsigned long clientTimeout = 1000;

// RFID timing variables
unsigned long lastRfidCheck = 0;

void setup() {
  Serial.begin(115200);
  
  // Initialize output pin
  pinMode(OUTPUT_PIN, OUTPUT);
  digitalWrite(OUTPUT_PIN, LOW);

  // Initialize RFID reader
  PN532Serial.begin(115200, SERIAL_8N1, 16, 17);
  if (!nfc.begin()) {
    Serial.println("❌ Failed to initialize PN532");
    while (1);
  }
  
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("❌ Didn't find PN532");
    while (1);
  }
  
  nfc.SAMConfig();
  Serial.println("✅ PN532 Ready");

  // Initialize filesystem
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ Failed to mount SPIFFS");
    while (1);
  }

  // Initialize network
  SPI.begin(18, 19, 23, 5);
  Ethernet.init(5);
  
  if (Ethernet.begin(mac) == 0) {
    Serial.println("⚠️ Failed to configure Ethernet using DHCP");
    // Fall back to static IP
    Ethernet.begin(mac, ip);
  }
  
  server.begin();
  Serial.print("💻 Server started at ");
  Serial.println(Ethernet.localIP());

  // Initialize data file
  if (!SPIFFS.exists(dataPath)) {
    File f = SPIFFS.open(dataPath, FILE_WRITE);
    if (!f) {
      Serial.println("❌ Failed to create data file");
    } else {
      f.close();
    }
  }

  // Load initial data
  if (!readFile(SPIFFS, dataPath, fileRows, MAX_ROWS)) {
    Serial.println("⚠️ Error reading initial data");
  }
}

void loop() {
  handleNetwork();
  handleRFID();
}

void handleNetwork() {
  // Handle client connections with timeout
  if (!client || !client.connected()) {
    client = server.available();
    currentRequestPos = 0;
    memset(currentRequest, 0, MAX_REQUEST_LENGTH);
    lastClientCheck = millis();
    return;
  }

  // Read client data with buffer protection
  while (client.available() && currentRequestPos < MAX_REQUEST_LENGTH - 1) {
    char c = client.read();
    currentRequest[currentRequestPos++] = c;

    // Check for end of request
    if (currentRequestPos >= 4 && 
        strncmp(&currentRequest[currentRequestPos-4], "\r\n\r\n", 4) == 0) {
      handleRequest(client, String(currentRequest));
      client.stop();
      return;
    }
  }

  // Client timeout
  if (millis() - lastClientCheck > clientTimeout) {
    client.stop();
    Serial.println("⌛ Client timeout");
  }
}

void handleRFID() {
  // Non-blocking RFID check
  if (millis() - lastRfidCheck > RFID_CHECK_INTERVAL) {
    lastRfidCheck = millis();
    
    uint8_t uid[7];
    uint8_t uidLength;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 0)) {
      Serial.print("📇 Card detected: ");
      ConvertByteToString(uid, uidLength);
      Serial.println(String_ID);
      
      if (checkCardAccess(String_ID)) {
        Serial.println("✅ Access granted - Activating output");
        digitalWrite(OUTPUT_PIN, HIGH);
        delay(1000);
        digitalWrite(OUTPUT_PIN, LOW);
      } else {
        Serial.println("❌ Access denied");
      }
    }
  }
}

// Improved UID conversion that handles variable length
void ConvertByteToString(byte *ID, uint8_t length) {
  String_ID = "";
  for (uint8_t i = 0; i < length; i++) {
    if (ID[i] < 0x10) String_ID += "0";
    String_ID += String(ID[i], HEX);
  }
  String_ID.toUpperCase();
}

// Safe file reading with error handling
bool readFile(fs::FS &fs, const char *path, String *Return, int arrayLength) {
  File file = fs.open(path);
  if (!file || file.isDirectory()) {
    Serial.println("❌ Failed to open file for reading");
    return false;
  }

  try {
    int i = 0;
    while (file.available() && i < arrayLength) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        Return[i++] = line;
      }
    }
    file.close();
    return true;
  } catch (...) {
    Serial.println("⚠️ Exception while reading file");
    file.close();
    return false;
  }
}

// [Rest of your existing functions (handleRequest, sendHTML, etc.) 
// with similar error handling improvements...]

// Example of improved file writing with error handling
bool writeData(String id, String nama, String unit, String enable) {
  String data = id + "|" + nama + "|" + unit + "|" + enable;
  
  File file = SPIFFS.open(dataPath, FILE_APPEND);
  if (!file) {
    Serial.println("❌ Failed to open file for writing");
    return false;
  }

  try {
    file.println(data);
    file.close();
    
    // Update in-memory data
    for (int i = 0; i < MAX_ROWS; i++) {
      if (fileRows[i].length() == 0) {
        fileRows[i] = data;
        break;
      }
    }
    return true;
  } catch (...) {
    Serial.println("⚠️ Exception while writing file");
    file.close();
    return false;
  }
}
