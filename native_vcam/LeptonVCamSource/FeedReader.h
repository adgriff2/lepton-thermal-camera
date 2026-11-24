#pragma once

#include "../shared/LeptonFeed.h"

// Read-only view of the frame feed (<folder of this DLL>\feed\frame.bin).
// The file content is written by an ordinary user process: nothing in it is trusted.
class FeedReader
{
public:
	enum class Result
	{
		NoSignal, // no producer, or it stopped sending
		Same,     // last frame is still the current one
		NewFrame, // pixels were updated
	};

	~FeedReader() { Close(); }

	// pixels must hold LEPTON_FEED_FRAME_SIZE bytes
	Result Read(BYTE* pixels);

private:
	bool EnsureOpen();
	void Close();

	const BYTE* _view = nullptr;
	ULONGLONG _nextOpenAttempt = 0;
	ULONGLONG _lastNewFrame = 0; // our own clock, so a producer cannot fake freshness
	UINT64 _counter = 0;
	bool _hasFrame = false;
	std::vector<BYTE> _scratch;
};
