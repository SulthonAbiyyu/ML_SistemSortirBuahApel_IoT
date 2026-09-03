@echo off
REM ============================================================
REM SETUP ADMIN SEKALI SAJA -- untuk Camera Bridge Server
REM ============================================================
REM Jalankan file ini SEBAGAI ADMINISTRATOR (klik kanan ->
REM "Run as administrator") HANYA SEKALI per komputer, sebelum
REM pertama kali pakai camera_bridge_server.exe di komputer itu.
REM
REM Setelah ini dijalankan sukses, .exe-nya BOLEH didobel-klik
REM biasa selamanya -- TIDAK perlu admin lagi, TIDAK perlu
REM utak-atik Windows Defender manual lagi.
REM
REM Yang dilakukan script ini:
REM   1. Membuka port 8080 di Windows Firewall (port internal
REM      yang dipakai Flask, WAJIB sama dengan INTERNAL_PORT di
REM      camera_bridge_server.py).
REM   2. Redirect semua request masuk ke port 80 -> port 8080 di
REM      komputer ini sendiri (netsh portproxy), supaya ESP32 MAIN
REM      yang manggil "http://camtop.local/..." (otomatis port 80)
REM      tetap nyambung ke Flask yang jalan di port 8080.
REM
REM Kalau nanti INTERNAL_PORT di camera_bridge_server.py diganti
REM angkanya, script ini WAJIB dijalankan ulang dengan angka yang
REM sama (ganti manual di 2 baris "set INTERNAL_PORT=" di bawah).
REM ============================================================

setlocal
set INTERNAL_PORT=8080

echo ============================================================
echo Setup Camera Bridge Server (sekali jalan, sebagai admin)
echo Port internal: %INTERNAL_PORT%
echo ============================================================
echo.

echo [1/2] Menambahkan Windows Firewall rule untuk port %INTERNAL_PORT% ...
netsh advfirewall firewall add rule name="CameraBridgeServer" dir=in action=allow protocol=TCP localport=%INTERNAL_PORT%
if %errorlevel% neq 0 (
    echo GAGAL menambahkan firewall rule. Pastikan file ini dijalankan SEBAGAI ADMINISTRATOR.
    pause
    exit /b 1
)

echo.
echo [2/2] Menghapus portproxy lama (kalau ada) lalu memasang redirect port 80 -^> %INTERNAL_PORT% ...
netsh interface portproxy delete v4tov4 listenport=80 listenaddress=0.0.0.0 >nul 2>&1
netsh interface portproxy add v4tov4 listenport=80 listenaddress=0.0.0.0 connectport=%INTERNAL_PORT% connectaddress=127.0.0.1
if %errorlevel% neq 0 (
    echo GAGAL memasang portproxy. Pastikan file ini dijalankan SEBAGAI ADMINISTRATOR.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo SELESAI. Setup ini permanen (tersimpan walau komputer restart).
echo Sekarang camera_bridge_server.exe bisa didobel-klik BIASA,
echo TANPA "Run as Administrator" lagi.
echo ============================================================
pause
