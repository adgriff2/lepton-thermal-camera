#include "pch.h"
#include "Tools.h"
#include "FrameGenerator.h"

static constexpr UINT W = LEPTON_FEED_WIDTH;
static constexpr UINT H = LEPTON_FEED_HEIGHT;

// tiny 5x7 font, only the characters we need: no font or GPU dependency inside the service
struct Glyph { char c; BYTE rows[7]; };
static const Glyph _glyphs[] =
{
	{ 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
	{ 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
	{ 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
	{ 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
	{ 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
	{ 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
	{ 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
	{ 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
	{ 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
	{ '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
	{ '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
	{ '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
	{ '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
	{ '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
	{ '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
	{ '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
	{ '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
};

static void DrawCentered(std::vector<BYTE>& bgrx, const char* text, UINT top, UINT scale, BYTE level)
{
	auto len = (UINT)strlen(text);
	auto left = (W - (len * 6 - 1) * scale) / 2;
	for (UINT i = 0; i < len; i++)
	{
		for (auto& glyph : _glyphs)
		{
			if (glyph.c != text[i])
				continue;

			for (UINT y = 0; y < 7 * scale; y++)
			{
				for (UINT x = 0; x < 5 * scale; x++)
				{
					if (glyph.rows[y / scale] & (0x10 >> (x / scale)))
					{
						auto px = &bgrx[((size_t)(top + y) * W + left + i * 6 * scale + x) * 4];
						px[0] = px[1] = px[2] = level;
					}
				}
			}
		}
	}
}

HRESULT FrameGenerator::Initialize()
{
	try
	{
		_feedBgrx.resize(LEPTON_FEED_FRAME_SIZE);
		_captureBgrx.resize(LEPTON_FEED_FRAME_SIZE);
		_messageBgrx.resize(LEPTON_FEED_FRAME_SIZE);
		_nv12.resize((size_t)W * H * 3 / 2);
	}
	CATCH_RETURN();
	return S_OK;
}

HRESULT FrameGenerator::Start()
{
	_startTick = GetTickCount64();
	_captureCounter = 0;
	_nv12Source = Source::None;
	RETURN_HR(_capture.Start());
}

void FrameGenerator::Stop()
{
	_capture.Stop();
}

void FrameGenerator::UpdateMessage()
{
	ULONG error;
	auto status = _capture.GetStatus(error);
	std::string reason;
	switch (status)
	{
	case LeptonCapture::Status::NoDriver: reason = "FTDI DRIVER NOT FOUND"; break;
	case LeptonCapture::Status::NotConfigured: reason = "NO BOARD CONFIGURED"; break;
	case LeptonCapture::Status::NoBoard: reason = "BOARD NOT CONNECTED"; break;
	case LeptonCapture::Status::Busy: reason = "BOARD IN USE"; break;
	case LeptonCapture::Status::Several: reason = "SEVERAL BOARDS FOUND"; break;
	case LeptonCapture::Status::NoData: reason = "NO DATA FROM LEPTON"; break;
	case LeptonCapture::Status::Error: reason = "BOARD ERROR " + std::to_string(error); break;
	}

	// it takes about a second to get the first frame
	std::string title = reason.empty() && GetTickCount64() - _startTick < 4000 ? "STARTING" : "NO SIGNAL";
	if (title + reason == _message)
		return;

	_message = title + reason;
	_nv12Source = Source::None;
	for (size_t i = 0; i < _messageBgrx.size(); i += 4)
	{
		_messageBgrx[i] = _messageBgrx[i + 1] = _messageBgrx[i + 2] = 32;
		_messageBgrx[i + 3] = 255;
	}
	DrawCentered(_messageBgrx, "LEPTON THERMAL", 150, 4, 110);
	DrawCentered(_messageBgrx, title.c_str(), 220, 8, 220);
	if (!reason.empty())
	{
		DrawCentered(_messageBgrx, reason.c_str(), 320, 3, 150);
	}
}

static void CopyRows(const BYTE* src, size_t srcStride, BYTE* dst, LONG dstStride, UINT rows)
{
	for (UINT y = 0; y < rows; y++)
	{
		CopyMemory(dst, src, srcStride);
		src += srcStride;
		dst += dstStride;
	}
}

HRESULT FrameGenerator::Generate(IMFSample* sample, REFGUID format)
{
	RETURN_HR_IF_NULL(E_POINTER, sample);
	RETURN_HR_IF(E_UNEXPECTED, _nv12.empty());

	auto source = Source::Message;
	const BYTE* bgrx = nullptr;
	try
	{
		auto feed = _feed.Read(_feedBgrx.data());
		auto changed = false;
		if (feed != FeedReader::Result::NoSignal)
		{
			source = Source::Feed;
			bgrx = _feedBgrx.data();
			changed = feed == FeedReader::Result::NewFrame;
		}
		else if (_capture.Latest(_captureBgrx.data(), _captureCounter, changed))
		{
			source = Source::Board;
			bgrx = _captureBgrx.data();
		}
		else
		{
			UpdateMessage();
			bgrx = _messageBgrx.data();
		}

		if (changed)
		{
			_nv12Source = Source::None;
		}
	}
	CATCH_RETURN();

	auto nv12 = format == MFVideoFormat_NV12;
	if (nv12 && _nv12Source != source)
	{
		BGRXToNV12(bgrx, W, H, _nv12.data());
		_nv12Source = source;
	}

	wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
	RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));
	wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
	RETURN_IF_FAILED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2D)));

	BYTE* scanline;
	LONG pitch;
	BYTE* start;
	DWORD length;
	RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &length));

	auto hr = S_OK;
	auto rowBytes = nv12 ? W : W * 4;
	auto rows = nv12 ? H * 3 / 2 : H;
	if (pitch < (LONG)rowBytes || (ULONGLONG)pitch * (rows - 1) + rowBytes > length)
	{
		hr = E_UNEXPECTED; // bottom-up or too small
	}
	else if (nv12)
	{
		// the UV plane directly follows the Y plane
		CopyRows(_nv12.data(), rowBytes, scanline, pitch, rows);
	}
	else
	{
		CopyRows(bgrx, rowBytes, scanline, pitch, rows);
	}
	buffer2D->Unlock2D();
	RETURN_IF_FAILED(hr);

	DWORD contiguous = 0;
	RETURN_IF_FAILED(buffer2D->GetContiguousLength(&contiguous));
	RETURN_IF_FAILED(buffer->SetCurrentLength(contiguous));
	return S_OK;
}
