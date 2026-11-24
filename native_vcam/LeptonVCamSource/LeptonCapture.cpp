#include "pch.h"
#include "LeptonCapture.h"
#include "InfernoPalette.h"

extern HMODULE _hModule;

// same numbers as lepton_cam.py --model 3
static constexpr size_t PKT = 164;                  // VoSPI packet
static constexpr size_t NPKT = 60;                  // packets per segment
static constexpr DWORD CHUNK = PKT * NPKT * 3;      // bytes per USB read
static constexpr BYTE CS = 1 << 3;                  // AD3
static constexpr BYTE DIRECTIONS = 0x03 | CS;
static constexpr BYTE DIVISOR = 2;                  // 60 MHz / ((1 + 2) * 2) = 10 MHz
static constexpr size_t MAX_QUEUE = 400;
static constexpr ULONGLONG RESYNC_MS = 3000;
static constexpr UINT LW = 160, LH = 120;
static constexpr UINT W = LEPTON_FEED_WIDTH, H = LEPTON_FEED_HEIGHT, SCALE = 4;
static_assert(LW * SCALE == W && LH * SCALE == H);
static const char BOARD_DESCRIPTION[] = "Dual RS232-HS A";

// D2XX, loaded on demand: the camera must also work (with the feed) where the FTDI driver is not installed
typedef PVOID FT_HANDLE;
typedef ULONG FT_STATUS;
static constexpr FT_STATUS FT_OK = 0, FT_DEVICE_NOT_FOUND = 2, FT_DEVICE_NOT_OPENED = 3;
static constexpr DWORD FT_FLAGS_OPENED = 1, FT_DEVICE_2232H = 6;

struct D2xx
{
	FT_STATUS(WINAPI* CreateDeviceInfoList)(DWORD*) = nullptr;
	FT_STATUS(WINAPI* GetDeviceInfoDetail)(DWORD, DWORD*, DWORD*, DWORD*, DWORD*, PVOID, PVOID, FT_HANDLE*) = nullptr;
	FT_STATUS(WINAPI* Open)(int, FT_HANDLE*) = nullptr;
	FT_STATUS(WINAPI* Close)(FT_HANDLE) = nullptr;
	FT_STATUS(WINAPI* ResetDevice)(FT_HANDLE) = nullptr;
	FT_STATUS(WINAPI* Purge)(FT_HANDLE, ULONG) = nullptr;
	FT_STATUS(WINAPI* SetTimeouts)(FT_HANDLE, ULONG, ULONG) = nullptr;
	FT_STATUS(WINAPI* SetLatencyTimer)(FT_HANDLE, UCHAR) = nullptr;
	FT_STATUS(WINAPI* SetUSBParameters)(FT_HANDLE, ULONG, ULONG) = nullptr;
	FT_STATUS(WINAPI* SetBitMode)(FT_HANDLE, UCHAR, UCHAR) = nullptr;
	FT_STATUS(WINAPI* Read)(FT_HANDLE, PVOID, DWORD, DWORD*) = nullptr;
	FT_STATUS(WINAPI* Write)(FT_HANDLE, PVOID, DWORD, DWORD*) = nullptr;

	// never unloaded: ftd2xx.dll has its own threads
	bool Load()
	{
		auto module = LoadLibraryExW(L"ftd2xx.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!module)
			return false;

		auto get = [&](auto& function, const char* name)
			{
				function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
				return function != nullptr;
			};
		return get(CreateDeviceInfoList, "FT_CreateDeviceInfoList") && get(GetDeviceInfoDetail, "FT_GetDeviceInfoDetail") &&
			get(Open, "FT_Open") && get(Close, "FT_Close") && get(ResetDevice, "FT_ResetDevice") && get(Purge, "FT_Purge") &&
			get(SetTimeouts, "FT_SetTimeouts") && get(SetLatencyTimer, "FT_SetLatencyTimer") && get(SetUSBParameters, "FT_SetUSBParameters") &&
			get(SetBitMode, "FT_SetBitMode") && get(Read, "FT_Read") && get(Write, "FT_Write");
	}
};

// process-wide counters for vcam_selftest
enum Counter { CHUNKS, RUNS, SEG0, SEG1, SEG2, SEG3, SEG4, SEG5, SEG6, SEG7, CRC_BAD, FRAMES, RESYNCS, DROPPED, OPENS, FAILS, OFFSET, SEARCHES, COUNTERS };
static std::atomic<UINT64> _counters[COUNTERS];

extern "C" void __stdcall LeptonStats(UINT64* values, UINT count)
{
	for (UINT i = 0; i < count; i++)
	{
		values[i] = i < COUNTERS ? _counters[i].load() : 0;
	}
}

// Shared by the owner and the two threads, so a thread stuck in the driver can be abandoned safely.
struct LeptonCapture::Session
{
	std::atomic<bool> stop = false;
	std::atomic<Status> status = Status::Starting;
	std::atomic<ULONG> error = 0;
	std::atomic<ULONGLONG> lastOk = 0;
	std::atomic<UINT> generation = 0; // changes each time the stream is restarted

	std::mutex queueLock;
	std::condition_variable queueEvent;
	std::deque<std::vector<BYTE>> queue;

	std::mutex frameLock;
	std::vector<BYTE> frame;
	UINT64 counter = 0;
	ULONGLONG frameTick = 0;

	bool Sleep(DWORD ms)
	{
		std::unique_lock guard(queueLock);
		return !queueEvent.wait_for(guard, std::chrono::milliseconds(ms), [this] { return stop.load(); });
	}
};

using Session = LeptonCapture::Session;
using Status = LeptonCapture::Status;

class Board
{
public:
	Board(const D2xx& ft, Session& session) : _ft(ft), _session(session) {}
	~Board()
	{
		if (!_handle)
			return;

		_ft.SetBitMode(_handle, 0, 0);
		Write({ 0x80, 0x01, 0x00 }); // release the pins
		_ft.Close(_handle);
	}

	Status Open(FT_STATUS& error)
	{
		DWORD count = 0;
		error = _ft.CreateDeviceInfoList(&count);
		if (error != FT_OK)
			return Status::Error;

		auto found = -1, matches = 0;
		auto opened = false;
		for (DWORD i = 0; i < count; i++)
		{
			DWORD flags = 0, type = 0, id = 0, location = 0;
			char serial[64] = "", description[128] = "";
			FT_HANDLE handle;
			if (_ft.GetDeviceInfoDetail(i, &flags, &type, &id, &location, serial, description, &handle) != FT_OK)
				continue;

			opened |= (flags & FT_FLAGS_OPENED) != 0; // details are hidden in that case
			if (!(flags & FT_FLAGS_OPENED) && type == FT_DEVICE_2232H && !strcmp(description, BOARD_DESCRIPTION))
			{
				found = (int)i;
				matches++;
			}
		}

		if (!matches)
			return opened ? Status::Busy : Status::NoBoard;

		if (matches > 1)
			return Status::Several;

		error = _ft.Open(found, &_handle);
		if (error != FT_OK)
		{
			_handle = nullptr;
			return error == FT_DEVICE_NOT_OPENED ? Status::Busy : error == FT_DEVICE_NOT_FOUND ? Status::NoBoard : Status::Error;
		}

		_ft.ResetDevice(_handle);
		_ft.Purge(_handle, 3);
		_ft.SetTimeouts(_handle, 2000, 2000);
		_ft.SetLatencyTimer(_handle, 1);
		_ft.SetUSBParameters(_handle, 65536, 65536);
		_ft.SetBitMode(_handle, 0, 0);
		_ft.SetBitMode(_handle, 0, 2);
		error = Write({ 0x85, 0x8A, 0x97, 0x8D, 0x86, DIVISOR, 0 });
		::Sleep(50);
		_ft.Purge(_handle, 3);
		return error == FT_OK ? Status::Streaming : Status::Error;
	}

	FT_STATUS StartStream()
	{
		_ft.SetBitMode(_handle, 0, 0); // abort anything still queued in the MPSSE
		_ft.SetBitMode(_handle, 0, 2);
		auto error = Write({ 0x85, 0x8A, 0x97, 0x8D, 0x86, DIVISOR, 0, 0x80, 0x01 | CS, DIRECTIONS });
		if (error != FT_OK)
			return error;

		::Sleep(300); // CS high > 185 ms so the Lepton restarts its VoSPI state
		_ft.Purge(_handle, 3);
		{
			std::lock_guard guard(_session.queueLock);
			_session.queue.clear();
			_session.generation++;
		}

		std::vector<BYTE> start{ 0x80, 0x01, DIRECTIONS }; // CS low, keep two reads queued
		for (auto i = 0; i < 2; i++)
		{
			start.insert(start.end(), std::begin(ReadCommand), std::end(ReadCommand));
		}
		return Write(start);
	}

	FT_STATUS ReadChunk(std::vector<BYTE>& buffer, bool& timeout)
	{
		buffer.resize(CHUNK);
		timeout = false;
		for (DWORD total = 0; total < CHUNK;)
		{
			DWORD read = 0;
			auto error = _ft.Read(_handle, buffer.data() + total, CHUNK - total, &read);
			if (error != FT_OK)
				return error;

			if (!read || read > CHUNK - total)
			{
				timeout = true;
				return FT_OK;
			}
			total += read;
		}
		return FT_OK;
	}

	FT_STATUS QueueRead() { return Write({ std::begin(ReadCommand), std::end(ReadCommand) }); }

private:
	// clock bytes in; no CS change, so the stream has no gaps
	static constexpr BYTE ReadCommand[] = { 0x20, (CHUNK - 1) & 0xFF, (CHUNK - 1) >> 8, 0x87 };

	FT_STATUS Write(std::vector<BYTE> data)
	{
		DWORD written = 0;
		return _ft.Write(_handle, data.data(), (DWORD)data.size(), &written);
	}

	const D2xx& _ft;
	Session& _session;
	FT_HANDLE _handle = nullptr;
};

static void CaptureOnce(const D2xx& ft, Session& session)
{
	Board board(ft, session);
	FT_STATUS error = FT_OK;
	auto status = board.Open(error);
	if (status != Status::Streaming)
	{
		session.error = error;
		session.status = status;
		return;
	}

	auto fail = [&](FT_STATUS e)
		{
			session.error = e;
			session.status = Status::Error;
			_counters[FAILS]++;
		};

	_counters[OPENS]++;
	session.lastOk = GetTickCount64();
	error = board.StartStream();
	if (error != FT_OK)
		return fail(error);

	std::vector<BYTE> buffer;
	while (!session.stop)
	{
		// does nothing but read, so the clock never stops
		auto timeout = false;
		error = board.ReadChunk(buffer, timeout);
		if (error != FT_OK || timeout)
			return fail(error);

		_counters[CHUNKS]++;
		error = board.QueueRead(); // always one read queued ahead of the one being read
		if (error != FT_OK)
			return fail(error);

		{
			std::lock_guard guard(session.queueLock);
			if (session.queue.size() < MAX_QUEUE)
			{
				session.queue.push_back(std::move(buffer));
			}
			else
			{
				_counters[DROPPED]++;
			}
		}
		session.queueEvent.notify_all();

		if (GetTickCount64() - session.lastOk > RESYNC_MS)
		{
			session.status = Status::NoData;
			_counters[RESYNCS]++;
			error = board.StartStream();
			if (error != FT_OK)
				return fail(error);

			session.lastOk = GetTickCount64();
		}
	}
}

static void Reader(std::shared_ptr<Session> session)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	try
	{
		static D2xx ft;
		static std::once_flag once;
		static bool loaded;
		std::call_once(once, [] { loaded = ft.Load(); });
		if (!loaded)
		{
			session->status = Status::NoDriver;
			return;
		}

		do
		{
			CaptureOnce(ft, *session);
		} while (session->Sleep(2000)); // try again until stopped
	}
	catch (...)
	{
		session->status = Status::Error; // out of memory: must not take the service down
	}
}

static bool PacketCrcOk(const BYTE* packet)
{
	static const auto table = []
		{
			std::array<UINT16, 256> t{};
			for (UINT i = 0; i < 256; i++)
			{
				auto c = (UINT16)(i << 8);
				for (auto j = 0; j < 8; j++)
				{
					c = (UINT16)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
				}
				t[i] = c;
			}
			return t;
		}();

	UINT16 crc = 0;
	for (size_t i = 0; i < PKT; i++)
	{
		auto b = i == 0 ? (BYTE)(packet[0] & 0x0F) : i == 2 || i == 3 ? (BYTE)0 : packet[i];
		crc = (UINT16)((crc << 8) ^ table[((crc >> 8) ^ b) & 0xFF]);
	}
	return crc == ((packet[2] << 8) | packet[3]);
}

// The stream often starts some bits away from a packet boundary and stays like that until it's restarted. The data
// is fine: find the boundary once, using packet numbers and CRCs, and read from there.
class PacketStream
{
	static constexpr size_t PKT_BITS = PKT * 8;
	std::vector<BYTE> _data;
	size_t _bit = 0; // position of the next packet in _data
	bool _locked = false;

	void Copy(size_t bit, BYTE* output, size_t size) const
	{
		auto p = _data.data() + bit / 8;
		auto shift = bit % 8;
		for (size_t i = 0; i < size; i++)
		{
			output[i] = shift ? (BYTE)((p[i] << shift) | (p[i + 1] >> (8 - shift))) : p[i];
		}
	}

public:
	bool Locked() const { return _locked; }

	void Reset()
	{
		_data.clear();
		_bit = 0;
		_locked = false;
	}

	void Add(const std::vector<BYTE>& chunk)
	{
		if (!_locked && _data.size() > CHUNK)
		{
			_data.clear(); // nothing usable in there
		}
		_data.erase(_data.begin(), _data.begin() + std::min(_bit / 8, _data.size()));
		_bit %= 8;
		_data.insert(_data.end(), chunk.begin(), chunk.end());
	}

	void Search()
	{
		auto bits = _data.size() * 8;
		size_t best = 0, bestStart = 0;
		for (size_t start = 0; start < PKT_BITS; start++)
		{
			size_t score = 0;
			auto previous = -1;
			for (auto bit = start; bit + PKT_BITS <= bits; bit += PKT_BITS)
			{
				BYTE packet[PKT];
				Copy(bit, packet, 2);
				if ((packet[0] & 0x0F) == 0x0F || packet[1] >= NPKT)
				{
					previous = -1;
					continue;
				}

				auto follows = previous >= 0 && packet[1] == previous + 1;
				previous = packet[1];
				if (follows)
				{
					Copy(bit, packet, PKT);
					score += PacketCrcOk(packet);
				}
			}

			if (score > best)
			{
				best = score;
				bestStart = start;
			}
		}

		if (best >= 20)
		{
			_locked = true;
			_bit = bestStart;
			_counters[OFFSET] = bestStart;
			_counters[SEARCHES]++;
		}
	}

	bool Next(BYTE* packet)
	{
		if (!_locked || _bit + PKT_BITS > _data.size() * 8)
			return false;

		Copy(_bit, packet, PKT);
		_bit += PKT_BITS;
		return true;
	}
};

// numpy.percentile (linear interpolation)
static float Percentile(std::vector<UINT16>& values, double percent)
{
	auto position = percent / 100 * (values.size() - 1);
	auto index = (size_t)position;
	std::nth_element(values.begin(), values.begin() + index, values.end());
	double a = values[index];
	double b = index + 1 < values.size() ? *std::min_element(values.begin() + index + 1, values.end()) : a;
	return (float)(a + (b - a) * (position - index));
}

// AGC + bicubic x4 + palette
extern "C" void __stdcall LeptonRender(const UINT16* pixels, float* lowState, float* highState, BOOL first, BYTE* bgrx)
{
	auto& low = *lowState;
	auto& high = *highState;
	std::vector<UINT16> sorted(pixels, pixels + LW * LH);
	auto lo = Percentile(sorted, 1);
	auto hi = std::max(Percentile(sorted, 99), lo + 20);

	// lepton_cam.py smooths by 0.9 on each display refresh, about 4 times per Lepton frame
	constexpr auto keep = 0.9f * 0.9f * 0.9f * 0.9f;
	low = first ? lo : keep * low + (1 - keep) * lo;
	high = first ? hi : keep * high + (1 - keep) * hi;

	std::vector<float> gray(LW * LH);
	for (size_t i = 0; i < gray.size(); i++)
	{
		gray[i] = std::floor(std::clamp((pixels[i] - low) / (high - low) * 255, 0.f, 255.f));
	}

	// cubic kernel of cv2.INTER_CUBIC (a = -0.75), one set of weights per sub-pixel position
	float weights[SCALE][4];
	int offsets[SCALE];
	for (UINT p = 0; p < SCALE; p++)
	{
		auto source = (p + 0.5f) / SCALE - 0.5f;
		auto base = std::floor(source);
		auto x = source - base;
		constexpr auto a = -0.75f;
		weights[p][0] = ((a * (x + 1) - 5 * a) * (x + 1) + 8 * a) * (x + 1) - 4 * a;
		weights[p][1] = ((a + 2) * x - (a + 3)) * x * x + 1;
		weights[p][2] = ((a + 2) * (1 - x) - (a + 3)) * (1 - x) * (1 - x) + 1;
		weights[p][3] = 1 - weights[p][0] - weights[p][1] - weights[p][2];
		offsets[p] = (int)base - 1;
	}

	std::vector<float> wide((size_t)W * LH);
	for (UINT y = 0; y < LH; y++)
	{
		for (UINT x = 0; x < W; x++)
		{
			auto sum = 0.f;
			for (auto k = 0; k < 4; k++)
			{
				auto sx = std::clamp((int)(x / SCALE) + offsets[x % SCALE] + k, 0, (int)LW - 1);
				sum += weights[x % SCALE][k] * gray[(size_t)y * LW + sx];
			}
			wide[(size_t)y * W + x] = sum;
		}
	}

	auto output = (UINT32*)bgrx;
	for (UINT y = 0; y < H; y++)
	{
		const float* rows[4];
		for (auto k = 0; k < 4; k++)
		{
			rows[k] = &wide[(size_t)std::clamp((int)(y / SCALE) + offsets[y % SCALE] + k, 0, (int)LH - 1) * W];
		}

		auto w = weights[y % SCALE];
		for (UINT x = 0; x < W; x++)
		{
			auto value = w[0] * rows[0][x] + w[1] * rows[1][x] + w[2] * rows[2][x] + w[3] * rows[3][x];
			*output++ = InfernoPalette[(int)(std::clamp(value, 0.f, 255.f) + 0.5f)];
		}
	}
}

static void Worker(std::shared_ptr<Session> session)
{
	try
	{
		static_assert(sizeof(BYTE[NPKT][PKT]) == NPKT * PKT);
		auto rows = std::make_unique<BYTE[][PKT]>(NPKT);
		std::vector<UINT16> canvas(LW * LH);
		std::vector<BYTE> image(LEPTON_FEED_FRAME_SIZE);
		PacketStream stream;
		UINT generation = 0;
		ULONGLONG lastSearch = 0;
		BYTE packet[PKT];
		size_t expected = 0;
		auto mask = 0;
		auto low = 0.f, high = 0.f;
		auto first = true;
		while (!session->stop)
		{
			std::vector<BYTE> buffer;
			{
				std::unique_lock guard(session->queueLock);
				session->queueEvent.wait_for(guard, std::chrono::milliseconds(200), [&] { return session->stop || !session->queue.empty(); });
				if (session->stop)
					return;

				if (session->queue.empty())
					continue;

				buffer = std::move(session->queue.front());
				session->queue.pop_front();
				if (generation != session->generation)
				{
					generation = session->generation;
					stream.Reset();
					expected = 0;
				}
			}

			stream.Add(buffer);
			auto now = GetTickCount64();
			if (!stream.Locked() || (now - session->lastOk > 1000 && now - lastSearch > 500))
			{
				lastSearch = now;
				stream.Search();
			}

			while (stream.Next(packet))
			{
				if ((packet[0] & 0x0F) == 0x0F)
					continue; // discard packet

				size_t number = packet[1];
				if (number == 0)
				{
					CopyMemory(rows[0], packet, PKT);
					expected = 1;
				}
				else if (number == expected && number < NPKT)
				{
					CopyMemory(rows[number], packet, PKT);
					expected++;
				}
				else
				{
					expected = 0;
				}

				if (expected != NPKT)
					continue;

				expected = 0;
				auto segment = (rows[20][0] >> 4) & 7;
				_counters[RUNS]++;
				_counters[SEG0 + segment]++;
				if (segment < 1 || segment > 4)
					continue; // invalid or duplicate segment

				if (!std::all_of(rows.get(), rows.get() + NPKT, [](auto& row) { return PacketCrcOk(row); }))
				{
					_counters[CRC_BAD]++;
					continue;
				}

				auto target = canvas.data() + (size_t)(segment - 1) * 30 * LW;
				for (size_t r = 0; r < NPKT; r++)
				{
					for (size_t i = 4; i < PKT; i += 2)
					{
						*target++ = (UINT16)((rows[r][i] << 8) | rows[r][i + 1]);
					}
				}

				mask = segment == 1 ? 1 : mask | (1 << (segment - 1));
				session->lastOk = GetTickCount64();
				if (segment != 4 || mask != 0xF)
					continue;

				LeptonRender(canvas.data(), &low, &high, first, image.data());
				first = false;
				{
					std::lock_guard guard(session->frameLock);
					session->frame.swap(image);
					session->counter++;
					session->frameTick = GetTickCount64();
				}
				image.resize(LEPTON_FEED_FRAME_SIZE);
				session->status = Status::Streaming;
				_counters[FRAMES]++;
			}
		}
	}
	catch (...)
	{
		session->status = Status::Error;
	}
}

HRESULT LeptonCapture::Start()
{
	Stop();
	try
	{
		auto session = std::make_shared<Session>();
		_session = session;
		_worker = std::thread(Worker, session);
		_reader = std::thread(Reader, session);
	}
	catch (...)
	{
		Stop();
		RETURN_CAUGHT_EXCEPTION();
	}
	return S_OK;
}

// A USB driver call should never last more than a couple of seconds. If one does, leave its thread behind (and this
// DLL loaded) rather than blocking Frame Server, which would freeze every camera of the PC.
static void JoinOrAbandon(std::thread& thread)
{
	if (!thread.joinable())
		return;

	if (WaitForSingleObject(thread.native_handle(), 4000) == WAIT_OBJECT_0)
	{
		thread.join();
		return;
	}

	HMODULE module;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&JoinOrAbandon, &module);
	thread.detach();
	WinTraceFormat(2, 0, L"LeptonCapture thread abandoned");
}

void LeptonCapture::Stop()
{
	if (_session)
	{
		{
			std::lock_guard guard(_session->queueLock);
			_session->stop = true;
		}
		_session->queueEvent.notify_all();
	}
	JoinOrAbandon(_reader);
	JoinOrAbandon(_worker);
	_session.reset();
}

bool LeptonCapture::Latest(BYTE* bgrx, UINT64& counter, bool& copied)
{
	copied = false;
	if (!_session)
		return false;

	std::lock_guard guard(_session->frameLock);
	if (!_session->counter || GetTickCount64() - _session->frameTick >= LEPTON_FEED_STALE_MS)
		return false;

	if (counter != _session->counter)
	{
		CopyMemory(bgrx, _session->frame.data(), LEPTON_FEED_FRAME_SIZE);
		counter = _session->counter;
		copied = true;
	}
	return true;
}

LeptonCapture::Status LeptonCapture::GetStatus(ULONG& error)
{
	error = _session ? _session->error.load() : 0;
	return _session ? _session->status.load() : Status::Starting;
}
