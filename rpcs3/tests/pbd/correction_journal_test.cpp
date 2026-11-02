#include "pbd_test.h"
#include "Emu/PBD/pbd_correction_journal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using pbd::test::check;
using kind = pbd::correction_journal::kind;
using namespace std::chrono_literals;

namespace
{
	// A disk the test controls: it can hold a write until released, cut writes to half or throw.
	struct disk
	{
		std::mutex mutex;
		std::string text;
		std::atomic<bool> stall{false}, throws{false}, cancelled{false};
		std::atomic<int> short_writes{0};
		std::counting_semaphore<64> entered{0}, release{0}, closed{0};

		std::string contents()
		{
			std::lock_guard lock(mutex);
			return text;
		}
	};

	struct file
	{
		disk* target = nullptr;
		~file()
		{
			if (target)
				target->closed.release();
		}
		bool open(disk* value)
		{
			target = value;
			return true;
		}
		std::size_t write(const void* bytes, std::size_t size)
		{
			if (target->stall.load())
			{
				target->entered.release();
				target->release.acquire();
			}
			if (target->throws.load())
				throw std::runtime_error("unavailable disk");
			if (target->cancelled.load())
				return 0;
			std::size_t written = size;
			if (target->short_writes.load() > 0 && size)
			{
				target->short_writes--;
				written = size / 2;
			}
			std::lock_guard lock(target->mutex);
			target->text.append(static_cast<const char*>(bytes), written);
			return written;
		}
	};

	std::vector<std::string> lines(const std::string& text)
	{
		std::vector<std::string> result;
		for (std::size_t at = 0; at < text.size();)
		{
			const auto end = text.find('\n', at);
			result.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
			at = end == std::string::npos ? text.size() : end + 1;
		}
		return result;
	}

	std::uint64_t field(const std::string& row, std::string_view name)
	{
		const auto key = "\"" + std::string(name) + "\":";
		const auto at = row.find(key);
		check(at != std::string::npos, "summary field missing: " + std::string(name));
		return std::stoull(row.substr(at + key.size()));
	}

	bool summary(const std::string& row)
	{
		return row.starts_with("{\"type\":\"correction_journal_end\",");
	}

	// Rows shaped like the ones the session writes.
	std::string predicted(unsigned index)
	{
		return "{\"type\":\"predicted_update\",\"index\":" + std::to_string(index) +
		       ",\"completed\":true,\"failure\":null,\"raw\":[0,16],\"after_counter\":5,\"held\":[0,16],\"rising\":[0,16]}\n";
	}
	std::string corrected(unsigned index)
	{
		return "{\"type\":\"corrected_update\",\"index\":" + std::to_string(index) +
		       ",\"elapsed_ns\":812345,\"completed\":true,\"failure\":null,\"raw\":[0,16],\"before_counter\":4,\"after_counter\":5,"
		       "\"syscall_total_ns\":1200,\"syscall_count\":3,\"syscalls\":[[\"sys_event_queue_receive\",1,900]],"
		       "\"ports\":[{\"port\":0,\"injected\":true,\"posted\":true,\"injected_r3\":0,\"raw\":0,\"effective\":0,\"held\":0,\"rising\":0,\"stored_held\":0,\"stored_rising\":0},"
		       "{\"port\":1,\"injected\":true,\"posted\":true,\"injected_r3\":16,\"raw\":16,\"effective\":16,\"held\":16,\"rising\":16,\"stored_held\":16,\"stored_rising\":16}]}\n";
	}
	const std::string restore = "{\"type\":\"correction\",\"correct_at\":130,\"depth\":3,\"predict_raw\":0,\"restored\":true,\"reason\":null,\"undo_pages\":[[268435456,1,2,127]]}\n";
	const std::string window_end = "{\"type\":\"correction_clock\",\"first_index\":127,\"journal\":[2,2,2],\"replayed\":[2,2,2],\"mismatches\":0,\"clock_error\":false}\n"
								   "{\"type\":\"correction_end\",\"boundary\":130,\"clock_mismatches\":0,\"clock_error\":false,\"correction_wall_ns\":3120000}\n";
	const std::string arm = "{\"type\":\"fault_arm\",\"armed\":12,\"by_mb\":[[268435456,12]]}\n";
	const std::string policy = "{\"type\":\"undo_policy\",\"admit_rsx_io\":false,\"admit_library\":true,\"deny_spans\":0,\"static_pages\":40}\n";
	const std::string timing = "{\"type\":\"host_timing\",\"through\":600,\"end\":false,\"live_ticks\":600}\n";

	template <typename Sink>
	bool put(Sink& journal, kind type, const std::string& row)
	{
		return journal.write(type, row.data(), row.size());
	}
} // namespace

PBD_TEST(PbdCorrectionJournal, AcceptedRowsAreByteIdenticalAndTheSummaryComesLast)
{
	disk output;
	pbd::correction_journal::sink<file, 4096> journal;
	check(!journal);
	check(journal.open(&output));
	journal.start();
	check(journal);
	const std::vector<std::pair<kind, std::string>> rows{{kind::undo_policy, policy}, {kind::fault, arm}, {kind::predicted_update, predicted(127)},
		{kind::predicted_update, predicted(128)}, {kind::correction, restore}, {kind::corrected_update, corrected(127)},
		{kind::corrected_update, corrected(128)}, {kind::correction_end, window_end}, {kind::host_timing, timing}};
	std::string expected;
	bool accepted = true;
	for (const auto& [type, row] : rows)
	{
		expected += row;
		accepted &= put(journal, type, row);
	}
	journal.close();
	check(accepted && !journal);
	check(!put(journal, kind::host_timing, timing), "a closed journal took a row");
	const auto text = output.contents();
	check(text.starts_with(expected), "accepted rows changed on their way to the file");
	const auto tail = lines(text.substr(expected.size()));
	check(tail.size() == 1 && summary(tail[0]) && text.ends_with("}\n"), "the file does not end with exactly one summary row");
	const auto& end = tail[0];
	check(field(end, "rows_written") == 10 && field(end, "bytes_written") == expected.size());
	check(field(end, "rows_dropped") == 0 && field(end, "bytes_dropped") == 0 && field(end, "rows_unwritten") == 0);
	check(field(end, "queue_capacity_bytes") == 4096 && field(end, "queue_high_water_bytes") >= corrected(128).size() &&
		  field(end, "queue_high_water_bytes") <= 4096);
	check(end.find("\"write_failed\":false}") != std::string::npos);
	check(end.find("\"owner_enqueue_max_ns\":") != std::string::npos && end.find("\"worker_write_max_ns\":") != std::string::npos);
}

PBD_TEST(PbdCorrectionJournal, SummaryRowStatesEveryTotal)
{
	pbd::correction_journal::totals t;
	t.rows_written = 1200;
	t.bytes_written = 345678;
	t.dropped_by_kind = {3, 1, 0, 2, 0, 0, 0};
	t.rows_dropped = 6;
	t.bytes_dropped = 2048;
	t.rows_unwritten = 4;
	t.capacity = 4194304;
	t.high_water = 65536;
	t.enqueue_max_ns = 950;
	t.write_max_ns = 812000000;
	t.slow_writes = {7, 3, 2, 1, 1};
	t.write_failed = true;
	const auto row = pbd::correction_journal::summary_row(t);
	check(row ==
		  "{\"type\":\"correction_journal_end\",\"rows_written\":1200,\"bytes_written\":345678,\"rows_dropped\":6,\"bytes_dropped\":2048,"
		  "\"dropped_predicted_update\":3,\"dropped_corrected_update\":1,\"dropped_correction\":0,\"dropped_correction_end\":2,\"dropped_fault\":0,"
		  "\"dropped_host_timing\":0,\"dropped_undo_policy\":0,"
		  "\"rows_unwritten\":4,\"queue_capacity_bytes\":4194304,\"queue_high_water_bytes\":65536,\"owner_enqueue_max_ns\":950,\"worker_write_max_ns\":812000000,"
		  "\"worker_writes_over_ms\":{\"1\":7,\"5\":3,\"16.7\":2,\"50\":1,\"100\":1},\"write_failed\":true}\n");
	// Tools pick rows by a quoted type name, such as any line containing "correction_end": none may match this row.
	for (const char* name : pbd::correction_journal::kind_names)
		check(row.find("\"" + std::string(name) + "\"") == std::string::npos, std::string("the summary quotes the row type ") + name);
}

PBD_TEST(PbdCorrectionJournal, OwnerNeverWaitsForAStalledDiskAndDropsWholeRowsWithCountsWhenFull)
{
	disk output;
	pbd::correction_journal::sink<file, 1024> journal;
	check(journal.open(&output));
	output.stall = true;
	journal.start();
	check(put(journal, kind::undo_policy, policy));
	check(output.entered.try_acquire_for(2s), "the worker never reached the disk");
	// The worker is now held inside its write. The owner keeps producing, beyond the queue's capacity.
	struct offered
	{
		kind type;
		std::string row;
		bool accepted;
	};
	auto producer = std::async(std::launch::async, [&]
		{
			std::vector<offered> result;
			const std::string overlarge = "{\"type\":\"correction\",\"undo_pages\":[" + std::string(1100, '1') + "]}\n";
			for (unsigned i = 0; i < 40; ++i)
			{
				const auto row = i % 4 == 3 ? corrected(i) : predicted(i);
				const auto type = i % 4 == 3 ? kind::corrected_update : kind::predicted_update;
				result.push_back({type, row, put(journal, type, row)});
				if (i % 10 == 0)
					result.push_back({kind::correction, overlarge, put(journal, kind::correction, overlarge)});
				if (i == 20)
					result.push_back({kind::correction_end, window_end, put(journal, kind::correction_end, window_end)});
			}
			return result;
		});
	const bool finished_while_stalled = producer.wait_for(5s) == std::future_status::ready;
	output.stall = false;
	output.release.release();
	check(finished_while_stalled, "the owner waited for the disk");
	const auto rows = producer.get();
	journal.close();

	std::string expected = policy;
	std::array<std::uint64_t, pbd::correction_journal::kind_count> dropped{};
	std::uint64_t dropped_rows = 0, dropped_bytes = 0, written_rows = 1, accepted = 0;
	for (const auto& entry : rows)
	{
		const auto count = static_cast<std::uint64_t>(std::count(entry.row.begin(), entry.row.end(), '\n'));
		if (entry.accepted)
		{
			expected += entry.row;
			written_rows += count;
			accepted++;
			continue;
		}
		dropped[static_cast<std::size_t>(entry.type)] += count;
		dropped_rows += count;
		dropped_bytes += entry.row.size();
	}
	check(accepted > 0 && dropped[static_cast<std::size_t>(kind::predicted_update)] > 0 && dropped[static_cast<std::size_t>(kind::correction)] == 4 &&
			  dropped[static_cast<std::size_t>(kind::correction_end)] == 2,
		"the test did not fill the queue as intended");
	const auto text = output.contents();
	check(text.starts_with(expected), "accepted rows were reordered, cut or mixed with dropped ones");
	const auto tail = lines(text.substr(expected.size()));
	check(tail.size() == 1 && summary(tail[0]));
	const auto& end = tail[0];
	check(field(end, "rows_written") == written_rows && field(end, "bytes_written") == expected.size());
	check(field(end, "rows_dropped") == dropped_rows && field(end, "bytes_dropped") == dropped_bytes);
	for (std::size_t k = 0; k < pbd::correction_journal::kind_count; ++k)
		check(field(end, std::string("dropped_") + pbd::correction_journal::kind_names[k]) == dropped[k],
			std::string("wrong drop count for ") + pbd::correction_journal::kind_names[k]);
	check(field(end, "queue_high_water_bytes") <= 1024 && field(end, "queue_high_water_bytes") > 1024 - corrected(0).size());
	check(field(end, "worker_write_max_ns") > 0);
}

PBD_TEST(PbdCorrectionJournal, WorkerDrainsInOrderAcrossManyWraps)
{
	disk output;
	pbd::correction_journal::sink<file, 4000> journal;
	check(journal.open(&output));
	journal.start();
	std::string expected;
	std::uint64_t dropped = 0;
	for (unsigned i = 0; i < 2000; ++i)
	{
		const auto row = predicted(i);
		if (put(journal, kind::predicted_update, row))
			expected += row;
		else
			dropped++;
		if (i % 16 == 15)
			std::this_thread::sleep_for(2ms);
	}
	journal.close();
	check(expected.size() > 4 * 4000, "too few rows were accepted to wrap the queue");
	const auto text = output.contents();
	check(text.starts_with(expected));
	const auto tail = lines(text.substr(expected.size()));
	check(tail.size() == 1 && summary(tail[0]) && field(tail[0], "rows_dropped") == dropped && field(tail[0], "bytes_written") == expected.size());
}

PBD_TEST(PbdCorrectionJournal, SessionEndGetsTheSummaryAndALateRowGetsAFreshOne)
{
	disk output;
	pbd::correction_journal::sink<file, 4096> journal;
	check(journal.open(&output));
	journal.start();
	check(put(journal, kind::predicted_update, predicted(1)) && put(journal, kind::predicted_update, predicted(2)));
	check(journal.finish(2s), "the summary was not written");
	auto all = lines(output.contents());
	check(all.size() == 3 && summary(all[2]) && field(all[2], "rows_written") == 2);
	// A closing host_timing row can follow the end; the file still ends with a summary that counts it.
	check(put(journal, kind::host_timing, timing));
	check(journal.finish(2s));
	all = lines(output.contents());
	check(all.size() == 5 && all[3] + "\n" == timing && summary(all[4]) && field(all[4], "rows_written") == 3);
	journal.close();
	check(lines(output.contents()).size() == 5, "close repeated a summary with nothing new");
}

PBD_TEST(PbdCorrectionJournal, ShutdownIsBoundedWhileTheDiskStaysStalled)
{
	static disk output; // Outlives the test: the worker left in the stalled write still owns its file.
	pbd::correction_journal::sink<file, 4096> journal;
	check(journal.open(&output));
	output.stall = true;
	std::atomic<int> cancels{0};
	journal.start([&](std::thread&)
		{
			cancels++;
		});
	check(put(journal, kind::predicted_update, predicted(1)));
	check(output.entered.try_acquire_for(2s));
	check(put(journal, kind::predicted_update, predicted(2)), "the queue refused a row while the disk was stalled");
	auto begin = std::chrono::steady_clock::now();
	const bool summarized = journal.finish(50ms);
	const auto finish_wait = std::chrono::steady_clock::now() - begin;
	begin = std::chrono::steady_clock::now();
	journal.close();
	const auto close_wait = std::chrono::steady_clock::now() - begin;
	const bool left_behind = !output.closed.try_acquire();
	output.stall = false;
	output.release.release();
	const bool worker_ended = output.closed.try_acquire_for(5s);
	check(!summarized && finish_wait < 1s, "the session end waited on the stalled disk");
	check(close_wait >= 150ms && close_wait < 2s, "teardown was not bounded");
	check(cancels == 2 && left_behind, "the stalled write was not cancelled before the writer was left behind");
	check(worker_ended, "the writer left behind never ended after the disk recovered");
	check(output.contents().find("correction_journal_end") == std::string::npos, "a cancelled journal claimed to be complete");
}

PBD_TEST(PbdCorrectionJournal, CancelledWriteEndsTheWorkerAndTeardownJoinsIt)
{
	disk output;
	pbd::correction_journal::sink<file, 4096> journal;
	check(journal.open(&output));
	output.stall = true;
	journal.start([&](std::thread&)
		{
			output.cancelled = true;
			output.stall = false;
			output.release.release();
		});
	check(put(journal, kind::predicted_update, predicted(1)));
	check(output.entered.try_acquire_for(2s));
	const auto begin = std::chrono::steady_clock::now();
	journal.close();
	check(std::chrono::steady_clock::now() - begin < 2s);
	check(output.closed.try_acquire_for(1s), "teardown did not join the cancelled writer");
	check(output.contents().empty());
}

PBD_TEST(PbdCorrectionJournal, FailedWriteDropsLaterRowsAndTheSummaryStartsItsOwnLine)
{
	disk output;
	output.short_writes = 1;
	pbd::correction_journal::sink<file, 4096> journal;
	check(journal.open(&output));
	journal.start();
	check(put(journal, kind::predicted_update, predicted(1)));
	// The worker has met the short write once the summary is out; later rows are dropped, not queued behind it.
	check(journal.finish(2s));
	check(!put(journal, kind::corrected_update, corrected(2)) && !put(journal, kind::correction_end, window_end));
	journal.close();
	const auto all = lines(output.contents());
	check(all.size() == 3 && all[0] + "\n" != predicted(1) && summary(all[1]) && summary(all[2]) && output.contents().ends_with("}\n"));
	check(field(all[1], "rows_dropped") == 0 && field(all[2], "rows_written") == 0 && field(all[2], "rows_unwritten") == 1 &&
		  field(all[2], "rows_dropped") == 3);
	check(field(all[2], "dropped_corrected_update") == 1 && field(all[2], "dropped_correction_end") == 2);
	check(all[2].find("\"write_failed\":true}") != std::string::npos);

	disk throwing;
	throwing.throws = true;
	pbd::correction_journal::sink<file, 4096> unavailable;
	check(unavailable.open(&throwing));
	unavailable.start();
	check(put(unavailable, kind::predicted_update, predicted(1)));
	unavailable.close();
	check(throwing.contents().empty());
}

PBD_TEST(PbdCorrectionJournal, RealFileIsCreatedExclusivelyAndKeepsEveryByte)
{
	struct temporary_file
	{
		std::filesystem::path path;
		~temporary_file()
		{
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}
	} temporary{std::filesystem::temp_directory_path() / std::filesystem::path(u8"pbd-tick-correction-é-")};
	temporary.path += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl";
	const auto utf8 = temporary.path.u8string();
	const std::string name(utf8.begin(), utf8.end());
	const std::string rows = predicted(1) + restore + corrected(1) + window_end;
	{
		pbd::correction_journal::sink<> journal;
		check(journal.open(name));
		journal.start();
		check(put(journal, kind::predicted_update, predicted(1)) && put(journal, kind::correction, restore) &&
			  put(journal, kind::corrected_update, corrected(1)) && put(journal, kind::correction_end, window_end));
		// The launcher's live connection report reads the file while the game writes it.
		check(std::ifstream(temporary.path, std::ios::binary).is_open(), "the journal cannot be read while it is written");
		journal.close();
		pbd::correction_journal::sink<> duplicate;
		check(!duplicate.open(name), "an existing journal was truncated");
	}
	std::ifstream input(temporary.path, std::ios::binary);
	const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	check(text.starts_with(rows), "the real file lost or changed queued bytes");
	const auto tail = lines(text.substr(rows.size()));
	check(tail.size() == 1 && summary(tail[0]) && field(tail[0], "rows_written") == 5 && field(tail[0], "queue_capacity_bytes") == 1024 * 1024);
}
