// lepton_vcam.exe: adds / removes the "Lepton Thermal" camera for the current user. Does not need admin rights
// (the media source DLL must have been registered by install.ps1 first).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../shared/LeptonVCamId.h"

using Microsoft::WRL::ComPtr;

static int Fail(const wchar_t* what, HRESULT hr)
{
	wchar_t text[512] = L"";
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0, text, _countof(text), nullptr);
	for (auto p = text; *p; p++) if (*p == L'\r' || *p == L'\n') *p = L' ';
	wprintf(L"FAILED %s hr=0x%08X %s\n", what, hr, text);
	if (hr == E_ACCESSDENIED)
		wprintf(L"hint: check that install.ps1 was run as admin, and Settings > Privacy & security > Camera (camera access, desktop apps)\n");
	return 1;
}

static std::wstring RegisteredDll()
{
	wchar_t path[MAX_PATH] = L"";
	DWORD size = sizeof(path);
	RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" LEPTON_VCAM_CLSID_STRING L"\\InprocServer32", nullptr, RRF_RT_REG_SZ, nullptr, path, &size);
	return path;
}

static HRESULT Create(MFVirtualCameraLifetime lifetime, IMFVirtualCamera** camera, const wchar_t* name = LEPTON_VCAM_NAME)
{
	return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, lifetime, MFVirtualCameraAccess_CurrentUser,
		name, LEPTON_VCAM_CLSID_STRING, nullptr, 0, camera);
}

// cameras added by v1.0 are called "FLIR Lepton"
static void RemoveOldName()
{
	ComPtr<IMFVirtualCamera> camera;
	if (SUCCEEDED(Create(MFVirtualCameraLifetime_System, &camera, LEPTON_VCAM_OLD_NAME)) && SUCCEEDED(camera->Remove()))
		wprintf(L"removed the old camera name \"%s\"\n", LEPTON_VCAM_OLD_NAME);
}

static HRESULT ListCameras(std::vector<std::wstring>& names)
{
	ComPtr<IMFAttributes> attributes;
	auto hr = MFCreateAttributes(&attributes, 1);
	if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	IMFActivate** devices = nullptr;
	UINT32 count = 0;
	if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
	if (FAILED(hr)) return hr;

	for (UINT32 i = 0; i < count; i++)
	{
		wchar_t* name = nullptr;
		UINT32 length;
		if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)))
			names.push_back(name);
		CoTaskMemFree(name);
		devices[i]->Release();
	}
	CoTaskMemFree(devices);
	return S_OK;
}

static int Status()
{
	auto dll = RegisteredDll();
	auto present = !dll.empty() && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
	wprintf(L"dll=%s%s\n", dll.empty() ? L"(not registered)" : dll.c_str(), dll.empty() || present ? L"" : L" (file missing)");
	std::vector<std::wstring> names;
	auto hr = ListCameras(names);
	if (FAILED(hr)) return Fail(L"MFEnumDeviceSources", hr);

	auto found = false;
	wchar_t board[128] = L"";
	DWORD boardSize = sizeof(board);
	RegGetValueW(HKEY_LOCAL_MACHINE, LEPTON_VCAM_REG_KEY, LEPTON_VCAM_REG_BOARD, RRF_RT_REG_SZ, nullptr, board, &boardSize);
	wprintf(L"board=%s\n", *board ? board : L"(not configured: run install.ps1 -Board)");
	for (auto& name : names)
	{
		wprintf(L"camera: %s\n", name.c_str());
		found |= name.rfind(LEPTON_VCAM_NAME, 0) == 0;
	}
	wprintf(L"lepton_camera=%s\n", found ? L"listed" : L"not listed");
	return found ? 0 : 2;
}

// FTDI devices as the camera DLL sees them (ftd2xx.dll), so the administrator can pick the -Board name
static int Boards()
{
	auto module = LoadLibraryExW(L"ftd2xx.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!module)
	{
		wprintf(L"FTDI D2XX driver not installed (ftd2xx.dll missing): plug in the board and let Windows Update install it\n");
		return 1;
	}
	auto createList = (ULONG(WINAPI*)(DWORD*))GetProcAddress(module, "FT_CreateDeviceInfoList");
	auto getDetail = (ULONG(WINAPI*)(DWORD, DWORD*, DWORD*, DWORD*, DWORD*, char*, char*, void**))GetProcAddress(module, "FT_GetDeviceInfoDetail");
	DWORD count = 0;
	if (!createList || !getDetail || createList(&count)) { wprintf(L"FAILED to list FTDI devices\n"); return 1; }

	static const wchar_t* types[] = { L"BM", L"AM", L"100AX", L"unknown", L"2232C", L"232R", L"2232H", L"4232H", L"232H", L"X-series" };
	for (DWORD i = 0; i < count; i++)
	{
		DWORD flags = 0, type = 0, id = 0, location = 0;
		char serial[64] = "", description[128] = "";
		void* handle;
		if (getDetail(i, &flags, &type, &id, &location, serial, description, &handle)) continue;
		auto mpsse = type == 6 || type == 7 || type == 8;
		wprintf(L"board %u: \"%S\"  chip=FT%s%s%s\n", i, description, type < _countof(types) ? types[type] : L"?",
			(flags & 1) ? L"  (in use by a program: details hidden)" : L"", !(flags & 1) && !mpsse ? L"  (cannot be used: no MPSSE)" : L"");
	}
	if (!count) wprintf(L"no FTDI device connected\n");
	return 0;
}

static int Install(bool session)
{
	BOOL supported = FALSE;
	auto hr = MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported);
	if (FAILED(hr) || !supported) return Fail(L"virtual cameras are not supported on this PC", FAILED(hr) ? hr : E_NOTIMPL);
	if (RegisteredDll().empty())
	{
		wprintf(L"FAILED the camera DLL is not registered, run install.ps1 as admin first\n");
		return 1;
	}

	RemoveOldName();
	ComPtr<IMFVirtualCamera> camera;
	hr = Create(session ? MFVirtualCameraLifetime_Session : MFVirtualCameraLifetime_System, &camera);
	if (FAILED(hr)) return Fail(L"MFCreateVirtualCamera", hr);
	hr = camera->Start(nullptr);
	if (FAILED(hr)) return Fail(L"IMFVirtualCamera::Start", hr);

	if (!session)
	{
		wprintf(L"OK installed \"%s\" for this user\n", LEPTON_VCAM_NAME);
		return 0;
	}

	wprintf(L"OK \"%s\" is available until this program ends. Press Enter to stop.\n", LEPTON_VCAM_NAME);
	fflush(stdout);
	getchar();
	camera->Remove();
	return 0;
}

static int Remove()
{
	RemoveOldName();
	ComPtr<IMFVirtualCamera> camera;
	auto hr = Create(MFVirtualCameraLifetime_System, &camera);
	if (FAILED(hr)) return Fail(L"MFCreateVirtualCamera", hr);
	hr = camera->Remove();
	if (FAILED(hr)) return Fail(L"IMFVirtualCamera::Remove", hr);
	wprintf(L"OK removed \"%s\" for this user\n", LEPTON_VCAM_NAME);
	return 0;
}

int wmain(int argc, wchar_t** argv)
{
	std::wstring command = argc > 1 ? argv[1] : L"";
	if (command == L"boards")
		return Boards();
	if (command != L"install" && command != L"remove" && command != L"status" && command != L"session")
	{
		wprintf(L"usage: lepton_vcam install | remove | status | session | boards\n"
			L"  install  add the camera for this user (stays until removed)\n"
			L"  remove   remove it\n"
			L"  status   show the registered DLL and the cameras Windows lists\n"
			L"  session  add the camera only while this program runs\n"
			L"  boards   list the FTDI boards, to choose install.ps1 -Board\n");
		return 1;
	}

	auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hr)) return Fail(L"CoInitializeEx", hr);
	hr = MFStartup(MF_VERSION);
	if (FAILED(hr)) return Fail(L"MFStartup", hr);
	auto result = command == L"install" ? Install(false) : command == L"session" ? Install(true) : command == L"remove" ? Remove() : Status();
	MFShutdown();
	CoUninitialize();
	return result;
}
