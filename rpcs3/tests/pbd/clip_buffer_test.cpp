#include "pbd_test.h"

#include "Emu/PBD/pbd_clip_buffer.h"
#include "Emu/PBD/pbd_held_presentation.h"
#include "util/pbd_frame_copy.h"
#include <chrono>
#include <future>
#include <mutex>

using pbd::test::check;

namespace
{
	void clip_checks()
	{
		using namespace pbd::clips;
		const auto occupied = [](const std::string& file)
		{
			return file == "Match.mp4" || file == "Match (2).mp4.part";
		};
		check(available_filename("Match", occupied) == "Match (3).mp4", "pending encodes and completed clips both reserve a name");
		check(!available_filename("Match", [](const auto&)
				  {
					  return true;
				  }),
			"exhausted names must never overwrite a clip");
		check(available_filename("Match", [](const auto&)
				  {
					  return false;
				  }) == "Match.mp4",
			"a free filename needs no suffix");
		buffer saved;
		saved.seconds(15);
		for (int second = 0; second < 40; ++second)
		{
			auto video = std::make_shared<packet>();
			video->time_us = second * 1'000'000ll;
			video->keyframe = second % 2 == 0;
			video->bytes.resize(100);
			saved.push(video);
			auto sound = std::make_shared<packet>();
			sound->time_us = video->time_us;
			sound->audio = true;
			sound->bytes.resize(10);
			saved.push(sound);
		}
		const auto clip = saved.snapshot();
		check(!clip.empty() && !clip.front()->audio && clip.front()->keyframe, "a clip must start on a video keyframe");
		check(clip.front()->time_us == 24'000'000 && clip.back()->time_us == 39'000'000, "save only the requested last part");
		check(saved.bytes() <= 17 * 110, "the encoded ring must evict old packets");
		saved.clear();
		check(saved.snapshot().empty(), "turning off background recording drops its history");
		saved.push(std::make_shared<packet>());
		check(saved.snapshot().empty(), "never save an undecodable partial GOP");
		gesture button;
		check(button.update(true, 0) == action::none && button.update(true, 2000) == action::none, "a button already held at startup is not a new press");
		button.update(false, 2001);
		button.update(true, 2100);
		check(button.update(false, 2200) == action::save, "a short Create press saves the buffer");
		button.update(true, 2300);
		check(button.update(true, 3300) == action::record, "a one-second hold starts or stops recording");
		check(button.update(true, 4300) == action::none && button.update(false, 4400) == action::none, "a hold fires once and never saves on release");
		int saves = 0;
		for (std::uint64_t now = 5000; now < 5600; now += 10)
			saves += button.update(true, now) == action::save;
		saves += button.update(false, 5600) == action::save;
		saves += button.update(false, 5700) == action::save;
		check(saves == 1, "one press saves one clip, however many samples it lasts");
		check(file_label("Ranked Jin Kazama vs Kazuya Mishima") == "Ranked Jin Kazama vs Kazuya Mishima", "a launcher label is kept as written");
		check(file_label("Lobby JACK-5 vs Roger Jr.") == "Lobby JACK-5 vs Roger Jr", "character marks stay; a trailing dot never meets the extension");
		check(file_label("..\\..\\Windows/System32:x*?\"<>|") == "Windows System32 x", "no path separator, drive mark or reserved character survives");
		check(file_label("Practice\t\r\nLili\xC3\x91") == "Practice Lili", "control characters and other bytes become one space at most");
		check(file_label(std::string(200, 'a')).size() == 80 && file_label("").empty() && file_label(" . ").empty(), "labels stay short and may be empty");
		std::vector<std::uint8_t> pixels(16, 42);
		auto lease = std::make_shared<std::atomic<bool>>(false);
		std::promise<bool> result;
		auto completed = result.get_future();
		frame_copy copy;
		check(copy.offer(pixels.data(), 8, 2, 2, true, 123, lease,
				  [&](std::vector<std::uint8_t>&& bytes, unsigned pitch, unsigned w, unsigned h, bool bgra, std::uint64_t time)
				  {
					  result.set_value(bytes == pixels && pitch == 8 && w == 2 && h == 2 && bgra && time == 123);
				  }),
			"enqueue a completed readback");
		const auto ready = completed.wait_for(std::chrono::seconds(10));
		copy.stop();
		check(ready == std::future_status::ready, "the worker delivers within its deadline");
		check(completed.get(), "the worker preserves pixels and capture time");
		check(!lease->load(), "shutdown must release mapped-buffer readers");
		// One fence check can finish several presented frames; offered back to back, every one is copied, in order.
		std::vector<std::shared_ptr<std::atomic<bool>>> leases;
		std::mutex delivered_mutex;
		std::vector<std::uint64_t> delivered;
		std::promise<void> all;
		auto all_completed = all.get_future();
		frame_copy burst;
		for (std::uint64_t time = 1; time <= 4; ++time)
		{
			leases.push_back(std::make_shared<std::atomic<bool>>(false));
			check(burst.offer(pixels.data(), 8, 2, 2, true, time, leases.back(),
					  [&](std::vector<std::uint8_t>&&, unsigned, unsigned, unsigned, bool, std::uint64_t value)
					  {
						  std::lock_guard lock(delivered_mutex);
						  delivered.push_back(value);
						  if (delivered.size() == 4)
							  all.set_value();
					  }),
				"frames finished together must all queue");
		}
		const auto all_ready = all_completed.wait_for(std::chrono::seconds(10));
		burst.stop();
		check(all_ready == std::future_status::ready, "all queued frames deliver within their deadline");
		check(delivered == std::vector<std::uint64_t>{1, 2, 3, 4}, "queued frames keep presentation order");
		check(!burst.offer(pixels.data(), 8, 2, 2, true, 5, leases.front(), [](auto&&...) {}), "a stopped copier refuses, so the caller counts the drop");
	}

	// A Sony pad read without SDL's mapping: raw buttons 0-13 in the backend's Sony order, Select at 8, Start at 9, Home at
	// 12 and the touchpad click at 13.
	pbd::controllers::layout sony_layout()
	{
		namespace pad = pbd::controllers;
		pad::layout value;
		constexpr std::array<unsigned, 14> order{pad::square, pad::cross, pad::circle, pad::triangle, pad::l1, pad::r1, pad::l2, pad::r2,
			pad::select, pad::start, pad::l3, pad::r3, pad::home, pad::touchpad};
		for (unsigned i = 0; i < order.size(); ++i)
			value[order[i]] = pad::binding{pad::binding::button, i};
		return value;
	}

	void pad_button_checks()
	{
		using namespace pbd::clips;
		namespace pad = pbd::controllers;
		check(parse_pad_button("home").source == pad_source::home, "Home keeps its meaning");
		const auto raw = parse_pad_button("button-8");
		check(raw.source == pad_source::raw && raw.index == 8, "a raw button keeps its number");
		for (const auto text : {"create", "share", "touchpad", "", "capture", "button-", "button-x", "button-8x"})
			check(parse_pad_button(text).source == pad_source::capture,
				"the Create default, the older Share and Touchpad and anything unknown are the pad's own capture button");

		const auto defaults = sony_layout();
		const pad::layout untaught{};
		pad::raw_state state;
		state.buttons.assign(16, false);
		const auto sample = [&](const pad_button& choice, std::uint32_t buttons, bool misc1 = false, bool versus = false, const pad::layout& taught = {})
		{
			return pressed(choice, {state, defaults, taught, buttons, misc1, versus});
		};
		const pad_button capture{};
		check(!sample(capture, pad::bit(pad::select)) && !sample(capture, pad::bit(pad::select) | pad::bit(pad::circle)),
			"Select, held alone or in a Practice chord, never saves");
		check(!sample(capture, pad::bit(pad::start)) && !sample(capture, pad::bit(pad::home)), "neither do Start and Home");
		check(sample(capture, 0, true) && !sample(capture, pad::bit(pad::touchpad)), "the capture button is MISC1 alone, never the touchpad click");
		pad::layout share_as_select{};
		share_as_select[pad::select] = pad::binding{pad::binding::button, 15};
		const auto misc1_on_15 = [&](const pad_button& choice, const pad::layout& taught)
		{
			return pressed(choice, {state, defaults, taught, 0, true, false, pad::binding{pad::binding::button, 15}});
		};
		check(!misc1_on_15(capture, share_as_select) && misc1_on_15(capture, untaught),
			"a Share button the player taught as a game button is that button and saves nothing");
		check(sample({pad_source::home}, pad::bit(pad::home)) && !sample({pad_source::home}, pad::bit(pad::home), false, true),
			"Home saves, except in a match, where it is the game's Start");
		for (const auto text : {"create", "share", "touchpad", "home", ""})
			check(!sample(parse_pad_button(text), pad::bit(pad::touchpad)),
				"no clip choice saves on the touchpad click, which the game keeps as Select or Start");

		const auto raw_pressed = [&](unsigned index, bool versus = false, const pad::layout& taught = {})
		{
			state.buttons.assign(16, false);
			state.buttons[index] = true;
			return sample({pad_source::raw, index}, 0, false, versus, taught);
		};
		check(!raw_pressed(8) && !raw_pressed(9), "a raw button that is Select or Start is refused");
		check(!raw_pressed(13), "so is the raw touchpad click, which the game gets as Select or Start");
		check(raw_pressed(12) && !raw_pressed(12, true), "raw Home saves, except in a match");
		check(raw_pressed(3) && raw_pressed(15), "any other raw button is the player's choice");
		pad::layout moved_select{};
		moved_select[pad::select] = pad::binding{pad::binding::button, 15};
		check(raw_pressed(8, false, moved_select) && !raw_pressed(15, false, moved_select), "Select follows the button the player taught it");
		pad::layout retaught{};
		retaught[pad::cross] = pad::binding{pad::binding::button, 8};
		check(raw_pressed(8, false, retaught), "a button taught another job is no longer Select");
		check(reaches_select_or_start(defaults, untaught, 9, false) && !raw_pressed(9, false, retaught), "Start keeps its button");
	}

	void standby_checks()
	{
		using pbd::clips::pad_press_counts;
		using namespace pbd::held_presentation;
		lifecycle game{purpose::none};
		check(pad_press_counts(game.can_show(), game.parked(), true), "a shown game saves on a controller press, focused or not");
		check(!pad_press_counts(game.can_show(), game.parked(), false), "a hidden or minimized game does not");
		lifecycle standby{purpose::match};
		check(!pad_press_counts(standby.can_show(), standby.parked(), true), "a standby waiting for its match saves nothing");
		standby.park(true);
		check(!pad_press_counts(standby.can_show(), standby.parked(), true), "nor does a parked one");
		check(standby.bound(purpose::match, false), "the standby is activated");
		standby.park(false);
		standby.play(0);
		check(pad_press_counts(standby.can_show(), standby.parked(), true), "the activated game saves like any shown game");
	}
} // namespace
PBD_TEST(PbdClips, KeyframesGesturesAndWorkerCopy)
{
	clip_checks();
}
PBD_TEST(PbdClips, PadButtonNeverSelectOrStart)
{
	pad_button_checks();
}
PBD_TEST(PbdClips, StandbyAndHiddenGamesIgnorePads)
{
	standby_checks();
}
