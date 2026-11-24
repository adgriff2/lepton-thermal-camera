// lepton_vcam.exe: adds / removes the "FLIR Lepton" camera for the current user. Does not need admin rights
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

static HRESULT Create(MFVirtualCameraLifetime lifetime, IMFVirtualCamera** camera)
{
	return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, lifetime, MFVirtualCameraAccess_CurrentUser,
		LEPTON_VCAM_NAME, LEPTON_VCAM_CLSID_STRING, nullptr, 0, camera);
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
	for (auto& name : names)
	{
		wprintf(L"camera: %s\n", name.c_str());
		found |= name.rfind(LEPTON_VCAM_NAME, 0) == 0;
	}
	wprintf(L"lepton_camera=%s\n", found ? L"listed" : L"not listed");
	return found ? 0 : 2;
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
	if (command != L"install" && command != L"remove" && command != L"status" && command != L"session")
	{
		wprintf(L"usage: lepton_vcam install | remove | status | session\n"
			L"  install  add the camera for this user (stays until removed)\n"
			L"  remove   remove it\n"
			L"  status   show the registered DLL and the cameras Windows lists\n"
			L"  session  add the camera only while this program runs\n");
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
