// By Fatih Nurrobi Alansshori Teknik Komputer UPI
#include <Arduino.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <RTClib.h>
#include <Firebase_ESP_Client.h>
#include <ModbusMaster.h>
#include <HardwareSerial.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

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

// Definisi untuk penyimpanan EEPROM
#define EEPROM_SIZE 4096
#define SCHEDULE_ADDR 0
#define SCHEDULE_MAGIC_NUMBER 0xAB56 // Untuk validasi data EEPROM

// Struktur untuk menyimpan waktu jadwal
struct ScheduleTime {
  uint8_t hour;
  uint8_t minute;
};

// Struktur untuk urutan mode dan durasi
struct ScheduleMode {
  char mode[10]; // isibak, mixing, supply, all_off
  uint16_t duration;
};

// Struktur untuk menyimpan jadwal lengkap
struct Schedule {
  uint16_t magicNumber; // Untuk verifikasi validitas data
  char name[32];
  ScheduleTime times[10]; // Maksimal 10 waktu aktivasi
  uint8_t timeCount;
  bool days[7]; // 0=Minggu, 1=Senin, dst
  ScheduleMode sequence[5]; // Maksimal 5 urutan mode
  uint8_t sequenceCount;
  bool active;
};

// Variabel global untuk jadwal
Schedule schedule;
bool scheduleLoaded = false;
unsigned long lastScheduleCheck = 0;
unsigned long lastSyncTime = 0;
const unsigned long SYNC_INTERVAL = 10000; // Sinkronisasi 
const unsigned long CHECK_INTERVAL = 10000;  // Cek jadwal 

// Tambahkan deklarasi fungsi ini di bagian atas setelah deklarasi fungsi lain yang sudah ada
bool syncScheduleFromFirebase();
void saveScheduleToEEPROM();
bool loadScheduleFromEEPROM();
void checkAndRunSchedule();
void executeScheduleSequence();
void printScheduleInfo();
void runMode(const char* mode);

// Tambahkan variabel global untuk tracking kapan terakhir waktu ditampilkan
unsigned long lastTimeDisplay = 0;
const unsigned long TIME_DISPLAY_INTERVAL = 1000; // Tampilkan waktu setiap 1 detik

// Tambahkan variabel dan fungsi ini
bool forceSync = false;

// Tambahkan variabel global ini
unsigned long lastScheduleCheckTime = 0;
const unsigned long SCHEDULE_CHECK_INTERVAL = 5000; // Cek perubahan jadwal setiap 5 detik

// Tambahkan penampilan info jadwal dengan interval
unsigned long lastScheduleDisplayTime = 0;
const unsigned long SCHEDULE_DISPLAY_INTERVAL = 5000; // Tampilkan info jadwal setiap 5 detik

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

  // Inisialisasi EEPROM dan load jadwal
  EEPROM.begin(EEPROM_SIZE);
  if (loadScheduleFromEEPROM()) {
    Serial.println("Schedule loaded successfully from EEPROM");
  } else {
    Serial.println("No valid schedule in EEPROM, will try to sync from Firebase later");
  }

  xTaskCreate(
    VFDTask,     // Fungsi task
    "VFDTask",       // Nama task
    4096,                   // Stack size
    NULL,                   // Parameter
    1,                      // Prioritas
    NULL                    // Task handle
  );
  
  Serial.println("DEBUG: Setting initial sync time to trigger sync soon");
  lastSyncTime = millis() - (SYNC_INTERVAL - 10000); // Sinkronisasi akan terjadi 10 detik setelah boot
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

    // Modifikasi di loop() untuk memeriksa apakah detik saat ini antara 20-40 untuk sinkronisasi
    // Ini akan menjauhkan sinkronisasi dari periode eksekusi jadwal (0-9 detik)
    DateTime now = rtc.now();
    bool okToSync = (now.second() >= 20 && now.second() <= 40);

    // Sinkronisasi jadwal jika waktunya atau dipaksa dan tidak bertabrakan dengan window eksekusi
    if ((forceSync || millis() - lastSyncTime > SYNC_INTERVAL) && okToSync) {
      Serial.println("Memulai sinkronisasi jadwal" + String(forceSync ? " (dipaksa)" : " (terjadwal)"));
      
      if (syncScheduleFromFirebase()) {
        Serial.println("✓ Jadwal berhasil disinkronkan dari Firebase");
      } else {
        Serial.println("❌ Gagal sinkronisasi jadwal");
      }
      
      lastSyncTime = millis();
      forceSync = false; // Reset flag
    }
  }

  // Periksa dan jalankan jadwal berdasarkan waktu RTC
  if (scheduleLoaded && millis() - lastScheduleCheck > CHECK_INTERVAL) {
    checkAndRunSchedule();
    lastScheduleCheck = millis();
  }

  // Menampilkan waktu setiap detik
  if (millis() - lastTimeDisplay >= TIME_DISPLAY_INTERVAL) {
    DateTime now = rtc.now();
    Serial.print("WAKTU: ");
    
    // Format jam dengan leading zero jika perlu
    if (now.hour() < 10) Serial.print("0");
    Serial.print(now.hour());
    Serial.print(":");
    
    // Format menit dengan leading zero jika perlu
    if (now.minute() < 10) Serial.print("0");
    Serial.print(now.minute());
    Serial.print(":");
    
    // Format detik dengan leading zero jika perlu
    if (now.second() < 10) Serial.print("0");
    Serial.println(now.second());
    
    lastTimeDisplay = millis();
  }

  // Tampilkan info jadwal terkini secara periodik
  if (millis() - lastScheduleDisplayTime >= SCHEDULE_DISPLAY_INTERVAL) {
    Serial.println("\n===== STATUS JADWAL SAAT INI =====");
    
    if (!scheduleLoaded) {
      Serial.println("STATUS: Tidak ada jadwal yang dimuat!");
    } else {
      Serial.print("Nama Jadwal: ");
      Serial.println(schedule.name);
      
      Serial.print("Status Aktif: ");
      Serial.println(schedule.active ? "AKTIF" : "TIDAK AKTIF");
      
      Serial.print("Waktu Aktivasi: ");
      if (schedule.timeCount == 0) {
        Serial.println("(tidak ada)");
      } else {
        for (int i = 0; i < schedule.timeCount; i++) {
          if (i > 0) Serial.print(", ");
          
          // Format waktu dengan leading zero
          if (schedule.times[i].hour < 10) Serial.print("0");
          Serial.print(schedule.times[i].hour);
          Serial.print(":");
          if (schedule.times[i].minute < 10) Serial.print("0");
          Serial.print(schedule.times[i].minute);
        }
        Serial.println();
      }
      
      Serial.print("Hari Aktif: ");
      String dayNames[] = {"Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"};
      bool anyDay = false;
      for (int i = 0; i < 7; i++) {
        if (schedule.days[i]) {
          if (anyDay) Serial.print(", ");
          Serial.print(dayNames[i]);
          anyDay = true;
        }
      }
      if (!anyDay) Serial.print("(tidak ada)");
      Serial.println();
      
      Serial.print("Waktu Terakhir Sinkronisasi: ");
      unsigned long timeAgo = (millis() - lastSyncTime) / 1000;
      Serial.print(timeAgo);
      Serial.println(" detik yang lalu");
      
      Serial.print("Waktu Hingga Sinkronisasi Berikutnya: ");
      if (millis() < lastSyncTime + SYNC_INTERVAL) {
        unsigned long remainingTime = (lastSyncTime + SYNC_INTERVAL - millis()) / 1000;
        Serial.print(remainingTime);
        Serial.println(" detik");
      } else {
        Serial.println("Segera");
      }
    }
    
    Serial.println("===================================\n");
    lastScheduleDisplayTime = millis();
  }

  // Cek perubahan jadwal di Firebase lebih sering
  if (WiFi.status() == WL_CONNECTED && Firebase.ready() && 
      millis() - lastScheduleCheckTime >= SCHEDULE_CHECK_INTERVAL) {
    // Cek apakah ada perubahan jadwal di Firebase
    if (Firebase.RTDB.getInt(&fbdo, "silagung-controller/schedule_version")) {
      int currentVersion = fbdo.intData();
      static int lastVersion = -1;
      
      if (lastVersion == -1) {
        // Inisialisasi pertama kali
        lastVersion = currentVersion;
        Serial.print("Inisialisasi versi jadwal: ");
        Serial.println(lastVersion);
      } else if (currentVersion > lastVersion) {
        // Versi jadwal berubah, paksa sinkronisasi
        Serial.print("Terdeteksi perubahan jadwal! Versi lama: ");
        Serial.print(lastVersion);
        Serial.print(", Versi baru: ");
        Serial.println(currentVersion);
        
        forceSync = true;
        lastVersion = currentVersion;
      }
    }
    
    lastScheduleCheckTime = millis();
  }

  delay(1000);
}

//fungsi fungsi
void isibak() {
  Serial.println("Mode: Isi Bak");
  // Menyalakan relay sesuai konfigurasi mode isi bak
  controlRelay(1, true);  // Relay 1 ON
  controlRelay(2, true);  // Relay 2 ON
  controlRelay(3, false); // Relay 3 OFF
  controlRelay(4, true);  // Relay 4 ON
  controlRelay(5, false); // Relay 5 OFF
  controlRelay(6, false); // Relay 6 OFF
  
  // Nyalakan VFD otomatis
  vfdRunning = true;
}

void mixing() {
  Serial.println("Mode: Mixing");
  // Menyalakan relay sesuai konfigurasi mode mixing
  controlRelay(1, false); // Relay 1 OFF
  controlRelay(2, false); // Relay 2 OFF 
  controlRelay(3, true);  // Relay 3 ON
  controlRelay(4, true);  // Relay 4 ON
  controlRelay(5, false); // Relay 5 OFF
  controlRelay(6, false); // Relay 6 OFF
  
  // Nyalakan VFD otomatis
  vfdRunning = true;
}

void supply() {
  Serial.println("Mode: Supply");
  // Menyalakan relay sesuai konfigurasi mode supply
  controlRelay(1, true);  // Relay 1 ON
  controlRelay(2, false); // Relay 2 OFF
  controlRelay(3, false); // Relay 3 OFF
  controlRelay(4, false); // Relay 4 OFF
  controlRelay(5, true);  // Relay 5 ON
  controlRelay(6, false); // Relay 6 OFF
  
  // Nyalakan VFD otomatis
  vfdRunning = true;
}

void relayoff() {
  Serial.println("Mode: Semua Relay Mati");
  // Matikan semua relay
  for (int i = 1; i <= 6; i++) {
    controlRelay(i, false);
  }
  
  // Matikan VFD
  vfdRunning = false;
}

// Fungsi untuk menyinkronkan jadwal dari Firebase ke EEPROM
bool syncScheduleFromFirebase() {
  Serial.println("\n======= MEMULAI SINKRONISASI JADWAL DARI FIREBASE =======");
  Serial.print("Waktu Saat Ini: ");
  DateTime now = rtc.now();
  Serial.println(formatDateTime(now));
  
  if (Firebase.RTDB.get(&fbdo, "silagung-controller/schedules")) {
    Serial.println("✓ Data Jadwal Diterima dari Firebase");
    String rawData = fbdo.to<String>();
    
    // Tambahkan ringkasan data untuk debugging
    if (rawData.length() > 500) {
      Serial.print("Data (ringkasan): ");
      Serial.print(rawData.substring(0, 200));
      Serial.println("... [terpotong]");
    } else {
      Serial.print("Data Lengkap: ");
      Serial.println(rawData);
    }
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, rawData);
    
    if (error) {
      Serial.print("DEBUG: deserializeJson() failed: ");
      Serial.println(error.c_str());
      return false;
    }
    
    // Cek apakah ada jadwal
    Serial.print("DEBUG: Document size: ");
    Serial.println(doc.size());
    
    if (doc.size() > 0) {
      // Ambil jadwal pertama (karena hanya 1 jadwal)
      JsonObject scheduleObj;
      String firstKey = "";
      
      // Cara untuk mengambil item pertama dari dokumen
      Serial.println("DEBUG: Iterating through document keys");
      for (JsonPair pair : doc.as<JsonObject>()) {
        firstKey = pair.key().c_str();
        scheduleObj = pair.value().as<JsonObject>();
        Serial.print("DEBUG: Found schedule key: ");
        Serial.println(firstKey);
        break; // Hanya ambil yang pertama
      }
      
      if (scheduleObj.isNull()) {
        Serial.println("DEBUG: Failed to get schedule object - null object");
        return false;
      }
      
      // Reset schedule data
      memset(&schedule, 0, sizeof(Schedule));
      schedule.magicNumber = SCHEDULE_MAGIC_NUMBER;
      
      // Copy nama jadwal
      const char* scheduleName = scheduleObj["name"] | "Default Schedule";
      strlcpy(schedule.name, scheduleName, sizeof(schedule.name));
      Serial.print("DEBUG: Schedule name: ");
      Serial.println(schedule.name);
      
      // Copy waktu aktivasi
      if (scheduleObj.containsKey("times") && scheduleObj["times"].is<JsonArray>()) {
        JsonArray times = scheduleObj["times"];
        schedule.timeCount = min((int)times.size(), 10);
        Serial.print("DEBUG: Found times array with ");
        Serial.print(schedule.timeCount);
        Serial.println(" time(s)");
        
        for (int i = 0; i < schedule.timeCount; i++) {
          String timeStr = times[i].as<String>();
          schedule.times[i].hour = timeStr.substring(0, 2).toInt();
          schedule.times[i].minute = timeStr.substring(3).toInt();
          Serial.print("DEBUG: Time ");
          Serial.print(i);
          Serial.print(": ");
          Serial.print(schedule.times[i].hour);
          Serial.print(":");
          Serial.println(schedule.times[i].minute);
        }
      } else if (scheduleObj.containsKey("time")) {
        // Untuk kompatibilitas dengan format lama yang hanya memiliki satu waktu
        String timeStr = scheduleObj["time"].as<String>();
        schedule.times[0].hour = timeStr.substring(0, 2).toInt();
        schedule.times[0].minute = timeStr.substring(3).toInt();
        schedule.timeCount = 1;
        Serial.print("DEBUG: Using legacy single time format: ");
        Serial.print(schedule.times[0].hour);
        Serial.print(":");
        Serial.println(schedule.times[0].minute);
      } else {
        Serial.println("DEBUG: No times or time field found in schedule");
      }
      
      // Copy hari
      for (int i = 0; i < 7; i++) {
        schedule.days[i] = false;
      }
      
      if (scheduleObj.containsKey("days") && scheduleObj["days"].is<JsonArray>()) {
        JsonArray days = scheduleObj["days"];
        Serial.print("DEBUG: Found days array with ");
        Serial.print(days.size());
        Serial.println(" day(s)");
        
        for (JsonVariant v : days) {
          String day = v.as<String>();
          Serial.print("DEBUG: Day: ");
          Serial.println(day);
          
          if (day == "monday") schedule.days[1] = true;
          else if (day == "tuesday") schedule.days[2] = true;
          else if (day == "wednesday") schedule.days[3] = true;
          else if (day == "thursday") schedule.days[4] = true;
          else if (day == "friday") schedule.days[5] = true;
          else if (day == "saturday") schedule.days[6] = true;
          else if (day == "sunday") schedule.days[0] = true;
        }
      } else {
        Serial.println("DEBUG: No days field found in schedule");
      }
      
      // Copy urutan mode
      if (scheduleObj.containsKey("sequence") && scheduleObj["sequence"].is<JsonArray>()) {
        JsonArray sequence = scheduleObj["sequence"];
        schedule.sequenceCount = min((int)sequence.size(), 5);
        Serial.print("DEBUG: Found sequence array with ");
        Serial.print(schedule.sequenceCount);
        Serial.println(" mode(s)");
        
        for (int i = 0; i < schedule.sequenceCount; i++) {
          const char* modeStr = sequence[i]["mode"] | "all_off";
          strlcpy(schedule.sequence[i].mode, modeStr, sizeof(schedule.sequence[i].mode));
          schedule.sequence[i].duration = sequence[i]["duration"] | 0;
          
          Serial.print("DEBUG: Mode ");
          Serial.print(i);
          Serial.print(": ");
          Serial.print(schedule.sequence[i].mode);
          Serial.print(" for ");
          Serial.print(schedule.sequence[i].duration);
          Serial.println(" minutes");
        }
      } else {
        Serial.println("DEBUG: No sequence field found in schedule");
      }
      
      // Copy status aktif
      schedule.active = scheduleObj["active"] | false;
      Serial.print("DEBUG: Schedule active status: ");
      Serial.println(schedule.active ? "Active" : "Inactive");
      
      // Simpan jadwal ke EEPROM
      Serial.println("DEBUG: Saving schedule to EEPROM");
      saveScheduleToEEPROM();
      
      scheduleLoaded = true;
      printScheduleInfo();
      Serial.println("\n✓ JADWAL BERHASIL DIPERBARUI");
      Serial.println("===== RINGKASAN JADWAL BARU =====");
      Serial.print("Nama: ");
      Serial.println(schedule.name);
      
      Serial.print("Waktu: ");
      for (int i = 0; i < schedule.timeCount; i++) {
        if (i > 0) Serial.print(", ");
        if (schedule.times[i].hour < 10) Serial.print("0");
        Serial.print(schedule.times[i].hour);
        Serial.print(":");
        if (schedule.times[i].minute < 10) Serial.print("0");
        Serial.print(schedule.times[i].minute);
      }
      Serial.println();
      
      Serial.print("Urutan Mode: ");
      for (int i = 0; i < schedule.sequenceCount; i++) {
        if (i > 0) Serial.print(" → ");
        Serial.print(schedule.sequence[i].mode);
        Serial.print("(");
        Serial.print(schedule.sequence[i].duration);
        Serial.print("mnt)");
      }
      Serial.println();
      
      Serial.println("================================\n");
      return true;
    } else {
      Serial.println("DEBUG: No schedules found in Firebase (empty document)");
      return false;
    }
  } else {
    Serial.print("❌ GAGAL mendapatkan data jadwal: ");
    Serial.println(fbdo.errorReason());
    return false;
  }
}

// Simpan jadwal ke EEPROM
void saveScheduleToEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t *ptr = (uint8_t*)&schedule;
  for (unsigned int i = 0; i < sizeof(Schedule); i++) {
    EEPROM.write(SCHEDULE_ADDR + i, *ptr++);
  }
  EEPROM.commit();
  Serial.println("Schedule saved to EEPROM");
}

// Load jadwal dari EEPROM
bool loadScheduleFromEEPROM() {
  Serial.println("==== DEBUG: Loading schedule from EEPROM ====");
  EEPROM.begin(EEPROM_SIZE);
  uint8_t *ptr = (uint8_t*)&schedule;
  for (unsigned int i = 0; i < sizeof(Schedule); i++) {
    *ptr++ = EEPROM.read(SCHEDULE_ADDR + i);
  }
  
  // Validasi data yang dibaca (menggunakan magic number)
  Serial.print("DEBUG: Magic number read: 0x");
  Serial.println(schedule.magicNumber, HEX);
  Serial.print("DEBUG: Expected magic number: 0x");
  Serial.println(SCHEDULE_MAGIC_NUMBER, HEX);
  Serial.print("DEBUG: Time count: ");
  Serial.println(schedule.timeCount);
  Serial.print("DEBUG: Sequence count: ");
  Serial.println(schedule.sequenceCount);
  
  if (schedule.magicNumber == SCHEDULE_MAGIC_NUMBER && 
      schedule.timeCount > 0 && schedule.timeCount <= 10 && 
      schedule.sequenceCount > 0 && schedule.sequenceCount <= 5) {
    scheduleLoaded = true;
    Serial.println("DEBUG: Valid schedule loaded from EEPROM");
    printScheduleInfo();
    return true;
  } else {
    Serial.println("DEBUG: No valid schedule found in EEPROM");
    scheduleLoaded = false;
    return false;
  }
}

// Periksa dan jalankan jadwal jika waktunya sesuai
void checkAndRunSchedule() {
  if (!scheduleLoaded) {
    Serial.println("GAGAL: Jadwal tidak dimuat");
    return;
  }
  
  if (!schedule.active) {
    Serial.println("GAGAL: Jadwal tidak aktif");
    return;
  }
  
  DateTime now = rtc.now();
  
  // Cek apakah hari ini jadwal aktif
  int dayOfWeek = now.dayOfTheWeek(); // 0=Minggu, 1=Senin, dst
  if (!schedule.days[dayOfWeek]) {
    Serial.println("GAGAL: Hari ini (" + String(daysOfTheWeek[dayOfWeek]) + ") tidak terjadwal");
    return;
  }
  
  // Cek apakah ada waktu yang cocok
  bool anyTimeMatch = false;
  for (int i = 0; i < schedule.timeCount; i++) {
    if (now.hour() == schedule.times[i].hour && 
        now.minute() == schedule.times[i].minute && 
        now.second() < 10) { // Beri jendela 10 detik untuk eksekusi
      
      anyTimeMatch = true;
      Serial.print("MATCH! Jadwal cocok pada ");
      Serial.print(now.hour());
      Serial.print(":");
      Serial.print(now.minute());
      Serial.print(":");
      Serial.print(now.second());
      Serial.print(" - ");
      Serial.println(schedule.name);
      
      if (schedule.sequenceCount == 0) {
        Serial.println("GAGAL: Jadwal tidak memiliki urutan mode");
        return;
      }
      
      executeScheduleSequence();
      // Tunggu sedikit agar tidak dijalankan berulang kali dalam 10 detik
      delay(10000);
      break;
    }
  }
  
  if (!anyTimeMatch && now.second() == 0) {
    Serial.print("TIDAK COCOK: Waktu sekarang ");
    Serial.print(now.hour());
    Serial.print(":");
    Serial.print(now.minute());
    Serial.print(" tidak cocok dengan jadwal ");
    
    for (int i = 0; i < schedule.timeCount; i++) {
      if (i > 0) Serial.print(", ");
      Serial.print(schedule.times[i].hour);
      Serial.print(":");
      if (schedule.times[i].minute < 10) Serial.print("0");
      Serial.print(schedule.times[i].minute);
    }
    Serial.println();
  }
}

// Jalankan urutan mode dalam jadwal
void executeScheduleSequence() {
  for (int i = 0; i < schedule.sequenceCount; i++) {
    Serial.print("Running mode: ");
    Serial.print(schedule.sequence[i].mode);
    Serial.print(" for ");
    Serial.print(schedule.sequence[i].duration);
    Serial.println(" minutes");
    
    // Jalankan mode berdasarkan string
    runMode(schedule.sequence[i].mode);
    
    // Tunggu selama durasi (dalam menit)
    unsigned long duration_ms = (unsigned long)schedule.sequence[i].duration * 60000;
    unsigned long start_time = millis();
    
    // Jangan gunakan delay panjang agar loop utama tetap responsif
    while (millis() - start_time < duration_ms) {
      delay(1000); // Cek setiap detik
      
      // Tanggapi perintah Firebase jika diperlukan
      if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        // Ambil perintah interrupt jika diperlukan (contoh untuk pengembangan future)
        if (Firebase.RTDB.getBool(&fbdo, "silagung-controller/commands/interrupt_schedule")) {
          bool interrupt = fbdo.boolData();
          if (interrupt) {
            Serial.println("Schedule execution interrupted by command");
            // Reset flag interrupt
            Firebase.RTDB.setBool(&fbdo, "silagung-controller/commands/interrupt_schedule", false);
            return;
          }
        }
      }
    }
  }
  
  // Setelah selesai, matikan semua
  runMode("all_off");
}

// Run mode berdasarkan string identifikasi
void runMode(const char* mode) {
  if (strcmp(mode, "isibak") == 0) {
    isibak();
  } else if (strcmp(mode, "mixing") == 0) {
    mixing();
  } else if (strcmp(mode, "supply") == 0) {
    supply();
  } else if (strcmp(mode, "all_off") == 0) {
    relayoff();
  }
}

// Tampilkan informasi jadwal untuk debugging
void printScheduleInfo() {
  Serial.println("Schedule: " + String(schedule.name));
  Serial.println("Active: " + String(schedule.active ? "Yes" : "No"));
  
  Serial.print("Times: ");
  for (int i = 0; i < schedule.timeCount; i++) {
    Serial.print(schedule.times[i].hour);
    Serial.print(":");
    if (schedule.times[i].minute < 10) Serial.print("0");
    Serial.print(schedule.times[i].minute);
    if (i < schedule.timeCount - 1) Serial.print(", ");
  }
  Serial.println();
  
  Serial.print("Days: ");
  String dayNames[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};
  bool first = true;
  for (int i = 0; i < 7; i++) {
    if (schedule.days[i]) {
      if (!first) Serial.print(", ");
      Serial.print(dayNames[i]);
      first = false;
    }
  }
  Serial.println();
  
  Serial.println("Sequence:");
  for (int i = 0; i < schedule.sequenceCount; i++) {
    Serial.print("  ");
    Serial.print(i + 1);
    Serial.print(". ");
    Serial.print(schedule.sequence[i].mode);
    Serial.print(" (");
    Serial.print(schedule.sequence[i].duration);
    Serial.println(" mnt)");
  }
}
