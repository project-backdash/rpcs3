#pragma once

#include <util/types.hpp>
#include <util/logs.hpp>
#include <deque>
#include <unordered_map>
#include "Emu/RSX/pbd_frame_tags.h"

template <typename T>
class named_thread;

namespace rsx
{
	enum class surface_antialiasing : u8;

	struct surface_scaling_config_t;

	struct framebuffer_dimensions_t
	{
		u16 width;
		u16 height;
		u8 samples_x;
		u8 samples_y;

		inline u32 samples_total() const
		{
			return static_cast<u32>(width) * height * samples_x * samples_y;
		}

		inline bool operator > (const framebuffer_dimensions_t& that) const
		{
			return samples_total() > that.samples_total();
		}

		std::string to_string(bool skip_aa_suffix = false) const;

		static framebuffer_dimensions_t make(u16 width, u16 height, rsx::surface_antialiasing aa);
	};

	struct framebuffer_statistics_t
	{
		std::unordered_map<rsx::surface_antialiasing, framebuffer_dimensions_t> data;

		// Replace the existing data with this input if it is greater than what is already known
		void add(u16 width, u16 height, rsx::surface_antialiasing aa);

		// Returns a formatted string representing the statistics collected over the frame.
		std::string to_string(const surface_scaling_config_t& scaling_config, bool squash) const;
	};

	struct frame_statistics_t
	{
		u32 draw_calls;
		u32 submit_count;

		s64 setup_time;
		s64 vertex_upload_time;
		s64 textures_upload_time;
		s64 draw_exec_time;
		s64 flip_time;

		u32 vertex_cache_request_count;
		u32 vertex_cache_miss_count;

		u32 program_cache_lookups_total;
		u32 program_cache_lookups_ellided;

		framebuffer_statistics_t framebuffer_stats;
	};

	struct frame_time_t
	{
		u64 preempt_count;
		u64 timestamp;
		u64 tsc;
	};

	struct display_flip_info_t
	{
		struct queued_buffer
		{
			u32 buffer;
			::pbd::frame_presentation::frame_tag_queue::record pbd_token;
			u32 pbd_bp_kind = 0, pbd_bp_marker = 0, pbd_bp_index = 0;
		};
		std::deque<queued_buffer> buffer_queue;
		::pbd::frame_presentation::frame_tag_queue::record pbd_token{};
		u64 pbd_context = ~u64{0};
		u64 pbd_sequence = 0;
		u32 pbd_boundary = ~u32{0};
		bool pbd_live = false;
		// Black-frame probe tag of the popped buffer: kind 0 none, 1 post-window live frame, 2 in flight at a restore, 3 baseline.
		u32 pbd_bp_kind = 0, pbd_bp_marker = 0, pbd_bp_index = 0;
		u32 buffer;
		bool skip_frame;
		bool emu_flip;
		bool in_progress;
		frame_statistics_t stats;

		inline void push(u32 _buffer, const ::pbd::frame_presentation::frame_tag_queue::record& token = {})
		{
			buffer_queue.push_back({_buffer, token});
		}

		inline bool pop(u32 _buffer)
		{
			if (buffer_queue.empty())
			{
				return false;
			}

			do
			{
				const auto index = buffer_queue.front();
				buffer_queue.pop_front();

				if (index.buffer == _buffer)
				{
					buffer = _buffer;
					pbd_token = index.pbd_token;
					pbd_context = pbd_token.context;
					pbd_sequence = pbd_token.sequence;
					pbd_boundary = pbd_token.boundary;
					pbd_live = pbd_token.live;
					pbd_bp_kind = index.pbd_bp_kind;
					pbd_bp_marker = index.pbd_bp_marker;
					pbd_bp_index = index.pbd_bp_index;
					return true;
				}
			} while (!buffer_queue.empty());

			// Need to observe this happening in the wild
			rsx_log.error("Display queue was discarded while not empty!");
			return false;
		}
	};

	class vblank_thread
	{
		std::shared_ptr<named_thread<std::function<void()>>> m_thread;

	public:
		vblank_thread() = default;
		vblank_thread(const vblank_thread&) = delete;

		void set_thread(std::shared_ptr<named_thread<std::function<void()>>> thread);

		// PBD: wake the vblank thread's poll so a virtual vblank credit is served at
		// once instead of at the next poll (see pbd::replay_pacing::grant).
		void notify();

		vblank_thread& operator=(thread_state);
		vblank_thread& operator=(const vblank_thread&) = delete;
	};
}
