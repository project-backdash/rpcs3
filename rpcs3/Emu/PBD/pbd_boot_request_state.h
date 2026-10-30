#pragma once

#include "../Cell/pbd_boot_request.h"
#include "pbd_env.h"
#include "pbd_guest_call.h"
#include "pbd_owner_flags.h"
#include "pbd_t5dr_memory.h"
#include "../Cell/pbd_practice_native.h"
#include "../Cell/PPUThread.h"
#include "Crypto/sha256.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"
#include "Utilities/File.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <vector>

extern atomic_t<bool> g_user_asked_for_screenshot;

namespace pbd::boot_request::detail
{
	// Private linkage/payload above the injected callee's SP; retained guard bytes at the low end.
	constexpr u32 scratch_bytes = 0x8000, linkage_bytes = 0x100;
	constexpr u64 basis = 0xcbf29ce484222325ull, prime = 0x100000001b3ull;
	std::string environment(const char* name);
	u32 word(u32 address);
	u32 byte(u32 address);
	u32 halfword(u32 address);
	u32 movie_child();
	std::string digest(u32 address, u32 bytes);
	u64 mix(u64 hash, u32 value);
	using restore_scope = pbd::guest_call::restore_scope<scratch_bytes>;
	struct boot_observation
	{
		fs::file output;
		std::mutex mutex;
		std::string target;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		u64 bytes = 0, draws = 0, unmatched = 0;
		std::atomic<bool> failed{false};
		bool enabled = false, attempted = false, applied = false, executing = false;
		bool screenshots = false, select_image = false, fight_image = false;
		bool getter_diagnostic = false;
		bool media_retire = false, teardown_requested = false, replay = false, capped = false;
		u32 teardown_child = 0, teardown_handle_generation = 0, teardown_media_generation = 0;
		u32 select_wait_observations = 0;
		std::map<u32, u32> media_allocations; // guest address -> allocating worker ID
		u32 media_worker = 0, media_generation = 0;
		bool media_free_observed = false, media_exit_observed = false;
		u32 image_scene = ~0u, image_stable = 0;
		u32 image_phase = ~0u, phase_stable = 0;
		u32 title = 0, stable = 0, last_frame = ~0u;
		std::array<u32, 4> previous{~0u, ~0u, ~0u, ~0u};
		std::array<u32, 2> previous_phase{~0u, ~0u};
		struct call
		{
			u32 caller, key, frame;
			u64 sp;
		};
		struct fingerprint
		{
			u64 count = 0, hash = basis;
		};
		std::map<std::pair<u32, u32>, call> pending;
		std::map<std::tuple<u32, u32, u32, u32>, fingerprint> rolls;

		boot_observation()
		{
			const auto path = environment("PBD_BOOT_WITNESS");
			target = environment("PBD_BOOT_TARGET");
			screenshots = environment("PBD_BOOT_SCREENSHOTS") == "1";
			getter_diagnostic = environment("PBD_BOOT_GETTER_DIAGNOSTIC") == "1";
			// A replay's fresh boot always retires the title movie first: with the movie's buffer still held, the character select's
			// load is refused memory and the game reads into an unmapped page.
			replay = !environment("PBD_REPLAY_V2_GAME").empty();
			media_retire = environment("PBD_BOOT_MEDIA_RETIRE") == "1" || replay;
			enabled = !path.empty() || !target.empty();
			if (!enabled)
				return;
			if (path.empty() || (!target.empty() && target != "vs" && target != "practice"))
			{
				failed = true;
				return;
			}
			output = fs::file(path, fs::rewrite);
			if (!output)
			{
				failed = true;
				return;
			}
			write(fmt::format("{\"type\":\"identity\",\"schema\":\"pbd-native-boot-witness-v1\",\"target\":\"%s\",\"control\":%s,\"rng_inventory_exhaustive\":false,\"mt_globally_coherent\":false,\"max_bytes\":%u}\n",
				target, target.empty() ? "true" : "false", 32u * 1024 * 1024));
		}
		void write(const std::string& row)
		{
			if (failed || capped)
				return;
			// A replay keeps the witness through its whole game, whose draws outgrow the cap long after the boot it records is
			// over: the witness then stops, and the game plays on.
			if (replay && bytes + row.size() > 32ull * 1024 * 1024)
			{
				capped = true;
				return;
			}
			if (row.size() > 65536 || bytes + row.size() > 32ull * 1024 * 1024 || output.write(row.data(), row.size()) != row.size())
			{
				failed = true;
				return;
			}
			bytes += row.size();
		}
		u64 elapsed() const
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		}
		void sample(ppu_thread& ppu, const char* phase)
		{
			std::ostringstream row;
			row << "{\"type\":\"state\",\"phase\":\"" << phase << "\",\"ms\":" << elapsed()
				<< ",\"frame\":" << word(0x1008f07c) << ",\"pad_frame\":" << word(0x1008eb9c)
				<< ",\"scene_timer\":" << word(0x1008eba8) << ",\"manager\":" << word(0x1008f060)
				<< ",\"pending\":" << word(0x1008f064) << ",\"scene\":" << word(pbd::t5dr::scene_address)
				<< ",\"subscene\":" << word(pbd::t5dr::subscene_address) << ",\"mode\":" << word(pbd::t5dr::game_mode_address)
				<< ",\"current_phase\":" << word(0x1009322c) << ",\"pending_phase\":" << word(0x10093230)
				<< ",\"native_round_ordinal\":" << word(0x1008f0e4)
				<< ",\"title_cursor\":" << word(0x10093464) << ",\"title_last\":" << byte(0x10093460)
				<< ",\"title_object\":" << title << ",\"title_initialized\":" << (title ? word(title + 0x80) : ~0u)
				<< ",\"native_title_state\":" << (title ? word(title + 0x58) : ~0u)
				<< ",\"prefs\":[";
			for (u32 i = 0; i < 6; ++i)
			{
				if (i)
					row << ',';
				row << byte(0x10684964 + i);
			}
			row << "],\"rounds_source\":" << byte(0x1008ebd4) << ",\"humans\":" << word(0x1008f0b0)
				<< ",\"flow_rules\":[" << word(0x1008f094) << ',' << word(0x1008f098) << ',' << word(0x1008f09c)
				<< ',' << word(0x1008f0a0) << ',' << word(0x1008f0a4) << ',' << word(0x1008f0dc)
				<< "],\"characters\":[" << word(0x10093f50) << ',' << word(0x10093f54)
				<< "],\"cursors\":[" << word(0x10093f58) << ',' << word(0x10093fd0)
				<< "],\"random_panels\":[" << word(0x10093f5c) << ',' << word(0x10093fd4)
				<< "],\"mokujin_style_table\":[";
			for (u32 i = 0; i < 18; ++i)
			{
				if (i)
					row << ',';
				row << byte(0x10093470 + i);
			}
			row << "],\"native_styles\":[" << halfword(0x1009007e) << ',' << halfword(0x1009094e)
				<< "],\"native_species\":[" << halfword(0x10090080) << ',' << halfword(0x10090950)
				<< "],\"round_random_words\":[" << halfword(0x100901b6) << ',' << halfword(0x10090a86)
				<< "],\"native_replay_flag\":" << word(0x10425ce0)
				<< ",\"availability\":[" << word(0x100934a0) << ',' << word(0x100934a4)
				<< "],\"stage\":" << word(0x1008f088) << ",\"health\":[" << word(0x100906ec) << ',' << word(0x10090fbc)
				<< "],\"battle_timer\":" << word(0x1008f054) << ",\"flow_lcg\":[" << word(0x1008f0c4) << ',' << word(0x1008f0d0)
				<< "],\"media_generation\":" << media_generation << ",\"media_worker\":" << media_worker
				<< ",\"media_allocations_live\":" << media_allocations.size()
				<< ",\"media_free_observed\":" << (media_free_observed ? "true" : "false")
				<< ",\"media_exit_observed\":" << (media_exit_observed ? "true" : "false")
				<< ",\"movie_child\":" << movie_child()
				<< ",\"movie_task_state\":" << (movie_child() ? word(movie_child() + 0x48) : ~0u)
				<< ",\"movie_stop_requested\":" << (movie_child() ? word(movie_child() + 0x64) : ~0u)
				<< ",\"teardown_requested\":" << (teardown_requested ? "true" : "false")
				<< ",\"demux_state\":" << word(0x10532a38 + 0x2ac)
				<< ",\"demux_joined\":" << byte(0x10532a38 + 0x2bc)
				<< ",\"libc_seed\":" << word(static_cast<u32>(ppu.gpr[13]) - 0x7000)
				<< ",\"libc_shuffle_sha256\":\"" << digest(static_cast<u32>(ppu.gpr[13]) + 0x9094, 136)
				<< "\",\"mt_index\":" << word(0x10053578) << ",\"mt_table_sha256\":\"" << digest(0x10095168, 2496)
				<< "\",\"draws\":" << draws << ",\"unmatched\":" << unmatched << ",\"rng_callers\":[";
			bool first = true;
			for (const auto& [key, value] : rolls)
			{
				if (!first)
					row << ',';
				first = false;
				const auto [thread, core, generator, caller] = key;
				row << '[' << thread << ',' << core << ',' << generator << ',' << caller << ',' << value.count << ',' << value.hash << ']';
			}
			row << "]}\n";
			write(row.str());
		}

		bool observe_owner(ppu_thread& ppu, u32 address);
		void observe_draw(ppu_thread& ppu, u32 address);
		bool finish_request(ppu_thread& ppu, bool teardown, u32 child, u32 callback);
	};

	boot_observation& state();
	bool apply(ppu_thread& ppu, boot_observation& s);
} // namespace pbd::boot_request::detail
