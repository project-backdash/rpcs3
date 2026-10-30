#include "pbd_test.h"
#include "Emu/PBD/pbd_peer_log.h"
#include "Emu/PBD/pbd_session_health.h"

#include <chrono>
#include <future>
#include <fstream>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>

using pbd::test::check;

namespace
{
	struct disk
	{
		std::string text;
		bool fail_open = false, short_write = false, throws = false, stall = false;
		std::binary_semaphore entered{0}, release{0};
	};
	struct file
	{
		disk* target = nullptr;
		bool open(disk* value)
		{
			target = value;
			return !target->fail_open;
		}
		std::size_t write(const void* bytes, std::size_t size)
		{
			if (target->stall)
			{
				target->entered.release();
				target->release.acquire();
			}
			if (target->throws)
				throw std::runtime_error("unavailable disk");
			const auto written = target->short_write && size ? size - 1 : size;
			target->text.append(static_cast<const char*>(bytes), written);
			return written;
		}
	};
} // namespace

PBD_TEST(PbdPeerLog, PreservesBytesAcrossWrapAndFinalDrain)
{
	disk output;
	pbd::peer_log::buffered_file<file, 16> log;
	check(log.open(&output));
	check(log.write("header\n", 7) == 7);
	check(output.text.empty());
	check(log.drain());
	check(log.write("sample\n", 7) == 7);
	check(log.drain());
	check(log.write("terminal\n", 9) == 9);
	check(log.drain());
	check(log.drain());
	check(output.text == "header\nsample\nterminal\n");
}

PBD_TEST(PbdPeerLog, BoundaryContinuesWhileDiskWriteIsBlocked)
{
	using namespace std::chrono_literals;
	disk output;
	pbd::peer_log::buffered_file<file, 16384> log;
	check(log.open(&output));
	check(log.write("header\n", 7) == 7);
	output.stall = true;
	std::jthread reader([&]
		{
			log.drain();
		});
	const bool disk_entered = output.entered.try_acquire_for(1s);
	auto producer = std::async(std::launch::async, [&]
		{
			for (unsigned i = 0; i < 1000; ++i)
				if (log.write("sample\n", 7) != 7)
					return false;
			return true;
		});
	const bool finished_before_disk = producer.wait_for(1s) == std::future_status::ready;
	output.release.release();
	reader.join();
	const bool produced = producer.get();
	output.stall = false;
	check(disk_entered && finished_before_disk && produced);
	check(log.drain());
	check(output.text.size() == 7007);
	check(output.text.starts_with("header\nsample\n"));
}

PBD_TEST(PbdPeerLog, FullQueueWithholdsTerminalAndMarksIncomplete)
{
	disk output;
	pbd::peer_log::buffered_file<file, 8> log;
	check(log.open(&output));
	check(log.write("12345678", 8) == 8);
	check(log.write("x", 1) == 0);
	check(!log);
	check(!log.drain());
	check(log.write("end\n", 4) == 0);
	const auto previous = output.text;
	check(!log.drain());
	check(previous == output.text);
	check(output.text == "12345678\n{\"type\":\"peer_log_incomplete\"}\n");
}

PBD_TEST(PbdPeerLog, ConcurrentDrainRetainsEveryPublishedByte)
{
	disk output;
	pbd::peer_log::buffered_file<file, 1048576> log;
	check(log.open(&output));
	std::atomic<bool> done{false};
	std::jthread reader([&]
		{
			while (!done.load(std::memory_order_acquire))
				log.drain();
			log.drain();
		});
	std::string expected;
	bool accepted = true;
	for (unsigned i = 0; i < 10000; ++i)
	{
		const auto row = std::to_string(i) + "\n";
		expected += row;
		accepted &= log.write(row.data(), row.size()) == row.size();
	}
	done.store(true, std::memory_order_release);
	reader.join();
	check(accepted);
	check(output.text == expected);
}

PBD_TEST(PbdPeerLog, FailedDiskNeverThrowsOnOrRetriesTheBoundary)
{
	for (const bool throws : {false, true})
	{
		disk output;
		output.short_write = !throws;
		output.throws = throws;
		pbd::peer_log::buffered_file<file, 16> log;
		check(log.open(&output));
		check(log.write("sample\n", 7) == 7);
		check(!log.drain());
		check(!log);
		check(log.write("end\n", 4) == 0);
		const auto previous = output.text;
		check(!log.drain());
		check(output.text == previous);
	}
	disk output;
	output.fail_open = true;
	pbd::peer_log::buffered_file<file, 8> unopened;
	check(!unopened.open(&output));
	check(!unopened);
	check(unopened.write("x", 1) == 0);
	check(unopened.drain());
}

PBD_TEST(PbdPeerLog, BlockedDiagnosticWorkerCannotStopAdvancingHealth)
{
	using namespace std::chrono_literals;
	disk output;
	pbd::peer_log::buffered_file<file, 32768> log;
	check(log.open(&output));
	check(log.write("header\n", 7) == 7);
	output.stall = true;
	log.start([&](std::thread&)
		{
			output.release.release();
		});
	const bool blocked = output.entered.try_acquire_for(1s);
	pbd::session_health::activity health;
	bool healthy = true, enqueued = true, pages = true;
	for (std::uint32_t boundary = 1; boundary <= 25 * 60; ++boundary)
	{
		const auto now = std::uint64_t{boundary} * 16'666'667;
		const auto state = health.observe(boundary, now, true, false, false);
		healthy &= state == "running";
		enqueued &= log.write("sample\n", 7) == 7;
		const auto line = "{\"type\":\"health\",\"sequence\":" + std::to_string(boundary) +
		                  ",\"state\":\"" + std::string(state) + "\",\"idle_ms\":0}\n";
		pages &= pbd::session_health::snapshot(line).has_value();
	}
	// The sink stays blocked through all 25 seconds of virtual game/health progress, beyond the launcher's 20 s watchdog.
	log.stop();
	check(blocked && healthy && enqueued && pages);
	check(!log, "cancelled pending diagnostic rows were presented as a complete log");
	check(health.observe(1500, 27'000'000'000, true, false, false) == "waiting",
		"isolating diagnostic I/O hid a genuinely stopped game");
}

PBD_TEST(PbdPeerLog, WorkerDrainsFinalRowsBeforeItsFileIsDestroyed)
{
	disk output;
	pbd::peer_log::buffered_file<file, 32768> log;
	check(log.open(&output));
	log.start();
	std::string expected;
	bool accepted = true;
	for (unsigned row = 0; row < 1000; ++row)
	{
		const auto text = std::to_string(row) + "\n";
		expected += text;
		accepted &= log.write(text.data(), text.size()) == text.size();
	}
	log.stop();
	check(accepted && log);
	check(output.text == expected);
	disk next;
	check(log.open(&next));
	log.start();
	check(log.write("next\n", 5) == 5);
	log.stop();
	check(next.text == "next\n" && output.text == expected, "reopening reused an old queue or file");
}

PBD_TEST(PbdPeerLog, RealDiagnosticFileIsExclusiveAndPreservesUtf8Paths)
{
	struct temporary_file
	{
		std::filesystem::path path;
		~temporary_file()
		{
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}
	} temporary{std::filesystem::temp_directory_path() / std::filesystem::path(u8"pbd-peer-log-\u00e9-")};
	temporary.path += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl";
	const auto utf8 = temporary.path.u8string();
	const std::string name(utf8.begin(), utf8.end());
	{
		pbd::peer_log::buffered_file<pbd::peer_log::diagnostic_file, 128> log;
		check(log.open(name));
		log.start();
		check(log.write("header\nterminal\n", 16) == 16);
		log.stop();
		check(log);
		pbd::peer_log::diagnostic_file duplicate;
		check(!duplicate.open(name), "an existing peer log was truncated");
	}
	std::ifstream input(temporary.path, std::ios::binary);
	const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	check(text == "header\nterminal\n", "the real file lost queued bytes at shutdown");
}
