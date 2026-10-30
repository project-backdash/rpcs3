#pragma once

#include "Emu/PBD/pbd_env.h"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"
#include "Emu/System.h"
#include "Emu/CPU/pbd_ordinary_metrics.h"
#include "pbd_frame_tags.h"
#include "Emu/PBD/pbd_t5dr_memory.h"
#include "Emu/PBD/pbd_fight_timing.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <map>
#include <vector>
#include <memory>

namespace pbd::frame_presentation
{
	// Labels published by the main PPU, never a read of live guest RAM from RSX.
	// They are temporal annotations, not a proof of which update built an image.
	inline std::atomic<u32> input_counter{0};
	inline void mark_input(u32 counter) noexcept
	{
		input_counter.store(counter, std::memory_order_release);
	}
	// An opt-in selector may key a real presented source by the live latch's segment/counter.
	// This remains a temporal label: two repeatable straight captures are still required.
	inline std::atomic<u64> logical_context{~u64{0}};
	inline std::atomic<u32> logical_boundary{~u32{0}};
	inline frame_tag_queue frame_tags;
	inline bool queue_tags_enabled() noexcept
	{
		static const bool enabled = []
		{
#ifdef _WIN32
			const auto value = pbd::env::find_wide("PBD_PRESENT_IDENTITY_QUEUE_TAG");
			return (value && std::wstring_view(value) == L"1") || pbd::fight_timing::requested();
#else
			return false;
#endif
		}();
		return enabled;
	}

	// The main owner publishes one immutable token template at each update
	// boundary. The flip thunk entry copies it into one command record together
	// with the context's write pointer; the RSX later binds that record to the
	// DRIVER_QUEUE command it executes by address (frame_tag_queue::pop_command).
	class render_token_source
	{
		std::mutex mutex;
		frame_tag_queue::record current{};
		std::array<u32, 8> buffer_generations{};
		u64 epoch = 0, render_serial = 0, request_serial = 0;
		u32 replay_generation = 0, flips_since_boundary = 0;
		u32 current_segment = ~u32{0}, segment_start = 0;
		u32 first_callback = 0;
		bool have_boundary = false, previous_live = true;

	public:
		void boundary(u32 counter, u32 segment, bool live, u32 represented_tick, u64 session_epoch, u32 scene = ~u32{0}) noexcept
		{
			std::lock_guard lock(mutex);
			if (session_epoch != epoch)
			{
				epoch = session_epoch;
				render_serial = request_serial = 0;
				replay_generation = 0;
				buffer_generations.fill(0);
				previous_live = true;
				current_segment = ~u32{0};
			}
			if (!live && previous_live)
				++replay_generation;
			previous_live = live;
			flips_since_boundary = 0;
			if (segment != current_segment && live)
			{
				current_segment = segment;
				segment_start = represented_tick;
			}
			current = {};
			current.segment_offset = represented_tick - segment_start;
			current.session_epoch = session_epoch;
			current.represented_tick = represented_tick;
			current.replay_generation = replay_generation;
			current.token_segment = segment;
			current.token_counter = counter;
			current.context = live ? (u64{segment} << 32) | counter : frame_tag_queue::invalid;
			current.boundary = represented_tick;
			current.scene = scene;
			current.live = live;
			have_boundary = true;
		}
		// Called on the PPU thread that entered the flip thunk. Only the update
		// latch's owner may carry a live identity: any other caller's command is
		// registered (so pairing stays exact) but can never be selected.
		frame_tag_queue::record register_flip(u32 caller_thread, bool owner, u32 buffer, u32 entry_current, u32 begin, u32 end, u32 context,
			u32 callback = 0) noexcept
		{
			std::lock_guard lock(mutex);
			// The one-JUMP segment-switch model holds for the title's own callback only: a different callback is a fault.
			if (callback && !first_callback)
				first_callback = callback;
			else if (callback && callback != first_callback)
				frame_tags.fault();
			frame_tag_queue::record identity = have_boundary && owner ? current : frame_tag_queue::record{};
			identity.caller_thread = caller_thread;
			identity.buffer = buffer;
			identity.entry_current = entry_current;
			identity.gcm_context = context;
			identity.gcm_callback = callback;
			identity.gcm_begin = begin;
			identity.gcm_end = end;
			identity.tick_flip_index = flips_since_boundary++;
			identity.render_serial = ++render_serial;
			identity.request_serial = ++request_serial;
			if (buffer < buffer_generations.size())
				identity.buffer_generation = ++buffer_generations[buffer];
			const auto recorded = frame_tags.push_record(buffer, identity.context, identity.boundary, identity.live, &identity);
			if (recorded.live && recorded.accepted && pbd::fight_timing::requested())
				pbd::fight_timing::samples.produced(recorded.session_epoch, recorded.boundary);
			return recorded;
		}
	};
	inline render_token_source render_tokens;
	inline void mark_logical(u32 counter, u32 segment, bool live, u32 boundary, u32 scene = ~u32{0}) noexcept
	{
		logical_boundary.store(boundary, std::memory_order_release);
		logical_context.store(live ? (u64{segment} << 32) | counter : ~u64{0}, std::memory_order_release);
		render_tokens.boundary(counter, segment, live, boundary, static_cast<u64>(Emu.GetEmulationIdentifier()), scene);
	}

	// Trace-only queue provenance. The environment value is an absolute output
	// path supplied by the harness. It does not affect selection or queue state.
	class tag_trace
	{
		fs::file output;
		std::mutex mutex;
		std::map<u32, u64> active_sequences;
		u64 ordinal = 0;
		bool enabled = false;
		tag_trace() noexcept
		{
			try
			{
#ifdef _WIN32
				const auto requested = pbd::env::find_wide("PBD_PRESENT_TAG_TRACE");
				if (!requested || !*requested)
					return;
				const std::filesystem::path path(requested);
				if (!path.is_absolute())
					return;
				const auto utf8 = path.u8string();
				if (!output.open(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile))
					return;
				const std::string header = "{\"type\":\"header\",\"schema\":3,\"source\":\"fifo_command_render_token\"}\n";
				enabled = output.write(header.data(), header.size()) == header.size();
#endif
			}
			catch (...)
			{
				enabled = false;
			}
		}
		void write_locked(std::string text) noexcept
		{
			if (!enabled)
				return;
			text.insert(1, fmt::format("\"ordinal\":%u,", ++ordinal));
			if (output.write(text.data(), text.size()) != text.size())
				enabled = false;
		}

	public:
		static tag_trace& get() noexcept
		{
			static tag_trace value;
			return value;
		}
		static u64 now_ns() noexcept
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
			    .count();
		}
		void produce(const frame_tag_queue::record& tag, u32 caller_thread, u32 owner_thread,
			u32 gcm_begin, u32 gcm_end, u32 gcm_current, u32 rsx_get, u32 rsx_put) noexcept
		{
			std::lock_guard lock(mutex);
			const bool nested = active_sequences.contains(caller_thread);
			active_sequences[caller_thread] = tag.sequence;
			write_locked(fmt::format("{\"type\":\"produce\",\"sequence\":%u,\"association_fault\":%s,\"accepted\":%s,\"buffer\":%u,\"boundary\":%u,\"live\":%s,\"context_valid\":%s,\"segment\":%u,\"counter\":%u,\"session_epoch\":%u,\"represented_tick\":%u,\"replay_generation\":%u,\"render_serial\":%u,\"fifo_address\":%u,\"buffer_generation\":%u,\"request_serial\":%u,\"caller_thread\":%u,\"owner_thread\":%u,\"caller_is_owner\":%s,\"gcm_begin\":%u,\"gcm_end\":%u,\"gcm_current\":%u,\"rsx_get\":%u,\"rsx_put\":%u,\"steady_ns\":%u}\n",
				tag.sequence, nested ? "true" : "false", tag.accepted ? "true" : "false", tag.buffer, tag.boundary, tag.live ? "true" : "false",
				tag.context != frame_tag_queue::invalid ? "true" : "false", tag.token_segment, tag.token_counter,
				tag.session_epoch, tag.represented_tick, tag.replay_generation, tag.render_serial, tag.fifo_address,
				tag.buffer_generation, tag.request_serial,
				caller_thread, owner_thread, caller_thread == owner_thread ? "true" : "false",
				gcm_begin, gcm_end, gcm_current, rsx_get, rsx_put, now_ns()));
		}
		void token_state(const char* state, const frame_tag_queue::record& tag, u64 guest_flip = 0,
			const char* disposition = "none", u32 width = 0, u32 height = 0, u32 guest_pitch = 0,
			u32 capture_pitch = 0, u32 format = 0, const char* fence = "none") noexcept
		{
			std::lock_guard lock(mutex);
			write_locked(fmt::format("{\"type\":\"token_state\",\"state\":\"%s\",\"disposition\":\"%s\",\"sequence\":%u,\"accepted\":%s,\"buffer\":%u,\"session_epoch\":%u,\"segment\":%u,\"counter\":%u,\"live\":%s,\"represented_tick\":%u,\"replay_generation\":%u,\"render_serial\":%u,\"tick_flip_index\":%u,\"fifo_address\":%u,\"entry_current\":%u,\"gcm_context\":%u,\"chain_words\":%u,\"fifo_generation\":%u,\"command\":\"%s\",\"flip_head_address\":%u,\"flip_head_verified\":%s,\"verified\":%s,\"buffer_generation\":%u,\"request_serial\":%u,\"caller_thread\":%u,\"guest_flip_index\":%u,\"width\":%u,\"height\":%u,\"guest_pitch\":%u,\"capture_pitch\":%u,\"format\":%u,\"readback_fence\":\"%s\",\"steady_ns\":%u}\n",
				state, disposition, tag.sequence, tag.accepted ? "true" : "false", tag.buffer, tag.session_epoch,
				tag.token_segment, tag.token_counter, tag.live ? "true" : "false", tag.represented_tick, tag.replay_generation, tag.render_serial,
				tag.tick_flip_index, tag.fifo_address, tag.entry_current, tag.gcm_context, tag.chain_words, tag.fifo_generation, command_state_name(tag.command),
				tag.flip_head_address, tag.flip_head_verified ? "true" : "false", tag.verified() ? "true" : "false",
				tag.buffer_generation, tag.request_serial, tag.caller_thread, guest_flip,
				width, height, guest_pitch, capture_pitch, format, fence, now_ns()));
		}
		void phase(const char* type, u32 caller_thread, u32 owner_thread, u32 boundary, u64 context,
			u32 gcm_begin, u32 gcm_end, u32 gcm_current, u32 rsx_get, u32 rsx_put) noexcept
		{
			std::lock_guard lock(mutex);
			const auto found = active_sequences.find(caller_thread);
			const bool associated = found != active_sequences.end();
			const u64 sequence = associated ? found->second : 0;
			write_locked(fmt::format("{\"type\":\"%s\",\"sequence\":%u,\"association_fault\":%s,\"boundary\":%u,\"context_valid\":%s,\"segment\":%u,\"counter\":%u,\"caller_thread\":%u,\"owner_thread\":%u,\"caller_is_owner\":%s,\"gcm_begin\":%u,\"gcm_end\":%u,\"gcm_current\":%u,\"rsx_get\":%u,\"rsx_put\":%u,\"steady_ns\":%u}\n",
				type, sequence, associated ? "false" : "true", boundary, context != frame_tag_queue::invalid ? "true" : "false", u32(context >> 32), u32(context),
				caller_thread, owner_thread, caller_thread == owner_thread ? "true" : "false",
				gcm_begin, gcm_end, gcm_current, rsx_get, rsx_put, now_ns()));
			if (std::string_view(type) == "present_end" && associated)
				active_sequences.erase(found);
		}
		void consume(const frame_tag_queue::record& tag, u64 guest_flip, bool skipped, bool replay_withhold, u32 withhold_left,
			u32 rsx_get, u32 rsx_put) noexcept
		{
			std::lock_guard lock(mutex);
			write_locked(fmt::format("{\"type\":\"consume\",\"sequence\":%u,\"accepted\":%s,\"buffer\":%u,\"boundary\":%u,\"live\":%s,\"context_valid\":%s,\"segment\":%u,\"counter\":%u,\"guest_flip_index\":%u,\"skipped\":%s,\"replay_withhold\":%s,\"withhold_left\":%u,\"rsx_get\":%u,\"rsx_put\":%u,\"steady_ns\":%u}\n",
				tag.sequence, tag.accepted ? "true" : "false", tag.buffer, tag.boundary, tag.live ? "true" : "false",
				tag.context != frame_tag_queue::invalid ? "true" : "false", u32(tag.context >> 32), u32(tag.context), guest_flip,
				skipped ? "true" : "false", replay_withhold ? "true" : "false", withhold_left, rsx_get, rsx_put, now_ns()));
		}
		void present(u64 context, u64 guest_flip, u32 buffer, u64 sequence, u32 boundary, bool live) noexcept
		{
			std::lock_guard lock(mutex);
			write_locked(fmt::format("{\"type\":\"present\",\"sequence\":%u,\"buffer\":%u,\"boundary\":%u,\"live\":%s,\"context_valid\":%s,\"segment\":%u,\"counter\":%u,\"guest_flip_index\":%u,\"steady_ns\":%u}\n",
				sequence, buffer, boundary, live ? "true" : "false", context != frame_tag_queue::invalid ? "true" : "false", u32(context >> 32), u32(context), guest_flip, now_ns()));
		}
	};
	// Renderer flip ordinals restart at every load and are not serialized. Only a
	// capture callback, after the capturing renderer joined, publishes its last
	// reported ordinal as the base for generations loaded from that capture.
	inline std::atomic<u64> flip_base{0};
	inline std::atomic<u64> last_reported_flip{0};

} // namespace pbd::frame_presentation
