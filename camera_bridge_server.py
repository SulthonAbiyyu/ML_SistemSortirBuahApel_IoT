import sys
import os
import time
import base64
import socket
import threading
from datetime import datetime

import cv2
import requests
from flask import Flask, Response, jsonify, request
from zeroconf import Zeroconf, ServiceInfo

try:
    from pygrabber.dshow_graph import FilterGraph
    HAS_PYGRABBER = True
except ImportError:
    HAS_PYGRABBER = False


# ============================================================
# FLASK
# ============================================================

app = Flask(__name__)

INTERNAL_PORT = 8080


# ============================================================
# CAMERA CONFIG
# ============================================================
# Sesuaikan dengan nama webcam yang muncul dari:
# python camera_bridge_server.py --list-cameras

CAMERA_NAME_TOP = "JETE-W7"
CAMERA_NAME_SIDE = "WEB CAMER"

# Fallback jika nama kamera tidak ditemukan.
CAMERA_INDEX_TOP_FALLBACK = 1
CAMERA_INDEX_SIDE_FALLBACK = 0


# ============================================================
# IMAGE CONFIG
# ============================================================

# Foto yang diambil /jpg untuk ESP32.
ML_WIDTH = 640
ML_HEIGHT = 480

# Target maksimum aman untuk ESP32.
MAX_ESP32_JPEG_BYTES = 80000

# Quality awal. Jika > 80 KB otomatis diturunkan.
ML_JPEG_QUALITY_START = 40
ML_JPEG_QUALITY_MIN = 20
ML_JPEG_QUALITY_STEP = 5

# Arsip Google Drive.
ARCHIVE_WIDTH = 1280
ARCHIVE_HEIGHT = 720
ARCHIVE_JPEG_QUALITY = 85


# ============================================================
# CLOUD ML API
# ============================================================
# Dipindah ke sini dari esp32_main.ino -- laptop yang sekarang langsung
# kirim foto ke cloud ML, ESP32 MAIN tidak lagi tahu URL ini sama sekali.

CLOUD_API_URL = "https://ml-sistemsortirbuahapel-iot.onrender.com/predict"


# ============================================================
# GOOGLE APPS SCRIPT
# ============================================================

# Isi dengan URL Web App Apps Script.
GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbxXaqN60uyMrsx000K40tFXMIs4NVsxpNYME9OlSjjoI-WDjyLVXw7W87ir9ZHUSyF7vA/exec"

# HARUS sama dengan key/SECRET_KEY di Code.gs.
GOOGLE_API_TOKEN = "kelompokPKM"

ENABLE_GOOGLE_UPLOAD = False

# Backup lokal.
LOCAL_ARCHIVE_DIR = "camera_archive"


# ============================================================
# CAPTURE PAIRING
# ============================================================
# ESP32 MAIN idealnya mengambil:
#   1. camtop.local/capture
#   2. camside.local/capture
#
# Karena kedua kamera harus masuk ke SATU baris Google Sheet,
# bridge otomatis memakai capture_id yang sama jika kamera kedua
# dipanggil dalam waktu singkat setelah kamera pertama.
#
# Jika kamera yang sama dipanggil lagi, dibuat ID baru.
#
# Untuk sistem yang sangat presisi, ESP32 dapat mengirim:
#   /capture?capture_id=123
# pada kedua kamera. Jika parameter itu tidak ada, pairing
# otomatis di bawah ini digunakan.

PAIR_WINDOW_SECONDS = 5.0

capture_state_lock = threading.Lock()
global_capture_id = 0
pending_capture_id = None
pending_unit = None
pending_time = 0.0


def get_capture_id_for_unit(unit_key, requested_capture_id=None):
    global global_capture_id
    global pending_capture_id
    global pending_unit
    global pending_time

    with capture_state_lock:
        # Jika ESP32 memberikan ID secara eksplisit, gunakan ID itu.
        if requested_capture_id:
            capture_id = str(requested_capture_id)

            # Tetap simpan sebagai pending agar kamera pasangan
            # yang tidak mengirim ID masih bisa dipasangkan.
            pending_capture_id = capture_id
            pending_unit = unit_key
            pending_time = time.time()

            return capture_id

        now = time.time()

        # Kamera berbeda dalam window -> pasangan foto yang sama.
        if (
            pending_capture_id is not None
            and pending_unit is not None
            and pending_unit != unit_key
            and (now - pending_time) <= PAIR_WINDOW_SECONDS
        ):
            capture_id = pending_capture_id

            # Setelah pasangan lengkap, kosongkan pending.
            pending_capture_id = None
            pending_unit = None
            pending_time = 0.0

            return capture_id

        # Tidak ada pasangan -> buat ID baru.
        global_capture_id += 1
        capture_id = str(global_capture_id)

        pending_capture_id = capture_id
        pending_unit = unit_key
        pending_time = now

        return capture_id


# ============================================================
# CAMERA DEVICE DISCOVERY
# ============================================================

def list_camera_devices():
    if not HAS_PYGRABBER:
        return []

    try:
        return FilterGraph().get_input_devices()
    except Exception as e:
        print(f"[KAMERA] Gagal membaca device via pygrabber: {e}")
        return []


def find_index_by_name(name_substring, device_names):
    needle = name_substring.strip().lower()

    matches = [
        i
        for i, name in enumerate(device_names)
        if needle in name.lower()
    ]

    if len(matches) == 1:
        return matches[0]

    return None


def resolve_single_camera_index(expected_name, fallback_index, device_names=None):
    """
    Cari index kamera untuk `expected_name`.

    PENTING (revisi): sebelumnya fungsi ini query pygrabber ULANG di
    setiap percobaan buka kamera. Itu ternyata jadi sumber bug: begitu
    kamera pertama (mis. TOP) sudah dibuka via cv2 CAP_DSHOW, device
    fisik itu jadi "terkunci" dan kadang MENGHILANG dari enumerasi
    pygrabber berikutnya -- akibatnya seluruh index sesudahnya ikut
    bergeser turun 1, dan kamera kedua (SIDE) jadi "cocok nama" di
    index yang salah, padahal index cv2 itu sebenarnya masih menunjuk
    ke device fisik yang SAMA dengan kamera pertama. Hasilnya: TOP dan
    SIDE diam-diam membuka device fisik yang sama (seolah cuma ada 1
    webcam aktif), walau kedua webcam lain sebenarnya menyala.

    Fix: nama di-resolve dari SATU snapshot device_names yang diambil
    SEKALI di awal (sebelum kamera manapun dibuka), dipakai bersama
    oleh TOP maupun SIDE. Snapshot ini tidak ikut bergeser karena
    diambil sebelum ada device yang terkunci oleh proses ini sendiri.
    Kalau caller tidak mengirim snapshot (device_names=None), baru
    fungsi ini query pygrabber langsung -- dipertahankan sebagai
    fallback untuk skenario kamera virtual (mis. OBS) yang start/stop
    belakangan.
    """
    if device_names is None:
        device_names = list_camera_devices()

    if not device_names:
        return fallback_index, device_names

    idx = find_index_by_name(expected_name, device_names)

    if idx is None:
        return fallback_index, device_names

    return idx, device_names


# ============================================================
# IMAGE ENCODING
# ============================================================

def encode_ml_jpeg(frame):
    """
    Resize ke 640x480 dan otomatis menurunkan quality sampai
    ukuran <= 80 KB atau quality minimum tercapai.
    """

    ml_frame = cv2.resize(
        frame,
        (ML_WIDTH, ML_HEIGHT),
        interpolation=cv2.INTER_AREA
    )

    quality = ML_JPEG_QUALITY_START

    while quality >= ML_JPEG_QUALITY_MIN:
        ok, buf = cv2.imencode(
            ".jpg",
            ml_frame,
            [
                int(cv2.IMWRITE_JPEG_QUALITY),
                quality
            ]
        )

        if not ok:
            return None, None

        data = buf.tobytes()

        if len(data) <= MAX_ESP32_JPEG_BYTES:
            return data, quality

        quality -= ML_JPEG_QUALITY_STEP

    # Jika masih lebih besar, gunakan quality minimum.
    ok, buf = cv2.imencode(
        ".jpg",
        ml_frame,
        [
            int(cv2.IMWRITE_JPEG_QUALITY),
            ML_JPEG_QUALITY_MIN
        ]
    )

    if not ok:
        return None, None

    return buf.tobytes(), ML_JPEG_QUALITY_MIN


def encode_archive_jpeg(frame):
    """
    Arsip HD. Tidak dipaksa 80 KB karena file ini untuk Drive,
    bukan untuk ESP32.
    """

    archive_frame = cv2.resize(
        frame,
        (ARCHIVE_WIDTH, ARCHIVE_HEIGHT),
        interpolation=cv2.INTER_AREA
    )

    ok, buf = cv2.imencode(
        ".jpg",
        archive_frame,
        [
            int(cv2.IMWRITE_JPEG_QUALITY),
            ARCHIVE_JPEG_QUALITY
        ]
    )

    if not ok:
        return None

    return buf.tobytes()


# ============================================================
# CAMERA CLASS
# ============================================================

class CameraUnit:

    def __init__(self, unit_key, unit_label, expected_name, fallback_index,
                 startup_device_names=None):
        self.unit_key = unit_key
        self.unit_label = unit_label
        self.expected_name = expected_name
        self.fallback_index = fallback_index
        # Snapshot device_names yang diambil SEKALI sebelum kamera manapun
        # dibuka -- lihat catatan panjang di resolve_single_camera_index().
        self.startup_device_names = startup_device_names

        self.lock = threading.Lock()

        self.camera_index, self.cap = self._open_camera_with_retry()

        self.cap.set(
            cv2.CAP_PROP_FRAME_WIDTH,
            ARCHIVE_WIDTH
        )

        self.cap.set(
            cv2.CAP_PROP_FRAME_HEIGHT,
            ARCHIVE_HEIGHT
        )

        self.last_jpeg_bytes = None
        self.last_archive_bytes = None
        self.last_capture_id = None
        self.capture_count = 0

        self.actual_width = None
        self.actual_height = None

        self.refresh_actual_resolution()

    def _open_camera_with_retry(self, attempts=6, delay_seconds=1.5):
        # Kalau kamera lain baru saja dibuka (CAP_DSHOW), driver kadang
        # butuh jeda sebelum bisa buka device berikutnya -- percobaan
        # pertama bisa gagal (isOpened() False) walau device-nya sehat.
        # Makanya di sini kita coba ulang beberapa kali dengan jeda,
        # bukan langsung nyerah di percobaan pertama.
        #
        # Index kamera juga di-resolve ULANG di setiap percobaan (bukan
        # dipakai dari snapshot lama) supaya kalau daftar device sempat
        # bergeser (mis. OBS Virtual Camera baru start/stop), percobaan
        # berikutnya otomatis pakai index yang sudah diperbarui.
        #
        # PENTING: index dari pygrabber (dipakai untuk cari nama) dan
        # index dari cv2.VideoCapture(..., CAP_DSHOW) (dipakai untuk
        # benar-benar buka device) adalah DUA PENOMORAN YANG TERPISAH --
        # OpenCV sendiri tidak menjamin keduanya selalu sama urutan.
        # Jadi walau nama "cocok" menurut pygrabber, device yang BENAR-
        # BENAR terbuka lewat cv2 bisa saja device fisik yang lain.
        # Makanya verifikasi nama di bawah TIDAK cukup dipakai sendirian
        # -- ditambah verifikasi isi video lewat _capture_is_static().
        last_index = self.fallback_index
        last_cap = None

        for attempt in range(1, attempts + 1):
            index, device_names = resolve_single_camera_index(
                self.expected_name, self.fallback_index,
                self.startup_device_names
            )
            last_index = index

            cap = cv2.VideoCapture(index, cv2.CAP_DSHOW)

            if not cap.isOpened():
                print(
                    f"[KAMERA] {self.unit_label}: index {index} gagal dibuka "
                    f"(percobaan {attempt}/{attempts})."
                )
                cap.release()
                if attempt < attempts:
                    time.sleep(delay_seconds)
                continue

            reject_reason = None

            # Cek 1 (lemah, informatif saja): nama di index tsb menurut
            # pygrabber. Bisa saja "cocok" padahal device fisik yang
            # benar-benar kebuka di cv2 tetap berbeda -- lihat catatan
            # di atas -- jadi ini TIDAK dijadikan satu-satunya penentu.
            if device_names and index < len(device_names):
                actual_name = device_names[index]
                if self.expected_name.strip().lower() not in actual_name.lower():
                    reject_reason = (
                        f"nama di index {index} menurut pygrabber adalah "
                        f"'{actual_name}' (BUKAN '{self.expected_name}')"
                    )

            # Cek 2 (penentu utama): isi video harus "hidup", bukan
            # bitmap statis. OBS Virtual Camera dalam kondisi idle/tidak
            # ada scene aktif mengirim frame yang identik persis
            # berulang-ulang -- kamera fisik asli (secanggung apa pun
            # kondisinya) selalu punya noise sensor kecil antar-frame.
            if reject_reason is None and self._capture_is_static(cap):
                reject_reason = (
                    "2 frame berturut-turut identik persis (ciri sumber "
                    "video statis/placeholder seperti OBS Virtual Camera "
                    "saat idle, bukan kamera fisik yang benar-benar hidup)"
                )

            if reject_reason is not None:
                print(
                    f"[KAMERA] {self.unit_label}: index {index} dibuka, "
                    f"tapi ditolak -- {reject_reason}. Kemungkinan "
                    f"index cv2 & index pygrabber tidak sinkron akibat "
                    f"kamera virtual (mis. OBS). Diulang "
                    f"(percobaan {attempt}/{attempts})."
                )
                cap.release()
                if attempt < attempts:
                    time.sleep(delay_seconds)
                    continue
                else:
                    print(
                        f"[KAMERA] PERINGATAN: {self.unit_label} tidak berhasil "
                        f"memverifikasi kamera hidup setelah {attempts} "
                        f"percobaan. Tetap memakai index {index} apa adanya -- "
                        f"cek koneksi/nama kamera & tutup OBS secepatnya."
                    )
                    last_cap = cap
                    break

            if attempt > 1:
                print(
                    f"[KAMERA] {self.unit_label}: index {index} berhasil "
                    f"dibuka & terverifikasi HIDUP di percobaan ke-{attempt}."
                )
            else:
                print(
                    f"[KAMERA] {self.unit_label}: index {index} berhasil "
                    f"dibuka & terverifikasi HIDUP (nama '{self.expected_name}')."
                )

            last_cap = cap
            break

        if last_cap is None:
            last_cap = cv2.VideoCapture(last_index, cv2.CAP_DSHOW)

        return last_index, last_cap

    @staticmethod
    def _capture_is_static(cap, delay_seconds=0.3):
        def grab_one():
            for _ in range(3):
                cap.grab()
            ok, frame = cap.read()
            return frame if ok else None

        frame_a = grab_one()
        time.sleep(delay_seconds)
        frame_b = grab_one()

        if frame_a is None or frame_b is None:
            # Gagal baca frame sama sekali -- jangan disimpulkan statis,
            # biar ditangani sebagai kegagalan biasa oleh pemanggil.
            return False

        if frame_a.shape != frame_b.shape:
            return False

        return bool((frame_a == frame_b).all())


    def refresh_actual_resolution(self):
        try:
            self.actual_width = int(
                self.cap.get(cv2.CAP_PROP_FRAME_WIDTH)
            )

            self.actual_height = int(
                self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
            )

        except Exception:
            self.actual_width = None
            self.actual_height = None

    def is_ok(self):
        with self.lock:
            return self.cap.isOpened()

    def _read_frame(self):
        # Buang beberapa frame lama dari buffer.
        for _ in range(3):
            self.cap.grab()

        ok, frame = self.cap.read()

        if not ok or frame is None:
            return None

        return frame

    def capture(self, capture_id):
        with self.lock:

            if not self.cap.isOpened():
                return False, None, None, None

            frame = self._read_frame()

            if frame is None:
                return False, None, None, None

            # ----------------------------------------------------
            # ML IMAGE
            # ----------------------------------------------------

            ml_bytes, ml_quality = encode_ml_jpeg(frame)

            if ml_bytes is None:
                return False, None, None, None

            # ----------------------------------------------------
            # ARCHIVE IMAGE
            # ----------------------------------------------------

            archive_bytes = encode_archive_jpeg(frame)

            if archive_bytes is None:
                return False, None, None, None

            # ----------------------------------------------------
            # MEMORY
            # ----------------------------------------------------

            self.last_jpeg_bytes = ml_bytes
            self.last_archive_bytes = archive_bytes
            self.last_capture_id = str(capture_id)
            self.capture_count += 1

            # ----------------------------------------------------
            # LOG
            # ----------------------------------------------------

            print(
                f"[{self.unit_label}] "
                f"capture_id={capture_id} | "
                f"ML={len(ml_bytes)} bytes "
                f"(Q={ml_quality}) | "
                f"ARCHIVE={len(archive_bytes)} bytes"
            )

            # ----------------------------------------------------
            # LOCAL BACKUP
            # ----------------------------------------------------

            try:
                save_local_archive(
                    self.unit_label,
                    capture_id,
                    archive_bytes
                )
            except Exception as e:
                print(f"[ARCHIVE] Gagal simpan lokal: {e}")

            # ----------------------------------------------------
            # KLASIFIKASI ML (LANGSUNG DARI LAPTOP)
            # ----------------------------------------------------
            # Foto ARSIP (kualitas lebih tinggi dari ml_bytes) yang dikirim
            # ke cloud ML sekarang -- bukan ml_bytes yang dulu dipaksa kecil
            # khusus buat muat di RAM ESP32. Hasilnya nanti dilaporkan balik
            # ke ESP32 MAIN lewat /classify setelah top+side dipasangkan.
            classify_thread = threading.Thread(
                target=classify_and_report_background,
                args=(
                    self.unit_key,
                    capture_id,
                    archive_bytes
                ),
                daemon=True
            )

            classify_thread.start()

            # ----------------------------------------------------
            # GOOGLE DRIVE
            # ----------------------------------------------------

            if ENABLE_GOOGLE_UPLOAD:
                thread = threading.Thread(
                    target=upload_to_google_background,
                    args=(
                        self.unit_key,
                        capture_id,
                        archive_bytes
                    ),
                    daemon=True
                )

                thread.start()

            return (
                True,
                ml_bytes,
                archive_bytes,
                ml_quality
            )

    def get_last_jpeg(self):
        with self.lock:
            return self.last_jpeg_bytes

    def get_last_archive(self):
        with self.lock:
            return self.last_archive_bytes


# ============================================================
# CREATE CAMERA OBJECTS
# ============================================================

_startup_device_names = list_camera_devices()

if _startup_device_names:
    print("[KAMERA] Device kamera terdeteksi saat start:")
    for _i, _name in enumerate(_startup_device_names):
        print(f"    index {_i}: {_name}")
elif not HAS_PYGRABBER:
    print("[KAMERA] pygrabber tidak tersedia.")
else:
    print("[KAMERA] Gagal membaca daftar device saat start.")

_camtop_unit = CameraUnit(
    "top",
    "ATAS",
    CAMERA_NAME_TOP,
    CAMERA_INDEX_TOP_FALLBACK,
    _startup_device_names
)

# Jeda sebelum buka kamera kedua -- DSHOW kadang gagal open device
# berikutnya kalau langsung nyusul device pertama tanpa jeda sama sekali.
time.sleep(1.5)

_camside_unit = CameraUnit(
    "side",
    "SAMPING",
    CAMERA_NAME_SIDE,
    CAMERA_INDEX_SIDE_FALLBACK,
    _startup_device_names
)

CAMERAS = {
    "camtop": _camtop_unit,
    "camside": _camside_unit
}


# ============================================================
# LOCAL ARCHIVE
# ============================================================

def save_local_archive(
    unit_label,
    capture_id,
    image_bytes
):
    os.makedirs(
        LOCAL_ARCHIVE_DIR,
        exist_ok=True
    )

    timestamp = datetime.now().strftime(
        "%Y%m%d_%H%M%S_%f"
    )

    filename = (
        f"{timestamp}_"
        f"{unit_label.lower()}_"
        f"{capture_id}.jpg"
    )

    filepath = os.path.join(
        LOCAL_ARCHIVE_DIR,
        filename
    )

    with open(filepath, "wb") as f:
        f.write(image_bytes)

    print(f"[ARCHIVE] Disimpan: {filepath}")

    return filepath


# ============================================================
# GOOGLE APPS SCRIPT UPLOAD
# ============================================================

def upload_to_google_background(
    unit_key,
    capture_id,
    image_bytes
):
    if not GOOGLE_SCRIPT_URL:
        print(
            "[GOOGLE] GOOGLE_SCRIPT_URL masih kosong. "
            "Foto hanya disimpan lokal."
        )
        return

    try:
        image_base64 = base64.b64encode(
            image_bytes
        ).decode("utf-8")

        payload = {
            "action": "upload_photo",
            "key": GOOGLE_API_TOKEN,
            "capture_id": str(capture_id),
            "unit": unit_key,
            "image_base64": image_base64
        }

        print(
            f"[GOOGLE] Upload foto "
            f"{unit_key} capture_id={capture_id}..."
        )

        response = requests.post(
            GOOGLE_SCRIPT_URL,
            json=payload,
            timeout=60
        )

        print(
            f"[GOOGLE] HTTP {response.status_code}"
        )

        try:
            result = response.json()
            print(f"[GOOGLE] Response: {result}")
        except Exception:
            print(
                f"[GOOGLE] Response text: "
                f"{response.text[:500]}"
            )

    except Exception as e:
        print(f"[GOOGLE] Upload gagal: {e}")


# ============================================================
# KLASIFIKASI ML (LANGSUNG DARI LAPTOP -- REVISI BESAR)
# ============================================================
# Sebelumnya: bridge cuma nyediain foto kecil (ML_WIDTH x ML_HEIGHT, dipaksa
# <=80KB) buat diunduh ESP32, lalu ESP32 sendiri yang kirim ke cloud ML dan
# hitung grade akhir. Batas 80KB itu murni gara-gara RAM ESP32 yang mepet,
# BUKAN karena cloud ML-nya butuh foto kecil -- jadi itu ngorbanin ketajaman
# foto buat grading tanpa alasan teknis yang perlu, khususnya buat kamera
# yang sebenarnya cuma webcam laptop (yang RAM & internetnya jauh lebih
# lega dari ESP32).
#
# Sekarang: begitu selesai capture, LAPTOP LANGSUNG kirim foto ARSIP
# (1280x720, quality 85 -- jauh lebih tajam dari yang dulu) ke cloud ML dari
# sini. Setelah grade ATAS & SAMPING untuk satu capture_id yang sama
# terkumpul (dipasangkan mirip pairing foto Sheets di atas), bridge hitung
# grade akhir (terjelek dari 2 sisi) SENDIRI, lalu lapor balik ke ESP32 MAIN
# lewat endpoint /classify. ESP32 tinggal terima hasil jadi & gerakin servo.

ESP32_MAIN_HOST = "main.local"
ESP32_MAIN_IP_FALLBACK = "192.168.1.24"  # sesuaikan kalau IP static MAIN berubah
ESP32_MAIN_PORT = 80

# Berapa lama nunggu HASIL SISI LAINNYA (top/side) sebelum lapor sendirian
# ke ESP32 (mis. satu kamera gagal klasifikasi tapi satunya berhasil).
GRADING_PAIR_WINDOW_SECONDS = 15.0

grading_lock = threading.Lock()
grading_state = {}  # capture_id -> {"top": result|None, "side": result|None, "reported": bool, "timer": Timer|None}


def call_cloud_ml(image_bytes):
    """
    Kirim SATU foto ke cloud ML API, balikin (ok, grade, buah, confidence).
    Ini versi Python dari classifyWithCloudAPI() yang DULU ada di esp32_main.ino
    -- sekarang tempatnya di sini, bukan di ESP32.
    """
    try:
        files = {
            "file": ("fruit.jpg", image_bytes, "image/jpeg")
        }

        response = requests.post(
            CLOUD_API_URL,
            files=files,
            timeout=75  # kasih toleransi cold-start Render.com free tier
        )

        if response.status_code != 200:
            print(f"[ML] CLOUD API ERROR, HTTP {response.status_code}: {response.text[:300]}")
            return False, None, None, 0.0

        data = response.json()

        status = data.get("status", "")
        grade = data.get("grade", "")
        buah = data.get("buah", "")
        confidence = data.get("confidence", 0.0)

        print(f"[ML] STATUS={status} BUAH={buah or '-'} GRADE={grade or '-'} CONF={confidence}")

        if status != "ok" or grade not in ("A", "B"):
            print("[ML] Hasil tidak valid (status bukan 'ok', atau grade bukan A/B) -> dianggap gagal.")
            return False, None, None, confidence

        return True, grade, buah, confidence

    except Exception as e:
        print(f"[ML] Request ke cloud ML gagal: {e}")
        return False, None, None, 0.0


def worse_grade(a, b):
    """A lebih bagus dari B -- balikin yang lebih jelek di antara dua grade."""
    rank = {"A": 0, "B": 1}
    if a not in rank:
        return b
    if b not in rank:
        return a
    return a if rank[a] > rank[b] else b


def send_grade_result_to_esp32(capture_id, grade, buah):
    payload = {
        "capture_id": str(capture_id),
        "grade": grade,
        "buah": buah or ""
    }

    urls_to_try = [
        f"http://{ESP32_MAIN_HOST}:{ESP32_MAIN_PORT}/classify",
        f"http://{ESP32_MAIN_IP_FALLBACK}:{ESP32_MAIN_PORT}/classify",
    ]

    for url in urls_to_try:
        try:
            print(f"[ESP32] Lapor hasil grading ke {url}: {payload}")
            response = requests.post(url, json=payload, timeout=10)
            print(f"[ESP32] HTTP {response.status_code}: {response.text[:300]}")
            if response.status_code == 200:
                return True
            # capture_id_mismatch/error lain -> jangan coba fallback IP,
            # itu bukan masalah koneksi, percuma dicoba ulang ke IP lain.
            if response.status_code == 409:
                return False
        except Exception as e:
            print(f"[ESP32] Gagal hubungi {url}: {e}")
            continue

    print("[ESP32] GAGAL lapor hasil grading ke ESP32 MAIN (mDNS & IP fallback dua-duanya gagal).")
    return False


def finalize_grading(capture_id):
    """
    Dipanggil begitu KEDUA sisi (top+side) sudah lapor, ATAU begitu timer
    GRADING_PAIR_WINDOW_SECONDS habis dan minimal satu sisi punya hasil.
    Menghitung grade akhir dan melapor ke ESP32 -- HANYA SEKALI per capture_id
    (dijaga pakai flag "reported" di bawah lock, supaya race antara "kedua
    sisi baru saja lengkap" dan "timer baru saja habis" tidak lapor dobel).
    """
    with grading_lock:
        entry = grading_state.get(capture_id)
        if entry is None or entry["reported"]:
            return
        entry["reported"] = True
        if entry["timer"] is not None:
            entry["timer"].cancel()
        top = entry["top"]
        side = entry["side"]
        del grading_state[capture_id]

    grade_top = top["grade"] if (top and top["ok"]) else None
    grade_side = side["grade"] if (side and side["ok"]) else None

    if top and top["ok"] and side and side["ok"] and top["buah"] != side["buah"]:
        print(f"[ML] PERINGATAN: ATAS & SAMPING deteksi buah beda ({top['buah']} vs {side['buah']}). Pakai hasil ATAS.")

    buah_final = ""
    if top and top["ok"]:
        buah_final = top["buah"]
    elif side and side["ok"]:
        buah_final = side["buah"]

    if grade_top is None and grade_side is None:
        print(f"[ML] capture_id={capture_id}: KEDUA sisi gagal klasifikasi -> lapor grade C (gagal deteksi).")
        send_grade_result_to_esp32(capture_id, "C", "")
        return

    grade_final = worse_grade(grade_top, grade_side) if (grade_top and grade_side) else (grade_top or grade_side)

    print(f"[ML] capture_id={capture_id}: GRADE FINAL={grade_final} (top={grade_top or '-'}, side={grade_side or '-'})")

    send_grade_result_to_esp32(capture_id, grade_final, buah_final)


def record_grading_result(capture_id, unit_key, ok, grade, buah, confidence):
    side_key = "top" if unit_key == "camtop" else "side"

    with grading_lock:
        entry = grading_state.get(capture_id)
        if entry is None:
            entry = {"top": None, "side": None, "reported": False, "timer": None}
            grading_state[capture_id] = entry

        entry[side_key] = {"ok": ok, "grade": grade, "buah": buah, "confidence": confidence}

        both_done = entry["top"] is not None and entry["side"] is not None

        if not both_done and entry["timer"] is None:
            # Ini hasil pertama yang masuk untuk capture_id ini -- kasih
            # waktu buat sisi lainnya nyusul sebelum lapor sendirian.
            timer = threading.Timer(GRADING_PAIR_WINDOW_SECONDS, finalize_grading, args=(capture_id,))
            timer.daemon = True
            entry["timer"] = timer
            timer.start()

    if both_done:
        finalize_grading(capture_id)


def classify_and_report_background(unit_key, capture_id, image_bytes):
    ok, grade, buah, confidence = call_cloud_ml(image_bytes)
    record_grading_result(capture_id, unit_key, ok, grade, buah, confidence)


def keep_cloud_api_warm_loop():
    """Ping cloud ML secara berkala biar gak cold-start pas buah pertama datang."""
    root_url = CLOUD_API_URL
    if root_url.endswith("/predict"):
        root_url = root_url[: -len("/predict")]

    while True:
        try:
            r = requests.get(root_url, timeout=70)
            print(f"[ML KEEP-ALIVE] HTTP {r.status_code}")
        except Exception as e:
            print(f"[ML KEEP-ALIVE] gagal: {e}")
        time.sleep(8 * 60)


# ============================================================
# UNIT RESOLUTION
# ============================================================

def resolve_unit():
    unit_param = (
        request.args.get("unit") or ""
    ).strip().lower()

    if unit_param in (
        "top",
        "atas",
        "camtop"
    ):
        return "camtop"

    if unit_param in (
        "side",
        "samping",
        "camside"
    ):
        return "camside"

    host = (
        request.host or ""
    ).split(":")[0].lower()

    if host.startswith("camtop"):
        return "camtop"

    if host.startswith("camside"):
        return "camside"

    return None


def unit_not_found_response():
    return jsonify({
        "status": "error",
        "error": (
            "Unit kamera tidak dapat ditentukan. "
            "Gunakan camtop.local / camside.local "
            "atau ?unit=top / ?unit=side."
        )
    }), 400


# ============================================================
# STATUS
# ============================================================

@app.route("/status", methods=["GET"])
def status():
    unit = resolve_unit()

    if unit is None:
        return unit_not_found_response()

    cam = CAMERAS[unit]

    if cam.is_ok():
        return jsonify({
            "status": "online",
            "unit": cam.unit_label,
            "camera_index": cam.camera_index,
            "resolution": (
                f"{cam.actual_width}x"
                f"{cam.actual_height}"
            ),
            "last_capture_id": cam.last_capture_id
        }), 200

    return jsonify({
        "status": "offline",
        "unit": cam.unit_label
    }), 503


# ============================================================
# CAPTURE
# ============================================================

@app.route("/capture", methods=["GET"])
def capture():
    unit = resolve_unit()

    if unit is None:
        return unit_not_found_response()

    cam = CAMERAS[unit]

    # ESP32 boleh mengirim capture_id sendiri.
    requested_capture_id = (
        request.args.get("capture_id") or ""
    ).strip()

    capture_id = get_capture_id_for_unit(
        cam.unit_key,
        requested_capture_id
    )

    ok, ml_bytes, archive_bytes, ml_quality = (
        cam.capture(capture_id)
    )

    if not ok:
        return jsonify({
            "status": "capture_failed",
            "unit": cam.unit_label,
            "capture_id": capture_id
        }), 500

    ml_size = len(ml_bytes)
    archive_size = len(archive_bytes)

    return jsonify({
        "status": "captured",
        "unit": cam.unit_label,
        "unit_key": cam.unit_key,
        "capture_id": str(capture_id),
        "ml_jpeg_bytes": ml_size,
        "archive_jpeg_bytes": archive_size,
        "ml_jpeg_quality": ml_quality,
        "ml_limit_bytes": MAX_ESP32_JPEG_BYTES,
        "ml_under_limit": (
            ml_size <= MAX_ESP32_JPEG_BYTES
        )
    }), 200


# ============================================================
# JPG
# ============================================================

@app.route("/jpg", methods=["GET"])
def jpg():
    unit = resolve_unit()

    if unit is None:
        return unit_not_found_response()

    cam = CAMERAS[unit]

    data = cam.get_last_jpeg()

    if data is None:
        return jsonify({
            "status": "error",
            "unit": cam.unit_label,
            "error": (
                "Belum ada foto. "
                "Panggil /capture terlebih dahulu."
            )
        }), 404

    return Response(
        data,
        mimetype="image/jpeg"
    )


# ============================================================
# STREAM
# ============================================================

@app.route("/stream", methods=["GET"])
def stream():
    unit = resolve_unit()

    if unit is None:
        return unit_not_found_response()

    cam = CAMERAS[unit]

    def generate():
        while True:
            with cam.lock:
                if not cam.cap.isOpened():
                    break

                ok, frame = cam.cap.read()

            if not ok or frame is None:
                continue

            frame = cv2.resize(
                frame,
                (ML_WIDTH, ML_HEIGHT),
                interpolation=cv2.INTER_AREA
            )

            ok_encode, buf = cv2.imencode(
                ".jpg",
                frame,
                [
                    int(cv2.IMWRITE_JPEG_QUALITY),
                    ML_JPEG_QUALITY_START
                ]
            )

            if not ok_encode:
                continue

            yield (
                b"--frame\r\n"
                b"Content-Type: image/jpeg\r\n\r\n"
                + buf.tobytes()
                + b"\r\n"
            )

    return Response(
        generate(),
        mimetype=(
            "multipart/x-mixed-replace;"
            "boundary=frame"
        )
    )


# ============================================================
# PREVIEW (2 KAMERA SEKALIGUS)
# ============================================================
# Halaman ini nampilin stream TOP dan SAMPING berdampingan dalam SATU
# halaman browser, masing-masing <img> nunjuk ke endpoint /stream milik
# unit itu sendiri. Browser bakal terus render frame baru sendiri karena
# /stream ngirim multipart MJPEG (gak perlu refresh manual / JS tambahan).

@app.route("/preview", methods=["GET"])
def preview_page():
    cards = []

    for key in ("camtop", "camside"):
        cam = CAMERAS[key]
        state = "ONLINE" if cam.is_ok() else "OFFLINE"

        cards.append(f"""
        <div class="cam-card">
            <h3>{cam.unit_label} ({key}.local) &mdash; {state}</h3>
            <img src="/stream?unit={key}" alt="{cam.unit_label}" />
            <p>index={cam.camera_index} | resolusi={cam.actual_width}x{cam.actual_height}</p>
        </div>
        """)

    html = f"""
    <!DOCTYPE html>
    <html lang="id">
    <head>
        <meta charset="utf-8">
        <title>Preview 2 Kamera</title>
        <style>
            body {{
                background: #111;
                color: #eee;
                font-family: sans-serif;
                text-align: center;
            }}
            .cam-row {{
                display: flex;
                flex-wrap: wrap;
                justify-content: center;
                gap: 20px;
                padding: 20px;
            }}
            .cam-card {{
                background: #1e1e1e;
                border-radius: 8px;
                padding: 10px;
            }}
            .cam-card img {{
                width: 480px;
                max-width: 90vw;
                border-radius: 4px;
                background: #000;
            }}
            h2 {{ margin-top: 20px; }}
        </style>
    </head>
    <body>
        <h2>Preview 2 Kamera Langsung</h2>
        <div class="cam-row">
            {"".join(cards)}
        </div>
        <p><a href="/" style="color:#8cf;">&larr; kembali</a></p>
    </body>
    </html>
    """

    return html, 200


# ============================================================
# ROOT
# ============================================================

@app.route("/", methods=["GET"])
def root():
    html = [
        "<h2>Camera Bridge Server</h2>",
        "<p>2 USB Webcam - TOP + SIDE</p>"
    ]

    for key, cam in CAMERAS.items():
        state = (
            "ONLINE"
            if cam.is_ok()
            else "OFFLINE"
        )

        html.append(
            f"<p>"
            f"<b>{cam.unit_label}</b> "
            f"({key}.local): {state}<br>"
            f"index={cam.camera_index}<br>"
            f"resolution="
            f"{cam.actual_width}x"
            f"{cam.actual_height}<br>"
            f"last_capture_id="
            f"{cam.last_capture_id}"
            f"</p>"
        )

    html.extend([
        "<hr>",
        '<p><a href="/preview" style="font-weight:bold;">'
        "Preview 2 Kamera Sekaligus</a></p>",
        '<p><a href="/status?unit=top">'
        "Status ATAS</a></p>",
        '<p><a href="/status?unit=side">'
        "Status SAMPING</a></p>",
        '<p><a href="/stream?unit=top">'
        "Stream ATAS</a></p>",
        '<p><a href="/stream?unit=side">'
        "Stream SAMPING</a></p>"
    ])

    return "\n".join(html), 200


# ============================================================
# mDNS
# ============================================================

def get_local_ip():
    s = socket.socket(
        socket.AF_INET,
        socket.SOCK_DGRAM
    )

    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = "127.0.0.1"
    finally:
        s.close()

    return ip


def start_mdns(local_ip):
    zc = Zeroconf()
    infos = []

    for mdns_name in (
        "camtop",
        "camside"
    ):
        info = ServiceInfo(
            "_http._tcp.local.",
            f"{mdns_name}-bridge._http._tcp.local.",
            addresses=[
                socket.inet_aton(local_ip)
            ],
            port=80,
            server=f"{mdns_name}.local."
        )

        zc.register_service(info)
        infos.append(info)

        print(
            f"[mDNS] {mdns_name}.local -> {local_ip}:80"
        )

    return zc


# ============================================================
# CAMERA TEST
# ============================================================

def list_cameras_and_exit():
    device_names = list_camera_devices()

    if device_names:
        print("Nama device kamera:")

        for i, name in enumerate(device_names):
            print(f"  index {i}: {name}")

    else:
        print(
            "Nama device tidak tersedia "
            "(pygrabber belum tersedia)."
        )

    print()
    print("Mengecek index kamera 0..5...")
    print()

    found_any = False

    for idx in range(6):
        cap = cv2.VideoCapture(
            idx,
            cv2.CAP_DSHOW
        )

        if not cap.isOpened():
            cap.release()
            continue

        ok, frame = cap.read()
        cap.release()

        if not ok or frame is None:
            print(
                f"index {idx}: GAGAL mengambil frame"
            )
            continue

        found_any = True

        filename = f"cam_index_{idx}.jpg"

        cv2.imwrite(
            filename,
            frame
        )

        print(
            f"index {idx}: OK -> {filename}"
        )

    if not found_any:
        print("Tidak ada kamera yang berhasil dibuka.")


# ============================================================
# MAIN
# ============================================================

if __name__ == "__main__":

    if "--list-cameras" in sys.argv:
        list_cameras_and_exit()
        sys.exit(0)

    local_ip = get_local_ip()

    zeroconf_instance = start_mdns(local_ip)

    print()
    print("========================================")
    print(" CAMERA BRIDGE SERVER - 2 USB WEBCAM")
    print("========================================")
    print(
        f"ML RESOLUTION       = "
        f"{ML_WIDTH}x{ML_HEIGHT}"
    )
    print(
        f"ML QUALITY START    = "
        f"{ML_JPEG_QUALITY_START}"
    )
    print(
        f"ML MAX SIZE         = "
        f"{MAX_ESP32_JPEG_BYTES} bytes"
    )
    print(
        f"ARCHIVE RESOLUTION  = "
        f"{ARCHIVE_WIDTH}x{ARCHIVE_HEIGHT}"
    )
    print(
        f"ARCHIVE QUALITY     = "
        f"{ARCHIVE_JPEG_QUALITY}"
    )
    print(
        f"GOOGLE UPLOAD       = "
        f"{ENABLE_GOOGLE_UPLOAD}"
    )
    print(
        f"PAIR WINDOW         = "
        f"{PAIR_WINDOW_SECONDS} seconds"
    )
    print(
        f"CLOUD ML URL        = "
        f"{CLOUD_API_URL}"
    )
    print(
        f"ESP32 MAIN CALLBACK = "
        f"{ESP32_MAIN_HOST} (fallback {ESP32_MAIN_IP_FALLBACK})"
    )
    print()

    keep_alive_thread = threading.Thread(
        target=keep_cloud_api_warm_loop,
        daemon=True
    )
    keep_alive_thread.start()

    for key, cam in CAMERAS.items():
        ready = (
            "SIAP"
            if cam.is_ok()
            else "TIDAK TERBACA"
        )

        print(
            f"{cam.unit_label:8s} "
            f"({key}.local) "
            f"index={cam.camera_index} "
            f"resolution="
            f"{cam.actual_width}x"
            f"{cam.actual_height} "
            f"{ready}"
        )

    print()
    print(f"IP LAPTOP = {local_ip}")
    print(f"FLASK INTERNAL PORT = {INTERNAL_PORT}")
    print()
    print("Endpoint:")
    print("  http://camtop.local/status")
    print("  http://camtop.local/capture")
    print("  http://camtop.local/jpg")
    print("  http://camtop.local/stream")
    print()
    print("  http://camside.local/status")
    print("  http://camside.local/capture")
    print("  http://camside.local/jpg")
    print("  http://camside.local/stream")
    print()
    print("========================================")

    try:
        app.run(
            host="0.0.0.0",
            port=INTERNAL_PORT,
            threaded=True
        )

    finally:
        zeroconf_instance.close()

        for cam in CAMERAS.values():
            try:
                cam.cap.release()
            except Exception:
                pass
