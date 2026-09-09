/**
 * SISTEM SORTIR BUAH (TANPA FOTO)
 * ==========================================================================
 * Google Apps Script - Code.gs
 *
 * FUNGSI:
 * 1. Menerima hasil grading dari ESP32
 * 2. Menulis hasil grading ke Google Sheet (1 baris per grading)
 *
 * CATATAN:
 * Versi ini SENGAJA menghapus semua fitur upload/simpan foto ke
 * Google Drive dan kolom IMAGE() di Sheet, karena membuat file
 * Sheet jadi terlalu berat, lemot, dan sering gagal dibuka.
 *
 * ==========================================================================
 */


/* ==========================================================================
 * KONFIGURASI
 * ========================================================================== */

// Nama sheet
var SHEET_NAME = "Log";

// Secret key.
// HARUS sama dengan yang digunakan oleh ESP32.
var SECRET_KEY = "kelompokPKM";

// Label grading
var GRADE_LABELS = {
  "A": "Bagus",
  "B": "Jelek"
};

// Header Google Sheet
var SHEET_HEADER = [
  "Timestamp",
  "Fruit ID",
  "Nama Buah",
  "Grade",
  "Kualitas",
  "Berat (gram)"
];


/* ==========================================================================
 * 1. DO GET
 * ==========================================================================
 *
 * Digunakan untuk:
 * - pengecekan apakah Web App aktif
 * - kompatibilitas dengan sistem lama (ESP32 kirim data via GET)
 *
 * ========================================================================== */

function doGet(e) {

  try {

    if (e && e.parameter && e.parameter.key) {
      return handleLegacyGet(e);
    }

    return jsonResponse({
      success: true,
      message: "Sistem Google Apps Script aktif",
      service: "Sortir Buah",
      version: "v7-no-photo"
    });

  } catch (err) {

    return jsonResponse({
      success: false,
      error: err.message
    });

  }

}


/* ==========================================================================
 * 2. DO POST
 * ==========================================================================
 *
 * POST digunakan oleh ESP32 untuk mengirim hasil grading.
 *
 * {
 *   "action": "grading",
 *   "key": "kelompokPKM",
 *   "fruitId": "123",
 *   "buah": "apel",
 *   "grade": "A",
 *   "weight": 150
 * }
 *
 * ========================================================================== */

function doPost(e) {

  var lock = LockService.getScriptLock();

  try {
    lock.waitLock(20000);
  } catch (lockErr) {
    return jsonResponse({
      success: false,
      error: "Server sibuk, coba lagi"
    });
  }

  try {

    if (!e || !e.postData || !e.postData.contents) {
      return jsonResponse({ success: false, error: "POST body kosong" });
    }

    var data;
    try {
      data = JSON.parse(e.postData.contents);
    } catch (parseErr) {
      return jsonResponse({ success: false, error: "Body request bukan JSON yang valid" });
    }

    if (!data.key || data.key !== SECRET_KEY) {
      return jsonResponse({ success: false, error: "invalid key" });
    }

    var action = (data.action || "").toString().trim();

    if (action === "grading") {
      return handleGrading(data);
    }

    // Kalau ada request upload_photo yang masih nyasar dari alat lama,
    // kita tolak dengan sopan (tidak lagi didukung).
    if (action === "upload_photo") {
      return jsonResponse({
        success: false,
        error: "Fitur upload foto sudah dinonaktifkan"
      });
    }

    return jsonResponse({
      success: false,
      error: "action tidak dikenal. Gunakan 'grading'"
    });

  } catch (err) {

    return jsonResponse({ success: false, error: err.message });

  } finally {

    lock.releaseLock();

  }

}


/* ==========================================================================
 * 3. HANDLE GRADING
 * ==========================================================================
 * Langsung menulis 1 baris ke Sheet, tanpa menunggu foto.
 * ========================================================================== */

function handleGrading(data) {

  var validation = validateGradingData(data);
  if (!validation.ok) {
    return jsonResponse({ success: false, error: validation.error });
  }

  var sheet = getOrCreateLogSheet();
  ensureHeader(sheet);

  sheet.appendRow([
    new Date(),
    data.fruitId || "",
    validation.buah,
    validation.grade,
    validation.kualitas,
    Number(data.weight)
  ]);

  return jsonResponse({
    success: true,
    action: "grading"
  });

}


/* ==========================================================================
 * 4. VALIDASI DATA GRADING
 * ========================================================================== */

function validateGradingData(data) {

  var buah = (data.buah || "").toString().trim();
  if (!buah) {
    return { ok: false, error: "parameter 'buah' kosong/tidak ada" };
  }

  var grade = (data.grade || "").toString().trim().toUpperCase();
  if (!GRADE_LABELS.hasOwnProperty(grade)) {
    return {
      ok: false,
      error: "parameter 'grade' harus 'A' atau 'B', dapat: '" + grade + "'"
    };
  }

  var weight = data.weight;
  if (weight === undefined || weight === null || weight === "" || isNaN(Number(weight))) {
    return { ok: false, error: "parameter 'weight' harus berupa angka" };
  }

  return {
    ok: true,
    buah: buah,
    grade: grade,
    kualitas: GRADE_LABELS[grade]
  };

}


/* ==========================================================================
 * 5. GET / CREATE SHEET
 * ========================================================================== */

function getOrCreateLogSheet() {

  var spreadsheet = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = spreadsheet.getSheetByName(SHEET_NAME);

  if (!sheet) {
    sheet = spreadsheet.insertSheet(SHEET_NAME);
  }

  return sheet;

}


/* ==========================================================================
 * 6. ENSURE HEADER
 * ========================================================================== */

function ensureHeader(sheet) {

  if (sheet.getLastRow() === 0) {
    sheet.getRange(1, 1, 1, SHEET_HEADER.length).setValues([SHEET_HEADER]);
    return;
  }

  var currentHeader = sheet.getRange(1, 1, 1, SHEET_HEADER.length).getValues()[0];

  var isSame = true;
  for (var i = 0; i < SHEET_HEADER.length; i++) {
    if (currentHeader[i] !== SHEET_HEADER[i]) {
      isSame = false;
      break;
    }
  }

  if (!isSame) {
    sheet.getRange(1, 1, 1, SHEET_HEADER.length).setValues([SHEET_HEADER]);
  }

}


/* ==========================================================================
 * 7. LEGACY GET
 * ==========================================================================
 *
 * Mendukung ESP32 versi lama yang kirim data lewat URL (GET):
 *
 * ?key=kelompokPKM&fruitId=123&buah=apel&grade=A&weight=150
 *
 * ========================================================================== */

function handleLegacyGet(e) {

  var data = e.parameter;

  if (!data.key || data.key !== SECRET_KEY) {
    return jsonResponse({ success: false, error: "invalid key" });
  }

  var validation = validateGradingData(data);
  if (!validation.ok) {
    return jsonResponse({ success: false, error: validation.error });
  }

  var sheet = getOrCreateLogSheet();
  ensureHeader(sheet);

  sheet.appendRow([
    new Date(),
    data.fruitId || "",
    validation.buah,
    validation.grade,
    validation.kualitas,
    Number(data.weight)
  ]);

  return jsonResponse({ success: true, mode: "legacy_get" });

}


/* ==========================================================================
 * 8. RESET LOG SHEET
 * ==========================================================================
 *
 * JALANKAN MANUAL DARI EDITOR APPS SCRIPT (bukan lewat URL).
 * Ini akan menghapus SEMUA isi sheet "Log" dan memasang ulang headernya.
 *
 * ========================================================================== */

function resetLogSheet() {

  var sheet = getOrCreateLogSheet();

  sheet.clear();

  sheet.getRange(1, 1, 1, SHEET_HEADER.length).setValues([SHEET_HEADER]);

  SpreadsheetApp.flush();

  Logger.log("Sheet '" + SHEET_NAME + "' berhasil direset.");

}


/* ==========================================================================
 * 9. JSON RESPONSE
 * ========================================================================== */

function jsonResponse(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
