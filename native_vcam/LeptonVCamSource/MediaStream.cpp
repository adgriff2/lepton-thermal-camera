#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "FrameGenerator.h"
#include "MediaStream.h"
#include "MediaSource.h"

static constexpr UINT FPS = 30;
static constexpr MFTIME FRAME_DURATION = 10000000 / FPS; // 100ns units

static HRESULT CreateVideoType(REFGUID subtype, UINT stride, UINT sampleSize, IMFMediaType** outType)
{
	wil::com_ptr_nothrow<IMFMediaType> type;
	RETURN_IF_FAILED(MFCreateMediaType(&type));
	RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype));
	RETURN_IF_FAILED(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, LEPTON_FEED_WIDTH, LEPTON_FEED_HEIGHT));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, stride));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
	RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, FPS, 1));
	RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, sampleSize * 8 * FPS));
	RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
	*outType = type.detach();
	return S_OK;
}

HRESULT MediaStream::Initialize(IMFMediaSource* source, int index)
{
	RETURN_HR_IF_NULL(E_POINTER, source);
	_source = source;
	_index = index;

	RETURN_IF_FAILED(_stopEvent.create(wil::EventOptions::ManualReset));
	RETURN_IF_FAILED(_requestEvent.create());
	RETURN_IF_FAILED(_generator.Initialize());

	RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, index));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

	wil::com_ptr_nothrow<IMFMediaType> nv12, rgb;
	RETURN_IF_FAILED(CreateVideoType(MFVideoFormat_NV12, LEPTON_FEED_WIDTH, LEPTON_FEED_WIDTH * LEPTON_FEED_HEIGHT * 3 / 2, &nv12));
	RETURN_IF_FAILED(CreateVideoType(MFVideoFormat_RGB32, LEPTON_FEED_STRIDE, LEPTON_FEED_FRAME_SIZE, &rgb));
	IMFMediaType* types[] = { nv12.get(), rgb.get() };
	RETURN_IF_FAILED_MSG(MFCreateStreamDescriptor(_index, _countof(types), types, &_descriptor), "MFCreateStreamDescriptor failed");

	wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
	RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
	RETURN_IF_FAILED(handler->SetCurrentMediaType(types[0]));
	return S_OK;
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
	winrt::slim_lock_guard control(_control);
	return StartLocked(type);
}

HRESULT MediaStream::StartLocked(IMFMediaType* type)
{
	StopWorker();
	{
		winrt::slim_lock_guard lock(_lock);
		RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator || !_descriptor);

		if (type)
		{
			_type = type;
		}
		else if (!_type)
		{
			wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
			RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
			RETURN_IF_FAILED(handler->GetCurrentMediaType(&_type));
		}

		RETURN_IF_FAILED(_type->GetGUID(MF_MT_SUBTYPE, &_format));
		WINTRACE(L"MediaStream::Start format: %s", GUID_ToStringW(_format).c_str());
		RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(10, _type.get()));
		_state = MF_STREAM_STATE_RUNNING;
		RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
	}
	// streaming works without the board too (feed, or a message), so failing to start the capture is not fatal
	LOG_IF_FAILED(_generator.Start());
	RETURN_IF_FAILED(StartWorker());
	return S_OK;
}

HRESULT MediaStream::Stop()
{
	winrt::slim_lock_guard control(_control);
	return StopLocked();
}

HRESULT MediaStream::StopLocked()
{
	StopWorker();
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	_requests.clear();
	_state = MF_STREAM_STATE_STOPPED;
	RETURN_IF_FAILED(_allocator->UninitializeSampleAllocator());
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
	return S_OK;
}

MFSampleAllocatorUsage MediaStream::GetAllocatorUsage()
{
	return MFSampleAllocatorUsage_UsesProvidedAllocator;
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
	RETURN_HR_IF_NULL(E_POINTER, allocator);
	winrt::slim_lock_guard lock(_lock);
	_allocator.reset();
	RETURN_HR(allocator->QueryInterface(&_allocator));
}

void MediaStream::Shutdown()
{
	winrt::slim_lock_guard control(_control);
	StopWorker();
	winrt::slim_lock_guard lock(_lock);
	if (_queue)
	{
		LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
		_queue.reset();
	}

	_state = MF_STREAM_STATE_STOPPED;
	_requests.clear();
	_allocator.reset();
	_type.reset();
	_descriptor.reset();
	_source.reset();
	_attributes.reset();
}

// Sample delivery. Frame Server may ask for the next sample as soon as it has one, so
// a worker thread paces the answers at the advertised frame rate.
HRESULT MediaStream::StartWorker()
{
	_stopEvent.ResetEvent();
	_requestEvent.ResetEvent();
	try
	{
		_worker = std::thread([this] { Worker(); });
	}
	CATCH_RETURN();
	return S_OK;
}

void MediaStream::StopWorker()
{
	if (_worker.joinable())
	{
		_stopEvent.SetEvent();
		_worker.join();
	}
	_generator.Stop(); // releases the board
}

void MediaStream::Worker()
{
	wil::unique_handle timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
	if (!timer)
	{
		timer.reset(CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS));
	}

	auto due = MFGetSystemTime();
	for (;;)
	{
		wil::com_ptr_nothrow<IUnknown> token;
		auto pending = false;
		{
			winrt::slim_lock_guard lock(_lock);
			if (!_requests.empty() && _state == MF_STREAM_STATE_RUNNING)
			{
				token = _requests.front();
				pending = true;
			}
		}

		if (!pending)
		{
			HANDLE handles[] = { _stopEvent.get(), _requestEvent.get() };
			if (WaitForMultipleObjects(_countof(handles), handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
				return;

			continue;
		}

		// wait for this frame's turn
		auto now = MFGetSystemTime();
		if (due < now - FRAME_DURATION)
		{
			due = now; // we were idle, don't try to catch up
		}

		if (due > now)
		{
			LARGE_INTEGER relative;
			relative.QuadPart = -(due - now);
			if (timer && SetWaitableTimer(timer.get(), &relative, 0, nullptr, nullptr, FALSE))
			{
				HANDLE handles[] = { _stopEvent.get(), timer.get() };
				if (WaitForMultipleObjects(_countof(handles), handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
					return;
			}
			else if (_stopEvent.wait((DWORD)((due - now) / 10000)))
				return;
		}
		else if (_stopEvent.is_signaled())
			return;

		due += FRAME_DURATION;

		// on failure (for example all samples are still in use) the request stays queued and is retried one frame later
		LOG_IF_FAILED(DeliverSample(token.get()));
	}
}

HRESULT MediaStream::DeliverSample(IUnknown* token)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_allocator || !_queue);
	if (_state != MF_STREAM_STATE_RUNNING || _requests.empty())
		return S_OK;

	wil::com_ptr_nothrow<IMFSample> sample;
	RETURN_IF_FAILED(_allocator->AllocateSample(&sample));
	RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
	RETURN_IF_FAILED(sample->SetSampleDuration(FRAME_DURATION));
	RETURN_IF_FAILED(_generator.Generate(sample.get(), _format));
	if (token)
	{
		RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));
	}
	RETURN_IF_FAILED(_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get()));
	_requests.pop_front();
	return S_OK;
}

// IMFMediaEventGenerator
STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	//WINTRACE(L"MediaSource::BeginGetEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	//WINTRACE(L"MediaStream::EndGetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	WINTRACE(L"MediaStream::GetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	WINTRACE(L"MediaStream::QueueEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

// IMFMediaStream
STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
	WINTRACE(L"MediaSource::GetMediaSource");
	RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
	*ppMediaSource = nullptr;
	RETURN_HR_IF(MF_E_SHUTDOWN, !_source);

	RETURN_IF_FAILED(_source.copy_to(ppMediaSource));
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
	WINTRACE(L"MediaStream::GetStreamDescriptor");
	RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
	*ppStreamDescriptor = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);

	RETURN_IF_FAILED(_descriptor.copy_to(ppStreamDescriptor));
	return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
	{
		winrt::slim_lock_guard lock(_lock);
		RETURN_HR_IF(MF_E_SHUTDOWN, !_allocator || !_queue);
		// may legitimately come slightly before Start is finished: the request then waits for the worker
		try
		{
			_requests.emplace_back(pToken);
		}
		CATCH_RETURN();
	}
	_requestEvent.SetEvent();
	return S_OK;
}

// IMFMediaStream2
STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
	winrt::slim_lock_guard control(_control);
	MF_STREAM_STATE current;
	{
		winrt::slim_lock_guard lock(_lock);
		current = _state;
	}

	WINTRACE(L"MediaStream::SetStreamState current:%u value:%u", current, value);
	if (current == value)
		return S_OK;

	switch (value)
	{
	case MF_STREAM_STATE_PAUSED:
	{
		winrt::slim_lock_guard lock(_lock);
		RETURN_HR_IF(MF_E_INVALID_STATE_TRANSITION, _state != MF_STREAM_STATE_RUNNING);
		_state = value;
		return S_OK;
	}

	case MF_STREAM_STATE_RUNNING:
		if (current == MF_STREAM_STATE_PAUSED)
		{
			{
				winrt::slim_lock_guard lock(_lock);
				_state = value;
			}
			_requestEvent.SetEvent(); // answer what was requested while paused
			return S_OK;
		}
		return StartLocked(nullptr);

	case MF_STREAM_STATE_STOPPED:
		return StopLocked();

	default:
		RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
	}
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
	RETURN_HR_IF_NULL(E_POINTER, value);
	winrt::slim_lock_guard lock(_lock);
	*value = _state;
	return S_OK;
}

// IKsControl
STDMETHODIMP_(NTSTATUS) MediaStream::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsProperty len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsProperty prop:%s", PKSIDENTIFIER_ToString(property, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsMethod len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsMethod method:%s", PKSIDENTIFIER_ToString(method, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsEvent evt:%p len:%u data:%p dataLength:%u", evt, length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsEvent event:%s", PKSIDENTIFIER_ToString(evt, length).c_str());
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
