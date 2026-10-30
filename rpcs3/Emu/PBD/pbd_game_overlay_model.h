#pragma once

// Host-owned in-game menus shared by Practice, replay and lobby flows. The launcher publishes one bounded command file; the
// emulation owner thread validates it, owns navigation, and publishes an immutable copy for the RSX overlay. No guest address or
// game state crosses this interface.

#include "pbd_env.h"
#include "../Cell/pbd_game_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbd::game_overlay
{
	inline bool hold_live(std::uint64_t now_ns, std::uint64_t polled_ns) noexcept
	{
		return polled_ns && now_ns >= polled_ns && now_ns - polled_ns <= 1'000'000'000ull;
	}

	// Native Practice pages carry up to fifteen rows; launcher files keep their own eight-row PBD-MENU-1 limit.
	inline constexpr std::size_t max_rows = 15;
	inline constexpr std::size_t max_file_rows = 8;
	inline constexpr std::size_t max_file_bytes = 16384;
	// score=: a set is first to 1..10 games; a player name is drawn up to this many bytes.
	inline constexpr std::uint32_t max_score_first_to = 10;
	inline constexpr std::size_t max_score_name_bytes = 48;

	enum class panel_kind : std::uint8_t
	{
		menu,
		drawer,
		prompt,
		text,
		challenger,
		post_set,
		choice_rejected,
		set_ended
	};
	enum class glyph_family : std::uint8_t
	{
		playstation,
		xbox,
		keyboard
	};

	struct row
	{
		bool enabled = true;
		std::string label;
		// Practice: the value column. A lobby panel: the note right-aligned in the row.
		std::string detail;
		// The focused row's help line (with {glyph} tokens). Native Practice only: whether the value is still the default, which
		// the renderer shows in a different colour.
		std::string help;
		bool is_default = false;
		// Native Practice only: a filled dummy slot's frequency weight (0..10; none when the row has none) and extra space
		// above the row in canvas units, so a group of related settings sits apart from the rows before it.
		std::optional<std::uint32_t> frequency;
		std::uint32_t gap = 0;
	};

	// The set's score under a between-games or set-over panel (PBD-MENU-1 score=, optional): one row per player, each with one dot
	// per decided game in order, filled where that player won it. A drawn game is replayed and never counts, so it has no dot.
	struct set_score
	{
		bool present = false;
		std::uint32_t first_to = 0;
		std::string games;       // one letter per decided game, in order: 'L' the left player won it, 'R' the right player
		std::string left, right; // the two players' names as shown
		std::uint32_t wins(bool right_side) const
		{
			return static_cast<std::uint32_t>(std::count(games.begin(), games.end(), right_side ? 'R' : 'L'));
		}
		bool operator==(const set_score&) const = default;
	};

	// Lobby panel keys' limits (PBD-MENU-1), in bytes of UTF-8 unless named otherwise.
	inline constexpr std::size_t max_help_bytes = 160, max_fighter_bytes = 32, max_status_bytes = 160;
	inline constexpr std::size_t max_timer_title_bytes = 40, max_timer_detail_bytes = 64;
	inline constexpr std::uint32_t max_timer_total_ms = 600000;
	inline constexpr std::size_t max_dialog_message_bytes = 80, max_dialog_detail_bytes = 200;
	inline constexpr std::size_t max_base_rows = 8, max_game_lines = 24, max_stage_bytes = 40, max_next_bytes = 48;

	// The band over a lobby panel (timer=): it drains toward an absolute deadline on this PC's wall clock (Unix ms, the clock the
	// launcher compares with the lobby's deadline), so the panel is not republished every second.
	struct countdown
	{
		bool present = false;
		std::uint64_t deadline_ms = 0;
		std::uint32_t total_ms = 0;
		std::string title, detail;
		bool ready = false; // the other player has answered
		bool operator==(const countdown&) const = default;
	};

	struct countdown_view
	{
		std::uint32_t seconds = 0; // whole seconds left, rounded up, at most 99
		float share = 0.f;         // the part of the countdown left, 0..1
	};

	inline countdown_view countdown_at(const countdown& value, std::uint64_t now_ms) noexcept
	{
		const std::uint64_t remaining = value.deadline_ms > now_ms ? value.deadline_ms - now_ms : 0;
		const std::uint64_t seconds = std::min<std::uint64_t>(99, (remaining + 999) / 1000);
		const float share = value.total_ms ? std::clamp(static_cast<float>(remaining) / static_cast<float>(value.total_ms), 0.f, 1.f) : 0.f;
		return {static_cast<std::uint32_t>(seconds), share};
	}

	// One game of the set in play order (game=), for Set complete's list: 'L' or 'R' won it, 'D' was a draw played again.
	struct game_line
	{
		std::uint32_t number = 0;
		char winner = 'D';
		std::uint32_t winner_rounds = 0, loser_rounds = 0;
		std::string stage;
		bool operator==(const game_line&) const = default;
	};

	// This player's rank once the lobby has rated the set (rank=, on Set complete): up for a promotion, down for a demotion,
	// attained for the set that placed them, same for a rated set that left the rank where it was, with the game's rank index
	// (Beginner 0 to Dark Lord 34), the rating and its change.
	enum class rank_change : std::uint8_t
	{
		none,
		up,
		down,
		attained,
		same
	};
	inline constexpr std::uint32_t max_rank_index = 34, max_rating = 99999;
	inline constexpr std::int32_t max_rating_change = 9999;
	struct rank_result
	{
		rank_change kind = rank_change::none;
		std::uint32_t rank = 0, rating = 0;
		std::int32_t change = 0;
		bool operator==(const rank_result&) const = default;
	};

	// A row of the panel under a dialog (base=): drawn as it was, never focused.
	struct base_row
	{
		bool enabled = true;
		std::string label, note;
		bool operator==(const base_row&) const = default;
	};

	// A question stacked over the panel (dialog=): its rows are the answers.
	struct dialog
	{
		bool present = false;
		std::string message, detail;
		bool operator==(const dialog&) const = default;
	};

	// Set complete's placement count (placing=): the rated sets this player had before the set, after it and the character's
	// total. `after` exceeds `before` by one once the lobby has rated the set, which plays the counter's flip once.
	inline constexpr std::uint32_t max_placement_total = 10;
	struct placing
	{
		bool present = false;
		std::uint32_t before = 0, after = 0, total = 0;
		bool operator==(const placing&) const = default;
	};

	// The set's connection under Set complete (link=): 0..4 signal bars and the words beside them ("Smooth · 38 ms").
	inline constexpr std::uint32_t max_bars = 4;
	inline constexpr std::size_t max_rival_bytes = 80, max_link_words_bytes = 48, max_card_character_bytes = 24, max_card_name_bytes = 48;
	inline constexpr std::uint32_t max_ping_ms = 9999;
	struct link_line
	{
		bool present = false;
		std::uint32_t bars = 0;
		std::string words;
		bool operator==(const link_line&) const = default;
	};

	// A plate beside a name: none, a rank plate (the game's rank index) or a placement plate (sets done of the total).
	struct plate
	{
		enum class kind : std::uint8_t
		{
			none,
			rank,
			placing
		};
		kind shown = kind::none;
		std::uint32_t rank = 0, done = 0, total = 0;
		bool operator==(const plate&) const = default;
	};

	// The opponent's settled standing on Set complete (opponent=), independent of this player's rank change.
	struct opponent_result
	{
		plate shown;
		std::uint32_t rating = 0;
		bool operator==(const opponent_result&) const = default;
	};

	// A challenger's card (card=): the fighter's name as the caption, the player's name and the plate.
	struct challenger_card
	{
		bool present = false;
		std::string character, name;
		plate shown;
		bool operator==(const challenger_card&) const = default;
	};

	// A challenger's connection (ping=): 1..4 bars and the round trip in milliseconds.
	struct ping_line
	{
		bool present = false;
		std::uint32_t bars = 0, ms = 0;
		bool operator==(const ping_line&) const = default;
	};

	struct snapshot
	{
		std::uint64_t generation = 0;
		// Lobby command whose visible result this snapshot applies; independent of presentation text.
		std::uint64_t command_serial = 0;
		bool visible = false;
		bool close_on_action = true;
		// Only the native selector may set this: its opaque card conceals stock selection without parking owner updates.
		// PBD-MENU-1 deliberately has no key that can grant this behavior to a launcher panel.
		bool selection_transition = false;
		panel_kind kind = panel_kind::menu;
		std::uint32_t selected = 0;
		std::uint32_t row_count = 0;
		std::string title;
		std::string subtitle;
		std::string confirm = "Choose";
		std::string back = "Back";
		std::array<row, max_rows> rows{};
		set_score score;
		// Lobby panels: each side's character under its name and which side this PC plays ('L', 'R' or 0); the countdown band;
		// the row this player already chose (every row is disabled meanwhile) and a status line; a dialog stacked over the panel,
		// whose rows are its answers, with the panel's own rows under it; the set's games and where each player goes next.
		std::array<std::string, 2> fighters{};
		char you = 0;
		countdown timer;
		std::optional<std::uint32_t> chosen;
		std::string status;
		dialog question;
		std::array<base_row, max_base_rows> base{};
		std::uint32_t base_count = 0;
		std::array<game_line, max_game_lines> games{};
		std::uint32_t game_count = 0;
		std::array<std::string, 2> next{};
		// The settled rank change.
		rank_result rank;
		opponent_result opponent;
		// Ranked notices: Set complete's placement count, the other player's status (drawn at the opposite edge with that
		// side's dot, or under a challenger's card), the set's connection, the row whose label carries the countdown's seconds,
		// and a challenger's card, connection and Placement Match pill.
		placing placement;
		std::string rival;
		link_line link;
		std::optional<std::uint32_t> countdown_row;
		challenger_card card;
		ping_line ping;
		bool placement_pill = false;
		// Practice's own pause menu (published by the latch, never parsed from a launcher file): drawn only in Practice's layout.
		bool practice = false;
	};

	// The layout the in-game menu overlay draws for the published panel. Practice's panel is drawn by the Practice menu renderer
	// only while the Practice view it was built for is current (practice_menu: the view shows its menu over DR's pause, with this
	// panel's generation). It never falls back to the lobby layout: a panel left from a pause the game has already left, or one
	// published a moment before its view, draws nothing.
	enum class menu_draw
	{
		hidden,
		lobby,
		practice
	};
	inline menu_draw choose_draw(const snapshot& panel, bool practice_menu, std::uint64_t practice_generation) noexcept
	{
		if (!panel.visible)
			return menu_draw::hidden;
		if (panel.practice)
			return practice_menu && practice_generation == panel.generation ? menu_draw::practice : menu_draw::hidden;
		return panel.generation ? menu_draw::lobby : menu_draw::hidden;
	}

	struct parse_result
	{
		std::optional<snapshot> value;
		std::string error;
	};

	// The loading cover over the game while the lobby's selection or direct load runs. It draws the set's own words (the game
	// and both players' cards, pbd::set_notices); a caller with words of its own (Practice's start) sets the title and line.
	inline snapshot selection_card(snapshot base)
	{
		base.generation = base.generation ? base.generation : 1;
		base.visible = true;
		base.selection_transition = true;
		base.close_on_action = false;
		base.kind = panel_kind::prompt;
		base.row_count = 0;
		base.selected = 0;
		base.title.clear();
		base.subtitle.clear();
		// The card replaces a between-games panel: everything else drawn on that panel belongs to it.
		base.score = {};
		base.fighters = {};
		base.you = 0;
		base.timer = {};
		base.chosen.reset();
		base.status.clear();
		base.question = {};
		base.base_count = 0;
		base.game_count = 0;
		base.next = {};
		base.rank = {};
		base.placement = {};
		base.rival.clear();
		base.link = {};
		base.countdown_row.reset();
		base.card = {};
		base.ping = {};
		base.placement_pill = false;
		return base;
	}

	// This PC's controller while a panel holds it (the claim). A panel shown, or one closed by the press that answered it (or, with a
	// peer, hidden after it: takes_controller), keeps the controller until every button is released, so that press never also
	// reaches the game.
	//
	// Where a second PC plays, the claim is applied once, to this PC's own sample at its admission point, after the leave chord has
	// seen the physical buttons and before the sample is frozen, queued, sent and recorded (admit_local_sample); the owned port then
	// executes exactly that frozen word (blanks_executed_port). The end of a hold that the host read sees is confirmed on those
	// samples before a button it saw may pass (admit_local_sample). Blanking only the executed port after the sample was sent
	// lets the other game run the held press (a fighter picked, a cursor moved or a first battle frame played on one PC only), and
	// every resend of that tick, which carries the executed row, contradicts the value sent first.
	//
	// A hidden selection that produces this PC's input owns the sample while it runs (the player's buttons never reach the game
	// then), so the claim is off and never blanks the selector's own presses. The release is still watched during it: a button
	// let go in the selection ends the hold, and one held through the whole selection keeps the next samples neutral, on both PCs,
	// until it is released.
	struct claim_read
	{
		bool ignore_until_release = false; // the press that took the controller is still held
		bool claimed = false;              // the game does not see this PC's controller
		bool navigate = false;             // this read may move or answer the visible panel
	};

	// One read of this PC's controller word `pad` by the shared panel (at a fresh boundary, and on every poll while a panel holds
	// one). The read that finds every button up ends the hold but navigates nothing, so the next press is a fresh edge.
	inline claim_read read_claim(bool ignore_until_release, std::uint16_t pad, bool visible, bool selector_owns) noexcept
	{
		const bool held = ignore_until_release && pad != 0;
		const bool panel = visible && !selector_owns;
		return {held, !selector_owns && (visible || held), panel && !ignore_until_release};
	}

	// Whether a newly read panel takes this PC's controller until every button is released. Every panel shown does. Where a second
	// PC plays, so does the hidden panel that replaces a shown one: a panel that does not close itself on a press (close=0, as every
	// launcher panel is) is answered by the launcher replacing or hiding it, and the next game's command can follow a choice before
	// any waiting card is drawn. The press that chose is then often still down when the hidden panel arrives, and held into the
	// game it would be a fresh press there (on both PCs, since it is in the frozen sample). The read after it ends the hold at once
	// if nothing is down. Practice keeps its rule (only a shown panel takes the controller).
	inline bool takes_controller(bool was_visible, bool visible, bool peer) noexcept
	{
		return visible || (peer && was_visible);
	}

	// The buttons the claim has seen down on this PC's controller (`buttons`), after one read of `pad`: every button down while the
	// controller is claimed, or while the press that took it is still held, joins them. They leave only through this PC's samples
	// (admit_local_sample).
	inline std::uint16_t hold_buttons(std::uint16_t buttons, std::uint16_t pad, const claim_read& read) noexcept
	{
		return read.claimed || read.ignore_until_release ? static_cast<std::uint16_t>(buttons | pad) : buttons;
	}

	struct input_result
	{
		std::uint16_t word = 0;    // the word frozen for this PC's own port
		std::uint16_t buttons = 0; // the claim's buttons no sample has shown released yet
	};

	// The word this PC admits for its own port where a second PC executes it: `sample` is the value after the leave chord (or the
	// selector's word), `physical` the controller word it was read from. Neutral while claimed, and the selector's own word while a
	// hidden selection owns the sample.
	//
	// The host read that ends a hold is newer than the sample admitted after it, which is the game's own, older read of the
	// controller (up to a frame with the entry sample). A press let go just before that host read can still be down in the sample,
	// and admitted as it is it would reach both games as one frame of a fresh press (a fighter picked in the select, an attack at
	// the first battle frames). So once the hold is over (`holding` false: a host read has seen every button up), each button the
	// claim saw down stays out of the samples until a sample shows it up; any other button passes at once. This decision reads
	// only the admitted samples, in order, at their single admission, so a re-execution never recomputes it.
	inline input_result admit_local_sample(std::uint16_t sample, std::uint16_t physical, bool claimed, bool holding, bool selector_owns,
		std::uint16_t buttons) noexcept
	{
		if (!claimed && !holding)
			buttons = static_cast<std::uint16_t>(buttons & physical);
		if (selector_owns)
			return {sample, buttons};
		if (claimed)
			return {0, buttons};
		return {static_cast<std::uint16_t>(sample & ~buttons), buttons};
	}

	// The same, with the in-game Settings menu holding this controller too (`menu`: pbd::settings_menu::claimed, open or the
	// press that closed it not yet released). While it holds, the sample is neutral and every button the sample shows down joins
	// the claim's buttons, so a press made in the menu (a Button Layout row's button among them) reaches neither game, then or
	// after the menu lets go.
	inline input_result admit_local_sample(std::uint16_t sample, std::uint16_t physical, bool claimed, bool holding, bool selector_owns,
		std::uint16_t buttons, bool menu) noexcept
	{
		if (menu)
			buttons = static_cast<std::uint16_t>(buttons | physical);
		return admit_local_sample(sample, physical, claimed || menu, holding, selector_owns, buttons);
	}

	// Republishing the same rows keeps the highlight and current button edges.
	// A different panel takes the controller until every button is released.
	inline bool continues(const snapshot& shown, const snapshot& next) noexcept
	{
		if (!shown.visible || !next.visible || shown.selection_transition || next.selection_transition || shown.kind != next.kind ||
			shown.row_count != next.row_count || shown.row_count > max_rows)
			return false;
		for (std::uint32_t i = 0; i < shown.row_count; i++)
			if (shown.rows[i].label != next.rows[i].label || shown.rows[i].enabled != next.rows[i].enabled)
				return false;
		return true;
	}

	// The highlight of a newly read panel: the player's (shown.selected) for a continuing panel whose file repeats the previous
	// file's selected= (shown_file_selected), otherwise the file's own.
	inline std::uint32_t republished_highlight(const snapshot& shown, std::uint32_t shown_file_selected, const snapshot& next) noexcept
	{
		if (continues(shown, next) && next.selected == shown_file_selected && shown.selected < next.row_count && next.rows[shown.selected].enabled)
			return shown.selected;
		return next.selected;
	}

	// Whether the claim blanks the ports at the port hook. Only where no other PC executes this PC's input (`local_only`, Practice);
	// with a peer the claim is already in the frozen sample, and the owned port executes that word unchanged.
	inline bool blanks_executed_port(bool claimed, bool peer, bool local_only) noexcept
	{
		return claimed && local_only && !peer;
	}

} // namespace pbd::game_overlay
