#pragma once

// Shared by the update latch's source files and by nothing else: the includes they need, the game addresses and
// limits the latch checks, the row, tick and checkpoint types, and the helpers over guest memory, files and JSON text.
// Every function and variable here is inline, so the files that include it share one definition of each.

#include "pbd_rollback_session.h"
#include "Emu/RSX/pbd_frame_presentation.h"
#include "Emu/RSX/pbd_replay_pacing.h"
#include "pbd_live_stats.h"
#include "Emu/PBD/pbd_replay_viewer_menu.h"
#include "pbd_game_overlay.h"
#include "pbd_search_badge.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "pbd_practice_mouse.h"
#include "pbd_practice_controls.h"
#include "Emu/PBD/pbd_movement_observation.h"
#include "pbd_practice_streak.h"
#include "Emu/PBD/pbd_practice_precision.h"
#include "pbd_practice_rolling.h"
#include "pbd_practice_native.h"
#include "../PBD/pbd_practice_commands.h"
#include "../PBD/pbd_practice_menu.h"
#include "../PBD/pbd_practice_pad.h"
#include "../PBD/pbd_session_start.h"
#include "../PBD/pbd_rematch_native.h"
#include "pbd_game_lifecycle.h"
#include "pbd_corner_chat.h"
#include "pbd_match_recording.h"
#include "pbd_sound_options.h"
#include "pbd_setup_digest.h"
#include "pbd_live_replay.h"
#include "pbd_direct_match.h"
#include "Emu/CPU/pbd_ordinary_metrics.h"
#include "pbd_update_script.h"
#include "pbd_session_history.h"
#include "pbd_fn_hook.h"
#include "../PBD/pbd_character_rule_native.h"
#include "../PBD/pbd_stage_rule_native.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/lv2/sys_mutex.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/Modules/cellGcmSys.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/Cell/lv2/sys_rsx.h"
#include "Input/pad_thread.h"
namespace rsx
{
	void set_native_ui_flip();
}
#include "Emu/CPU/CPUThread.h"
#include "Emu/Io/pad_config.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "pbd_peer_endpoint.h"
#include "pbd_lobby_set_driver.h"
#include "pbd_hidden_selection.h"
#include "pbd_select_timer.h"
#include "pbd_select_band.h"
#include "pbd_vs_cards.h"
#include "Emu/PBD/pbd_set_notices.h"
#include "pbd_notice_art.h"
#include "pbd_game_profile.h"
#include "Emu/PBD/pbd_spectator_channel_input.h"
#include "pbd_session_wait.h"
#include "pbd_player_volume.h"
#include "Emu/PBD/pbd_settings_runtime.h"
#include "pbd_controller_takeover.h"

#include "PPUThread.h"
#include "Crypto/sha256.h"
#include "Emu/PBD/pbd_emulator_identity.h"
extern std::string ppu_get_syscall_name(u64 code);
extern std::atomic<u32> g_pbd_flip_double_done, g_pbd_flip_redelivered, g_pbd_flip_dropped, g_pbd_flip_resets; // sys_rsx.cpp
extern std::atomic<u64> g_pbd_audio_callbacks, g_pbd_audio_underruns, g_pbd_audio_underrun_bytes;              // cellAudio.cpp
extern std::atomic<u64> g_pbd_audio_time_advances, g_pbd_audio_silence_blocks;
#include "Emu/System.h"
#include "Emu/pbd_session_mode.h"
#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_locking.h"
#include "util/vm.hpp"
#include <bit>
#include "Utilities/File.h"
#include "Utilities/Thread.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <shared_mutex>
#include <mutex>
#include <random>
#include <deque>
#include <unordered_map>
#include <map>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include "Emu/system_config.h"
#include "Utilities/JIT.h"

// Write-fault capture helpers, used at each boundary and by the capture functions (pbd_rollback_session_capture.cpp).
#include "../PBD/pbd_checkpoint_capture.h" // namespace pbd::session_runtime

extern u64 g_pbd_restored_timebase;
extern bool g_pbd_restored_timebase_valid;
extern alignas(64) u64 g_pbd_undo_skip_bits[16384];
u64 sys_time_get_timebase_frequency();

LOG_CHANNEL(pbd_session_log, "PBD update latch");

#include "../PBD/pbd_session_samples.h"
#include "../PBD/pbd_checkpoint_memory.h"

namespace pbd::session_runtime
{
	inline void require(bool condition, const char* message)
	{
		if (!condition)
			throw session_error{message};
	}

	inline bool read_dword(u32 address, u64& value) noexcept
	{
		be_t<u64> word{};
		if (!vm::try_access(address, &word, sizeof(word), false))
			return false;
		value = word;
		return true;
	}

	inline bool write_dword(u32 address, u64 value) noexcept
	{
		be_t<u64> word = value;
		return vm::try_access(address, &word, sizeof(word), true);
	}

	inline bool read_word(u32 address, u32& value) noexcept
	{
		be_t<u32> word{};
		if (!vm::try_access(address, &word, sizeof(word), false))
			return false;
		value = word;
		return true;
	}

	// One port's word from the game's input ring through `ring`: the newest slot's word, or `newest` (the word the game's own
	// read returned), with the buttons tapped into slots written since the previous read held for this one. Nothing when the
	// ring cannot be read. `written` receives the count of slots written that the read saw.
	inline std::optional<u16> read_input_ring(pbd::input_taps::ring_reader& ring, u32 port, std::optional<u16> newest = {}, u32* written = nullptr) noexcept
	{
		be_t<u32> count{};
		std::array<be_t<u16>, 16> stored{};
		if (!vm::try_access(input_ring_written_address, &count, sizeof(count), false) ||
			!vm::try_access(input_ring_address + 0x20 * port, stored.data(), sizeof(stored), false))
			return std::nullopt;
		std::array<u16, 16> slots{};
		for (usz i = 0; i < slots.size(); i++)
			slots[i] = stored[i];
		if (written)
			*written = count;
		return ring.read(slots, count, newest.value_or(slots[(count - 1) & 15]));
	}

	// Practice's capture files go through the C runtime, not fs::file: fs::file's write treats a failed WriteFile (a full
	// disk, say) as fatal and ends the process, while fwrite reports it as a short count, so a diagnostic file that cannot
	// be written only stops itself. Opening replaces an earlier file of the same name; others may read it while it grows.
	inline std::FILE* open_diagnostic_file(const std::filesystem::path& path) noexcept
	{
#ifdef _WIN32
		return _wfopen(path.c_str(), L"wb");
#else
		return std::fopen(path.c_str(), "wb");
#endif
	}

	inline bool write_diagnostic_file(const std::filesystem::path& path, std::string_view text) noexcept
	{
		std::FILE* out = open_diagnostic_file(path);
		if (!out)
			return false;
		const bool written = std::fwrite(text.data(), 1, text.size(), out) == text.size();
		return std::fclose(out) == 0 && written;
	}

	struct practice_pause_observation
	{
		bool valid = false, paused = true, counter_read = false;
		u32 blocked = 0, request = 0, menu = 0, mode = 0, counter = 0;
	};

	inline practice_pause_observation read_practice_pause() noexcept
	{
		practice_pause_observation result;
		const bool mode_read = read_word(pbd::t5dr::game_mode_address, result.mode);
		result.counter_read = read_word(0x1008f05c, result.counter);
		// The native predicate tests manager +0 and +0xC. Guard its code and storage instead of the cached inverse flag.
		constexpr std::array<std::array<u32, 2>, 11> code{{{0x1ca6c, 0x8122827c}, {0x1ca74, 0x8009000c}, {0x1ca80, 0x80090000}, {0x1cad4, 0x9069000c},
			{0x154a8, 0x48007a9d}, {0x154c4, 0x480075a9}, {0x154d8, 0x5400fffe},
			{0x154e4, 0x90090058}, {0x154f0, 0x41920324}, {0x15584, 0x900b0000}, {0x1583c, 0x900b0000}}};
		for (const auto& site : code)
		{
			u32 observed = 0;
			if (!read_word(site[0], observed) || observed != site[1])
				return result;
		}
		u32 pointer = 0, pointer_after = 0;
		std::array<be_t<u32>, 4> manager{}, manager_after{};
		if (!read_word(0x4687bc, pointer) || pointer != 0x10092e98 ||
			!vm::try_access(pointer, manager.data(), sizeof(manager), false) ||
			!vm::try_access(pointer, manager_after.data(), sizeof(manager_after), false) ||
			manager != manager_after || !read_word(0x4687bc, pointer_after) || pointer_after != pointer)
			return result;
		result.blocked = manager[0];
		result.request = manager[2];
		result.menu = manager[3];
		result.valid = mode_read && result.mode == 10 && result.counter_read;
		result.paused = !result.valid || result.blocked != 0 || result.menu != 0;
		return result;
	}

	inline bool scene_matches() noexcept
	{
		std::array<be_t<u32>, 2> selectors{};
		const auto& start = initial_state().scenes.front();
		return vm::try_access(pbd::t5dr::scene_address, selectors.data(), sizeof(selectors), false) &&
		       selectors[0] == start.first && selectors[1] == start.second;
	}
	// A scene pair admitted at a boundary: the fixture's battle scene always, the later declared pairs under the lifecycle.
	inline bool scene_admitted(u32 a, u32 b, bool lifecycle) noexcept
	{
		const auto& scenes = initial_state().scenes;
		for (usz i = 0; i < scenes.size(); i++)
			if (scenes[i].first == a && scenes[i].second == b)
				return i == 0 || lifecycle;
		return false;
	}

	inline bool mapping_matches(u32 port) noexcept
	{
		std::array<be_t<u16>, 16> words{};
		if (!vm::try_access(mapping_base + port * 32, words.data(), sizeof(words), false))
			return false;
		for (u32 bit = 0; bit < 16; bit++)
		{
			if (u16{words[bit]} != (bit < 4 ? 0 : 1u << bit))
				return false;
		}
		return true;
	}

	inline u32 mapped_mask(u32 raw) noexcept
	{
		u32 value = raw & 0xfff0;
		if ((value & 0x5000) == 0x5000)
			value &= ~0x5000;
		if ((value & 0xa000) == 0xa000)
			value &= ~0xa000;
		return value;
	}

	inline std::string hash_hex(const void* bytes, usz size)
	{
		std::array<u8, 32> hash{};
		require(mbedtls_sha256_ret(static_cast<const u8*>(bytes), size, hash.data(), 0) == 0, "sha256_failed");
		constexpr char hex[] = "0123456789abcdef";
		std::string result;
		result.reserve(64);
		for (u8 value : hash)
		{
			result += hex[value >> 4];
			result += hex[value & 15];
		}
		return result;
	}

	using pbd::emulator_identity::executable_sha256;

	inline bool lobby_executable_matches(std::string_view expected)
	{
		return expected.size() == 64 && !executable_sha256().empty() && executable_sha256() == expected;
	}

	using pbd::json::nullable_quoted;

	inline const char* json_bool(bool value) noexcept
	{
		return value ? "true" : "false";
	}

	// "Waiting for <name>'s game to load..." over a sync point where this PC holds for the other one (pbd::session_wait). Once
	// the wait has lasted show_after it publishes the elapsed seconds and the Leave this session has, raises the flag the RSX
	// overlay reads and asks for a native UI flip about 30 times a second, as the in-game menu hold does, so the held game's screen
	// shows the modal. The flag drops the moment the wait ends. From leave_after on it reads this PC's controller (every 5 ms,
	// nothing while the in-game Settings menu holds it) and poll() reports a completed two-second Circle hold; the caller then ends
	// the session. The read never reaches a sample: the game is held here, and the session ends at that read.
	struct session_wait_modal
	{
		session_clock::time_point from = session_clock::now(), next_flip{}, next_read{};
		pbd::session_wait::leave_kind kind = pbd::session_wait::leave_kind::none;
		u32 published_seconds = 0;
		pbd::session_wait::leave_read leave_read;
		bool shown = false;

		explicit session_wait_modal(pbd::session_wait::leave_kind leave) : kind(leave) {}
		session_wait_modal(const session_wait_modal&) = delete;
		session_wait_modal& operator=(const session_wait_modal&) = delete;
		~session_wait_modal()
		{
			end();
		}

		// Called on every pass of the wait loop; true when the player took Leave at this read.
		bool poll()
		{
			const auto now = session_clock::now();
			const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - from);
			if (!shown)
			{
				if (waited < pbd::session_wait::show_after)
					return false;
				shown = true;
				published_seconds = static_cast<u32>(waited.count() / 1000);
				pbd::session_wait::seconds().store(published_seconds, std::memory_order_release);
				pbd::session_wait::leave().store(kind, std::memory_order_release);
				pbd::session_wait::leave_held_ms().store(0, std::memory_order_release);
				pbd::session_wait::showing().store(true, std::memory_order_release);
			}
			if (const u32 whole = static_cast<u32>(waited.count() / 1000); whole != published_seconds)
			{
				published_seconds = whole;
				pbd::session_wait::seconds().store(whole, std::memory_order_release);
			}
			bool chosen = false;
			if (kind != pbd::session_wait::leave_kind::none && waited >= pbd::session_wait::leave_after && now >= next_read)
			{
				next_read = now + std::chrono::milliseconds(5);
				if (pbd::settings_menu::claimed())
					leave_read = {};
				else
					leave_read = pbd::session_wait::read_leave(waited, kind, pbd_local_pad_word(), leave_read);
				pbd::session_wait::leave_held_ms().store(leave_read.held_ms, std::memory_order_release);
				chosen = leave_read.chosen;
			}
			if (now >= next_flip)
			{
				next_flip = now + std::chrono::milliseconds(33);
				rsx::set_native_ui_flip();
			}
			return chosen;
		}
		void end() noexcept
		{
			if (shown)
			{
				pbd::session_wait::showing().store(false, std::memory_order_release);
				pbd::session_wait::leave().store(pbd::session_wait::leave_kind::none, std::memory_order_release);
				pbd::session_wait::seconds().store(0, std::memory_order_release);
				pbd::session_wait::leave_held_ms().store(0, std::memory_order_release);
			}
			shown = false;
			leave_read = {};
		}
	};

	inline bool replay_sha256_text(std::string_view value) noexcept
	{
		return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch)
										 {
											 return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
										 });
	}

	// The loader captured this identity before opening the actual savestate reader. Its accessor rejects an old boot generation.
	// Bind to that file, so an environment flag cannot bless a continuing match or a different checkpoint with a similar name.
	inline bool replay_clean_checkpoint(const std::filesystem::path& declared, std::string_view expected_hash)
	{
		std::error_code error;
		if (!declared.is_absolute() || !replay_sha256_text(expected_hash))
			return false;
		const auto identity = Emu.PBDGetCheckpointBootIdentity();
		if (!identity || identity->bytes < 8 || identity->bytes > 2ull * 1024 * 1024 * 1024 || identity->sha256 != expected_hash)
			return false;
		const auto actual = std::filesystem::weakly_canonical(std::filesystem::path(std::u8string(identity->path.begin(), identity->path.end())), error);
		if (error)
			return false;
		const auto wanted = std::filesystem::weakly_canonical(declared, error);
		return !error && std::filesystem::equivalent(actual, wanted, error) && !error;
	}
	inline std::string number_or_null(bool present, u64 value)
	{
		return present ? fmt::format("%u", value) : "null";
	}
	// The end of a replay's fast forward (replay_state::fast_state::active): the vblank rate and the sound return to what they were.
	template <typename Storage>
	void end_replay_fast(Storage& data, u32 index) noexcept
	{
		if (!data.replay.fast.active)
			return;
		data.replay.fast.active = false;
		data.replay.fast.to_match = 0;
		data.replay.fast.to_frame = 0;
		data.replay.fast.until_match = 0;
		data.takeover.next_round = 0;
		data.takeover.next_match = 0;
		if (data.replay.viewer_enabled)
			data.replay.viewer.barrier(pbd_local_pad_word());
		g_cfg.video.vblank_rate.set(data.replay.fast.saved_vblank);
		pbd::replay_pacing::vblank_request.store(static_cast<u32>(data.replay.fast.saved_vblank), std::memory_order_release);
		pbd::player_volume::restore();
		pbd_session_log.warning("Replay: fast forward ends at boundary %u (match %u played); normal speed and sound.", index, data.session.matches);
	}

	inline const char* reason_text(u32 reason) noexcept
	{
		switch (reason)
		{
		case update_limit: return "update_limit";
		case output_failed: return "output_failed";
		case guard_failed: return "guard_failed";
		case setup_failed: return "setup_failed";
		case hook_invalidated: return "hook_invalidated";
		case user_stopped: return "user_stopped";
		case match_ended: return "match_ended";
		case left_vs_mode: return "left_vs_mode";
		case takeover_relaunch: return "takeover_relaunch";
		case lobby_set_ended: return "lobby_set_ended";
		default: return "emulation_stopped";
		}
	}
	using pbd::practice::dummy_state;

	// Closing the game window stops the emulation while the owner may be inside a session wait (a barrier wait can last long
	// enough for RPCS3 to report "PS3 Game/Application Is Unresponsive"). Every long session wait ends when the emulation stops.
	inline bool pbd_emulation_stopping(const ppu_thread& ppu) noexcept
	{
		return !!(ppu.state & (cpu_flag::exit + cpu_flag::req_exit)) || Emu.IsStopped();
	}

	// One word of host entropy for a choice only this PC makes and the recorded input carries (the random stage's roulette
	// spin). The game's own generator is never read or advanced for it. A random_device that cannot be opened falls back to the
	// high-resolution clock, which is still independent of the game.
	inline u32 host_random_word() noexcept
	{
		try
		{
			std::random_device device;
			return static_cast<u32>(device());
		}
		catch (...)
		{
			const auto ticks = static_cast<u64>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
			return static_cast<u32>(ticks ^ (ticks >> 32));
		}
	}
} // namespace pbd::session_runtime

using namespace pbd::session_runtime;
