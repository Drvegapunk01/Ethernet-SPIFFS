#include <SPI.h>              // Library komunikasi SPI
#include <Ethernet.h>         // Library Ethernet untuk koneksi jaringan (W5500)
#include "FS.h"               // File System (abstraksi)
#include "SPIFFS.h"           // File system SPIFFS untuk menyimpan file di flash
#include <Adafruit_PN532.h>   // Library untuk modul RFID PN532
#include <Wire.h>             // Library I2C (tidak digunakan di sini, tapi biasa default untuk PN532)

#define MAX_ROWS 200          // Batas maksimum baris yang akan dibaca dari file
String fileRows[MAX_ROWS];    // Array untuk menyimpan baris-baris file

// Konfigurasi alamat MAC dan IP statis untuk W5500
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 177);     // IP statis ESP32
EthernetServer server(80);         // Membuat server HTTP di port 80

// Konfigurasi komunikasi RFID PN532 menggunakan UART2 (GPIO 16 dan 17)
HardwareSerial PN532Serial(2);      // UART2 pada ESP32
Adafruit_PN532 nfc(PN532Serial);    // Inisialisasi objek PN532

String String_ID = "";              // Variabel untuk menyimpan hasil ID kartu RFID

// Konfigurasi pin SPI untuk W5500 (disesuaikan dengan board ESP32 kamu)
#define PIN_SCK  18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_CS   5

// Pin GPIO untuk output
#define OUTPUT_PIN 36

// Path file di SPIFFS
const char* dataPath = "/data.txt";

// Variabel tambahan untuk web server
String currentRequest = "";                   // Menyimpan request HTTP dari client
EthernetClient client;                        // Objek client Ethernet
unsigned long lastClientCheck = 0;            // Timer untuk mendeteksi timeout client
const unsigned long clientTimeout = 1000;     // Timeout client (ms)

// ==== Deklarasi fungsi tambahan ====
void handleRequest(EthernetClient &client, String request);
void sendHTML(EthernetClient &client, String msg = "");
void writeData(String id, String nama, String unit, String enable);
void eraseAllData();
void deleteRowById(String targetId);
String urlDecode(String input);
void ConvertByteToString(byte *ID);
void readFile(fs::FS &fs, const char *path, String *Return, int arrayLength);
void printFileRows(String *rows, int maxRows);
String generateHTMLTable();
String escapeHTML(String input);
void toggleEnable(String targetId);
bool checkCardAccess(String cardId);

// ==== Fungsi setup ====
void setup() {
  Serial.begin(115200);                       // Memulai komunikasi Serial
  PN532Serial.begin(115200, SERIAL_8N1, 16, 17); // Inisialisasi UART2 untuk PN532
  nfc.begin();                                // Mulai komunikasi PN532

  // Konfigurasi GPIO 36 sebagai output
  pinMode(OUTPUT_PIN, OUTPUT);
  digitalWrite(OUTPUT_PIN, LOW); // Pastikan awal dalam keadaan LOW

  // Cek versi firmware PN532
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("❌ Tidak menemukan PN532");
    while (1); // Stop program
  }
  Serial.print("✅ PN532 Ditemukan, Firmware versi: 0x");
  Serial.println(versiondata, HEX);
  nfc.SAMConfig();                            // Konfigurasi secure access mode
  Serial.println("Scan kartu NFC...");

  // Inisialisasi SPI untuk Ethernet W5500
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);                     // Tentukan pin CS Ethernet

  // Mount SPIFFS filesystem
  if (!SPIFFS.begin(true)) {
    Serial.println("Gagal mount SPIFFS");
    return;
  }

  // Inisialisasi koneksi Ethernet dengan IP statis
  Ethernet.begin(mac, ip);
  delay(1000);
  server.begin();                             // Mulai server
  Serial.print("Server aktif di: ");
  Serial.println(Ethernet.localIP());

  // Jika file belum ada, buat file kosong
  if (!SPIFFS.exists(dataPath)) {
    File f = SPIFFS.open(dataPath, FILE_WRITE);
    f.close();
  }

  // Baca isi file ke array dan tampilkan ke Serial
  readFile(SPIFFS, dataPath, fileRows, MAX_ROWS);
  printFileRows(fileRows, MAX_ROWS);
}

// ==== Fungsi utama loop ====
void loop() {
  // Cek apakah client terhubung
  if (!client || !client.connected()) {
    client = server.available();         // Cek koneksi baru
    currentRequest = "";
    lastClientCheck = millis();          // Simpan waktu koneksi
    return;
  }

  // Menerima data dari client
  while (client.available()) {
    char c = client.read();
    currentRequest += c;

    // Cek apakah request HTTP selesai (diakhiri \r\n\r\n)
    if (currentRequest.endsWith("\r\n\r\n")) {
      handleRequest(client, currentRequest); // Proses permintaan HTTP
      client.stop();                         // Tutup koneksi
      return;
    }
  }

  // Jika tidak ada data dari client selama batas waktu, putuskan koneksi
  if (millis() - lastClientCheck > clientTimeout) {
    client.stop();
    Serial.println("Client timeout");
  }

  // Cek apakah ada kartu RFID yang terbaca
  uint8_t uid[7];
  uint8_t uidLength;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    Serial.print("UID Tag: ");
    for (uint8_t i = 0; i < uidLength; i++) {
      Serial.print(uid[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    ConvertByteToString(uid); // Simpan hasil ke String_ID
    
    // Cek apakah kartu terdaftar dan aktif
    if (checkCardAccess(String_ID)) {
      digitalWrite(OUTPUT_PIN, HIGH); // Aktifkan GPIO 36
      Serial.println("Kartu terdaftar dan aktif - GPIO 36 HIGH");
      delay(1000); // Tahan HIGH selama 1 detik
      digitalWrite(OUTPUT_PIN, LOW); // Matikan GPIO 36
      Serial.println("GPIO 36 LOW");
    } else {
      Serial.println("Kartu tidak terdaftar atau tidak aktif");
    }
  }
}

// ==== Fungsi: Cek apakah kartu terdaftar dan aktif ====
bool checkCardAccess(String cardId) {
  for (int i = 0; i < MAX_ROWS; i++) {
    if (fileRows[i].length() > 0) {
      // Format data: ID|Nama|Unit|Enable
      int firstPipe = fileRows[i].indexOf('|');
      if (firstPipe != -1) {
        String id = fileRows[i].substring(0, firstPipe);
        if (id == cardId) {
          // Cek status enable
          int thirdPipe = fileRows[i].lastIndexOf('|');
          if (thirdPipe != -1) {
            String enable = fileRows[i].substring(thirdPipe + 1);
            return (enable == "1");
          }
        }
      }
    }
  }
  return false;
}

