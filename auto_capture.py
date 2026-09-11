"""
AUTO CAPTURE DATASET - 2 WEBCAM
================================
Cara pakai:
1. Colokin 2 webcam eksternal ke laptop (jangan pake webcam bawaan laptop).
2. Jalanin: python auto_capture.py
3. Pertama kali jalan -> mode SETUP: bakal muncul preview tiap kamera yang
   kedeteksi (termasuk kamera bawaan laptop kalau ada). Kamu tinggal pencet:
      1 -> jadiin kamera ini sebagai KAMERA 1
      2 -> jadiin kamera ini sebagai KAMERA 2
      s -> skip (bukan kamera yang dimaksud, misal kamera bawaan laptop)
      q -> batal/keluar
4. Setelah 2 kamera udah dipilih, config otomatis kesimpen di camera_config.json.
   Lain kali jalanin ulang -> LANGSUNG capture otomatis, gak nanya lagi.
5. Mau pilih ulang kamera? jalanin: python auto_capture.py --reset

Hasil capture kesimpen di folder "dataset_capture/cam1" dan "dataset_capture/cam2",
tiap 5 detik, dengan nama file berupa timestamp biar gampang di-pairing.
Berhenti kapan aja dengan CTRL+C.
"""

import cv2
import os
import sys
import json
import time
from datetime import datetime

try:
    from pygrabber.dshow_graph import FilterGraph
    _HAS_PYGRABBER = True
except ImportError:
    _HAS_PYGRABBER = False


def get_camera_names():
    """Ambil nama asli device kamera (Windows only, butuh pip install pygrabber).
    Urutan nama ini sesuai urutan index DirectShow, jadi nama[i] = index i."""
    if not _HAS_PYGRABBER:
        return None
    try:
        graph = FilterGraph()
        return graph.get_input_devices()
    except Exception:
        return None

CONFIG_PATH = "camera_config.json"
OUTPUT_DIR = "dataset_capture"
INTERVAL_SECONDS = 5
MAX_INDEX_SCAN = 8  # scan index kamera 0..7


def open_camera(index):
    """Buka kamera pakai backend DirectShow (paling stabil di Windows)."""
    cap = cv2.VideoCapture(index, cv2.CAP_DSHOW)
    if not cap.isOpened():
        cap.release()
        cap = cv2.VideoCapture(index)  # fallback backend default
    return cap


def scan_available_cameras():
    """Cari semua index kamera yang beneran bisa ngasih frame."""
    found = []
    for i in range(MAX_INDEX_SCAN):
        cap = open_camera(i)
        if cap.isOpened():
            ok, frame = cap.read()
            if ok and frame is not None:
                found.append(i)
        cap.release()
    return found


def setup_cameras():
    print("\n=== MODE SETUP KAMERA ===")
    print("Mencari kamera yang nyambung ke laptop...\n")
    candidates = scan_available_cameras()

    if len(candidates) == 0:
        print("Tidak ada kamera terdeteksi sama sekali. Cek koneksi webcam kamu.")
        sys.exit(1)

    print(f"Ditemukan {len(candidates)} kamera pada index: {candidates}")

    names = get_camera_names()
    if names:
        print("\nNama device yang kedeteksi Windows (urutan sesuai index):")
        for idx in candidates:
            nm = names[idx] if idx < len(names) else "(nama tidak diketahui)"
            print(f"  Index {idx}: {nm}")
    else:
        print("\n[Info] Nama device gak kedeteksi otomatis. Biar lebih akurat, jalankan:")
        print("  pip install pygrabber")
        print("terus jalankan ulang script ini.")

    print("\nSekarang akan ditampilkan preview satu per satu.\n")

    selected = {"cam1": None, "cam2": None}

    print("PENTING: SEMUA kamera yang kedeteksi akan ditampilkan satu-satu,")
    print("walaupun CAM1/CAM2 udah keisi duluan (biar gak ada yang kelewat).")
    print("Kalau salah pencet, tinggal pencet ulang di kamera yang lain, nanti otomatis timpa.\n")

    for idx in candidates:
        cap = open_camera(idx)
        if not cap.isOpened():
            continue

        cam_name = names[idx] if (names and idx < len(names)) else f"index {idx}"
        print(f"--- Menampilkan preview kamera index {idx}  |  Nama: {cam_name} ---")
        print("Tekan: [1]=jadikan KAMERA 1  [2]=jadikan KAMERA 2  [s]=skip  [q]=keluar")

        chosen_key = None
        while True:
            ok, frame = cap.read()
            if not ok:
                break

            label = f"Index {idx}: {cam_name}"
            label2 = "1=CAM1  2=CAM2  s=skip  q=quit"
            display = frame.copy()
            cv2.putText(display, label, (10, 30), cv2.FONT_HERSHEY_SIMPLEX,
                        0.7, (0, 255, 0), 2)
            cv2.putText(display, label2, (10, 60), cv2.FONT_HERSHEY_SIMPLEX,
                        0.7, (0, 255, 0), 2)
            cv2.imshow("Setup Kamera - Identifikasi", display)

            key = cv2.waitKey(1) & 0xFF
            if key == ord('1'):
                chosen_key = 'cam1'
                break
            elif key == ord('2'):
                chosen_key = 'cam2'
                break
            elif key == ord('s'):
                chosen_key = None
                break
            elif key == ord('q'):
                cap.release()
                cv2.destroyAllWindows()
                print("Setup dibatalkan.")
                sys.exit(0)

        cap.release()
        cv2.destroyAllWindows()

        if chosen_key is not None:
            if selected[chosen_key] is not None:
                print(f"  -> Slot {chosen_key} udah dipakai index {selected[chosen_key]}, diganti ke index {idx}.")
            selected[chosen_key] = idx
            print(f"  -> Index {idx} disimpan sebagai {chosen_key.upper()}\n")
        else:
            print(f"  -> Index {idx} dilewati.\n")

    if selected["cam1"] is None or selected["cam2"] is None:
        print("Kamu belum menentukan 2 kamera (CAM1 dan CAM2). Jalankan ulang script untuk setup lagi.")
        sys.exit(1)

    print("=== RINGKASAN PILIHAN ===")
    print(f"CAM1 -> index {selected['cam1']}")
    print(f"CAM2 -> index {selected['cam2']}")
    confirm = input("Sudah benar? (y = lanjut, n = batal & jalankan ulang script): ").strip().lower()
    if confirm != 'y':
        print("Dibatalkan. Jalankan ulang 'python auto_capture.py --reset' untuk setup ulang.")
        sys.exit(0)

    config = build_config_with_names(selected, names)

    with open(CONFIG_PATH, "w") as f:
        json.dump(config, f, indent=2)

    print(f"Config kamera disimpan ke {CONFIG_PATH}: {config}\n")
    return config


def build_config_with_names(selected, names):
    """Simpan bukan cuma index, tapi juga NAMA device + urutan kemunculannya
    (occurrence), supaya kalau index berubah lain kali dijalankan (ini SERING
    terjadi di Windows), script tetap bisa nemuin kamera yang benar dari namanya."""
    config = {}
    for key in ("cam1", "cam2"):
        idx = selected[key]
        if names and idx < len(names):
            name = names[idx]
            occurrence = names[:idx + 1].count(name) - 1  # keberapa kali nama ini muncul sampai idx ini
        else:
            name = None
            occurrence = 0
        config[key] = {"index": idx, "name": name, "occurrence": occurrence}
    return config


def resolve_index(entry, current_names):
    """Cari index kamera SEKARANG berdasarkan nama + urutan kemunculan yang disimpan.
    Kalau nama gak ada / gak kedeteksi, fallback ke index lama (kurang stabil)."""
    name = entry.get("name")
    occurrence = entry.get("occurrence", 0)
    if name and current_names:
        matches = [i for i, n in enumerate(current_names) if n == name]
        if occurrence < len(matches):
            return matches[occurrence], True
        elif matches:
            return matches[0], True
    return entry["index"], False


def load_config():
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, "r") as f:
            return json.load(f)
    return None


def capture_loop(config):
    current_names = get_camera_names()

    cam1_entry = config["cam1"]
    cam2_entry = config["cam2"]

    cam1_idx, cam1_ok = resolve_index(cam1_entry, current_names)
    cam2_idx, cam2_ok = resolve_index(cam2_entry, current_names)

    if not cam1_ok or not cam2_ok:
        print("[!] PERINGATAN: gak bisa mencocokkan nama device kamera sekarang,")
        print("    jadi terpaksa pakai index lama yang mungkin sudah gak sesuai.")
        print("    Kalau capture-nya salah kamera, jalankan: python auto_capture.py --reset\n")

    if cam1_idx == cam2_idx:
        print(f"[!] CAM1 dan CAM2 kebetulan mengarah ke index yang sama ({cam1_idx}).")
        print("    Ini kemungkinan besar salah. Jalankan: python auto_capture.py --reset\n")
        sys.exit(1)

    cap1 = open_camera(cam1_idx)
    cap2 = open_camera(cam2_idx)

    if not cap1.isOpened() or not cap2.isOpened():
        print("Gagal membuka salah satu kamera. Cek koneksi, atau jalankan ulang dengan --reset.")
        sys.exit(1)

    dir1 = os.path.join(OUTPUT_DIR, "cam1")
    dir2 = os.path.join(OUTPUT_DIR, "cam2")
    os.makedirs(dir1, exist_ok=True)
    os.makedirs(dir2, exist_ok=True)

    print(f"=== MULAI CAPTURE OTOMATIS (tiap {INTERVAL_SECONDS} detik) ===")
    print(f"CAM1 = index {cam1_idx}  (nama: {cam1_entry.get('name')})  ->  disimpan ke {dir1}")
    print(f"CAM2 = index {cam2_idx}  (nama: {cam2_entry.get('name')})  ->  disimpan ke {dir2}")
    print("Tekan CTRL+C untuk berhenti.\n")

    count = 0
    try:
        while True:
            ok1, frame1 = cap1.read()
            ok2, frame2 = cap2.read()

            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")

            if ok1 and frame1 is not None:
                path1 = os.path.join(dir1, f"cam1_{timestamp}.jpg")
                cv2.imwrite(path1, frame1)
            else:
                path1 = None
                print("  [!] Gagal ambil frame dari CAM1")

            if ok2 and frame2 is not None:
                path2 = os.path.join(dir2, f"cam2_{timestamp}.jpg")
                cv2.imwrite(path2, frame2)
            else:
                path2 = None
                print("  [!] Gagal ambil frame dari CAM2")

            count += 1
            print(f"[{count}] {timestamp} -> {path1} | {path2}")

            # countdown biar keliatan hidup, sambil tetep total nunggu 5 detik
            for remaining in range(INTERVAL_SECONDS, 0, -1):
                print(f"  capture berikutnya dalam {remaining} detik...", end="\r")
                time.sleep(1)
            print(" " * 50, end="\r")  # bersihin baris countdown

    except KeyboardInterrupt:
        print("\n\nDihentikan oleh user. Total capture: ", count)
    finally:
        cap1.release()
        cap2.release()
        cv2.destroyAllWindows()


def main():
    if "--reset" in sys.argv and os.path.exists(CONFIG_PATH):
        os.remove(CONFIG_PATH)
        print("Config lama dihapus. Akan setup ulang.\n")

    config = load_config()

    # deteksi format config lama (versi sebelum ada nama device) -> minta reset
    if config is not None:
        try:
            is_old_format = not isinstance(config["cam1"], dict)
        except Exception:
            is_old_format = True
        if is_old_format:
            print("Format camera_config.json kamu masih versi lama.")
            print("Jalankan ulang dengan: python auto_capture.py --reset\n")
            sys.exit(1)

    if config is None:
        config = setup_cameras()

    capture_loop(config)


if __name__ == "__main__":
    main()
