#pragma once

// Private MP4 packet output and destination selection for util/pbd_clips.cpp.
namespace pbd::clips
{
	namespace
	{
		std::atomic<unsigned> requests{0};
		std::mutex output_name_mutex;
		using clock = std::chrono::steady_clock;
		constexpr AVRational microseconds{1, 1'000'000};
		constexpr unsigned width = 1280, height = 720;
		struct clip_error : std::exception
		{
		};
		void require(bool ok)
		{
			if (!ok)
				throw clip_error{};
		}
		std::string utf8(const std::filesystem::path& path);
#ifdef _WIN32
		bool media_foundation_present()
		{
			// Windows editions without the Media Feature Pack must still be able to play.
			static const auto module = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			return module != nullptr;
		}
#endif

		struct mp4
		{
			AVFormatContext* format = nullptr;
			std::filesystem::path temporary, destination;
			std::int64_t start = -1;
			std::array<std::int64_t, 2> last{-1, -1};
			bool header = false, video = false;
			mp4(const std::filesystem::path& file, const std::vector<u8>& h264, const AVCodecContext* audio, std::int64_t from)
				: temporary(file), destination(file), start(from)
			{
				temporary += ".part";
				try
				{
					require(!h264.empty());
					std::filesystem::create_directories(file.parent_path());
					require(avformat_alloc_output_context2(&format, nullptr, "mp4", utf8(temporary).c_str()) >= 0 && format);
					auto* stream = avformat_new_stream(format, nullptr);
					require(stream);
					stream->time_base = microseconds;
					stream->avg_frame_rate = {60, 1};
					auto* codec = stream->codecpar;
					codec->codec_type = AVMEDIA_TYPE_VIDEO;
					codec->codec_id = AV_CODEC_ID_H264;
					codec->width = width;
					codec->height = height;
					codec->extradata = static_cast<u8*>(av_mallocz(h264.size() + AV_INPUT_BUFFER_PADDING_SIZE));
					require(codec->extradata);
					std::copy(h264.begin(), h264.end(), codec->extradata);
					codec->extradata_size = static_cast<int>(h264.size());
					if (audio)
					{
						auto* sound = avformat_new_stream(format, nullptr);
						require(sound);
						sound->time_base = microseconds;
						require(avcodec_parameters_from_context(sound->codecpar, audio) >= 0);
					}
					require(avio_open(&format->pb, utf8(temporary).c_str(), AVIO_FLAG_WRITE) >= 0);
					AVDictionary* flags = nullptr;
					av_dict_set(&flags, "movflags", "+faststart", 0);
					const auto result = avformat_write_header(format, &flags);
					av_dict_free(&flags);
					require(result >= 0);
					header = true;
				}
				catch (...)
				{
					close(false);
					throw;
				}
			}
			~mp4()
			{
				close(false);
			}
			void append(const packet_ptr& value)
			{
				if (value->time_us < start || (value->audio && format->nb_streams < 2))
					return;
				const unsigned index = value->audio ? 1 : 0;
				auto* stream = format->streams[index];
				const auto pts = av_rescale_q(value->time_us - start, microseconds, stream->time_base);
				if (pts <= last[index])
					return;
				AVPacket* packet = av_packet_alloc();
				require(packet);
				if (av_new_packet(packet, static_cast<int>(value->bytes.size())) < 0)
				{
					av_packet_free(&packet);
					throw clip_error{};
				}
				std::copy(value->bytes.begin(), value->bytes.end(), packet->data);
				packet->pts = packet->dts = pts;
				packet->duration = av_rescale_q(value->duration_us, microseconds, stream->time_base);
				packet->stream_index = index;
				if (value->keyframe)
					packet->flags |= AV_PKT_FLAG_KEY;
				const auto result = av_interleaved_write_frame(format, packet);
				av_packet_free(&packet);
				require(result >= 0);
				last[index] = pts;
				video |= !value->audio;
			}
			bool close(bool save)
			{
				if (!format)
					return false;
				bool ok = save && header && video;
				if (header && av_write_trailer(format) < 0)
					ok = false;
				if (format->pb && avio_closep(&format->pb) < 0)
					ok = false;
				avformat_free_context(format);
				format = nullptr;
				std::error_code error;
				if (ok)
				{
					std::filesystem::rename(temporary, destination, error);
					ok = !error;
				}
				if (!ok)
					std::filesystem::remove(temporary, error);
				return ok;
			}
		};

		std::filesystem::path folder(const std::string& configured)
		{
			if (!configured.empty())
				return std::filesystem::u8path(configured);
#ifdef _WIN32
			PWSTR path = nullptr;
			require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &path)) && path);
			std::filesystem::path result(path);
			CoTaskMemFree(path);
			return result / "Project Backdash";
#else
			throw clip_error{};
#endif
		}
		// "Project Backdash 2026-01-01 14-17-22 Ranked Jin Kazama vs Kazuya Mishima.mp4" in the player's own time ("... Recording
		// ..." for a recording). The date comes first, so the folder sorts by when the moment happened; the mode and fighters
		// follow when the launcher knows them. A second file with the same name gets " (2)", " (3)" and so on.
		std::filesystem::path filename(const options& config, bool recording)
		{
			const auto now = std::time(nullptr);
			std::tm time{};
#ifdef _WIN32
			localtime_s(&time, &now);
#else
			localtime_r(&now, &time);
#endif
			char stamp[32]{};
			std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &time);
			const auto label = file_label(config.label);
			const auto base = std::string(recording ? "Project Backdash Recording " : "Project Backdash ") + stamp + (label.empty() ? "" : " " + label);
			const auto directory = folder(config.folder);
			// Both output threads hold output_name_mutex through MP4 creation. Active .part files reserve their names.
			const auto name = available_filename(base, [&](const std::string& file)
				{
					return std::filesystem::exists(directory / file);
				});
			require(name.has_value());
			return directory / *name;
		}
		std::string utf8(const std::filesystem::path& path)
		{
			const auto value = path.u8string();
			return {value.begin(), value.end()};
		}
	} // namespace

} // namespace pbd::clips
