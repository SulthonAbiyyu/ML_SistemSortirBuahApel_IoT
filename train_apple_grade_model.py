"""
TRAINING KLASIFIKASI APEL GRADE A / B / BUKAN_APEL (VERSI RINGAN UNTUK ESP32-CAM)
======================================================================
REVISI: sekarang 3 KELAS, bukan 2. Kelas ke-3 "BUKAN_APEL" ditambahkan
supaya model punya cara EKSPLISIT untuk bilang "ini bukan apel", bukan
dipaksa selalu memilih A atau B walau yang lewat kamera cuma background
kosong / tangan / meja. Ini yang memperbaiki bug "grade A palsu saat
tidak ada apel sama sekali".

Model custom CNN kecil (bukan MobileNetV2) supaya muat di RAM ESP32-CAM
(~320KB). Target ukuran akhir: di bawah ~300KB setelah quantize int8.

CARA PAKAI DI GOOGLE COLAB:
1. Dataset di Google Drive dengan struktur folder:

   datasheet_apel/
     A/            (apel bagus)
       img001.jpg
       ...
     B/            (apel jelek)
       ...
     BUKAN_APEL/   (BARU: foto conveyor/meja kosong, tangan, background,
                     apapun yang BUKAN apel - lihat catatan di bawah)
       img001.jpg
       ...

   >>> PENTING soal folder BUKAN_APEL <<<
   Ambil foto ini LANGSUNG dari kedua ESP32-CAM kamu (ATAS & SAMPING),
   bukan dari internet/dataset publik - supaya sudut, jarak, dan
   pencahayaannya sama persis dengan kondisi asli di conveyor kamu.
   Variasikan kondisinya: conveyor benar-benar kosong, tangan operator
   lewat, benda lain (bukan apel) taruh di dudukan, dll. Target minimal
   ~100-150 foto per sisi kamera (jadi ~200-300 total) supaya seimbang
   dengan jumlah foto A dan B.

2. Runtime -> Change runtime type -> GPU (T4)
3. Jalankan cell demi cell dari atas ke bawah.
"""

# ============================================================
# CELL 1 - Install & Import
# ============================================================
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers
import numpy as np
import pathlib

print("TensorFlow version:", tf.__version__)
print("GPU tersedia:", tf.config.list_physical_devices('GPU'))

# ============================================================
# CELL 2 - Konfigurasi
# ============================================================
DATASET_DIR = "/content/drive/MyDrive/datasheet_apel"  # <-- GANTI kalau perlu

# Gambar dibuat kecil karena target ESP32-CAM (RAM & flash terbatas)
IMG_SIZE = 64          # lebih kecil dari sebelumnya (96) -> model lebih ringan
BATCH_SIZE = 32
EPOCHS = 30             # dinaikkan sedikit dari 25 -> 3 kelas butuh sedikit lebih
                        # banyak epoch supaya kelas BUKAN_APEL ikut konvergen baik
# REVISI: tambah kelas ke-3 "BUKAN_APEL". URUTAN INI HARUS SAMA PERSIS
# dengan CLASS_NAMES di app.py (cloud API) - kalau urutan beda, index
# hasil prediksi akan salah dipetakan ke nama kelas yang salah!
CLASS_NAMES = ["A", "B", "BUKAN_APEL"]

# ============================================================
# CELL 3 - Load Dataset
# ============================================================
data_dir = pathlib.Path(DATASET_DIR)

train_ds = keras.utils.image_dataset_from_directory(
    data_dir,
    validation_split=0.2,
    subset="training",
    seed=42,
    image_size=(IMG_SIZE, IMG_SIZE),
    batch_size=BATCH_SIZE,
    label_mode="categorical",
    class_names=CLASS_NAMES,
)

val_ds = keras.utils.image_dataset_from_directory(
    data_dir,
    validation_split=0.2,
    subset="validation",
    seed=42,
    image_size=(IMG_SIZE, IMG_SIZE),
    batch_size=BATCH_SIZE,
    label_mode="categorical",
    class_names=CLASS_NAMES,
)

print("Kelas:", train_ds.class_names)

# Cek cepat jumlah foto per kelas biar ketahuan dari awal kalau BUKAN_APEL
# terlalu sedikit dibanding A/B (dataset timpang bikin model bias).
for class_name in CLASS_NAMES:
    n = len(list((data_dir / class_name).glob("*")))
    print(f"  {class_name}: {n} foto")

AUTOTUNE = tf.data.AUTOTUNE
train_ds = train_ds.cache().shuffle(1000).prefetch(buffer_size=AUTOTUNE)
val_ds = val_ds.cache().prefetch(buffer_size=AUTOTUNE)

# ============================================================
# CELL 4 - Augmentasi
# ============================================================
data_augmentation = keras.Sequential([
    layers.RandomFlip("horizontal_and_vertical"),
    layers.RandomRotation(0.15),
    layers.RandomZoom(0.15),
    layers.RandomContrast(0.15),
    layers.RandomBrightness(0.15),
])

# ============================================================
# CELL 5 - Bangun Model (Custom CNN Kecil, dilatih dari nol)
# ============================================================
# Tidak pakai transfer learning (MobileNetV2 terlalu besar untuk ESP32-CAM).
# Pakai depthwise separable conv (sama seperti prinsip MobileNet) tapi
# jumlah layer & filter jauh lebih sedikit -> hasil model jauh lebih kecil.
# Jumlah output otomatis ikut len(CLASS_NAMES) -> otomatis jadi 3 sekarang,
# tidak perlu diubah manual.

inputs = keras.Input(shape=(IMG_SIZE, IMG_SIZE, 3))
x = data_augmentation(inputs)
x = layers.Rescaling(1.0 / 255)(x)

x = layers.Conv2D(8, 3, padding="same", activation="relu")(x)
x = layers.MaxPooling2D()(x)

x = layers.SeparableConv2D(16, 3, padding="same", activation="relu")(x)
x = layers.MaxPooling2D()(x)

x = layers.SeparableConv2D(32, 3, padding="same", activation="relu")(x)
x = layers.MaxPooling2D()(x)

x = layers.SeparableConv2D(32, 3, padding="same", activation="relu")(x)
x = layers.GlobalAveragePooling2D()(x)

x = layers.Dropout(0.3)(x)
x = layers.Dense(16, activation="relu")(x)
outputs = layers.Dense(len(CLASS_NAMES), activation="softmax")(x)

model = keras.Model(inputs, outputs)
model.compile(
    optimizer=keras.optimizers.Adam(learning_rate=1e-3),
    loss="categorical_crossentropy",
    metrics=["accuracy"],
)
model.summary()

# ============================================================
# CELL 6 - Training
# ============================================================
callbacks = [
    keras.callbacks.EarlyStopping(patience=6, restore_best_weights=True),
    keras.callbacks.ReduceLROnPlateau(factor=0.5, patience=3),
]

history = model.fit(
    train_ds,
    validation_data=val_ds,
    epochs=EPOCHS,
    callbacks=callbacks,
)

# ============================================================
# CELL 7 - Evaluasi
# ============================================================
loss, acc = model.evaluate(val_ds)
print(f"Akurasi validasi akhir: {acc*100:.2f}%")

# Cek juga akurasi PER KELAS, bukan cuma rata-rata keseluruhan - supaya
# ketahuan kalau kelas BUKAN_APEL secara khusus masih sering salah,
# meskipun akurasi keseluruhan kelihatan tinggi (bisa ketutup oleh
# akurasi A/B yang tinggi).
y_true = []
y_pred = []
for images, labels in val_ds:
    preds = model.predict(images, verbose=0)
    y_true.extend(np.argmax(labels.numpy(), axis=1))
    y_pred.extend(np.argmax(preds, axis=1))

from collections import Counter
print("\nAkurasi per kelas:")
for i, name in enumerate(CLASS_NAMES):
    idx = [j for j, t in enumerate(y_true) if t == i]
    if idx:
        correct = sum(1 for j in idx if y_pred[j] == i)
        print(f"  {name}: {correct}/{len(idx)} ({100*correct/len(idx):.1f}%)")

model.save("apple_grade_model.keras")

# ============================================================
# CELL 8 - Convert ke TFLite (int8 quantized) untuk ESP32-CAM
# ============================================================
# Catatan: konversi TFLite ini disiapkan untuk rencana jangka panjang
# (kalau nanti benar-benar pindah ke inferensi on-device di ESP32-CAM).
# Untuk arsitektur SEKARANG (cloud API di Render), file yang dipakai
# app.py adalah apple_grade_model.keras dari CELL 7, BUKAN file .tflite
# ini - jadi cell ini boleh dilewati kalau kamu tidak butuh on-device.

def representative_dataset():
    for images, _ in train_ds.take(50):
        for img in images:
            img = tf.expand_dims(img, axis=0)
            yield [tf.cast(img, tf.float32)]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_dataset
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.int8

tflite_model = converter.convert()

with open("apple_grade_model_int8.tflite", "wb") as f:
    f.write(tflite_model)

size_kb = len(tflite_model) / 1024
print("Ukuran model TFLite (int8):", size_kb, "KB")
if size_kb > 300:
    print("PERINGATAN: model masih di atas ~300KB, mungkin masih berat untuk ESP32-CAM tanpa PSRAM.")
else:
    print("Ukuran model sudah masuk kisaran aman untuk ESP32-CAM.")

# ============================================================
# CELL 9 - Convert .tflite ke C array (untuk di-flash ke ESP32-CAM, opsional)
# ============================================================
# Jalankan di terminal Colab (HANYA kalau kamu pakai jalur on-device, lihat
# catatan di CELL 8):
# !xxd -i apple_grade_model_int8.tflite > apple_grade_model.h

print("\nSELESAI. File yang dihasilkan:")
print("- apple_grade_model.keras       (WAJIB: dipakai app.py di cloud API, upload ini ke repo Render)")
print("- apple_grade_model_int8.tflite (opsional, cuma perlu kalau nanti pindah ke on-device ESP32-CAM)")
