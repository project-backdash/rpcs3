#pragma once

#include "PPUInterpreter.h"
#include "../PBD/pbd_input_taps.h"
#include "../PBD/pbd_t5dr_memory.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace pbd::practice
{
	struct pbd_lesson_game;
}
enum class thread_state : u32;
namespace pbd::spectator
{
	struct manifest;
}
namespace pbd::spectator_standby
{
	struct policy;
}

// Enabled only while a tick correction ring is configured; interpreter store sites then report spans before writing.
extern std::atomic<bool> g_pbd_tick_undo_enabled;
void pbd_tick_undo_store(ppu_thread& ppu, u64 address, u32 bytes) noexcept;

// PBD: compiled write capture. Requested per process through PBD_LLVM_UNDO=1 and read once per
// compilation, so it namespaces objects (the undo1- tag in the object name) instead of changing behaviour inside an already compiled module.
// Ordinary runs leave it unset and keep their existing objects untouched.
bool pbd_llvm_undo_requested();
bool pbd_jit_copy16_enabled();
u32 pbd_jit_inline_leaf_mode();
u32 pbd_jit_scalar_mode();

// PBD write-fault capture: PBD_TICK_UNDO_CAPTURE=fault. The latch write-protects candidate guest
// pages on vm::g_base_addr (the sudo alias at +4 GiB stays writable for its own restores and reads) and the host
// fault handler asks the latch first: an armed page is captured once per tick from the fault, unprotected, and
// re-armed at the next boundary. No per-store instrumentation runs in generated code or the interpreter.
extern std::atomic<bool> g_pbd_fault_capture_active;
// Nonzero while the owner PPU re-executes a corrected window tick; ppu_execute_syscall then reports each
// syscall's wall time through pbd_replay_syscall_time (owner thread only; diagnostic, changes no syscall).
extern std::atomic<bool> g_pbd_replay_timing;
extern std::atomic<bool> g_pbd_replay_frame;
// Counts thread and address-space lifecycle syscalls while an in-memory replay anchor is live.
extern std::atomic<bool> g_pbd_takeover_anchor_live;
extern std::atomic<u64> g_pbd_takeover_lifecycle_syscalls;
void pbd_takeover_note_syscall(u64 code) noexcept;
// Poll-age instrument (record only): steady_clock ns of the newest cellPadGetData call per port (0, 1) and of the newest
// completed poll of the host pad handlers. The latch logs their ages at every boundary's entry sample ("sample" rows).
extern std::atomic<s64> g_pbd_pad_getdata_ns[2];
extern std::atomic<s64> g_pbd_pad_poll_ns;
u16 pbd_local_pad_word() noexcept; // Modules/pbd_pad_words.cpp: this PC's controller, for the replay controls
// The same for one reader that reads it once per frame: a button tapped since that reader's previous read reads as held.
u16 pbd_local_pad_word(pbd::input_taps::reader reader) noexcept;
std::array<u16, 2> pbd_replay_pad_words() noexcept;   // Both real controllers, read under one pad lock.
std::optional<u16> pbd_local_pad_word_try() noexcept; // the same without waiting for the pad lock (the game window's GUI thread)
// Input/pbd_pad_spectator.cpp: a watcher's own controllers as one canonical word, read by the pad thread through handler instances of its
// own (the spectator's ports are virtual and neutral); 0 outside spectating, while the game window does not have the focus, or
// with no pad.
u16 pbd_spectator_pad_word() noexcept;
void pbd_replay_syscall_time(u64 code, u64 ns) noexcept;
extern u64 g_pbd_fault_bits[16384];
bool pbd_tick_write_fault(u32 addr) noexcept;                             // true: captured or already handled, retry the store
void pbd_tick_pages_remapped(u32 addr, u32 size, bool writable) noexcept; // vm map / unmap / protect replaced host protection

namespace pbd
{
	namespace test
	{
		struct peer_wait_fixture;
		struct practice_moment_fixture;
	} // namespace test

	namespace match_bind
	{
		struct configuration;
	}
	namespace practice_store
	{
		struct memory;
	}

	// The session at the game's frame boundary: input delay, guest-state checkpoints and restore, re-execution, peer link.
	class rollback_session
	{
	public:
		static constexpr std::array<u32, 4> pcs{pbd::t5dr::boundary_pc, 0x12f80, 0x12ec0, 0x12efc};
		static constexpr std::array<u32, 4> opcodes{0x38000070, 0x60000000, 0x48223e85, 0x60000000};
		static constexpr u32 max_updates = 65536; // Physical history capacity; continuous sessions retain absolute tick identities.
		static constexpr int slot_for(u32 address) noexcept
		{
			for (u32 index = 0; index < pcs.size(); index++)
				if (pcs[index] == address)
					return index;
			return -1;
		}

		rollback_session() noexcept;
		~rollback_session();
		rollback_session(const rollback_session&) = delete;
		rollback_session& operator=(const rollback_session&) = delete;
		static bool requested() noexcept;
		bool configure(const std::array<ppu_intrp_func_t, 4>& originals, const char* dispatch_failure, bool compiled = false) noexcept;
		ppu_intrp_func_t original(u32 slot) const noexcept
		{
			return m_originals[slot].load();
		}
		void observe(ppu_thread& ppu, u32 pc) noexcept;
		// Journal the owner's timebase reads of tick N and replay them when it re-executes.
		u64 tick_timebase(ppu_thread& ppu, u32 pc, u64 live) noexcept;
		// GPU duration consumed by the game's load monitor, in the same logical clock domain as CPU work.
		u64 gpu_elapsed_time(ppu_thread& ppu, u64 measured) const noexcept;
		// Rolling undo: before a main-PPU interpreter store, save pages outside the static tick write set it touches.
		void tick_undo_store(ppu_thread& ppu, u64 address, u32 bytes) noexcept;
		// Write-fault capture: a write faulted on an armed page (page = address >> 12).
		bool tick_write_fault(u32 page) noexcept;
		void replay_syscall_time(u64 code, u64 ns) noexcept; // syscall wall time inside a replayed tick
		void tick_pages_remapped(u32 addr, u32 size, bool writable) noexcept;
		void invalidate() noexcept;
		void operator=(thread_state state) noexcept;

	private:
		friend struct test::peer_wait_fixture;
		friend struct test::practice_moment_fixture;
		struct storage;
		struct boundary_call;
		struct lobby_command_steps;
		struct takeover_restore_steps;
		struct spectator_boundary_steps; // observe()'s steps at one boundary (pbd_rollback_session_boundary.h)
		struct tick_dispatch;
		struct peer_boundary_steps;
		struct phase_input_steps; // the stages of one boundary's tick (pbd_rollback_session_boundary.h)
		struct practice_steps;    // the steps of practice_controls_at() (pbd_rollback_session_boundary.h)
		struct replay_steps;
		struct replay_pad_steps; // the steps of replay_controls_at() (pbd_rollback_session_boundary.h)
		struct dummy_word_steps;
		struct dummy_cycle_steps; // two steps of practice_dummy_word() (pbd_rollback_session_boundary.h)
		std::unique_ptr<storage> m_storage;
		// Practice's saved settings and Continue (pbd_practice_memory.h), when the launcher names a store; owner thread only.
		std::unique_ptr<practice_store::memory> m_practice_memory;
		std::array<std::atomic<ppu_intrp_func_t>, 4> m_originals{};
		std::atomic<bool> m_active{false};
		std::mutex m_setup_mutex;
		bool m_stopping = false;
		bool m_finalized = false;
		void finish(u32 reason) noexcept;
		void finalize() noexcept;
		void fault_protect(std::vector<u32>& pages, bool arm) noexcept;
		void fault_arm_all() noexcept;
		void fault_rearm() noexcept;
		void fault_disarm_all() noexcept;
		bool write_header() noexcept;
		bool write_status(const char* state, const char* reason) noexcept;
		std::string pad_identity() const;
		std::string initial_state_record() const;
		void write_records() noexcept;
		// A watcher's set (pbd_rollback_session_lobby.cpp): its mode and input policy, and the set's manifest taken at
		// launch or at a standby watcher's bind. accept_spectator returns null when the manifest is taken, otherwise the refusal.
		bool watcher_mode() const;
		spectator_standby::policy watcher_policy() const;
		const char* accept_spectator(const std::filesystem::path& root, pbd::spectator::manifest&& manifest);

		// configure()'s steps. Each throws session_error on a bad switch, which configure() turns into a setup failure. Most
		// read switches that only Windows builds take.
		template <typename KeepPeer>
		struct overlay_controls;
		struct lesson_steps;
		struct practice_choice_steps;
		struct local_input_steps;
		struct peer_poll_steps;
		struct record_writer;
		struct undo_capture;
		struct session_configuration;
		struct peer_configuration;
		struct peer_receiver;
		struct match_standby_steps;
		struct setup_inputs;
		void configure_output(const setup_inputs& sources);
		void read_input_history(setup_inputs& sources);
		void validate_dispatch_modes(const std::array<ppu_intrp_func_t, 4>& originals, bool compiled);
		void configure_local_modes();
		void write_setup_headers();
		void start_session_workers(const std::array<ppu_intrp_func_t, 4>& originals);
		void abort_setup();
		void finish_lobby_leave();
		void finish_hud();
		void write_audio_health();
		void write_dummy_health();
		void write_input_policy();
		void send_session_end(u32 reason);
		void restore_session_presentation();
		void configure_watch_channels();
		void configure_lobby_channel(const std::filesystem::path& root, bool late_bound = false);
		void configure_live_recording();
		void configure_spectator(const std::filesystem::path& root);
		void check_start_state();
		void prepare_undo_capture();
		void write_undo_policy();
#ifdef _WIN32
		bool parse_history_index(const char* name, u32& out, const char* message);
		void configure_input_options();
		void configure_tick_trace();
		void configure_page_set();
		void read_checkpoint_pages(const wchar_t* checkpoint_pages, bool public_capture);
		void configure_capture_runs();
		void configure_checkpoint_exclusions(bool public_capture);
		void configure_undo_checks();
		void configure_undo_denials();
		void configure_checkpoint_checks();
		void configure_initial_state();
		void configure_replay_modes();
		void configure_session();
		void configure_match_replay();
		void configure_practice();
		void configure_practice_rewind(const wchar_t* practice);
		void configure_practice_dummy(const wchar_t* dummy);
		void configure_practice_memory();
		void configure_rolling();
		void configure_peer(const match_bind::configuration* bound = nullptr);
		void configure_correct_at();
		void start_peer_receiver();
#endif
	};
} // namespace pbd
