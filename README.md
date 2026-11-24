# FLIR Lepton thermal camera for Windows 11

Live thermal video from a FLIR Lepton 3 on an FTDI FT2232H board (MPSSE SPI).

- **Native Windows camera** ([native_vcam](native_vcam/README.md)): shows up as "FLIR Lepton" in the Windows Camera app,
  browsers, Teams, Zoom, without OBS. The camera driver reads the board itself while an app uses the camera.
  Needs administrator rights once per PC.
- **Viewer** ([lepton_cam.py](lepton_cam.py)): a preview window (palettes, flip, mirror), no installation needed.
  It can also feed the native camera (`--nativecam`) or OBS Virtual Camera (`--vcam`).

Ready-made downloads are attached to the [Releases](../../releases): `LeptonCamera.zip` (the camera) and
`LeptonViewer.zip` (the viewer, no Python needed). Each contains a `README.txt`.

The native camera is based on Simon Mourier's [VCamSample](https://github.com/smourier/VCamSample) (MIT, see
`native_vcam/LICENSE`).
