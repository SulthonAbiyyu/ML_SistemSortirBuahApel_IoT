#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include "HX711.h"
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HTTPUpdate.h>
// "mbedtls/base64.h" DIHAPUS -- tidak dipakai di mana pun di file ini.

// ==========================================
// VERSI FIRMWARE
// ==========================================
// WAJIB dinaikkan (mis. "1.0.1") SETIAP KALI kamu upload firmware baru ke
// ota/version.json di GitHub. Kalau angka di version.json TIDAK lebih besar
// dari angka di sini, ESP32 akan menganggap tidak ada update dan tidak
// akan mendownload apapun. Format: MAJOR.MINOR.PATCH (angka saja per bagian).
#define FIRMWARE_VERSION "1.0.1"

// ==========================================
// KONFIGURASI WIFI & CLOUD API
// ==========================================
const char* WIFI_SETUP_AP_NAME     = "SORTIR-BUAH-MAIN-SETUP";
const char* WIFI_SETUP_AP_PASSWORD = "sortirbuah123";

WiFiManager wm;

// CLOUD_API_URL DIHAPUS -- ESP32 tidak lagi memanggil cloud ML API sendiri.
// Kedua kamera (ATAS & SAMPING) sekarang webcam laptop lewat
// camera_bridge_server.py, yang capture+classify+lapor balik ke /classify.
String SHEET_WEBAPP_URL = "https://script.google.com/macros/s/AKfycbxXaqN60uyMrsx000K40tFXMIs4NVsxpNYME9OlSjjoI-WDjyLVXw7W87ir9ZHUSyF7vA/exec";
const char* SHEET_SECRET_KEY = "kelompokPKM";

// ==========================================
// KONFIGURASI OTA (UPDATE FIRMWARE JARAK JAUH VIA INTERNET)
// ==========================================
// GANTI URL INI dengan raw URL file version.json di repo GitHub kamu.
// Sudah diisi sesuai repo yang kamu pakai: SulthonAbiyyu/ML_SistemSortirBuahApel_IoT
const char* OTA_VERSION_URL = "https://raw.githubusercontent.com/SulthonAbiyyu/ML_SistemSortirBuahApel_IoT/main/ota/version.json";
unsigned long lastOTACheck = 0;
const unsigned long OTA_CHECK_INTERVAL_MS = 30UL * 1000UL; // cek otomatis tiap 30 detik (idle & saat menunggu hasil ML)

const char* CAM_TOP_HOST  = "camtop.local";
const char* CAM_SIDE_HOST = "camside.local";

String currentCaptureId = "";
// MAX_PHOTO_SIZE_BYTES DIHAPUS -- ESP32 tidak lagi mengunduh foto sama sekali.

WebServer server(80);

// ==========================================
// KONFIGURASI PIN HARDWARE
// ==========================================
#define HX711_DT_PIN   4
#define HX711_SCK_PIN  5

#define SERVO_PUSH_PIN 13
#define SERVO_SORT_PIN 14
// Servo sortir KEDUA (baru, dipasang di belakang servo sortir pertama).
// GPIO27 dipilih karena aman: bukan pin strapping boot (0/2/5/12/15),
// bukan input-only (34-39), dan belum dipakai komponen manapun di board ini.
#define SERVO_SORT2_PIN 27
#define BUZZER_PIN     25

#define OLED_SDA_PIN     21
#define OLED_SCL_PIN     22
#define OLED_I2C_ADDRESS 0x3C
#define OLED_WIDTH       128
#define OLED_HEIGHT      64
#define OLED_RESET_PIN   -1

// ==========================================
// GLOBAL OBJECTS & VARIABLES
// ==========================================
HX711 scale;
Preferences prefs;

bool hx711Ready = false;
float CALIBRATION_FACTOR = 1.0;
bool AUTO_DETECT_ENABLED = true;
float WEIGHT_TRIGGER_THRESHOLD = 20.0;

const unsigned long WEIGHT_STABLE_MS = 3000;
const float WEIGHT_STABLE_TOLERANCE_G = 5.0;
const unsigned long STABILIZE_FEEDBACK_MS = 1000;

bool weightAboveThreshold = false;
unsigned long weightAboveSince = 0;
float weightStableMin = 0;
float weightStableMax = 0;
unsigned long lastStabilizeFeedback = 0;
bool readyForNextTrigger = true;

Servo servoPush;
Servo servoSort;    // Gerbang sortir 1. Default TUTUP (menahan buah -> grade A).
                    // Dibuka untuk grade B & C supaya buah lewat.
Servo servoSort2;   // Gerbang sortir 2 (di belakang servoSort). Default TUTUP
                    // (menahan buah -> grade B). Dibuka HANYA untuk grade C
                    // supaya buah lewat terus sampai keranjang belakang (reject).

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
bool lcdReady = false;

// ==========================================
// PENGATURAN DAN LOGIKA SERVO
// ==========================================
// Setiap servo bisa punya arah putarnya sendiri-sendiri (REVERSED true/false)
// tanpa mempengaruhi servo yang lain -- karena secara fisik tiap servo bisa
// terpasang dengan orientasi berbeda. Kalau REVERSED true, sudut logicalAngle
// (0-180) dicerminkan (180 - angle) sebelum dikirim ke servo.write().
int servoPhysicalAngle(int logicalAngle, bool reversed) {
  if (reversed) {
    return 180 - logicalAngle;
  }
  return logicalAngle;
}

const bool PUSH_SERVO_REVERSED  = false; // servo dorong -- arahnya sudah benar
const bool SORT_SERVO1_REVERSED = true;  // servo sortir 1 (pin 14) -- DIBALIK
const bool SORT_SERVO2_REVERSED = true;  // servo sortir 2 (pin 27) -- DIBALIK

const int PUSH_IDLE_ANGLE   = 180;
// Nilai default adalah 90. Diturunkan ke 60 agar sapuan servo lebih luas ke kanan (180 -> 60 derajat).
// Semakin KECIL nilai ini (misal 45), maka servo akan bergerak semakin JAUH ke kanan.
const int PUSH_ACTIVE_ANGLE = 110;
const int PUSH_HOLD_MS      = 600;

// ==========================================
// LOGIKA FISIK SERVO SORTIR (DIREVISI SESUAI INSTRUKSI TERBARU)
// ==========================================
// Kedua servo sortir defaultnya DIAM di SORT_IDLE_ANGLE.
//
//   Grade A: KEDUA servo DIAM TOTAL, tidak ada gerakan sama sekali.
//   Grade B: servo 1 (servoSort)  BERGERAK BUKA, hold 5 detik, lalu tutup lagi.
//            servo 2 (servoSort2) TETAP DIAM.
//   Grade C: KEDUA servo (servoSort & servoSort2) BERGERAK BUKA bersamaan,
//            hold 5 detik, lalu tutup lagi bersamaan.
//
// Rotasi SORT_ACTIVE_ANGLE dipakai untuk servo 1 MAUPUN servo 2 supaya besar
// putarannya SAMA (sebelumnya servo 1 cuma 30 derajat, sekarang disamakan
// dengan servo 2 yang sudah benar di 90 derajat).
const int SORT_IDLE_ANGLE   = 180;   // DIAM (default kedua servo)
const int SORT_ACTIVE_ANGLE = 130;    // BUKA (dipakai servo 1 ATAU servo 2, rotasi disamakan)
const unsigned long GRADE_HOLD_MS = 7000UL; // hold 5 detik saat servo aktif buka
const int SORT_HOLD_MS    = 4000;  // dipakai untuk demo manual lama, tidak dipakai lagi di logic baru

// ==========================================r
// KALIBRASI WAKTU TEMPUH BELT (dihitung dari SAAT PUSH, bukan dari kapan
// hasil ML selesai -- belt berjalan KONTINU jadi servo harus standby sesuai
// jadwal, diukur manual oleh pengguna di conveyor fisik)
// ==========================================
const unsigned long TRAVEL_PUSH_TO_SORT1_MS = 3000UL; // push -> posisi gerbang 1
const unsigned long TRAVEL_PUSH_TO_SORT2_MS = 4000UL; // push -> posisi gerbang 2
const unsigned long TRAVEL_PUSH_TO_BACK_MS  = 5000UL; // push -> keranjang belakang (reject, grade C)

bool camTopOnline  = false;
bool camSideOnline = false;
bool systemBusy = false;
unsigned long fruitID = 0;
bool internetReady = false;
bool otaCheckInProgress = false;
String otaLastResult = "Belum pernah cek.";

unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 15UL * 1000UL;
unsigned long lastCamCheck = 0;
const unsigned long CAM_CHECK_INTERVAL_MS = 30UL * 1000UL;
// lastKeepAlive/KEEP_ALIVE_INTERVAL_MS/cloudAPIWarm DIHAPUS -- keep-alive
// cloud ML sekarang tanggung jawab camera_bridge_server.py, bukan ESP32.

// ============================================================
// GRADE RESULT (dari camera_bridge_server.py, lewat POST /classify)
// ============================================================
// handleFruitArrival() menunggu variabel ini di-set oleh handleClassify()
// setelah bridge selesai klasifikasi KEDUA foto (ATAS+SAMPING) ke cloud ML
// dan menghitung grade akhir (terjelek dari 2 sisi) sendiri di sisi laptop.
volatile bool gradeResultReady = false;
String gradeResultCaptureId = "";
char gradeResultGrade = 0;      // 'A' / 'B' / 'C' (gagal) / 0 = belum ada
String gradeResultBuah = "";
const unsigned long GRADE_WAIT_TIMEOUT_MS = 90UL * 1000UL; // toleransi cold-start Render + waktu ML

// MultipartStream DIHAPUS -- dulu dipakai classifyWithCloudAPI() yang
// sekarang sudah tidak ada (ESP32 tidak lagi kirim foto ke cloud ML).

// ==========================================
// FUNGSI PENDUKUNG SERVO & PERIPHERAL
// ==========================================
void setupServos() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("INISIALISASI SERVO");
  Serial.println("========================================");
  
  servoPush.setPeriodHertz(50);
  servoSort.setPeriodHertz(50);
  servoSort2.setPeriodHertz(50);
  
  servoPush.attach(SERVO_PUSH_PIN, 500, 2400);
  servoSort.attach(SERVO_SORT_PIN, 500, 2400);
  servoSort2.attach(SERVO_SORT2_PIN, 500, 2400);
  
  servoPush.write(servoPhysicalAngle(PUSH_IDLE_ANGLE, PUSH_SERVO_REVERSED));
  servoSort.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO1_REVERSED));
  servoSort2.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO2_REVERSED));
  
  Serial.println("SERVO PUSH     : SIAP (idle)");
  Serial.println("SERVO SORTIR 1 : SIAP (default TUTUP -- gerbang 1)");
  Serial.println("SERVO SORTIR 2 : SIAP (default TUTUP -- gerbang 2)");
}

unsigned long pushToConveyor() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("PUSH BUAH KE CONVEYOR");
  Serial.println("========================================");
  
  systemBusy = true;
  unsigned long pushStartTime = millis(); // referensi t=0 buat jadwal servo sortir
  servoPush.write(servoPhysicalAngle(PUSH_ACTIVE_ANGLE, PUSH_SERVO_REVERSED));
  delay(PUSH_HOLD_MS);
  servoPush.write(servoPhysicalAngle(PUSH_IDLE_ANGLE, PUSH_SERVO_REVERSED));
  systemBusy = false;
  
  Serial.println("PUSH SELESAI, servo kembali idle.");
  return pushStartTime;
}

// Tunggu sampai waktu target tercapai TANPA blocking total -- server.handleClient()
// tetap dipanggil supaya request HTTP lain (mis. /classify buah berikutnya jika
// ada overlap) tetap sempat diproses selama menunggu.
void waitUntilMillis(unsigned long targetMillis) {
  while ((long)(millis() - targetMillis) < 0) {
    server.handleClient();
    delay(5);
  }
}

// Versi MANUAL untuk testing lewat serial menu (1/2/3) -- gerak langsung
// begitu dipanggil, TIDAK dijadwalkan berdasarkan waktu tempuh push. Dipakai
// buat ngecek posisi/arah servo sortir doang, bukan buat siklus buah asli.
void sortFruit(char grade) {
  Serial.println();
  Serial.println("========================================");
  Serial.println("SORTIR BUAH (TES MANUAL)");
  Serial.println("========================================");
  Serial.print("GRADE: ");
  Serial.println(grade);
  
  systemBusy = true;
  
  if (grade == 'A') {
    // KEDUA servo DIAM TOTAL, tidak ada gerakan sama sekali. Default tutup
    // sudah cukup menahan buah di gerbang 1 (FIX: sebelumnya blok ini ikut
    // buka-tutup servo 1 seperti grade B, itu salah).
    servoSort.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO1_REVERSED));
    servoSort2.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO2_REVERSED));
  } else if (grade == 'B') {
    // Servo 1 BUKA, hold 5 detik, lalu tutup lagi. Servo 2 TETAP DIAM.
    servoSort2.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO2_REVERSED)); // servo 2 diam
    servoSort.write(servoPhysicalAngle(SORT_ACTIVE_ANGLE, SORT_SERVO1_REVERSED)); // servo 1 buka
    delay(GRADE_HOLD_MS);
    servoSort.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO1_REVERSED));  // servo 1 tutup lagi
  } else if (grade == 'C') {
    // KEDUA servo BUKA bersamaan, hold 5 detik, lalu tutup lagi bersamaan.
    servoSort.write(servoPhysicalAngle(SORT_ACTIVE_ANGLE, SORT_SERVO1_REVERSED));
    servoSort2.write(servoPhysicalAngle(SORT_ACTIVE_ANGLE, SORT_SERVO2_REVERSED));
    delay(GRADE_HOLD_MS);
    servoSort.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO1_REVERSED));
    servoSort2.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO2_REVERSED));
  } else {
    Serial.println("GRADE TIDAK DIKENALI, servo tetap netral.");
    systemBusy = false;
    return;
  }
  
  systemBusy = false;
  Serial.println("SORTIR SELESAI, servo kembali netral.");
}

// Versi OTOMATIS dipakai di siklus buah asli (handleFruitArrival()) --
// servo dijadwalkan berdasarkan estimasi waktu tempuh dari SAAT PUSH
// (pushStartTime), bukan langsung begitu grade diketahui.
void sortFruitTimed(char grade, unsigned long pushStartTime) {
  Serial.println();
  Serial.println("========================================");
  Serial.println("SORTIR BUAH (TIMED, sesuai jadwal push)");
  Serial.println("========================================");
  Serial.print("GRADE: ");
  Serial.println(grade);
  
  systemBusy = true;
  
  if (grade == 'A') {
    // KEDUA servo DIAM TOTAL, tidak ada gerakan sama sekali sepanjang proses.
    // Default tutup sudah cukup menahan buah di gerbang 1 (FIX: sebelumnya
    // blok ini ikut buka-tutup servo 1 seperti grade B, itu salah).
    servoSort.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO1_REVERSED));
    servoSort2.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO2_REVERSED));
  } else if (grade == 'B') {
    // Tunggu buah sampai di gerbang 1, lalu servo 1 BUKA, hold 5 detik,
    // baru tutup lagi. Servo 2 TETAP DIAM sepanjang proses.
    servoSort2.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO2_REVERSED)); // servo 2 diam
    waitUntilMillis(pushStartTime + TRAVEL_PUSH_TO_SORT1_MS);
    servoSort.write(servoPhysicalAngle(SORT_ACTIVE_ANGLE, SORT_SERVO1_REVERSED)); // servo 1 buka
    delay(GRADE_HOLD_MS);
    servoSort.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO1_REVERSED));  // servo 1 tutup lagi
  } else if (grade == 'C') {
    // Tunggu buah sampai di gerbang 1, lalu KEDUA servo BUKA bersamaan,
    // hold 5 detik, baru tutup lagi bersamaan.
    waitUntilMillis(pushStartTime + TRAVEL_PUSH_TO_SORT1_MS);
    servoSort.write(servoPhysicalAngle(SORT_ACTIVE_ANGLE, SORT_SERVO1_REVERSED));
    servoSort2.write(servoPhysicalAngle(SORT_ACTIVE_ANGLE, SORT_SERVO2_REVERSED));
    delay(GRADE_HOLD_MS);
    servoSort.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO1_REVERSED));
    servoSort2.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO2_REVERSED));
  } else {
    Serial.println("GRADE TIDAK DIKENALI, servo tetap netral.");
    systemBusy = false;
    return;
  }
  
  systemBusy = false;
  Serial.println("SORTIR SELESAI, servo kembali netral.");
}

void buzzerBeep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) {
      delay(durationMs);
    }
  }
}

// ==========================================
// INTI ALUR WIFI, INTERNET, & OLED LCD
// ==========================================
void setupWiFi() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("START WIFI (WiFiManager)");
  Serial.println("========================================");
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  
  pinMode(0, INPUT_PULLUP);
  delay(50);
  if (digitalRead(0) == LOW) {
    Serial.println("Tombol BOOT ditekan -> hapus WiFi tersimpan.");
    wm.resetSettings();
  }
  
  wm.setConfigPortalTimeout(180);
  bool ok = wm.autoConnect(WIFI_SETUP_AP_NAME, WIFI_SETUP_AP_PASSWORD);
  
  if (ok) {
    internetReady = true;
    Serial.println("WIFI: TERSAMBUNG");
    Serial.print("IP ESP32 MAIN: ");
    Serial.println(WiFi.localIP());
  } else {
    internetReady = false;
    Serial.println("WIFI: BELUM TERSAMBUNG (setup portal timeout 3 menit).");
  }
  
  if (MDNS.begin("main")) {
    Serial.println("mDNS aktif -> board dipanggil sebagai: main.local");
  } else {
    Serial.println("mDNS GAGAL start.");
  }
  Serial.println("----------------------------------------");
}

void setupLCD() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("OLED I2C (SSD1306)");
  Serial.println("========================================");
  
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    lcdReady = false;
    Serial.println("OLED: GAGAL DIINISIALISASI");
    return;
  }
  
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println("SORTIR BUAH");
  oled.setCursor(0, 16);
  oled.println("Booting...");
  oled.display();
  
  lcdReady = true;
  Serial.println("OLED: SIAP");
}

void lcdShow(String line1, String line2) {
  if (!lcdReady) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println(line1);
  oled.setCursor(0, 16);
  oled.println(line2);
  oled.display();
}

// Layar IDLE (nunggu buah) -- dipakai di semua tempat yang sebelumnya
// hardcode lcdShow("SORTIR BUAH READY", "Menunggu buah..") supaya status
// kamera offline langsung kelihatan di OLED tanpa perlu cek serial monitor.
void showIdleScreen() {
  if (!camTopOnline && !camSideOnline) {
    lcdShow("KEDUA CAM OFFLINE!", "Cek bridge server");
  } else if (!camTopOnline) {
    lcdShow("CAM ATAS OFFLINE!", "Menunggu buah..");
  } else if (!camSideOnline) {
    lcdShow("CAM SAMPING OFFLINE!", "Menunggu buah..");
  } else {
    lcdShow("SORTIR BUAH READY", "Menunggu buah..");
  }
}

// ==========================================
// SISTEM LOAD CELL HX711 (KALIBRASI & TIMBANG)
// ==========================================
void setupHX711() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("HX711 LOAD CELL");
  Serial.println("========================================");
  
  prefs.begin("siambali", false);
  CALIBRATION_FACTOR = prefs.getFloat("calfactor", 1.0);
  WEIGHT_TRIGGER_THRESHOLD = prefs.getFloat("wthresh", WEIGHT_TRIGGER_THRESHOLD);
  
  Serial.print("Calibration factor: ");
  Serial.println(CALIBRATION_FACTOR, 4);
  Serial.print("Trigger threshold : ");
  Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1);
  Serial.println(" gram");
  
  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
  delay(500);
  
  if (scale.is_ready()) {
    hx711Ready = true;
    Serial.println("HX711: READY");
    scale.set_scale(CALIBRATION_FACTOR);
    scale.tare(20);
    Serial.println("Tare otomatis saat boot selesai.");
  } else {
    hx711Ready = false;
    Serial.println("HX711: NOT READY (cek wiring DT/SCK)");
  }
}

float readWeight() {
  static float lastValidWeight = 0;
  if (!hx711Ready) return 0;
  if (!scale.is_ready()) {
    return lastValidWeight;
  }
  float weight = scale.get_units(10);
  if (weight > -2 && weight < 2) {
    weight = 0;
  }
  lastValidWeight = weight;
  return weight;
}

void tareLoadCell() {
  if (!hx711Ready) {
    Serial.println("HX711 tidak tersedia, tidak bisa tare.");
    return;
  }
  Serial.println();
  Serial.println("========================================");
  Serial.println("TARE LOAD CELL");
  Serial.println("========================================");
  delay(2000);
  scale.set_scale(CALIBRATION_FACTOR);
  scale.tare(20);
  Serial.println("TARE SELESAI. Berat sekarang ~0.");
}

String waitForSerialLine() {
  while (!Serial.available()) {
    delay(10);
  }
  String line = Serial.readStringUntil('\n');
  line.trim();
  return line;
}

void calibrateLoadCell() {
  if (!hx711Ready) {
    Serial.println("HX711 tidak tersedia, tidak bisa kalibrasi.");
    return;
  }
  Serial.println();
  Serial.println("========================================");
  Serial.println("KALIBRASI LOAD CELL");
  Serial.println("========================================");
  
  Serial.println("LANGKAH 1: Kosongkan load cell.");
  Serial.println("Ketik apa saja lalu ENTER jika sudah kosong...");
  waitForSerialLine();
  
  scale.set_scale(1.0);
  scale.tare(20);
  Serial.println("Tare selesai.");
  
  Serial.println("\nLANGKAH 2: Taruh beban yang diketahui beratnya.");
  Serial.println("Ketik beratnya dalam GRAM lalu ENTER (contoh: 100):");
  
  String input = waitForSerialLine();
  float knownWeight = input.toFloat();
  if (knownWeight <= 0) {
    Serial.println("Berat tidak valid. Kalibrasi batal.");
    return;
  }
  
  Serial.println("Membaca, jangan sentuh load cell...");
  delay(500);
  long rawReading = scale.get_units(15);
  float newFactor = (float)rawReading / knownWeight;
  
  if (newFactor == 0) {
    Serial.println("Gagal menghitung faktor kalibrasi.");
    return;
  }
  
  CALIBRATION_FACTOR = newFactor;
  scale.set_scale(CALIBRATION_FACTOR);
  prefs.putFloat("calfactor", CALIBRATION_FACTOR);
  
  Serial.print("Factor baru disimpan: ");
  Serial.println(CALIBRATION_FACTOR, 4);
  
  Serial.println("\nLANGKAH 3: Verifikasi pembacaan...");
  delay(1000);
  float verify = scale.get_units(10);
  Serial.print("Terbaca: "); Serial.print(verify, 1); Serial.println(" gram");
  
  float errorPercent = fabs(verify - knownWeight) / knownWeight * 100.0;
  Serial.print("Selisih: "); Serial.print(errorPercent, 2); Serial.println(" %");
  
  if (errorPercent <= 5.0) {
    Serial.println(">>> KALIBRASI BERHASIL <<<");
  } else {
    Serial.println(">>> SELISIH MASIH BESAR (>5%) <<<");
  }
}

void setThresholdInteractive() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("SET TRIGGER THRESHOLD");
  Serial.println("========================================");
  Serial.print("Threshold saat ini: ");
  Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1);
  Serial.println(" gram");
  Serial.println("Masukkan threshold baru (gram) lalu ENTER:");
  
  String input = waitForSerialLine();
  float newThreshold = input.toFloat();
  if (newThreshold <= 0) {
    Serial.println("Batal. Threshold tidak berubah.");
    return;
  }
  WEIGHT_TRIGGER_THRESHOLD = newThreshold;
  prefs.putFloat("wthresh", WEIGHT_TRIGGER_THRESHOLD);
  Serial.print("Threshold baru disimpan: ");
  Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1);
  Serial.println(" gram.");
}

void printAutoDetectStatus() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("STATUS AUTO-DETECT");
  Serial.println("========================================");
  Serial.print("Auto-detect       : "); Serial.println(AUTO_DETECT_ENABLED ? "AKTIF" : "NONAKTIF");
  Serial.print("Threshold         : "); Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1); Serial.println(" gram");
  Serial.print("Siklus Aktif Ready: "); Serial.println(readyForNextTrigger ? "YA" : "TIDAK (menunggu buah turun)");
  Serial.print("Berat saat ini    : "); Serial.print(readWeight(), 1); Serial.println(" gram");
}

// ==========================================
// INTEGRASI KAMERA (WEBCAM LEWAT BRIDGE) & PROSES CLASSIFY / SHEET
// ==========================================
bool checkCameraAt(const char* host, const char* label) {
  HTTPClient http;
  String url = "http://" + String(host) + "/status";
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  
  if (!http.begin(url)) return false;
  int code = http.GET();
  http.end();
  return (code == 200);
}

bool captureCameraAt(const char* host, const char* label, const String &captureId) {
  HTTPClient http;
  String url = "http://" + String(host) + "/capture?capture_id=" + captureId;
  http.setConnectTimeout(3000);
  http.setTimeout(15000);
  
  if (!http.begin(url)) return false;
  int code = http.GET();
  String response = http.getString();
  http.end();
  
  if (code != 200 || response.indexOf("\"capture_id\"") < 0) {
    return false;
  }
  return true;
}

bool triggerBothCameras(const String &captureId) {
  Serial.println();
  Serial.println("========================================");
  Serial.print("TRIGGER CAPTURE DUA KAMERA (ID: ");
  Serial.println(captureId + ")");
  Serial.println("========================================");
  
  bool okTop = captureCameraAt(CAM_TOP_HOST, "ATAS", captureId);
  if (!okTop) {
    delay(500);
    okTop = captureCameraAt(CAM_TOP_HOST, "ATAS", captureId);
  }
  
  bool okSide = captureCameraAt(CAM_SIDE_HOST, "SAMPING", captureId);
  if (!okSide) {
    delay(500);
    okSide = captureCameraAt(CAM_SIDE_HOST, "SAMPING", captureId);
  }
  
  Serial.print("CAM ATAS   : "); Serial.println(okTop ? "OK" : "GAGAL");
  Serial.print("CAM SAMPING: "); Serial.println(okSide ? "OK" : "GAGAL");
  
  // Return true kalau MINIMAL SATU kamera berhasil -- itu cukup buat bridge
  // punya foto buat diklasifikasi (bridge akan lapor pakai sisi yang ada
  // kalau sisi satunya tidak nyusul dalam GRADING_PAIR_WINDOW_SECONDS).
  // Kalau KEDUANYA gagal, tidak ada foto sama sekali yang terkirim ke
  // bridge -- jadi TIDAK ADA GUNANYA nunggu /classify, itu pasti tidak
  // akan pernah datang.
  return okTop || okSide;
}

// fetchImageFromCamOnce()/fetchImageFromCam() DIHAPUS -- ESP32 tidak lagi
// mengunduh foto dari bridge. Bridge kirim foto langsung ke cloud ML sendiri.

// getCloudAPIRootURL()/keepCloudAPIWarm() DIHAPUS -- keep-alive cloud ML
// sekarang tanggung jawab camera_bridge_server.py.

// classifyWithCloudAPI() DIHAPUS -- klasifikasi sekarang dilakukan laptop
// (camera_bridge_server.py), hasilnya dilaporkan balik lewat POST /classify.

// worseGrade() DIHAPUS -- grade akhir (terjelek dari 2 sisi) sekarang
// dihitung di camera_bridge_server.py, dikirim sudah jadi lewat /classify.

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

int postJsonFollowRedirectManual(const String &url, const String &body, String &outResponse) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  http.setConnectTimeout(8000);
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  
  if (!http.begin(secureClient, url)) {
    outResponse = "";
    return -1;
  }
  
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  
  if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
    String location = http.getLocation();
    http.end();
    
    if (location.length() == 0) {
      outResponse = "";
      return code;
    }
    
    HTTPClient http2;
    WiFiClientSecure secureClient2;
    secureClient2.setInsecure();
    http2.setConnectTimeout(8000);
    http2.setTimeout(20000);
    
    if (!http2.begin(secureClient2, location)) {
      outResponse = "";
      return -1;
    }
    
    int code2 = http2.GET();
    outResponse = http2.getString();
    http2.end();
    return code2;
  }
  
  outResponse = http.getString();
  http.end();
  return code;
}

void logToGoogleSheet(unsigned long fruitIdVal, char gradeVal, float weightVal, String buahVal, const String &captureId) {
  if (!internetReady) return;
  if (SHEET_WEBAPP_URL.indexOf("GANTI_DENGAN_URL") >= 0) return;
  if (gradeVal != 'A' && gradeVal != 'B') return;
  if (buahVal.length() == 0 || captureId.length() == 0) return;
  
  String body;
  body.reserve(220 + buahVal.length() + captureId.length());
  body += "{";
  body += "\"action\":\"grading\",";
  body += "\"key\":\"" + String(SHEET_SECRET_KEY) + "\",";
  body += "\"capture_id\":\"" + jsonEscape(captureId) + "\",";
  body += "\"fruitId\":\"" + String(fruitIdVal) + "\",";
  body += "\"buah\":\"" + jsonEscape(buahVal) + "\",";
  body += "\"grade\":\"" + String(gradeVal) + "\",";
  body += "\"weight\":" + String(weightVal, 1);
  body += "}";
  
  String response;
  postJsonFollowRedirectManual(SHEET_WEBAPP_URL, body, response);
}

// ==========================================
// OPERASI ALUR BUAH UTAMA (PROSES SIKLUS)
// ==========================================
void handleFruitArrival() {
  fruitID++;
  currentCaptureId = String(fruitID) + "_" + String(millis());
  
  Serial.println();
  Serial.println("########################################");
  Serial.print("BUAH TERDETEKSI - ID: "); Serial.println(fruitID);
  Serial.println("########################################");
  
  lcdShow("Buah #" + String(fruitID), "Menimbang...");
  float weight = readWeight();
  
  lcdShow("Berat: " + String(weight, 1) + "g", "Memotret...");
  bool anyCamOk = triggerBothCameras(currentCaptureId);
  buzzerBeep(1, 150);
  
  gradeResultReady = false;
  gradeResultGrade = 0;
  gradeResultBuah = "";
  gradeResultCaptureId = "";
  
  if (!anyCamOk) {
    // KEDUA kamera gagal -- tidak ada foto sama sekali yang terkirim ke
    // bridge, jadi /classify PASTI tidak akan pernah datang. Langsung
    // lompat ke jalur GAGAL tanpa nunggu 90 detik sia-sia (dulu ini yang
    // bikin satu siklus buah gagal bisa makan >2 menit).
    Serial.println("KEDUA KAMERA GAGAL -- lewati tunggu ML, langsung ke jalur GAGAL.");
    lcdShow("Buah #" + String(fruitID), "Kamera gagal!");
  } else {
    lcdShow("Buah #" + String(fruitID), "Menunggu hasil ML..");
    
    // ----------------------------------------------------------
    // TUNGGU CALLBACK DARI camera_bridge_server.py KE /classify
    // ----------------------------------------------------------
    // PERBAIKAN LOGIC: buah masih diam di stasiun timbang statis (BELUM
    // didorong ke belt) selama menunggu di sini -- jadi aman ditunggu berapa
    // lama pun, termasuk kalau cloud ML (Render) masih cold-start. Push ke
    // belt baru dilakukan SETELAH grade akhir didapat (lihat di bawah), supaya
    // servo sortir bisa dijadwalkan berdasarkan waktu tempuh dari SAAT PUSH,
    // bukan dari kapan ML kebetulan selesai. Ini otomatis mencegah buah
    // didorong ke belt selagi Render belum panas/ready.
    //
    // Bridge yang capture foto kualitas tinggi, kirim ke cloud ML, pasangkan
    // hasil ATAS+SAMPING (pakai capture_id yang sama), hitung grade akhir
    // (terjelek dari 2 sisi) SENDIRI di laptop, lalu POST hasilnya ke /classify.
    // server.handleClient() WAJIB tetap dipanggil di loop tunggu ini supaya
    // request /classify yang masuk sempat diproses.
    unsigned long waitStart = millis();
    while (!gradeResultReady && (millis() - waitStart) < GRADE_WAIT_TIMEOUT_MS) {
      server.handleClient();
      delay(20);
      
      // Cek OTA JUGA di sini, bukan cuma di loop() utama -- soalnya selama
      // fungsi ini berjalan, loop() sepenuhnya diam (tertahan di while ini),
      // jadi kalau cuma mengandalkan loop() untuk cek OTA, update baru bisa
      // sampai 90 detik "tersembunyi" nunggu buah ini kelar. Titik ini AMAN
      // diinterupsi: buah masih diam di stasiun timbang, belum didorong ke
      // belt. Kalau ternyata ada update dan berhasil, ESP32 restart di sini
      // juga (di dalam checkFirmwareUpdate()) -- buah yang sedang diproses
      // otomatis "hilang" dari alur ini, tapi fisiknya tetap aman di tempat;
      // begitu ESP32 nyala lagi, auto-detect akan mendeteksinya ulang selama
      // beratnya masih di atas threshold.
      if (!systemBusy && internetReady && millis() - lastOTACheck >= OTA_CHECK_INTERVAL_MS) {
        lastOTACheck = millis();
        checkFirmwareUpdate();
      }
    }
  }
  
  bool success = gradeResultReady && (gradeResultGrade == 'A' || gradeResultGrade == 'B');
  char grade = success ? gradeResultGrade : 0;
  String buahFinal = gradeResultBuah;
  
  if (!gradeResultReady) {
    Serial.println("TIMEOUT: tidak ada balasan /classify dari bridge dalam " + String(GRADE_WAIT_TIMEOUT_MS / 1000) + " detik.");
  }
  
  char finalGradeForSort;
  if (success) {
    logToGoogleSheet(fruitID, grade, weight, buahFinal, currentCaptureId);
    
    lcdShow(
      (buahFinal.length() ? buahFinal : "Buah"),
      "Final Grade: " + String(grade)
    );
    finalGradeForSort = grade;
  } else {
    Serial.println("KLASIFIKASI GAGAL/TIMEOUT. Buah ke jalur belakang.");
    buzzerBeep(3, 150);
    lcdShow("Buah #" + String(fruitID), "GAGAL! -> Jalur belakang");
    finalGradeForSort = 'C';
  }
  
  // ----------------------------------------------------------
  // PUSH BARU DILAKUKAN DI SINI -- setelah grade akhir diketahui, bukan
  // sebelum. pushStartTime jadi referensi t=0 buat jadwal servo sortir.
  // ----------------------------------------------------------
  lcdShow("Buah #" + String(fruitID), "Push ke conveyor");
  unsigned long pushStartTime = pushToConveyor();
  
  sortFruitTimed(finalGradeForSort, pushStartTime);
  
  currentCaptureId = "";
  buzzerBeep(1, 150);
  showIdleScreen();
}

// ==========================================
// HTTP HANDLERS (WEB SERVER SERVER-SIDE)
// ==========================================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width'><title>Sortir Buah</title></head><body>";
  html += "<h1>SORTIR BUAH ESP32 MAIN</h1><h2>System Online</h2>";
  html += "<p>MAIN IP: " + WiFi.localIP().toString() + "</p>";
  html += "<p>HX711: " + String(hx711Ready ? "READY" : "OFFLINE") + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleStatus() {
  String json = "{";
  json += "\"system\":\"Sortir Buah ESP32 MAIN\",";
  json += "\"status\":\"online\",";
  json += "\"hx711\":" + String(hx711Ready ? "true" : "false") + ",";
  json += "\"weight\":" + String(readWeight(), 1) + ",";
  json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleCapture() {
  String manualCaptureId = "manual_" + String(millis());
  triggerBothCameras(manualCaptureId);
  server.send(200, "application/json", "{\"success\":true}");
}

void handleTrigger() {
  if (systemBusy) {
    server.send(409, "application/json", "{\"success\":false,\"error\":\"system_busy\"}");
    return;
  }
  handleFruitArrival();
  readyForNextTrigger = false;
  server.send(200, "application/json", "{\"success\":true,\"fruit_id\":" + String(fruitID) + "}");
}

// Dipanggil oleh camera_bridge_server.py setelah selesai klasifikasi KEDUA
// foto (ATAS+SAMPING) ke cloud ML dan menghitung grade akhir sendiri.
// Body JSON yang diharapkan: {"capture_id":"3_182233","grade":"B","buah":"jeruk"}
void handleClassify() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"body_missing\"}");
    return;
  }
  String body = server.arg("plain");
  
  Serial.println();
  Serial.println("CLASSIFICATION RECEIVED (dari bridge): " + body);
  
  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, body);
  if (jsonErr) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_json\"}");
    return;
  }
  
  String capId    = doc["capture_id"] | "";
  String gradeStr = doc["grade"]      | "";
  String buah     = doc["buah"]       | "";
  
  if (capId.length() == 0 || gradeStr.length() == 0) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"missing_fields\"}");
    return;
  }
  
  char grade = gradeStr.charAt(0);
  if (grade != 'A' && grade != 'B' && grade != 'C') {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_grade\"}");
    return;
  }
  
  // Tolak kalau capture_id gak cocok dengan buah yang SEDANG ditunggu ESP32
  // sekarang -- mencegah hasil "nyasar" (mis. hasil telat dari buah sebelumnya).
  if (capId != currentCaptureId) {
    Serial.println("GRADE RESULT DITOLAK: capture_id '" + capId + "' tidak cocok dengan yang sedang diproses ('" + currentCaptureId + "').");
    server.send(409, "application/json", "{\"success\":false,\"error\":\"capture_id_mismatch\"}");
    return;
  }
  
  Serial.println("Grade diterima untuk fruit ID " + String(fruitID) + " (capture_id=" + capId + "): buah=" + (buah.length() ? buah : "-") + " grade=" + String(grade));
  
  gradeResultCaptureId = capId;
  gradeResultGrade = grade;
  gradeResultBuah = buah;
  gradeResultReady = true;
  
  server.send(200, "application/json", "{\"success\":true}");
}

// Trigger cek+update firmware manual lewat HTTP (mis. dari browser/HP
// selama ESP32 masih di jaringan lokal yang sama). Untuk update jarak jauh
// otomatis (jaringan mitra yang berbeda), ESP32 tetap cek sendiri secara
// berkala lewat OTA_CHECK_INTERVAL_MS di loop() -- endpoint ini hanya
// mempercepat pengecekan tanpa perlu menunggu jadwal berikutnya.
void handleOTACheck() {
  if (systemBusy) {
    server.send(409, "application/json", "{\"success\":false,\"error\":\"servo_bergerak\"}");
    return;
  }
  checkFirmwareUpdate();
  // Kalau update berhasil, baris di bawah ini tidak akan pernah terkirim
  // karena ESP32 sudah keburu restart -- itu wajar, bukan bug.
  String json = "{\"success\":true,\"current_version\":\"" + String(FIRMWARE_VERSION) + "\",\"result\":\"" + jsonEscape(otaLastResult) + "\"}";
  server.send(200, "application/json", json);
}

void handleOTAStatus() {
  String json = "{\"current_version\":\"" + String(FIRMWARE_VERSION) + "\",\"last_check_result\":\"" + jsonEscape(otaLastResult) + "\",\"check_in_progress\":" + String(otaCheckInProgress ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void testHTTPSDiagnostic() {
  if (!internetReady) return;
  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  
  if (http.begin(secureClient, "https://example.com/")) {
    int code = http.GET();
    http.end();
    Serial.printf("TES HTTPS: CODE %d\n", code);
  }
}

// ==========================================
// OTA (UPDATE FIRMWARE JARAK JAUH VIA INTERNET)
// ==========================================
// Cara kerja: ESP32 secara berkala (dan sekali tiap boot) mengambil file
// version.json dari GitHub. Kalau versi di sana LEBIH BARU dari
// FIRMWARE_VERSION yang sedang jalan, ESP32 otomatis download file .bin
// yang ditunjuk, flash sendiri ke partition OTA, lalu restart. Ini bekerja
// di JARINGAN WIFI APAPUN (bukan cuma WiFi saat development) selama ESP32
// bisa akses internet -- cocok untuk update jarak jauh setelah alat
// diserahkan ke mitra dan disambungkan ke WiFi mitra sendiri.
//
// AMAN dari "brick": ESP32 (dual OTA partition) mendownload firmware baru
// ke partition CADANGAN, bukan menimpa partition yang sedang jalan. Kalau
// download gagal/putus/checksum salah, ESP32 TETAP boot ke firmware LAMA
// yang masih utuh -- asal Partition Scheme di Arduino IDE yang dipakai saat
// upload PERTAMA KALI mendukung OTA (lihat catatan di akhir chat).
//
// Format ota/version.json yang harus disediakan di GitHub:
// {
//   "version": "1.0.1",
//   "url": "https://raw.githubusercontent.com/USERNAME/REPO/main/ota/firmware.bin",
//   "notes": "keterangan perubahan (opsional, tidak dipakai ESP32)"
// }

// Bandingkan versi format "MAJOR.MINOR.PATCH". Return >0 kalau versi b
// lebih baru dari a, 0 kalau sama, <0 kalau a >= b.
int compareVersions(const String &a, const String &b) {
  int aParts[3] = {0, 0, 0};
  int bParts[3] = {0, 0, 0};

  int idx = 0, start = 0;
  for (unsigned int i = 0; i <= a.length() && idx < 3; i++) {
    if (i == a.length() || a.charAt(i) == '.') {
      aParts[idx++] = a.substring(start, i).toInt();
      start = i + 1;
    }
  }
  idx = 0; start = 0;
  for (unsigned int i = 0; i <= b.length() && idx < 3; i++) {
    if (i == b.length() || b.charAt(i) == '.') {
      bParts[idx++] = b.substring(start, i).toInt();
      start = i + 1;
    }
  }

  for (int i = 0; i < 3; i++) {
    if (bParts[i] != aParts[i]) return bParts[i] - aParts[i];
  }
  return 0;
}

void onOTAProgress(size_t done, size_t total) {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint < 500) return;
  lastPrint = millis();
  int pct = (total > 0) ? (int)((done * 100UL) / total) : 0;
  Serial.printf("OTA DOWNLOAD: %d%% (%u/%u bytes)\n", pct, (unsigned)done, (unsigned)total);
  lcdShow("Update Firmware", String(pct) + "% terunduh...");
}

void checkFirmwareUpdate() {
  if (otaCheckInProgress) {
    Serial.println("OTA: pengecekan lain sedang berjalan, dilewati.");
    return;
  }
  if (!internetReady || WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA: tidak ada internet, cek dilewati.");
    otaLastResult = "Dilewati: tidak ada internet.";
    return;
  }
  if (systemBusy) {
    // systemBusy HANYA true selagi servo benar-benar sedang bergerak
    // (dorong buah / buka-tutup gerbang sortir) -- itu blocking delay() di
    // level fisik, jadi memang tidak bisa diinterupsi di titik ini. Fase
    // lain (menimbang, menunggu hasil ML) TIDAK menahan OTA lagi.
    Serial.println("OTA: servo sedang bergerak, cek ditunda sebentar.");
    otaLastResult = "Dilewati: servo sedang bergerak.";
    return;
  }

  otaCheckInProgress = true;
  Serial.println();
  Serial.println("========================================");
  Serial.println("OTA: CEK VERSI FIRMWARE TERBARU");
  Serial.println("========================================");
  Serial.print("Versi terpasang saat ini : "); Serial.println(FIRMWARE_VERSION);

  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  http.setConnectTimeout(10000);
  http.setTimeout(15000);

  if (!http.begin(secureClient, OTA_VERSION_URL)) {
    Serial.println("OTA: gagal memulai koneksi ke version.json.");
    otaLastResult = "Gagal konek ke server versi.";
    otaCheckInProgress = false;
    return;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("OTA: gagal ambil version.json, HTTP code %d\n", code);
    otaLastResult = "HTTP error " + String(code) + " saat cek versi.";
    http.end();
    otaCheckInProgress = false;
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, payload);
  if (jsonErr) {
    Serial.println("OTA: version.json tidak valid (JSON error).");
    otaLastResult = "version.json tidak valid/rusak.";
    otaCheckInProgress = false;
    return;
  }

  String remoteVersion = doc["version"] | "";
  String firmwareUrl   = doc["url"]     | "";

  if (remoteVersion.length() == 0 || firmwareUrl.length() == 0) {
    Serial.println("OTA: version.json tidak lengkap (butuh field 'version' dan 'url').");
    otaLastResult = "version.json tidak lengkap.";
    otaCheckInProgress = false;
    return;
  }

  Serial.print("Versi di server (GitHub) : "); Serial.println(remoteVersion);

  if (compareVersions(FIRMWARE_VERSION, remoteVersion) <= 0) {
    Serial.println("OTA: sudah pakai versi terbaru, tidak ada update.");
    otaLastResult = "Sudah versi terbaru (" + String(FIRMWARE_VERSION) + ").";
    otaCheckInProgress = false;
    return;
  }

  Serial.println(">>> UPDATE TERSEDIA -- MULAI DOWNLOAD & FLASH <<<");
  otaLastResult = "Sedang update ke v" + remoteVersion + " ...";
  buzzerBeep(2, 200);
  lcdShow("Update Firmware!", "v" + remoteVersion + " ditemukan");
  delay(1500);

  WiFiClientSecure otaClient;
  otaClient.setInsecure();

  httpUpdate.setLedPin(-1);
  httpUpdate.onProgress(onOTAProgress);
  httpUpdate.rebootOnUpdate(false); // restart manual di bawah, biar sempat kasih feedback ke OLED/buzzer

  t_httpUpdate_return ret = httpUpdate.update(otaClient, firmwareUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA GAGAL (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      otaLastResult = "Update GAGAL: " + httpUpdate.getLastErrorString();
      lcdShow("Update GAGAL", "Lanjut firmware lama");
      buzzerBeep(4, 150);
      delay(2000);
      showIdleScreen();
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: server bilang tidak ada update.");
      otaLastResult = "Tidak ada update di server.";
      break;

    case HTTP_UPDATE_OK:
      Serial.println("OTA: BERHASIL. Restart ke firmware baru...");
      otaLastResult = "Berhasil update ke v" + remoteVersion + ", restart...";
      lcdShow("Update BERHASIL!", "Restarting...");
      buzzerBeep(3, 100);
      delay(2000);
      ESP.restart();
      break;
  }

  otaCheckInProgress = false;
}

// ==========================================
// SERIAL INTERACTIVE INTERFACE
// ==========================================
void printMenu() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("COMMAND MENU (ketik lalu ENTER)");
  Serial.println("========================================");
  Serial.println("c = cek kamera online");
  Serial.println("p = trigger capture");
  Serial.println("w = baca berat");
  Serial.println("t = tare load cell");
  Serial.println("k = kalibrasi load cell");
  Serial.println("u = gerak servo PUSH (test)");
  Serial.println("1 = sortir ke A | 2 = sortir ke B | 3 = sortir ke C");
  Serial.println("r = semua servo ke idle");
  Serial.println("z = test buzzer");
  Serial.println("d = cek status auto-detect");
  Serial.println("a = atur threshold berat");
  Serial.println("e = toggle auto-detect");
  Serial.println("f = trigger simulasi alur buah penuh");
  Serial.println("s = status sistem");
  Serial.println("y = tes HTTPS diagnosa");
  Serial.println("o = cek & update firmware sekarang (OTA)");
  Serial.println("x = hapus wifi + restart");
  Serial.println("========================================");
}

void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;
  
  char c = cmd.charAt(0);
  switch (c) {
    case 'c': case 'C':
      camTopOnline = checkCameraAt(CAM_TOP_HOST, "ATAS");
      camSideOnline = checkCameraAt(CAM_SIDE_HOST, "SAMPING");
      Serial.printf("CAM ATAS: %s | CAM SAMPING: %s\n", camTopOnline?"OK":"OFF", camSideOnline?"OK":"OFF");
      break;
    case 'p': case 'P':
      triggerBothCameras(String(fruitID) + "_" + String(millis()));
      break;
    case 'w': case 'W':
      Serial.printf("BERAT: %.1fg\n", readWeight());
      break;
    case 't': case 'T':
      tareLoadCell();
      break;
    case 'k': case 'K':
      calibrateLoadCell();
      break;
    case 'u': case 'U':
      pushToConveyor();
      break;
    case '1': sortFruit('A'); break;
    case '2': sortFruit('B'); break;
    case '3': sortFruit('C'); break;
    case 'r': case 'R':
      servoPush.write(servoPhysicalAngle(PUSH_IDLE_ANGLE, PUSH_SERVO_REVERSED));
      servoSort.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO1_REVERSED));
      servoSort2.write(servoPhysicalAngle(SORT_IDLE_ANGLE, SORT_SERVO2_REVERSED));
      Serial.println("Semua servo kembali ke idle.");
      break;
    case 'z': case 'Z':
      buzzerBeep(2, 200);
      break;
    case 'd': case 'D':
      printAutoDetectStatus();
      break;
    case 'a': case 'A':
      setThresholdInteractive();
      break;
    case 'e': case 'E':
      AUTO_DETECT_ENABLED = !AUTO_DETECT_ENABLED;
      weightAboveThreshold = false;
      Serial.printf("AUTO_DETECT: %s\n", AUTO_DETECT_ENABLED?"AKTIF":"NONAKTIF");
      break;
    case 'f': case 'F':
      handleFruitArrival();
      readyForNextTrigger = false;
      break;
    case 's': case 'S':
      printAutoDetectStatus();
      break;
    case 'y': case 'Y':
      testHTTPSDiagnostic();
      break;
    case 'o': case 'O':
      checkFirmwareUpdate();
      Serial.println("HASIL OTA: " + otaLastResult);
      break;
    case 'x': case 'X':
      wm.resetSettings();
      delay(1000);
      ESP.restart();
      break;
    default:
      printMenu();
      break;
  }
}

bool runComponentSelfTest() {
  Serial.println("\n--- SELF TEST ---");
  bool ok = hx711Ready && lcdReady;
  Serial.printf("HX711: %s | OLED: %s\n", hx711Ready?"OK":"FAIL", lcdReady?"OK":"FAIL");
  return ok;
}

// ==========================================
// SETUP & LOOP (ARDUINO ENTRY POINTS)
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  
  setupLCD();
  setupHX711();
  setupWiFi();
  
  // Cek update firmware SEKALI setiap boot, sebelum lanjut ke inisialisasi
  // servo -- supaya kalau ada versi baru di GitHub, ESP32 langsung update
  // duluan (dan restart otomatis) tanpa perlu menunggu jadwal
  // OTA_CHECK_INTERVAL_MS di loop(). Kalau tidak ada internet/tidak ada
  // update, fungsi ini langsung return dan setup lanjut seperti biasa.
  if (internetReady) {
    checkFirmwareUpdate();
  }
  lastOTACheck = millis();
  
  setupServos();
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/trigger", HTTP_GET, handleTrigger);
  server.on("/classify", HTTP_POST, handleClassify);
  server.on("/ota/check", HTTP_GET, handleOTACheck);
  server.on("/ota/status", HTTP_GET, handleOTAStatus);
  server.begin();
  
  runComponentSelfTest();
  
  if (internetReady) {
    buzzerBeep(1, 150);
  }
  
  printMenu();
  showIdleScreen();
}

void loop() {
  server.handleClient();
  handleSerial();
  
  // Reconnect WiFi jika terputus
  if (!systemBusy && millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheck = millis();
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    if (!nowConnected) {
      internetReady = false;
      WiFi.reconnect();
    } else if (!internetReady) {
      internetReady = true;
      showIdleScreen();
    }
  }
  
  // Cek Kamera secara berkala
  if (!systemBusy && internetReady && millis() - lastCamCheck >= CAM_CHECK_INTERVAL_MS) {
    lastCamCheck = millis();
    bool prevTop = camTopOnline;
    bool prevSide = camSideOnline;
    camTopOnline  = checkCameraAt(CAM_TOP_HOST, "ATAS");
    camSideOnline = checkCameraAt(CAM_SIDE_HOST, "SAMPING");
    if (camTopOnline != prevTop || camSideOnline != prevSide) {
      // Status berubah -- update layar OLED cuma kalau lagi di layar idle
      // (tidak lagi menimbang/proses buah), biar tidak menimpa layar lain.
      if (!weightAboveThreshold) {
        showIdleScreen();
      }
    }
  }
  
  // Blok keep-alive cloud API DIHAPUS -- itu sekarang tanggung jawab
  // camera_bridge_server.py, ESP32 tidak lagi memanggil cloud ML sama sekali.
  
  // Cek update firmware secara BERKALA & OTOMATIS (tiap OTA_CHECK_INTERVAL_MS,
  // default 30 detik). Ini yang membuat update "jarak jauh" beneran otomatis
  // -- ESP32 akan menemukan firmware baru sendiri hampir seketika kapan saja
  // dia online, di jaringan WiFi APAPUN (termasuk WiFi mitra yang beda dari
  // saat development), tanpa perlu ada orang yang trigger manual dari HP/
  // laptop. (Titik cek TAMBAHAN untuk saat sedang menunggu hasil ML ada di
  // dalam handleFruitArrival(), bukan di sini -- lihat komentar di sana.)
  if (!systemBusy && internetReady &&
      millis() - lastOTACheck >= OTA_CHECK_INTERVAL_MS) {
    lastOTACheck = millis();
    checkFirmwareUpdate();
  }
  
  // Deteksi Otomatis Berat Buah
  if (AUTO_DETECT_ENABLED && hx711Ready && !systemBusy) {
    float w = readWeight();
    if (w > WEIGHT_TRIGGER_THRESHOLD) {
      if (readyForNextTrigger) {
        if (!weightAboveThreshold) {
          weightAboveThreshold = true;
          weightAboveSince = millis();
          weightStableMin = w;
          weightStableMax = w;
          lastStabilizeFeedback = millis();
          lcdShow("Ada beban..", "Menimbang...");
        } else {
          if (w < weightStableMin) weightStableMin = w;
          if (w > weightStableMax) weightStableMax = w;
          
          bool stillFluctuating = (weightStableMax - weightStableMin) > WEIGHT_STABLE_TOLERANCE_G;
          if (stillFluctuating) {
            weightAboveSince = millis();
            weightStableMin = w;
            weightStableMax = w;
          }
          
          if (millis() - lastStabilizeFeedback >= STABILIZE_FEEDBACK_MS) {
            lastStabilizeFeedback = millis();
            unsigned long elapsedSec = (millis() - weightAboveSince) / 1000;
            lcdShow("Menimbang " + String(w, 0) + "g", "Stabil " + String(elapsedSec) + "/3 dtk");
            buzzerBeep(1, 40);
          }
          
          if (millis() - weightAboveSince >= WEIGHT_STABLE_MS) {
            handleFruitArrival();
            weightAboveThreshold = false;
            readyForNextTrigger = false;
          }
        }
      }
    } else {
      if (weightAboveThreshold) {
        showIdleScreen();
      }
      weightAboveThreshold = false;
      readyForNextTrigger = true;
    }
  }
  delay(5);
}
