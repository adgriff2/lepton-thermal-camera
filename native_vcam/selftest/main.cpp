// vcam_selftest.exe: reads frames and prints statistics.
//   vcam_selftest dll <path> [seconds] [nv12|rgb32] [out.raw]   load the media source DLL in-process: nothing needs to be registered
//   vcam_selftest cam <name> [seconds] [nv12|rgb32] [out.raw]   read the installed camera through Windows Frame Server
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <stdio.h>
#include <string>
#include <set>
#include "../shared/LeptonVCamId.h"

using Microsoft::WRL::ComPtr;

#define CHECK(expr) do { auto _hr = (expr); if (FAILED(_hr)) { wprintf(L"FAILED %S hr=0x%08X\n", #expr, _hr); return 1; } } while (0)

static HMODULE _dll;

static void PrintCaptureStats()
{
	auto stats = _dll ? (void(WINAPI*)(UINT64*, UINT))GetProcAddress(_dll, "LeptonStats") : nullptr;
	if (!stats) return;
	UINT64 v[18];
	stats(v, 18);
	wprintf(L"capture: frames=%llu runs=%llu seg0-7=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu crc_bad=%llu resyncs=%llu dropped=%llu usb_reads=%llu opens=%llu fails=%llu bit_offset=%llu searches=%llu\n",
		v[11], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[12], v[13], v[0], v[14], v[15], v[16], v[17]);
}

static int OpenDll(const wchar_t* path, ComPtr<IMFMediaSource>& source)
{
	auto module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module) { wprintf(L"FAILED LoadLibrary error=%u\n", GetLastError()); return 1; }
	auto getClassObject = (HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**))GetProcAddress(module, "DllGetClassObject");
	if (!getClassObject) { wprintf(L"FAILED no DllGetClassObject\n"); return 1; }

	_dll = module;
	static const GUID clsid = LEPTON_VCAM_CLSID_INIT;
	ComPtr<IClassFactory> factory;
	CHECK(getClassObject(clsid, IID_PPV_ARGS(&factory)));
	ComPtr<IMFActivate> activate;
	CHECK(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate)));
	CHECK(activate->ActivateObject(IID_PPV_ARGS(&source)));

	// Frame Server normally provides the allocator
	ComPtr<IMFSampleAllocatorControl> control;
	CHECK(source.As(&control));
	DWORD input;
	MFSampleAllocatorUsage usage;
	CHECK(control->GetAllocatorUsage(0, &input, &usage));
	if (usage != MFSampleAllocatorUsage_UsesProvidedAllocator) { wprintf(L"FAILED allocator usage %u\n", usage); return 1; }
	ComPtr<IMFVideoSampleAllocatorEx> allocator;
	CHECK(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator)));
	CHECK(control->SetDefaultAllocator(0, allocator.Get()));
	return 0;
}

static int OpenCamera(const std::wstring& prefix, ComPtr<IMFMediaSource>& source)
{
	ComPtr<IMFAttributes> attributes;
	CHECK(MFCreateAttributes(&attributes, 1));
	CHECK(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
	IMFActivate** devices = nullptr;
	UINT32 count = 0;
	CHECK(MFEnumDeviceSources(attributes.Get(), &devices, &count));
	for (UINT32 i = 0; i < count; i++)
	{
		wchar_t* name = nullptr;
		UINT32 length;
		if (!source && SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)) && std::wstring(name).rfind(prefix, 0) == 0)
		{
			wprintf(L"opening \"%s\"\n", name);
			CHECK(devices[i]->ActivateObject(IID_PPV_ARGS(&source)));
		}
		CoTaskMemFree(name);
		devices[i]->Release();
	}
	CoTaskMemFree(devices);
	if (!source) { wprintf(L"FAILED no camera named \"%s...\" (%u cameras)\n", prefix.c_str(), count); return 1; }
	return 0;
}

static int Read(IMFMediaSource* source, double seconds, bool nv12, const wchar_t* out)
{
	ComPtr<IMFSourceReader> reader;
	CHECK(MFCreateSourceReaderFromMediaSource(source, nullptr, &reader));
	const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;

	auto selected = false;
	for (DWORD i = 0;; i++)
	{
		ComPtr<IMFMediaType> type;
		auto hr = reader->GetNativeMediaType(stream, i, &type);
		if (hr == MF_E_NO_MORE_TYPES) break;
		CHECK(hr);
		GUID subtype;
		UINT32 w = 0, h = 0, num = 0, den = 1;
		CHECK(type->GetGUID(MF_MT_SUBTYPE, &subtype));
		MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
		MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
		auto isNv12 = subtype == MFVideoFormat_NV12;
		wprintf(L"type %u: %s %ux%u @ %.1f\n", i, isNv12 ? L"NV12" : subtype == MFVideoFormat_RGB32 ? L"RGB32" : L"other", w, h, den ? (double)num / den : 0.0);
		if (!selected && (nv12 ? isNv12 : subtype == MFVideoFormat_RGB32) && w == 640 && h == 480)
		{
			CHECK(reader->SetCurrentMediaType(stream, nullptr, type.Get()));
			selected = true;
		}
	}
	if (!selected) { wprintf(L"FAILED requested format is not offered\n"); return 1; }

	CHECK(reader->SetStreamSelection(stream, TRUE)); // our presentation descriptor starts with nothing selected

	UINT frames = 0, gaps = 0;
	std::set<UINT64> distinct;
	LONGLONG previous = 0;
	double worst = 0;
	LARGE_INTEGER frequency, begin, now;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&begin);
	LARGE_INTEGER first = begin, last = begin;
	for (;;)
	{
		DWORD index, flags;
		LONGLONG time;
		ComPtr<IMFSample> sample;
		CHECK(reader->ReadSample(stream, 0, &index, &flags, &time, &sample));
		if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) { wprintf(L"FAILED stream flags=0x%X after %u frames\n", flags, frames); return 1; }
		QueryPerformanceCounter(&now);
		if (sample)
		{
			ComPtr<IMFMediaBuffer> buffer;
			CHECK(sample->ConvertToContiguousBuffer(&buffer));
			BYTE* data;
			DWORD length;
			CHECK(buffer->Lock(&data, nullptr, &length));
			UINT64 hash = 14695981039346656037ull;
			for (DWORD i = 0; i + 8 <= length; i += 8) hash = (hash ^ *(UINT64*)(data + i)) * 1099511628211ull;
			distinct.insert(hash);
			if (!frames)
			{
				first = now;
				wprintf(L"first frame after %.0f ms, %u bytes\n", (now.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart, length);
			}
			else
			{
				auto gap = (now.QuadPart - last.QuadPart) * 1000.0 / frequency.QuadPart;
				if (gap > worst) worst = gap;
				if (gap > 100) gaps++;
				if (time <= previous) { wprintf(L"FAILED timestamps go backwards\n"); return 1; }
			}
			previous = time;
			last = now;
			frames++;
			if (out && (now.QuadPart - begin.QuadPart) > seconds * frequency.QuadPart)
			{
				FILE* file;
				if (!_wfopen_s(&file, out, L"wb")) { fwrite(data, 1, length, file); fclose(file); }
			}
			buffer->Unlock();
		}
		if ((now.QuadPart - begin.QuadPart) > seconds * frequency.QuadPart) break;
	}
	auto elapsed = (double)(last.QuadPart - first.QuadPart) / frequency.QuadPart;
	wprintf(L"OK format=%s frames=%u fps=%.2f distinct=%u distinct_fps=%.2f worst_gap_ms=%.0f gaps_over_100ms=%u\n", nv12 ? L"NV12" : L"RGB32", frames,
		elapsed > 0 ? (frames - 1) / elapsed : 0, (UINT)distinct.size(), elapsed > 0 ? distinct.size() / elapsed : 0, worst, gaps);
	return 0;
}

int wmain(int argc, wchar_t** argv)
{
	std::wstring mode = argc > 2 ? argv[1] : L"";
	if (mode != L"dll" && mode != L"cam") { wprintf(L"usage: vcam_selftest dll <path> | cam <name>  [seconds] [nv12|rgb32] [out.raw]\n"); return 1; }
	auto seconds = argc > 3 ? _wtof(argv[3]) : 3.0;
	auto nv12 = argc > 4 ? std::wstring(argv[4]) != L"rgb32" : true;
	CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	CHECK(MFStartup(MF_VERSION));
	ComPtr<IMFMediaSource> source;
	auto result = mode == L"dll" ? OpenDll(argv[2], source) : OpenCamera(argv[2], source);
	if (!result) result = Read(source.Get(), seconds, nv12, argc > 5 ? argv[5] : nullptr);
	PrintCaptureStats();
	if (source)
	{
		auto hr = source->Shutdown();
		if (FAILED(hr)) wprintf(L"note: Shutdown hr=0x%08X\n", hr);
	}
	source.Reset();
	MFShutdown();
	return result;
}
