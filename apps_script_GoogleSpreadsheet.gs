/**
 * LOG HASIL GRADING BUAH KE GOOGLE SHEET
 * ==========================================================================
 * REVISI v3 - SELARAS DENGAN app.py v3 (multi-buah, tanpa grade atas/samping):
 * - Sebelumnya sheet mencatat "Grade Atas" & "Grade Samping" terpisah, sisa
 *   dari skema scan multi-sudut lama. Sekarang app.py cuma menghasilkan SATU
 *   grade final per buah, jadi kolom itu DIHAPUS. Yang dicatat sekarang:
 *   Nama Buah, Grade, Kualitas (Bagus/Jelek, taruh di sebelah Berat), Berat.
 * - Kolom "Kualitas" DIHITUNG DI SINI dari parameter "grade" yang dikirim
 *   ESP32 (A -> Bagus, B -> Jelek) lewat GRADE_LABELS di bawah, bukan
 *   dikirim terpisah -- supaya mapping grade->label cuma hidup di SATU
 *   tempat dan tidak bisa beda-beda antar perangkat/kode.
 * - Ditambahkan LockService: begitu ada lebih dari satu unit ESP32 nge-log
 *   hampir bersamaan, appendRow() tidak saling tabrakan/kehilangan baris.
 * - Ditambahkan validasi parameter (buah wajib ada, grade wajib A/B, weight
 *   wajib angka) SEBELUM menulis ke sheet, supaya baris sampah tidak masuk.
 * - try/catch/finally di doGet() DIPERTAHANKAN dari revisi sebelumnya: kalau
 *   ada error (sheet ke-hapus, kuota Google habis, dll), Apps Script tetap
 *   membalas HTTP 200 (quirk Apps Script) tapi body-nya eksplisit
 *   {success:false, error:"..."} supaya ESP32 (yang cuma cek "code == 200")
 *   tidak salah mengira log berhasil.
 *
 * PARAMETER YANG DIHARAPKAN DARI ESP32 (GET query string):
 *   key     - secret key, HARUS SAMA dengan SHEET_SECRET_KEY di kode ESP32
 *   fruitId - ID/nomor urut buah (opsional, buat tracing)
 *   buah    - nama buah, mis. "apel" (dari field "buah" respons app.py)
 *   grade   - "A" atau "B" (dari field "grade" respons app.py)
 *   weight  - berat dalam gram (angka)
 *
 * CATATAN: kalau app.py membalas status "unsure" (buah/grade = null), ESP32
 * SEHARUSNYA TIDAK memanggil endpoint ini sama sekali (masuk jalur reject,
 * bukan dicatat sebagai grade sah). Validasi di bawah tetap menjaga-jaga
 * kalau ternyata grade yang terkirim bukan "A"/"B".
 */

// A = bagus/segar, B = jelek/busuk. SATU-SATUNYA tempat mapping ini
// didefinisikan di sisi logging -- kalau training script nanti menambah
// grade baru, cukup update di sini.
var GRADE_LABELS = { "A": "Bagus", "B": "Jelek" };

var SHEET_NAME = "Log";
var SECRET_KEY = "kelompokPKM"; // HARUS SAMA dengan SHEET_SECRET_KEY di kode ESP32
var SHEET_HEADER = ["Timestamp", "Fruit ID", "Nama Buah", "Grade", "Kualitas", "Berat (gram)"];

function doGet(e) {
  // Lock supaya beberapa request nyaris bersamaan (multi-unit ESP32) tidak
  // saling menimpa saat appendRow(). Tunggu maks 10 detik sebelum menyerah.
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
  } catch (lockErr) {
    return jsonResponse({ success: false, error: "Server sibuk, coba lagi (lock timeout)" });
  }

  try {
    var data = e.parameter; // dari parameter URL (GET), bukan dari body JSON

    if (data.key !== SECRET_KEY) {
      return jsonResponse({ success: false, error: "invalid key" });
    }

    var buah = (data.buah || "").toString().trim();
    var grade = (data.grade || "").toString().trim().toUpperCase();
    var weight = data.weight;

    if (!buah) {
      return jsonResponse({ success: false, error: "parameter 'buah' kosong/tidak ada" });
    }
    if (!GRADE_LABELS.hasOwnProperty(grade)) {
      return jsonResponse({ success: false, error: "parameter 'grade' harus 'A' atau 'B', dapat: '" + grade + "'" });
    }
    if (weight === undefined || weight === "" || isNaN(Number(weight))) {
      return jsonResponse({ success: false, error: "parameter 'weight' harus berupa angka" });
    }

    var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME)
                || SpreadsheetApp.getActiveSpreadsheet().insertSheet(SHEET_NAME);

    if (sheet.getLastRow() === 0) {
      sheet.appendRow(SHEET_HEADER);
    }

    var kualitas = GRADE_LABELS[grade];

    sheet.appendRow([
      new Date(),
      data.fruitId || "",
      buah,
      grade,
      kualitas,
      Number(weight)
    ]);

    return jsonResponse({ success: true });

  } catch (err) {
    return jsonResponse({ success: false, error: err.message });
  } finally {
    lock.releaseLock();
  }
}

function jsonResponse(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
