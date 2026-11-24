# Lepton Thermal: a native Windows 11 camera for the FLIR Lepton

Shows the live thermal image of a FLIR Lepton 3 as the camera **Lepton Thermal**, in the Windows Camera app and in any other
camera app (browsers, Teams, Zoom), without OBS. Based on Simon Mourier's [VCamSample](https://github.com/smourier/VCamSample)
(MIT, see `LICENSE`).

The camera DLL reads the Lepton itself, only while an app uses the camera. No program has to be started:
```
FT2232H + Lepton 3 <──SPI──> LeptonVCamSource.dll (inside Windows Frame Server) ──> camera apps
                                        ^ optional: frames from a program have priority
lepton_cam.py --nativecam ──writes──> C:\ProgramData\LeptonVCam\feed\frame.bin
```
- `LeptonVCamSource/LeptonCapture.cpp`: the capture, a C++ port of `Grabber` and `render()` in `../lepton_cam.py` (same SPI
  commands), including bit realignment: the stream often starts some bits off a packet boundary, which is found with CRCs.
- Messages on the picture: STARTING, NO BOARD CONFIGURED, BOARD NOT CONNECTED, BOARD IN USE (by another program),
  FTDI DRIVER NOT FOUND, NO DATA FROM LEPTON, SEVERAL BOARDS FOUND, BOARD ERROR n (an FTDI D2XX status code).
- Lepton 3 only, inferno palette. While an app uses the camera, other programs cannot open the board.

Not affiliated with or endorsed by Teledyne FLIR. FLIR and Lepton are trademarks of Teledyne FLIR LLC.

## Hardware

Tested with the **FLIR Lepton Dev Kit 1** (board 250-0588, R100): an FTDI FT2232H wired to a Lepton 3 socket, powered and
connected by one micro USB cable. Windows lists it as two FTDI devices, "Dual RS232-HS A" and "Dual RS232-HS B".

- Channel A carries the video (SPI mode 3 at 10 MHz): clock on ADBUS0, Lepton data out on ADBUS2, chip select on ADBUS3.
  Other FT2232H/FT232H boards wired the same way should work, but are untested.
- Channel B is the Lepton's I2C control bus. It isn't used; never configure it as the camera's board.
- Lepton 3 only (160x120). Lepton 1/2 work in the viewer (`--model 2`), not in the camera.
- Every kit reports the same names, so with two kits plugged in the camera shows SEVERAL BOARDS FOUND: plug in one.
- The FTDI D2XX driver (`ftd2xx.dll`). Windows Update normally installs it when the board is plugged in.

## Install

1. Build, or download `LeptonCamera.zip` from the Releases. Building needs Visual Studio with
   "Desktop development with C++"; run from this folder (normal PowerShell):
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build.ps1
   ```
2. Find your board's name (normal PowerShell, from `dist` or the unzipped folder):
   ```powershell
   .\lepton_vcam.exe boards
   ```
3. Install once per PC, giving that name (**elevated** PowerShell). Run it again after each rebuild; the board name is kept.
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install.ps1 -Board "Dual RS232-HS A"
   ```
   FTDI's factory default names such as "Dual RS232-HS A" are shared by many devices (debug adapters, other FT2232H
   boards). The camera only uses the board named here and never guesses, but it can't tell two boards with the same name
   apart: if another device with that name is connected, the camera shows SEVERAL BOARDS FOUND and doesn't touch either.
4. Add the camera, once per user (normal PowerShell):
   ```powershell
   & "$env:ProgramData\LeptonVCam\lepton_vcam.exe" install; & "$env:ProgramData\LeptonVCam\lepton_vcam.exe" status
   ```

## Other commands

Sending frames from a program instead of the board (a test pattern, or the Python viewer with its own palette keys):
```powershell
python .\python\test_pattern.py --seconds 60
python ..\lepton_cam.py --model 3 --nativecam
```

Testing a freshly built DLL on the board without installing it (prints the capture counters; no admin):
```powershell
$env:LEPTON_VCAM_BOARD = "Dual RS232-HS A"; & .\dist\vcam_selftest.exe dll .\dist\LeptonVCamSource.dll 10
```

Reading the installed camera through Windows without a camera app (prints fps and the number of distinct frames):
```powershell
& "$env:ProgramData\LeptonVCam\vcam_selftest.exe" cam "Lepton Thermal" 5
```

Packaging for other x64 Windows 11 PCs, after a build. Makes `package\LeptonCamera.zip` (the camera, admin once per PC) and
`package\LeptonViewer.zip` (portable viewer window, no admin), each with its own `README.txt`:
```powershell
powershell -ExecutionPolicy Bypass -File .\package.ps1
```

Removing everything (normal PowerShell, then elevated):
```powershell
& "$env:ProgramData\LeptonVCam\lepton_vcam.exe" remove
powershell -ExecutionPolicy Bypass -File "$env:ProgramData\LeptonVCam\uninstall.ps1"
```

## Security

- The files are not signed. SmartScreen may warn, and Smart App Control or company policy may block them.
- The camera DLL runs inside the Windows camera service (Frame Server, as Local Service). `C:\ProgramData\LeptonVCam` and
  the board setting (`HKLM\SOFTWARE\LeptonVCam`) can only be changed by administrators.
- Any signed-in user of the PC can read and write `C:\ProgramData\LeptonVCam\feed\frame.bin`, so on a shared PC other
  users could see or replace the picture sent by a program. The DLL treats that file as untrusted input.

## Notes
- The camera offers 640x480 at 30 fps in NV12 and RGB32. The last frame repeats between Lepton frames (about 9 per second).
  After 2 s without a new frame it shows NO SIGNAL with the reason.
- `--nativecam` sends from the preview loop: dragging the preview window pauses the camera picture (`--no-preview` avoids it).
- Only one program can send frames at a time.
- `lepton_vcam.exe install` fails with access denied: check that `install.ps1` ran, and
  Settings > Privacy & security > Camera (camera access, and desktop apps).
- `install.ps1` cannot replace the DLL: close every app that uses a camera and run it again.
- Cameras added by v1.0 were called "FLIR Lepton"; `lepton_vcam.exe install` removes that name.
- Frame feed protocol: `shared/LeptonFeed.h`; keep `python/leptonfeed.py` in sync.
