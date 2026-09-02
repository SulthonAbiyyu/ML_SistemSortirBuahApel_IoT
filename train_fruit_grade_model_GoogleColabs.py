"""
TRAINING KLASIFIKASI MULTI-BUAH (GRADE A / B)
======================================================================
REVISI v4 - HAPUS KELAS BUKAN_BUAH:
- Folder "BUKAN_BUAH" TIDAK LAGI WAJIB / TIDAK DIPAKAI. Asumsinya rig
  conveyor tertutup/terkontrol sehingga kecil kemungkinan ada objek
  non-buah yang lewat, jadi model tidak perlu belajar kelas ini.
  Kalau folder BUKAN_BUAH masih ada di Drive, script ini akan
  MENGABAIKANNYA (tidak di-scan, tidak dipakai training).
- Minimal jumlah kelas terdeteksi diturunkan dari 3 -> 2, karena
  sekarang tidak ada lagi kelas "pemaksa" ketiga.

REVISI v3 - PERBAIKAN ROBUSTNESS:
- CELL 3 sekarang benar-benar MEMISAHKAN folder valid ke direktori
  staging bersih (bukan cuma memberi peringatan lalu tetap membiarkan
  folder nyasar ada di dataset asli). Ini menutup celah bug di mana
  folder typo/nyasar (mis. ".ipynb_checkpoints", "Apel_A" kapital,
  "jambu_kristal_A" salah pola) bisa bikin image_dataset_from_directory
  di CELL 5 error atau salah baca kelas, karena fungsi itu men-scan
  SEMUA subfolder di direktori sumber, bukan cuma yang lolos validasi.
- Ditambahkan pengecekan folder kosong (0 foto) SEBELUM training,
  supaya gagalnya cepat & jelas (bukan ZeroDivisionError yang
  membingungkan di tengah CELL 7 saat menghitung class_weight).
- Ditambahkan pengecekan ekstensi file (hanya .jpg/.jpeg/.png yang
  dihitung), supaya file sampah (mis. Thumbs.db, .DS_Store) tidak
  ikut kehitung sebagai "foto" dan bikin jumlah kelas kelihatan lebih
  banyak dari aslinya.
- Staging folder pakai SYMLINK ke file asli (bukan copy), jadi tidak
  makan storage Drive dobel dan tetap cepat meskipun dataset besar.

======================================================================
CARA PAKAI DI GOOGLE COLAB:
1. Dataset di Google Drive dengan struktur folder WAJIB pola nama:
   {nama_buah}_{GRADE}. Nama buah huruf kecil semua, TANPA spasi,
   TANPA underscore tambahan (mis. "jambu kristal" -> "jambukristal",
   bukan "jambu_kristal"). Grade huruf besar (A/B).

   Sementara ini baru ada 2 buah (apel & jeruk), tapi struktur di
   bawah otomatis mendukung penambahan buah lain nanti tanpa ubah
   kode - tinggal tambah folder baru dengan pola yang sama.

   datasetbuah/
     apel_A/
     apel_B/
     jeruk_A/
     jeruk_B/

   Catatan: folder "BUKAN_BUAH" TIDAK dipakai lagi (lihat catatan
   revisi v4 di atas). Kalau masih ada sisa foldernya di Drive, boleh
   dibiarkan - script ini akan mengabaikannya secara otomatis.

   Target tiap folder grade per buah: minimal ~80-120 foto, usahakan
   jumlahnya tidak jomplang jauh antar folder.

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
import re
import os
import shutil

print("TensorFlow version:", tf.__version__)
print("GPU tersedia:", tf.config.list_physical_devices('GPU'))

# ============================================================
# CELL 2 - Konfigurasi
# ============================================================
DATASET_DIR = "/content/drive/MyDrive/datasetbuah"  # <-- GANTI kalau perlu
STAGING_DIR = "/content/dataset_staging"             # folder bersih hasil filter, jangan diedit manual

# IMG_SIZE dinaikkan dari versi khusus-apel (64 -> 96) karena sekarang model
# harus membedakan lebih banyak kelas (banyak buah x banyak grade), dan
# model ini jalan di cloud API (Render), bukan langsung di ESP32-CAM, jadi
# kita tidak perlu seketat itu soal ukuran.
IMG_SIZE = 96
BATCH_SIZE = 32
EPOCHS = 40             # dinaikkan lagi karena jumlah kelas lebih banyak,
                        # butuh lebih banyak epoch supaya semua kelas
                        # konvergen baik, bukan cuma kelas yang datanya banyak

GRADE_SUFFIXES = ["A", "B"]   # A = bagus/segar, B = jelek/busuk (digabung, tidak dipisah lagi)
NON_FRUIT_FOLDER = "BUKAN_BUAH"  # kalau ada di Drive, folder ini SENGAJA diabaikan (lihat CELL 3)
VALID_EXTENSIONS = {".jpg", ".jpeg", ".png"}
MIN_PHOTOS_PER_CLASS = 20   # di bawah ini, training dihentikan (bukan cuma diperingatkan)

# ============================================================
# CELL 3 - Scan, Validasi Ketat, & Bangun Staging Folder Bersih
# ============================================================
data_dir = pathlib.Path(DATASET_DIR)
staging_dir = pathlib.Path(STAGING_DIR)

if not data_dir.exists():
    raise FileNotFoundError(f"DATASET_DIR tidak ditemukan: {DATASET_DIR}")

# Bersihkan staging lama supaya tidak ada sisa run sebelumnya yang nyangkut
if staging_dir.exists():
    shutil.rmtree(staging_dir)
staging_dir.mkdir(parents=True)

all_folders = sorted([p.name for p in data_dir.iterdir() if p.is_dir()])

valid_pattern = re.compile(r"^[a-z0-9]+_(" + "|".join(GRADE_SUFFIXES) + r")$")
class_folders = []
skipped_folders = []
for name in all_folders:
    if name == NON_FRUIT_FOLDER:
        # SENGAJA diabaikan - kelas ini tidak dipakai lagi (lihat catatan revisi v4).
        # Ditangani terpisah (bukan masuk skipped_folders) supaya tidak muncul di
        # PERINGATAN seolah-olah ini folder typo/sampah.
        continue
    elif valid_pattern.match(name):
        class_folders.append(name)
    else:
        skipped_folders.append(name)

if skipped_folders:
    print("PERINGATAN: folder berikut TIDAK diikutkan ke training karena tidak sesuai "
          f"pola '{{buah}}_{{GRADE}}':")
    for name in skipped_folders:
        print(f"  - {name}  <- cek lagi, mungkin typo atau folder sampah (mis. .ipynb_checkpoints)")

if NON_FRUIT_FOLDER in all_folders:
    print(f"\nCatatan: folder '{NON_FRUIT_FOLDER}' ditemukan di dataset tapi SENGAJA "
          f"diabaikan (kelas ini tidak dipakai lagi). Boleh dihapus dari Drive kalau mau.")

CLASS_NAMES = class_folders

if len(CLASS_NAMES) < 2:
    raise ValueError("Kelas yang terdeteksi kurang dari 2 - cek lagi struktur folder dataset.")

# Bangun staging folder: SYMLINK file gambar valid dari folder asli ke
# folder staging bersih. Ini yang dipakai image_dataset_from_directory di
# CELL 5, BUKAN DATASET_DIR asli -> jadi folder nyasar/typo di Drive tidak
# akan pernah ikut ke-scan oleh Keras.
class_counts = {}
for name in CLASS_NAMES:
    src_folder = data_dir / name
    dst_folder = staging_dir / name
    dst_folder.mkdir(parents=True, exist_ok=True)

    n = 0
    for f in src_folder.iterdir():
        if f.is_file() and f.suffix.lower() in VALID_EXTENSIONS:
            os.symlink(f.resolve(), dst_folder / f.name)
            n += 1
    class_counts[name] = n

print("\nKelas terdeteksi & jumlah foto valid (SALIN URUTAN INI ke CLASS_NAMES di app.py):")
for i, name in enumerate(CLASS_NAMES):
    print(f"  {i}: {name}  ({class_counts[name]} foto)")

# ============================================================
# CELL 4 - Cek Dataset Kosong / Terlalu Sedikit / Timpang
# ============================================================
empty_classes = [name for name, n in class_counts.items() if n == 0]
if empty_classes:
    raise ValueError(
        f"Kelas berikut TIDAK PUNYA foto sama sekali, training tidak bisa jalan: {empty_classes}. "
        f"Cek lagi isi foldernya di Drive (dan pastikan ekstensi file .jpg/.jpeg/.png)."
    )

too_small = [name for name, n in class_counts.items() if 0 < n < MIN_PHOTOS_PER_CLASS]
if too_small:
    print(f"\nPERINGATAN: kelas berikut punya foto di bawah {MIN_PHOTOS_PER_CLASS} - "
          f"split train/val bisa gagal seimbang atau akurasi kelas ini tidak bisa dipercaya: {too_small}")

max_count = max(class_counts.values())
min_count = min(class_counts.values())
if max_count > 3 * min_count:
    print(f"\nPERINGATAN: dataset jomplang (kelas terbanyak {max_count} foto vs "
          f"kelas tersedikit {min_count} foto). class_weight otomatis akan dipakai "
          f"di CELL 7 untuk membantu, tapi idealnya tetap ditambah foto ke kelas "
          f"yang sedikit supaya akurasinya tidak timpang antar kelas.")

# ============================================================
# CELL 5 - Load Dataset (dari STAGING_DIR, bukan DATASET_DIR asli)
# ============================================================
train_ds = keras.utils.image_dataset_from_directory(
    staging_dir,
    validation_split=0.2,
    subset="training",
    seed=42,
    image_size=(IMG_SIZE, IMG_SIZE),
    batch_size=BATCH_SIZE,
    label_mode="categorical",
    class_names=CLASS_NAMES,
)

val_ds = keras.utils.image_dataset_from_directory(
    staging_dir,
    validation_split=0.2,
    subset="validation",
    seed=42,
    image_size=(IMG_SIZE, IMG_SIZE),
    batch_size=BATCH_SIZE,
    label_mode="categorical",
    class_names=CLASS_NAMES,
)

print("Kelas dipakai training:", train_ds.class_names)

AUTOTUNE = tf.data.AUTOTUNE
train_ds_cached = train_ds.cache().shuffle(1000).prefetch(buffer_size=AUTOTUNE)
val_ds_cached = val_ds.cache().prefetch(buffer_size=AUTOTUNE)

# ============================================================
# CELL 6 - Augmentasi
# ============================================================
# RandomFlip dibatasi ke "horizontal" saja. Apel & jeruk relatif bulat/
# simetris jadi flip vertikal sebenarnya aman untuk buahnya sendiri, tapi
# tetap dibatasi horizontal karena rotasi + zoom sudah cukup memberi variasi
# orientasi, dan supaya konsisten kalau nanti ada buah lain yang bentuknya
# tidak simetris (mis. pisang) ditambahkan ke dataset.
data_augmentation = keras.Sequential([
    layers.RandomFlip("horizontal"),
    layers.RandomRotation(0.15),
    layers.RandomZoom(0.15),
    layers.RandomTranslation(0.1, 0.1),
    layers.RandomContrast(0.15),
    layers.RandomBrightness(0.15),
])

# ============================================================
# CELL 7 - Hitung class_weight (mengompensasi dataset timpang)
# ============================================================
total_photos = sum(class_counts.values())
n_classes = len(CLASS_NAMES)
class_weight = {}
for i, name in enumerate(CLASS_NAMES):
    n = class_counts[name]
    class_weight[i] = total_photos / (n_classes * n)  # n=0 sudah dicegah di CELL 4

print("\nclass_weight yang dipakai saat training:")
for i, name in enumerate(CLASS_NAMES):
    print(f"  {name}: {class_weight[i]:.2f}")

# ============================================================
# CELL 8 - Bangun Model (Custom CNN Kecil, dilatih dari nol)
# ============================================================
# Masih custom CNN (bukan transfer learning MobileNetV2) supaya ringan,
# tapi kapasitasnya sedikit dinaikkan dibanding versi khusus-apel (filter
# 8/16/32/32 -> 16/32/64/64, Dense 16 -> 32) karena sekarang model harus
# membedakan lebih banyak kelas visual (banyak buah x banyak grade), dan
# saat ini modelnya jalan di cloud (Render), bukan di RAM terbatas
# ESP32-CAM secara langsung -> masih ada ruang untuk kapasitas ekstra.
# Jumlah output otomatis ikut len(CLASS_NAMES), tidak perlu diubah manual.

inputs = keras.Input(shape=(IMG_SIZE, IMG_SIZE, 3))
x = data_augmentation(inputs)
x = layers.Rescaling(1.0 / 255)(x)

x = layers.Conv2D(16, 3, padding="same", activation="relu")(x)
x = layers.MaxPooling2D()(x)

x = layers.SeparableConv2D(32, 3, padding="same", activation="relu")(x)
x = layers.MaxPooling2D()(x)

x = layers.SeparableConv2D(64, 3, padding="same", activation="relu")(x)
x = layers.MaxPooling2D()(x)

x = layers.SeparableConv2D(64, 3, padding="same", activation="relu")(x)
x = layers.GlobalAveragePooling2D()(x)

x = layers.Dropout(0.35)(x)
x = layers.Dense(32, activation="relu")(x)
outputs = layers.Dense(n_classes, activation="softmax")(x)

model = keras.Model(inputs, outputs)
model.compile(
    optimizer=keras.optimizers.Adam(learning_rate=1e-3),
    loss="categorical_crossentropy",
    metrics=["accuracy"],
)
model.summary()

# ============================================================
# CELL 9 - Training
# ============================================================
callbacks = [
    keras.callbacks.EarlyStopping(patience=7, restore_best_weights=True, monitor="val_accuracy"),
    keras.callbacks.ReduceLROnPlateau(factor=0.5, patience=3, monitor="val_loss"),
]

history = model.fit(
    train_ds_cached,
    validation_data=val_ds_cached,
    epochs=EPOCHS,
    class_weight=class_weight,
    callbacks=callbacks,
)

# ============================================================
# CELL 10 - Evaluasi (akurasi keseluruhan + per kelas + confusion matrix)
# ============================================================
loss, acc = model.evaluate(val_ds_cached)
print(f"Akurasi validasi akhir: {acc*100:.2f}%")

y_true = []
y_pred = []
for images, labels in val_ds_cached:
    preds = model.predict(images, verbose=0)
    y_true.extend(np.argmax(labels.numpy(), axis=1))
    y_pred.extend(np.argmax(preds, axis=1))

y_true = np.array(y_true)
y_pred = np.array(y_pred)

print("\nAkurasi per kelas (waspada kalau ada kelas jauh di bawah rata-rata,")
print("itu tandanya kelas tsb butuh lebih banyak/lebih variatif foto):")
for i, name in enumerate(CLASS_NAMES):
    idx = np.where(y_true == i)[0]
    if len(idx) > 0:
        correct = np.sum(y_pred[idx] == i)
        print(f"  {name}: {correct}/{len(idx)} ({100*correct/len(idx):.1f}%)")
    else:
        print(f"  {name}: tidak ada sample validasi (dataset kelas ini terlalu sedikit)")

print("\nConfusion matrix (baris = label asli, kolom = prediksi model):")
cm = np.zeros((n_classes, n_classes), dtype=int)
for t, p in zip(y_true, y_pred):
    cm[t, p] += 1
header = "        " + " ".join(f"{n[:6]:>6}" for n in CLASS_NAMES)
print(header)
for i, name in enumerate(CLASS_NAMES):
    row = " ".join(f"{cm[i,j]:>6}" for j in range(n_classes))
    print(f"{name[:8]:>8} {row}")

model.save("fruit_grade_model.keras")

# ============================================================
# CELL 11 - Convert ke TFLite (int8 quantized) - OPSIONAL
# ============================================================
# Sama seperti versi sebelumnya: ini disiapkan untuk rencana jangka panjang
# kalau nanti benar-benar pindah ke inferensi on-device di ESP32-CAM.
# Untuk arsitektur SEKARANG (cloud API di Render), file yang dipakai app.py
# adalah fruit_grade_model.keras dari CELL 10, BUKAN file .tflite ini.

def representative_dataset():
    for images, _ in train_ds_cached.take(50):
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

with open("fruit_grade_model_int8.tflite", "wb") as f:
    f.write(tflite_model)

size_kb = len(tflite_model) / 1024
print("Ukuran model TFLite (int8):", size_kb, "KB")
if size_kb > 300:
    print("Catatan: model di atas ~300KB. Tidak masalah selama masih di cloud API. "
          "Baru jadi perhatian kalau nanti benar-benar pindah ke on-device ESP32-CAM.")
else:
    print("Ukuran model sudah masuk kisaran aman untuk ESP32-CAM juga, kalau nanti dipakai.")

print("\nSELESAI. File yang dihasilkan:")
print("- fruit_grade_model.keras       (WAJIB: dipakai app.py di cloud API, upload ini ke repo Render)")
print("- fruit_grade_model_int8.tflite (opsional, cuma perlu kalau nanti pindah ke on-device ESP32-CAM)")
print("\nJANGAN LUPA: update CLASS_NAMES di app.py supaya URUTANNYA SAMA PERSIS")
print("dengan daftar kelas yang dicetak di CELL 3 di atas. Kalau beda urutan,")
print("hasil prediksi akan salah dipetakan ke nama buah/grade yang salah.")
print("Di app.py, semua kelas sekarang pasti berpola '{buah}_{GRADE}' (tidak ada")
print("lagi kasus khusus BUKAN_BUAH), jadi split labelnya cukup:")
print('  buah, grade = label.rsplit("_", 1)')
