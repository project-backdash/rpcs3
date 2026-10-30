#pragma once

#include "pbd_present_identity.h"

namespace pbd::frame_presentation
{
	class capture
	{
		fs::file output;
		std::filesystem::path directory;
		u32 count = 0;
		u64 bytes = 0;
		bool enabled = false;
		static constexpr u32 max_frames = 32;
		static constexpr u64 max_bytes = 512ull * 1024 * 1024;

		capture() noexcept
		{
			try
			{
#ifdef _WIN32
				const auto requested = pbd::env::find_wide("PBD_PRESENT_OUTPUT");
				if (!requested || !*requested || Emu.GetTitleID() != pbd::t5dr::title_id)
					return;
				const std::filesystem::path path(requested);
				if (!path.is_absolute())
					return;
				directory = path.parent_path();
				const auto utf8 = path.u8string();
				if (!output.open(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile))
					return;
				const std::string text = "{\"type\":\"header\",\"schema\":2,\"source\":\"vulkan_guest_present_source\",\"max_frames\":32,\"max_bytes\":536870912,\"overlays_included\":false,\"timing_perturbed_by_readback\":true}\n";
				enabled = output.write(text.data(), text.size()) == text.size();
#endif
			}
			catch (...)
			{
				enabled = false;
			}
		}

	public:
		static std::unique_ptr<capture>& slot() noexcept
		{
			static std::unique_ptr<capture> value;
			return value;
		}
		static capture& get() noexcept
		{
			auto& value = slot();
			if (!value)
				value.reset(new capture);
			return *value;
		}
		// Called only by the owning Vulkan renderer thread, on guest flips.
		bool wants_frame() const noexcept
		{
			return enabled && count < (::pbd::ordinary_metrics::requested() ? 1u : max_frames) && bytes < max_bytes && ::pbd::ordinary_metrics::capture_allowed();
		}
		void append(const std::vector<u8>& pixels, u32 width, u32 height, bool bgra, u32 buffer, u64 flip) noexcept
		{
			if (!wants_frame())
				return;
			try
			{
				const u64 size = u64{width} * height * 4;
				if (!width || !height || width > 4096 || height > 4096 || pixels.size() != size || size > max_bytes - bytes)
				{
					enabled = false;
					return;
				}
				const auto filename = fmt::format("present-%03u.bin", count);
				const auto utf8 = (directory / filename).u8string();
				fs::file image(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile);
				if (!image || image.write(pixels.data(), size) != size)
				{
					enabled = false;
					return;
				}
				image.close();
				const u64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
				const auto text = fmt::format("{\"type\":\"frame\",\"index\":%u,\"file\":\"%s\",\"width\":%u,\"height\":%u,\"bytes\":%u,\"is_bgra\":%s,\"buffer\":%u,\"guest_flip_index\":%u,\"latest_input_counter\":%u,\"steady_ns\":%u}\n",
					count, filename, width, height, size, bgra ? "true" : "false", buffer, flip, input_counter.load(std::memory_order_acquire), now);
				if (output.write(text.data(), text.size()) != text.size())
				{
					enabled = false;
					return;
				}
				if (::pbd::ordinary_metrics::requested())
					output.close();
				++count;
				bytes += size;
			}
			catch (...)
			{
				enabled = false;
			}
		}
	};

	// Additive, explicitly versioned capture. Never changes the legacy temporal
	// selection above. Identity is the actual renderer-owned guest flip ordinal;
	// latest_input_counter remains only a temporal annotation.
	class identity_capture
	{
		fs::file output;
		std::filesystem::path directory;
		u64 requested_flip = 0;
		u64 selected_flip = 0;
		u32 requested_counter = 0, requested_segment = 0, selected_counter = 0, selected_boundary = ~u32{0};
		u64 selected_sequence = 0;
		frame_tag_queue::record selected_token{};
		std::unique_ptr<counter_epoch_selector> counter_selector;
		bool counter_selection = false;
		bool enabled = false, finished = false;
		void reject(u64 actual, const char* reason) noexcept
		{
			if (!enabled || finished)
				return;
			const auto text = fmt::format("{\"type\":\"rejected\",\"requested_guest_flip\":%u,\"actual_guest_flip\":%u,\"reason\":\"%s\"}\n", requested_flip, actual, reason);
			output.write(text.data(), text.size());
			output.close();
			finished = true;
		}
		identity_capture() noexcept
		{
			try
			{
#ifdef _WIN32
				const auto requested = pbd::env::find_wide("PBD_PRESENT_IDENTITY_FLIP");
				const auto counter = pbd::env::find_wide("PBD_PRESENT_IDENTITY_COUNTER");
				const auto segment = pbd::env::find_wide("PBD_PRESENT_IDENTITY_SEGMENT");
				const auto legacy = pbd::env::find_wide("PBD_PRESENT_OUTPUT");
				if (!requested || !*requested || !legacy || !*legacy || Emu.GetTitleID() != pbd::t5dr::title_id ||
					!::pbd::ordinary_metrics::requested())
					return;
				for (const auto* digit = requested; *digit; ++digit)
				{
					if (*digit < L'0' || *digit > L'9' || requested_flip > 100000)
						return;
					requested_flip = requested_flip * 10 + (*digit - L'0');
				}
				if (!requested_flip || requested_flip > 1000000)
					return;
				if (counter && *counter)
				{
					const auto decimal = [](const wchar_t* value, u32& out)
					{
						if (!value || !*value)
							return false;
						u64 n = 0;
						for (; *value; ++value)
						{
							if (*value < L'0' || *value > L'9')
								return false;
							n = n * 10 + (*value - L'0');
							if (n > 0xffffffffull)
								return false;
						}
						out = static_cast<u32>(n);
						return true;
					};
					if (!decimal(counter, requested_counter) || !decimal(segment, requested_segment) || requested_segment >= 0x80000000u)
						return;
					counter_selection = true;
					counter_selector = std::make_unique<counter_epoch_selector>(requested_segment, requested_counter);
				}
				const std::filesystem::path path(legacy);
				if (!path.is_absolute())
					return;
				directory = path.parent_path();
				const auto utf8 = (directory / "present-identity-v1.jsonl").u8string();
				if (!output.open(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile))
					return;
				const auto text = fmt::format("{\"type\":\"header\",\"schema\":%u,\"source\":\"vulkan_guest_present_source\",\"selection\":\"%s\",\"requested_guest_flip\":%u,\"requested_input_counter\":%u,\"requested_segment\":%u,\"max_frames\":1,\"overlays_included\":false,\"requires_closed_measurement\":true,\"legacy_capture_preserved\":true}\n", queue_tags_enabled() ? 4u : counter_selection ? 2u :
																																																																																																			   1u,
					counter_selection ? (queue_tags_enabled() ? "queued_live_segment_input_counter" : "live_segment_input_counter") : "guest_flip_index", requested_flip, requested_counter, requested_segment);
				enabled = output.write(text.data(), text.size()) == text.size();
#endif
			}
			catch (...)
			{
				enabled = false;
			}
		}

	public:
		static std::unique_ptr<identity_capture>& slot() noexcept
		{
			static std::unique_ptr<identity_capture> value;
			return value;
		}
		static identity_capture& get() noexcept
		{
			auto& value = slot();
			if (!value)
				value.reset(new identity_capture);
			return *value;
		}
		// Called once per real guest flip, before deciding whether GPU readback is
		// required. A missed or premature requested ordinal is an explicit failure.
		bool wants_frame(u64 actual, u64 queued_context = ~u64{0}, u64 queued_sequence = 0, u32 queued_boundary = ~u32{0}, bool queued_live = false,
			const frame_tag_queue::record& token = {}) noexcept
		{
			if (!enabled || finished)
				return false;
			if (counter_selection)
			{
				if (queue_tags_enabled() && frame_tags.snapshot().faults)
				{
					reject(actual, "frame_tag_queue_fault");
					return false;
				}
				const u64 context = queue_tags_enabled() ? queued_context : logical_context.load(std::memory_order_acquire);
				if (queue_tags_enabled() && (!queued_live || context == ~u64{0}))
					return false;
				const auto decision = counter_selector->observe(context);
				if (decision == counter_select_result::wait)
					return false;
				if (decision == counter_select_result::reject_segment)
				{
					reject(actual, "requested_segment_missed");
					return false;
				}
				if (decision == counter_select_result::reject_counter)
				{
					reject(actual, "requested_counter_missed");
					return false;
				}
				if (decision == counter_select_result::reject_epoch_ended)
				{
					reject(actual, "requested_counter_epoch_ended");
					return false;
				}
				// A queued selection is valid only for a command whose address the
				// RSX proved (DRIVER_QUEUE by NOP/JUMP chain, FLIP_HEAD by chain).
				if (queue_tags_enabled() && !token.verified())
				{
					reject(actual, "unverified_command_identity");
					return false;
				}
				selected_counter = u32(context);
				selected_boundary = queue_tags_enabled() ? queued_boundary : logical_boundary.load(std::memory_order_acquire);
				selected_sequence = queue_tags_enabled() ? queued_sequence : 0;
			}
			else
			{
				if (actual < requested_flip)
					return false;
				if (actual > requested_flip)
				{
					reject(actual, "requested_flip_missed");
					return false;
				}
				selected_counter = input_counter.load(std::memory_order_acquire);
			}
			// PBD_PRESENT_IDENTITY_ANYTIME=1 lets a frame INSIDE the measured window be captured (presented-frame
			// check after a rollback); the readback disturbs that run's timing, which such a run ignores.
			static const bool anytime = []
			{
				const auto v = pbd::env::find_wide("PBD_PRESENT_IDENTITY_ANYTIME");
				return v && v[0] == L'1' && v[1] == 0;
			}();
			if (!anytime && !::pbd::ordinary_metrics::capture_allowed())
			{
				reject(actual, "measurement_not_closed");
				return false;
			}
			selected_token = token;
			selected_flip = actual;
			return true;
		}
		// `fence` names the GPU-completion wait performed before the host read; a
		// failed wait rejects the capture instead of writing possibly-stale pixels.
		void append(const std::vector<u8>& pixels, u32 width, u32 height, bool bgra, u32 buffer, u64 flip, u32 guest_pitch = 0, u32 format = 0,
			const char* fence = "flush_command_queue_hard_sync", bool fence_ok = true) noexcept
		{
			if (!enabled || finished || flip != selected_flip)
				return;
			if (!fence_ok)
			{
				reject(flip, "readback_fence_failed");
				return;
			}
			try
			{
				const u64 size = u64{width} * height * 4;
				if (!width || !height || width > 4096 || height > 4096 || pixels.size() != size)
				{
					reject(flip, "invalid_image_extent");
					return;
				}
				const auto utf8 = (directory / "present-identity-v1.bin").u8string();
				fs::file image(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile);
				if (!image || image.write(pixels.data(), size) != size)
				{
					reject(flip, "image_write_failed");
					return;
				}
				image.close();
				const u64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
				const auto& t = selected_token;
				const auto text = fmt::format("{\"type\":\"frame\",\"index\":0,\"file\":\"present-identity-v1.bin\",\"width\":%u,\"height\":%u,\"bytes\":%u,\"is_bgra\":%s,\"buffer\":%u,\"guest_flip_index\":%u,\"latest_input_counter\":%u,\"selected_input_counter\":%u,\"selected_segment\":%u,\"selected_boundary\":%u,\"selected_tag_sequence\":%u,\"session_epoch\":%u,\"represented_tick\":%u,\"replay_generation\":%u,\"render_serial\":%u,\"tick_flip_index\":%u,\"token_live\":%s,\"fifo_address\":%u,\"entry_current\":%u,\"gcm_context\":%u,\"chain_words\":%u,\"fifo_generation\":%u,\"command\":\"%s\",\"flip_head_address\":%u,\"flip_head_verified\":%s,\"token_verified\":%s,\"buffer_generation\":%u,\"request_serial\":%u,\"caller_thread\":%u,\"guest_pitch\":%u,\"capture_pitch\":%u,\"format\":%u,\"readback_fence\":\"%s\",\"counter_selection\":%s,\"queue_tagged\":%s,\"steady_ns\":%u}\n", width, height, size, bgra ? "true" : "false", buffer, flip, input_counter.load(std::memory_order_acquire), selected_counter, requested_segment, selected_boundary, selected_sequence, t.session_epoch, t.represented_tick, t.replay_generation, t.render_serial, t.tick_flip_index, t.live ? "true" : "false", t.fifo_address, t.entry_current, t.gcm_context, t.chain_words, t.fifo_generation, command_state_name(t.command), t.flip_head_address, t.flip_head_verified ? "true" : "false", t.verified() ? "true" : "false", t.buffer_generation, t.request_serial, t.caller_thread, guest_pitch, width * 4, format, fence, counter_selection ? "true" : "false", queue_tags_enabled() ? "true" : "false", now);
				output.write(text.data(), text.size());
				output.close();
				finished = true;
			}
			catch (...)
			{
				reject(flip, "capture_exception");
			}
		}
	};

	// Several frozen targets per run, keyed by (session segment, offset from the segment's first boundary),
	// which is comparable across runs whose pre-battle timelines are identical. A target is captured only from a LIVE presented
	// flip whose command identity is verified; every target ends as a frame, an explicit rejection or a "missed" row.
	class target_capture
	{
		fs::file output;
		std::filesystem::path directory;
		u32 segment = 0;
		std::vector<u32> offsets;
		std::vector<bool> done;
		bool enabled = false, finished = false;
		int selected = -1;
		u64 selected_flip = 0;
		frame_tag_queue::record selected_token{};
		void row(const std::string& text) noexcept
		{
			if (enabled)
				output.write(text.data(), text.size());
		}
		void close_target(usz i, const char* type, const char* reason) noexcept
		{
			row(fmt::format("{\"type\":\"%s\",\"target_offset\":%u,\"segment\":%u,\"reason\":\"%s\"}\n", type, offsets[i], segment, reason));
			done[i] = true;
		}
		target_capture() noexcept
		{
			try
			{
#ifdef _WIN32
				const auto list = pbd::env::find_wide("PBD_PRESENT_TARGET_OFFSETS");
				const auto seg = pbd::env::find_wide("PBD_PRESENT_TARGET_SEGMENT");
				const auto legacy = pbd::env::find_wide("PBD_PRESENT_OUTPUT");
				if (!list || !*list || !seg || !*seg || !legacy || !*legacy ||
					Emu.GetTitleID() != pbd::t5dr::title_id || !queue_tags_enabled())
					return;
				u64 n = 0;
				bool digit = false;
				for (const wchar_t* c = seg; *c; ++c)
				{
					if (*c < L'0' || *c > L'9')
						return;
					n = n * 10 + (*c - L'0');
					if (n > 1000000)
						return;
				}
				segment = static_cast<u32>(n);
				n = 0;
				for (const wchar_t* c = list;; ++c)
				{
					if (*c >= L'0' && *c <= L'9')
					{
						n = n * 10 + (*c - L'0');
						digit = true;
						if (n > 1000000)
							return;
						continue;
					}
					if ((*c == L',' || !*c) && digit)
					{
						offsets.push_back(static_cast<u32>(n));
						n = 0;
						digit = false;
					}
					else
						return;
					if (!*c)
						break;
				}
				std::sort(offsets.begin(), offsets.end());
				offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
				if (offsets.empty() || offsets.size() > 16)
					return;
				done.assign(offsets.size(), false);
				const std::filesystem::path path(legacy);
				if (!path.is_absolute())
					return;
				directory = path.parent_path();
				const auto utf8 = (directory / "present-targets-v1.jsonl").u8string();
				if (!output.open(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile))
					return;
				enabled = true;
				std::string list_text;
				for (usz i = 0; i < offsets.size(); i++)
					fmt::append(list_text, "%s%u", i ? "," : "", offsets[i]);
				row(fmt::format("{\"type\":\"header\",\"schema\":1,\"source\":\"vulkan_guest_present_source\",\"key\":\"segment_offset\",\"segment\":%u,\"offsets\":[%s],\"overlays_included\":false,\"identity\":\"fifo_command_address\"}\n", segment, list_text));
#endif
			}
			catch (...)
			{
				enabled = false;
			}
		}

	public:
		~target_capture()
		{
			for (usz i = 0; i < offsets.size(); i++)
				if (enabled && !done[i])
					close_target(i, "missed", "no_live_presented_flip");
		}
		static std::unique_ptr<target_capture>& slot() noexcept
		{
			static std::unique_ptr<target_capture> value;
			return value;
		}
		static target_capture& get() noexcept
		{
			auto& value = slot();
			if (!value)
				value.reset(new target_capture);
			return *value;
		}
		bool wants_frame(u64 flip, const frame_tag_queue::record& token) noexcept
		{
			if (!enabled || finished)
				return false;
			if (frame_tags.snapshot().faults)
			{
				for (usz i = 0; i < offsets.size(); i++)
					if (!done[i])
						close_target(i, "rejected", "frame_tag_queue_fault");
				finished = true;
				return false;
			}
			if (!token.live || token.token_segment != segment)
				return false;
			for (usz i = 0; i < offsets.size(); i++)
			{
				if (done[i] || offsets[i] != token.segment_offset)
					continue;
				if (!token.verified())
				{
					close_target(i, "rejected", "unverified_command_identity");
					return false;
				}
				selected = static_cast<int>(i);
				selected_flip = flip;
				selected_token = token;
				return true;
			}
			return false;
		}
		void append(const std::vector<u8>& pixels, u32 width, u32 height, bool bgra, u32 buffer, u64 flip, u32 guest_pitch, u32 format,
			const char* fence, bool fence_ok) noexcept
		{
			if (!enabled || selected < 0 || flip != selected_flip)
				return;
			const usz i = static_cast<usz>(selected);
			selected = -1;
			try
			{
				if (!fence_ok)
				{
					close_target(i, "rejected", "readback_fence_failed");
					return;
				}
				const u64 size = u64{width} * height * 4;
				if (!width || !height || width > 4096 || height > 4096 || pixels.size() != size)
				{
					close_target(i, "rejected", "invalid_image_extent");
					return;
				}
				const auto name = fmt::format("present-target-o%u.bin", offsets[i]);
				const auto utf8 = (directory / name).u8string();
				fs::file image(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile);
				if (!image || image.write(pixels.data(), size) != size)
				{
					close_target(i, "rejected", "image_write_failed");
					return;
				}
				image.close();
				const auto& t = selected_token;
				row(fmt::format("{\"type\":\"frame\",\"target_offset\":%u,\"file\":\"%s\",\"width\":%u,\"height\":%u,\"bytes\":%u,\"is_bgra\":%s,\"buffer\":%u,\"guest_flip_index\":%u,\"segment\":%u,\"segment_offset\":%u,\"counter\":%u,\"represented_tick\":%u,\"session_epoch\":%u,\"replay_generation\":%u,\"render_serial\":%u,\"request_serial\":%u,\"tick_flip_index\":%u,\"sequence\":%u,\"fifo_address\":%u,\"fifo_generation\":%u,\"gcm_context\":%u,\"command\":\"%s\",\"flip_head_verified\":%s,\"token_verified\":%s,\"token_live\":%s,\"guest_pitch\":%u,\"capture_pitch\":%u,\"format\":%u,\"readback_fence\":\"%s\"}\n",
					offsets[i], name, width, height, size, bgra ? "true" : "false", buffer, flip, t.token_segment, t.segment_offset, t.token_counter,
					t.represented_tick, t.session_epoch, t.replay_generation, t.render_serial, t.request_serial, t.tick_flip_index, t.sequence,
					t.fifo_address, t.fifo_generation, t.gcm_context, command_state_name(t.command), t.flip_head_verified ? "true" : "false",
					t.verified() ? "true" : "false", t.live ? "true" : "false", guest_pitch, width * 4, format, fence));
				done[i] = true;
				finished = std::all_of(done.begin(), done.end(), [](bool d)
					{
						return d;
					});
			}
			catch (...)
			{
				close_target(i, "rejected", "capture_exception");
			}
		}
	};
} // namespace pbd::frame_presentation
