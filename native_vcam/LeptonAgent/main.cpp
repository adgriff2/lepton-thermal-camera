// lepton_agent.exe: starts the Lepton program while an app uses the "FLIR Lepton" camera, stops it afterwards.
//   lepton_agent run -- <command line>   (hidden, one per session; normally started at sign-in by autostart.ps1)
//   lepton_agent stop                    ask the running agent to end (it ends the Lepton program first)
//   lepton_agent watch [seconds]         only log camera activity (diagnostics)
// Log: %LOCALAPPDATA%\LeptonVCam\agent.log
// The Lepton program is asked to stop by closing its standard input (see python/run_on_demand.py): killing it could
// leave the FTDI board in the middle of a transfer. If the agent itself dies, that pipe closes too.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "../shared/LeptonVCamId.h"

using namespace Microsoft::WRL;

static const DWORD LINGER_MS = 5000;        // keep capturing that long after the last app closed the camera
static const DWORD MIN_RUN_MS = 5000;       // don't interrupt a program that is still starting
static const DWORD STOP_TIMEOUT_MS = 12000; // then terminate, as a last resort
static const DWORD MAX_BACKOFF_MS = 30000;

static std::wstring _folder;
static FILE* _log;

static void Log(const wchar_t* format, ...)
{
	wchar_t text[1024];
	va_list args;
	va_start(args, format);
	_vsnwprintf_s(text, _TRUNCATE, format, args);
	va_end(args);
	SYSTEMTIME t;
	GetLocalTime(&t);
	if (_log)
	{
		fwprintf(_log, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
		fflush(_log);
	}
}

static void OpenLog()
{
	PWSTR path = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
	{
		_folder = std::wstring(path) + L"\\LeptonVCam";
		CreateDirectoryW(_folder.c_str(), nullptr);
	}
	CoTaskMemFree(path);
	auto file = _folder + L"\\agent.log";
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) && data.nFileSizeLow > 1000000)
		MoveFileExW(file.c_str(), (file + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
	_log = _wfsopen(file.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
}

class ActivityCallback : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFSensorActivitiesReportCallback>
{
public:
	std::mutex lock;
	std::set<ULONG> users; // processes streaming from our camera
	HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	bool verbose = false;

	STDMETHOD(OnActivitiesReport)(IMFSensorActivitiesReport* report) override
	{
		ULONG count = 0;
		if (!report || FAILED(report->GetCount(&count))) return S_OK;
		if (verbose) Log(L"report: %u cameras", count);
		if (!count) SetUsers({}); // cameras that are not in use are not reported
		for (ULONG i = 0; i < count; i++)
		{
			ComPtr<IMFSensorActivityReport> camera;
			wchar_t name[256] = L"";
			ULONG written = 0, processes = 0;
			if (FAILED(report->GetActivityReport(i, &camera)) || FAILED(camera->GetFriendlyName(name, _countof(name), &written))) continue;
			camera->GetProcessCount(&processes);
			auto ours = std::wstring(name).rfind(LEPTON_VCAM_NAME, 0) == 0;
			std::set<ULONG> streaming;
			for (ULONG j = 0; j < processes; j++)
			{
				ComPtr<IMFSensorProcessActivity> process;
				BOOL active = FALSE;
				ULONG pid = 0;
				if (FAILED(camera->GetProcessActivity(j, &process))) continue;
				process->GetStreamingState(&active);
				process->GetProcessId(&pid);
				if (verbose) Log(L"  \"%s\" pid=%u streaming=%d", name, pid, active);
				if (active) streaming.insert(pid);
			}
			if (verbose && !processes) Log(L"  \"%s\" no process", name);
			if (ours) SetUsers(streaming);
		}
		return S_OK;
	}

	void SetUsers(const std::set<ULONG>& value)
	{
		std::lock_guard guard(lock);
		if (users != value) { users = value; SetEvent(changed); }
	}

	// an app that crashed may never be reported as stopped
	int CountUsers()
	{
		std::lock_guard guard(lock);
		for (auto it = users.begin(); it != users.end();)
		{
			auto dead = false;
			auto process = OpenProcess(SYNCHRONIZE, FALSE, *it);
			if (process)
			{
				dead = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
				CloseHandle(process);
			}
			else dead = GetLastError() == ERROR_INVALID_PARAMETER; // no such process (access denied means it's alive)
			it = dead ? users.erase(it) : std::next(it);
		}
		return (int)users.size();
	}
};

struct Child
{
	HANDLE process = nullptr;
	HANDLE input = nullptr; // write end of its standard input
	ULONGLONG started = 0;
};

static bool StartChild(const std::wstring& commandLine, Child& child)
{
	SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
	HANDLE readEnd = nullptr, writeEnd = nullptr;
	if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return false;
	SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);
	auto output = CreateFileW((_folder + L"\\capture.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, 0, nullptr);

	// only these two handles are inherited
	HANDLE handles[] = { readEnd, output };
	SIZE_T size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
	std::vector<BYTE> list(size);
	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = readEnd;
	si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = output;
	si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)list.data();
	PROCESS_INFORMATION pi{};
	auto ok = output != INVALID_HANDLE_VALUE &&
		InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) &&
		UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);
	if (ok)
	{
		auto command = commandLine; // CreateProcess may modify it
		ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
		auto error = GetLastError();
		DeleteProcThreadAttributeList(si.lpAttributeList);
		SetLastError(error);
	}
	auto error = GetLastError();
	CloseHandle(readEnd);
	if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
	if (!ok)
	{
		CloseHandle(writeEnd);
		Log(L"cannot start the Lepton program, error %u", error);
		return false;
	}

	CloseHandle(pi.hThread);
	child.process = pi.hProcess;
	child.input = writeEnd;
	child.started = GetTickCount64();
	Log(L"started the Lepton program, pid %u", pi.dwProcessId);
	return true;
}

static void StopChild(Child& child)
{
	if (!child.process) return;
	CloseHandle(child.input);
	if (WaitForSingleObject(child.process, STOP_TIMEOUT_MS) != WAIT_OBJECT_0)
	{
		Log(L"the Lepton program did not stop in time, terminating it");
		TerminateProcess(child.process, 99);
		WaitForSingleObject(child.process, 5000);
	}
	DWORD code = 0;
	GetExitCodeProcess(child.process, &code);
	Log(L"stopped the Lepton program, exit code %u", code);
	CloseHandle(child.process);
	child = {};
}

// the camera may have to be added again, for example after a restart
static void EnsureCamera()
{
	wchar_t exe[MAX_PATH];
	if (!ExpandEnvironmentStringsW(L"%ProgramData%\\LeptonVCam\\lepton_vcam.exe", exe, _countof(exe)) || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES)
	{
		Log(L"the camera is not installed on this PC (%s is missing)", exe);
		return;
	}
	for (auto action : { L"status", L"install" })
	{
		auto command = L"\"" + std::wstring(exe) + L"\" " + action;
		STARTUPINFOW si{ sizeof(si) };
		PROCESS_INFORMATION pi{};
		if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return;
		DWORD code = 1;
		WaitForSingleObject(pi.hProcess, 30000);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		Log(L"lepton_vcam %s: exit code %u", action, code);
		if (!code) return;
	}
}

static int Run(const std::wstring& command, DWORD watchSeconds)
{
	auto stop = CreateEventW(nullptr, TRUE, FALSE, L"Local\\LeptonCameraAgentStop");
	ResetEvent(stop);
	auto watch = command.empty();
	auto callback = Make<ActivityCallback>();
	callback->verbose = watch;
	ComPtr<IMFSensorActivityMonitor> monitor;
	auto hr = MFCreateSensorActivityMonitor(callback.Get(), &monitor);
	if (SUCCEEDED(hr)) hr = monitor->Start();
	if (FAILED(hr)) { Log(L"cannot monitor camera activity, hr=0x%08X", hr); return 1; }
	Log(L"agent running%s", watch ? L" (watch only)" : L"");

	Child child;
	auto begin = GetTickCount64();
	ULONGLONG lastUse = 0, nextStart = 0;
	DWORD backoff = 2000;
	auto previousUsers = 0;
	for (;;)
	{
		HANDLE handles[] = { stop, callback->changed, child.process };
		auto wait = WaitForMultipleObjects(child.process ? 3 : 2, handles, FALSE, 500);
		auto now = GetTickCount64();
		if (wait == WAIT_OBJECT_0 || (watch && now - begin > watchSeconds * 1000ull)) break;

		auto users = callback->CountUsers();
		if (users != previousUsers) Log(L"apps using the camera: %d", users);
		if (users && !previousUsers) { nextStart = 0; backoff = 2000; }
		previousUsers = users;
		if (users) lastUse = now;
		if (watch) continue;

		if (child.process && WaitForSingleObject(child.process, 0) == WAIT_OBJECT_0)
		{
			DWORD code = 0;
			GetExitCodeProcess(child.process, &code);
			if (now - child.started > 30000) backoff = 2000;
			Log(L"the Lepton program ended by itself, exit code %u, see capture.log (next try in %u s if still needed)", code, backoff / 1000);
			nextStart = now + backoff;
			backoff = std::min(backoff * 2, MAX_BACKOFF_MS);
			CloseHandle(child.input);
			CloseHandle(child.process);
			child = {};
		}

		if (!child.process && users && now >= nextStart)
		{
			if (!StartChild(command, child))
			{
				nextStart = now + backoff;
				backoff = std::min(backoff * 2, MAX_BACKOFF_MS);
			}
		}
		else if (child.process && !users && now - lastUse > LINGER_MS && now - child.started > MIN_RUN_MS)
		{
			StopChild(child);
		}
	}

	StopChild(child);
	monitor->Stop();
	Log(L"agent stopped");
	return 0;
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	OpenLog();
	std::wstring mode = __argc > 1 ? __wargv[1] : L"";
	std::wstring all = GetCommandLineW(), command;
	auto pos = all.find(L" -- ");
	if (pos != std::wstring::npos) command = all.substr(pos + 4);

	if (mode == L"stop")
	{
		auto stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\LeptonCameraAgentStop");
		if (stop) SetEvent(stop);
		auto mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\LeptonCameraAgent");
		if (!mutex) return 0;
		return WaitForSingleObject(mutex, 25000) == WAIT_TIMEOUT ? 1 : 0;
	}
	if (mode != L"watch" && (mode != L"run" || command.empty())) return 2;

	auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\LeptonCameraAgent");
	if (mode == L"run" && WaitForSingleObject(mutex, 0) == WAIT_TIMEOUT) { Log(L"another agent is already running"); return 3; }

	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) { Log(L"Media Foundation is not available"); return 1; }
	if (mode == L"run") EnsureCamera();
	auto result = Run(mode == L"run" ? command : L"", __argc > 2 ? (DWORD)_wtoi(__wargv[2]) : 20);
	MFShutdown();
	if (mode == L"run") ReleaseMutex(mutex);
	return result;
}
