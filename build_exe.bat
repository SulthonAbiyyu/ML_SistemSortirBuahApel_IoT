@echo off
REM ============================================================
REM  build_exe.bat
REM  Otomatis compile camera_bridge_server.py -> CameraBridgeServer.exe
REM  Jalankan file ini dengan cara double-click di Windows.
REM  Taruh file ini SATU FOLDER dengan camera_bridge_server.py
REM ============================================================

REM PENTING: paksa pindah ke folder tempat file .bat ini berada.
REM Tanpa baris ini, kalau dijalankan "Run as administrator", Windows
REM sering start folder kerja di C:\WINDOWS\System32 (bukan folder
REM file ini) -- akibatnya file camera_bridge_server.py "tidak
REM ketemu" padahal sebenarnya ada, cuma dicari di folder yang salah.
cd /d "%~dp0"

echo ========================================
echo   BUILD CameraBridgeServer.exe
echo   Folder kerja: %cd%
echo ========================================
echo.

REM --- Cek apakah camera_bridge_server.py ada di folder yang sama ---
if not exist "camera_bridge_server.py" (
    echo [GAGAL] File camera_bridge_server.py tidak ditemukan di folder ini.
    echo Pastikan build_exe.bat ditaruh SATU FOLDER dengan camera_bridge_server.py
    echo.
    pause
    exit /b 1
)

REM --- Cek Python terinstall ---
python --version >nul 2>&1
if errorlevel 1 (
    echo [GAGAL] Python tidak ditemukan di PATH.
    echo Install Python dulu dari https://www.python.org/downloads/
    echo Saat install, centang "Add Python to PATH".
    echo.
    pause
    exit /b 1
)

echo [1/4] Mengecek / menginstall dependency yang dibutuhkan...
echo (pyinstaller, opencv-python, flask, zeroconf, pygrabber)

python -m pip --version >nul 2>&1
if errorlevel 1 (
    echo pip belum tersedia di instalasi Python ini, mencoba perbaiki otomatis...
    python -m ensurepip --upgrade
    if errorlevel 1 (
        echo [GAGAL] ensurepip juga gagal. Download get-pip.py manual dari:
        echo https://bootstrap.pypa.io/get-pip.py
        echo lalu jalankan: python get-pip.py
        echo.
        pause
        exit /b 1
    )
)

REM Catatan: "python -m pip install --upgrade pip" SENGAJA tidak dijalankan
REM di sini -- di sebagian instalasi Python (terutama yang ter-install ke
REM folder butuh admin, mis. C:\PythonXXX), langkah upgrade pip itu sendiri
REM bisa gagal separuh jalan dan malah MERUSAK pip yang sudah ada. Install
REM dependency langsung tanpa upgrade pip dulu jauh lebih aman.

python -m pip install pyinstaller opencv-python flask zeroconf pygrabber
if errorlevel 1 (
    echo.
    echo Install gagal -- kemungkinan butuh hak admin untuk folder Python ini.
    echo Mencoba ulang dengan opsi --user ...
    python -m pip install --user pyinstaller opencv-python flask zeroconf pygrabber
    if errorlevel 1 (
        echo.
        echo [GAGAL] Install dependency tetap gagal. Kemungkinan penyebab:
        echo   1. Perlu jalankan build_exe.bat ini SEBAGAI ADMINISTRATOR
        echo      ^(klik kanan -^> Run as administrator^), ATAU
        echo   2. Tidak ada koneksi internet, ATAU
        echo   3. pip di komputer ini rusak -- coba jalankan manual:
        echo      python -m ensurepip --upgrade
        echo.
        pause
        exit /b 1
    )
)

echo.
echo [2/4] Membersihkan hasil build lama (kalau ada)...
if exist "build" rmdir /s /q "build"
if exist "dist" rmdir /s /q "dist"
if exist "CameraBridgeServer.spec" del /q "CameraBridgeServer.spec"

echo.
echo [3/4] Compile jadi satu file exe (bisa makan waktu 1-3 menit)...
python -m PyInstaller --onefile --name CameraBridgeServer camera_bridge_server.py

if not exist "dist\CameraBridgeServer.exe" (
    echo.
    echo [GAGAL] Build tidak menghasilkan exe. Lihat pesan error PyInstaller di atas.
    pause
    exit /b 1
)

echo.
echo [4/4] Menyalin hasil dari dist\ ke folder ini...
copy /y "dist\CameraBridgeServer.exe" "CameraBridgeServer.exe" >nul

echo.
echo ========================================
echo   SELESAI!
echo   File baru: CameraBridgeServer.exe
echo   (di folder yang sama dengan script ini)
echo ========================================
echo.
echo Jalankan CameraBridgeServer.exe yang baru untuk memastikan
echo index kamera ATAS/SAMPING sudah tidak kebalik lagi.
echo.
pause
