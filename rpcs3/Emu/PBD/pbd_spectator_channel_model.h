#pragma once

#include "pbd_env.h"
#include "../Cell/pbd_set_channel.h"
#include "../Cell/pbd_corner_chat.h"
#include "../Cell/pbd_spectator_ui.h"
#include "pbd_t5dr_memory.h"
#include <array>
#include <chrono>
#include <deque>
#include <mutex>

// This channel is a local, bounded consumer. It has no network handle and never publishes fighter input.
// Receipt is separate from execution: only the ordinary two-port post callback can acknowledge a chunk.
namespace pbd::spectator
{
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using digest = std::array<std::uint8_t, 32>;
	using value = pbd::set_channel::json_value;
	inline constexpr std::size_t header_bytes = 96, frame_bytes = 64, packet_frames = 120;
	inline constexpr std::size_t applied_frame_bytes = 80;
	inline constexpr std::size_t maximum_packet_bytes = header_bytes + packet_frames * applied_frame_bytes;
	inline constexpr u32 maximum_frames = 65536;
	inline constexpr u32 maximum_boundary = 0xfffffffeu;

	struct native_round
	{
		u32 pending_phase = 4, ordinal = 0;
		std::array<std::uint16_t, 2> random{};
		bool operator==(const native_round&) const = default;
		value json() const
		{
			return value::object({{"encoding", value::string("pbd-native-round-start-v1")}, {"pendingPhase", value::number(pending_phase)},
				{"roundOrdinal", value::number(ordinal)}, {"randomWords", value::array({value::number(random[0]), value::number(random[1])})}});
		}
	};
	inline std::optional<native_round> read_native_round(const value& item)
	{
		using namespace pbd::set_channel;
		native_round out;
		u32 left = 0, right = 0;
		const auto* words = item.find("randomWords");
		if (!pbd::set_channel::detail::is(item.find("encoding"), "pbd-native-round-start-v1") ||
			!pbd::set_channel::detail::number32(item.find("pendingPhase"), 4, 4, out.pending_phase) ||
			!pbd::set_channel::detail::number32(item.find("roundOrdinal"), 0, 8, out.ordinal) ||
			!words || words->kind != json_kind::array || words->items.size() != 2 ||
			!pbd::set_channel::detail::number32(&words->items[0], 0, 65535, left) ||
			!pbd::set_channel::detail::number32(&words->items[1], 0, 65535, right))
			return {};
		out.random = {static_cast<std::uint16_t>(left), static_cast<std::uint16_t>(right)};
		return out;
	}

	inline bool unhex(std::string_view text, digest& out)
	{
		if (text.size() != 64)
			return false;
		const auto digit = [](char c)
		{
			return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
			                                                               -1;
		};
		for (std::size_t i = 0; i < out.size(); ++i)
		{
			const auto a = digit(text[2 * i]), b = digit(text[2 * i + 1]);
			if (a < 0 || b < 0)
				return false;
			out[i] = static_cast<std::uint8_t>((a << 4) | b);
		}
		return std::any_of(out.begin(), out.end(), [](auto b)
			{
				return b != 0;
			});
	}
	inline std::string hex(const digest& bytes)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string result;
		result.reserve(64);
		for (const auto byte : bytes)
		{
			result += digits[byte >> 4];
			result += digits[byte & 15];
		}
		return result;
	}
	inline u32 le32(const std::uint8_t* b)
	{
		return u32{b[0]} | (u32{b[1]} << 8) | (u32{b[2]} << 16) | (u32{b[3]} << 24);
	}
	inline u64 le64(const std::uint8_t* b)
	{
		return le32(b) | (u64{le32(b + 4)} << 32);
	}
	inline std::optional<std::filesystem::path> requested_directory()
	{
#ifdef _WIN32
		const wchar_t* text = pbd::env::find_wide("PBD_SPECTATOR_CHANNEL");
#else
		const char* text = pbd::env::find("PBD_SPECTATOR_CHANNEL");
#endif
		if (!text || !*text)
			return {};
		return std::filesystem::path(text);
	}
	// The fighters' rows and the stage one game was played with, as the game applied them.
	struct picks
	{
		u32 stage = 0, left = 0, right = 0;
		bool operator==(const picks&) const = default;
	};
	inline constexpr u32 maximum_stage = pbd::set_channel::max_stage_id, maximum_row = pbd::set_channel::max_character_row;
	inline bool plausible(const picks& item)
	{
		return item.stage <= maximum_stage && item.left <= maximum_row && item.right <= maximum_row;
	}
	// Whether a game's picks keep what its Rematch load froze: all of them, except the row of a repick's chooser.
	inline bool keeps(const pbd::rematch::load& load, const picks& item)
	{
		return pbd::rematch::matches(load, {{item.left, item.right}, item.stage});
	}
	// What a drawn game shows before watching ends: the lobby replays it under its own number, and a watcher follows from the
	// next game instead.
	inline constexpr std::string_view draw_notice = "This game is replayed after a draw. Watching resumes with the next game.";

	struct manifest
	{
		u64 generation = 0;
		// A fixed set binds one stage and two rows for every game. An in-game set (the players choose in the game's own select)
		// binds none: each game's picks come from the game being replayed, read at its first intro.
		u32 seed = 0, stage = 0, left = 0, right = 0, rounds = 0;
		bool in_game = false;
		std::string build, compat, title, emulator, checkpoint, recipe;
		digest binding{};
		bool applied = false, policy_present = false, start_skip = true, exit_gate = true, phase_delay = false, keyed_draws = false;
		bool ban_jinpachi = false, direct_start = false;
		u32 pbd_capsule_game = 0, pbd_source_game = 0, pbd_first_frame = 0, pbd_source_origin = 0, pbd_source_segment = 0, pbd_game_origin = 0;
		std::string pbd_capsule;
		u32 battle_clock_version = 2, menu_hash_mask = 0;
		picks fixed() const
		{
			return {stage, left, right};
		}
	};
	// The three pick fields of a manifest or a control: all numbers (picks), all null or left out (none), anything else invalid.
	inline bool read_picks(const value& root, std::optional<picks>& out)
	{
		using namespace pbd::set_channel;
		const auto *stage = root.find("stageId"), *left = root.find("leftCharacterRow"), *right = root.find("rightCharacterRow");
		const auto absent = [](const value* item)
		{
			return !item || item->kind == json_kind::null;
		};
		if (absent(stage) && absent(left) && absent(right))
		{
			out.reset();
			return true;
		}
		picks found;
		if (!pbd::set_channel::detail::number32(stage, 0, maximum_stage, found.stage) || !pbd::set_channel::detail::number32(left, 0, maximum_row, found.left) ||
			!pbd::set_channel::detail::number32(right, 0, maximum_row, found.right))
			return false;
		out = found;
		return true;
	}
	inline std::string_view read_manifest_policy(const value& root, manifest& out)
	{
		using namespace pbd::set_channel;
		if (const auto* applied = root.find("appliedEncoding"))
		{
			if (!pbd::set_channel::detail::is(applied, "pbd-applied-pad-v1"))
				return "spectator_applied_encoding_invalid";
			out.applied = true;
		}
		if (const auto* policy = root.find("inputPolicy"))
		{
			const auto *skip = policy->find("sessionStartSkip"), *gate = policy->find("exitGate"), *delay = policy->find("phaseDelay"), *keyed = policy->find("keyedDraws");
			if (!skip || skip->kind != json_kind::boolean || !gate || gate->kind != json_kind::boolean || !delay || delay->kind != json_kind::boolean ||
				!keyed || keyed->kind != json_kind::boolean ||
				!pbd::set_channel::detail::number32(policy->find("battleClockVersion"), 1, 2, out.battle_clock_version) ||
				!pbd::set_channel::detail::number32(policy->find("menuHashMask"), 0, 7, out.menu_hash_mask))
				return "spectator_input_policy_invalid";
			out.policy_present = true;
			out.start_skip = skip->truth;
			out.exit_gate = gate->truth;
			out.phase_delay = delay->truth;
			out.keyed_draws = keyed->truth;
			if (const auto* ban = policy->find("banJinpachi"); ban && !pbd::set_channel::detail::boolean(ban, out.ban_jinpachi))
				return "spectator_input_policy_invalid";
			if (const auto* direct = policy->find("directStart"); direct && !pbd::set_channel::detail::boolean(direct, out.direct_start))
				return "spectator_input_policy_invalid";
			if (out.direct_start && out.in_game)
				return "spectator_direct_start_needs_fixed_picks";
		}
		if (out.applied && !out.policy_present)
			return "spectator_input_policy_missing";
		return {};
	}

	inline std::string_view read_manifest_capsule(const value& root, manifest& out)
	{
		using namespace pbd::set_channel;
		if (const auto* capsule = root.find("capsuleStart"))
		{
			const auto* enabled = pbd::env::find("PBD_SPECTATOR_NATIVE_2");
			const auto* payload = capsule->find("capsule");
			if (!enabled || std::string_view(enabled) != "1" || !out.applied || !payload || payload->kind != json_kind::object ||
				!pbd::set_channel::detail::number32(capsule->find("gameNumber"), 1, 19, out.pbd_capsule_game) ||
				!pbd::set_channel::detail::number32(capsule->find("sourceGame"), 1, 65535, out.pbd_source_game) ||
				!pbd::set_channel::detail::number32(capsule->find("firstFrame"), 0, 65535, out.pbd_first_frame) ||
				!pbd::set_channel::detail::number32(capsule->find("sourceBoundary"), 0, 0xfffffffeu, out.pbd_source_origin) ||
				!pbd::set_channel::detail::number32(capsule->find("gameOrigin"), 0, out.pbd_source_origin, out.pbd_game_origin) ||
				!pbd::set_channel::detail::number32(capsule->find("sourceSegment"), 0, 255, out.pbd_source_segment) ||
				out.pbd_first_frame > out.pbd_source_origin + 1 - out.pbd_game_origin)
				return "spectator_capsule_invalid";
			u32 seed = 0, segment = 0;
			const auto* host = payload->find("hostContext");
			const auto* selection = payload->find("selection");
			bool allow = true;
			if (!pbd::set_channel::detail::number32(payload->find("sessionSeed"), 1, 0xffffffffu, seed) || seed != out.seed ||
				!host || !pbd::set_channel::detail::number32(host->find("sourceSegment"), 0, 255, segment) || segment != out.pbd_source_segment ||
				!selection || (selection->find("allowJinpachi") && !pbd::set_channel::detail::boolean(selection->find("allowJinpachi"), allow)) ||
				allow == out.ban_jinpachi)
				return "spectator_capsule_identity_mismatch";
			out.pbd_capsule = pbd::set_channel::detail::serialize(*payload);
		}
		return {};
	}

	inline pbd::set_channel::result<manifest> read_manifest(const std::filesystem::path& directory)
	{
		using namespace pbd::set_channel;
		const auto text = pbd::set_channel::detail::read_text(directory / "spectator-session.json");
		if (!text.value)
			return {{}, text.error};
		const auto parsed = pbd::set_channel::detail::json_parser(*text.value, 16384, pbd::json::max_file_bytes, 65536).parse();
		if (!parsed.value)
			return {{}, parsed.error};
		const auto& root = *parsed.value;
		manifest out;
		const auto string = [&](const char* key, std::string& target, std::size_t maximum)
		{
			const auto* item = root.find(key);
			if (!item || item->kind != json_kind::string || item->text.empty() || item->text.size() > maximum)
				return false;
			target = item->text;
			return true;
		};
		const auto* generation = root.find("generation");
		if (!pbd::set_channel::detail::is(root.find("schema"), "pbd-spectator-session-1") || !generation || generation->kind != json_kind::string ||
			generation->text.empty() || generation->text.size() > 16)
			return {{}, "spectator_session_invalid"};
		const auto integer = std::from_chars(generation->text.data(), generation->text.data() + generation->text.size(), out.generation);
		digest ignored{};
		std::optional<picks> fixed;
		const auto* binding = root.find("bindingDigest");
		if (integer.ec != std::errc{} || integer.ptr != generation->text.data() + generation->text.size() || !out.generation || out.generation > max_safe_integer ||
			!binding || binding->kind != json_kind::string || !unhex(binding->text, out.binding) ||
			!string("build", out.build, 128) || !string("compat", out.compat, 128) || !string("titleId", out.title, 32) || out.title != pbd::t5dr::title_id ||
			!string("emulatorSha256", out.emulator, 64) || !unhex(out.emulator, ignored) ||
			!string("checkpointSha256", out.checkpoint, 64) || !unhex(out.checkpoint, ignored) ||
			!string("recipeSha256", out.recipe, 64) || !unhex(out.recipe, ignored) ||
			!pbd::set_channel::detail::number32(root.find("seed"), 1, 0xffffffffu, out.seed) ||
			!read_picks(root, fixed) ||
			!pbd::set_channel::detail::number32(root.find("roundsPerGame"), 3, 3, out.rounds) ||
			!pbd::set_channel::detail::is(root.find("syncEncoding"), "pbd-sync-facts-v1") || !pbd::set_channel::detail::is(root.find("syncDefinition"), "pbd-peer-sync-v3"))
			return {{}, "spectator_session_invalid"};
		if (fixed)
		{
			out.stage = fixed->stage;
			out.left = fixed->left;
			out.right = fixed->right;
		}
		else
			out.in_game = true;
		if (const auto error = read_manifest_policy(root, out); !error.empty())
			return {{}, std::string(error)};
		if (const auto error = read_manifest_capsule(root, out); !error.empty())
			return {{}, std::string(error)};
		return {std::move(out), {}};
	}
	inline bool keyed_policy_matches(const manifest& declared, bool actual)
	{
		return declared.policy_present && declared.keyed_draws == actual;
	}
	inline constexpr const char* keyed_policy_refusal = "This spectator uses a different game setting (keyed draws).";
	struct frame
	{
		std::array<std::uint16_t, 2> pads{};
		u32 boundary = 0, segment = 0, counter = 0, scene = 0, mask = 0, gate_hold = 0;
		std::array<u64, 3> combat{};
		std::array<std::uint16_t, 2> applied{};
		bool applied_verified = false;
	};
	struct presentation
	{
		struct input_run
		{
			std::uint16_t held = 0, applied = 0;
			u32 frames = 0;
		};
		static constexpr u32 history_runs = 8;
		// A saved replay can adopt the shared held game and retire its live-watching surface.
		bool active = true;
		bool hidden = true, catch_up = false, broadcast = false, analyze = false;
		u32 delay_frames = 180, left_score = 0, right_score = 0;
		u32 native_game = 0, native_through = 0, sync_mask = 0;
		std::array<std::array<input_run, history_runs>, 2> history{};
		std::array<u32, 2> history_count{};
		std::string left_name, right_name;
		// Set when watching ends without a fault (a drawn game): the cover says why instead of "preparing".
		std::string notice;
		// Between two games of the set (the launcher's waiting control): the cover says what the watch waits for.
		std::string waiting_title, waiting_text;
	};
	inline std::mutex surface_mutex;
	inline presentation surface;
	inline u64 surface_generation = 0;
	inline void publish_presentation(u64 generation, const presentation& item)
	{
		std::lock_guard lock(surface_mutex);
		surface = item;
		surface_generation = generation;
	}
	inline presentation load_presentation(u64 generation)
	{
		std::lock_guard lock(surface_mutex);
		return surface_generation == generation ? surface : presentation{};
	}

	// How long the owner waits for the next input with nothing arriving. A watch is legitimately idle between two games of a set and
	// through the next game's select and VS load (its inputs reach the watcher at that game's start, the select included), which
	// can take longer than this. So the launcher feeds the waiting control while the room says the set is between games or its
	// next game has not started on the stream yet; each one that names the game this watch waits for moves the deadline to its
	// arrival plus input_wait. A real stall still ends: nothing for input_wait without a waiting control (spectator_buffer_timeout),
	// nothing for input_wait after the last one (spectator_source_stalled), or no game for input_wait_limit however many came
	// (spectator_wait_limit).
	inline constexpr std::chrono::seconds input_wait{60};
	inline constexpr std::chrono::minutes input_wait_limit{10};
	enum class wait_outcome
	{
		waiting,
		buffer_timeout,
		source_stalled,
		wait_limit
	};
	inline wait_outcome input_wait_outcome(std::chrono::steady_clock::time_point from, std::optional<std::chrono::steady_clock::time_point> alive,
		std::chrono::steady_clock::time_point now)
	{
		if (now >= from + input_wait_limit)
			return wait_outcome::wait_limit;
		const bool fed = alive && *alive >= from;
		if (now >= (fed ? *alive : from) + input_wait)
			return fed ? wait_outcome::source_stalled : wait_outcome::buffer_timeout;
		return wait_outcome::waiting;
	}
	inline const char* wait_reason(wait_outcome outcome)
	{
		switch (outcome)
		{
		case wait_outcome::source_stalled: return "spectator_source_stalled";
		case wait_outcome::wait_limit: return "spectator_wait_limit";
		default: return "spectator_buffer_timeout";
		}
	}

} // namespace pbd::spectator
