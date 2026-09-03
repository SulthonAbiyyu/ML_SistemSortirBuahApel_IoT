/**
 * LOG HASIL GRADING BUAH KE GOOGLE SHEET
 * ==========================================================================
 * REVISI v4 - NAMBAH DUKUNGAN FOTO (thumbnail di dalam sel):
 * - ESP32 sekarang kirim data lewat POST (JSON body), bukan GET query
 *   string lagi -- soalnya foto (base64) terlalu besar buat dimasukin ke
 *   URL. doGet() TETAP dipertahankan untuk kompatibilitas mundur (kalau ada
 *   perangkat lama yang masih manggil pakai GET tanpa foto).
 * - Foto (imgAtas / imgSamping, base64 JPEG) di-decode lalu disimpan sebagai
 *   file di folder Google Drive "Foto Sortir Buah" (dibuat otomatis kalau
 *   belum ada). File di-share "siapa saja yang punya link" (view only),
 *   supaya URL-nya bisa dipakai fungsi IMAGE() di Sheet.
 * - Kolom baru: "Foto Atas" dan "Foto Samping", isinya formula =IMAGE(url)
 *   yang bikin foto muncul sebagai thumbnail langsung di dalam sel.
 * - Kalau salah satu foto tidak dikirim (mis. kamera ATAS lagi rusak),
 *   kolom foto itu dikosongin aja -- tidak menggagalkan pencatatan baris.
 * - ensureHeader(): kalau header di baris 1 beda dari SHEET_HEADER yang
 *   didefinisikan di bawah (misal masih header versi lama), header
 *   OTOMATIS ditimpa ke versi terbaru. Ini juga jadi ekstra pengaman kalau
 *   dulu sempat ke-mismatch antara skema lama & baru.
 * - LockService dipertahankan supaya beberapa ESP32 yang log hampir
 *   bersamaan tidak saling menimpa baris.
 *
 * PARAMETER YANG DIHARAPKAN DARI ESP32 (JSON body lewat POST):
 *   key         - secret key, HARUS SAMA dengan SHEET_SECRET_KEY di ESP32
 *   fruitId     - ID/nomor urut buah (opsional, buat tracing)
 *   buah        - nama buah, mis. "apel"
 *   grade       - "A" atau "B"
 *   weight      - berat dalam gram (angka)
 *   imgAtas     - (opsional) foto kamera ATAS, base64 JPEG tanpa prefix data:
 *   imgSamping  - (opsional) foto kamera SAMPING, base64 JPEG tanpa prefix
 *
 * PENTING: setiap kali script ini diedit, WAJIB deploy ulang sebagai versi
 * BARU (Deploy > Manage deployments > Edit > New version), kalau tidak,
 * URL Web App lama tetap menjalankan kode versi SEBELUMNYA.
 */

var GRADE_LABELS = { "A": "Bagus", "B": "Jelek" };
var SHEET_NAME = "Log";
var SECRET_KEY = "kelompokPKM"; // HARUS SAMA dengan SHEET_SECRET_KEY di kode ESP32
var DRIVE_FOLDER_NAME = "Foto Sortir Buah"; // folder Drive tempat nyimpen foto
var SHEET_HEADER = ["Timestamp", "Fruit ID", "Nama Buah", "Grade", "Kualitas", "Berat (gram)", "Foto Atas", "Foto Samping"];
var THUMBNAIL_ROW_HEIGHT = 100; // px, biar thumbnail kelihatan penuh di sel

// ESP32 (versi baru) manggil endpoint ini lewat POST supaya bisa kirim
// body JSON yang isinya foto base64 (kalau ada).
function doPost(e) {
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
  } catch (lockErr) {
    return jsonResponse({ success: false, error: "Server sibuk, coba lagi (lock timeout)" });
  }

  try {
    var data;
    try {
      data = JSON.parse(e.postData.contents);
    } catch (parseErr) {
      return jsonResponse({ success: false, error: "Body request bukan JSON yang valid" });
    }

    var validation = validateAndPrep(data);
    if (!validation.ok) {
      return jsonResponse({ success: false, error: validation.error });
    }

    var sheet = getOrCreateLogSheet();
    ensureHeader(sheet);

    var rowIndex = sheet.getLastRow() + 1;

    sheet.getRange(rowIndex, 1, 1, 6).setValues([[
      new Date(),
      data.fruitId || "",
      validation.buah,
      validation.grade,
      validation.kualitas,
      Number(data.weight)
    ]]);

    var fotoAtasFormula = uploadPhotoAndGetImageFormula(data.imgAtas, data.fruitId, "atas");
    var fotoSampingFormula = uploadPhotoAndGetImageFormula(data.imgSamping, data.fruitId, "samping");

    if (fotoAtasFormula) {
      sheet.getRange(rowIndex, 7).setFormula(fotoAtasFormula);
    }
    if (fotoSampingFormula) {
      sheet.getRange(rowIndex, 8).setFormula(fotoSampingFormula);
    }
    if (fotoAtasFormula || fotoSampingFormula) {
      sheet.setRowHeight(rowIndex, THUMBNAIL_ROW_HEIGHT);
    }

    return jsonResponse({ success: true });

  } catch (err) {
    return jsonResponse({ success: false, error: err.message });
  } finally {
    lock.releaseLock();
  }
}

// Dipertahankan untuk kompatibilitas mundur -- ESP32 lama (tanpa fitur foto)
// yang masih manggil pakai GET query string tetap bisa jalan, cuma tanpa foto.
function doGet(e) {
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
  } catch (lockErr) {
    return jsonResponse({ success: false, error: "Server sibuk, coba lagi (lock timeout)" });
  }

  try {
    var data = e.parameter; // dari parameter URL (GET)

    var validation = validateAndPrep(data);
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

    return jsonResponse({ success: true });

  } catch (err) {
    return jsonResponse({ success: false, error: err.message });
  } finally {
    lock.releaseLock();
  }
}

// Validasi & siapin field yang dipakai bareng oleh doGet() dan doPost(),
// supaya aturan validasinya cuma didefinisikan di SATU tempat.
function validateAndPrep(data) {
  if (data.key !== SECRET_KEY) {
    return { ok: false, error: "invalid key" };
  }

  var buah = (data.buah || "").toString().trim();
  var grade = (data.grade || "").toString().trim().toUpperCase();
  var weight = data.weight;

  if (!buah) {
    return { ok: false, error: "parameter 'buah' kosong/tidak ada" };
  }
  if (!GRADE_LABELS.hasOwnProperty(grade)) {
    return { ok: false, error: "parameter 'grade' harus 'A' atau 'B', dapat: '" + grade + "'" };
  }
  if (weight === undefined || weight === "" || isNaN(Number(weight))) {
    return { ok: false, error: "parameter 'weight' harus berupa angka" };
  }

  return {
    ok: true,
    buah: buah,
    grade: grade,
    kualitas: GRADE_LABELS[grade]
  };
}

function getOrCreateLogSheet() {
  return SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME)
      || SpreadsheetApp.getActiveSpreadsheet().insertSheet(SHEET_NAME);
}

// Pasang/timpa header baris 1 supaya selalu sesuai SHEET_HEADER terbaru --
// termasuk kalau sebelumnya sheet masih pakai header skema versi lama.
function ensureHeader(sheet) {
  if (sheet.getLastRow() === 0) {
    sheet.appendRow(SHEET_HEADER);
    return;
  }

  var currentHeader = sheet.getRange(1, 1, 1, SHEET_HEADER.length).getValues()[0];
  var isSame = true;
  for (var i = 0; i < SHEET_HEADER.length; i++) {
    if (currentHeader[i] !== SHEET_HEADER[i]) { isSame = false; break; }
  }
  if (!isSame) {
    sheet.getRange(1, 1, 1, SHEET_HEADER.length).setValues([SHEET_HEADER]);
  }
}

// Decode base64 -> simpan ke Drive -> hasilkan formula =IMAGE(url) yang
// bikin fotonya muncul sebagai thumbnail langsung di dalam sel Sheet.
// Return null kalau base64Data kosong/tidak ada (kamera itu lagi gagal).
function uploadPhotoAndGetImageFormula(base64Data, fruitId, label) {
  if (!base64Data) return null;

  try {
    var folder = getOrCreatePhotoFolder();
    var decoded = Utilities.base64Decode(base64Data);
    var fileName = "buah_" + (fruitId || "x") + "_" + label + "_" + new Date().getTime() + ".jpg";
    var blob = Utilities.newBlob(decoded, "image/jpeg", fileName);
    var file = folder.createFile(blob);
    file.setSharing(DriveApp.Access.ANYONE_WITH_LINK, DriveApp.Permission.VIEW);

    var imageUrl = "https://drive.google.com/uc?export=view&id=" + file.getId();
    return '=IMAGE("' + imageUrl + '")';
  } catch (uploadErr) {
    Logger.log("Gagal upload foto (" + label + "): " + uploadErr.message);
    return null;
  }
}

function getOrCreatePhotoFolder() {
  var folders = DriveApp.getFoldersByName(DRIVE_FOLDER_NAME);
  if (folders.hasNext()) {
    return folders.next();
  }
  return DriveApp.createFolder(DRIVE_FOLDER_NAME);
}

function jsonResponse(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

// ==========================================================================
// RESET LOG -- JALANKAN MANUAL DARI SINI (editor Apps Script), BUKAN lewat
// URL Web App. Sengaja TIDAK dibuat sebagai endpoint doGet/doPost supaya
// tidak ada orang lain yang bisa nge-reset data cuma dengan tahu URL-nya.
//
// Cara pakai:
//   1. Di toolbar atas editor Apps Script, pilih fungsi "resetLogSheet" dari
//      dropdown daftar fungsi (di sebelah tombol Run/segitiga).
//   2. Klik Run (▶). Kalau ini pertama kali, akan diminta izin akses --
//      klik Review permissions > pilih akun Google kamu > Allow.
//   3. Cek tab "Log" di Sheet -- semua baris data lama sudah hilang, cuma
//      tersisa header sesuai SHEET_HEADER yang terbaru.
//
// CATATAN: ini cuma menghapus ISI SHEET-nya. File foto lama di folder Drive
// "Foto Sortir Buah" TIDAK ikut terhapus (linknya di sheet saja yang hilang)
// -- kalau mau bersih total, hapus manual folder itu dari Google Drive.
function resetLogSheet() {
  var sheet = getOrCreateLogSheet();
  sheet.clear();
  sheet.appendRow(SHEET_HEADER);
  SpreadsheetApp.flush();
  Logger.log("Sheet '" + SHEET_NAME + "' sudah direset. Header terpasang: " + SHEET_HEADER.join(", "));
}
