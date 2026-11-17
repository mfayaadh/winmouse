# winmouse

[English](README.md)

Akselerasi dan sensitivitas pointer Windows 11 yang presisi untuk Linux / Omarchy (Hyprland) — hasil *reverse engineering* dari `win32kbase.sys` dan terverifikasi bit-per-bit pada **600.000 paket** langsung terhadap kode mesin Microsoft (lihat [docs/REVERSE-ENGINEERING.md](docs/REVERSE-ENGINEERING.md)).

---

## Latar Belakang Masalah (Dual-Boot)

Saya menjalankan **dual OS (Windows 11 dan Linux / Omarchy)** di laptop yang sama untuk dua aktivitas pekerjaan berbeda — satu pekerjaan membutuhkan ekosistem Windows, sementara pekerjaan *software engineering* dan server berjalan di Linux / Omarchy (Hyprland).

Setiap kali berganti OS di laptop yang sama, *feeling* kursor mouse selalu terasa jauh berbeda. Windows menghitung pergerakan pointer dengan slider sensitivitas 1–20 serta kurva **"Enhance pointer precision"** (`EPP`) berbasis aritmetika *fixed-point* 16.16 per paket HID, sedangkan Linux (`libinput`) memakai kurva akselerasi yang benar-benar berbeda. Berulang kali berpindah OS terasa memusingkan dan memaksa penyesuaian ulang setiap sesi.

Menebak kurva `libinput` secara manual tidak pernah menyamai rasa Windows yang sebenarnya. Karena itu, `winmouse` merekonstruksi langsung fungsi akselerasi Windows 11 dari `win32kbase.sys`, memverifikasinya identik bit-per-bit di emulator CPU Unicorn, lalu membungkusnya menjadi daemon C ringan berbasis `evdev` → `uinput`.

---

## Cara Kerja

`winmouse` tidak menimpa konfigurasi mouse Linux Anda. Tersedia dua mode yang bisa diganti kapan saja:

- **Mode Linux (daemon berhenti):** mouse langsung masuk ke `libinput` memakai konfigurasi normal `~/.config/hypr/input.lua`.
- **Mode Windows 11 (daemon aktif):** mengambil event khusus mouse USB & Bluetooth (`ID_INPUT_MOUSE=1`), menghitung pergerakan per paket laporan HID memakai aritmetika integer 16.16 persis seperti Windows (termasuk *segment-transition gain averaging* dan sisa *subpixel*), lalu mengirim ulang melalui perangkat virtual `WinMouse Virtual Pointer` agar Hyprland meneruskannya secara datar `1:1` (`flat`).

Keamanan & isolasi:
- Touchpad bawaan laptop (I2C) tidak pernah disentuh.
- Keyboard tetap terisolasi (`70-winmouse.rules` memakai tag `uaccess` hanya untuk `/dev/uinput` dan node `event*` mouse milik sesi aktif — tidak butuh grup `input`).
- Jika daemon dihentikan atau berhenti mendadak, kernel otomatis melepaskan grab sehingga perilaku Linux langsung kembali normal.

---

## Instalasi & Penggunaan

```sh
./install.sh
```

Setelah terpasang, ubah mode lewat menu **Omarchy (Setup → Mouse Sensitivity)** atau lewat terminal:

```sh
winmouse-ctl mode windows      # Aktifkan rasa pointer Windows 11
winmouse-ctl mode linux        # Kembali ke default Hyprland / libinput
winmouse-ctl toggle            # Ganti mode cepat
winmouse-ctl speed 8           # Atur slider sensitivitas Windows (1–20)
winmouse-ctl epp on|off|toggle # Aktif/nonaktifkan Enhance pointer precision
winmouse-ctl status            # Cek mode dan konfigurasi aktif
```

Konfigurasi tersimpan di `~/.config/winmouse/config` dan langsung dimuat ulang (`SIGHUP`) tanpa memutus mouse.

## Verifikasi & Lisensi

```sh
gcc -O2 -Wall -Wextra -o build/winmouse src/winmouse.c
uv run --with unicorn python verify/verify_against_windows.py /path/to/win32kbase.sys build/winmouse
```

Lisensi: [MIT](LICENSE).
