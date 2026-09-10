"""
CLOUD API KLASIFIKASI MULTI-BUAH GRADE A/B (REVISI v3: SELARAS DENGAN TRAINING v4)
====================================================================================
Dipanggil oleh ESP32 MAIN (fungsi classifyWithCloudAPI di esp32_main.ino).
Terima foto (multipart/form-data, field "file").

REVISI dari versi sebelumnya (v2, multi-buah + BUKAN_BUAH):
- Kelas "BUKAN_BUAH" DIHAPUS TOTAL, mengikuti training script v4
  (train_fruit_grade_model.py) yang sudah tidak melatih kelas ini lagi
  (asumsi rig conveyor tertutup/terkontrol, kecil kemungkinan ada objek
  non-buah lewat). CLASS_NAMES di bawah sekarang HANYA berisi kelas
  berpola "{buah}_{GRADE}", tidak ada lagi kelas pemaksa ketiga.
- Field response "status": "unsure" sekarang HANYA bisa terjadi karena
  confidence di bawah CONFIDENCE_THRESHOLD (bukan lagi karena kelas
  BUKAN_BUAH, yang sudah tidak ada).
- Ditambah field "kualitas" ("Bagus"/"Jelek") di response, turunan
  langsung dari grade (A=Bagus, B=Jelek), lewat SATU mapping terpusat
  (GRADE_LABELS) supaya konsumen API (ESP32, Apps Script logging) tidak
  perlu menghardcode arti A/B masing-masing.
- Ditambah validasi input (tipe & ukuran file), endpoint /health, dan
  logging terstruktur -- supaya server lebih mudah didiagnosis kalau ada
  masalah di lapangan (skala industrial, bukan cuma demo).
- Server sengaja GAGAL START (bukan nyala setengah-setengah) kalau model
  gagal di-load atau jumlah output model tidak cocok dengan CLASS_NAMES --
  lebih baik ketahuan di deploy log daripada 500 error membingungkan tiap
  ada request /predict masuk.

PENTING SOAL CLASS_NAMES DI BAWAH:
Urutan CLASS_NAMES di sini HARUS SAMA PERSIS dengan urutan yang dicetak
training script (train_fruit_grade_model.py) di CELL 3, bagian
"Kelas terdeteksi & jumlah foto valid". Urutan itu SELALU alfabetis
berdasarkan nama folder di Drive kamu (mis. apel_A, apel_B, jeruk_A,
jeruk_B; kalau nanti nambah buah baru mis. "mangga", dia otomatis masuk
sesuai abjad: apel_A, apel_B, jeruk_A, jeruk_B, mangga_A, mangga_B).
List di bawah CUMA CONTOH sesuai docstring training script (baru ada
apel & jeruk) -- ganti dengan list ASLI hasil print CELL 3 punya kamu
kalau dataset kamu sudah beda.
"""

import io
import logging

import numpy as np
import tensorflow as tf
from fastapi import FastAPI, File, HTTPException, UploadFile
from fastapi.responses import JSONResponse
from PIL import Image, UnidentifiedImageError

# ----------------------------------------------------------------------
# Logging - error di lapangan (foto korup, model gagal load, dll) harus
# kelihatan jelas di log Render, bukan cuma hilang jadi HTTP 500 kosong.
# ----------------------------------------------------------------------
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("siam-grade-api")

app = FastAPI(title="SIAM Bali Grade API", version="3.0.0")

# ----------------------------------------------------------------------
# Konfigurasi model
# ----------------------------------------------------------------------
MODEL_PATH = "fruit_grade_model.keras"
IMG_SIZE = 96  # HARUS SAMA dengan IMG_SIZE saat training (CELL 2 train_fruit_grade_model.py)

# GANTI list ini dengan urutan ASLI hasil print CELL 3 di Colab kamu.
CLASS_NAMES = [
    "apel_A",
    "apel_B",
    "jeruk_A",
    "jeruk_B",
]

# A = bagus/segar, B = jelek/busuk (definisi sama seperti training script).
# SATU-SATUNYA tempat mapping ini didefinisikan di sisi API.
GRADE_LABELS = {"A": "Bagus", "B": "Jelek"}

# Ambang minimum keyakinan model (0.0 - 1.0) supaya grade dianggap valid dan
# boleh dipakai sistem. Lihat print "Akurasi per kelas" di CELL 10 training
# untuk menentukan angka yang wajar. 0.75 adalah titik awal.
CONFIDENCE_THRESHOLD = 0.6

# Validasi upload dasar, supaya file sampah/kebesaran tidak bikin server hang.
ALLOWED_CONTENT_TYPES = {"image/jpeg", "image/jpg", "image/png"}
MAX_FILE_SIZE_BYTES = 8 * 1024 * 1024  # 8 MB

# ----------------------------------------------------------------------
# Load model sekali saat server nyala, dipakai berulang-ulang tiap request.
# ----------------------------------------------------------------------
try:
    model = tf.keras.models.load_model(MODEL_PATH)
    if model.output_shape[-1] != len(CLASS_NAMES):
        raise ValueError(
            f"Jumlah output model ({model.output_shape[-1]}) tidak cocok dengan "
            f"jumlah CLASS_NAMES ({len(CLASS_NAMES)}). Cek lagi CLASS_NAMES di app.py "
            f"apakah sudah sesuai urutan CELL 3 training script."
        )
    logger.info("Model berhasil di-load. Kelas: %s", CLASS_NAMES)
except Exception:
    logger.exception("GAGAL load model saat startup - server tidak akan bisa melayani /predict.")
    raise


@app.get("/")
def root():
    return {"status": "online", "message": "SIAM Bali Grade API siap"}


@app.get("/health")
def health():
    """Health check sederhana untuk load balancer / uptime monitor."""
    return {"status": "ok", "model_loaded": model is not None, "classes": CLASS_NAMES}


@app.post("/predict")
async def predict(file: UploadFile = File(...)):
    # --- Validasi input dasar ---
    if file.content_type not in ALLOWED_CONTENT_TYPES:
        raise HTTPException(status_code=400, detail=f"Tipe file tidak didukung: {file.content_type}")

    image_bytes = await file.read()
    if len(image_bytes) > MAX_FILE_SIZE_BYTES:
        raise HTTPException(status_code=400, detail="Ukuran file terlalu besar (maks 8MB).")

    try:
        img = Image.open(io.BytesIO(image_bytes)).convert("RGB")
    except UnidentifiedImageError:
        return JSONResponse({"status": "error", "error": "File bukan gambar yang valid"}, status_code=400)

    try:
        img = img.resize((IMG_SIZE, IMG_SIZE))
        img_array = np.array(img, dtype=np.float32)
        img_array = np.expand_dims(img_array, axis=0)  # jadi batch of 1

        prediction = model.predict(img_array, verbose=0)
        class_index = int(np.argmax(prediction[0]))
        confidence = float(prediction[0][class_index])
        predicted_label = CLASS_NAMES[class_index]

        # Semua kelas sekarang pasti berpola "{buah}_{GRADE}" (tidak ada lagi
        # kasus khusus BUKAN_BUAH), jadi split labelnya cukup rsplit sekali.
        buah, grade = predicted_label.rsplit("_", 1)
        kualitas = GRADE_LABELS.get(grade, "Tidak diketahui")

        if confidence >= CONFIDENCE_THRESHOLD:
            return JSONResponse({
                "status": "ok",
                "buah": buah,
                "grade": grade,
                "kualitas": kualitas,
                "confidence": round(confidence, 4),
            })

        # Confidence di bawah threshold -> JANGAN PERNAH dianggap grade sah,
        # walau kelas mentahnya kebetulan buah_grade yang valid.
        logger.info(
            "Prediksi unsure: %s (confidence=%.4f < threshold=%.2f)",
            predicted_label, confidence, CONFIDENCE_THRESHOLD,
        )
        return JSONResponse({
            "status": "unsure",
            "buah": None,
            "grade": None,
            "kualitas": None,
            "confidence": round(confidence, 4),
            "raw_class": predicted_label,  # buat debugging/logging, bukan dipakai ESP32
        })

    except Exception as e:
        logger.exception("Error saat memproses /predict")
        return JSONResponse({"status": "error", "error": str(e)}, status_code=500)
