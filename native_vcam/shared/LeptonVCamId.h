#pragma once
// identity shared by the media source DLL and the tools
// {5d5c12fe-863c-4d8a-8b21-79f4a8b9419e}
#define LEPTON_VCAM_CLSID_INIT { 0x5d5c12fe, 0x863c, 0x4d8a, { 0x8b, 0x21, 0x79, 0xf4, 0xa8, 0xb9, 0x41, 0x9e } }
#define LEPTON_VCAM_CLSID_STRING L"{5d5c12fe-863c-4d8a-8b21-79f4a8b9419e}"
#define LEPTON_VCAM_NAME L"Lepton Thermal"
#define LEPTON_VCAM_OLD_NAME L"FLIR Lepton" // name used up to v1.0, removed when the camera is added again
#define LEPTON_VCAM_REG_KEY L"SOFTWARE\\LeptonVCam"     // HKLM, written by install.ps1 (admin only)
#define LEPTON_VCAM_REG_BOARD L"Board"                  // description of the FTDI board to use
