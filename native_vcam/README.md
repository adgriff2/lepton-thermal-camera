# FLIR Lepton as a native Windows 11 camera

Shows the live thermal image in the Windows Camera app (and any other camera app) as **FLIR Lepton**, without OBS.
Based on Simon Mourier's [VCamSample](https://github.com/smourier/VCamSample) (MIT, see `LICENSE`).

The camera DLL reads the Lepton itself, only while an app uses the camera. No program has to be started:
```
FT2232H + Lepton 3 <──SPI──> LeptonVCamSource.dll (inside Windows Frame Server) ──> Camera app
                                        ^ optional: frames from a program have priority
lepton_cam.py --nativecam ──writes──> C:\ProgramData\LeptonVCam\feed\frame.bin
```
- `LeptonVCamSource/LeptonCapture.cpp`: capture, a C++ port of `Grabber` and `render()` in `lepton_cam.py` (same SPI commands),
  plus bit realignment: the stream often starts some bits off a packet boundary ("silent", `runs=0`), which is found with CRCs.
- Messages on the picture: STARTING, BOARD NOT CONNECTED, BOARD IN USE (by another program), FTDI DRIVER NOT FOUND,
  NO DATA FROM LEPTON, SEVERAL BOARDS FOUND (only one free "Dual RS232-HS A" FT2232H is accepted), BOARD ERROR n (D2XX status).
- Lepton 3 only, inferno palette. While an app uses the camera, other programs cannot open the board.

## Commands

Run these from the `native_vcam` folder.

Build (normal PowerShell; needs Visual Studio with "Desktop development with C++"):
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Install, once per PC and after each rebuild (**elevated** PowerShell):
```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

Add the camera, once per user (normal PowerShell):
```powershell
& "$env:ProgramData\LeptonVCam\lepton_vcam.exe" install; & "$env:ProgramData\LeptonVCam\lepton_vcam.exe" status
```

Optional, sending frames from a program (test pattern, or the Python viewer with its own palette keys):
```powershell
python .\python\test_pattern.py --seconds 60
python ..\lepton_cam.py --model 3 --nativecam
```

Test a freshly built DLL on the board without installing it (prints the capture counters):
```powershell
& .\dist\vcam_selftest.exe dll .\dist\LeptonVCamSource.dll 10
```

Start-on-demand helper (`autostart.ps1 on | off | status`): the earlier solution, which starts the Python program when the camera
is used. Not needed any more; keep it off, or both will want the board.

Read the camera through Windows without the Camera app (prints fps and the number of distinct frames):
```powershell
& "$env:ProgramData\LeptonVCam\vcam_selftest.exe" cam "FLIR Lepton" 5
```

Package for other x64 Windows 11 PCs (normal PowerShell, after a build). Makes `package\LeptonCamera.zip` (the camera, admin
once per PC) and `package\LeptonViewer.zip` (portable preview window, no admin), each with its own `README.txt`:
```powershell
powershell -ExecutionPolicy Bypass -File .\package.ps1
```

Remove (normal PowerShell, then elevated):
```powershell
& "$env:ProgramData\LeptonVCam\lepton_vcam.exe" remove
powershell -ExecutionPolicy Bypass -File "$env:ProgramData\LeptonVCam\uninstall.ps1"
```

## Notes
- The camera offers 640x480 @ 30 fps in NV12 and RGB32. The last frame repeats between Lepton frames.
  After 2 s without a new frame (from a program or the board) it shows "NO SIGNAL".
- `--nativecam` sends from the preview loop: dragging the preview window pauses the camera image (`--no-preview` avoids it).
- Only one program can send frames at a time.
- `C:\ProgramData\LeptonVCam` is writable by administrators only, because system services load the DLL from there.
  Any signed-in user of the PC can read and write `feed\frame.bin`, so on a shared PC others could see or replace the image.
- `lepton_vcam.exe install` fails with access denied: check that `install.ps1` ran, and
  Settings > Privacy & security > Camera (camera access, and desktop apps).
- `install.ps1` cannot replace the DLL: close every app that uses a camera and run it again.
- Protocol: `shared/LeptonFeed.h`; keep `python/leptonfeed.py` in sync.
