#include "pch.h"
#include "FeedReader.h"

extern HMODULE _hModule;

// SEH helpers: touching a mapped file can raise EXCEPTION_IN_PAGE_ERROR if the disk read fails.
// They must not contain C++ objects that need unwinding.
static bool SafeCopy(void* dst, const void* src, size_t size)
{
	__try
	{
		CopyMemory(dst, src, size);
		return true;
	}
	__except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
		return false;
	}
}

static bool SafeRead64(const volatile UINT64* src, UINT64* value)
{
	__try
	{
		*value = ReadAcquire64((const volatile LONG64*)src);
		return true;
	}
	__except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
		return false;
	}
}

bool FeedReader::EnsureOpen()
{
	if (_view)
		return true;

	auto now = GetTickCount64();
	if (now < _nextOpenAttempt)
		return false;

	_nextOpenAttempt = now + 1000;

	std::wstring path = wil::GetModuleFileNameW(_hModule).get();
	auto pos = path.find_last_of(L'\\');
	if (pos == std::wstring::npos)
		return false;

	path.resize(pos);
	path += L"\\feed\\frame.bin";

	// don't follow a link: users may write to this file, so they could try to redirect us
	wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (!file)
	{
		WinTraceFormat(2, 0, L"FeedReader cannot open '%s' error:%u", path.c_str(), GetLastError());
		return false;
	}

	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(file.get(), &info) ||
		(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
		info.nFileSizeHigh != 0 ||
		info.nFileSizeLow != LEPTON_FEED_FILE_SIZE)
	{
		WinTraceFormat(2, 0, L"FeedReader '%s' is not a valid feed file", path.c_str());
		return false;
	}

	// once mapped, the file cannot be truncated or deleted under us
	wil::unique_handle mapping(CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0, LEPTON_FEED_FILE_SIZE, nullptr));
	if (!mapping)
	{
		WinTraceFormat(2, 0, L"FeedReader CreateFileMapping error:%u", GetLastError());
		return false;
	}

	_view = (const BYTE*)MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, LEPTON_FEED_FILE_SIZE);
	if (!_view)
	{
		WinTraceFormat(2, 0, L"FeedReader MapViewOfFile error:%u", GetLastError());
		return false;
	}
	return true;
}

void FeedReader::Close()
{
	if (_view)
	{
		UnmapViewOfFile(_view);
		_view = nullptr;
	}
}

FeedReader::Result FeedReader::Read(BYTE* pixels)
{
	auto now = GetTickCount64();
	auto unchanged = [&]
		{
			return _hasFrame && now - _lastNewFrame < LEPTON_FEED_STALE_MS ? Result::Same : Result::NoSignal;
		};

	if (!EnsureOpen())
		return Result::NoSignal;

	LeptonFeedHeader header;
	if (!SafeCopy(&header, _view, sizeof(header)))
	{
		Close();
		return Result::NoSignal;
	}

	if (header.magic != LEPTON_FEED_MAGIC ||
		header.version != LEPTON_FEED_VERSION ||
		header.width != LEPTON_FEED_WIDTH ||
		header.height != LEPTON_FEED_HEIGHT ||
		header.stride != LEPTON_FEED_STRIDE ||
		header.format != LEPTON_FEED_FORMAT_BGRX ||
		header.slots != LEPTON_FEED_SLOTS ||
		header.headerSize != LEPTON_FEED_HEADER_SIZE)
		return Result::NoSignal;

	auto shared = (const LeptonFeedHeader*)_view;
	UINT64 counter;
	if (!SafeRead64(&shared->frameCounter, &counter) || !counter || counter == _counter)
		return unchanged();

	if (!_counter)
	{
		// first time we look at this file: it may contain an old frame from a producer that's gone
		if (header.tick > now || now - header.tick >= LEPTON_FEED_STALE_MS)
		{
			_counter = counter;
			return Result::NoSignal;
		}
	}

	auto slot = (UINT)((counter - 1) % LEPTON_FEED_SLOTS); // always in range, whatever the counter
	UINT64 before, after;
	if (!SafeRead64(&shared->slotSeq[slot], &before) || before != counter)
		return unchanged();

	// copy to a private buffer first so a torn frame never replaces a good one
	static_assert(LEPTON_FEED_HEADER_SIZE + LEPTON_FEED_SLOTS * LEPTON_FEED_FRAME_SIZE <= LEPTON_FEED_FILE_SIZE);
	if (_scratch.empty())
	{
		_scratch.resize(LEPTON_FEED_FRAME_SIZE);
	}

	if (!SafeCopy(_scratch.data(), _view + LEPTON_FEED_HEADER_SIZE + (size_t)slot * LEPTON_FEED_FRAME_SIZE, LEPTON_FEED_FRAME_SIZE))
		return unchanged();

	MemoryBarrier();
	if (!SafeRead64(&shared->slotSeq[slot], &after) || after != counter)
		return unchanged();

	CopyMemory(pixels, _scratch.data(), LEPTON_FEED_FRAME_SIZE);
	_counter = counter;
	_hasFrame = true;
	_lastNewFrame = now;
	return Result::NewFrame;
}
