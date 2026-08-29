"""
CLOUD API KLASIFIKASI APEL GRADE A/B
======================================
Dipanggil oleh ESP32 MAIN (fungsi classifyWithCloudAPI di esp32_main.ino).
Terima foto (multipart/form-data, field "file"), balas JSON {"grade": "A"} atau {"grade": "B"}.
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
CLASS_NAMES = ["A", "B"]  # urutan HARUS SAMA seperti saat training


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
        grade = CLASS_NAMES[class_index]

        return JSONResponse({
            "grade": grade,
            "confidence": round(confidence, 4)
        })

    except Exception as e:
        return JSONResponse({"error": str(e)}, status_code=500)
