﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "pch.h"
#include "Plugin.CaptureEngine.h"
#include "Plugin.CaptureEngine.g.cpp"

#include "Media.Functions.h"
#include "Media.Payload.h"
#include "Media.Capture.MrcAudioEffect.h"
#include "Media.Capture.MrcVideoEffect.h"

#include <mferror.h>
#include <mfmediacapture.h>

#include <pplawait.h>

using namespace winrt;
using namespace CameraCapture::Plugin::implementation;
using namespace CameraCapture::Media::Capture::implementation;
using namespace Windows::Foundation;
using namespace Windows::Media::Effects;
using namespace Windows::Media::Core;
using namespace Windows::Media::Capture;
using namespace Windows::Media::MediaProperties;

_Use_decl_annotations_
CameraCapture::Plugin::Module CaptureEngine::Create(
    std::weak_ptr<IUnityDeviceResource> const& unityDevice,
    StateChangedCallback fnCallback,
    void* pCallbackObject)
{
    auto capture = make<CaptureEngine>();

    if (SUCCEEDED(capture.as<IModulePriv>()->Initialize(unityDevice, fnCallback, pCallbackObject)))
    {
        return capture;
    }

    return nullptr;
}


CaptureEngine::CaptureEngine()
	: m_isShutdown(false)
	, m_startPreviewEventHandle(CreateEvent(nullptr, true, true, nullptr))
	, m_stopPreviewEventHandle(CreateEvent(nullptr, true, true, nullptr))
    , m_mediaDevice(nullptr)
	, m_dxgiDeviceManager(nullptr)
	, m_category(MediaCategory::Communications)
	, m_streamType(MediaStreamType::VideoPreview)
	, m_videoProfile(KnownVideoProfile::VideoConferencing)
	, m_sharingMode(MediaCaptureSharingMode::ExclusiveControl)
    , m_startPreviewOp(nullptr)
    , m_stopPreviewOp(nullptr)
	, m_mediaCapture(nullptr)
	, m_initSettings(nullptr)
	, m_mrcAudioEffect(nullptr)
    , m_mrcVideoEffect(nullptr)
    , m_mrcPreviewEffect(nullptr)
    , m_mediaSink(nullptr)
    , m_payloadHandler(nullptr)
    , m_audioSample(nullptr)
    , m_sharedVideoTexture(nullptr)
    , m_appCoordinateSystem(nullptr)
{
	InitializeCriticalSection(&m_cs);
}

CaptureEngine::~CaptureEngine()
{
	DeleteCriticalSection(&m_cs);
}

void CaptureEngine::Shutdown()
{
	auto guard = CriticalSectionGuard(m_cs);

    if (m_isShutdown)
    {
        return;
    }
    m_isShutdown = true;

	// see if any outstanding operations are running
    if (m_startPreviewOp != nullptr && m_startPreviewOp.Status() == AsyncStatus::Started)
    {
		concurrency::create_task([this]()
			{
				WaitForSingleObject(m_startPreviewEventHandle.get(), INFINITE);
			}).get();
	}

	if (m_mediaCapture != nullptr)
	{
		StopPreview();
	}

	if (m_stopPreviewOp != nullptr && m_stopPreviewOp.Status() == AsyncStatus::Started)
	{
		concurrency::create_task([this]()
			{
				WaitForSingleObject(m_stopPreviewEventHandle.get(), INFINITE);
			}).get();
	}

	ReleaseDeviceResources();

    Module::Shutdown();
}

hresult CaptureEngine::StartPreview(uint32_t width, uint32_t height, bool enableAudio, bool enableMrc)
{
	auto guard = CriticalSectionGuard(m_cs);

	if (m_startPreviewOp != nullptr || m_stopPreviewOp != nullptr)
	{
		IFR(E_ABORT);
	}

	ResetEvent(m_startPreviewEventHandle.get());

	IFR(CreateDeviceResources());

	m_startPreviewOp = StartPreviewCoroutine(width, height, enableAudio, enableMrc);
    m_startPreviewOp.Completed([this](auto const asyncOp, AsyncStatus const status)
    {
        UNREFERENCED_PARAMETER(asyncOp);

		auto guard = CriticalSectionGuard(m_cs);

        m_startPreviewOp = nullptr;

        if (status == AsyncStatus::Error)
        {
            Failed();
        }
        else if (status == AsyncStatus::Completed)
        {
            CALLBACK_STATE state{};
            ZeroMemory(&state, sizeof(CALLBACK_STATE));

            state.type = CallbackType::Capture;

            ZeroMemory(&state.value.captureState, sizeof(CAPTURE_STATE));

            state.value.captureState.stateType = CaptureStateType::PreviewStarted;

            Callback(state);
        }
    });

    return S_OK;
}

hresult CaptureEngine::StopPreview()
{
	auto guard = CriticalSectionGuard(m_cs);

	if (m_startPreviewOp != nullptr || m_stopPreviewOp != nullptr)
	{
		IFR(E_ABORT);
	}

	ResetEvent(m_stopPreviewEventHandle.get());

	m_stopPreviewOp = StopPreviewCoroutine();
    m_stopPreviewOp.Completed([this](auto const asyncOp, AsyncStatus const status)
    {
        UNREFERENCED_PARAMETER(asyncOp);

		auto guard = CriticalSectionGuard(m_cs);

		m_stopPreviewOp = nullptr;

        if (status == AsyncStatus::Error)
        {
            Failed();
        }
        else if (status == AsyncStatus::Completed)
        {
            CALLBACK_STATE state{};
            ZeroMemory(&state, sizeof(CALLBACK_STATE));

            state.type = CallbackType::Capture;

            ZeroMemory(&state.value.captureState, sizeof(CAPTURE_STATE));

            state.value.captureState.stateType = CaptureStateType::PreviewStopped;

            Callback(state);
        }
    });

    return S_OK;
}

// private
hresult CaptureEngine::CreateDeviceResources()
{
    if (m_mediaDevice != nullptr && m_dxgiDeviceManager != nullptr)
    {
        return S_OK;
    }

	// get the adapter from the Unity device
    auto resources = m_d3d11DeviceResources.lock();
    NULL_CHK_HR(resources, MF_E_UNEXPECTED);

	com_ptr<IDXGIDevice> dxgiDevice = nullptr;
	IFR(resources->GetDevice()->QueryInterface(guid_of<IDXGIDevice>(), dxgiDevice.put_void()));

    com_ptr<IDXGIAdapter> dxgiAdapter = nullptr;
    IFR(dxgiDevice->GetAdapter(dxgiAdapter.put()));

    com_ptr<ID3D11Device> mediaDevice = nullptr;
    IFR(CreateMediaDevice(dxgiAdapter.get(), mediaDevice.put()));

    // create DXGIManager
    uint32_t resetToken;
    com_ptr<IMFDXGIDeviceManager> dxgiDeviceManager = nullptr;
    IFR(MFCreateDXGIDeviceManager(&resetToken, dxgiDeviceManager.put()));

    // associate device with dxgiManager
    IFR(dxgiDeviceManager->ResetDevice(mediaDevice.get(), resetToken));

    // success, store the values
    m_mediaDevice.attach(mediaDevice.detach());
    m_dxgiDeviceManager.attach(dxgiDeviceManager.detach());
    m_resetToken = resetToken;

    return S_OK;
}

void CaptureEngine::ReleaseDeviceResources()
{
    if (m_audioSample != nullptr)
    {
        m_audioSample = nullptr;
    }

    if (m_sharedVideoTexture != nullptr)
    {
        m_sharedVideoTexture->Reset();

        m_sharedVideoTexture = nullptr;
    }

    if (m_dxgiDeviceManager != nullptr)
    {
        m_dxgiDeviceManager = nullptr;
    }

    if (m_mediaDevice != nullptr)
    {
        m_mediaDevice = nullptr;
    }
}


CameraCapture::Media::PayloadHandler CaptureEngine::PayloadHandler()
{
	auto guard = CriticalSectionGuard(m_cs);

	return m_payloadHandler;
}
void CaptureEngine::PayloadHandler(CameraCapture::Media::PayloadHandler const& value)
{
	auto guard = CriticalSectionGuard(m_cs);

	ResetPayloadHandler();

	m_payloadHandler = value;

	if (m_mediaSink != nullptr)
	{
		m_mediaSink.PayloadHandler(m_payloadHandler);
	}

	m_profileEventToken = m_payloadHandler.OnMediaProfile([this](auto const sender, MediaEncodingProfile const& profile)
		{
			UNREFERENCED_PARAMETER(sender);

			if (m_isShutdown)
			{
				return;
			}

			bool hasAudio = false;
			if (profile.Audio() != nullptr)
			{
				hasAudio = true;
			}

			bool hasVideo = false;
			if (profile.Video() != nullptr)
			{
				hasVideo = true;
			}

			Log(L"Has Audio: %s, Has Video: %s\n",
				hasAudio ? L"Yes" : L"No",
				hasVideo ? L"Yes" : L"No");
		});

	m_streamDescriptionEventToken = m_payloadHandler.OnStreamDescription([this](auto const sender, IMediaEncodingProperties const& description)
		{
			UNREFERENCED_PARAMETER(sender);
			UNREFERENCED_PARAMETER(description);

			if (m_isShutdown)
			{
				return;
			}
		});

	m_streamMetaDataEventToken = m_payloadHandler.OnStreamMetadata([this](auto const sender, MediaPropertySet const& metaData)
		{
			UNREFERENCED_PARAMETER(sender);

			if (m_isShutdown)
			{
				return;
			}

			auto payloadType = metaData.TryLookup(MF_PAYLOAD_MARKER_TYPE);
			if (payloadType != nullptr)
			{
				auto type = static_cast<MFSTREAMSINK_MARKER_TYPE>(unbox_value<uint32_t>(payloadType));
				if (type == MFSTREAMSINK_MARKER_TYPE::MFSTREAMSINK_MARKER_ENDOFSEGMENT)
				{
					// notify End of Segment
					Log(L"End of Segment\n");
				}
				else if (type == MFSTREAMSINK_MARKER_TYPE::MFSTREAMSINK_MARKER_TICK)
				{
					auto timestamp = unbox_value<int64_t>(metaData.Lookup(MF_PAYLOAD_MARKER_TICK_TIMESTAMP));

					Log(L"Tick: %d\n", timestamp);
				}
			}

			auto flushType = metaData.TryLookup(MF_PAYLOAD_FLUSH);
			if (flushType != nullptr)
			{
				Log(L"Flush\n");
			}
		});

	m_streamSampleEventToken = m_payloadHandler.OnStreamSample([this](auto const sender, MediaStreamSample const& description)
		{
			UNREFERENCED_PARAMETER(sender);
			UNREFERENCED_PARAMETER(description);

			if (m_isShutdown)
			{
				return;
			}

		});

	m_streamSampleEventToken = m_payloadHandler.OnStreamPayload([this](auto const sender, Media::Payload const& payload)
		{
			UNREFERENCED_PARAMETER(sender);

			if (m_isShutdown)
			{
				return;
			}

			if (sender != m_payloadHandler)
			{
				return;
			}

			if (payload == nullptr)
			{
				return;
			}

			GUID majorType = GUID_NULL;

			auto mediaStreamSample = payload.MediaStreamSample();
			if (mediaStreamSample != nullptr)
			{
				auto type = mediaStreamSample.ExtendedProperties().TryLookup(MF_MT_MAJOR_TYPE);
				if (type != nullptr)
				{
					majorType = winrt::unbox_value<guid>(type);
				}
			}

			auto streamSample = payload.as<IStreamSample>();
			if (streamSample == nullptr)
			{
				return;
			}

			if (MFMediaType_Audio == majorType)
			{
				if (m_audioSample == nullptr)
				{
					DWORD bufferSize = 0;
					IFV(streamSample->Sample()->GetTotalLength(&bufferSize));

					com_ptr<IMFMediaBuffer> dstBuffer = nullptr;
					IFV(MFCreateMemoryBuffer(bufferSize, dstBuffer.put()));

					com_ptr<IMFSample> dstSample = nullptr;
					IFV(MFCreateSample(dstSample.put()));

					IFV(dstSample->AddBuffer(dstBuffer.get()));

					m_audioSample.attach(dstSample.detach());
				}

				IFV(CopySample(MFMediaType_Audio, streamSample->Sample(), m_audioSample));

				CALLBACK_STATE state{};
				ZeroMemory(&state, sizeof(CALLBACK_STATE));

				state.type = CallbackType::Capture;

				ZeroMemory(&state.value.captureState, sizeof(CAPTURE_STATE));

				state.value.captureState.stateType = CaptureStateType::PreviewAudioFrame;
				//state.value.captureState.width = 0;
				//state.value.captureState.height = 0;
				//state.value.captureState.texturePtr = nullptr;
				Callback(state);
			}
			else if (MFMediaType_Video == majorType)
			{
				boolean bufferChanged = false;

				auto videoProps = payload.EncodingProperties().as<IVideoEncodingProperties>();

				if (m_sharedVideoTexture == nullptr
					||
					m_sharedVideoTexture->frameTexture == nullptr
					||
					m_sharedVideoTexture->frameTextureDesc.Width != videoProps.Width()
					||
					m_sharedVideoTexture->frameTextureDesc.Height != videoProps.Height())
				{
					auto resources = m_d3d11DeviceResources.lock();
					NULL_CHK_R(resources);

					// make sure we have created our own d3d device
					IFV(CreateDeviceResources());

					IFV(SharedTexture::Create(resources->GetDevice(), m_dxgiDeviceManager, videoProps.Width(), videoProps.Height(), m_sharedVideoTexture));

					bufferChanged = true;
				}

				// copy the data
				IFV(CopySample(MFMediaType_Video, streamSample->Sample(), m_sharedVideoTexture->mediaSample));

				// did the texture description change, if so, raise callback
				CALLBACK_STATE state{};
				ZeroMemory(&state, sizeof(CALLBACK_STATE));

				state.type = CallbackType::Capture;

				ZeroMemory(&state.value.captureState, sizeof(CAPTURE_STATE));

				state.value.captureState.stateType = CaptureStateType::PreviewVideoFrame;
				state.value.captureState.width = m_sharedVideoTexture->frameTextureDesc.Width;
				state.value.captureState.height = m_sharedVideoTexture->frameTextureDesc.Height;
				state.value.captureState.texturePtr = m_sharedVideoTexture->frameTextureSRV.get();

				// if there is transform change, update matricies
				if (m_appCoordinateSystem != nullptr)
				{
					if (SUCCEEDED(m_sharedVideoTexture->UpdateTransforms(m_appCoordinateSystem)))
					{
						state.value.captureState.worldMatrix = m_sharedVideoTexture->cameraToWorldTransform;
						state.value.captureState.projectionMatrix = m_sharedVideoTexture->cameraProjectionMatrix;

						bufferChanged = true;
					}
				}

				if (bufferChanged)
				{
					Callback(state);
				}
			}
		});
}

CameraCapture::Media::Capture::Sink CaptureEngine::MediaSink()
{
	auto guard = CriticalSectionGuard(m_cs);

	return m_mediaSink;
}

Windows::Perception::Spatial::SpatialCoordinateSystem CaptureEngine::AppCoordinateSystem()
{
	auto guard = CriticalSectionGuard(m_cs);

	return m_appCoordinateSystem;
}
void CaptureEngine::AppCoordinateSystem(Windows::Perception::Spatial::SpatialCoordinateSystem const& value)
{
	auto guard = CriticalSectionGuard(m_cs);

	m_appCoordinateSystem = value;
}


IAsyncAction CaptureEngine::StartPreviewCoroutine(
    uint32_t const width, uint32_t const height,
    boolean const enableAudio, boolean const enableMrc)
{
	winrt::apartment_context calling_thread;

    co_await resume_background();

	{
		auto guard = CriticalSectionGuard(m_cs);

		if (m_mediaCapture == nullptr)
		{
			co_await CreateMediaCaptureAsync(enableAudio, width, height);
		}
		else
		{
			co_await RemoveMrcEffectsAsync();
		}
	}
	
	// set video controller properties
	auto videoController = m_mediaCapture.VideoDeviceController();
	videoController.DesiredOptimization(Windows::Media::Devices::MediaCaptureOptimization::LatencyThenQuality);

	// override video controller media stream properties
	if (m_initSettings.SharingMode() == MediaCaptureSharingMode::ExclusiveControl)
	{
		auto videoEncProps = GetVideoDeviceProperties(videoController, m_streamType, width, height, MediaEncodingSubtypes::Nv12());
		co_await videoController.SetMediaStreamPropertiesAsync(m_streamType, videoEncProps);

		auto captureSettings = m_mediaCapture.MediaCaptureSettings();
		if (m_streamType != MediaStreamType::VideoPreview
			&&
			captureSettings.VideoDeviceCharacteristic() != VideoDeviceCharacteristic::AllStreamsIdentical
			&&
			captureSettings.VideoDeviceCharacteristic() != VideoDeviceCharacteristic::PreviewRecordStreamsIdentical)
		{
			videoEncProps = GetVideoDeviceProperties(videoController, MediaStreamType::VideoRecord, width, height, MediaEncodingSubtypes::Nv12());
			co_await videoController.SetMediaStreamPropertiesAsync(MediaStreamType::VideoRecord, videoEncProps);
		}
	}

	// encoding profile based on 720p
    auto encodingProfile = MediaEncodingProfile::CreateMp4(VideoEncodingQuality::HD720p);
    encodingProfile.Container(nullptr);

	// update the audio profile to match device
	if (enableAudio)
	{
		auto audioController = m_mediaCapture.AudioDeviceController();
		auto audioMediaProperties = audioController.GetMediaStreamProperties(MediaStreamType::Audio);
		auto audioMediaProperty = audioMediaProperties.as<AudioEncodingProperties>();

		encodingProfile.Audio().Bitrate(audioMediaProperty.Bitrate());
		encodingProfile.Audio().BitsPerSample(audioMediaProperty.BitsPerSample());
		encodingProfile.Audio().ChannelCount(audioMediaProperty.ChannelCount());
		encodingProfile.Audio().SampleRate(audioMediaProperty.SampleRate());
		if (m_streamType == MediaStreamType::VideoPreview) // for local playback only
		{
			encodingProfile.Audio().Subtype(MediaEncodingSubtypes::Float());
		}
	}
	else
	{
		encodingProfile.Audio(nullptr);
	}

    auto videoMediaProperty = videoController.GetMediaStreamProperties(m_streamType).as<VideoEncodingProperties>();
    if (videoMediaProperty != nullptr)
    {
        encodingProfile.Video().Width(videoMediaProperty.Width());
        encodingProfile.Video().Height(videoMediaProperty.Height());
        if (m_streamType == MediaStreamType::VideoPreview) // for local playback only
        {
            encodingProfile.Video().Subtype(MediaEncodingSubtypes::Bgra8());
        }
    }

    // media sink
    auto mediaSink = make<Sink>(encodingProfile);

    // create mrc effects first
    if (enableMrc)
    {
        co_await AddMrcEffectsAsync(enableAudio);
    }

    if (m_streamType == MediaStreamType::VideoRecord)
    {
        co_await m_mediaCapture.StartRecordToCustomSinkAsync(encodingProfile, mediaSink);
    }
    else if (m_streamType == MediaStreamType::VideoPreview)
    {
        co_await m_mediaCapture.StartPreviewToCustomSinkAsync(encodingProfile, mediaSink);

        auto previewFrame = co_await m_mediaCapture.GetPreviewFrameAsync();
    }

    // store locals
	m_mediaSink = mediaSink;

	if (m_payloadHandler != nullptr)
	{
		m_mediaSink.PayloadHandler(m_payloadHandler);
	}

	SetEvent(m_startPreviewEventHandle.get());

	// if the calling thread has an outstanding call that is waiting, this will ASSERT
	// TODO: refactor callback for plugin in a way this can be avoided
	if (!m_isShutdown)
	{
		co_await calling_thread;
	}
 }

IAsyncAction CaptureEngine::StopPreviewCoroutine()
{
	winrt::apartment_context calling_thread;

	co_await resume_background();

	ResetPayloadHandler();

	if (m_mediaSink != nullptr)
	{
		m_mediaSink.PayloadHandler(nullptr);
		m_mediaSink = nullptr;
	}

	if (m_mediaCapture != nullptr)
	{
		if (m_mediaCapture.CameraStreamState() == Windows::Media::Devices::CameraStreamState::Streaming)
		{
			if (m_streamType == MediaStreamType::VideoRecord)
			{
				co_await m_mediaCapture.StopRecordAsync();
			}
			else if (m_streamType == MediaStreamType::VideoPreview)
			{
				co_await m_mediaCapture.StopPreviewAsync();
			}
		}
		co_await ReleaseMediaCaptureAsync();
	}

	SetEvent(m_stopPreviewEventHandle.get());

	// if the calling thread has an outstanding call that is waiting, this will ASSERT
	// TODO: refactor callback for plugin in a way this can be avoided
	if (!m_isShutdown)
	{
		co_await calling_thread;
	}
}


IAsyncAction CaptureEngine::CreateMediaCaptureAsync(
	boolean const& enableAudio, 
	uint32_t const& width, 
	uint32_t const& height)
{
    if (m_mediaCapture != nullptr)
    {
        co_return;
    }

    auto audioDevice = co_await GetFirstDeviceAsync(Windows::Devices::Enumeration::DeviceClass::AudioCapture);
	auto videoDevice = co_await GetFirstDeviceAsync(Windows::Devices::Enumeration::DeviceClass::VideoCapture);

	// initialize settings
	auto initSettings = MediaCaptureInitializationSettings();
	initSettings.MemoryPreference(MediaCaptureMemoryPreference::Auto);
	initSettings.StreamingCaptureMode(enableAudio ? StreamingCaptureMode::AudioAndVideo : StreamingCaptureMode::Video);
	initSettings.MediaCategory(m_category);
	initSettings.VideoDeviceId(videoDevice.Id());
	if (enableAudio)
	{
		initSettings.AudioDeviceId(audioDevice.Id());
	}

	// which stream should photo capture use
	if (m_streamType == MediaStreamType::VideoPreview)
	{
		initSettings.PhotoCaptureSource(PhotoCaptureSource::VideoPreview);
	}
	else
	{
		initSettings.PhotoCaptureSource(PhotoCaptureSource::Auto);
	}

	// set the DXGIManger for the media capture
	auto advancedInitSettings = initSettings.as<IAdvancedMediaCaptureInitializationSettings>();
	IFT(advancedInitSettings->SetDirectxDeviceManager(m_dxgiDeviceManager.get()));

	// if profiles are supported
	if (MediaCapture::IsVideoProfileSupported(videoDevice.Id()))
	{
		initSettings.SharingMode(MediaCaptureSharingMode::SharedReadOnly);

		setlocale(LC_ALL, "");

		// set the profile / mediaDescription that matches
		MediaCaptureVideoProfile videoProfile = nullptr;
		MediaCaptureVideoProfileMediaDescription videoProfileMediaDescription = nullptr;
		auto profiles = MediaCapture::FindKnownVideoProfiles(videoDevice.Id(), m_videoProfile);
		for (auto const& profile : profiles)
		{
			auto const& videoProfileMediaDescriptions = m_streamType == (MediaStreamType::VideoPreview) ? profile.SupportedPreviewMediaDescription() : profile.SupportedRecordMediaDescription();
			auto const& found = std::find_if(begin(videoProfileMediaDescriptions), end(videoProfileMediaDescriptions), [&](MediaCaptureVideoProfileMediaDescription const& desc)
				{
					Log(L"\tFormat: %s: %i x %i @ %f fps",
						desc.Subtype().c_str(),
						desc.Width(),
						desc.Height(),
						desc.FrameRate());

					// store a default
					if (videoProfile == nullptr)
					{
						videoProfile = profile;
					}

					if (videoProfileMediaDescription == nullptr)
					{
						videoProfileMediaDescription = desc;
					}

					// select a size that will be == width/height @ 30fps, final size will be set with enc props
					bool match =
						_wcsicmp(desc.Subtype().c_str(), MediaEncodingSubtypes::Nv12().c_str()) == 0 &&
						desc.Width() == width &&
						desc.Height() == height &&
						desc.FrameRate() == 30.0;
					if (match)
					{
						Log(L" - found\n");
					}
					else
					{
						Log(L"\n");
					}

					return match;
				});

			if (found != end(videoProfileMediaDescriptions))
			{
				videoProfile = profile;
				videoProfileMediaDescription = *found;
				break;
			}
		}

		initSettings.VideoProfile(videoProfile);
		if (m_streamType == MediaStreamType::VideoPreview)
		{
			initSettings.PreviewMediaDescription(videoProfileMediaDescription);
		}
		else
		{
			initSettings.RecordMediaDescription(videoProfileMediaDescription);
		}
	}
	else
	{
		initSettings.SharingMode(MediaCaptureSharingMode::ExclusiveControl);
	}

    auto mediaCapture = Windows::Media::Capture::MediaCapture();
    co_await mediaCapture.InitializeAsync(initSettings);

    m_mediaCapture = mediaCapture;
	m_initSettings = initSettings;
}

IAsyncAction CaptureEngine::ReleaseMediaCaptureAsync()
{
    if (m_mediaCapture == nullptr)
    {
        co_return;
    }

    co_await RemoveMrcEffectsAsync();

	if (m_initSettings != nullptr)
	{
		m_initSettings.as<IAdvancedMediaCaptureInitializationSettings>()->SetDirectxDeviceManager(nullptr);

		m_initSettings = nullptr;
	}

    m_mediaCapture.Close();

    m_mediaCapture = nullptr;
}


IAsyncAction CaptureEngine::AddMrcEffectsAsync(
    boolean const enableAudio)
{
    if (m_mediaCapture == nullptr)
    {
        co_return;
    }

    auto captureSettings = m_mediaCapture.MediaCaptureSettings();

    try
    {
        auto mrcVideoEffect = make<MrcVideoEffect>().as<IVideoEffectDefinition>();
        if (captureSettings.VideoDeviceCharacteristic() == VideoDeviceCharacteristic::AllStreamsIdentical ||
            captureSettings.VideoDeviceCharacteristic() == VideoDeviceCharacteristic::PreviewRecordStreamsIdentical)
        {
            // This effect will modify both the preview and the record streams
            m_mrcVideoEffect = co_await m_mediaCapture.AddVideoEffectAsync(mrcVideoEffect, MediaStreamType::VideoRecord);
        }
        else
        {
            m_mrcVideoEffect = co_await m_mediaCapture.AddVideoEffectAsync(mrcVideoEffect, MediaStreamType::VideoRecord);
            m_mrcPreviewEffect = co_await m_mediaCapture.AddVideoEffectAsync(mrcVideoEffect, MediaStreamType::VideoPreview);
        }

        if (enableAudio)
        {
            auto mrcAudioEffect = make<MrcAudioEffect>().as<IAudioEffectDefinition>();

            m_mrcAudioEffect = co_await m_mediaCapture.AddAudioEffectAsync(mrcAudioEffect);
        }
    }
    catch (hresult_error const& e)
    {
        Log(L"failed to add Mrc effects to streams: %s", e.message().c_str());
    }

    co_return;
}

IAsyncAction CaptureEngine::RemoveMrcEffectsAsync()
{
    if (m_mediaCapture == nullptr)
    {
        co_return;
    }

	if (m_mrcAudioEffect != nullptr || m_mrcPreviewEffect != nullptr || m_mrcVideoEffect != nullptr)
	{
		if (m_mrcAudioEffect != nullptr)
		{
			co_await m_mediaCapture.RemoveEffectAsync(m_mrcAudioEffect);
			m_mrcAudioEffect = nullptr;
		}

		if (m_mrcPreviewEffect != nullptr)
		{
			co_await m_mediaCapture.RemoveEffectAsync(m_mrcPreviewEffect);
			m_mrcPreviewEffect = nullptr;
		}

		if (m_mrcVideoEffect != nullptr)
		{
			co_await m_mediaCapture.RemoveEffectAsync(m_mrcVideoEffect);
			m_mrcVideoEffect = nullptr;
		}
	}
	else
	{
		co_return;
	}
}


void CaptureEngine::ResetPayloadHandler()
{
	if (m_payloadHandler != nullptr)
	{
		m_payloadHandler.OnMediaProfile(m_profileEventToken);
		m_payloadHandler.OnStreamPayload(m_payloadEventToken);
		m_payloadHandler.OnStreamSample(m_streamSampleEventToken);
		m_payloadHandler.OnStreamMetadata(m_streamMetaDataEventToken);
		m_payloadHandler.OnStreamDescription(m_streamDescriptionEventToken);

		m_payloadHandler = nullptr;
	}
}