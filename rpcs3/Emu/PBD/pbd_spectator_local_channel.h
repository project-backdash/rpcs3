#pragma once

#include "pbd_spectator_channel_model.h"

namespace pbd::spectator
{
	class local_channel
	{
		std::filesystem::path m_directory;
		manifest m_identity;
		std::deque<frame> m_frames;
		presentation m_presentation;
		u64 m_native_generation = 0, m_control_serial = 0, m_report_serial = 0;
		u32 m_packet_serial = 0, m_game = 0, m_origin = 0, m_receipt = 0, m_consumed = 0, m_checked = 0;
		u32 m_packet_through = 0;
		digest m_packet_digest{};
		std::optional<u32> m_pbd_native_anchor;
		u32 pbd_source_boundary(u32 boundary) const
		{
			if (m_identity.pbd_capsule.empty())
				return boundary;
			if (!m_pbd_native_anchor || boundary < *m_pbd_native_anchor || m_identity.pbd_source_origin >= maximum_boundary ||
				boundary - *m_pbd_native_anchor > maximum_boundary - m_identity.pbd_source_origin - 1)
				return ~0u;
			return m_identity.pbd_source_origin + 1 + boundary - *m_pbd_native_anchor;
		}
		std::optional<u32> m_end;
		std::optional<native_round> m_expected_round;
		// This game's picks: what the lobby reported for it (begin_game or a game_picks marker, whichever came), and what the game
		// applied at its first intro. Whichever arrives second is checked against the first; the intro never waits for a report.
		std::optional<picks> m_reported_picks, m_applied_picks;
		u64 m_rematch_epoch = 0;
		std::optional<std::pair<pbd::rematch::load, pbd::rematch::sealed_state>> m_rematch;
		// This game's Rematch load, if it has one: what its picks must keep (pbd::rematch::matches).
		std::optional<pbd::rematch::load> m_load;
		// The values the players' games wrote for the game's own Ranked screens (begin_game's rankScreens), written the same here.
		std::optional<pbd::rank_stakes::game_values> m_rank_screens;
		picks fixed_picks() const
		{
			return m_load ? picks{m_load->selected.stage, m_load->selected.rows[0], m_load->selected.rows[1]} : m_identity.fixed();
		}
		bool m_round_required = false, m_from_start = false, m_direct_load = false;
		bool m_prepared = false, m_visible = false, m_reserved = false, m_ended = false, m_stopped = false, m_failed = false;
		// A drawn game ended the watch: the lobby replays it under its own number, which this process does not follow.
		bool m_draw_closed = false;
		std::string m_reason;
		// The watcher's own screen (pbd_spectator_ui): the launcher's last watch_ui, whether the watched game has a fight on screen
		// at the last boundary, the open chat's Quick-line highlight, and the serial of the last pad event file. Display and pad
		// events only: none of it reaches the watched game.
		std::optional<spectator_ui::state> m_ui;
		spectator_ui::pad_reader m_pad;
		bool m_fight = false;
		// The in-game menu holds the watcher's controller (read_pad): the screen does not show meanwhile.
		bool m_menu = false;
		u64 m_pad_serial = 0, m_preview_since = 0;
		// The arrival of the last waiting control that named the game this watch waits for (input_wait_outcome).
		std::optional<std::chrono::steady_clock::time_point> m_alive_at;
		std::optional<std::chrono::steady_clock::time_point> m_input_read_denied;
		bool ui_shown() const
		{
			return m_ui && m_fight && !m_menu && m_visible && !m_presentation.hidden && !m_presentation.broadcast && healthy();
		}
		void draw_ui();
		void draw()
		{
			auto shown = m_presentation;
			shown.hidden = shown.hidden || !can_show() || !healthy();
			publish_presentation(m_native_generation, shown);
			draw_ui();
		}
		// Selection can show only after exact native consumption. The first intro still requires its actual round witness.
		bool can_show() const
		{
			return m_visible || (m_from_start && !m_direct_load && m_consumed && !m_ended);
		}
		// An immutable file in the channel directory, published under its final name only when complete; an existing name is never
		// replaced.
		static bool write_new(const std::filesystem::path& destination, const std::filesystem::path& temporary, const std::string& text);
		// One watcher press for the launcher: spectator-pad-N.json, schema pbd-spectator-pad-1, with its own contiguous serial. A press
		// that cannot be written is dropped and the watch goes on; the next press takes the same serial.
		bool publish_pad(const spectator_ui::press& press);
		bool publish(std::string_view type, value payload);
		// A Rematch game loads straight into VS from the previous game's verified end. Either watch contract can follow it as long
		// as the actual round call is checked: a selection stream receives it in game_witness, a start-at-intro stream with
		// begin_game. The previous game's applied picks and end are read back the same way in both.
		bool accept_rematch(const value& root, u32 game, bool round_required, const pbd::rematch::options* actual);
		void clear_game_presentation(u32 game);

		bool begin_game(const value& root, const pbd::rematch::options* actual_options);

		bool control_stop();

		bool control_game_state(const value& root);

		bool control_end_game(const value& root);

		bool control_game_picks(const value& root);

		bool control_presentation(const value& root);

		bool control_chat(const value& root);

		bool control_waiting(const value& root);

		bool control_watch_ui(const value& root);

		bool apply_control(const value& root, const pbd::rematch::options* actual_options);

		bool decode_frames(const std::uint8_t* bytes, u32 count, std::size_t stride, bool applied, std::deque<frame>& incoming);

	public:
		bool needs_round_state() const
		{
			return m_round_required;
		}
		auto take_rematch()
		{
			return std::exchange(m_rematch, std::nullopt);
		}
		local_channel(std::filesystem::path directory, manifest identity) : m_directory(std::move(directory)), m_identity(std::move(identity))
		{
			std::error_code ec;
			m_failed = !m_directory.is_absolute() || !std::filesystem::is_directory(m_directory, ec) || std::filesystem::is_symlink(m_directory, ec) || ec;
		}
		const manifest& identity() const
		{
			return m_identity;
		}
		u32 pbd_recorded_boundary(u32 boundary) const
		{
			return pbd_source_boundary(boundary);
		}
		u32 pbd_native_boundary(u32 source) const
		{
			if (m_identity.pbd_capsule.empty())
				return source;
			if (!m_pbd_native_anchor || source <= m_identity.pbd_source_origin || source > maximum_boundary ||
				source - m_identity.pbd_source_origin - 1 > maximum_boundary - *m_pbd_native_anchor)
				return ~0u;
			return *m_pbd_native_anchor + source - m_identity.pbd_source_origin - 1;
		}
		bool pbd_set_native_anchor(u32 boundary);
		bool healthy() const
		{
			return !m_failed && !m_stopped;
		}
		bool prepared() const
		{
			return m_prepared;
		}
		bool stopped() const
		{
			return m_stopped;
		}
		bool visible() const
		{
			return can_show() && !m_presentation.hidden && healthy();
		}
		bool game_active() const
		{
			return m_game && !m_ended;
		}
		const presentation& display() const
		{
			return m_presentation;
		}
		const std::string& reason() const
		{
			return m_reason;
		}
		u32 game() const
		{
			return m_game;
		}
		const std::optional<pbd::rank_stakes::game_values>& rank_screens() const
		{
			return m_rank_screens;
		}
		u32 origin() const
		{
			return m_origin;
		}
		u32 consumed() const
		{
			return m_consumed;
		}
		std::optional<u32> following_scene() const
		{
			return m_reserved && m_frames.size() > 1 ? std::optional<u32>{m_frames[1].scene} : std::nullopt;
		}
		bool refuse(std::string_view reason);
		// Ends watching without a fault: reported as stopped with its reason, the cover keeps its notice, and the owner ends the
		// session as a user stop. Returns false like refuse, so every caller's stop path runs.
		bool close(std::string_view reason);
		bool draw_closed() const
		{
			return m_draw_closed;
		}
		// The launcher's last watch_ui (the VS cards take its set length and origins), and how many pad events were written.
		const std::optional<spectator_ui::state>& screen() const
		{
			return m_ui;
		}
		u64 pad_events() const
		{
			return m_pad_serial;
		}
		// When the last waiting control for the game this watch waits for arrived; none since that game's begin_game or before one.
		std::optional<std::chrono::steady_clock::time_point> alive_at() const
		{
			return m_alive_at;
		}
		// Whether the watch is between games: its last game ended (or none began yet) and no input of the next one is held.
		bool between_games() const
		{
			return (!m_game || m_ended) && m_frames.empty() && !m_reserved;
		}
		// The watched game's phase at this boundary (pbd_spectator_ui::fight_on_screen): the watcher's screen shows only in a fight.
		void observe_fight(bool fight)
		{
			if (fight == m_fight)
				return;
			m_fight = fight;
			draw_ui();
		}
		// One read of the watcher's controller word (pbd_spectator_pad_word): its presses become pad event files for the launcher,
		// and the open chat's highlight follows its directions. Nothing here reaches the watched game's input. `menu`: the in-game
		// menu (Esc) holds the controller (pbd::settings_menu::claimed). Its Circle goes back or closes it, so while it holds the
		// controller no press becomes a pad event (a Circle there never also stops the watch), and the watcher's screen gives way
		// to it as Practice's HUD gives way to the pause menu: two control bars on the same line would draw over each other.
		void read_pad(std::uint16_t word, bool menu = false);
		// All arguments are verified native facts from this owner callback, not manifest assertions.
		bool prepare(u64 native_generation, u32 owner, u32 boundary, u32 counter, std::string_view checkpoint, std::string_view emulator);
		bool poll_control(const pbd::rematch::options* actual_options = nullptr);
		template <typename Hash>
		bool poll_input(Hash hash, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		std::optional<frame> reserve()
		{
			if (!healthy() || !m_prepared || !game_active() || m_frames.empty() || m_reserved)
				return {};
			m_reserved = true;
			return m_frames.front();
		}
		bool verify_state(u32 boundary, u32 segment, u32 scene, u32 counter, u32 mask, const std::array<u64, 3>& combat);
		// The game's applied picks at its first intro. A fixed set must show its own picks; an in-game set shows whatever this game
		// chose (within the native bounds), checked against the lobby's report when one came first. They are this game's binding.
		bool expose(u32 owner, u32 boundary, u32 stage, u32 left, u32 right, u32 rounds, bool first_intro, const native_round* actual_round = nullptr);
		template <typename Row>
		bool commit(u32 owner, u32 pc, u32 boundary, const Row& row, const std::array<std::uint16_t, 2>& expected);
		// A drawn game (both fighters at the rounds to win on the same final round) ends like any game, with "outcome":"draw". The
		// lobby replays it under its own number, which this watch does not follow: the cover says so, and the next begin_game (or
		// the launcher's stop) ends the watch without a fault.
		bool end(u32 boundary, bool native_end, bool drawn = false);
		bool awaiting_end() const
		{
			return healthy() && !m_ended && m_end && m_consumed == *m_end && !m_reserved;
		}
	};
} // namespace pbd::spectator
