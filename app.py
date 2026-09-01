"""
CLOUD API KLASIFIKASI APEL GRADE A/B (REVISI: 3 KELAS + CONFIDENCE THRESHOLD)
======================================
Dipanggil oleh ESP32 MAIN (fungsi classifyWithCloudAPI di esp32_main.ino).
Terima foto (multipart/form-data, field "file").

REVISI dari versi sebelumnya:
- Model sekarang 3 kelas: A, B, BUKAN_APEL (lihat train_apple_grade_model.py).
- Ditambah CONFIDENCE_THRESHOLD: walau kelas terprediksi A/B, kalau model
  sendiri tidak cukup yakin (confidence rendah), tetap dianggap TIDAK VALID.
  Ini mencegah kasus "kebetulan nebak A padahal cuma 51% yakin".
- Format respons berubah, sekarang SELALU ada field "status":
    "status": "ok"      -> grade valid, boleh dipakai (grade berisi "A"/"B")
    "status": "unsure"  -> BUKAN grade sah (bisa karena kelas BUKAN_APEL,
                            atau karena confidence < threshold). grade akan
                            berisi null. ESP32 WAJIB treat ini sama seperti
                            gagal klasifikasi (masuk jalur belakang / box C),
                            BUKAN pernah dipakai sebagai grade C.
    "status": "error"   -> error internal (file rusak, dll)
"""

from fastapi import FastAPI, File, UploadFile
from fastapi.responses import JSONResponse
import tensorflow as tf
import numpy as np
from PIL import Image
import io

app = FastAPI()

# Model di-load sekali saat server nyala, dipakai berulang-ulang tiap request.
model = tf.keras.models.load_model("apple_grade_model.keras")

IMG_SIZE = 64  # HARUS SAMA dengan IMG_SIZE saat training di Colab

# URUTAN INI HARUS SAMA PERSIS dengan CLASS_NAMES saat training
# (train_apple_grade_model.py CELL 2) - kalau beda urutan, index hasil
# prediksi akan salah dipetakan ke nama kelas yang salah.
CLASS_NAMES = ["A", "B", "BUKAN_APEL"]

# Ambang minimum keyakinan model (0.0 - 1.0) supaya grade A/B dianggap
# valid dan boleh dipakai sistem. Kalau confidence di bawah ini, walau
# kelas mentahnya A/B, tetap dibalas sebagai "unsure".
#
# Cara nentuin angka ini: lihat print "Akurasi per kelas" di CELL 7
# training. Kalau model masih sering salah, naikkan threshold (lebih
# ketat, lebih banyak "unsure" tapi lebih jarang salah grade). Kalau
# model sudah sangat akurat tapi kebanyakan buah malah dianggap
# "unsure", boleh turunkan sedikit. 0.75 adalah titik awal yang wajar.
CONFIDENCE_THRESHOLD = 0.75


@app.get("/")
def root():
    return {"status": "online", "message": "SIAM Bali Grade API siap"}


@app.post("/predict")
async def predict(file: UploadFile = File(...)):
    try:
        image_bytes = await file.read()
        img = Image.open(io.BytesIO(image_bytes)).convert("RGB")
        img = img.resize((IMG_SIZE, IMG_SIZE))

        img_array = np.array(img, dtype=np.float32)
        img_array = np.expand_dims(img_array, axis=0)  # jadi batch of 1

        prediction = model.predict(img_array, verbose=0)
        class_index = int(np.argmax(prediction[0]))
        confidence = float(prediction[0][class_index])
        predicted_class = CLASS_NAMES[class_index]

        is_confident_enough = confidence >= CONFIDENCE_THRESHOLD
        is_actual_apple = predicted_class in ("A", "B")

        if is_actual_apple and is_confident_enough:
            return JSONResponse({
                "status": "ok",
                "grade": predicted_class,
                "confidence": round(confidence, 4)
            })

        # Kelas BUKAN_APEL, ATAU confidence di bawah threshold -> JANGAN
        # PERNAH dianggap grade sah, walau kelas mentahnya kebetulan A/B.
        return JSONResponse({
            "status": "unsure",
            "grade": None,
            "confidence": round(confidence, 4),
            "raw_class": predicted_class  # buat debugging/logging, bukan dipakai ESP32
        })

    except Exception as e:
        return JSONResponse({"status": "error", "error": str(e)}, status_code=500)
