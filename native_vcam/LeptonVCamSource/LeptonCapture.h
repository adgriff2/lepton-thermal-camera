#pragma once

#include "../shared/LeptonFeed.h"

// Captures a Lepton 3 on an FT2232H board (channel A, MPSSE SPI): C++ port of Grabber + render in lepton_cam.py.
// The board is only held between Start and Stop.
class LeptonCapture
{
public:
	enum class Status
	{
		Starting,
		Streaming,
		NoDriver,   // ftd2xx.dll is not installed
		NoBoard,
		Busy,       // another program uses the board
		Several,    // more than one possible board: don't guess
		NoData,     // board opened but no valid Lepton frames
		Error,
	};

	~LeptonCapture() { Stop(); }

	HRESULT Start();
	void Stop();

	// Copies the latest frame if it's newer than counter. Returns true if the latest frame is fresh enough to show.
	bool Latest(BYTE* bgrx, UINT64& counter, bool& copied);
	Status GetStatus(ULONG& error);

	struct Session;

private:
	std::shared_ptr<Session> _session;
	std::thread _reader;
	std::thread _worker;
};

// AGC, bicubic x4 and palette. Exported so that it can be compared with lepton_cam.py.
extern "C" void __stdcall LeptonRender(const UINT16* pixels, float* low, float* high, BOOL first, BYTE* bgrx);
