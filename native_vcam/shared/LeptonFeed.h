#pragma once
// Frame feed protocol between the producer (python/leptonfeed.py) and the media source DLL.
// Keep in sync with python/leptonfeed.py.
//
// The feed is a fixed-size file, mapped by both sides. There is one writer and any number of readers.
// Layout: 4096-byte header, then 3 frame slots. Frame number N (starting at 1) lives in slot (N-1) % 3.
//
// Writer:  slotSeq[s] = 0;  copy pixels;  slotSeq[s] = N;  tick = now;  frameCounter = N
// Reader:  N = frameCounter;  check slotSeq[s] == N;  copy pixels;  check slotSeq[s] == N again (else torn, ignore)

#include <stdint.h>
#include <stddef.h>

#define LEPTON_FEED_MAGIC       0x4643564Cu // "LVCF"
#define LEPTON_FEED_VERSION     1u
#define LEPTON_FEED_FORMAT_BGRX 1u
#define LEPTON_FEED_WIDTH       640u
#define LEPTON_FEED_HEIGHT      480u
#define LEPTON_FEED_STRIDE      (LEPTON_FEED_WIDTH * 4u)
#define LEPTON_FEED_SLOTS       3u
#define LEPTON_FEED_HEADER_SIZE 4096u
#define LEPTON_FEED_FRAME_SIZE  (LEPTON_FEED_STRIDE * LEPTON_FEED_HEIGHT)
#define LEPTON_FEED_FILE_SIZE   (LEPTON_FEED_HEADER_SIZE + LEPTON_FEED_SLOTS * LEPTON_FEED_FRAME_SIZE)
#define LEPTON_FEED_STALE_MS    2000u       // no new frame for that long => "no signal"

struct LeptonFeedHeader
{
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint32_t slots;
	uint32_t headerSize;
	uint64_t frameCounter; // last published frame, 0 = none yet
	uint64_t tick;         // GetTickCount64() when it was published
	uint64_t slotSeq[LEPTON_FEED_SLOTS];
};

static_assert(offsetof(LeptonFeedHeader, frameCounter) == 32, "layout");
static_assert(offsetof(LeptonFeedHeader, tick) == 40, "layout");
static_assert(offsetof(LeptonFeedHeader, slotSeq) == 48, "layout");
static_assert(sizeof(LeptonFeedHeader) == 72, "layout");
static_assert(LEPTON_FEED_FILE_SIZE == 3690496u, "size (install.ps1 creates the file with this size)");
