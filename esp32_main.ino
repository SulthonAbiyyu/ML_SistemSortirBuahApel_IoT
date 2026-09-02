#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
// ^ Wajib untuk request HTTPS (cloud API Render.com & Google Apps Script).
//   Tanpa ini, HTTPClient::begin(url) ke alamat https:// akan gagal
//   konek dan HTTP CODE selalu -1, walau internet & hotspot normal.
#include <WiFiManager.h>
// ^ REVISI: dipakai supaya ganti WiFi CUKUP LEWAT HP (tanpa Arduino
//   IDE/laptop sama sekali). Kalau belum ada, install lewat: Arduino
//   IDE > Tools > Manage Libraries > cari "WiFiManager" by tzapu,
//   pilih yang itu (bukan yang lain, banyak library mirip namanya).
#include <ESPmDNS.h>
// ^ REVISI: dipakai supaya ESP32 MAIN bisa manggil ESP32-CAM pakai
//   NAMA (contoh: camtop.local) bukan ANGKA IP yang harus di-set
//   manual dan gampang salah tiap ganti jaringan WiFi.
#include <ESP32Servo.h>
#include <Preferences.h>   
#include "HX711.h"
#include <ArduinoJson.h>
// ^ REVISI: dipakai untuk parsing JSON dari cloud API secara proper
//   (gantiin cara lama yang cari substring manual pakai indexOf, yang
//   gampang salah kalau format JSON dari server berubah dikit).
//   Kalau belum ada, install lewat: Arduino IDE > Tools > Manage
//   Libraries > cari "ArduinoJson" (by Benoit Blanchon), versi 6 atau 7.
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
// ^ OLED 0.96" SSD1306 (128x64, I2C). Kalau belum ada, install lewat:
//   Arduino IDE > Tools > Manage Libraries > cari "Adafruit SSD1306"
//   (nanti akan diminta install juga "Adafruit GFX Library", terima/install)

// ============================================================
// SORTIR APEL ESP32 MAIN - REVISI TOTAL
// ============================================================
// Alur kerja:
// 1. Load cell mendeteksi berat buah datang (di atas threshold,
//    stabil beberapa ratus ms) -> buzzer bunyi pendek
//    (bisa juga dipicu manual: serial 'f' atau HTTP GET /trigger)
// 2. Berat dibaca dari load cell (HX711)
// 3. ESP32-CAM (atas & samping) dipicu untuk capture gambar
// 4. Servo PUSH mendorong buah dari dudukan ke conveyor
// 5. Sistem eksternal (nanti: ML di laptop/PC) kirim hasil
//    grade (A/B/C) ke endpoint POST /classify
// 6. Servo SORTIR bergerak ke posisi A/B/C untuk menjatuhkan
//    buah ke wadah yang sesuai
// ============================================================


// ============================================================
// WIFI - SEMUA DEVICE (MAIN + 2 CAM) JADI CLIENT DI 1 WIFI YANG SAMA
// ============================================================
// ESP32 MAIN TIDAK LAGI jadi Access Point sendiri (mode AP dibuang
// total). MAIN, CAM ATAS, dan CAM SAMPING sekarang semuanya connect
// sebagai client biasa ke SATU WiFi yang sama.
//
// REVISI BESAR - WiFi SUDAH TIDAK DI-HARDCODE LAGI DI SINI.
// Dulu SSID/password ditulis langsung di kode (harus edit + upload
// ulang tiap ganti WiFi). Sekarang pakai WiFiManager: kalau board
// belum tahu/gagal connect ke WiFi yang tersimpan, dia OTOMATIS
// bikin hotspot sendiri bernama "SORTIR-APEL-MAIN-SETUP". Tinggal:
//   1. Connect HP ke hotspot itu (passwordnya: sortirapel123)
//   2. HP biasanya otomatis munculin halaman pengaturan (kalau
//      tidak, buka browser HP, ke alamat 192.168.4.1)
//   3. Pilih nama WiFi yang mau dipakai + masukin passwordnya
//   4. ESP32 MAIN simpan otomatis & langsung connect
// Tidak perlu Arduino IDE / laptop sama sekali untuk ganti WiFi.
// Kalau nanti pindah ke jaringan baru dan WiFi lama sudah tidak
// kejangkau, board ini OTOMATIS masuk mode setup lagi sendiri saat
// nyala (tidak perlu tombol apa-apa).
const char* WIFI_SETUP_AP_NAME     = "SORTIR-APEL-MAIN-SETUP";
const char* WIFI_SETUP_AP_PASSWORD = "sortirapel123"; // min. 8 karakter

WiFiManager wm;

// ============================================================
// IP - SUDAH TIDAK STATIS LAGI, PAKAI DHCP OTOMATIS
// ============================================================
// Dulu di sini ada IP statis (MAIN_IP, GATEWAY, SUBNET) yang harus
// diganti manual tiap pindah jaringan (subnet hotspot Android beda
// dengan iPhone, beda lagi dengan WiFi kampus/tempat lain). Sekarang
// ESP32 MAIN minta IP otomatis ke jaringan manapun (persis kayak HP
// connect WiFi biasa), jadi baris² itu sudah tidak diperlukan sama
// sekali - device ini dicari lewat mDNS ("main.local"), bukan IP.


// ============================================================
// >>> WAJIB DIISI: URL API CLOUD ML (dari Render.com) <<<
// ============================================================
// Setelah deploy FastAPI ke Render, Anda akan dapat URL seperti:
// https://sortir-apel-grade-api-xxxx.onrender.com
// Tambahkan "/predict" di belakangnya seperti contoh di bawah.

String CLOUD_API_URL = "https://ml-sistemsortirbuahapel-iot.onrender.com/predict";


// ============================================================
// >>> WAJIB DIISI: URL GOOGLE APPS SCRIPT (LOG KE GOOGLE SHEETS) <<<
// ============================================================
// Ikuti langkah setup Apps Script di penjelasan chat untuk dapat URL
// ini. Bentuknya seperti:
// https://script.google.com/macros/s/xxxxxxxxxxxxxxxxxxxx/exec

String SHEET_WEBAPP_URL = "https://script.google.com/macros/s/AKfycbxXaqN60uyMrsx000K40tFXMIs4NVsxpNYME9OlSjjoI-WDjyLVXw7W87ir9ZHUSyF7vA/exec";

// Kunci rahasia sederhana biar endpoint Apps Script kamu nggak
// sembarangan bisa dipakai orang lain (harus SAMA PERSIS dengan
// SECRET_KEY di kode Apps Script).
const char* SHEET_SECRET_KEY = "kelompokPKM";


// ============================================================
// ESP32-CAM (2 unit: ATAS & SAMPING)
// ============================================================
// Kedua ESP32-CAM connect ke WiFi yang SAMA dengan MAIN (diatur lewat
// hotspot setup masing-masing, sama seperti MAIN). Mereka TIDAK
// dikabel ke ESP32 MAIN sama sekali (kecuali kabel daya). Komunikasi
// 100% lewat WiFi (HTTP).
//
// REVISI: dulu ini IP statis (harus SAMA PERSIS dengan IP yang
// di-set manual di esp32camATAS.ino). Sekarang pakai NAMA (mDNS),
// jadi walaupun IP kamera berubah-ubah tiap connect WiFi baru
// (karena sekarang DHCP, bukan statis), ESP32 MAIN tetap bisa
// nemuin kamera lewat namanya. Nama ini HARUS SAMA PERSIS dengan
// MDNS_NAME yang di-set di esp32camATAS.ino untuk masing-masing unit.
const char* CAM_TOP_HOST  = "camtop.local";   // ESP32-CAM sisi ATAS
const char* CAM_SIDE_HOST = "camside.local";  // ESP32-CAM sisi SAMPING (atau laptop bridge, lihat camera_bridge_server.py)


// ============================================================
// WEB SERVER
// ============================================================

WebServer server(80);


// ============================================================
// PIN MAPPING (LIHAT PENJELASAN WIRING DI CHAT / DOKUMENTASI)
// ============================================================
// Semua pin di bawah ini merujuk ke label GPIO pada board
// extension/screw-terminal yang sudah terpasang di ESP32 devkit.
// Cari terminal dengan label angka yang SAMA (mis. pin 4 -> cari
// terminal berlabel "4" atau "D4" atau "GPIO4").

// ---- HX711 (Load Cell Amplifier) ----
#define HX711_DT_PIN   4     // DOUT/DT HX711 -> GPIO4
#define HX711_SCK_PIN  5     // SCK/CLK HX711 -> GPIO5
// VCC HX711 -> pin "3V3" | GND HX711 -> pin "GND" (lihat chat)

// ---- Servo 1: PUSH (dorong buah dari dudukan ke conveyor) ----
#define SERVO_PUSH_PIN 13

// ---- Servo 2: SORTIR (arahkan buah ke grade A/B/C) ----
#define SERVO_SORT_PIN 14

// ---- Buzzer (1 unit) ----
#define BUZZER_PIN     25

// ---- OLED 0.96" SSD1306 (128x64, I2C) ----
// OLED cuma butuh 2 pin data: SDA & SCL. Pakai pin I2C default ESP32
// (GPIO21/22) supaya tidak bentrok dengan komponen lain
// (load cell = GPIO4/5, servo = GPIO13/14, buzzer = GPIO25).
#define OLED_SDA_PIN     21
#define OLED_SCL_PIN     22
// VCC OLED -> pin "3V3" (JANGAN 5V, kebanyakan modul OLED cuma tahan 3.3V!)
// GND OLED -> pin "GND"

// Alamat I2C OLED: paling umum 0x3C, sebagian modul pakai 0x3D.
// Kalau layar tidak nyala sama sekali, coba ganti ke 0x3D, atau jalankan
// "I2C Scanner" sketch bawaan contoh Arduino untuk cek alamat aslinya.
#define OLED_I2C_ADDRESS 0x3C
#define OLED_WIDTH       128
#define OLED_HEIGHT      64
#define OLED_RESET_PIN   -1   // -1 = pakai reset pin bareng ESP32 (tidak ada pin reset fisik terpisah)


// ============================================================
// HX711 - LOAD CELL
// ============================================================

HX711 scale;
Preferences prefs;

bool hx711Ready = false;

// Nilai default sebelum kalibrasi. Setelah kalibrasi lewat menu
// serial 'k', nilai hasil kalibrasi disimpan otomatis ke memori
// permanen ESP32 (NVS) - TIDAK perlu edit & upload ulang kode.
float CALIBRATION_FACTOR = 1.0;


// ============================================================
// DETEKSI BUAH OTOMATIS VIA LOAD CELL (pengganti sensor IR)
// ============================================================
// Buah dianggap "datang" kalau berat terbaca melewati threshold
// ini dan bertahan stabil selama WEIGHT_STABLE_MS berturut-turut
// (supaya getaran/goyangan sesaat tidak dianggap buah).
// Threshold bisa diubah & disimpan permanen lewat menu serial 'a'.

bool AUTO_DETECT_ENABLED = true;

float WEIGHT_TRIGGER_THRESHOLD = 20.0;      // gram, default awal
const unsigned long WEIGHT_STABLE_MS = 300;  // lama berat harus stabil di atas threshold

bool weightAboveThreshold = false;
unsigned long weightAboveSince = 0;

// Wajib TRUE dulu sebelum auto-detect boleh trigger buah baru. Jadi FALSE
// begitu 1 buah selesai di-trigger (manual 'f'/HTTP maupun otomatis), dan
// baru balik TRUE lagi kalau berat sudah turun ke bawah threshold (artinya
// buah/benda sebelumnya sudah benar-benar tidak ada di load cell). Tanpa
// ini, benda yang masih nangkring di load cell setelah trigger akan terus
// dianggap "buah baru" berulang-ulang selama beratnya masih di atas
// threshold -> auto-detect trigger terus-menerus tanpa henti.
bool readyForNextTrigger = true;


// ============================================================
// SERVO
// ============================================================

Servo servoPush;
Servo servoSort;

// ============================================================
// OLED
// ============================================================

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
bool lcdReady = false;   // nama variabel dibiarkan "lcdReady" biar tidak perlu ubah nama di tempat lain

// --- Servo PUSH ---
const int PUSH_IDLE_ANGLE   = 0;    // posisi diam/standby
const int PUSH_ACTIVE_ANGLE = 90;   // posisi mendorong buah
const int PUSH_HOLD_MS      = 600;  // lama posisi mendorong sebelum kembali

// --- Servo SORTIR ---
// Mekanisme: servo ini menutup/membuka JALUR conveyor secara bertahap.
// - Posisi netral (0 derajat)   = menutup jalur, buah tertahan di depan servo.
// - Geser sedikit (grade A)     = buah bagus terdorong jatuh ke box A.
// - Geser lebih jauh (grade B)  = buah jelek terdorong jatuh ke box B (di sebelah box A).
// - Buka penuh (gagal deteksi)  = jalur terbuka penuh, buah lewat terus ke
//                                  belakang dan jatuh ke box C (buah gagal dikenali).
//
// PENTING: Angka derajat di bawah ini masih PERKIRAAN AWAL. Setelah upload,
// tes manual dulu pakai serial monitor (ketik 'r' untuk lihat posisi idle,
// lalu '1'/'2'/'3' untuk lihat posisi A/B/C satu-satu) sambil lihat langsung
// pergerakan fisiknya. Kalau buah belum jatuh ke box yang benar, ubah angka
// SORT_A_ANGLE / SORT_B_ANGLE / SORT_C_ANGLE di bawah sampai pas, lalu upload ulang.
const int SORT_IDLE_ANGLE = 0;      // posisi netral/default: MENUTUP jalur conveyor
const int SORT_A_ANGLE    = 20;     // geser sedikit -> buah grade A jatuh ke box A
const int SORT_B_ANGLE    = 45;     // geser lebih jauh -> buah grade B jatuh ke box B
const int SORT_C_ANGLE    = 90;     // buka penuh -> buah gagal deteksi lewat ke box C
const int SORT_HOLD_MS    = 800;    // lama posisi sortir sebelum kembali menutup (idle)


// ============================================================
// STATE
// ============================================================

bool camTopOnline  = false;
bool camSideOnline = false;

bool systemBusy = false;

unsigned long fruitID = 0;


// ============================================================
// SETUP WIFI
// ============================================================

bool internetReady = false;

void setupWiFi()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("START WIFI (WiFiManager - ganti WiFi lewat HP)");
  Serial.println("========================================");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // ---- Tombol BOOT ditekan saat nyala -> paksa buka setup WiFi ----
  // Kebanyakan board ESP32 DevKit punya tombol fisik berlabel "BOOT"
  // di GPIO0. Kalau ditekan & ditahan pas board baru nyala/di-reset,
  // WiFi yang tersimpan akan DIHAPUS, jadi board otomatis masuk mode
  // setup lagi (hotspot SORTIR-APEL-MAIN-SETUP muncul), walaupun WiFi
  // lama sebenarnya masih bisa dijangkau. Berguna kalau kamu MAU
  // ganti WiFi padahal WiFi lamanya masih nyala/kejangkau.
  pinMode(0, INPUT_PULLUP);
  delay(50);
  if (digitalRead(0) == LOW)
  {
    Serial.println("Tombol BOOT ditekan saat nyala -> hapus WiFi tersimpan.");
    wm.resetSettings();
  }

  // Portal setup otomatis nutup sendiri kalau 3 menit tidak diisi,
  // supaya board tidak "nyangkut" selamanya nunggu HP nyambung -
  // nanti dicoba lagi otomatis di boot berikutnya / oleh loop().
  wm.setConfigPortalTimeout(180);

  // autoConnect() otomatis: (1) coba connect ke WiFi tersimpan kalau
  // ada, (2) kalau gagal/belum ada, buka hotspot setup sendiri dan
  // TUNGGU sampai dikonfigurasi lewat HP atau sampai timeout di atas.
  bool ok = wm.autoConnect(WIFI_SETUP_AP_NAME, WIFI_SETUP_AP_PASSWORD);

  if (ok)
  {
    internetReady = true;
    Serial.println("WIFI: TERSAMBUNG");
    Serial.print("IP ESP32 MAIN : "); Serial.println(WiFi.localIP());
  }
  else
  {
    internetReady = false;
    Serial.println("WIFI: BELUM TERSAMBUNG (setup portal timeout 3 menit).");
    Serial.print("Nyalakan ulang board, lalu connect HP ke hotspot \""); Serial.print(WIFI_SETUP_AP_NAME);
    Serial.println("\" untuk memilih WiFi.");
  }

  // ---- mDNS: supaya device lain bisa manggil board ini via nama ----
  if (MDNS.begin("main"))
  {
    Serial.println("mDNS aktif -> board ini bisa dipanggil sebagai: main.local");
  }
  else
  {
    Serial.println("mDNS GAGAL start (tidak fatal, board tetap jalan).");
  }

  Serial.println("----------------------------------------");
}


// ============================================================
// HX711 - SETUP + LOAD KALIBRASI DARI NVS
// ============================================================

void setupHX711()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("HX711");
  Serial.println("========================================");

  prefs.begin("siambali", false);
  CALIBRATION_FACTOR = prefs.getFloat("calfactor", 1.0);
  WEIGHT_TRIGGER_THRESHOLD = prefs.getFloat("wthresh", WEIGHT_TRIGGER_THRESHOLD);

  Serial.print("Calibration factor tersimpan: ");
  Serial.println(CALIBRATION_FACTOR, 4);
  Serial.print("Weight trigger threshold tersimpan: ");
  Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1);
  Serial.println(" gram");

  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);

  delay(500);

  if (scale.is_ready())
  {
    hx711Ready = true;
    Serial.println("HX711: READY");
    scale.set_scale(CALIBRATION_FACTOR);
    scale.tare(20);
    Serial.println("Tare otomatis saat boot selesai.");
  }
  else
  {
    hx711Ready = false;
    Serial.println("HX711: NOT READY (cek wiring DT/SCK/VCC/GND)");
    Serial.println("Sistem tetap berjalan tanpa load cell.");
  }
}


// ============================================================
// TARE (nolkan timbangan, load cell HARUS kosong)
// ============================================================

void tareLoadCell()
{
  if (!hx711Ready)
  {
    Serial.println("HX711 tidak tersedia, tidak bisa tare.");
    return;
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("TARE LOAD CELL");
  Serial.println("========================================");
  Serial.println("Pastikan load cell KOSONG (tidak ada beban)...");

  delay(2000);

  scale.set_scale(CALIBRATION_FACTOR);
  scale.tare(20);

  Serial.println("TARE SELESAI. Berat sekarang seharusnya ~0.");
}


// ============================================================
// ATUR THRESHOLD DETEKSI BUAH (via Serial Monitor)
// ============================================================

void setThresholdInteractive()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("ATUR WEIGHT TRIGGER THRESHOLD");
  Serial.println("========================================");
  Serial.print("Threshold sekarang: ");
  Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1);
  Serial.println(" gram");
  Serial.println("Masukkan berat minimum (gram) yang dianggap 'ada buah'.");
  Serial.println("Saran: sedikit di atas berat buah paling ringan yang dipakai,");
  Serial.println("misal kalau buah paling ringan ~50g, pakai threshold ~20-30g.");
  Serial.println("Ketik angka baru lalu ENTER (atau ketik 0 untuk batal):");

  String input = waitForSerialLine();
  float newThreshold = input.toFloat();

  if (newThreshold <= 0)
  {
    Serial.println("Dibatalkan, threshold tidak berubah.");
    return;
  }

  WEIGHT_TRIGGER_THRESHOLD = newThreshold;
  prefs.putFloat("wthresh", WEIGHT_TRIGGER_THRESHOLD);

  Serial.print("Threshold baru: ");
  Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1);
  Serial.println(" gram (tersimpan permanen).");
}

void printAutoDetectStatus()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("STATUS DETEKSI OTOMATIS (LOAD CELL)");
  Serial.println("========================================");
  Serial.print("Auto-detect       : ");
  Serial.println(AUTO_DETECT_ENABLED ? "AKTIF" : "NONAKTIF");
  Serial.print("Threshold          : ");
  Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1);
  Serial.println(" gram");
  Serial.print("Stabil minimum     : ");
  Serial.print(WEIGHT_STABLE_MS);
  Serial.println(" ms");
  Serial.print("Siap trigger baru  : ");
  Serial.println(readyForNextTrigger ? "YA" : "TIDAK (menunggu berat turun di bawah threshold)");
  Serial.print("Berat saat ini     : ");
  Serial.print(readWeight(), 1);
  Serial.println(" gram");
}


// ============================================================
// KALIBRASI INTERAKTIF (via Serial Monitor)
// ============================================================
// Langkah kalibrasi ada di penjelasan chat. Fungsi ini menuntun
// user step-by-step lewat Serial Monitor.

String waitForSerialLine()
{
  while (!Serial.available())
  {
    delay(10);
  }
  String line = Serial.readStringUntil('\n');
  line.trim();
  return line;
}

void calibrateLoadCell()
{
  if (!hx711Ready)
  {
    Serial.println("HX711 tidak tersedia, tidak bisa kalibrasi.");
    return;
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("MODE KALIBRASI LOAD CELL");
  Serial.println("========================================");

  Serial.println("LANGKAH 1: Kosongkan load cell sepenuhnya.");
  Serial.println("Ketik apa saja lalu ENTER jika sudah kosong...");
  waitForSerialLine();

  scale.set_scale(1.0);
  scale.tare(20);
  Serial.println("Tare selesai (raw = 0 saat kosong).");

  Serial.println();
  Serial.println("LANGKAH 2: Taruh benda dengan berat yang SUDAH");
  Serial.println("kamu ketahui pasti (misal koin/anak timbangan).");
  Serial.println("Ketik beratnya dalam GRAM lalu ENTER (contoh: 100)");

  String input = waitForSerialLine();
  float knownWeight = input.toFloat();

  if (knownWeight <= 0)
  {
    Serial.println("Berat tidak valid. Kalibrasi dibatalkan.");
    return;
  }

  Serial.println("Membaca raw value, jangan sentuh load cell...");
  delay(500);

  long rawReading = scale.get_units(15);

  Serial.print("Raw reading: ");
  Serial.println(rawReading);

  float newFactor = (float)rawReading / knownWeight;

  if (newFactor == 0)
  {
    Serial.println("Gagal menghitung faktor kalibrasi (hasil 0).");
    Serial.println("Cek wiring HX711, lalu ulangi kalibrasi.");
    return;
  }

  CALIBRATION_FACTOR = newFactor;
  scale.set_scale(CALIBRATION_FACTOR);

  prefs.putFloat("calfactor", CALIBRATION_FACTOR);

  Serial.println();
  Serial.print("Calibration factor baru: ");
  Serial.println(CALIBRATION_FACTOR, 4);
  Serial.println("Sudah tersimpan permanen (tidak hilang saat mati/reset).");

  Serial.println();
  Serial.println("LANGKAH 3 (VERIFIKASI): Biarkan benda tetap di atas");
  Serial.println("load cell, sistem akan membaca ulang beratnya...");
  delay(1000);

  float verify = scale.get_units(10);

  Serial.print("Berat terbaca : ");
  Serial.print(verify, 1);
  Serial.println(" gram");

  Serial.print("Berat asli    : ");
  Serial.print(knownWeight, 1);
  Serial.println(" gram");

  float errorPercent = fabs(verify - knownWeight) / knownWeight * 100.0;

  Serial.print("Selisih       : ");
  Serial.print(errorPercent, 2);
  Serial.println(" %");

  if (errorPercent <= 5.0)
  {
    Serial.println();
    Serial.println(">>> KALIBRASI BERHASIL (selisih <= 5%) <<<");
  }
  else
  {
    Serial.println();
    Serial.println(">>> SELISIH MASIH BESAR (> 5%) <<<");
    Serial.println("Coba ulangi kalibrasi dari LANGKAH 1, pastikan:");
    Serial.println("- Load cell benar-benar kosong saat tare");
    Serial.println("- Berat yang diketik akurat");
    Serial.println("- Load cell terpasang kokoh, tidak goyang");
  }

  Serial.println();
  Serial.println("Angkat beban, ketik 'w' untuk cek apakah kembali ke 0.");
}


// ============================================================
// BACA BERAT
// ============================================================

float readWeight()
{
  // Nilai valid terakhir, disimpan supaya pembacaan tidak "kedip" ke 0.
  // HX711 default cuma menghasilkan data baru ~10x/detik, sedangkan
  // fungsi ini dipanggil jauh lebih sering dari loop() -> tanpa cache,
  // sebagian besar pemanggilan akan kena "belum siap" dan (dulu)
  // langsung dianggap 0, padahal beratnya masih ada, cuma sensornya
  // belum sempat mengukur ulang.
  static float lastValidWeight = 0;

  if (!hx711Ready) return 0;

  if (!scale.is_ready())
  {
    return lastValidWeight;
  }

  float weight = scale.get_units(10);

  if (weight > -2 && weight < 2)
  {
    weight = 0;
  }

  lastValidWeight = weight;
  return weight;
}


// ============================================================
// OLED - SETUP
// ============================================================

void setupLCD()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("OLED I2C (SSD1306)");
  Serial.println("========================================");

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS))
  {
    lcdReady = false;
    Serial.println("OLED: GAGAL DIINISIALISASI");
    Serial.println("Cek: alamat I2C benar 0x3C/0x3D? wiring SDA/SCL benar? VCC 3.3V?");
    return;
  }

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println("SORTIR APEL");
  oled.setCursor(0, 16);
  oled.println("Booting...");
  oled.display();

  lcdReady = true;
  Serial.println("OLED: SIAP");
}

// ============================================================
// OLED - HELPER TAMPILKAN 2 BARIS
// ============================================================
// Dipanggil di titik-titik penting alur proses supaya operator
// bisa lihat status tanpa buka Serial Monitor / laptop.

void lcdShow(String line1, String line2)
{
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


// ============================================================
// SERVO SETUP
// ============================================================

void setupServos()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("SERVO");
  Serial.println("========================================");

  servoPush.setPeriodHertz(50);
  servoSort.setPeriodHertz(50);

  servoPush.attach(SERVO_PUSH_PIN, 500, 2400);
  servoSort.attach(SERVO_SORT_PIN, 500, 2400);

  servoPush.write(PUSH_IDLE_ANGLE);
  servoSort.write(SORT_IDLE_ANGLE);

  Serial.println("SERVO PUSH  : READY (idle)");
  Serial.println("SERVO SORTIR: READY (idle/menutup jalur)");
}


// ============================================================
// SERVO PUSH: dorong buah dari dudukan ke conveyor
// ============================================================

void pushToConveyor()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("PUSH BUAH KE CONVEYOR");
  Serial.println("========================================");

  systemBusy = true;

  servoPush.write(PUSH_ACTIVE_ANGLE);
  delay(PUSH_HOLD_MS);
  servoPush.write(PUSH_IDLE_ANGLE);

  systemBusy = false;

  Serial.println("PUSH SELESAI, servo kembali idle.");
}


// ============================================================
// SERVO SORTIR: arahkan ke grade A/B/C
// ============================================================

void sortFruit(char grade)
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("SORTIR BUAH");
  Serial.println("========================================");
  Serial.print("GRADE: ");
  Serial.println(grade);

  systemBusy = true;

  int angle = SORT_IDLE_ANGLE;

  if (grade == 'A') angle = SORT_A_ANGLE;
  else if (grade == 'B') angle = SORT_B_ANGLE;
  else if (grade == 'C') angle = SORT_C_ANGLE;
  else
  {
    Serial.println("GRADE TIDAK DIKENALI, servo tetap netral.");
    systemBusy = false;
    return;
  }

  servoSort.write(angle);
  delay(SORT_HOLD_MS);
  servoSort.write(SORT_IDLE_ANGLE);

  systemBusy = false;

  Serial.println("SORTIR SELESAI, servo kembali netral.");
}


// ============================================================
// BUZZER
// ============================================================

void buzzerBeep(int times, int durationMs)
{
  for (int i = 0; i < times; i++)
  {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);

    if (i < times - 1)
    {
      delay(durationMs);
    }
  }
}


// ============================================================
// CEK ESP32-CAM (generik untuk ATAS & SAMPING)
// ============================================================

bool checkCameraAt(const char* host, const char* label)
{
  HTTPClient http;

  String url = "http://" + String(host) + "/status";

  Serial.println();
  Serial.print("CHECK ESP32-CAM ");
  Serial.println(label);
  Serial.print("URL: ");
  Serial.println(url);

  http.setConnectTimeout(3000);
  http.setTimeout(5000);

  if (!http.begin(url))
  {
    Serial.println("HTTP BEGIN FAILED");
    return false;
  }

  int code = http.GET();
  String response = http.getString();
  http.end();

  Serial.print("HTTP CODE: "); Serial.println(code);
  Serial.print("RESPONSE : "); Serial.println(response);

  return (code == 200);
}

bool captureCameraAt(const char* host, const char* label)
{
  HTTPClient http;

  String url = "http://" + String(host) + "/capture";

  Serial.println();
  Serial.print("CAPTURE ESP32-CAM ");
  Serial.println(label);
  Serial.print("URL: ");
  Serial.println(url);

  http.setConnectTimeout(3000);
  http.setTimeout(15000);

  if (!http.begin(url))
  {
    Serial.println("HTTP BEGIN FAILED");
    return false;
  }

  int code = http.GET();
  String response = http.getString();
  http.end();

  Serial.print("HTTP CODE: "); Serial.println(code);
  Serial.print("RESPONSE : "); Serial.println(response);

  return (code == 200);
}

void triggerBothCameras()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("TRIGGER CAPTURE 2 KAMERA");
  Serial.println("========================================");

  bool okTop = captureCameraAt(CAM_TOP_HOST, "ATAS");
  if (!okTop)
  {
    Serial.println("CAPTURE ATAS gagal, coba ulang 1x...");
    delay(500);
    okTop = captureCameraAt(CAM_TOP_HOST, "ATAS");
  }

  bool okSide = captureCameraAt(CAM_SIDE_HOST, "SAMPING");
  if (!okSide)
  {
    Serial.println("CAPTURE SAMPING gagal, coba ulang 1x...");
    delay(500);
    okSide = captureCameraAt(CAM_SIDE_HOST, "SAMPING");
  }

  Serial.print("CAM ATAS   : "); Serial.println(okTop ? "OK" : "GAGAL");
  Serial.print("CAM SAMPING: "); Serial.println(okSide ? "OK" : "GAGAL");
}


// ============================================================
// AMBIL FOTO TERAKHIR DARI ESP32-CAM (GET /jpg -> data JPEG mentah)
// ============================================================
// Ini yang tadinya HILANG: setelah kamera capture foto (disimpan
// di memori kamera itu sendiri), ESP32 MAIN perlu MENGAMBIL file
// JPEG-nya lewat endpoint /jpg supaya bisa dikirim ke cloud API.

uint8_t* fetchImageFromCamOnce(const char* host, size_t &outLen)
{
  HTTPClient http;
  String url = "http://" + String(host) + "/jpg";

  Serial.print("Mengambil foto dari: "); Serial.println(url);

  http.setConnectTimeout(3000);
  http.setTimeout(10000);

  if (!http.begin(url))
  {
    Serial.println("HTTP BEGIN GAGAL (ambil foto kamera)");
    outLen = 0;
    return nullptr;
  }

  int code = http.GET();

  if (code != 200)
  {
    Serial.print("GAGAL AMBIL FOTO, HTTP CODE: ");
    Serial.println(code);
    http.end();
    outLen = 0;
    return nullptr;
  }

  int len = http.getSize();

  if (len <= 0)
  {
    Serial.println("UKURAN FOTO TIDAK VALID");
    http.end();
    outLen = 0;
    return nullptr;
  }

  uint8_t* buf = (uint8_t*) malloc(len);

  if (buf == nullptr)
  {
    Serial.println("GAGAL ALOKASI MEMORI UNTUK FOTO");
    http.end();
    outLen = 0;
    return nullptr;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t readTotal = 0;
  unsigned long startTime = millis();

  while (readTotal < (size_t)len && (millis() - startTime) < 10000)
  {
    if (stream->available())
    {
      int r = stream->read(buf + readTotal, len - readTotal);
      if (r > 0) readTotal += r;
    }
    else
    {
      delay(2);
    }
  }

  http.end();

  if (readTotal != (size_t)len)
  {
    Serial.println("PERINGATAN: foto mungkin terpotong (koneksi lambat/putus)");
  }

  Serial.print("Foto diterima: "); Serial.print(readTotal); Serial.println(" bytes");

  outLen = readTotal;
  return buf;
}

// Wrapper dengan retry 1x: kalau percobaan pertama gagal (misal WiFi
// ESP32-CAM sempat putus sesaat), coba lagi sekali sebelum benar-benar
// dianggap gagal. Ini mengatasi kasus kamera baru saja "ONLINE" tapi
// tiba-tiba HTTP CODE -1 pas diminta fotonya.
uint8_t* fetchImageFromCam(const char* host, size_t &outLen)
{
  uint8_t* buf = fetchImageFromCamOnce(host, outLen);

  if (buf == nullptr)
  {
    Serial.println("AMBIL FOTO gagal, coba ulang 1x...");
    delay(500);
    buf = fetchImageFromCamOnce(host, outLen);
  }

  return buf;
}


// ============================================================
// KIRIM FOTO KE CLOUD API ML -> TERIMA GRADE (A/B/C)
// ============================================================
// Ini bagian yang tadinya HILANG: mengirim foto ke API cloud
// (FastAPI di Render) lewat multipart/form-data, lalu membaca
// hasil "grade" dari respons JSON-nya.

// ============================================================
// JAGA CLOUD API TETAP "BANGUN" (ANTI COLD START)
// ============================================================
// Render free tier tidur kalau nggak ada trafik ~15 menit. Daripada
// nunggu buah lewat baru "membangunkan" (bikin buah pertama nunggu
// lama), kita ping server secara berkala di background supaya dia
// nggak sempat tidur selama sistem sedang dipakai.

unsigned long lastKeepAlive = 0;
const unsigned long KEEP_ALIVE_INTERVAL_MS = 8UL * 60UL * 1000UL; // tiap 8 menit

// ============================================================
// REVISI: CEK WIFI & STATUS KAMERA SECARA PERIODIK
// ============================================================
// Sebelumnya WiFi cuma dicek sekali di setup() -- kalau putus di
// tengah operasi, internetReady tetap TRUE selamanya dan sistem gagal
// diam-diam. Sekarang dicek ulang tiap WIFI_CHECK_INTERVAL_MS, dan
// auto-reconnect kalau ternyata putus. Status kamera juga sekarang
// di-cek ulang otomatis (sebelumnya cuma lewat menu serial manual),
// supaya /status dan halaman utama selalu menampilkan info terkini.
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 15UL * 1000UL; // tiap 15 detik

unsigned long lastCamCheck = 0;
const unsigned long CAM_CHECK_INTERVAL_MS = 30UL * 1000UL; // tiap 30 detik
bool cloudAPIWarm = false; // status buat ditampilkan ke OLED/serial

String getCloudAPIRootURL()
{
  // CLOUD_API_URL berakhiran "/predict" -> ambil bagian sebelum itu saja,
  // supaya ping keep-alive nggak perlu kirim gambar (cukup nyentuh server-nya).
  String suffix = "/predict";
  if (CLOUD_API_URL.endsWith(suffix))
  {
    return CLOUD_API_URL.substring(0, CLOUD_API_URL.length() - suffix.length());
  }
  return CLOUD_API_URL;
}

void keepCloudAPIWarm()
{
  if (!internetReady) return;

  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // skip validasi sertifikat (cukup buat cloud API pribadi)
  String rootUrl = getCloudAPIRootURL();

  Serial.println();
  Serial.println("KEEP-ALIVE: ngecek/membangunkan cloud API...");

  http.setConnectTimeout(60000); // dinaikkan: pas cold start, proxy Render bisa
                                  // lambat nerima koneksi TCP/TLS baru, bukan cuma
                                  // lambat ngasih respons - jangan nyerah kecepetan
  http.setTimeout(70000); // sama seperti request asli, jaga-jaga kalau lagi cold start

  if (!http.begin(secureClient, rootUrl))
  {
    Serial.println("KEEP-ALIVE: HTTP BEGIN GAGAL");
    return;
  }

  unsigned long startMs = millis();
  int code = http.GET();
  unsigned long elapsedMs = millis() - startMs;
  http.end();

  Serial.print("KEEP-ALIVE: HTTP CODE "); Serial.print(code);
  Serial.print(", butuh "); Serial.print(elapsedMs / 1000.0, 1); Serial.println(" detik");

  // elapsed lama (>10 detik) = kemungkinan besar tadi baru bangun dari sleep.
  // Kode selain 200 masih dianggap "server merespons" (misal 404 kalau
  // root URL tidak punya handler) - yang penting bukan -1/-11 (gagal konek).
  cloudAPIWarm = (code > 0);

  if (cloudAPIWarm)
  {
    Serial.println("KEEP-ALIVE: server merespons, cloud API siap dipakai.");
  }
  else
  {
    Serial.println("KEEP-ALIVE: server belum merespons / masih proses bangun.");
  }
}


bool classifyWithCloudAPI(uint8_t* imageData, size_t imageLen, char &outGrade, String &outBuah)
{
  if (!internetReady)
  {
    Serial.println("TIDAK ADA INTERNET, tidak bisa hubungi cloud API.");
    return false;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // skip validasi sertifikat (cukup buat cloud API pribadi)

  Serial.print("Mengirim foto ke cloud API: "); Serial.println(CLOUD_API_URL);
  Serial.print("Sisa RAM sebelum request: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");

  http.setConnectTimeout(60000); // dinaikkan: proxy Render bisa lambat nerima
                                  // koneksi TCP/TLS baru pas cold start
  http.setTimeout(70000); // Render free tier: cold start bisa 40-50 detik+,
                           // 70 detik dikasih margin aman di atas itu.
                           // Ini HANYA kepake kalau server-nya lagi "tidur";
                           // kalau sudah "bangun" (lihat keepCloudAPIWarm di
                           // bawah), respons biasanya balik dalam 1-3 detik.

  if (!http.begin(secureClient, CLOUD_API_URL))
  {
    Serial.println("HTTP BEGIN GAGAL (cloud API)");
    return false;
  }

  String boundary = "SortirApelBoundary123456";

  String head = "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"fruit.jpg\"\r\n";
  head += "Content-Type: image/jpeg\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = head.length() + imageLen + tail.length();

  uint8_t* body = (uint8_t*) malloc(totalLen);

  if (body == nullptr)
  {
    Serial.println("GAGAL ALOKASI MEMORI UNTUK REQUEST BODY");
    http.end();
    return false;
  }

  size_t idx = 0;
  memcpy(body + idx, head.c_str(), head.length()); idx += head.length();
  memcpy(body + idx, imageData, imageLen);         idx += imageLen;
  memcpy(body + idx, tail.c_str(), tail.length()); idx += tail.length();

  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  int code = http.POST(body, totalLen);
  free(body);

  if (code != 200)
  {
    Serial.print("CLOUD API ERROR, HTTP CODE: ");
    Serial.println(code);
    Serial.println(http.getString());
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  Serial.println("RESPON CLOUD API:");
  Serial.println(response);

  // ============================================================
  // REVISI: parsing JSON proper pakai ArduinoJson, ganti cara lama
  // (cari substring manual pakai indexOf) yang gampang salah kalau
  // format JSON dari server berubah dikit (misal ada spasi setelah
  // tanda titik dua).
  // ============================================================
  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, response);

  if (jsonErr)
  {
    Serial.print("GAGAL PARSE JSON DARI CLOUD API: ");
    Serial.println(jsonErr.c_str());
    return false;
  }

  String status = doc["status"] | "";      // "ok" / "unsure" / "error"
  String grade  = doc["grade"]  | "";       // "A" / "B" / kosong kalau null
  String buah   = doc["buah"]   | "";       // REVISI: cloud API sekarang multi-buah,
                                             // "apel" / "jeruk" / dst, kosong kalau null
  float confidence = doc["confidence"] | 0.0;

  Serial.print("STATUS: "); Serial.print(status);
  Serial.print(" | BUAH: "); Serial.print(buah.length() ? buah : "-");
  Serial.print(" | GRADE: "); Serial.print(grade.length() ? grade : "-");
  Serial.print(" | CONFIDENCE: "); Serial.println(confidence, 4);

  // PENTING: sistem ini hanya mengenal 2 grade ASLI dari ML: A dan B,
  // DAN cuma dianggap sah kalau status dari API persis "ok".
  // REVISI: app.py v3 sudah MENGHAPUS TOTAL kelas "BUKAN_BUAH" (model
  // sekarang cuma dilatih dengan kelas berpola "{buah}_{GRADE}", tidak
  // ada lagi kelas pemaksa ketiga). Jadi status "unsure" SEKARANG HANYA
  // bisa terjadi karena confidence di bawah CONFIDENCE_THRESHOLD milik
  // app.py -- bukan lagi karena BUKAN_BUAH. Penanganannya di ESP32 tetap
  // SAMA: status "unsure" (dan grade yang bukan A/B) TIDAK dianggap
  // grade sah -- diperlakukan sama seperti kegagalan klasifikasi (return
  // false), supaya buah/objek yang tidak jelas masuk ke jalur "gagal
  // deteksi" di belakang, BUKAN ikut disebut/dicatat sebagai grade A/B.
  if (status != "ok" || (grade != "A" && grade != "B"))
  {
    Serial.println("HASIL TIDAK VALID (status bukan 'ok', atau grade bukan A/B) -> dianggap gagal deteksi.");
    return false;
  }

  outGrade = grade.charAt(0);
  outBuah  = buah;
  return true;
}


// ============================================================
// BANDINGKAN 2 GRADE, AMBIL YANG LEBIH JELEK (LEBIH AMAN)
// ============================================================
// Sistem ini cuma punya 2 grade ASLI: A (bagus) dan B (kurang bagus).
// Kalau cacat cuma kelihatan dari salah satu sisi (misal busuk di
// bagian yang cuma keliatan dari samping, tidak dari atas), grade
// akhir tetap ikut yang lebih jelek (B) -- supaya sistem tidak salah
// meloloskan buah jelek ke grade bagus (A). Kalau salah satu sisi
// gagal diklasifikasi (grade = 0 / tidak valid), otomatis pakai hasil
// sisi yang berhasil saja. Kalau KEDUA sisi gagal, itu ditangani di
// luar fungsi ini (handleFruitArrival) sebagai "gagal deteksi total"
// -> buah masuk jalur belakang, BUKAN grade C.

char worseGrade(char a, char b)
{
  auto rank = [](char g) -> int
  {
    if (g == 'A') return 0;
    if (g == 'B') return 1;
    return -1; // tidak valid / gagal diklasifikasi
  };

  int ra = rank(a);
  int rb = rank(b);

  if (ra < 0) return b; // sisi A tidak valid -> pakai b
  if (rb < 0) return a; // sisi B tidak valid -> pakai a

  return (ra > rb) ? a : b; // rank lebih besar = lebih jelek
}


// ============================================================
// LOG HASIL GRADE KE GOOGLE SHEETS (VIA APPS SCRIPT WEB APP)
// ============================================================
// REVISI v3 - SELARAS DENGAN apps_script_log.gs v3:
// - Skema lama (gradeTop/gradeSide/gradeFinal, 3 kolom grade terpisah)
//   SUDAH DIHAPUS di apps_script_log.gs v3. Apps Script sekarang cuma
//   menerima SATU parameter "grade" (harus persis "A" atau "B"), lalu
//   MENOLAK request dengan {success:false, error:"..."} kalau grade-nya
//   bukan "A"/"B" -- termasuk string "GAGAL" yang dulu dikirim ke sini.
//   Jadi fungsi ini SEKARANG HANYA BOLEH DIPANGGIL untuk buah yang
//   BERHASIL diklasifikasi (grade valid A/B). Kasus gagal deteksi TIDAK
//   dicatat ke Sheet sama sekali (lihat handleFruitArrival) -- sesuai
//   catatan di kepala apps_script_log.gs.
// - Parameter gradeTopVal/gradeSideVal juga sudah tidak ada -- kolom
//   "Grade Atas"/"Grade Samping" sudah dihapus dari sheet, cukup 1
//   kolom "Grade" (grade akhir/terjelek dari 2 sisi, hasil worseGrade()).
//
// Dipanggil sekali tiap buah BERHASIL diklasifikasi, supaya admin bisa
// pantau jumlah per grade dan riwayatnya langsung dari Google Sheets.
// Kalau gagal kirim log (misal internet lagi bermasalah), proses sortir
// buah TETAP LANJUT -- logging tidak boleh menghambat alur utama.

void logToGoogleSheet(unsigned long fruitIdVal, char gradeVal, float weightVal,
                       String buahVal)
{
  if (!internetReady)
  {
    Serial.println("LOG SHEET: dilewati, tidak ada internet.");
    return;
  }

  if (SHEET_WEBAPP_URL.indexOf("GANTI_DENGAN_URL") >= 0)
  {
    Serial.println("LOG SHEET: dilewati, SHEET_WEBAPP_URL belum diisi.");
    return;
  }

  // Jaga-jaga di sisi ESP32 juga (selain validasi di Apps Script):
  // grade WAJIB 'A' atau 'B'. Fungsi ini tidak boleh dipanggil dengan
  // grade lain (0 / gagal deteksi) -- Apps Script pasti akan menolaknya.
  if (gradeVal != 'A' && gradeVal != 'B')
  {
    Serial.print("LOG SHEET: dilewati, grade tidak valid untuk dicatat ('");
    Serial.print(gradeVal);
    Serial.println("').");
    return;
  }

  // buah wajib diisi (Apps Script menolak kalau kosong).
  if (buahVal.length() == 0)
  {
    Serial.println("LOG SHEET: dilewati, nama buah kosong.");
    return;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // skip validasi sertifikat

  http.setConnectTimeout(5000);
  http.setTimeout(10000);

  // PENTING: URL Apps Script (.../exec) selalu balikin HTTP 302 dulu
  // (redirect ke script.googleusercontent.com) sebelum benar-benar jalan.
  // Ini paksa HTTPClient tetap ikutin redirect itu.
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  // CATATAN: dulu ini pakai POST dengan body JSON, tapi HTTPClient ESP32
  // punya bug lama: saat POST dialihkan (redirect) dari script.google.com
  // ke script.googleusercontent.com, isi body JSON-nya suka hilang di
  // tengah jalan -> Google nerima body kosong -> nolak dengan HTTP 400.
  // Solusinya: ganti ke GET, datanya dikirim lewat parameter URL (bukan
  // body), jadi tidak ada apa-apa yang bisa "hilang" saat redirect.
  // Sisi Apps Script juga WAJIB pakai doGet() -- lihat apps_script_log.gs.

  // REVISI: parameter URL sekarang PERSIS mengikuti apps_script_log.gs v3:
  // key, fruitId, buah, grade ("A"/"B"), weight. Tidak ada lagi gradeTop/
  // gradeSide/gradeFinal.
  String url = SHEET_WEBAPP_URL;
  url += "?key=" + String(SHEET_SECRET_KEY);
  url += "&fruitId=" + String(fruitIdVal);
  url += "&buah=" + buahVal;
  url += "&grade=" + String(gradeVal);
  url += "&weight=" + String(weightVal, 1);

  if (!http.begin(secureClient, url))
  {
    Serial.println("LOG SHEET: HTTP BEGIN GAGAL");
    return;
  }

  int code = http.GET();

  if (code == 200)
  {
    // CATATAN: Apps Script SELALU balas HTTP 200 walau gagal secara
    // logis (quirk Apps Script, lihat catatan di apps_script_log.gs).
    // Jadi HTTP 200 di sini BUKAN jaminan baris berhasil masuk ke
    // Sheet -- baca juga body-nya untuk tahu success:true/false yang
    // sebenarnya.
    String resp = http.getString();
    Serial.print("LOG SHEET: response HTTP 200, body: ");
    Serial.println(resp);

    if (resp.indexOf("\"success\":true") >= 0)
    {
      Serial.println("LOG SHEET: berhasil dicatat.");
    }
    else
    {
      Serial.println("LOG SHEET: DITOLAK Apps Script (success:false) -- lihat body di atas.");
    }
  }
  else
  {
    Serial.print("LOG SHEET: GAGAL, HTTP CODE ");
    Serial.println(code);
  }

  http.end();
}


// ============================================================
// ALUR PROSES SAAT BUAH DIPICU (manual serial 'f' / HTTP /trigger)
// ============================================================

void handleFruitArrival()
{
  fruitID++;

  Serial.println();
  Serial.println("########################################");
  Serial.print("BUAH TERDETEKSI - ID: ");
  Serial.println(fruitID);
  Serial.println("########################################");

  buzzerBeep(1, 150);

  lcdShow("Buah #" + String(fruitID), "Menimbang...");

  float weight = readWeight();
  Serial.print("Berat: ");
  Serial.print(weight, 1);
  Serial.println(" gram");

  lcdShow("Berat: " + String(weight, 1) + "g", "Memotret...");

  triggerBothCameras();

  lcdShow("Buah #" + String(fruitID), "Push ke conveyor");

  pushToConveyor();

  Serial.println("Buah sudah di conveyor. Mengirim foto ke cloud AI...");

  if (cloudAPIWarm)
  {
    // Server kemungkinan sudah "bangun" dari keep-alive terakhir -> cepat
    lcdShow("Buah #" + String(fruitID), "Cek Cloud AI...");
  }
  else
  {
    // Belum ada konfirmasi server bangun -> kasih tau operator supaya
    // nggak panik kalau OLED diem agak lama (bisa sampai ~70 detik)
    lcdShow("Memanaskan server", "Mohon tunggu (<=70dtk)");
  }

  // ---- Ambil & klasifikasi foto dari KEDUA kamera (ATAS & SAMPING) ----
  // Grade akhir = yang LEBIH JELEK dari keduanya (lihat worseGrade()),
  // supaya cacat yang cuma kelihatan dari satu sudut tetap membuat buah
  // turun grade, bukan lolos ke grade bagus. Catatan: ini berarti 2x
  // request ke cloud API per buah -> proses jadi lebih lama dari
  // sebelumnya (terutama kalau server belum "hangat").

  char gradeTop = 0;
  char gradeSide = 0;
  bool successTop = false;
  bool successSide = false;

  // REVISI: cloud API sekarang multi-buah, jadi tiap sisi juga balikin
  // nama buah ("apel", "jeruk", dst), bukan cuma grade A/B saja.
  String buahTop = "";
  String buahSide = "";

  size_t imgLenTop = 0;
  uint8_t* imgDataTop = fetchImageFromCam(CAM_TOP_HOST, imgLenTop);

  if (imgDataTop != nullptr && imgLenTop > 0)
  {
    successTop = classifyWithCloudAPI(imgDataTop, imgLenTop, gradeTop, buahTop);
    free(imgDataTop);
  }
  else
  {
    Serial.println("GAGAL AMBIL FOTO DARI KAMERA ATAS.");
  }

  Serial.print("GRADE ATAS   : ");
  Serial.println(successTop ? String(gradeTop) : "GAGAL");

  size_t imgLenSide = 0;
  uint8_t* imgDataSide = fetchImageFromCam(CAM_SIDE_HOST, imgLenSide);

  if (imgDataSide != nullptr && imgLenSide > 0)
  {
    successSide = classifyWithCloudAPI(imgDataSide, imgLenSide, gradeSide, buahSide);
    free(imgDataSide);
  }
  else
  {
    Serial.println("GAGAL AMBIL FOTO DARI KAMERA SAMPING.");
  }

  Serial.print("GRADE SAMPING: ");
  Serial.println(successSide ? String(gradeSide) : "GAGAL");

  bool success = successTop || successSide;
  char grade = 0;

  // Nama buah dipakai buat log/LCD saja (bukan buat logika sortir, sortir
  // tetap murni berdasarkan grade). Diambil dari sisi ATAS dulu (lebih
  // stabil framing-nya), fallback ke SAMPING kalau sisi atas gagal.
  // Kalau kedua sisi berhasil tapi beda nama buah (jarang terjadi, tapi
  // bisa kalau ada 2 buah beririsan/foto ambigu), dicatat sebagai
  // peringatan tapi tetap pakai nama dari sisi ATAS supaya proses lanjut.
  String buahFinal = successTop ? buahTop : (successSide ? buahSide : "");

  if (successTop && successSide && buahTop != buahSide)
  {
    Serial.println("PERINGATAN: kamera ATAS & SAMPING mendeteksi jenis buah BERBEDA ("
                    + buahTop + " vs " + buahSide + "). Memakai hasil ATAS untuk log.");
  }

  if (success)
  {
    grade = worseGrade(successTop ? gradeTop : 0, successSide ? gradeSide : 0);
  }

  // ---- Catat ke Google Sheets (HANYA kalau berhasil diklasifikasi) ----
  // REVISI: apps_script_log.gs v3 menolak (success:false) request dengan
  // grade selain "A"/"B", dan dokumentasinya eksplisit bilang ESP32
  // SEHARUSNYA TIDAK memanggil endpoint ini sama sekali untuk hasil yang
  // tidak valid/gagal. Jadi logging sekarang di dalam blok `if (success)`
  // di bawah, BUKAN dipanggil selalu seperti revisi sebelumnya.

  if (success)
  {
    logToGoogleSheet(fruitID, grade, weight, buahFinal);

    Serial.print("BUAH TERDETEKSI: "); Serial.println(buahFinal.length() ? buahFinal : "-");
    Serial.print("GRADE FINAL (terjelek dari 2 sisi): ");
    Serial.println(grade);

    lcdShow((buahFinal.length() ? buahFinal : "Buah") + " A:" + (successTop ? String(gradeTop) : String("-")) +
             " S:" + (successSide ? String(gradeSide) : String("-")),
            "Final Grade: " + String(grade));

    sortFruit(grade);
  }
  else
  {
    Serial.println("KLASIFIKASI GAGAL. Buah akan lewat ke jalur belakang (gagal deteksi, BUKAN grade C).");
    buzzerBeep(3, 150); // 3x bunyi pendek = tanda error

    lcdShow("Buah #" + String(fruitID), "GAGAL! -> Jalur Belakang");

    sortFruit('C'); // 'C' di sini cuma nama posisi servo (buka penuh /
                     // jalur belakang), BUKAN grade ML. Fisiknya sama
                     // dengan servo yang tadinya diberi nama SORT_C_ANGLE.
  }

  lcdShow("SORTIR APEL READY", "Menunggu buah..");
}


// ============================================================
// WEB: ROOT
// ============================================================

void handleRoot()
{
  String html;
  html += "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width'>";
  html += "<title>Sortir Apel</title></head><body>";
  html += "<h1>SORTIR APEL ESP32 MAIN</h1>";
  html += "<h2>System Online</h2>";
  html += "<p>MAIN IP: " + WiFi.localIP().toString() + "</p>";
  html += "<p>CAM TOP: " + String(CAM_TOP_HOST) + "</p>";
  html += "<p>CAM SIDE: " + String(CAM_SIDE_HOST) + "</p>";
  html += "<p>HX711: "; html += hx711Ready ? "READY" : "OFFLINE"; html += "</p>";
  html += "<p>CAM ATAS: "; html += camTopOnline ? "ONLINE" : "OFFLINE"; html += "</p>";
  html += "<p>CAM SAMPING: "; html += camSideOnline ? "ONLINE" : "OFFLINE"; html += "</p>";
  html += "<p><a href='/status'>STATUS (JSON)</a></p>";
  html += "<p><a href='/capture'>TRIGGER CAPTURE</a></p>";
  html += "<p><a href='/trigger'>TRIGGER ALUR BUAH (push+capture+weigh)</a></p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}


// ============================================================
// WEB: STATUS
// ============================================================

void handleStatus()
{
  String json;
  json += "{";
  json += "\"system\":\"Sortir Apel ESP32 MAIN\",";
  json += "\"status\":\"online\",";
  json += "\"main_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"cam_top_host\":\"" + String(CAM_TOP_HOST) + "\",";
  json += "\"cam_side_host\":\"" + String(CAM_SIDE_HOST) + "\",";
  json += "\"cam_top\":"; json += camTopOnline ? "true" : "false"; json += ",";
  json += "\"cam_side\":"; json += camSideOnline ? "true" : "false"; json += ",";
  json += "\"hx711\":"; json += hx711Ready ? "true" : "false"; json += ",";
  json += "\"internet\":"; json += internetReady ? "true" : "false"; json += ",";
  json += "\"weight\":"; json += String(readWeight(), 1); json += ",";
  json += "\"auto_detect\":"; json += AUTO_DETECT_ENABLED ? "true" : "false"; json += ",";
  json += "\"weight_threshold\":"; json += String(WEIGHT_TRIGGER_THRESHOLD, 1);
  json += "}";

  server.send(200, "application/json", json);
}


// ============================================================
// WEB: CAPTURE (trigger manual kedua kamera)
// ============================================================

void handleCapture()
{
  triggerBothCameras();

  server.send(200, "application/json",
    "{\"success\":true,\"message\":\"capture_requested_both_cams\"}");
}


// ============================================================
// WEB: TRIGGER MANUAL ALUR BUAH (pengganti sensor IR)
// ============================================================

void handleTrigger()
{
  if (systemBusy)
  {
    server.send(409, "application/json",
      "{\"success\":false,\"error\":\"system_busy\"}");
    return;
  }

  handleFruitArrival();
  readyForNextTrigger = false; // cegah auto-detect langsung nyamber lagi kalau beban masih ada

  server.send(200, "application/json",
    "{\"success\":true,\"message\":\"fruit_flow_triggered\",\"fruit_id\":" + String(fruitID) + "}");
}


// ============================================================
// WEB: TERIMA HASIL KLASIFIKASI -> GERAKKAN SERVO SORTIR
// ============================================================

void handleClassify()
{
  if (!server.hasArg("plain"))
  {
    server.send(400, "application/json",
      "{\"success\":false,\"error\":\"body_missing\"}");
    return;
  }

  String body = server.arg("plain");

  Serial.println();
  Serial.println("========================================");
  Serial.println("CLASSIFICATION RECEIVED");
  Serial.println("========================================");
  Serial.println(body);

  char grade = 0;

  if (body.indexOf("\"grade\":\"A\"") >= 0 || body.indexOf("\"A\"") >= 0) grade = 'A';
  else if (body.indexOf("\"grade\":\"B\"") >= 0 || body.indexOf("\"B\"") >= 0) grade = 'B';
  else if (body.indexOf("\"grade\":\"C\"") >= 0 || body.indexOf("\"C\"") >= 0) grade = 'C';

  if (grade == 0)
  {
    server.send(400, "application/json",
      "{\"success\":false,\"error\":\"invalid_grade\"}");
    return;
  }

  Serial.print("Grade diterima untuk fruit ID ");
  Serial.print(fruitID);
  Serial.print(": ");
  Serial.println(grade);

  lcdShow("Buah #" + String(fruitID), "Grade: " + String(grade) + " -> sortir");

  sortFruit(grade);

  lcdShow("SORTIR APEL READY", "Menunggu buah..");

  server.send(200, "application/json", "{\"success\":true}");
}


// ============================================================
// SERIAL MENU
// ============================================================

// ============================================================
// TES DIAGNOSA HTTPS (isolasi: masalah di Render, atau di ESP32/HTTPS
// secara umum?) - nembak ke example.com (situs ringan, selalu online,
// TIDAK ada cold start, sertifikatnya sederhana)
// ============================================================

void testHTTPSDiagnostic()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("TES DIAGNOSA HTTPS");
  Serial.println("========================================");
  Serial.print("Sisa RAM SEBELUM request: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");

  if (!internetReady)
  {
    Serial.println("TIDAK ADA INTERNET, tes dibatalkan.");
    return;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  http.setConnectTimeout(15000);
  http.setTimeout(15000);

  String testUrl = "https://example.com/";
  Serial.print("Nembak ke: "); Serial.println(testUrl);

  if (!http.begin(secureClient, testUrl))
  {
    Serial.println("HTTP BEGIN GAGAL");
    return;
  }

  unsigned long startMs = millis();
  int code = http.GET();
  unsigned long elapsedMs = millis() - startMs;
  http.end();

  Serial.print("HASIL: HTTP CODE "); Serial.print(code);
  Serial.print(", butuh "); Serial.print(elapsedMs / 1000.0, 1); Serial.println(" detik");
  Serial.print("Sisa RAM SETELAH request: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");

  if (code == 200)
  {
    Serial.println(">>> HTTPS UMUM: BERHASIL. Berarti ESP32 & jaringan OK,");
    Serial.println(">>> masalahnya spesifik ke server Render (bukan ke kita).");
  }
  else
  {
    Serial.println(">>> HTTPS UMUM: GAGAL JUGA. Berarti bukan Render yang");
    Serial.println(">>> bermasalah, tapi ada masalah HTTPS/RAM di ESP32 ini.");
  }
  Serial.println("========================================");
}

void printMenu()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("COMMAND MENU (ketik lalu ENTER)");
  Serial.println("========================================");
  Serial.println("c = cek kedua ESP32-CAM online/tidak");
  Serial.println("p = trigger capture kedua kamera");
  Serial.println();
  Serial.println("w = baca berat sekarang");
  Serial.println("t = tare load cell (kosongkan dulu)");
  Serial.println("k = mulai KALIBRASI load cell (wizard)");
  Serial.println();
  Serial.println("u = gerak servo PUSH (test dorong ke conveyor)");
  Serial.println("1 = test sortir ke grade A");
  Serial.println("2 = test sortir ke grade B");
  Serial.println("3 = test sortir ke grade C");
  Serial.println("r = servo kembali ke posisi idle/netral");
  Serial.println();
  Serial.println("z = test buzzer (bunyi 2x)");
  Serial.println();
  Serial.println("d = cek status deteksi otomatis (load cell)");
  Serial.println("a = atur weight trigger threshold (gram)");
  Serial.println("e = toggle auto-detect ON/OFF");
  Serial.println();
  Serial.println("f = trigger PENUH alur buah datang (push+capture+weigh)");
  Serial.println("    (juga bisa dipicu lewat HTTP GET /trigger)");
  Serial.println("s = tampilkan status sistem");
  Serial.println("y = TES DIAGNOSA: cek HTTPS ke situs lain (bukan Render) + sisa RAM");
  Serial.println("x = HAPUS WiFi tersimpan + restart (buka hotspot setup lagi)");
  Serial.println("========================================");
}

void handleSerial()
{
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd.length() == 0) return;

  char c = cmd.charAt(0);

  switch (c)
  {
    case 'c': case 'C':
      camTopOnline = checkCameraAt(CAM_TOP_HOST, "ATAS");
      camSideOnline = checkCameraAt(CAM_SIDE_HOST, "SAMPING");
      Serial.print("CAM ATAS   : "); Serial.println(camTopOnline ? "ONLINE" : "OFFLINE");
      Serial.print("CAM SAMPING: "); Serial.println(camSideOnline ? "ONLINE" : "OFFLINE");
      break;

    case 'p': case 'P':
      triggerBothCameras();
      break;

    case 'w': case 'W':
      Serial.print("BERAT: ");
      Serial.print(readWeight(), 1);
      Serial.println(" gram");
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

    case '1':
      sortFruit('A');
      break;

    case '2':
      sortFruit('B');
      break;

    case '3':
      sortFruit('C');
      break;

    case 'r': case 'R':
      servoPush.write(PUSH_IDLE_ANGLE);
      servoSort.write(SORT_IDLE_ANGLE);
      Serial.println("SERVO PUSH & SORTIR: KEMBALI KE IDLE/NETRAL");
      break;

    case 'z': case 'Z':
      Serial.println("TEST BUZZER...");
      buzzerBeep(2, 200);
      Serial.println("SELESAI");
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
      Serial.print("AUTO_DETECT_ENABLED sekarang: ");
      Serial.println(AUTO_DETECT_ENABLED ? "AKTIF" : "NONAKTIF");
      break;

    case 'f': case 'F':
      Serial.println("SIMULASI ALUR PENUH (manual trigger)...");
      handleFruitArrival();
      readyForNextTrigger = false; // cegah auto-detect langsung nyamber lagi kalau beban masih ada
      break;

    case 's': case 'S':
      Serial.println();
      Serial.println("SYSTEM STATUS");
      Serial.print("MAIN IP     : "); Serial.println(WiFi.localIP());
      Serial.print("CAM TOP  : "); Serial.println(CAM_TOP_HOST);
      Serial.print("CAM SIDE : "); Serial.println(CAM_SIDE_HOST);
      Serial.print("CAM ATAS    : "); Serial.println(camTopOnline ? "ONLINE" : "OFFLINE");
      Serial.print("CAM SAMPING : "); Serial.println(camSideOnline ? "ONLINE" : "OFFLINE");
      Serial.print("HX711       : "); Serial.println(hx711Ready ? "READY" : "OFFLINE");
      Serial.print("WEIGHT      : "); Serial.print(readWeight(), 1); Serial.println(" gram");
      Serial.print("AUTO-DETECT : "); Serial.print(AUTO_DETECT_ENABLED ? "AKTIF" : "NONAKTIF");
      Serial.print(" (threshold "); Serial.print(WEIGHT_TRIGGER_THRESHOLD, 1); Serial.println(" gram)");
      Serial.print("FRUIT ID    : "); Serial.println(fruitID);
      break;

    case 'y': case 'Y':
      testHTTPSDiagnostic();
      break;

    case 'x': case 'X':
      // Reset WiFi tersimpan lewat Serial (alternatif tombol BOOT yang
      // suka meleset timing-nya). Setelah ini board RESTART otomatis dan
      // langsung masuk mode setup -> hotspot SORTIR-APEL-MAIN-SETUP muncul.
      Serial.println("Menghapus WiFi tersimpan lewat perintah Serial...");
      wm.resetSettings();
      Serial.println("Selesai. Restart board dalam 2 detik...");
      delay(2000);
      ESP.restart();
      break;

    default:
      printMenu();
      break;
  }
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("########################################");
  Serial.println("       SORTIR APEL ESP32 MAIN - REVISI");
  Serial.println("########################################");

  // ---- BUZZER ----
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // ---- LCD ----
  setupLCD();

  // ---- HX711 ----
  // SENGAJA dicek SEBELUM WiFi: proses konek WiFi (AP+STA) menarik arus
  // besar & bikin tegangan sesaat "goyang". Kalau HX711 dicek pas kondisi
  // itu, is_ready() bisa gagal padahal wiring-nya sebenarnya benar.
  // Dengan HX711 dicek duluan (radio WiFi belum aktif sama sekali),
  // hasil deteksi READY/NOT READY jadi akurat mencerminkan wiring asli.
  setupHX711();

  // ---- WIFI ----
  setupWiFi();

  // ---- SERVO ----
  setupServos();

  // ---- WEB SERVER ----
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/trigger", HTTP_GET, handleTrigger);
  server.on("/classify", HTTP_POST, handleClassify);
  server.begin();

  // ---- BANGUNKAN CLOUD API SEDINI MUNGKIN ----
  // Dipanggil sekali di sini supaya kalau server lagi "tidur", proses
  // bangunnya (bisa 40-50 detik) kejadian SEKARANG (pas boot, sebelum ada
  // buah), bukan pas buah pertama lewat dan operator harus nunggu lama.
  if (internetReady)
  {
    lcdShow("Menyiapkan sistem", "Memanaskan Cloud AI..");
    keepCloudAPIWarm();
    lastKeepAlive = millis();
  }

  // ---- READY ----
  Serial.println();
  Serial.println("========================================");
  Serial.println("SYSTEM READY");
  Serial.println("========================================");
  Serial.print("MAIN IP  : "); Serial.println(WiFi.localIP());
  Serial.println("MAIN NAME: main.local");
  Serial.print("CAM TOP  : "); Serial.println(CAM_TOP_HOST);
  Serial.print("CAM SIDE : "); Serial.println(CAM_SIDE_HOST);
  Serial.println();
  Serial.println("Kedua ESP32-CAM & MAIN harus connect ke WiFi yang SAMA");
  Serial.println("(atur lewat hotspot setup masing-masing board via HP).");

  printMenu();

  lcdShow("SORTIR APEL READY", "Menunggu buah..");
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  server.handleClient();
  handleSerial();

  // ---- REVISI: cek & reconnect WiFi otomatis kalau putus ----
  // (Cuma jalan kalau sistem lagi tidak sibuk, sama seperti keep-alive
  // di bawah, supaya tidak mengganggu proses buah yang sedang berjalan.)
  if (!systemBusy && millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL_MS)
  {
    lastWifiCheck = millis();

    bool nowConnected = (WiFi.status() == WL_CONNECTED);

    if (!nowConnected && internetReady)
    {
      // Baru saja putus -> catat & beri tahu operator lewat OLED.
      internetReady = false;
      Serial.println("WIFI TERPUTUS! Mencoba reconnect...");
      lcdShow("WiFi terputus!", "Mencoba sambung..");
    }

    if (!nowConnected)
    {
      WiFi.reconnect();
    }
    else if (!internetReady)
    {
      // Baru saja berhasil sambung kembali.
      internetReady = true;
      Serial.println("WIFI TERSAMBUNG KEMBALI.");
      lcdShow("WiFi tersambung", "kembali");
      delay(1000);
      lcdShow("SORTIR APEL READY", "Menunggu buah..");
    }
  }

  // ---- REVISI: cek status kedua ESP32-CAM secara otomatis ----
  // (Sebelumnya cuma di-update lewat menu serial manual, jadi halaman
  // /status & '/' bisa menampilkan info basi.)
  if (!systemBusy && internetReady &&
      millis() - lastCamCheck >= CAM_CHECK_INTERVAL_MS)
  {
    lastCamCheck = millis();
    camTopOnline  = checkCameraAt(CAM_TOP_HOST, "ATAS");
    camSideOnline = checkCameraAt(CAM_SIDE_HOST, "SAMPING");
  }

  // ---- keep-alive cloud API, jalan di background tiap beberapa menit ----
  // (Cuma jalan kalau sistem lagi tidak sibuk proses buah, supaya nggak
  // nge-block deteksi/push/sortir buah yang sedang berjalan.)
  if (internetReady && !systemBusy &&
      millis() - lastKeepAlive >= KEEP_ALIVE_INTERVAL_MS)
  {
    lastKeepAlive = millis();
    keepCloudAPIWarm();
  }

  // ---- deteksi otomatis buah datang lewat load cell ----
  // Buah dianggap datang kalau berat > threshold dan stabil di
  // atas threshold selama WEIGHT_STABLE_MS berturut-turut.
  // Setelah handleFruitArrival() jalan, buah otomatis pindah ke
  // conveyor lewat servo PUSH, jadi berat akan turun sendiri dan
  // sistem siap mendeteksi buah berikutnya.

  if (AUTO_DETECT_ENABLED && hx711Ready && !systemBusy)
  {
    float w = readWeight();

    if (w > WEIGHT_TRIGGER_THRESHOLD)
    {
      if (readyForNextTrigger)
      {
        if (!weightAboveThreshold)
        {
          weightAboveThreshold = true;
          weightAboveSince = millis();
        }
        else if (millis() - weightAboveSince >= WEIGHT_STABLE_MS)
        {
          handleFruitArrival();
          weightAboveThreshold = false;
          readyForNextTrigger = false; // tunggu berat turun dulu sebelum boleh trigger lagi
        }
      }
    }
    else
    {
      weightAboveThreshold = false;
      readyForNextTrigger = true; // berat sudah turun -> siap deteksi buah berikutnya
    }
  }

  delay(5);
}
