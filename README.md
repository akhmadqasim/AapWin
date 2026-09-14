# AapWin — AirPods AAP lewat L2CAP di Windows (eksperimen)

Driver KMDF `AapL2cap` membuka channel L2CAP ke PSM `0x1001` (Apple Accessory
Protocol) pada AirPods yang sudah paired, lalu mengeksposnya ke user mode
sebagai device interface. Tool console `aapctl` bicara protokol AAP di atas
channel itu: handshake, subscribe notifikasi, baca baterai / ear detection /
listening mode, dan kirim perintah (ANC, Transparency, Adaptive, Conversational
Awareness).

Status: **belum diuji di hardware**. Semua yang ada di repo ini hanya sudah
lolos compile (`/W4 /WX` + Code Analysis) dan signing dengan test cert.

## Kenapa perlu driver

Windows tidak mengizinkan user mode membuka L2CAP PSM sembarangan (WinSock
Bluetooth hanya RFCOMM). Saat AirPods dipasangkan, BTHENUM membuat PDO untuk
setiap service UUID yang diiklankan AirPods; UUID AAP
`{74ec2172-0bad-4d01-8f77-997b2be0722a}` muncul di Device Manager sebagai
"AAP Server" tanpa driver. Driver ini menempel ke PDO tersebut (compatible ID
`BTHENUM\{74ec2172-...}`), bertanya ke BTHENUM alamat remote-nya
(`IOCTL_INTERNAL_BTHENUM_GET_DEVINFO`), dan membuka channel via
`BRB_L2CA_OPEN_CHANNEL` ke bthport.

## Struktur repo

```
include/aapl2cap_public.h   GUID device interface + PSM, dipakai driver & tool
driver/AapL2cap/            KMDF driver (turunan sample WDK bthecho/bthcli)
tool/aapctl/                console tool C++20 (Win32, cfgmgr32)
scripts/build.ps1           build semua + salin paket ke out\
scripts/sign-package.ps1    inf2cat + signtool (harus elevated)
out/                        hasil build (di-ignore git)
```

Model I/O driver: satu handle = satu channel L2CAP.

| Operasi user mode | Yang dilakukan driver |
|---|---|
| `CreateFile(interface)` | `BRB_L2CA_OPEN_CHANNEL` ke PSM 0x1001, Create selesai saat channel terbuka |
| `WriteFile` | `BRB_L2CA_ACL_TRANSFER` OUT, seluruh buffer = satu SDU |
| `ReadFile` | `BRB_L2CA_ACL_TRANSFER` IN + `ACL_SHORT_TRANSFER_OK`, satu ReadFile = satu SDU |
| `CloseHandle` | `BRB_L2CA_CLOSE_CHANNEL` (sinkron di `EvtFileClose`) |
| AirPods memutus channel | callback `IndicationRemoteDisconnect` → close channel, ReadFile yang pending gagal dengan `ERROR_DEVICE_NOT_CONNECTED` |

Parameter channel: `CF_ROLE_EITHER | CF_LINK_ENCRYPTED`, MTU keluar
min 48 / preferred = max 672 (`L2CAP_DEFAULT_MTU`), MTU masuk diterima 48..65535,
`IncomingQueueDepth` 10, mode basic (bukan enhanced/ERTM) supaya peer chip H2
yang menolak mode enhanced tetap bisa connect.

## Prasyarat

- Windows 11 x64, AirPods sudah paired (Device Manager menampilkan
  "AAP Server" tanpa driver).
- VS Build Tools 2026 (MSVC v145) + komponen WDK Build Tools.
- Windows SDK + WDK 10.0.28000.x.
- Sertifikat test `CN=AapWin Test` di `Cert:\LocalMachine\My`
  (thumbprint `1AC618F9730121AFA692F49C7E5E20E16F8A5F6B`), sudah di-trust di
  Root dan TrustedPublisher.

## Build

```powershell
$msb = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"

# driver
& $msb E:\AapWin\driver\AapL2cap\AapL2cap.vcxproj /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.28000.0
& $msb E:\AapWin\driver\AapL2cap\AapL2cap.vcxproj /p:Configuration=Debug   /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.28000.0
# tambahkan /p:RunCodeAnalysis=true untuk PREfast

# tool
& $msb E:\AapWin\tool\aapctl\aapctl.vcxproj /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.28000.0
```

Atau sekaligus: `pwsh -File scripts\build.ps1 [-CodeAnalysis]`. Hasil:
`driver\AapL2cap\x64\<cfg>\AapL2cap\{AapL2cap.sys,AapL2cap.inf}` dan
`out\aapctl.exe`; script menyalin paket driver ke `out\AapL2cap\` (Release) dan
`out\Debug\AapL2cap\` (Debug).

## Signing

Private key cert ada di machine store, jadi harus dari PowerShell **Run as
Administrator**:

```powershell
pwsh -File E:\AapWin\scripts\sign-package.ps1 -PackageDir E:\AapWin\out\AapL2cap
pwsh -File E:\AapWin\scripts\sign-package.ps1 -PackageDir E:\AapWin\out\Debug\AapL2cap
```

Script menjalankan `inf2cat /driver:<dir> /os:10_X64`, lalu
`signtool sign /v /sm /s My /sha1 <thumb> /fd sha256 AapL2cap.sys aapl2cap.cat`,
lalu `signtool verify /v /pa` keduanya.

## Install (dilakukan manual oleh user, bukan oleh build)

1. Aktifkan test signing, lalu reboot (elevated):
   `bcdedit /set testsigning on`
2. Install paket (elevated):
   `pnputil /add-driver E:\AapWin\out\AapL2cap\AapL2cap.inf /install`
3. Cek Device Manager → Bluetooth → "AirPods AAP L2CAP Client (experimental)"
   harus menggantikan "AAP Server" tanpa tanda seru. Kalau device masih
   memakai driver lama, klik kanan → Update driver → Browse → arahkan ke
   `out\AapL2cap`, atau Scan for hardware changes.
4. `E:\AapWin\out\aapctl.exe list` → harus mencetak satu path
   `\\?\BTHENUM#{74ec2172-...}#...#{0907fade-e50e-4873-b132-d1be9bddac55}`.
5. Pastikan AirPods sedang connect (audio), lalu `aapctl monitor`.

Uninstall:

```powershell
pnputil /enum-drivers            # cari Published Name oemNN.inf dengan Provider AapWin
pnputil /delete-driver oemNN.inf /uninstall /force
bcdedit /set testsigning off     # opsional, lalu reboot
```

## aapctl

```
aapctl list                          path device interface yang ada
aapctl monitor                       handshake + subscribe, cetak event sampai Ctrl+C
aapctl mode off|anc|transparency|adaptive
aapctl ca on|off                     conversational awareness
aapctl raw <hex bytes>               kirim paket mentah, cetak balasan 2 detik
aapctl decode <hex bytes>            decode paket offline (tanpa device)
```

`monitor` selalu menjaga satu overlapped `ReadFile` pending. Kalau AirPods
memutus channel, ReadFile gagal dengan `ERROR_DEVICE_NOT_CONNECTED` dan tool
keluar.

## Cheat-sheet protokol AAP

Sumber: LibrePods `docs/AAP Definitions.md`.

Urutan awal (dikirim `aapctl`):

```
00 00 04 00 01 00 02 00 00 00 00 00 00 00 00 00   handshake, balasan mulai 01 00 04 00
04 00 04 00 4D 00 FF 00 00 00 00 00 00 00         feature caps (~300 ms kemudian)
04 00 04 00 0F 00 FF FF FF FF                     notification filter (~300 ms kemudian)
04 00 04 00 09 00 34 01 00 00 00                  AllowOffOption
```

Frame masuk semua diawali `04 00 04 00`, opcode u16 LE di byte 4..5:

| Opcode | Isi |
|---|---|
| `0x0006` ear detection | `[6]`=primary `[7]`=secondary; 00 In Ear, 01 Out, 02 In Case, 03 Disconnected |
| `0x0004` battery | `[6]`=count, lalu grup 5 byte `{komponen, 01, level%, status, 01}`; komponen 01 Headset 02 Right 04 Left 08 Case; status 00 unknown 01 charging 02 discharging 04 disconnected 05 optimized; pod pertama = primary |
| `0x0009` control | `[6]`=id `[7]`=value; id `0x0D` listening mode (01 Off 02 ANC 03 Transparency 04 Adaptive), `0x28` conversational awareness (01 on 02 off), `0x1B` one-bud ANC, `0x2E` adaptive noise level |
| `0x004B` CA event | `04 00 04 00 4B 00 02 00 01 [level]` |
| `0x001D` metadata | string UTF-8 null-terminated: name, model, manufacturer, serial, fw, ... |

Perintah keluar: `04 00 04 00 09 00 <id> <value> 00 00 00` — contoh ANC
`04 00 04 00 09 00 0D 02 00 00 00`; AirPods meng-echo paket yang sama sebagai
konfirmasi.

## Risiko

- Driver kernel eksperimental dan belum pernah jalan di hardware: bug di
  path disconnect/cancel bisa berakhir BSOD. Jangan pakai di mesin kerja tanpa
  backup.
- Test signing menonaktifkan sebagian perlindungan signature Windows selama
  aktif; matikan lagi setelah selesai eksperimen.
- Binding ke compatible ID berarti driver ini akan menempel ke semua device
  yang mengiklankan UUID AAP (semua AirPods/Beats yang paired ke PC ini).
- Protokol AAP tidak terdokumentasi resmi; layout paket mengikuti reverse
  engineering LibrePods dan bisa berbeda antar firmware.

## Penyimpangan dari sample WDK bthecho

- SDP lookup (`IOCTL_BTH_SDP_*`, parsing tree) dihapus, PSM hardcoded.
- `BRB_L2CA_OPEN_CHANNEL` basic dipakai, bukan `BRB_L2CA_OPEN_ENHANCED_CHANNEL`
  (sample memakai enhanced + ERTM di Win8+).
- Continuous reader (khusus server) dibuang; ditambah daftar request read/write
  yang sedang in-flight per connection supaya remote disconnect bisa
  membatalkannya (`WdfRequestCancelSentRequest`) dan mengembalikan
  `STATUS_DEVICE_NOT_CONNECTED`.
- Context request bukan lagi `BRB` telanjang tapi `AAPL2CAP_REQUEST_CONTEXT`
  (BRB + list entry + pointer connection).
- `WdfDriverCreate` dipanggil dengan attributes (sample mengisi
  `EvtCleanupCallback` tapi meneruskan NULL, jadi `WPP_CLEANUP` tidak pernah
  dipanggil).
- INF: `Standard.NTamd64.10.0...16299` (InfVerif menolak `NTamd64.10.0` polos
  karena `DefaultDestDir = 13` butuh dekorasi TargetOSVersion ≥ 16299).
