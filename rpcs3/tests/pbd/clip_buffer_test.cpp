#include "pbd_test.h"

#include "Emu/PBD/pbd_clip_buffer.h"
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
		gesture create;
		create.update(false, 0, true);
		create.update(true, 100, true);
		check(create.update(false, 300, true) == action::none, "a Create/Share tap stays the game's Select and saves nothing");
		create.update(true, 400, true);
		check(create.update(true, 900, true) == action::save, "a half-second Create/Share hold saves the buffer");
		check(create.update(true, 2000, true) == action::none && create.update(false, 2100, true) == action::none, "the hold saves once and never records");
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
} // namespace
PBD_TEST(PbdClips, KeyframesGesturesAndWorkerCopy)
{
	clip_checks();
}
