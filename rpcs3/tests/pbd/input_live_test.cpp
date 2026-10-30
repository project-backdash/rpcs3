#include "pbd_test.h"

#include "Emu/Cell/Modules/pbd_input_live.h"
#include "Emu/PBD/pbd_json_reader.h"

#include <chrono>
#include <fstream>

namespace pbd::test
{
	struct input_live_fixture : input_live
	{
		std::filesystem::path directory;
		input_live_fixture()
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			directory = std::filesystem::temp_directory_path() / ("pbd-input-live-" + std::to_string(stamp));
			pbd::test::check(std::filesystem::create_directory(directory));
			m_dir = directory;
			m_run_id = "native-live";
			m_enabled = true;
		}
		~input_live_fixture()
		{
			std::error_code error;
			if (directory.parent_path() == std::filesystem::temp_directory_path() && directory.filename().string().starts_with("pbd-input-live-"))
				std::filesystem::remove_all(directory, error);
		}
		void press(u32 hold, u32 release)
		{
			std::ofstream out(directory / "cmd-000001.txt");
			out << "DR2026-LIVE-CMD 1\nrun native-live\nseq 1\nop press\nbuttons 16 64\nhold_polls " << hold
				<< "\nrelease_polls " << release << "\nstable_polls 2\nend\n";
			out.close();
			pbd::test::check(out.good());
		}
		json::json_value observation() const
		{
			const auto text = json::detail::read_text(directory / "obs-000001.json");
			pbd::test::check(text.value);
			auto parsed = json::detail::json_parser(*text.value).parse();
			pbd::test::check(parsed.value);
			return std::move(*parsed.value);
		}
	};
} // namespace pbd::test

PBD_TEST(PbdInputLive, CommandsReturnTheRequestedPressAndReleaseCounts)
{
	using pbd::json::detail::number;
	using pbd::test::check;
	for (const u32 hold : {1u, 2u, 3u, 30u})
		for (const u32 release : {1u, 2u, 3u, 120u})
		{
			pbd::test::input_live_fixture input;
			input.press(hold, release);
			std::vector<std::pair<u16, u16>> samples;
			for (u32 poll = 0; poll < 256 && !std::filesystem::exists(input.directory / "obs-000001.json"); ++poll)
				samples.push_back(input.next({}));
			const auto observed = input.observation();
			check(pbd::json::detail::is(observed.find("status"), "done"));
			std::uint64_t command_poll = 0, press_offset = 0, release_offset = 0, pressed = 0, released = 0, completed = 0;
			check(number(observed.find("command_poll"), 0, 256, command_poll));
			check(number(observed.find("press_offset"), 0, 256, press_offset) && press_offset == 2, "two neutral polls precede input");
			check(number(observed.find("release_offset"), 0, 256, release_offset) && release_offset == press_offset + hold);
			check(number(observed.find("pressed_polls"), 0, 30, pressed) && pressed == hold);
			check(number(observed.find("released_polls"), 0, 120, released) && released == release);
			check(number(observed.find("poll"), 0, 256, completed) && completed == command_poll + release_offset + release + 1,
				"acknowledgement takes two stable polls after all requested release samples");
			for (usz i = 0; i < samples.size(); ++i)
			{
				const auto poll = i + 1;
				const bool down = poll >= command_poll + press_offset && poll < command_poll + release_offset;
				check(samples[i] == (down ? std::pair<u16, u16>{16, 64} : std::pair<u16, u16>{0, 0}),
					"the real command stream returns each pressed and neutral word at its reported offset");
			}
			check(input.next({}) == std::pair<u16, u16>{0, 0}, "a completed command is not replayed");
		}
}

PBD_TEST(PbdInputLive, InterceptionKeepsOnlyConsumedPhaseSamples)
{
	using pbd::json::detail::number;
	using pbd::test::check;
	for (const bool after_release : {false, true})
	{
		pbd::test::input_live_fixture input;
		input.press(3, 3);
		u32 pressed = 0, released = 0;
		for (u32 poll = 0; poll < 16; ++poll)
		{
			const auto word = input.next({});
			if (word == std::pair<u16, u16>{16, 64})
				++pressed;
			else if (pressed)
				++released;
			if ((!after_release && pressed == 1) || released == 1)
				break;
		}
		input.note_intercepted();
		const auto observed = input.observation();
		std::uint64_t consumed = 0;
		check(pbd::json::detail::is(observed.find("status"), "intercepted"));
		check(number(observed.find("pressed_polls"), 0, 30, consumed) && consumed == pressed);
		check(number(observed.find("released_polls"), 0, 120, consumed) && consumed == released);
		check(number(observed.find("intercepted_polls"), 0, 1, consumed) && consumed == 1);
		for (unsigned poll = 0; poll < 8; ++poll)
			check(input.next({}) == std::pair<u16, u16>{0, 0}, "interception aborts the remaining press without resending");
	}
}
