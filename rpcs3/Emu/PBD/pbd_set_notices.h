#pragma once

// The Ranked and Quick Match notices drawn over a lobby set's own screens: which notice shows on which screen, for which side,
// with which words, and where on the 1280x720 canvas. The update latch composes one snapshot of the set at each fresh
// boundary (the scene and the fight's phase, both players' names, fighters and plates, the game and its stakes) and the RSX
// overlays draw it with the launcher's panel. Nothing here reads the pad, writes input or touches game state, so no notice
// can change what either game executes; every notice rides a frame the game already draws. Standard-library only, so the
// rules, the words and the layout are testable.

#include "pbd_game_overlay_model.h"
#include "pbd_rank_stakes.h"
#include "../Cell/pbd_game_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbd::set_notices
{
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using plate = pbd::game_overlay::plate;

	// Where the set came from (the launcher's vs_cards mode): a lobby room, Quick Match or Ranked.
	enum class set_mode : std::uint8_t
	{
		lobby,
		quick,
		ranked
	};

	// The plate a side shows: a placement plate while the launcher names that side's count in a Ranked set, else its rank. Quick
	// Match never shows a placement plate: it does not count toward placement, so the game's own plate stays. A lobby room's
	// players have no rank, so their cards show none.
	inline plate plate_for(set_mode mode, const std::optional<pbd::rank_stakes::placement>& placing, u32 rank) noexcept
	{
		if (mode == set_mode::lobby)
			return {};
		if (mode == set_mode::ranked && placing && placing->total)
			return {plate::kind::placing, 0, placing->done, placing->total};
		return {plate::kind::rank, pbd::game_profile::rank_byte(rank), 0, 0};
	}

	// The game's screen at a boundary, from its scene and its pending fight phase (pbd::match_recording::classify). The knockout
	// is the K.O. and its slow motion, the HUD still up; the round's result is the replay, the win pose and the reload after.
	enum class screen : std::uint8_t
	{
		other,
		vs,
		character_intro,
		round_intro,
		fight,
		knockout,
		round_result
	};

	// No side: the intro's own side is not known (a reload between rounds, a frame between the two intros).
	inline constexpr std::uint32_t no_side = 2;

	struct side
	{
		std::string name;      // the set manifest's display name
		std::string character; // the VS caption ("SERGEI DRAGUNOV"); empty when the launcher named none
		plate shown;
		bool operator==(const side&) const = default;
	};

	struct snapshot
	{
		u64 generation = 0; // the emulation that published it; another emulation draws nothing
		bool active = false;
		set_mode mode = set_mode::lobby;
		std::array<side, 2> sides{};
		u32 local = 0; // this PC's side: 0 left, 1 right
		u32 game_number = 0, first_to = 0;
		std::array<u32, 2> games{}; // the games each side has won before this game
		bool replay = false;        // a drawn game played again under its number
		// The set's stakes from the launcher (rank_stakes), for the stake words over the VS cards and between games.
		std::optional<pbd::rank_stakes::stakes> stakes;
		// The stake words the game itself shows in this game (its own "Ranking Match" pill): 0 none, 1 promotion, 2 demotion.
		std::array<u32, 2> game_stakes{};
		// Whether the fight draws each side's plate (pbd::game_profile::plate_drawn).
		std::array<bool, 2> plate_drawn{};
		// Whether the game has written a side's change word at this knockout (its plate flips to the new rank).
		std::array<bool, 2> promoted{};
		screen now = screen::other;
		// The game's first character intro: no round of this game has been fought yet.
		bool first_intro = false;
		// Whose character intro plays (0 left, 1 right, no_side between them).
		u32 intro_side = no_side;
		bool operator==(const snapshot&) const = default;
	};

	// What the latch knows when it composes: the manifest, the launcher's display commands and the game at this boundary.
	struct set_view
	{
		set_mode mode = set_mode::lobby;
		std::array<std::string, 2> names, characters;
		std::array<u32, 2> ranks{};
		std::array<std::optional<pbd::rank_stakes::placement>, 2> placements;
		u32 local = 0, game_number = 0, first_to = 0;
		std::array<u32, 2> games{};
		bool replay = false;
		std::optional<pbd::rank_stakes::stakes> stakes;
		std::array<u32, 2> game_stakes{};
		std::array<bool, 2> plate_drawn{};
		std::array<bool, 2> promoted{};
		screen now = screen::other;
		bool first_intro = false;
		u32 intro_side = no_side;
	};

	inline snapshot compose(const set_view& set)
	{
		snapshot out;
		out.active = true;
		out.mode = set.mode;
		for (u32 player = 0; player < 2; ++player)
			out.sides[player] = {set.names[player], set.characters[player], plate_for(set.mode, set.placements[player], set.ranks[player])};
		out.local = std::min<u32>(set.local, 1);
		out.game_number = set.game_number;
		out.first_to = set.first_to;
		out.games = set.games;
		out.replay = set.replay;
		out.stakes = set.mode == set_mode::ranked ? set.stakes : std::nullopt;
		out.game_stakes = set.game_stakes;
		out.plate_drawn = set.plate_drawn;
		out.promoted = set.promoted;
		out.now = set.now;
		out.first_intro = set.first_intro;
		out.intro_side = std::min(set.intro_side, no_side);
		return out;
	}

	// The scene and pending phase as a screen. Scene 10 is the VS screen; 8/9 and 11/12 are black loading scenes.
	// Scenes 16 and 17 are a fight, where
	// pending 0..3 is the character intro (or a later round's reload), 4 the round call, 5 the fight, 6..8 the knockout with
	// the HUD up (6 for a frame, then 8 for about two seconds of slow motion while the end-of-match bookkeeping runs), and
	// 9..12 and 37 the replay, the win pose and what follows, with the HUD down (the game shows PROMOTED TO in 9 and 10).
	inline constexpr screen screen_of(u32 scene, u32 pending) noexcept
	{
		if (scene == 10)
			return screen::vs;
		if (scene != 16 && scene != 17)
			return screen::other;
		if (pending <= 3)
			return screen::character_intro;
		if (pending == 4)
			return screen::round_intro;
		if (pending == 5)
			return screen::fight;
		if (pending >= 6 && pending <= 8)
			return screen::knockout;
		return (pending >= 9 && pending <= 12) || pending == 37 ? screen::round_result : screen::other;
	}

	// Whose character intro a battle scene's pending phase plays: the game's first intro runs pending 1 (the left fighter's,
	// about 150 frames from the battle scene's first frame) and then pending 2 (the right fighter's, about 150 to 180 frames)
	// before the round call (4), as every recorded VS boot shows; any other phase names no side.
	inline constexpr u32 intro_side_of(u32 scene, u32 pending) noexcept
	{
		if (scene != 16 && scene != 17)
			return no_side;
		return pending == 1 ? 0u : pending == 2 ? 1u :
		                                          no_side;
	}

	// The gold Placement Match pill: on a placing player's own character intro in a Ranked game, every game of the set, unless
	// the game draws its own "Ranking Match" pill there (either side's stake word is not 0). If both players are placing, each
	// intro shows the pill for its own player; never over the other player's intro.
	inline bool intro_pill(const snapshot& set) noexcept
	{
		return set.active && set.mode == set_mode::ranked && set.now == screen::character_intro && set.first_intro &&
		       set.intro_side < 2 && set.sides[set.intro_side].shown.shown == plate::kind::placing && !set.game_stakes[0] &&
		       !set.game_stakes[1];
	}

	// Our placement plate over the game's Beginner plate under a life bar: a Ranked side that is placing, while the game
	// draws its plate, in the round call, the fight and the knockout until the HUD goes down. It stops at the knockout where the
	// game writes that side's change word, where the game flips its own plate to the new rank (PROMOTED TO follows), and it
	// never carries lamps.
	inline bool life_plate(const snapshot& set, u32 player) noexcept
	{
		return player < 2 && set.active && set.mode == set_mode::ranked && set.sides[player].shown.shown == plate::kind::placing &&
		       set.plate_drawn[player] && !set.promoted[player] &&
		       (set.now == screen::round_intro || set.now == screen::fight || set.now == screen::knockout);
	}

	// The stake word over a side's card on the VS screen and under its plate between games: the game's own words,
	// "Promotion Chance" or "Demotion Risk", for the game the score leads to. None outside Ranked or before the game that can end
	// the set; a side with both shows its promotion chance, as the game does.
	enum class stake_word : std::uint8_t
	{
		none,
		promotion,
		demotion
	};
	inline stake_word stake_for(const snapshot& set, u32 player, const std::array<u32, 2>& games) noexcept
	{
		if (!set.stakes || player > 1)
			return stake_word::none;
		const auto stake = pbd::rank_stakes::at(*set.stakes, player, games, set.first_to);
		return stake.promotion ? stake_word::promotion : stake.demotion ? stake_word::demotion :
		                                                                  stake_word::none;
	}
	inline std::string_view stake_words(stake_word word) noexcept
	{
		return word == stake_word::promotion ? "Promotion Chance" : word == stake_word::demotion ? "Demotion Risk" :
		                                                                                           "";
	}

	// The notices' words.
	inline std::string upper(std::string_view text)
	{
		std::string out(text);
		for (auto& c : out)
			if (c >= 'a' && c <= 'z')
				c = static_cast<char>(c - 'a' + 'A');
		return out;
	}

	// The cover's tag over its headline: RANKED or QUICK MATCH; nothing in a lobby room.
	inline std::string_view mode_tag(set_mode mode) noexcept
	{
		return mode == set_mode::ranked ? "RANKED" : mode == set_mode::quick ? "QUICK MATCH" :
		                                                                       "";
	}
	inline std::string game_headline(u32 game_number)
	{
		return "GAME " + std::to_string(game_number);
	}
	inline std::string first_to_caption(u32 first_to)
	{
		return "FIRST TO " + std::to_string(first_to);
	}
	// The cover's caption at the bottom: FIRST TO n, or DRAW · PLAYED AGAIN for a drawn game's replay; GAME n · FIRST TO n when a
	// wait takes the headline's place.
	inline std::string cover_caption(const snapshot& set, bool waiting)
	{
		if (waiting)
			return game_headline(set.game_number) + " \xC2\xB7 " + first_to_caption(set.first_to);
		return set.replay ? std::string("DRAW \xC2\xB7 PLAYED AGAIN") : first_to_caption(set.first_to);
	}

	// A placement plate's words: "Placing 2/5".
	inline std::string placing_words(u32 done, u32 total)
	{
		return "Placing " + std::to_string(done) + "/" + std::to_string(total);
	}
	// What is left of placement under Set complete's big plate: "2 sets to go", "1 set to go", or "1 more set" for a character
	// at its total that is still not placed.
	inline std::string sets_to_go(u32 done, u32 total)
	{
		if (done >= total)
			return "1 more set";
		const u32 left = total - done;
		return std::to_string(left) + (left == 1 ? " set to go" : " sets to go");
	}

	inline bool opponent_result_shown(const pbd::game_overlay::snapshot& panel) noexcept
	{
		return panel.kind == pbd::game_overlay::panel_kind::post_set && (panel.you == 'L' || panel.you == 'R') &&
		       panel.opponent.shown.shown != plate::kind::none;
	}

	inline std::string opponent_caption(const pbd::game_overlay::opponent_result& result)
	{
		if (result.shown.shown == plate::kind::rank)
			return "MMR " + std::to_string(result.rating);
		if (result.shown.shown != plate::kind::placing)
			return {};
		const u32 left = result.shown.total > result.shown.done ? result.shown.total - result.shown.done : 1;
		return std::to_string(left) + (left == 1 ? " set to rank" : " sets to rank");
	}

	// Set complete's headline (one per state): RANK UP or RANK ATTAINED for a settled rank change, else the panel's own title
	// (YOU WIN, YOU LOSE, NO CONTEST, FORFEITED ...).
	inline std::string headline(const pbd::game_overlay::snapshot& panel)
	{
		using pbd::game_overlay::rank_change;
		if (panel.rank.kind == rank_change::up)
			return "RANK UP";
		if (panel.rank.kind == rank_change::attained)
			return "RANK ATTAINED";
		return upper(panel.title);
	}
	// The caption under the set's score: SET COMPLETE after the set, FIRST TO n between games.
	inline std::string score_caption(const pbd::game_overlay::snapshot& panel)
	{
		return panel.kind == pbd::game_overlay::panel_kind::post_set ? std::string("SET COMPLETE") : first_to_caption(panel.score.first_to);
	}
	// The other player's status at the opposite edge: rival=, or an older launcher's countdown detail.
	inline std::string_view rival_line(const pbd::game_overlay::snapshot& panel) noexcept
	{
		if (!panel.rival.empty())
			return panel.rival;
		return panel.timer.present ? std::string_view(panel.timer.detail) : std::string_view{};
	}
	// The row whose label carries the countdown's seconds: countdown_row=, or the first row with an older launcher's timer=.
	inline std::optional<u32> countdown_row(const pbd::game_overlay::snapshot& panel) noexcept
	{
		if (!panel.timer.present || !panel.row_count)
			return std::nullopt;
		return panel.countdown_row ? panel.countdown_row : std::optional<u32>(0);
	}
	// "MMR 1216", with the change after a promotion or a set that left the rank ("+13", "−14"); no change for the set that placed
	// the player, whose rating meant little until then.
	inline std::string rating_caption(const pbd::game_overlay::rank_result& rank)
	{
		return "MMR " + std::to_string(rank.rating);
	}
	inline std::string rating_change(const pbd::game_overlay::rank_result& rank)
	{
		using pbd::game_overlay::rank_change;
		if (rank.kind == rank_change::attained || rank.kind == rank_change::none)
			return {};
		const auto magnitude = static_cast<u32>(rank.change < 0 ? -static_cast<std::int64_t>(rank.change) : rank.change);
		return (rank.change < 0 ? "\xE2\x88\x92" : "+") + std::to_string(magnitude);
	}

	// A connection's 0..4 bars as the notices colour them: green at 4, amber at 2..3, red below.
	enum class signal : std::uint8_t
	{
		good,
		fair,
		poor
	};
	inline constexpr signal signal_of(u32 bars) noexcept
	{
		return bars >= 4 ? signal::good : bars >= 2 ? signal::fair :
		                                              signal::poor;
	}

	// The search line under the dummy's life bar while the launcher searches during Practice, after its turning icon and
	// SEARCHING: "Ranked · Placing " and the count (drawn in gold), or "Ranked", or "Quick Match".
	struct search_line
	{
		std::string mode, count;
		bool operator==(const search_line&) const = default;
	};
	inline search_line search_words(bool ranked, std::optional<std::pair<u32, u32>> placing)
	{
		if (!ranked)
			return {"Quick Match", {}};
		if (!placing)
			return {"Ranked", {}};
		return {"Ranked · Placing ", std::to_string(placing->first) + "/" + std::to_string(placing->second)};
	}

	// The connection on the in-game controls page of a two-PC set: the route and the frames live there, not over the fight.
	inline std::string connection_words(u32 delay, u32 rollback, bool relayed)
	{
		return "Connection: Delay " + std::to_string(delay) + "f \xC2\xB7 Rollback " + std::to_string(rollback) + "f \xC2\xB7 " +
		       (relayed ? "Relay" : "Direct");
	}

	// The leave chord's line while it is held: a Ranked set says the hold forfeits it; then "Leaving".
	inline std::string leave_words(set_mode mode, bool set_complete, bool done)
	{
		if (done)
			return "Leaving";
		if (mode == set_mode::ranked && !set_complete)
			return "{start}+{select} Hold to forfeit the set";
		return "{start}+{select} Hold to leave";
	}

	// Set complete's counter flip, on the frame the game already holds once the rating arrives: the next lamp lights with
	// the game's flash, the number flips, then what is left fades in. Milliseconds from the moment the rated panel first showed.
	inline constexpr u64 flip_lamp_ms = 250, flip_number_ms = 600, flip_settle_ms = 1000;
	struct flip_look
	{
		u32 lamps = 0;      // lamps lit
		bool flash = false; // the newest lamp carries the game's flash
		u32 count = 0;      // the number on the plate
		float to_go = 0.f;  // what is left, 0..1 opacity
	};
	inline flip_look flip_at(const pbd::game_overlay::placing& placement, u64 elapsed_ms) noexcept
	{
		if (placement.after <= placement.before)
			return {placement.before, false, placement.before, 0.f};
		flip_look out{placement.before, false, placement.before, 0.f};
		if (elapsed_ms >= flip_lamp_ms)
		{
			out.lamps = placement.after;
			out.flash = elapsed_ms < flip_settle_ms;
		}
		if (elapsed_ms >= flip_number_ms)
		{
			out.count = placement.after;
			out.to_go = std::min(1.f, static_cast<float>(elapsed_ms - flip_number_ms) / static_cast<float>(flip_settle_ms - flip_number_ms));
		}
		return out;
	}
	// Which look a launcher panel takes: the loading cover; a question asked in place (FORFEIT SET?); Set complete with the big
	// placement plate, with a rank headline (RANK UP, RANK ATTAINED) and its plate on one line, with a quiet rank line (a set
	// that left the rank, or took it down) or with one line (NOT RATED ...); between games; a prompt with no rows.
	enum class panel_look : std::uint8_t
	{
		cover,
		question,
		placement,
		rank_headline,
		rank_line,
		result,
		between,
		prompt
	};
	inline panel_look look_of(const pbd::game_overlay::snapshot& panel) noexcept
	{
		using pbd::game_overlay::panel_kind;
		using pbd::game_overlay::rank_change;
		if (panel.selection_transition)
			return panel_look::cover;
		if (panel.question.present)
			return panel_look::question;
		if (panel.kind == panel_kind::post_set)
		{
			if (panel.rank.kind == rank_change::up || panel.rank.kind == rank_change::attained)
				return panel_look::rank_headline;
			if (panel.placement.present)
				return panel_look::placement;
			if (panel.rank.kind == rank_change::down || panel.rank.kind == rank_change::same)
				return panel_look::rank_line;
			return panel_look::result;
		}
		return panel.row_count ? panel_look::between : panel_look::prompt;
	}
	// How far the held game dims under each look: lightest between games, a little more for the
	// question asked in place and for Set complete's one line, deepest under a rank headline; a panel without the score (no HUD
	// over the game) dims more.
	inline constexpr float held_shade(panel_look look, bool scored) noexcept
	{
		if (look == panel_look::rank_headline)
			return .55f;
		if (!scored)
			return .45f;
		switch (look)
		{
		case panel_look::between: return .28f;
		case panel_look::question:
		case panel_look::result:
		case panel_look::rank_line: return .34f;
		default: return .3f;
		}
	}

	// Your own plate under your name band steps aside when Set complete shows your plate itself (the big plate, a rank headline
	// or a rank line), so it is never shown twice.
	inline bool own_plate_aside(panel_look look) noexcept
	{
		return look == panel_look::placement || look == panel_look::rank_headline || look == panel_look::rank_line;
	}
	// The prompts along the foot, only for keys that act now: Move with two or more open rows, then the panel's confirm and back.
	inline std::vector<std::pair<std::string, std::string>> prompts(const pbd::game_overlay::snapshot& panel)
	{
		std::vector<std::pair<std::string, std::string>> out;
		u32 open = 0;
		for (u32 i = 0; i < panel.row_count && i < pbd::game_overlay::max_rows; ++i)
			open += panel.rows[i].enabled ? 1 : 0;
		if (!open)
			return out;
		if (open >= 2)
			out.emplace_back("{dpad:ud}", "Move");
		if (!panel.confirm.empty())
			out.emplace_back("{cross}", panel.confirm);
		if (!panel.back.empty())
			out.emplace_back("{circle}", panel.back);
		return out;
	}

	// The flip plays once per panel command and count, however often the panel is republished.
	inline u64 flip_key(const pbd::game_overlay::snapshot& panel) noexcept
	{
		const auto& placement = panel.placement;
		if (!placement.present || placement.after <= placement.before)
			return 0;
		return (panel.command_serial << 16) | (u64{placement.before} << 8) | placement.after;
	}

	// Where each notice sits on the 1280x720 canvas, in canvas units. Text is
	// placed by its line's centre; an em is the face's size.
	namespace layout
	{
		inline constexpr float margin = 56.f, right_edge = 1280.f - margin;
		// The game's art at its own size: a rank plate (192 x 96 at 1080p) and the placement plate (1.28x as wide), and both
		// at Set complete's big size; the lamps at both sizes.
		inline constexpr float rank_w = 132.f, rank_h = 66.f, placing_w = 169.f, placing_h = 66.f;
		inline constexpr float big_rank_w = 176.f, big_rank_h = 88.f, big_placing_w = 225.f, big_placing_h = 88.f;
		inline constexpr float lamps_w = 232.f, lamps_h = 107.f, big_lamps_w = 290.f, big_lamps_h = 134.f;
		// A card on a cover: the fighter caption, the name and the plate under it.
		inline constexpr float card_caption_y = 555.f, card_name_y = 581.f, card_name_em = 28.f, card_plate_top = 589.f;
		// The plate's edge: 4 outside the card's edge.
		inline constexpr float card_plate_inset = 4.f;
		// The cover's middle: the tag, the headline and the caption at the foot.
		inline constexpr float cover_tag_y = 308.f, cover_headline_y = 357.f, cover_headline_em = 58.f, cover_caption_y = 627.f;
		// The VS screen's score, in the gap between the game's two W/L bars, and its caption below them; over a cover the stake
		// word 30 above the card.
		inline constexpr float vs_score_y = 588.f, vs_score_em = 50.f, vs_caption_y = 628.f, vs_stake_top = 518.f, stake_h = 22.f;
		// Over the game's own VS screen the stake word sits 6 above the game's name bar (its top at 438), at the plate's outer
		// edge, and clear of the GET READY banner (its bottom about 384).
		inline constexpr float vs_game_stake_top = 410.f;
		// The intro's pill: the game's 1000 x 400 pill texture 1:1 at 1080p, its body 855 x 201 centred at (960, 541).
		inline constexpr float pill_x = 306.67f, pill_y = 227.33f, pill_w = 666.67f, pill_h = 266.67f;
		// The life bar's plate: the game's plate box's top-left; the right side mirrors at the box's right edge.
		inline constexpr float life_plate_left = 52.f, life_plate_right = 1228.f, life_plate_top = 115.f;
		// Between games and Set complete: the name bands where the life bars were, the plates under them, the
		// stake word under a threatened plate, the score at the top, the headline and the rows on this player's side.
		inline constexpr float band_top = 84.f, band_h = 30.f, band_w = 400.f, hud_plate_top = 110.f, hud_stake_top = 178.f, hud_stake_edge = 62.f;
		inline constexpr float hud_score_y = 69.f, hud_score_em = 52.f, hud_caption_y = 108.f;
		inline constexpr float menu_x = 76.f, headline_em = 46.f, row_w = 380.f, row_h = 33.f;
		inline constexpr float between_headline_y = 275.f, between_rows_top = 318.f;
		inline constexpr float confirm_line_y = 318.f, confirm_rows_top = 342.f;
		inline constexpr float result_headline_y = 261.f, result_line_y = 306.f, result_rows_top = 332.f;
		inline constexpr float placing_plate_x = 66.f, placing_plate_top = 296.f, placing_lamps_x = 252.f, placing_lamps_top = 267.f;
		inline constexpr float placing_caption_y = 393.f, placing_rows_top = 420.f;
		inline constexpr float rank_line_y = 247.f, rank_em = 36.f, rank_caption_y = 291.f, rank_rows_top = 318.f;
		// The prompts and the rival's status along the foot.
		inline constexpr float prompt_y = 693.f;
		// The waits: the headline, the gold line with its clock, the line under it.
		inline constexpr float wait_headline_y = 332.f, wait_line_y = 378.f, wait_em = 44.f, wait_track_w = 260.f;
		inline constexpr float reconnect_headline_y = 342.f, reconnect_line_y = 388.f;
		inline constexpr float controller_headline_y = 324.f, controller_line_y = 368.f;
		// The connection line during a fight and the leave chord's line: bottom centre.
		inline constexpr float net_y = 699.f, leave_y = 690.f;
		// A NEW CHALLENGER: the game's word art, the challenger's card, the rows, the pill under them.
		inline constexpr float challenger_art_x = 226.5f, challenger_art_y = 150.f, challenger_art_w = 827.f, challenger_art_h = 112.f;
		inline constexpr float challenger_caption_y = 307.f, challenger_name_y = 333.f, challenger_plate_top = 343.f;
		inline constexpr float challenger_rows_x = 470.f, challenger_rows_w = 340.f, challenger_rows_top = 440.f;
		inline constexpr float challenger_pill_x = 490.f, challenger_pill_y = 500.f, challenger_pill_w = 300.f, challenger_pill_h = 120.f;
		// The search line, right-aligned under the dummy's life bar.
		inline constexpr float search_right = 1228.f, search_y = 135.f;
	} // namespace layout

	// The hand-off to the launcher: once the lobby ends the set (end_set), the picture fades to black over 0.3 s while the
	// launcher takes over and closes or parks the window. It adds no time: the session ends as it always did, and the fade is
	// cut short if the window goes first.
	inline constexpr u64 handoff_fade_ms = 300;
	inline std::atomic<u64> handoff_generation{0}, handoff_at_ms{0};
	inline void hand_off(u64 generation, u64 now_ms) noexcept
	{
		handoff_at_ms.store(now_ms, std::memory_order_relaxed);
		handoff_generation.store(generation, std::memory_order_release);
	}
	// The black over the picture, 0..1, `elapsed_ms` after the hand-off.
	inline constexpr float handoff_fade(u64 elapsed_ms) noexcept
	{
		return elapsed_ms >= handoff_fade_ms ? 1.f : static_cast<float>(elapsed_ms) / static_cast<float>(handoff_fade_ms);
	}
	// The black for this emulation at `now_ms` (the clock hand_off was given); 0 before any hand-off.
	inline float handoff_shade(u64 generation, u64 now_ms) noexcept
	{
		if (!generation || handoff_generation.load(std::memory_order_acquire) != generation)
			return 0.f;
		const u64 at = handoff_at_ms.load(std::memory_order_relaxed);
		return handoff_fade(now_ms > at ? now_ms - at : 0);
	}

	// One published snapshot for the renderer. The revision moves only when a drawn value changes.
	inline std::mutex published_mutex;
	inline snapshot published;
	inline std::atomic<u64> published_revision{0};

	inline void publish(u64 generation, snapshot value)
	{
		value.generation = generation;
		std::lock_guard lock(published_mutex);
		if (published == value)
			return;
		published = std::move(value);
		published_revision.fetch_add(1, std::memory_order_release);
	}
	inline u64 revision()
	{
		return published_revision.load(std::memory_order_acquire);
	}
	inline snapshot load()
	{
		std::lock_guard lock(published_mutex);
		return published;
	}
} // namespace pbd::set_notices
