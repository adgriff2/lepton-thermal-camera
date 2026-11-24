#pragma once

#include "FeedReader.h"
#include "LeptonCapture.h"

// Fills samples with, in this order: the latest feed frame (a program is sending frames), the Lepton captured
// directly by this DLL, or a "no signal" picture that says why. CPU only.
class FrameGenerator
{
	enum class Source { None, Feed, Board, Message };

	FeedReader _feed;
	LeptonCapture _capture;
	UINT64 _captureCounter = 0;
	ULONGLONG _startTick = 0;
	std::vector<BYTE> _feedBgrx;
	std::vector<BYTE> _captureBgrx;
	std::vector<BYTE> _messageBgrx;
	std::string _message;
	std::vector<BYTE> _nv12; // conversion of the current picture
	Source _nv12Source = Source::None;

	void UpdateMessage();

public:
	HRESULT Initialize();
	HRESULT Start();
	void Stop();
	HRESULT Generate(IMFSample* sample, REFGUID format);
};
