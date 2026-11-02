#include "stdafx.h"
#include "pbd_clips.h"
#include "Emu/PBD/pbd_clip_buffer.h"
#include "Utilities/StrUtil.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include "pbd_clip_frames.h"
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#endif

#include "Emu/PBD/pbd_clip_destination.h"

LOG_CHANNEL(clips_log, "PBD Clips");

namespace pbd::clips
{

	struct recorder::impl
	{
		recorder& owner;
		std::function<void(status)> publish;
		std::mutex mutex;
		std::condition_variable wake;
		// Readbacks finished at the same fence check arrive together; they wait for the encoder in order. About 130 ms
		// of frames covers a slow file write or a late encoder input request; only a full queue loses a frame (counted).
		static constexpr std::size_t queued_frames = 8;
		std::deque<utils::video_sink::encoder_frame> frames, waiting;
		std::deque<utils::video_sink::encoder_sample> samples;
		options config, next;
		bool changed = false, stopping = false, save = false, toggle = false;
		std::atomic<bool> accepting{false};
		std::atomic<std::uint64_t> dropped{0};
		status state;
		buffer recent;
		std::unique_ptr<mp4> recording;
		std::filesystem::path recording_path;
		bool awaiting_keyframe = false;
		clock::time_point record_deadline{}, last_publish{};
		std::thread worker;
		AVCodecContext* audio = nullptr;
		SwrContext* resampler = nullptr;
		AVAudioFifo* fifo = nullptr;
		SwsContext* scaler = nullptr;
		std::int64_t audio_start = -1, audio_samples = 0;
		std::vector<u8> h264;
		struct saved_job
		{
			options config;
			std::vector<packet_ptr> packets;
			std::vector<u8> h264;
		};
		std::deque<saved_job> saves;
		std::mutex saves_mutex;
		std::condition_variable saves_wake;
		std::thread saver;
		bool saver_stopping = false;
#ifdef _WIN32
		using MicrosoftPtr = Microsoft::WRL::ComPtr<IMFTransform>;
		MicrosoftPtr encoder;
		Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events;
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager;
		input_frames surfaces;
		// Kept for encoders that leave output samples to the caller.
		Microsoft::WRL::ComPtr<IMFSample> output_sample;
		DWORD output_size = 0;
		unsigned input_credit = 0;
		std::optional<usz> keyframe_ms;
		bool gpu_input = false;
		template <class T>
		using ptr = Microsoft::WRL::ComPtr<T>;
		void check(HRESULT result)
		{
			require(SUCCEEDED(result));
		}
		static std::string text(IMFAttributes* from, const GUID& key)
		{
			WCHAR* value = nullptr;
			UINT32 length = 0;
			if (FAILED(from->GetAllocatedString(key, &value, &length)))
				return "unknown";
			std::string result = wchar_to_utf8({value, length});
			CoTaskMemFree(value);
			return result;
		}
		void property(const GUID& key, unsigned value)
		{
			ptr<ICodecAPI> api;
			if (SUCCEEDED(encoder.As(&api)))
			{
				VARIANT option{};
				option.vt = VT_UI4;
				option.ulVal = value;
				api->SetValue(&key, &option);
			}
		}
		void open_video()
		{
			check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
				D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
				D3D11_SDK_VERSION, &device, nullptr, &context));
			ptr<ID3D10Multithread> multithread;
			if (SUCCEEDED(device.As(&multithread)))
				multithread->SetMultithreadProtected(TRUE);
			UINT token = 0;
			check(MFCreateDXGIDeviceManager(&token, &manager));
			check(manager->ResetDevice(device.Get(), token));
			IMFActivate** choices = nullptr;
			UINT32 count = 0;
			bool on_card = false;
			check(h264_encoders(device.Get(), &choices, &count, on_card));
			std::string name = "none", maker = "none";
			for (UINT i = 0; i < count; ++i)
			{
				if (!encoder)
				{
					ptr<IMFTransform> candidate;
					if (SUCCEEDED(choices[i]->ActivateObject(IID_PPV_ARGS(&candidate))))
					{
						encoder = candidate;
						name = text(choices[i], MFT_FRIENDLY_NAME_Attribute);
						maker = text(choices[i], MFT_ENUM_HARDWARE_VENDOR_ID_Attribute);
					}
				}
				choices[i]->Release();
			}
			CoTaskMemFree(choices);
			ptr<IMFAttributes> attributes;
			UINT32 asynchronous = 0, aware = 0;
			if (encoder && SUCCEEDED(encoder->GetAttributes(&attributes)))
			{
				attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous);
				attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware);
			}
			// A report needs both the card and the encoder to tell a driver problem from a mismatch.
			std::string card = "unknown";
			DXGI_ADAPTER_DESC adapter{};
			ptr<IDXGIDevice> graphics;
			ptr<IDXGIAdapter> display;
			if (SUCCEEDED(device.As(&graphics)) && SUCCEEDED(graphics->GetAdapter(&display)) && SUCCEEDED(display->GetDesc(&adapter)))
				card = wchar_to_utf8(adapter.Description);
			clips_log.notice("Clip recorder: graphics card \"%s\" (vendor 0x%04x, device 0x%04x), H.264 encoder \"%s\" (%s, %s, %s, reads %s)",
				card, adapter.VendorId, adapter.DeviceId, name, maker, on_card ? "on this card" : "card not matched",
				asynchronous ? "asynchronous" : "synchronous", aware ? "textures" : "memory");
			require(encoder && attributes && asynchronous != 0);
			check(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
			ptr<IMFMediaType> raw;
			check(MFCreateMediaType(&raw));
			check(raw->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
			check(raw->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
			check(raw->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
			check(MFSetAttributeSize(raw.Get(), MF_MT_FRAME_SIZE, width, height));
			check(MFSetAttributeRatio(raw.Get(), MF_MT_FRAME_RATE, 60, 1));
			check(MFSetAttributeRatio(raw.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
			// Textures go only to an encoder on the device's card, and only from the frame pool; otherwise frames go in system
			// memory, as they do for an encoder that reads no textures.
			gpu_input = aware != 0 && on_card;
			if (gpu_input)
			{
				if (const HRESULT pool = surfaces.open(manager.Get(), device.Get(), raw.Get()); FAILED(pool))
				{
					clips_log.warning("Clip recorder: no texture frames for the encoder (0x%08x); frames go through system memory", static_cast<u32>(pool));
					gpu_input = false;
				}
			}
			if (gpu_input)
				check(encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())));
			check(encoder.As(&events));
			property(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
			property(CODECAPI_AVEncCommonMeanBitRate, 8'000'000);
			property(CODECAPI_AVEncMPVGOPSize, 60);
			property(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
			ptr<ICodecAPI> api;
			if (SUCCEEDED(encoder.As(&api)))
			{
				VARIANT low{};
				low.vt = VT_BOOL;
				low.boolVal = VARIANT_TRUE;
				api->SetValue(&CODECAPI_AVLowLatencyMode, &low);
			}
			ptr<IMFMediaType> encoded;
			check(MFCreateMediaType(&encoded));
			check(encoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
			check(encoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
			check(encoded->SetUINT32(MF_MT_AVG_BITRATE, 8'000'000));
			check(encoded->SetUINT32(MF_MT_MPEG2_PROFILE, 66)); // H.264 Baseline forbids reordered B pictures.
			check(encoded->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
			check(MFSetAttributeSize(encoded.Get(), MF_MT_FRAME_SIZE, width, height));
			check(MFSetAttributeRatio(encoded.Get(), MF_MT_FRAME_RATE, 60, 1));
			check(MFSetAttributeRatio(encoded.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
			check(encoder->SetOutputType(0, encoded.Get(), 0));
			check(encoder->SetInputType(0, raw.Get(), 0));
			property(CODECAPI_AVEncMPVGOPSize, 60);
			property(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
			check(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
			check(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
			state.encoder = "Media Foundation hardware H.264";
		}
		// False leaves the frame to the caller for later: the encoder still holds every pooled frame, and a released one comes
		// back to the pool a moment after the release.
		bool submit_video(const utils::video_sink::encoder_frame& value)
		{
			if (!input_credit)
			{
				++dropped;
				return true;
			}
			scaler = sws_getCachedContext(scaler, value.width, value.height, static_cast<AVPixelFormat>(value.av_pixel_format),
				width, height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
			require(scaler);
			const u8* source[]{value.data.data(), nullptr, nullptr, nullptr};
			const int stride[]{static_cast<int>(value.pitch), 0, 0, 0};
			// Scales straight into the encoder's frame.
			const auto scale = [&](u8* luma, u8* chroma, std::size_t pitch)
			{
				u8* target[]{luma, chroma, nullptr, nullptr};
				const int pitches[]{static_cast<int>(pitch), static_cast<int>(pitch), 0, 0};
				return sws_scale(scaler, source, stride, 0, value.height, target, pitches) == static_cast<int>(height);
			};
			ptr<IMFSample> sample;
			if (gpu_input)
			{
				const HRESULT result = surfaces.next(sample, scale);
				if (result == MF_E_SAMPLEALLOCATOR_EMPTY)
					return false;
				check(result);
			}
			else
			{
				constexpr DWORD size = width * height * 3 / 2;
				ptr<IMFMediaBuffer> bytes;
				check(MFCreateMemoryBuffer(size, &bytes));
				BYTE* data = nullptr;
				check(bytes->Lock(&data, nullptr, nullptr));
				const bool scaled = scale(data, data + width * height, width);
				bytes->Unlock();
				require(scaled);
				check(bytes->SetCurrentLength(size));
				check(MFCreateSample(&sample));
				check(sample->AddBuffer(bytes.Get()));
			}
			if (!keyframe_ms || value.timestamp_ms - *keyframe_ms >= 1000)
			{
				property(CODECAPI_AVEncVideoForceKeyFrame, 1);
				keyframe_ms = value.timestamp_ms;
			}
			check(sample->SetSampleTime(static_cast<LONGLONG>(value.timestamp_ms) * 10'000));
			check(sample->SetSampleDuration(10'000'000 / 60));
			check(encoder->ProcessInput(0, sample.Get(), 0));
			--input_credit;
			return true;
		}
		void output_video()
		{
			MFT_OUTPUT_STREAM_INFO info{};
			check(encoder->GetOutputStreamInfo(0, &info));
			const bool own = !(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);
			// No packet has this time, so a packet the encoder did not stamp is caught rather than taking the last one's.
			constexpr LONGLONG unstamped = std::numeric_limits<LONGLONG>::min();
			if (own)
			{
				// One sample with one buffer takes every packet, whose bytes are copied out before the next. What the last
				// packet left on it, its keyframe mark and time above all, is cleared first.
				const DWORD size = std::max<DWORD>(info.cbSize, 2 * 1024 * 1024);
				DWORD buffers = 0;
				if (!output_sample || output_size < size || FAILED(output_sample->GetBufferCount(&buffers)) || buffers != 1)
				{
					ptr<IMFMediaBuffer> data;
					check(MFCreateMemoryBuffer(size, &data));
					ptr<IMFSample> fresh;
					check(MFCreateSample(&fresh));
					check(fresh->AddBuffer(data.Get()));
					output_sample = std::move(fresh);
					output_size = size;
				}
				ptr<IMFMediaBuffer> data;
				check(output_sample->GetBufferByIndex(0, &data));
				check(data->SetCurrentLength(0));
				check(output_sample->DeleteAllItems());
				check(output_sample->SetSampleTime(unstamped));
				check(output_sample->SetSampleDuration(10'000'000 / 60));
			}
			MFT_OUTPUT_DATA_BUFFER output{};
			output.pSample = own ? output_sample.Get() : nullptr;
			DWORD flags = 0;
			const auto result = encoder->ProcessOutput(0, 1, &output, &flags);
			// Owned from here, so a failure below releases them.
			ptr<IMFCollection> unused;
			unused.Attach(output.pEvents);
			ptr<IMFSample> sample;
			if (own)
				sample = output_sample;
			else
				sample.Attach(output.pSample);
			if (result == MF_E_TRANSFORM_STREAM_CHANGE)
			{
				ptr<IMFMediaType> type;
				check(encoder->GetOutputAvailableType(0, 0, &type));
				check(encoder->SetOutputType(0, type.Get(), 0));
				output_video();
				return;
			}
			check(result);
			require(sample);
			if (h264.empty())
			{
				ptr<IMFMediaType> type;
				check(encoder->GetOutputCurrentType(0, &type));
				UINT32 size = 0;
				if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size && size < 65536)
				{
					h264.resize(size);
					check(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, h264.data(), size, nullptr));
				}
			}
			LONGLONG time = 0, duration = 10'000'000 / 60;
			UINT32 key = 0;
			check(sample->GetSampleTime(&time));
			require(time != unstamped);
			sample->GetSampleDuration(&duration);
			sample->GetUINT32(MFSampleExtension_CleanPoint, &key);
			ptr<IMFMediaBuffer> data;
			check(sample->ConvertToContiguousBuffer(&data));
			BYTE* source = nullptr;
			DWORD size = 0;
			check(data->Lock(&source, nullptr, &size));
			auto value = std::make_shared<packet>();
			value->bytes.assign(source, source + size);
			data->Unlock();
			value->time_us = time / 10;
			value->duration_us = duration / 10;
			value->keyframe = key != 0;
			accept_packet(std::move(value));
		}
		void pump_video()
		{
			for (unsigned count = 0; count < 32; ++count)
			{
				ptr<IMFMediaEvent> event;
				const auto result = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
				if (result == MF_E_NO_EVENTS_AVAILABLE)
					return;
				check(result);
				MediaEventType type{};
				check(event->GetType(&type));
				HRESULT event_status = S_OK;
				check(event->GetStatus(&event_status));
				check(event_status);
				if (type == METransformNeedInput)
					input_credit = std::min(input_credit + 1, 8u);
				if (type == METransformHaveOutput)
					output_video();
			}
		}
#endif
		impl(recorder& parent, options value, std::function<void(status)> callback)
			: owner(parent), publish(std::move(callback)), config(std::move(value)), next(config)
		{
			worker = std::thread([this]
				{
					run();
				});
		}
		void notify()
		{
			state.background = state.available && config.background;
			state.recording = recording || awaiting_keyframe;
			state.dropped = dropped.load();
			if (publish)
				publish(state);
			last_publish = clock::now();
		}
		void open_audio()
		{
			if (!config.channels)
				return;
			const auto* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
			require(codec);
			audio = avcodec_alloc_context3(codec);
			require(audio);
			audio->sample_rate = 48000;
			audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
			audio->bit_rate = 192000;
			av_channel_layout_default(&audio->ch_layout, 2);
			audio->time_base = {1, 48000};
			audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			require(avcodec_open2(audio, codec, nullptr) >= 0);
			AVChannelLayout input;
			av_channel_layout_default(&input, config.channels);
			const auto result = swr_alloc_set_opts2(&resampler, &audio->ch_layout, audio->sample_fmt, audio->sample_rate,
				&input, AV_SAMPLE_FMT_FLT, config.sample_rate, 0, nullptr);
			av_channel_layout_uninit(&input);
			require(result >= 0);
			require(swr_init(resampler) >= 0);
			fifo = av_audio_fifo_alloc(audio->sample_fmt, 2, audio->frame_size * 4);
			require(fifo);
		}
		void encode_audio(const utils::video_sink::encoder_sample& value)
		{
			if (!audio || value.channels != config.channels)
				return;
			if (audio_start < 0)
			{
				audio_start = value.timestamp_us;
				audio_samples = 0;
			}
			const auto expected = audio_start + av_rescale(audio_samples + av_audio_fifo_size(fifo), 1'000'000, audio->sample_rate);
			if (static_cast<s64>(value.timestamp_us) > expected + 100'000)
			{
				av_audio_fifo_reset(fifo);
				swr_close(resampler);
				require(swr_init(resampler) >= 0);
				audio_start = value.timestamp_us;
				audio_samples = 0;
			}
			u8** converted = nullptr;
			const int capacity = swr_get_out_samples(resampler, value.sample_count);
			require(av_samples_alloc_array_and_samples(&converted, nullptr, 2, capacity, audio->sample_fmt, 0) >= 0);
			const u8* source[]{value.data.data()};
			const int count = swr_convert(resampler, converted, capacity, source, value.sample_count);
			if (count < 0)
			{
				av_freep(&converted[0]);
				av_freep(&converted);
				throw clip_error{};
			}
			const int written = av_audio_fifo_write(fifo, reinterpret_cast<void**>(converted), count);
			av_freep(&converted[0]);
			av_freep(&converted);
			require(written == count);
			while (av_audio_fifo_size(fifo) >= audio->frame_size)
			{
				AVFrame* frame = av_frame_alloc();
				require(frame);
				frame->format = audio->sample_fmt;
				frame->sample_rate = audio->sample_rate;
				av_channel_layout_copy(&frame->ch_layout, &audio->ch_layout);
				frame->nb_samples = audio->frame_size;
				frame->pts = av_rescale(audio_start, audio->sample_rate, 1'000'000) + audio_samples;
				if (av_frame_get_buffer(frame, 0) < 0)
				{
					av_frame_free(&frame);
					throw clip_error{};
				}
				av_audio_fifo_read(fifo, reinterpret_cast<void**>(frame->data), audio->frame_size);
				audio_samples += audio->frame_size;
				const int sent = avcodec_send_frame(audio, frame);
				av_frame_free(&frame);
				require(sent >= 0);
				AVPacket* encoded = av_packet_alloc();
				require(encoded);
				while (avcodec_receive_packet(audio, encoded) >= 0)
				{
					auto packet = std::make_shared<clips::packet>();
					packet->audio = true;
					packet->time_us = av_rescale_q(encoded->pts, audio->time_base, microseconds);
					packet->duration_us = av_rescale_q(encoded->duration, audio->time_base, microseconds);
					packet->bytes.assign(encoded->data, encoded->data + encoded->size);
					av_packet_unref(encoded);
					accept_packet(std::move(packet));
				}
				av_packet_free(&encoded);
			}
		}
		void finish_recording()
		{
			awaiting_keyframe = false;
			if (recording)
			{
				if (recording->close(true))
				{
					state.saved = utf8(recording_path);
					state.error.clear();
				}
				else
					state.error = "Couldn't save the recording. Check the Clips folder.";
				recording.reset();
			}
			accepting = config.background && state.available;
			notify();
		}
		void accept_packet(packet_ptr value)
		{
			if (config.background)
				recent.push(value);
			if (awaiting_keyframe && !value->audio && value->keyframe)
			{
				std::lock_guard name_lock(output_name_mutex);
				recording_path = filename(config, true);
				recording = std::make_unique<mp4>(recording_path, h264, audio, value->time_us);
				awaiting_keyframe = false;
				notify();
			}
			if (recording)
				recording->append(value);
		}
		void save_recent()
		{
			auto packets = recent.snapshot();
			if (packets.empty())
			{
				state.error = config.background ? "The clip buffer is getting ready." : "Turn on background recording to save the last part.";
				notify();
				return;
			}
			std::lock_guard lock(saves_mutex);
			if (saves.size() >= 2)
			{
				state.error = "A clip is still saving. Try again in a moment.";
				notify();
				return;
			}
			saves.push_back({config, std::move(packets), h264});
			saves_wake.notify_one();
		}
		void save_loop()
		{
			while (true)
			{
				saved_job job;
				{
					std::unique_lock lock(saves_mutex);
					saves_wake.wait(lock, [&]
						{
							return saver_stopping || !saves.empty();
						});
					if (saves.empty())
						return;
					job = std::move(saves.front());
					saves.pop_front();
				}
				std::string saved, error;
				try
				{
					std::filesystem::path path;
					std::unique_ptr<mp4> file;
					{
						std::lock_guard name_lock(output_name_mutex);
						path = filename(job.config, false);
						file = std::make_unique<mp4>(path, job.h264, audio, job.packets.front()->time_us);
					}
					for (const auto& packet : job.packets)
						file->append(packet);
					require(file->close(true));
					saved = utf8(path);
				}
				catch (...)
				{
					error = "Couldn't save the clip. Check the Clips folder.";
				}
				{
					std::lock_guard lock(mutex);
					saved_message = std::move(saved);
					error_message = std::move(error);
					saved_result = true;
				}
				wake.notify_one();
			}
		}
		bool saved_result = false;
		std::string saved_message, error_message;
		void run()
		{
#ifdef _WIN32
			bool com = false, mf = false;
			try
			{
				require(media_foundation_present());
				check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
				com = true;
				check(MFStartup(MF_VERSION, MFSTARTUP_LITE));
				mf = true;
				open_video();
				open_audio();
				state.available = true;
				accepting = config.background;
				recent.seconds(config.seconds);
				notify();
				saver = std::thread([this]
					{
						save_loop();
					});
				while (true)
				{
					std::deque<utils::video_sink::encoder_sample> sound;
					bool save_now = false, toggle_now = false;
					{
						std::unique_lock lock(mutex);
						wake.wait_for(lock, std::chrono::milliseconds(3), [&]
							{
								return stopping || !frames.empty() || !samples.empty() || changed || save || toggle || saved_result;
							});
						if (stopping)
							break;
						if (changed)
						{
							config.background = next.background;
							config.seconds = next.seconds;
							config.folder = next.folder;
							config.label = next.label;
							changed = false;
							recent.seconds(config.seconds);
							if (!config.background)
								recent.clear();
							notify();
						}
						if (saved_result)
						{
							state.saved = std::move(saved_message);
							state.error = std::move(error_message);
							saved_result = false;
							notify();
						}
						for (auto& value : frames)
							waiting.push_back(std::move(value));
						frames.clear();
						sound.swap(samples);
						save_now = std::exchange(save, false);
						toggle_now = std::exchange(toggle, false);
					}
					if (toggle_now)
					{
						if (recording || awaiting_keyframe)
							finish_recording();
						else
						{
							awaiting_keyframe = true;
							record_deadline = clock::now() + std::chrono::minutes(60);
							property(CODECAPI_AVEncVideoForceKeyFrame, 1);
							notify();
						}
					}
					accepting = config.background || recording || awaiting_keyframe;
					pump_video();
					// The encoder asks for each input; a frame waits for that request, or for a pooled frame to come back,
					// rather than being dropped when a group of frames arrives faster.
					if (!accepting)
						waiting.clear();
					while (waiting.size() > queued_frames)
					{
						waiting.pop_front();
						++dropped;
					}
					while (!waiting.empty() && input_credit && submit_video(waiting.front()))
					{
						waiting.pop_front();
						pump_video();
					}
					for (const auto& sample : sound)
						if (accepting)
							encode_audio(sample);
					if (save_now)
						save_recent();
					if ((recording || awaiting_keyframe) && clock::now() >= record_deadline)
						finish_recording();
					if (clock::now() - last_publish >= std::chrono::seconds(1))
						notify();
				}
				finish_recording();
			}
			catch (...)
			{
				state.error = state.available ? "Clip recording stopped. Restart the game to try again." : "Needs a graphics card video encoder";
			}
			accepting = false;
			{
				std::lock_guard lock(saves_mutex);
				saver_stopping = true;
			}
			saves_wake.notify_one();
			if (saver.joinable())
				saver.join();
			recording.reset();
			if (encoder)
			{
				ptr<IMFShutdown> shutdown;
				if (SUCCEEDED(encoder.As(&shutdown)))
					shutdown->Shutdown();
			}
			events.Reset();
			encoder.Reset();
			surfaces.close();
			output_sample.Reset();
			manager.Reset();
			context.Reset();
			device.Reset();
			if (mf)
				MFShutdown();
			if (com)
				CoUninitialize();
#else
			state.error = "Needs a graphics card video encoder";
#endif
			state.available = false;
			state.recording = false;
			notify();
			if (fifo)
				av_audio_fifo_free(fifo);
			swr_free(&resampler);
			avcodec_free_context(&audio);
			sws_freeContext(scaler);
		}
	};

	recorder::recorder(options config, std::function<void(status)> publish) : m(std::make_unique<impl>(*this, std::move(config), std::move(publish)))
	{
		use_internal_audio = true;
		use_internal_video = true;
		m_framerate = 60;
	}
	recorder::~recorder()
	{
		stop();
	}
	void recorder::configure(options config)
	{
		std::lock_guard lock(m->mutex);
		m->next = std::move(config);
		m->changed = true;
		m->wake.notify_one();
	}
	void recorder::save_clip()
	{
		std::lock_guard lock(m->mutex);
		m->save = true;
		m->wake.notify_one();
	}
	void recorder::toggle_recording()
	{
		std::lock_guard lock(m->mutex);
		m->toggle = !m->toggle;
		m->wake.notify_one();
	}
	// Every presented frame is wanted while the buffer or a recording runs. A frame still waiting for the encoder
	// must not turn the next flip away: that skip happened before anything could count it.
	bool recorder::wants_frame() const
	{
		return m->accepting && !m_paused;
	}
	bool recorder::offer_frame(std::vector<u8>& frame, u32 pitch, u32 width, u32 height, s32 pixel_format, usz timestamp_ms)
	{
		if (!wants_frame())
			return false;
		if (!width || !height || width > 16384 || height > 16384 || pitch < width * 4 || frame.size() < static_cast<std::size_t>(pitch) * height)
		{
			++m->dropped;
			return false;
		}
		// The encoder thread never encodes while holding this lock, so the frame-copy thread waits only for a queue swap.
		std::lock_guard lock(m->mutex);
		if (m->frames.size() >= impl::queued_frames)
		{
			++m->dropped;
			return false;
		}
		m->frames.emplace_back(timestamp_ms, pitch, width, height, pixel_format, std::move(frame));
		m->wake.notify_one();
		return true;
	}
	void recorder::frame_dropped()
	{
		++m->dropped;
	}
	bool recorder::offer_samples(const u8* buf, u32 count, u16 channels, usz timestamp_us)
	{
		if (!m->accepting || m_paused || !buf || !count || count > 4096 || !channels || channels > 8)
			return false;
		std::unique_lock lock(m->mutex, std::try_to_lock);
		if (!lock || m->samples.size() >= 32)
		{
			++m->dropped;
			return false;
		}
		std::vector<u8> bytes(buf, buf + count * channels * sizeof(float));
		m->samples.emplace_back(timestamp_us, count, channels, std::move(bytes));
		m->wake.notify_one();
		return true;
	}
	void recorder::stop(bool)
	{
		if (!m || !m->worker.joinable())
			return;
		{
			std::lock_guard lock(m->mutex);
			m->stopping = true;
			m->accepting = false;
		}
		m->wake.notify_one();
		m->worker.join();
	}
	void recorder::pause(bool)
	{
		m_paused = true;
	}
	void recorder::resume()
	{
		m_paused = false;
	}
	void request_save()
	{
		requests.fetch_or(1);
	}
	void request_record()
	{
		requests.fetch_or(2);
	}
	unsigned take_requests()
	{
		return requests.exchange(0);
	}
	bool hardware_available()
	{
#ifdef _WIN32
		if (!media_foundation_present())
			return false;
		if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
			return false;
		bool available = false;
		if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
		{
			IMFActivate** choices = nullptr;
			UINT count = 0;
			MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
			if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
					nullptr, &output, &choices, &count)))
			{
				available = count != 0;
				for (UINT i = 0; i < count; ++i)
					choices[i]->Release();
				CoTaskMemFree(choices);
			}
			MFShutdown();
		}
		CoUninitialize();
		return available;
#else
		return false;
#endif
	}
} // namespace pbd::clips
