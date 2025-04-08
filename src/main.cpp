// By Fatih Nurrobi Alansshori Teknik Komputer UPI
#include <Arduino.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <RTClib.h>
#include <Firebase_ESP_Client.h>
#include <ModbusMaster.h>
#include <HardwareSerial.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Firebase
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// Konfigurasi RS485 untuk ESP32-S3 dengan RS485 terintegrasi
#define RS485_RX 18  // Pin RX untuk RS485
#define RS485_TX 17  // Pin TX untuk RS485
#define RS485_DE 16  // Pin DE/RE untuk RS485

#define RELAY1 1
#define RELAY2 2
#define RELAY3 41
#define RELAY4 42
#define RELAY5 45
#define RELAY6 46

// Tentukan pin I2C secara eksplisit
#define SDA_PIN 8
#define SCL_PIN 9

// Firebase credentials
#define API_KEY "AIzaSyASs8IMEdH5s-ne-W7zVQ7nY4Bl9VbQgEE"
#define DATABASE_URL "https://silagung-default-rtdb.asia-southeast1.firebasedatabase.app"

// Gunakan HardwareSerial untuk ESP32
HardwareSerial rs485Serial(1); // Gunakan Serial1
ModbusMaster node;  // Buat objek Modbus

// Inisialisasi objek Firebase
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// Inisialisasi objek RTC
RTC_DS3231 rtc;
char daysOfTheWeek[7][12] = {"Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"};

// Tambahkan variabel global ini di bagian atas file, setelah deklarasi objek-objek
float vfdFrequency = 50.0; // Frekuensi tetap 50 Hz
bool vfdRunning = false;   // Status VFD

// Tambahkan variabel untuk status relay
bool relay_status[6] = {false, false, false, false, false, false};

// Fungsi preTransmission dan postTransmission
void preTransmission() {
  digitalWrite(RS485_DE, HIGH);
  delay(2);
}

void postTransmission() {
  digitalWrite(RS485_DE, LOW);
}

// Task VFD
void VFDTask(void * parameter) {
  // Inisialisasi RS485 dengan HardwareSerial
  rs485Serial.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  
  // Setup pin DE/RE untuk RS485
  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);
  
  // Inisialisasi Modbus
  node.begin(1, rs485Serial);  // Slave ID = 1
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  
  Serial.println("VFD Task Started");
  
  while(1) {
    if (vfdRunning) {
      // Sequence untuk menjalankan VFD
      uint8_t result;
      result = node.writeSingleRegister(8501, 6);  // Pre-charge
      delay(100);
      result = node.writeSingleRegister(8501, 7);  // Start
      delay(100);
      
      // Set frekuensi (50 Hz * 10 = 500)
      result = node.writeSingleRegister(8502, (uint16_t)(vfdFrequency * 10));
      delay(100);
      
      result = node.writeSingleRegister(8501, 15);  // Forward run
      
      if (result == node.ku8MBSuccess) {
        Serial.println("VFD berhasil dijalankan pada " + String(vfdFrequency) + " Hz");
      } else {
        Serial.println("Gagal menjalankan VFD!");
      }
      
      vfdRunning = false; // Reset flag
    }
    
    // Baca status VFD setiap 2 detik
    uint8_t result = node.readHoldingRegisters(8503, 1);
    if (result == node.ku8MBSuccess) {
      int16_t freq = node.getResponseBuffer(0);
      float actualFreq = freq / 10.0;
      Serial.print("Frekuensi aktual: ");
      Serial.print(actualFreq);
      Serial.println(" Hz");
    }
    
    vTaskDelay(pdMS_TO_TICKS(2000)); // Delay 2 detik
  }
}

String formatDateTime(const DateTime &dt) {
  char buffer[25];
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
          dt.year(), dt.month(), dt.day(),
          dt.hour(), dt.minute(), dt.second());
  return String(buffer);
}

//fungsi fungsi
void isibak();
void mixing();
void supply();
void relayoff();

// Fungsi untuk mengontrol relay individual
void controlRelay(int relay, bool state) {
  if(relay >= 1 && relay <= 6) {
    digitalWrite(relay == 1 ? RELAY1 : 
                relay == 2 ? RELAY2 : 
                relay == 3 ? RELAY3 : 
                relay == 4 ? RELAY4 : 
                relay == 5 ? RELAY5 : RELAY6, 
                state ? HIGH : LOW);
    relay_status[relay-1] = state;
  }
}

void setup(){
  Serial.begin(115200);
  Serial.println("Silagung Controller Starting...");

  // Inisialisasi pin relay
  pinMode(RELAY1, OUTPUT);  
  pinMode(RELAY2, OUTPUT);
  pinMode(RELAY3, OUTPUT);
  pinMode(RELAY4, OUTPUT);
  pinMode(RELAY5, OUTPUT);
  pinMode(RELAY6, OUTPUT);

  // Matikan semua relay pada awalnya

  // Inisialisasi I2C dengan pin yang ditentukan
  Wire.begin(SDA_PIN, SCL_PIN);

  // Inisialisasi RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  // Cek apakah RTC kehilangan daya
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting default time!");
    // Atur waktu default jika RTC kehilangan daya
    rtc.adjust(DateTime(2025, 4, 8, 10, 11, 0));
  }

  // Tampilkan waktu saat ini
  DateTime now = rtc.now();
  Serial.print("Waktu saat ini: ");
  Serial.println(formatDateTime(now));
  Serial.print("Hari: ");
  Serial.println(daysOfTheWeek[now.dayOfTheWeek()]);
  Serial.print("Suhu: ");

  xTaskCreate(
    VFDTask,     // Fungsi task
    "VFDTask",       // Nama task
    4096,                   // Stack size
    NULL,                   // Parameter
    1,                      // Prioritas
    NULL                    // Task handle
  );
  
  
}

void loop() {
  // Cek koneksi WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi terputus, mencoba menghubungkan kembali...");
    WiFiManager wm;
    
    // Atur timeout
    wm.setConfigPortalTimeout(180);
    
    // Atur nama AP dan password
    bool res = wm.autoConnect("Silagung", "admin123");
    
    if (res) {
      Serial.println("WiFi connected successfully");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      
      // Tunggu sebentar sebelum inisialisasi Firebase
      delay(2000);
      
      // Inisialisasi Firebase
      Serial.println("Initializing Firebase...");
      
      // Konfigurasi Firebase
      config.api_key = API_KEY;
      config.database_url = DATABASE_URL;
      
      Serial.println("Firebase API Key: " + String(API_KEY));
      Serial.println("Firebase Database URL: " + String(DATABASE_URL));
      
      // Autentikasi anonim
      Serial.println("Attempting Firebase signup...");
      if (Firebase.signUp(&config, &auth, "", "")) {
        Serial.println("Firebase signup OK");
        signupOK = true;
      } else {
        Serial.println("Firebase signup failed");
        if (config.signer.signupError.message.length() > 0) {
          Serial.print("Reason: ");
          Serial.println(config.signer.signupError.message.c_str());
        }
      }
      
      // Callback untuk token
      config.token_status_callback = tokenStatusCallback;
      
      // Mulai koneksi Firebase
      Serial.println("Starting Firebase connection...");
      Firebase.begin(&config, &auth);
      Firebase.reconnectWiFi(true);
      
      Serial.println("Firebase setup complete");
    } else {
      Serial.println("WiFi connection failed");
    }
  }

  // Jika kedua koneksi berhasil, jalankan logika utama
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    // Baca perintah VFD dari Firebase
    if (Firebase.RTDB.getBool(&fbdo, "silagung-controller/commands/vfd")) {
      bool newVfdState = fbdo.boolData();
      if (newVfdState != vfdRunning) {
        vfdRunning = newVfdState;
        Serial.println("VFD state changed to: " + String(vfdRunning));
      }
    }
    
    // Baca status relay dari Firebase
    for(int i = 1; i <= 6; i++) {
      String path = "silagung-controller/relay/relay" + String(i);
      if (Firebase.RTDB.getBool(&fbdo, path)) {
        bool newState = fbdo.boolData();
        if(newState != relay_status[i-1]) {
          controlRelay(i, newState);
          Serial.println("Relay " + String(i) + " changed to: " + String(newState));
        }
      }
    }
    
    // Kirim status terkini ke Firebase
    FirebaseJson json;
    json.add("vfdRunning", vfdRunning);
    for(int i = 0; i < 6; i++) {
      json.add(("relay" + String(i+1)).c_str(), relay_status[i]);
    }
    
    if (Firebase.RTDB.setJSON(&fbdo, "silagung-controller/status", &json)) {
      Serial.println("Status berhasil diperbarui");
    } else {
      Serial.println("Gagal memperbarui status: " + fbdo.errorReason());
    }
  }

  delay(1000);
}
