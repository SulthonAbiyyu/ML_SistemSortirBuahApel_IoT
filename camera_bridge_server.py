"""
CAMERA BRIDGE SERVER (MULTI-KAMERA) - 2 webcam USB jadi pengganti KEDUA
ESP32-CAM (ATAS & SAMPING) yang sudah tidak dipakai lagi.
======================================================================
REVISI BESAR dari versi sebelumnya:
- Versi lama: cuma 1 webcam (gantiin ESP32-CAM SAMPING yang rusak),
  ESP32-CAM ATAS masih fisik asli.
- Versi ini: KEDUA sisi (ATAS & SAMPING) sekarang webcam USB, dicolok
  BARENG ke 1 LAPTOP YANG SAMA. esp32camATAS.ino / esp32camSAMPING.ino
  SUDAH TIDAK DIPAKAI SAMA SEKALI (lihat catatan besar di bagian bawah
  docstring ini) -- board ESP32-CAM fisiknya boleh dilepas/disimpan.

Jalan di LAPTOP, bukan di ESP32. Tujuannya: meniru PERSIS endpoint yang
biasanya disediakan ESP32-CAM (/status, /capture, /jpg) UNTUK KEDUA
UNIT SEKALIGUS, supaya esp32_main.ino TIDAK PERLU diubah kodenya sama
sekali (dia tetap manggil "camtop.local" dan "camside.local" seperti
biasa -- lihat CAM_TOP_HOST/CAM_SIDE_HOST di esp32_main.ino).

============================================================
KENAPA BISA 1 LAPTOP MELAYANI 2 "NAMA KAMERA" SEKALIGUS?
============================================================
esp32_main.ino manggil URL tanpa port eksplisit (mis. "http://camtop.
local/status"), yang artinya SELALU port 80 (default HTTP). Satu
laptop CUMA BISA punya 1 proses yang pegang port 80 di 1 waktu -- jadi
TIDAK BISA jalankan 2 Flask app terpisah di port 80 yang sama.

Solusinya: HANYA ADA 1 proses Flask, tapi dia:
1. Mengumumkan DUA nama mDNS sekaligus ("camtop.local" DAN
   "camside.local"), keduanya menunjuk ke IP laptop yang SAMA, port 80
   yang SAMA.
2. Saat ESP32 MAIN request masuk, request HTTP itu SELALU membawa
   header "Host:" berisi nama yang tadi dipanggil ESP32 (mis.
   "Host: camtop.local" atau "Host: camside.local") -- ini bagian
   standar protokol HTTP/1.1, browser/HTTPClient ESP32 otomatis
   mengirimkannya. Server ini MEMBACA header itu untuk menentukan
   "oh, ini request untuk kamera ATAS" vs "ini untuk kamera SAMPING",
   lalu ambil gambar dari webcam yang sesuai.
Jadi walau secara jaringan cuma ada 1 IP:port, secara LOGIKA ESP32
tetap "melihat" 2 kamera terpisah persis seperti sebelumnya.

============================================================
REVISI #2 -- SUPAYA .EXE BISA DIPAKAI USER AWAM TANPA RIBET
============================================================
Dua masalah yang diperbaiki di revisi ini dibanding versi sebelumnya:

(A) DULU: harus "Run as Administrator" SETIAP KALI jalankan .exe,
    karena port 80 butuh hak admin di Windows.
    SEKARANG: Flask jalan di INTERNAL_PORT (default 8080, TIDAK butuh
    admin). Supaya ESP32 MAIN yang manggil "http://camtop.local/..."
    (otomatis port 80) tetap kena, ada 1x SETUP ADMIN YANG DILAKUKAN
    SEKALI SAJA per komputer (lihat file setup_admin_onetime.bat yang
    menyertai script ini) -- setelah setup itu, .exe bisa didobel-klik
    biasa SELAMANYA tanpa admin/UAC/dialog Defender lagi. Setup itu
    memasang:
      1. Firewall rule (izinkan .exe ini menerima koneksi masuk)
      2. netsh portproxy: semua request ke port 80 di-redirect diam-
         diam ke port 8080 (jadi ESP32 MAIN tidak perlu tahu apa-apa,
         tetap manggil port 80 seperti biasa).

(B) DULU: index webcam (CAMERA_INDEX_TOP/SIDE) berupa ANGKA yang bisa
    berubah-ubah tergantung urutan colok/status kamera laptop bawaan
    (kalau kamera laptop nyala/disable, semua angka index kamera lain
    bisa ikut geser).
    SEKARANG: kamera dikenali lewat NAMA device (CAMERA_NAME_TOP /
    CAMERA_NAME_SIDE di bawah), bukan angka. Nama device biasanya
    TIDAK BERUBAH walau kamera laptop nyala/mati/index geser -- jadi
    kamera laptop otomatis diabaikan (namanya beda), TIDAK PERLU
    di-disable manual lagi. Ini butuh library "pygrabber" (Windows-
    only, pip install pygrabber) untuk baca nama device DirectShow.

============================================================
CARA SETUP (dilakukan SEKALI oleh kamu/developer, bukan user akhir)
============================================================
1. pip install flask opencv-python zeroconf pygrabber
2. Colokkan KEDUA webcam USB ke laptop target.
3. Cari nama asli tiap webcam + cocokkan sama fotonya:

       python camera_bridge_server.py --list-cameras

   Ini mencetak daftar SEMUA device kamera yang OS kenali (index +
   NAMA device DirectShow-nya, termasuk kamera laptop bawaan kalau
   ada), lalu untuk tiap index yang berhasil dibuka, simpan 1 foto
   sebagai cam_index_0.jpg, cam_index_1.jpg, dst supaya kamu tahu
   nama itu punya kamera yang mana secara visual.
4. Isi CAMERA_NAME_TOP dan CAMERA_NAME_SIDE di bawah dengan potongan
   nama (case-insensitive, cukup sebagian, tidak perlu persis semua)
   yang UNIK milik tiap webcam eksternal -- JANGAN pakai potongan
   nama yang juga cocok ke kamera laptop bawaan (nama kamera laptop
   biasanya mengandung kata seperti "Integrated Camera", "HD WebCam",
   "Built-in"). Kalau ragu unik atau tidak, lihat daftar lengkap yang
   dicetak --list-cameras tadi.
5. Build ulang .exe (PyInstaller, pastikan pygrabber ikut ter-bundle,
   biasanya otomatis karena di-import di script ini).
6. Jalankan setup_admin_onetime.bat SEBAGAI ADMINISTRATOR di komputer
   target -- SEKALI SAJA per komputer, tidak perlu diulang tiap deploy
   .exe baru selama nomor portnya tidak diganti.
7. Pastikan laptop connect ke WiFi/hotspot YANG SAMA dengan ESP32 MAIN.
8. Mulai sekarang, user tinggal DOBEL-KLIK .exe-nya seperti biasa
   (TANPA klik kanan "Run as Administrator"). Cek working dengan buka
   browser DI LAPTOP YANG SAMA:
   http://camtop.local/status    -> {"status":"online","unit":"ATAS"}
   http://camside.local/status   -> {"status":"online","unit":"SAMPING"}
   (Kalau browser di laptop sendiri belum bisa resolve *.local, itu
   normal di sebagian OS -- coba dari HP yang connect ke WiFi yang
   sama, biasanya HP lebih konsisten dukung mDNS/Bonjour.)
9. (Debug tambahan) http://<IP-laptop>:8080/ -- halaman ringkasan
   status KEDUA kamera + nama device yang terdeteksi (akses lewat
   port 8080 langsung, bukan lewat portproxy).

CATATAN: kalau suatu saat kamu ganti merk/model webcam, nama device-
nya juga berubah -- ulangi langkah 3-5 (cari nama baru, update
CAMERA_NAME_TOP/SIDE, build ulang .exe). Kalau pygrabber gagal load
atau nama tidak ketemu, script FALLBACK otomatis ke angka index manual
(CAMERA_INDEX_TOP_FALLBACK/CAMERA_INDEX_SIDE_FALLBACK) supaya sistem
tidak mati total -- tapi ini kembali ke masalah lama (index bisa
geser), jadi perbaiki nama device-nya begitu sempat.

============================================================
PENTING - keterbatasan yang harus kamu terima sadar
============================================================
- Laptop HARUS tetap nyala & script ini HARUS tetap jalan selama
  sistem sortir beroperasi -- SEKARANG UNTUK KEDUA SISI SEKALIGUS.
  Kalau laptop sleep/mati/script ditutup, KEDUA kamera (ATAS &
  SAMPING) otomatis "OFFLINE" bersamaan (beda dengan versi lama, yang
  cuma SAMPING doang yang kena).
- Matikan sleep mode laptop (Settings > Power > Sleep > Never) selama
  dipakai buat testing/demo.
- Firewall & portproxy sudah diurus SEKALI oleh setup_admin_onetime.bat
  (lihat REVISI #2 di atas) -- kalau ESP32 MAIN masih gagal connect
  padahal .exe sudah jalan, kemungkinan setup itu belum pernah
  dijalankan di komputer ini, atau nomor INTERNAL_PORT di script beda
  dengan yang di-setup di .bat-nya. mDNS butuh network profile
  "Private" (bukan "Public") di Windows.
- KALAU WiFi/venue-nya WiFi publik/kampus dengan "client isolation"
  aktif, TIDAK ADA metode manapun yang akan jalan (bukan bug kode,
  batasan jaringan). Demo/testing paling aman pakai hotspot HP sendiri.
- Kalau salah satu webcam USB dicabut/lepas di tengah operasi, HANYA
  unit itu yang jadi OFFLINE (dideteksi lewat /status per-unit) --
  unit satunya tetap jalan normal, tidak saling menjatuhkan.
- Kamera laptop bawaan BOLEH tetap aktif (tidak perlu di-disable) --
  deteksi sekarang berdasarkan NAMA device (CAMERA_NAME_TOP/SIDE),
  bukan angka index, jadi kamera laptop otomatis diabaikan selama
  namanya tidak mengandung potongan teks yang sama dengan 2 webcam
  eksternal.

============================================================
STATUS FILE esp32camATAS.ino / esp32camSAMPING.ino: SUDAH TIDAK DIPAKAI
============================================================
Karena KEDUA sisi (ATAS & SAMPING) sekarang webcam USB lewat bridge
server ini, file esp32camATAS.ino dan esp32camSAMPING.ino (source untuk
board fisik AI-Thinker ESP32-CAM) TIDAK PERLU di-upload/dijalankan
sama sekali untuk saat ini. Jangan dihapus dari project -- simpan saja
sebagai arsip/cadangan, supaya kalau nanti salah satu atau kedua board
ESP32-CAM diperbaiki dan mau dipakai lagi, tinggal upload ulang file
itu ke board-nya (tidak perlu menulis ulang dari nol). esp32_main.ino
TIDAK PEDULI apakah lawan bicaranya board ESP32-CAM asli atau bridge
laptop ini -- selama endpoint /status, /capture, /jpg tersedia di
"camtop.local" dan "camside.local", perilakunya identik dari sudut
pandang ESP32 MAIN.
"""

import sys
import threading
import socket

from flask import Flask, Response, jsonify, request
import cv2
from zeroconf import Zeroconf, ServiceInfo

try:
    from pygrabber.dshow_graph import FilterGraph
    HAS_PYGRABBER = True
except ImportError:
    # pygrabber tidak ke-install / bukan Windows -> nanti fallback ke
    # index manual otomatis, lihat resolve_camera_indices().
    HAS_PYGRABBER = False

app = Flask(__name__)

# ============================================================
# PORT INTERNAL FLASK -- lihat REVISI #2 di docstring atas.
# ============================================================
# TIDAK PAKAI 80 lagi supaya .exe tidak butuh admin tiap dijalankan.
# Port 80 tetap "kelihatan" dari luar (ESP32 MAIN) berkat portproxy
# yang dipasang SEKALI lewat setup_admin_onetime.bat. Kalau kamu ganti
# angka ini, WAJIB jalankan ulang setup_admin_onetime.bat juga dengan
# angka yang sama supaya portproxy-nya cocok.
INTERNAL_PORT = 8080

# ============================================================
# >>> WAJIB DICEK SEKALI: NAMA WEBCAM DI OS <<<
# ============================================================
# Jalankan "python camera_bridge_server.py --list-cameras" untuk lihat
# nama semua device kamera yang OS kenali (lihat instruksi CARA SETUP
# langkah 3 di docstring atas). Isi potongan nama yang UNIK untuk tiap
# webcam eksternal -- jangan sampai potongan ini juga cocok ke nama
# kamera laptop bawaan. Cocok = "mengandung teks ini", tidak perlu
# sama persis, tidak case-sensitive.
CAMERA_NAME_TOP = "JETE-W7"
CAMERA_NAME_SIDE = "WEB CAMER"

# Fallback kalau pygrabber tidak tersedia ATAU nama di atas tidak
# ketemu/ambigu -- dipakai APA ADANYA, jadi tetap rawan geser kalau
# kamera laptop nyala-mati. Perbaiki nama device di atas begitu sempat.
CAMERA_INDEX_TOP_FALLBACK = 1
CAMERA_INDEX_SIDE_FALLBACK = 0


def list_camera_devices():
    """Kembalikan list nama device kamera sesuai urutan index yang
    dipakai cv2.VideoCapture (index list = posisi array). Kosong kalau
    pygrabber tidak tersedia."""
    if not HAS_PYGRABBER:
        return []
    try:
        return FilterGraph().get_input_devices()
    except Exception as e:
        print(f"[KAMERA] Gagal baca daftar nama device via pygrabber: {e}")
        return []


def find_index_by_name(name_substring, device_names):
    """Cari SATU index yang namanya mengandung name_substring
    (case-insensitive). Return None kalau tidak ketemu atau ambigu
    (lebih dari 1 cocok -- lebih aman gagal daripada salah pilih)."""
    needle = name_substring.strip().lower()
    matches = [i for i, name in enumerate(device_names) if needle in name.lower()]
    if len(matches) == 1:
        return matches[0]
    return None


def resolve_camera_indices():
    """Tentukan index TOP & SIDE yang dipakai. Prioritas: cocokkan
    CAMERA_NAME_TOP/SIDE ke daftar nama device (kamera laptop otomatis
    terhindar karena namanya beda). Kalau gagal, fallback ke angka
    manual di atas -- dicetak warning supaya ketahuan."""
    device_names = list_camera_devices()

    if device_names:
        print("[KAMERA] Device kamera terdeteksi OS:")
        for i, name in enumerate(device_names):
            print(f"    index {i}: {name}")

        idx_top = find_index_by_name(CAMERA_NAME_TOP, device_names)
        idx_side = find_index_by_name(CAMERA_NAME_SIDE, device_names)

        if idx_top is not None and idx_side is not None and idx_top != idx_side:
            print(f"[KAMERA] Cocok by NAMA -> ATAS=index {idx_top}, SAMPING=index {idx_side}")
            return idx_top, idx_side

        print("[KAMERA] PERINGATAN: CAMERA_NAME_TOP/SIDE tidak ketemu/ambigu/sama. "
              "Cek lagi isi CAMERA_NAME_TOP/CAMERA_NAME_SIDE di script ini "
              "dibanding daftar nama di atas. FALLBACK ke index manual.")
    else:
        print("[KAMERA] pygrabber tidak tersedia/gagal baca nama device. "
              "FALLBACK ke index manual (rawan geser kalau kamera laptop nyala-mati).")

    return CAMERA_INDEX_TOP_FALLBACK, CAMERA_INDEX_SIDE_FALLBACK


CAMERA_INDEX_TOP, CAMERA_INDEX_SIDE = resolve_camera_indices()

# Resolusi capture. Turunkan kalau webcam/USB hub kamu tidak kuat di
# 1280x720 (capture jadi gagal/lambat) -- 640x480 biasanya aman.
CAPTURE_WIDTH = 1280
CAPTURE_HEIGHT = 720


# ============================================================
# STATE PER KAMERA
# ============================================================
# REVISI: dulu cuma 1 set variabel global (cap, camera_lock,
# last_jpeg_bytes) karena cuma 1 webcam. Sekarang dibungkus per unit
# dalam dict CAMERAS, supaya ATAS & SAMPING punya lock dan buffer foto
# TERAKHIR masing-masing yang independen -- capture di satu unit tidak
# saling tunggu/tabrakan dengan unit lainnya.

class CameraUnit:
    def __init__(self, unit_label, camera_index):
        self.unit_label = unit_label       # "ATAS" / "SAMPING", buat log & JSON
        self.camera_index = camera_index
        self.lock = threading.Lock()
        self.cap = cv2.VideoCapture(camera_index)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, CAPTURE_WIDTH)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CAPTURE_HEIGHT)
        self.last_jpeg_bytes = None
        self.capture_count = 0

    def is_ok(self):
        with self.lock:
            return self.cap.isOpened()

    def capture(self):
        """Ambil 1 foto baru dari webcam ini, simpan sbg foto terakhir.
        Return True/False."""
        with self.lock:
            if not self.cap.isOpened():
                return False

            # Buang beberapa frame basi dulu - webcam USB murah sering
            # nyimpen buffer beberapa frame lama.
            for _ in range(3):
                self.cap.grab()

            ok, frame = self.cap.read()
            if not ok or frame is None:
                return False

            ok_encode, buf = cv2.imencode(".jpg", frame)
            if not ok_encode:
                return False

            self.last_jpeg_bytes = buf.tobytes()
            self.capture_count += 1
            return True

    def get_last_jpeg(self):
        with self.lock:
            return self.last_jpeg_bytes


# Kunci dict: "camtop" / "camside" -- dipetakan dari Host header request.
CAMERAS = {
    "camtop": CameraUnit("ATAS", CAMERA_INDEX_TOP),
    "camside": CameraUnit("SAMPING", CAMERA_INDEX_SIDE),
}


# ============================================================
# TENTUKAN UNIT MANA DARI REQUEST YANG MASUK
# ============================================================
# Prioritas:
# 1. Query string eksplisit ?unit=top / ?unit=side -- buat testing
#    manual lewat browser/curl langsung ke IP laptop (Host header-nya
#    IP, bukan "camtop.local"/"camside.local", jadi tidak bisa
#    dibedakan otomatis lewat Host).
# 2. Host header request (kasus normal: ESP32 MAIN manggil lewat
#    hostname mDNS, jadi Host header-nya persis "camtop.local" atau
#    "camside.local", dengan atau tanpa ":80" di belakangnya).
def resolve_unit():
    unit_param = (request.args.get("unit") or "").strip().lower()
    if unit_param in ("top", "atas", "camtop"):
        return "camtop"
    if unit_param in ("side", "samping", "camside"):
        return "camside"

    host = (request.host or "").split(":")[0].lower()
    if host.startswith("camtop"):
        return "camtop"
    if host.startswith("camside"):
        return "camside"

    return None


def unit_not_found_response():
    return jsonify({
        "status": "error",
        "error": (
            "Tidak bisa tentukan unit kamera dari request ini. Akses lewat "
            "http://camtop.local/... atau http://camside.local/..., atau "
            "tambahkan query ?unit=top / ?unit=side untuk testing manual."
        ),
    }), 400


# ============================================================
# ENDPOINT - SAMA PERSIS strukturnya dengan ESP32-CAM asli, supaya
# esp32_main.ino tidak perlu tahu bedanya.
# ============================================================

@app.route("/status", methods=["GET"])
def status():
    unit = resolve_unit()
    if unit is None:
        return unit_not_found_response()

    cam = CAMERAS[unit]
    if cam.is_ok():
        return jsonify({"status": "online", "unit": cam.unit_label}), 200
    else:
        # Webcam tidak terbaca (kabel USB lepas, dipakai app lain, dll)
        # -> balas bukan 200, supaya checkCameraAt() di ESP32 MAIN
        # benar-benar mendeteksi ini sebagai OFFLINE.
        return jsonify({"status": "offline", "unit": cam.unit_label}), 503


@app.route("/capture", methods=["GET"])
def capture():
    unit = resolve_unit()
    if unit is None:
        return unit_not_found_response()

    cam = CAMERAS[unit]
    ok = cam.capture()

    if ok:
        return jsonify({
            "status": "captured",
            "unit": cam.unit_label,
            "capture_id": cam.capture_count,
        }), 200
    else:
        return jsonify({"status": "capture_failed", "unit": cam.unit_label}), 500


@app.route("/jpg", methods=["GET"])
def jpg():
    unit = resolve_unit()
    if unit is None:
        return unit_not_found_response()

    cam = CAMERAS[unit]
    data = cam.get_last_jpeg()

    if data is None:
        # Belum pernah ada /capture yang berhasil sebelumnya untuk unit ini.
        return jsonify({
            "status": "error",
            "unit": cam.unit_label,
            "error": "belum ada foto, panggil /capture dulu",
        }), 404

    return Response(data, mimetype="image/jpeg")


@app.route("/", methods=["GET"])
def root():
    """Halaman ringkasan status KEDUA kamera sekaligus -- buat debug
    lewat IP laptop langsung, tidak perlu tahu hostname mDNS."""
    lines = ["<h2>Camera Bridge Server (2 webcam)</h2>"]
    for key, cam in CAMERAS.items():
        state = "ONLINE" if cam.is_ok() else "OFFLINE"
        lines.append(
            f"<p><b>{cam.unit_label}</b> ({key}.local, index kamera "
            f"{cam.camera_index}): {state}, capture_count={cam.capture_count}</p>"
        )
    lines.append('<p><a href="/status?unit=top">/status?unit=top</a></p>')
    lines.append('<p><a href="/status?unit=side">/status?unit=side</a></p>')
    return "\n".join(lines), 200


# ============================================================
# mDNS - umumkan KEDUA nama sekaligus, menunjuk ke IP+port yang SAMA
# ============================================================

def get_local_ip():
    """Cari IP laptop di jaringan WiFi saat ini secara otomatis."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip


def start_mdns(local_ip):
    """Daftarkan 'camtop.local' DAN 'camside.local', keduanya menunjuk
    ke IP+port yang sama (1 proses Flask ini). Perbedaan kamera
    ditentukan di level aplikasi lewat Host header (lihat resolve_unit())."""
    zc = Zeroconf()
    infos = []

    for mdns_name in ("camtop", "camside"):
        info = ServiceInfo(
            "_http._tcp.local.",
            f"{mdns_name}-bridge._http._tcp.local.",
            addresses=[socket.inet_aton(local_ip)],
            port=80,
            server=f"{mdns_name}.local.",
        )
        zc.register_service(info)
        infos.append(info)
        print(f"[mDNS] Diumumkan sebagai {mdns_name}.local -> {local_ip}")

    return zc


def list_cameras_and_exit():
    """--list-cameras: cetak NAMA device tiap kamera (kalau pygrabber
    ada), lalu buka index 0..5, simpan 1 foto tiap index yang berhasil
    -- buat bantu tentukan CAMERA_NAME_TOP/SIDE yang benar."""
    device_names = list_camera_devices()
    if device_names:
        print("Nama device kamera yang terdeteksi OS (urutan = index):")
        for i, name in enumerate(device_names):
            print(f"  index {i}: {name}")
        print()
    else:
        print("(pygrabber tidak tersedia -- tidak bisa baca nama device, "
              "cuma bisa cocokkan lewat foto di bawah)")
        print()

    print("Mengecek index webcam yang tersedia (0..5)...")
    found_any = False

    for idx in range(6):
        cap = cv2.VideoCapture(idx)
        opened = cap.isOpened()

        if not opened:
            cap.release()
            continue

        ok, frame = cap.read()
        cap.release()

        if not ok or frame is None:
            print(f"  index {idx}: kebuka tapi GAGAL ambil frame, dilewati.")
            continue

        found_any = True
        filename = f"cam_index_{idx}.jpg"
        cv2.imwrite(filename, frame)
        print(f"  index {idx}: OK, foto disimpan ke '{filename}'. "
              f"Buka file itu untuk lihat ini webcam yang mana.")

    if not found_any:
        print("Tidak ada webcam terdeteksi sama sekali. Cek koneksi USB / driver.")

    print("\nSetelah lihat foto-fotonya dan cocokkan dengan nama device di "
          "atas, set CAMERA_NAME_TOP dan CAMERA_NAME_SIDE di bagian atas "
          "file ini dengan potongan nama yang unik untuk tiap webcam "
          "eksternal (jangan yang juga cocok ke kamera laptop bawaan).")


if __name__ == "__main__":
    if "--list-cameras" in sys.argv:
        list_cameras_and_exit()
        sys.exit(0)

    local_ip = get_local_ip()
    zeroconf_instance = start_mdns(local_ip)

    print()
    print("========================================")
    print("CAMERA BRIDGE SERVER - 2 WEBCAM")
    print("========================================")
    for key, cam in CAMERAS.items():
        ready = "SIAP" if cam.is_ok() else "TIDAK TERBACA (cek USB/nama device!)"
        print(f"  {cam.unit_label:8s} ({key}.local) <- index {cam.camera_index}: {ready}")
    print(f"IP laptop: {local_ip}")
    print(f"Flask jalan di port internal {INTERNAL_PORT} (bukan 80 -- "
          f"lihat REVISI #2 di docstring, port 80 di-handle portproxy).")
    print("Kalau ESP32 MAIN belum bisa connect ke camtop.local/camside.local, "
          "pastikan setup_admin_onetime.bat sudah pernah dijalankan SEBAGAI "
          "ADMIN di komputer ini (cukup sekali).")
    print("========================================")
    print()

    try:
        # host="0.0.0.0" -> bisa diakses dari device lain di jaringan
        # yang sama (ESP32 MAIN, lewat portproxy port 80 -> INTERNAL_PORT).
        # port=INTERNAL_PORT -> port bebas (>1024), TIDAK butuh admin.
        app.run(host="0.0.0.0", port=INTERNAL_PORT, threaded=True)
    finally:
        # Cabut pengumuman mDNS kalau script ditutup (Ctrl+C), supaya
        # camtop.local/camside.local tidak "nyangkut" nunjuk ke laptop
        # yang sudah tidak jalan lagi.
        zeroconf_instance.close()
