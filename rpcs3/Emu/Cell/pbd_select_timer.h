#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

// Character and stage countdowns start at their first observed selection boundaries. Menu lockstep and segment-entry
// alignment give both peers the same origins; counting boundaries pauses both timers during a stall.
// Timeout and handicap confirmations enter only the local player's frozen input stream, so peers, replays and watchers
// execute the same presses. Buttons held on entry or suppressed by the timer stay withheld until release, preventing
// false edges when the game changes selection state.
namespace pbd::select_timer
{
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;

	inline constexpr u32 boundaries_per_second = 60;
	inline constexpr u32 max_seconds = 600;
	// The game's canonical 16-bit digital word.
	inline constexpr u16 cross = 0x0040;
	inline constexpr u16 directions = 0xf000; // Up 0x1000, Right 0x2000, Down 0x4000, Left 0x8000
	// The character state's costume buttons: Square, Triangle, Cross, Circle and the fifth costume's Start.
	inline constexpr u16 costume_buttons = 0x0080 | 0x0010 | 0x0040 | 0x0020 | 0x0800;
	// Six samples held, eighteen released give each confirmation a fresh edge.
	inline constexpr u32 press_samples = 6, cadence_samples = 24;
	// Cover the lockstep input delay (up to six boundaries, plus a missing entry sample) before admitting directions or
	// another confirmation, so delayed input cannot change the handicap or confirm the following state.
	inline constexpr u32 confirm_flight_samples = 12;
	inline constexpr u32 even_handicap = 100;
	// Vary the roulette stop delay within the configured stage duration rather than follow a fixed selection timing.
	inline constexpr u32 random_stop_span = 3 * boundaries_per_second;

	// The flow scene of the select, and the select record's state byte (+0x20) in VS mode.
	inline constexpr u32 scene_select = 3;
	inline constexpr u32 state_character = 3, state_handicap = 9, state_stage = 13;
	inline bool battle_scene(u32 scene)
	{
		return scene == 16 || scene == 17;
	}
	// Waiting for the other player, stage or load. Mask directions so none can arrive after a return to the handicap state.
	inline bool waiting_state(u32 state)
	{
		switch (state)
		{
		case 0:
		case 1:
		case 4:
		case 5:
		case 6:
		case 8:
		case 10:
		case 11:
		case 12:
		case 14:
		case 15:
		case 21:
		case 22: return true;
		default: return false;
		}
	}

	// Read both select records at each local sample so peers observe the same selection state.
	struct view
	{
		bool verified = false; // every field below was read
		bool vs_mode = false;  // VS mode 4, subscene 44, grid mode 1
		u32 scene = 0;
		std::array<u32, 2> state{};
		std::array<u32, 2> handicap{even_handicap, even_handicap};
		bool stage_open = false; // a record is in the stage state
		u32 stage_owner = 0;     // the shared work object's stage owner (a role) while one is
	};

	enum class phase : std::uint8_t
	{
		waiting,
		character,
		stage,
		finished
	};

	// What a countdown band shows. Display only: nothing here decides input.
	struct band
	{
		phase current = phase::waiting;
		u32 seconds = 0;                // whole seconds left in the current countdown
		u32 frames = 0, total = 0;      // boundaries left in it, and its full length, for a draining bar
		u32 stage_owner = 0;            // the role that picks the stage (phase::stage)
		std::array<bool, 2> choosing{}; // per role: still in the character or handicap state
		bool random_stage = false;      // this game's stage countdown starts at zero (Rematch with random stage)
		std::optional<u32> chooser;     // a repick: the only role that picks a fighter; the other keeps its fighter and the stage stays
	};

	class timer
	{
		u32 character_frames = 0, stage_frames = 0, role = 0;
		bool armed = false, done = false, sampled = false, random_stage_ = false;
		std::optional<u32> character_from, stage_from;
		// Origin of the local handicap confirmation cadence.
		std::optional<u32> handicap_from;
		// Timeout presses wait for the last handicap confirmation to reach the game.
		u32 confirmation_until = 0;
		u32 owner = 0, last_source = 0, flight_until = 0;
		// Bound the caller's random draw once the local role is known to own the stage.
		u32 stop_draw = 0, stop_span = 1, stop_delay = 0;
		// A repick (begin_repick): the role that picks again. Absent in every other selection.
		std::optional<u32> chooser_;
		u16 last_word = 0;
		u16 withheld = 0;
		const char* refusal_ = nullptr;

		static bool pressing(u32 source, u32 deadline)
		{
			return (source - deadline) % cadence_samples < press_samples;
		}
		static u32 remaining(u32 source, u32 from, u32 frames)
		{
			const u32 spent = source > from ? source - from : 0;
			return spent >= frames ? 0 : (frames - spent + boundaries_per_second - 1) / boundaries_per_second;
		}

		void observe_selection(u32 source, const view& now, u16 physical)
		{
			if (!character_from)
			{
				character_from = source;
				withheld = static_cast<u16>(withheld | physical);
			}
			if (now.stage_open && !stage_from)
			{
				stage_from = source;
				owner = now.stage_owner;
				if (random_stage_ && owner == role)
					stop_delay = stop_draw % stop_span;
			}
			// Refuse selection if either handicap changes from full health.
			for (u32 r = 0; r < 2; ++r)
				if (now.state[r] == state_handicap && now.handicap[r] != even_handicap)
					refusal_ = "selection_handicap_changed";
		}

		u16 filter_sample(u32 source, const view& now, u16 word, u16 previous, bool fresh)
		{
			const u16 physical = word;
			// Suppressed physical buttons must be released before they can produce another edge.
			const auto admit = [&](u16 out) -> u16
			{
				if (fresh)
					withheld = static_cast<u16>(withheld | (physical & ~out));
				return out;
			};
			if (!now.vs_mode)
				return word;
			// Neutral samples before character select cannot leak into its first boundaries.
			// Once a selection has started, later scenes pass the input through.
			if (now.scene != scene_select)
				return character_from ? word : admit(0);
			if (fresh)
				observe_selection(source, now, physical);
			if (refusal_)
				return static_cast<u16>(word & ~directions);
			const u32 own = now.state[role];
			// A repick's steered words (the kept player's, and the stage owner's in the stage state) are the driver's own.
			if (chooser_ && (*chooser_ != role || own == state_stage))
				return word;
			const u16 open = static_cast<u16>(physical & ~withheld);
			// Confirm full health without physical input, waiting for any earlier confirmation still in flight.
			if (own == state_handicap)
			{
				if (!handicap_from)
					handicap_from = source;
				const u32 window = source - (source - *handicap_from) % cadence_samples;
				const bool press = pressing(source, *handicap_from) && window >= flight_until;
				if (press)
					confirmation_until = window + confirm_flight_samples;
				return admit(press ? cross : u16{0});
			}
			if (fresh)
				handicap_from.reset();
			// Cross confirms the highlighted fighter or stage, or stops the stage roulette.
			const bool stage_state = own == state_stage, choosing = own == state_character;
			const bool stage_pick = stage_state && now.stage_owner == role;
			const auto deadline = stage_state ? (stage_pick ? stage_stop() : std::nullopt) : choosing ? character_deadline() :
			                                                                                            std::nullopt;
			const bool forced = deadline && source >= *deadline;
			// Keep physical input from stopping the random roulette or opening a stage list.
			const bool spinning = random_stage_ && stage_pick && !forced;
			// During a repick, a second press could reach stage select while the costume confirmation is in flight.
			const u16 late = chooser_ && source < flight_until ? static_cast<u16>(open & ~previous) : u16{0};
			// Only admitted costume presses extend the flight window; withheld input cannot postpone a timeout.
			if (fresh && !forced && !spinning && (stage_state || choosing) && (open & ~late & ~previous & costume_buttons))
				flight_until = source + confirm_flight_samples;
			const bool in_flight = source < flight_until;
			if (forced)
				return admit(!in_flight && source >= confirmation_until && pressing(source, *deadline) ? cross : u16{0});
			if (spinning)
				return admit(0);
			if (waiting_state(own))
				return admit(chooser_ ? u16{0} : static_cast<u16>(open & ~directions));
			if ((stage_state || choosing) && in_flight)
				return admit(static_cast<u16>(open & ~directions & ~late));
			return admit(open);
		}

	public:
		// Arm a new fighter and stage selection without changing sides. Random stage displays zero while the picker waits
		// random_stop_draw % min(random_stop_span, stage duration) boundaries, then confirms when earlier presses have cleared.
		// The caller draws host entropy once per game; the game's RNG is untouched.
		bool begin(u32 character_seconds, u32 stage_seconds, u32 local_role, bool random_stage = false, u32 random_stop_draw = 0)
		{
			if (!character_seconds || character_seconds > max_seconds || !stage_seconds || stage_seconds > max_seconds || local_role > 1)
				return false;
			*this = timer{};
			character_frames = character_seconds * boundaries_per_second;
			stage_frames = random_stage ? 0 : stage_seconds * boundaries_per_second;
			role = local_role;
			random_stage_ = random_stage;
			if (random_stage)
			{
				stop_draw = random_stop_draw;
				stop_span = std::min(random_stop_span, stage_seconds * boundaries_per_second);
			}
			armed = true;
			return true;
		}
		// Only the loser chooses a new fighter; pass the hidden-selection driver's input unchanged for the kept fighter and
		// stage. Run the chooser's character countdown and handicap confirmation, with no stage countdown.
		bool begin_repick(u32 character_seconds, u32 local_role, u32 chooser)
		{
			if (!character_seconds || character_seconds > max_seconds || local_role > 1 || chooser > 1)
				return false;
			*this = timer{};
			character_frames = character_seconds * boundaries_per_second;
			role = local_role;
			chooser_ = chooser;
			armed = true;
			return true;
		}
		bool random_stage() const
		{
			return random_stage_;
		}
		std::optional<u32> chooser() const
		{
			return chooser_;
		}
		// Armed and the battle has not started yet. Only then does the local sampling point need the native view.
		bool selecting() const
		{
			return armed && !done && !refusal_;
		}
		const char* refusal() const
		{
			return refusal_;
		}
		std::optional<u32> character_deadline() const
		{
			if (!character_from)
				return {};
			return *character_from + character_frames;
		}
		std::optional<u32> stage_deadline() const
		{
			if (!stage_from)
				return {};
			return *stage_from + stage_frames;
		}
		// The stage picker's first stop boundary includes the random roulette delay.
		std::optional<u32> stage_stop() const
		{
			if (!stage_from)
				return {};
			return *stage_from + stage_frames + stop_delay;
		}

		band status(u32 source, const view& now) const
		{
			band out;
			out.random_stage = random_stage_;
			out.chooser = chooser_;
			if (done)
			{
				out.current = phase::finished;
				return out;
			}
			if (!character_from)
				return out;
			const auto left_of = [&](u32 from, u32 frames)
			{
				const u32 spent = source > from ? source - from : 0;
				out.frames = spent >= frames ? 0 : frames - spent;
				out.total = frames;
			};
			if (stage_from)
			{
				out.current = phase::stage;
				out.seconds = remaining(source, *stage_from, stage_frames);
				out.stage_owner = owner;
				left_of(*stage_from, stage_frames);
			}
			else
			{
				out.current = phase::character;
				out.seconds = remaining(source, *character_from, character_frames);
				left_of(*character_from, character_frames);
			}
			for (u32 r = 0; r < 2; ++r)
				out.choosing[r] = now.verified && now.scene == scene_select && (now.state[r] == state_character || now.state[r] == state_handicap);
			return out;
		}

		// The caller freezes each source once; newer sources advance origins and held-button tracking.
		u16 sample(u32 source, const view& now, u16 word)
		{
			if (!selecting() || !now.verified)
				return word;
			// Stop filtering at battle entry, but ignore the previous game's battle while a continuation is armed.
			if (battle_scene(now.scene))
			{
				if (character_from)
					done = true;
				return word;
			}
			const bool fresh = !sampled || source > last_source;
			if (fresh)
			{
				sampled = true;
				last_source = source;
			}
			const u16 physical = word;
			const u16 previous = last_word;
			if (fresh)
			{
				last_word = physical;
				withheld = static_cast<u16>(withheld & physical);
			}
			return filter_sample(source, now, word, previous, fresh);
		}
	};

	// A plain Rematch clears the timer; recreate it when a later game returns to stock selection.
	inline bool arm_select(std::optional<timer>& clock, u32 character_seconds, u32 stage_seconds, u32 local_role, bool random_stage = false,
		u32 random_stop_draw = 0)
	{
		if (!clock)
			clock.emplace();
		return clock->begin(character_seconds, stage_seconds, local_role, random_stage, random_stop_draw);
	}
} // namespace pbd::select_timer
