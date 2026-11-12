#pragma once

#include "pbd_practice_lesson.h"
#include "pbd_practice_store.h"

#include <cstdlib>

namespace pbd::practice::lesson
{
	struct command
	{
		std::string id, generation, kind;
		pbd_lesson_setup setup;
		std::uint32_t port = 0;
		std::vector<std::uint16_t> words;
	};
	inline json::result<command> parse(std::string_view text)
	{
		auto document = json::detail::json_parser(text, 4096, 16384, 8192).parse();
		if (!document.value)
			return {{}, document.error};
		const auto& value = *document.value;
		using namespace json::detail;
		command out;
		if (!is(value.find("schema"), "pbd-practice-lesson-command-1") || !identifier(value.find("id")) ||
			!identifier(value.find("generation")) || !plain_text(value.find("kind"), 1, 16))
			return {{}, "invalid_command"};
		out.id = value.find("id")->text;
		out.generation = value.find("generation")->text;
		out.kind = value.find("kind")->text;
		if (out.kind == "cancel" || out.kind == "record_end")
			return {std::move(out), {}};
		if (out.kind != "prepare" && out.kind != "play" && out.kind != "reset" && out.kind != "record_start")
			return {{}, "invalid_command"};
		const auto* setup = value.find("setup");
		const auto* characters = setup ? setup->find("characters") : nullptr;
		if (!characters || characters->kind != json::json_kind::array || characters->items.size() != 2 ||
			!number32(&characters->items[0], 0, 34, out.setup.characters[0]) ||
			!number32(&characters->items[1], 0, 34, out.setup.characters[1]) ||
			!number32(setup->find("stage"), 0, 255, out.setup.stage))
			return {{}, "invalid_setup"};
		if (!is(setup->find("side"), "left") && !is(setup->find("side"), "right"))
			return {{}, "invalid_setup"};
		if (!is(setup->find("stance"), "standing") && !is(setup->find("stance"), "crouching"))
			return {{}, "invalid_setup"};
		if (!is(setup->find("hitCondition"), "normal") && !is(setup->find("hitCondition"), "counter"))
			return {{}, "invalid_setup"};
		out.setup.side = is(setup->find("side"), "right") ? pbd_lesson_side::right : pbd_lesson_side::left;
		out.setup.stance = is(setup->find("stance"), "crouching") ? pbd_lesson_stance::crouching : pbd_lesson_stance::standing;
		out.setup.hit_condition = is(setup->find("hitCondition"), "counter") ? pbd_lesson_hit_condition::counter : pbd_lesson_hit_condition::normal;
		if (!stage_rule::loadable(out.setup.stage))
			return {{}, "unsupported_stage"};
		if (out.kind == "play")
		{
			if (!number32(value.find("port"), 0, 1, out.port) || !plain_text(value.find("words"), 1, 4800))
				return {{}, "invalid_recording"};
			auto words = practice_store::decode_words(value.find("words")->text);
			if (!words || words->empty() || words->size() > 1800 ||
				std::any_of(words->begin(), words->end(), [](auto word)
					{
						return (word & 0x0900) != 0;
					}))
				return {{}, "invalid_recording"};
			if (std::none_of(words->begin(), words->end(), [](auto word)
					{
						return word != 0;
					}))
				return {{}, "empty_recording"};
			out.words = std::move(*words);
		}
		return {std::move(out), {}};
	}

	struct playback
	{
		std::vector<std::uint16_t> words;
		std::uint32_t port = 0, position = 0;
		bool pending = false;
		bool demonstration = true;
		bool active() const
		{
			return position < words.size();
		}
		void stop()
		{
			words.clear();
			position = 0;
			pending = false;
		}
		void observe(bool advanced)
		{
			if (pending && advanced && active())
				++position;
			pending = false;
		}
		// Both port hooks share one prepared frame. A paused or withheld frame consumes no recording word.
		std::uint16_t input(std::uint32_t target, bool allowed)
		{
			if (!active() || !allowed || target != port)
				return 0;
			pending = true;
			return words[position];
		}
		std::optional<std::uint16_t> override_for(std::uint32_t target, bool allowed)
		{
			if (!active() || (!demonstration && target != port))
				return {};
			return input(target, allowed);
		}
	};
	struct channel
	{
		std::filesystem::path root;
		std::string generation, seen, ack, outcome, published, rejected, rejection;
		std::optional<command> preparing;
		playback play;
		struct input_capture
		{
			std::vector<std::uint16_t> words;
			pbd_lesson_setup setup;
			std::uint32_t port = 0;
			bool active = false, started = false, limit_reached = false, valid = true;
			void begin(const pbd_lesson_setup& requested)
			{
				*this = {};
				setup = requested;
				port = pbd_lesson_play_port(setup.side, true);
				active = started = true;
			}
			void check_setup(pbd_lesson_game observed)
			{
				// Pausing adds no inputs; changing the situation invalidates the whole take, including a full one.
				observed.paused = false;
				if (started && pbd_admit_lesson(setup, observed) != pbd_lesson_check::ready)
					valid = active = false;
			}
			void observe(bool advanced, std::uint16_t word, std::optional<pbd::practice::movement::screen_side> side)
			{
				if (!active || !advanced)
					return;
				if (!side)
				{
					valid = active = false;
					return;
				}
				words.push_back(pbd::practice::movement::record_word(static_cast<std::uint16_t>(word & ~0x0900u), *side));
				if (words.size() == 1800)
				{
					active = false;
					limit_reached = true;
				}
			}
		} capture;
		std::uint32_t reset = 0;
		std::chrono::steady_clock::time_point deadline{}, next_poll{}, next_publish{};
		bool permitted = false;
		void end_capture(const std::string& id);
		void refuse(std::string id, std::string reason)
		{
			rejected = std::move(id);
			rejection = std::move(reason);
		}
		void finish(std::string result)
		{
			outcome = std::move(result);
			preparing.reset();
			reset = 0;
			play.stop();
		}
	};
	inline std::string capture_document(const channel::input_capture& capture, const std::string& id, const std::string& generation)
	{
		using json::json_value;
		const auto& setup = capture.setup;
		return json::detail::serialize(json_value::object({{"schema", json_value::string("pbd-practice-lesson-capture-1")}, {"requestId", json_value::string(id)},
				   {"generation", json_value::string(generation)}, {"character", json_value::number(setup.characters[0])},
				   {"frames", json_value::number(capture.words.size())}, {"limitReached", json_value::boolean(capture.limit_reached)},
				   {"words", json_value::string(practice_store::encode_words(capture.words))},
				   {"setup", json_value::object({{"characters", json_value::array({json_value::number(setup.characters[0]), json_value::number(setup.characters[1])})},
								 {"stage", json_value::number(setup.stage)},
								 {"side", json_value::string(setup.side == pbd_lesson_side::right ? "right" : "left")},
								 {"stance", json_value::string(setup.stance == pbd_lesson_stance::crouching ? "crouching" : "standing")},
								 {"hitCondition", json_value::string(setup.hit_condition == pbd_lesson_hit_condition::counter ? "counter" : "normal")}})}})) +
		       '\n';
	}
	inline thread_local channel current;
	inline void channel::end_capture(const std::string& id)
	{
		ack = id;
		capture.active = false;
		if (!capture.started || !capture.valid)
		{
			capture.started = false;
			outcome = "capture_unavailable";
			return;
		}
		const bool written = practice_store::commit_text(root / "capture.json", capture_document(capture, id, generation));
		if (written)
			capture.started = false;
		finish(written ? "complete" : "capture_unavailable");
	}
	inline bool controls_dummy()
	{
		return current.play.active();
	}
	inline std::optional<std::uint16_t> input(std::uint32_t port, bool allowed, bool on_right)
	{
		if (!current.play.active())
			return {};
		const auto word = current.play.override_for(port, allowed && current.permitted);
		if (!word)
			return {};
		return on_right ? pbd::practice::movement::oriented(*word, pbd::practice::movement::screen_side::right) : *word;
	}
	inline const char* refusal(pbd_lesson_check result)
	{
		constexpr std::array names{"ready", "invalid_setup", "unsupported_stage", "not_practice", "not_ready",
			"wrong_fighters", "wrong_stage", "wrong_side", "wrong_stance", "wrong_hit_condition"};
		return names[static_cast<std::size_t>(result)];
	}
} // namespace pbd::practice::lesson
