# Lepton Thermal: FLIR Lepton 3 thermal camera for Windows 11

Live thermal video from a FLIR Lepton 3 on an FTDI FT2232H board (MPSSE SPI).

- **Native Windows camera** ([native_vcam](native_vcam/README.md)): shows up as "Lepton Thermal" in the Windows Camera app,
  browsers, Teams and Zoom, without OBS. The camera reads the board itself while an app uses it.
  Needs administrator rights once per PC.
- **Viewer** ([lepton_cam.py](lepton_cam.py)): a preview window (palettes, flip, mirror), no installation and no admin.
  It can also feed the native camera (`--nativecam`) or OBS Virtual Camera (`--vcam`).

Ready-made downloads are in the [Releases](../../releases): `LeptonCamera.zip` (the camera) and `LeptonViewer.zip` (the
viewer, no Python needed). Each contains a `README.txt`. Wiring and installation: [native_vcam/README.md](native_vcam/README.md).

Running the viewer from source: `pip install ftd2xx numpy opencv-python`, then `python lepton_cam.py --model 3`
(keys: q quit, p palette, f flip, m mirror).

Not affiliated with or endorsed by Teledyne FLIR. FLIR and Lepton are trademarks of Teledyne FLIR LLC.

MIT License, see [LICENSE](LICENSE). The native camera is based on Simon Mourier's
[VCamSample](https://github.com/smourier/VCamSample) (MIT, see `native_vcam/LICENSE`).
