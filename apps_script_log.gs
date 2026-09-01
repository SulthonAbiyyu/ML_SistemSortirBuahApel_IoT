/**
 * REVISI dari versi sebelumnya: seluruh isi doGet() sekarang dibungkus
 * try/catch. Sebelumnya, kalau appendRow() gagal (misal sheet ke-hapus,
 * kuota Google habis, dll), Apps Script akan melempar error mentah yang
 * bisa jadi tetap dibalas dengan HTTP 200 (quirk Apps Script) -- akibatnya
 * ESP32 MAIN (yang cuma mengecek "code == 200") mengira log berhasil
 * tercatat padahal sebenarnya gagal. Sekarang errornya ditangkap dan
 * dibalas eksplisit sebagai {success:false, error:"..."} supaya jelas.
 */
function doGet(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName("Log")
                || SpreadsheetApp.getActiveSpreadsheet().insertSheet("Log");

    var SECRET_KEY = "kelompokPKM"; // HARUS SAMA dengan SHEET_SECRET_KEY di kode ESP32

    var data = e.parameter; // dari parameter URL (GET), bukan dari body JSON

    if (data.key !== SECRET_KEY) {
      return ContentService.createTextOutput(JSON.stringify({success: false, error: "invalid key"}))
        .setMimeType(ContentService.MimeType.JSON);
    }

    if (sheet.getLastRow() === 0) {
      sheet.appendRow(["Timestamp", "Fruit ID", "Grade Atas", "Grade Samping", "Grade Final", "Berat (gram)"]);
    }

    sheet.appendRow([
      new Date(),
      data.fruitId,
      data.gradeTop,
      data.gradeSide,
      data.gradeFinal,
      data.weight
    ]);

    return ContentService.createTextOutput(JSON.stringify({success: true}))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (err) {
    // REVISI: sebelumnya tidak ada penanganan ini sama sekali.
    return ContentService.createTextOutput(JSON.stringify({success: false, error: err.message}))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
