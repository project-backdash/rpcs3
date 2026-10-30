#include "stdafx.h"
#include "../PBD/pbd_boot_request_state.h"

LOG_CHANNEL(pbd_boot_log, "PBD boot");

namespace pbd::boot_request
{
	using namespace detail;
	namespace
	{
		void observation_failed(detail::boot_observation* observation, const char* operation) noexcept
		{
			static std::atomic<bool> initialization_reported{false};
			const bool reported = observation ? observation->failed.exchange(true) : initialization_reported.exchange(true);
			if (!reported)
				pbd_boot_log.error("Boot observer disabled after %s failed; media retirement can no longer be verified.", operation);
		}
	} // namespace

	bool detail::boot_observation::observe_owner(ppu_thread& ppu, u32 address)
	{
		bool request = false;
		if (address == 0x56258)
			title = static_cast<u32>(ppu.gpr[3]);
		if (address == pbd::t5dr::boundary_pc && !executing)
		{
			const u32 frame = word(0x1008f07c);
			const std::array<u32, 4> now{word(0x1008f060), word(pbd::t5dr::scene_address),
				word(pbd::t5dr::subscene_address), word(pbd::t5dr::game_mode_address)};
			const std::array<u32, 2> phase{word(0x1009322c), word(0x10093230)};
			if (now != previous || phase != previous_phase || (frame != last_frame && frame % 60 == 0))
				sample(ppu, "boundary");
			previous = now;
			last_frame = frame;
			previous_phase = phase;
			image_stable = now[1] == image_scene ? image_stable + 1 : 0;
			image_scene = now[1];
			phase_stable = phase[0] == image_phase ? phase_stable + 1 : 0;
			image_phase = phase[0];
			const bool select = now[1] == 3 && !select_image;
			// 159f0(kind0x13) -> 41034/case19 initializes Practice phase0x11.
			const u32 fight_phase = now[3] == 10 ? 17u : 5u;
			const bool fight = now[1] == 17 && phase[0] == fight_phase && phase_stable >= 120 && select_image && !fight_image;
			if (screenshots && image_stable >= 120 && (select || fight))
			{
				if (select)
					select_image = true;
				else
					fight_image = true;
				g_user_asked_for_screenshot = true;
				sample(ppu, select ? "select_gpu_screenshot_requested" : "fight_gpu_screenshot_requested");
			}
			const bool ready = now == std::array<u32, 4>{1, 0, 44, 0} && word(0x1008f064) == 14 &&
			                   title && word(title + 0x80) == 1 && word(0x10093464) != 8 &&
			                   word(0x1008eba8) >= 100 && byte(0x10684964) == 1 && byte(0x10684966) == 2;
			stable = ready ? stable + 1 : 0;
			const bool retired = !media_retire || (teardown_requested && media_generation == teardown_media_generation &&
													  media_free_observed && media_exit_observed && media_allocations.empty() &&
													  word(0x10532a38 + 0x2ac) == 0 && byte(0x10532a38 + 0x2bc) == 1);
			const bool can_teardown = media_retire && !teardown_requested && media_generation && !media_allocations.empty();
			request = !target.empty() && !attempted && stable >= 20 && !ppu.state.load() && (retired || can_teardown);
		}
		if (address == 0x56f20 || address == 0x1e9e8 || address == 0x159f0 || address == 0x15a9c || address == 0x15ac4 ||
			address == 0xafc64 || address == 0xcb96c || address == 0x52298 || address == 0x5a554 ||
			address == 0x53ec4 || address == 0x53f70 || address == 0x146d4 || address == 0x15a88)
		{
			write(fmt::format("{\"type\":\"native_entry\",\"ms\":%u,\"address\":%u,\"r3\":%u,\"r4\":%u,\"lr\":%u,\"frame\":%u}\n",
				elapsed(), address, static_cast<u32>(ppu.gpr[3]), static_cast<u32>(ppu.gpr[4]), static_cast<u32>(ppu.lr), word(0x1008f07c)));
			sample(ppu, "native_entry");
		}
		if ((address == 0x5b0f0 || address == 0x5b0f8) && select_wait_observations++ < 128)
		{
			write(fmt::format("{\"type\":\"native_select_wait\",\"ms\":%u,\"frame\":%u,\"address\":%u,\"getter_return\":%u,\"mode\":%u,\"scene\":%u,\"request_applied\":%s}\n",
				elapsed(), word(0x1008f07c), address, static_cast<u32>(ppu.gpr[3]), word(pbd::t5dr::game_mode_address), word(pbd::t5dr::scene_address), applied ? "true" : "false"));
		}
		return request;
	}

	void detail::boot_observation::observe_draw(ppu_thread& ppu, u32 address)
	{
		u32 core = 0;
		bool entry = false;
		if (address == 0x3afc3c || address == 0x3afd48)
		{
			core = 1;
			entry = address == 0x3afc3c;
		}
		if (address == 0xe859c || address == 0xe85c4)
		{
			core = 2;
			entry = address == 0xe859c;
		}
		if (address == 0x66950 || address == 0x66af8)
		{
			core = 3;
			entry = address == 0x66950;
		}
		if (core)
		{
			const auto key = std::pair{ppu.id, core};
			if (entry)
			{
				if (pending.contains(key) || pending.size() >= 256)
					++unmatched;
				else
					pending[key] = {static_cast<u32>(ppu.lr), core == 1 ? static_cast<u32>(ppu.gpr[13]) : core == 2 ? static_cast<u32>(ppu.gpr[3]) :
																													  3,
						word(0x1008f07c), ppu.gpr[1]};
			}
			else
			{
				const auto found = pending.find(key);
				if (found == pending.end())
					++unmatched;
				else
				{
					const auto call = found->second;
					pending.erase(found);
					if (call.sp != ppu.gpr[1] || call.caller != static_cast<u32>(ppu.lr))
						++unmatched;
					else if (rolls.size() >= 256 && !rolls.contains({ppu.id, core, call.key, call.caller}))
						++unmatched;
					else
					{
						auto& f = rolls[{ppu.id, core, call.key, call.caller}];
						const u32 value = static_cast<u32>(ppu.gpr[3]);
						++f.count;
						++draws;
						f.hash = mix(f.hash, value);
						write(fmt::format("{\"type\":\"draw\",\"thread\":%u,\"core\":%u,\"generator\":%u,\"caller\":%u,\"frame\":%u,\"value\":%u,\"count\":%u,\"hash\":%u}\n",
							ppu.id, core, call.key, call.caller, call.frame, value, f.count, f.hash));
					}
				}
			}
		}
	}

	void memory_allocated(cpu_thread& cpu, u32 address, u32 bytes) noexcept
	{
		if (bytes != 0x3700000)
			return;
		detail::boot_observation* observation = nullptr;
		try
		{
			auto& s = state();
			observation = &s;
			if (!s.enabled || s.failed || cpu.get_name().find("CPamfDemuxer") == std::string::npos)
				return;
			std::lock_guard lock(s.mutex);
			s.media_allocations[address] = cpu.id;
			s.media_worker = cpu.id;
			++s.media_generation;
			s.media_free_observed = false;
			s.media_exit_observed = false;
			s.write(fmt::format("{\"type\":\"media_allocated\",\"ms\":%u,\"frame\":%u,\"address\":%u,\"bytes\":%u,\"worker\":%u,\"generation\":%u}\n",
				s.elapsed(), word(0x1008f07c), address, bytes, cpu.id, s.media_generation));
		}
		catch (...)
		{
			observation_failed(observation, "media allocation observation");
		}
	}

	void memory_freed(cpu_thread& cpu, u32 address, u32 bytes) noexcept
	{
		if (bytes != 0x3700000)
			return;
		detail::boot_observation* observation = nullptr;
		try
		{
			auto& s = state();
			observation = &s;
			if (!s.enabled || s.failed)
				return;
			std::lock_guard lock(s.mutex);
			const auto found = s.media_allocations.find(address);
			if (found == s.media_allocations.end())
				return;
			const u32 worker = found->second;
			s.media_allocations.erase(found);
			if (worker == s.media_worker)
				s.media_free_observed = true;
			s.write(fmt::format("{\"type\":\"media_freed\",\"ms\":%u,\"frame\":%u,\"address\":%u,\"bytes\":%u,\"worker\":%u,\"freeing_thread\":%u,\"generation\":%u}\n",
				s.elapsed(), word(0x1008f07c), address, bytes, worker, cpu.id, s.media_generation));
		}
		catch (...)
		{
			observation_failed(observation, "media release observation");
		}
	}

	void thread_exited(ppu_thread& ppu) noexcept
	{
		detail::boot_observation* observation = nullptr;
		try
		{
			auto& s = state();
			observation = &s;
			if (!s.enabled || s.failed)
				return;
			std::lock_guard lock(s.mutex);
			if (!s.media_worker || ppu.id != s.media_worker)
				return;
			s.media_exit_observed = true;
			s.write(fmt::format("{\"type\":\"media_worker_exited\",\"ms\":%u,\"frame\":%u,\"worker\":%u,\"generation\":%u,\"allocations_live\":%u}\n",
				s.elapsed(), word(0x1008f07c), ppu.id, s.media_generation, s.media_allocations.size()));
		}
		catch (...)
		{
			observation_failed(observation, "media worker exit observation");
		}
	}

	bool event(ppu_thread& ppu, u32 address) noexcept
	{
		detail::boot_observation* observation = nullptr;
		try
		{
			auto& s = state();
			observation = &s;
			if (!s.enabled)
				return true;
			if (s.failed)
				return false;
			bool request = false;
			{
				std::lock_guard lock(s.mutex);
				if (ppu.id == pbd::t5dr::main_ppu_id)
				{
					request = s.observe_owner(ppu, address);
				}
				s.observe_draw(ppu, address);
			}
			if (request && !apply(ppu, s))
				return false;
			return !s.failed;
		}
		catch (...)
		{
			observation_failed(observation, "boot boundary observation");
			return false;
		}
	}
} // namespace pbd::boot_request
