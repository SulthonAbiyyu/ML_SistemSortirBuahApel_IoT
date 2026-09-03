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

#include "mbedtls/base64.h"

const char* WIFI_SETUP_AP_NAME     = "SORTIR-APEL-MAIN-SETUP";
const char* WIFI_SETUP_AP_PASSWORD = "sortirapel123";

WiFiManager wm;

String CLOUD_API_URL = "https://ml-sistemsortirbuahapel-iot.onrender.com/predict";

String SHEET_WEBAPP_URL = "https://script.google.com/macros/s/AKfycbxXaqN60uyMrsx000K40tFXMIs4NVsxpNYME9OlSjjoI-WDjyLVXw7W87ir9ZHUSyF7vA/exec";

const char* SHEET_SECRET_KEY = "kelompokPKM";

const char* CAM_TOP_HOST  = "camtop.local";
const char* CAM_SIDE_HOST = "camside.local";

WebServer server(80);

#define HX711_DT_PIN   4
#define HX711_SCK_PIN  5

#define SERVO_PUSH_PIN 13

#define SERVO_SORT_PIN 14

#define BUZZER_PIN     25

#define OLED_SDA_PIN     21
#define OLED_SCL_PIN     22

#define OLED_I2C_ADDRESS 0x3C
#define OLED_WIDTH       128
#define OLED_HEIGHT      64
#define OLED_RESET_PIN   -1

HX711 scale;
Preferences prefs;

bool hx711Ready = false;

float CALIBRATION_FACTOR = 1.0;

bool AUTO_DETECT_ENABLED = true;

float WEIGHT_TRIGGER_THRESHOLD = 20.0;
const unsigned long WEIGHT_STABLE_MS = 300;

bool weightAboveThreshold = false;
unsigned long weightAboveSince = 0;

bool readyForNextTrigger = true;

Servo servoPush;
Servo servoSort;

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
bool lcdReady = false;

const int PUSH_IDLE_ANGLE   = 0;
const int PUSH_ACTIVE_ANGLE = 90;
const int PUSH_HOLD_MS      = 600;

const int SORT_IDLE_ANGLE = 0;
const int SORT_A_ANGLE    = 30;
const int SORT_B_ANGLE    = 55;
const int SORT_C_ANGLE    = 90;
const int SORT_HOLD_MS    = 10000;

bool camTopOnline  = false;
bool camSideOnline = false;

bool systemBusy = false;

unsigned long fruitID = 0;

bool internetReady = false;

void setupWiFi()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("START WIFI (WiFiManager - ganti WiFi lewat HP)");
  Serial.println("========================================");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  pinMode(0, INPUT_PULLUP);
  delay(50);
  if (digitalRead(0) == LOW)
  {
    Serial.println("Tombol BOOT ditekan saat nyala -> hapus WiFi tersimpan.");
    wm.resetSettings();
  }

  wm.setConfigPortalTimeout(180);

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

float readWeight()
{
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

unsigned long lastKeepAlive = 0;
const unsigned long KEEP_ALIVE_INTERVAL_MS = 8UL * 60UL * 1000UL;

unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 15UL * 1000UL;

unsigned long lastCamCheck = 0;
const unsigned long CAM_CHECK_INTERVAL_MS = 30UL * 1000UL;
bool cloudAPIWarm = false;

String getCloudAPIRootURL()
{
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
  secureClient.setInsecure();
  String rootUrl = getCloudAPIRootURL();

  Serial.println();
  Serial.println("KEEP-ALIVE: ngecek/membangunkan cloud API...");

  http.setConnectTimeout(60000);

  http.setTimeout(70000);

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
  secureClient.setInsecure();

  Serial.print("Mengirim foto ke cloud API: "); Serial.println(CLOUD_API_URL);
  Serial.print("Sisa RAM sebelum request: "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");

  http.setConnectTimeout(60000);

  http.setTimeout(70000);

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

  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, response);

  if (jsonErr)
  {
    Serial.print("GAGAL PARSE JSON DARI CLOUD API: ");
    Serial.println(jsonErr.c_str());
    return false;
  }

  String status = doc["status"] | "";
  String grade  = doc["grade"]  | "";
  String buah   = doc["buah"]   | "";

  float confidence = doc["confidence"] | 0.0;

  Serial.print("STATUS: "); Serial.print(status);
  Serial.print(" | BUAH: "); Serial.print(buah.length() ? buah : "-");
  Serial.print(" | GRADE: "); Serial.print(grade.length() ? grade : "-");
  Serial.print(" | CONFIDENCE: "); Serial.println(confidence, 4);

  if (status != "ok" || (grade != "A" && grade != "B"))
  {
    Serial.println("HASIL TIDAK VALID (status bukan 'ok', atau grade bukan A/B) -> dianggap gagal deteksi.");
    return false;
  }

  outGrade = grade.charAt(0);
  outBuah  = buah;
  return true;
}

char worseGrade(char a, char b)
{
  auto rank = [](char g) -> int
  {
    if (g == 'A') return 0;
    if (g == 'B') return 1;
    return -1;
  };

  int ra = rank(a);
  int rb = rank(b);

  if (ra < 0) return b;
  if (rb < 0) return a;

  return (ra > rb) ? a : b;
}

// Encode buffer biner (foto JPEG) jadi string base64, dipakai buat kirim
// foto ke Google Apps Script lewat body JSON (Apps Script cuma nerima teks).
String base64EncodeBuffer(const uint8_t* data, size_t len)
{
  if (data == nullptr || len == 0) return "";

  size_t outputLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outputLen, data, len); // hitung ukuran output dulu

  uint8_t* outputBuf = (uint8_t*) malloc(outputLen + 1);
  if (outputBuf == nullptr)
  {
    Serial.println("LOG SHEET: gagal alokasi memori buat base64 encode foto.");
    return "";
  }

  size_t actualLen = 0;
  int ret = mbedtls_base64_encode(outputBuf, outputLen, &actualLen, data, len);
  if (ret != 0)
  {
    free(outputBuf);
    Serial.println("LOG SHEET: base64 encode foto gagal.");
    return "";
  }

  outputBuf[actualLen] = '\0';
  String result = String((char*) outputBuf);
  free(outputBuf);
  return result;
}

// Escape tanda kutip/backslash biar nama buah aman dimasukin ke string JSON manual.
String jsonEscape(const String &s)
{
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s.charAt(i);
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

void logToGoogleSheet(unsigned long fruitIdVal, char gradeVal, float weightVal,
                       String buahVal,
                       const uint8_t* imgTop, size_t imgTopLen,
                       const uint8_t* imgSide, size_t imgSideLen)
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

  if (gradeVal != 'A' && gradeVal != 'B')
  {
    Serial.print("LOG SHEET: dilewati, grade tidak valid untuk dicatat ('");
    Serial.print(gradeVal);
    Serial.println("').");
    return;
  }

  if (buahVal.length() == 0)
  {
    Serial.println("LOG SHEET: dilewati, nama buah kosong.");
    return;
  }

  Serial.println("LOG SHEET: encode foto ke base64...");
  String imgTopB64 = base64EncodeBuffer(imgTop, imgTopLen);
  String imgSideB64 = base64EncodeBuffer(imgSide, imgSideLen);

  size_t estimatedLen = 200 + imgTopB64.length() + imgSideB64.length();
  String body;
  body.reserve(estimatedLen);

  body += "{";
  body += "\"key\":\"" + String(SHEET_SECRET_KEY) + "\",";
  body += "\"fruitId\":" + String(fruitIdVal) + ",";
  body += "\"buah\":\"" + jsonEscape(buahVal) + "\",";
  body += "\"grade\":\"" + String(gradeVal) + "\",";
  body += "\"weight\":" + String(weightVal, 1);
  if (imgTopB64.length() > 0)
  {
    body += ",\"imgAtas\":\"" + imgTopB64 + "\"";
  }
  if (imgSideB64.length() > 0)
  {
    body += ",\"imgSamping\":\"" + imgSideB64 + "\"";
  }
  body += "}";

  imgTopB64 = "";  // bebasin memori secepatnya, gak dipakai lagi
  imgSideB64 = "";

  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  http.setConnectTimeout(8000);
  http.setTimeout(20000); // upload foto butuh waktu lebih lama dari sekadar teks

  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  if (!http.begin(secureClient, SHEET_WEBAPP_URL))
  {
    Serial.println("LOG SHEET: HTTP BEGIN GAGAL");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  Serial.print("LOG SHEET: mengirim data + foto (");
  Serial.print(body.length());
  Serial.println(" bytes)...");

  int code = http.POST(body);
  body = ""; // bebasin memori body abis dikirim

  if (code == 200)
  {
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

void handleFruitArrival()
{
  fruitID++;

  Serial.println();
  Serial.println("########################################");
  Serial.print("BUAH TERDETEKSI - ID: ");
  Serial.println(fruitID);
  Serial.println("########################################");

  lcdShow("Buah #" + String(fruitID), "Menimbang...");

  float weight = readWeight();
  Serial.print("Berat: ");
  Serial.print(weight, 1);
  Serial.println(" gram");

  lcdShow("Berat: " + String(weight, 1) + "g", "Memotret...");

  triggerBothCameras();

  buzzerBeep(1, 150);

  lcdShow("Buah #" + String(fruitID), "Push ke conveyor");

  pushToConveyor();

  Serial.println("Buah sudah di conveyor. Mengirim foto ke cloud AI...");

  if (cloudAPIWarm)
  {
    lcdShow("Buah #" + String(fruitID), "Cek Cloud AI...");
  }
  else
  {
    lcdShow("Memanaskan server", "Mohon tunggu (<=70dtk)");
  }

  char gradeTop = 0;
  char gradeSide = 0;
  bool successTop = false;
  bool successSide = false;

  String buahTop = "";
  String buahSide = "";

  size_t imgLenTop = 0;
  uint8_t* imgDataTop = fetchImageFromCam(CAM_TOP_HOST, imgLenTop);

  if (imgDataTop != nullptr && imgLenTop > 0)
  {
    // imgDataTop SENGAJA belum di-free() di sini -- masih dipakai buat
    // dilampirkan ke Google Sheets nanti. Di-free() di akhir fungsi ini.
    successTop = classifyWithCloudAPI(imgDataTop, imgLenTop, gradeTop, buahTop);
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
    // Sama kayak imgDataTop, belum di-free() -- dipakai buat lampiran Sheets.
    successSide = classifyWithCloudAPI(imgDataSide, imgLenSide, gradeSide, buahSide);
  }
  else
  {
    Serial.println("GAGAL AMBIL FOTO DARI KAMERA SAMPING.");
  }

  Serial.print("GRADE SAMPING: ");
  Serial.println(successSide ? String(gradeSide) : "GAGAL");

  bool success = successTop || successSide;
  char grade = 0;

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

  if (success)
  {
    // Foto ATAS/SAMPING masih ada di memori (belum di-free), jadi bisa
    // ikut dilampirkan ke Google Sheets sebagai thumbnail.
    logToGoogleSheet(fruitID, grade, weight, buahFinal,
                      imgDataTop, imgLenTop, imgDataSide, imgLenSide);

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
    buzzerBeep(3, 150);

    lcdShow("Buah #" + String(fruitID), "GAGAL! -> Jalur Belakang");

    sortFruit('C');
  }

  // Bebasin memori foto di titik ini -- SETELAH klasifikasi + logging Sheets
  // selesai dipakai, baik itu jalur sukses maupun gagal di atas.
  if (imgDataTop != nullptr) { free(imgDataTop); imgDataTop = nullptr; }
  if (imgDataSide != nullptr) { free(imgDataSide); imgDataSide = nullptr; }

  buzzerBeep(1, 150);

  lcdShow("SORTIR APEL READY", "Menunggu buah..");
}

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

void handleCapture()
{
  triggerBothCameras();

  server.send(200, "application/json",
    "{\"success\":true,\"message\":\"capture_requested_both_cams\"}");
}

void handleTrigger()
{
  if (systemBusy)
  {
    server.send(409, "application/json",
      "{\"success\":false,\"error\":\"system_busy\"}");
    return;
  }

  handleFruitArrival();
  readyForNextTrigger = false;

  server.send(200, "application/json",
    "{\"success\":true,\"message\":\"fruit_flow_triggered\",\"fruit_id\":" + String(fruitID) + "}");
}

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
      readyForNextTrigger = false;
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

bool runComponentSelfTest()
{
  Serial.println();
  Serial.println("========================================");
  Serial.println("SELF-TEST KOMPONEN");
  Serial.println("========================================");

  bool allOk = true;

  Serial.print("HX711 (load cell)  : ");
  Serial.println(hx711Ready ? "OK" : "TIDAK TERDETEKSI");
  if (!hx711Ready) allOk = false;

  Serial.print("OLED               : ");
  Serial.println(lcdReady ? "OK" : "TIDAK TERDETEKSI");
  if (!lcdReady) allOk = false;

  Serial.print("WiFi/Internet      : ");
  Serial.println(internetReady ? "OK" : "TIDAK TERDETEKSI");
  if (!internetReady) allOk = false;

  Serial.print("Servo PUSH         : ");
  Serial.println(servoPush.attached() ? "OK" : "TIDAK TERDETEKSI");
  if (!servoPush.attached()) allOk = false;

  Serial.print("Servo SORTIR       : ");
  Serial.println(servoSort.attached() ? "OK" : "TIDAK TERDETEKSI");
  if (!servoSort.attached()) allOk = false;

  if (internetReady)
  {
    camTopOnline = checkCameraAt(CAM_TOP_HOST, "ATAS");
    Serial.print("Kamera ATAS        : ");
    Serial.println(camTopOnline ? "OK" : "TIDAK TERDETEKSI");
    if (!camTopOnline) allOk = false;

    camSideOnline = checkCameraAt(CAM_SIDE_HOST, "SAMPING");
    Serial.print("Kamera SAMPING     : ");
    Serial.println(camSideOnline ? "OK" : "TIDAK TERDETEKSI");
    if (!camSideOnline) allOk = false;
  }
  else
  {
    Serial.println("Kamera ATAS        : DILEWATI (tidak ada internet)");
    Serial.println("Kamera SAMPING     : DILEWATI (tidak ada internet)");
    allOk = false;
  }

  Serial.println("========================================");

  if (allOk)
  {
    Serial.println("SELF-TEST: SEMUA KOMPONEN OK");
  }
  else
  {
    Serial.println("SELF-TEST: ADA KOMPONEN TIDAK TERDETEKSI (lihat daftar di atas)");
    lcdShow("CEK KOMPONEN!", "Lihat Serial Monitor");
    buzzerBeep(5, 150);
  }

  return allOk;
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("########################################");
  Serial.println("       SORTIR APEL ESP32 MAIN - REVISI");
  Serial.println("########################################");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  setupLCD();

  setupHX711();

  setupWiFi();

  setupServos();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/trigger", HTTP_GET, handleTrigger);
  server.on("/classify", HTTP_POST, handleClassify);
  server.begin();

  runComponentSelfTest();

  if (internetReady)
  {
    buzzerBeep(1, 150);
    lcdShow("SORTIR APEL READY", "Menghangatkan AI..");
    // Cloud API di-"panasin" secara async lewat loop(), bukan di setup(),
    // supaya boot tidak nge-block lama dan memicu watchdog reset
    // (Render.com free tier bisa butuh 30-60+ detik buat cold-start).
    lastKeepAlive = millis() - KEEP_ALIVE_INTERVAL_MS; // biar langsung dicoba di loop() pertama
  }

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

  buzzerBeep(3, 150);

  lcdShow("SORTIR APEL READY", "Menunggu buah..");
}

void loop()
{
  server.handleClient();
  handleSerial();

  if (!systemBusy && millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL_MS)
  {
    lastWifiCheck = millis();

    bool nowConnected = (WiFi.status() == WL_CONNECTED);

    if (!nowConnected && internetReady)
    {
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
      internetReady = true;
      Serial.println("WIFI TERSAMBUNG KEMBALI.");
      lcdShow("WiFi tersambung", "kembali");
      delay(1000);
      lcdShow("SORTIR APEL READY", "Menunggu buah..");
    }
  }

  if (!systemBusy && internetReady &&
      millis() - lastCamCheck >= CAM_CHECK_INTERVAL_MS)
  {
    lastCamCheck = millis();
    camTopOnline  = checkCameraAt(CAM_TOP_HOST, "ATAS");
    camSideOnline = checkCameraAt(CAM_SIDE_HOST, "SAMPING");
  }

  if (internetReady && !systemBusy &&
      millis() - lastKeepAlive >= KEEP_ALIVE_INTERVAL_MS)
  {
    lastKeepAlive = millis();
    keepCloudAPIWarm();
  }

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
          readyForNextTrigger = false;
        }
      }
    }
    else
    {
      weightAboveThreshold = false;
      readyForNextTrigger = true;
    }
  }

  delay(5);
}

