#include "pbd_test.h"

#include "Emu/Cell/pbd_session_history.h"
#include "Emu/Cell/pbd_update_script.h"
#include "Emu/PBD/pbd_replay_standby.h"
#include "Emu/PBD/pbd_stall_metrics.h"
#include "Emu/PBD/pbd_session_trace.h"
#include "Emu/PBD/pbd_session_health.h"
#include "Emu/PBD/pbd_json_reader.h"
#include <sstream>
#include <future>
#include <thread>
#include <set>
#include <barrier>
#include <chrono>
#include <cstdio>

using pbd::test::check;

PBD_TEST(PbdSessionHealth, RoundProgressIsOptionalAndFitsTheFixedHealthPage)
{
	using namespace pbd::session_health;
	check(progress_json({}).empty(), "missing native rounds were invented as a tied score");
	check(progress_json(progress{0, {0, 0}}).empty() && progress_json(progress{65536, {0, 0}}).empty(),
		"an invalid game number became health evidence");
	check(progress_json(progress{1, {5, 0}}).empty(), "an impossible unfinished round count became health evidence");
	const auto fields = progress_json(progress{65535, {4, 3}});
	const auto decoded = pbd::json::detail::json_parser("{\"type\":\"health\"" + fields + "}").parse();
	check(decoded.value.has_value(), "native round observations are not valid JSON");
	const auto* round_progress = decoded.value->find("progress");
	check(round_progress != nullptr, "native round observations are missing");
	std::uint32_t number = 0;
	check(pbd::json::detail::number32(round_progress->find("gameNumber"), 1, 65535, number) && number == 65535,
		"native round observations lost the game number");
	const auto* wins = round_progress->find("roundWins");
	check(wins != nullptr, "native round observations lost the round wins");
	check(pbd::json::detail::number32(wins->find("left"), 0, 4, number) && number == 4,
		"native round observations lost the left wins");
	check(pbd::json::detail::number32(wins->find("right"), 0, 4, number) && number == 3,
		"native round observations lost the right wins");
	check(snapshot("{\"type\":\"health\",\"schema\":1,\"sequence\":1,\"state\":\"waiting\",\"idle_ms\":90000" + fields + "}\n").has_value(),
		"round progress outgrew the allocated health page");
}

PBD_TEST(PbdSessionHealth, AdvancingFightDoesNotBecomeAPeerWait)
{
	pbd::session_health::activity health;
	for (std::uint32_t boundary = 1; boundary <= 6000; ++boundary)
	{
		const auto now = std::uint64_t{boundary} * 16'666'667;
		check(health.observe(boundary, now, true, false, false) == "running",
			"a progressing fight was reported as a peer wait");
		check(health.observe(boundary, now + 4'000'000, true, false, false) == "running",
			"sampling between two boundary publications reported a peer wait");
	}
}

PBD_TEST(PbdSessionHealth, FrozenPublicationsRemainWaitingDespiteFreshHealthPolls)
{
	pbd::session_health::activity health;
	check(health.observe(42, 0, true, false, false) == "running", "initial fight activity was lost");
	check(health.observe(42, 999'999'999, true, false, false) == "running", "a subsecond frame wait became a path fault");
	for (std::uint64_t second = 1; second <= 120; ++second)
		check(health.observe(42, second * 1'000'000'000, true, false, false) == "waiting",
			"repeated health sampling renewed a game with no advancing boundaries");
	check(health.observe(43, 120'001'000'000, true, false, false) == "running", "actual resumed progress did not clear waiting");
}

PBD_TEST(PbdSessionHealth, NetworkRecoveryRequiresResumedGameProgress)
{
	pbd::session_health::activity health;
	check(health.observe(42, 0, true, false, false) == "running", "initial fight was not running");
	check(health.observe(43, 100'000'000, true, true, true) == "waiting", "an active recovery was hidden by pause or advancing inputs");
	check(health.observe(43, 60'000'000'000, true, false, true) == "waiting", "a minute-long outage was reported healthy");
	check(health.observe(43, 60'100'000'000, true, false, false) == "waiting", "a cleared network flag invented resumed game progress");
	check(health.observe(44, 60'200'000'000, true, false, false) == "running", "confirmed advancement did not recover the same game");
}

PBD_TEST(PbdSessionHealth, LoadingAndIntentionalPausesStayDistinct)
{
	pbd::session_health::activity health;
	check(health.observe(0, 0, false, false, false) == "loading", "boot became an established path fault");
	check(health.observe(0, 60'000'000'000, false, false, false) == "loading", "a slow boot became an established path fault");
	check(health.observe(1, 61'000'000'000, true, false, false) == "running", "first battle publication was lost");
	check(health.observe(1, 120'000'000'000, true, true, false) == "paused", "an intentional menu hold became a peer wait");
	check(health.observe(1, 120'010'000'000, true, false, false) == "running", "resuming a menu hold falsely reported an outage");
	check(health.observe(1, 121'000'000'000, true, false, false) == "waiting", "a frozen game after a pause renewed forever");
}

PBD_TEST(PbdSessionHealth, FixedExtentAndChecksumSurviveSequenceGrowth)
{
	using namespace pbd::session_health;
	check(checksum("hello") == 0x4f9f2cabu, "snapshot checksum disagrees with the FNV-1a32 reader contract");
	check(!snapshot("") && !snapshot(std::string(snapshot_bytes, 'x')), "invalid snapshot size was accepted");
	// The same allocated page can carry every one-second observation of a three-hour session.
	page allocated;
	for (unsigned sequence = 1; sequence <= 10801; ++sequence)
	{
		const std::string payload = "{\"type\":\"health\",\"schema\":1,\"sequence\":" + std::to_string(sequence) +
		                            ",\"state\":\"paused\",\"idle_ms\":100000}\n";
		const auto encoded = snapshot(payload);
		check(encoded.has_value(), "three hours of heartbeat sequence growth exceeded its fixed page");
		allocated = *encoded;
		const std::string_view bytes(allocated.data(), allocated.size());
		check(bytes.size() == 4096 && bytes.substr(0, 13) == "PBD-HEALTH-1\n", "snapshot changed its allocated extent or magic");
		const auto length = std::stoul(std::string(bytes.substr(13, 8)), nullptr, 16);
		const auto expected = std::stoul(std::string(bytes.substr(22, 8)), nullptr, 16);
		check(length == payload.size() && bytes.substr(header_bytes, length) == payload, "snapshot lost its current owner facts");
		check(expected == checksum(bytes.substr(header_bytes, length)), "snapshot payload and envelope were torn");
		allocated[header_bytes + length / 2] ^= 1;
		check(expected != checksum(std::string_view(allocated.data() + header_bytes, length)), "a torn payload could refresh the heartbeat");
	}
}

PBD_TEST(PbdStallMetrics, BoundedConcurrentPublication)
{
	pbd::stall_metrics::queue<32> queue;
	std::atomic<unsigned> done{0}, accepted{0};
	std::array<std::thread, 4> writers;
	for (unsigned writer = 0; writer < writers.size(); ++writer)
		writers[writer] = std::thread([&, writer]
			{
				for (unsigned n = 0; n < 1000; ++n)
				{
					pbd::stall_metrics::record row;
					row.detail = writer * 1000 + n;
					row.bytes = row.detail + 17;
					if (queue.push(row))
						accepted.fetch_add(1);
				}
				done.fetch_add(1);
			});
	std::set<std::uint64_t> seen;
	bool intact = true;
	const auto consume = [&](const pbd::stall_metrics::record& row)
	{
		intact &= row.bytes == row.detail + 17;
		intact &= seen.insert(row.detail).second;
	};
	while (done.load() != writers.size())
		if (const auto row = queue.pop())
			consume(*row);
	for (auto& writer : writers)
		writer.join();
	while (const auto row = queue.pop())
		consume(*row);
	check(intact, "concurrent metric rows were torn or repeated");
	check(seen.size() == accepted.load() && seen.size() + queue.dropped() == 4000, "metric loss was not counted");
	for (unsigned repeat = 0; repeat < 10000; ++repeat)
	{
		check(queue.push({}), "drained metric storage could not be reused");
		check(queue.pop().has_value(), "reused metric slot was lost");
	}
}

PBD_TEST(PbdStallMetrics, ThreeHoursOfConcurrentFramesAndDelayedWriter)
{
	// Four emitting threads, two observations per frame, and a writer delayed for five seconds of frames.
	constexpr unsigned frames = 3 * 60 * 60 * 60, batch = 5 * 60, producers = 4, sites = 2;
	auto queue = std::make_unique<pbd::stall_metrics::queue<4096>>();
	std::barrier ready(producers + 1), drained(producers + 1);
	std::array<std::thread, producers> threads;
	std::array<std::uint64_t, producers> cost{};
	std::atomic<bool> intact{true};
	for (unsigned producer = 0; producer < producers; ++producer)
		threads[producer] = std::thread([&, producer]
			{
				for (unsigned first = 0; first < frames; first += batch)
				{
					const auto begin = std::chrono::steady_clock::now();
					for (unsigned frame = first; frame < first + batch; ++frame)
						for (unsigned site = 0; site < sites; ++site)
						{
							pbd::stall_metrics::record row;
							row.thread = producer;
							row.boundary = frame;
							row.detail = site;
							row.bytes = frame * sites + site + 17;
							if (!queue->push(row))
								intact.store(false);
						}
					cost[producer] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
					ready.arrive_and_wait();
					drained.arrive_and_wait();
				}
			});
	std::array<std::uint64_t, producers> next{};
	for (unsigned first = 0; first < frames; first += batch)
	{
		ready.arrive_and_wait();
		while (const auto row = queue->pop())
		{
			if (row->thread >= producers || row->boundary * sites + row->detail != next[row->thread]++ ||
				row->bytes != row->boundary * sites + row->detail + 17)
				intact.store(false);
		}
		drained.arrive_and_wait();
	}
	for (auto& thread : threads)
		thread.join();
	std::uint64_t total_cost = 0;
	for (unsigned producer = 0; producer < producers; ++producer)
	{
		intact.store(intact.load() && next[producer] == frames * sites);
		total_cost += cost[producer];
	}
	std::printf("trace publication: %u rows, %llu ns total, %llu ns/row, %llu dropped\n", frames * producers * sites,
		static_cast<unsigned long long>(total_cost), static_cast<unsigned long long>(total_cost / (frames * producers * sites)),
		static_cast<unsigned long long>(queue->dropped()));
	check(intact.load() && queue->dropped() == 0, "concurrent frame observations were dropped without exhausting the ring");
}

PBD_TEST(PbdStallMetrics, FullRingKeepsAllRetainedRowsAndCountsOnlyOverflow)
{
	pbd::stall_metrics::queue<4> queue;
	for (unsigned n = 0; n < 4; ++n)
	{
		pbd::stall_metrics::record row;
		row.detail = n;
		check(queue.push(row), "empty ring capacity was unavailable");
	}
	check(!queue.push({}) && queue.dropped() == 1, "overflow overwrote a retained observation");
	for (unsigned n = 0; n < 4; ++n)
	{
		const auto row = queue.pop();
		check(row && row->detail == n, "overflow changed retained frame order");
	}
	check(!queue.pop(), "drained ring retained an extra observation");
	check(queue.push({}) && queue.pop() && queue.dropped() == 1, "overflow permanently stalled a ring reservation");
}

PBD_TEST(PbdStallMetrics, TerminalDrainFinishesEveryReservedRowBeforeTheFooter)
{
	auto queue = std::make_unique<pbd::stall_metrics::queue<16384>>();
	for (unsigned n = 0; n < 10000; ++n)
	{
		pbd::stall_metrics::record row;
		row.boundary = n;
		check(queue->push(row), "terminal backlog did not fit the ring");
	}
	unsigned next = 0;
	const auto write = [&](const pbd::stall_metrics::record& row)
	{
		check(row.boundary == next++, "terminal drain omitted or repeated a frame");
	};
	check(queue->consume(write, 512) == 512, "ordinary writer batch did not respect its bound");
	queue->close();
	check(!queue->push({}), "closed output admitted an observation after its footer");
	check(queue->consume(write) == 9488 && next == 10000 && !queue->pop() && queue->dropped() == 0,
		"footer hid a backlog larger than the ordinary 8192-row batch");
	queue->restart();
	check(queue->push({}) && queue->pop() && queue->dropped() == 0, "resident restart inherited the preceding trace closure");
}

PBD_TEST(PbdStallMetrics, ResidentBootsRefreshEnablementAndOutputAttempt)
{
	pbd::stall_metrics::generation_flag enabled, output;
	unsigned settings_reads = 0, output_attempts = 0;
	for (std::uint64_t generation = 1; generation <= 300; ++generation)
	{
		const bool requested = generation % 3 != 1; // Practice/no trace, then two independently rooted matched boots.
		const bool writable = generation % 11 != 0; // A failed output is retried only after another resident boot.
		for (unsigned frame = 0; frame < 120; ++frame)
		{
			check(enabled.get(generation, [&]
					  {
						  ++settings_reads;
						  return requested;
					  }) == requested,
				"a resident boot inherited the preceding environment's enablement");
			check(output.get(generation, [&]
					  {
						  ++output_attempts;
						  return writable;
					  }) == writable,
				"a resident boot failed to retry its own output or reused a preceding result");
		}
	}
	check(settings_reads == 300 && output_attempts == 300, "measurement setup grew or repeated per frame");
}

PBD_TEST(PbdSessionHistory, Checks)
{
	using history = pbd::session_history::buffer<std::int32_t, 65536>;
	history arrivals;
	arrivals.fill(-1);
	const auto* allocation = &arrivals[0];
	for (std::uint32_t tick = 0; tick < 720000; ++tick)
	{
		check(arrivals[tick] == -1, "new input inherited an old arrival");
		arrivals[tick] = static_cast<std::int32_t>(tick);
		if (tick)
			check(arrivals[tick - 1] == static_cast<std::int32_t>(tick - 1), "previous input lost");
		check(arrivals.contains(tick), "current input identity missing");
		if (tick >= 65536)
			check(!arrivals.contains(tick - 65536), "expired input identity survived reuse");
	}
	check(!history::reclaimable(65536, 0), "writer-owned row was recyclable");
	check(history::reclaimable(65536, 1), "retired row was not recyclable");
	check(!history::reclaimable(70000, 4464), "writer retirement boundary is inclusive");
	check(history::reclaimable(70000, 4465), "writer retirement boundary was lost");
	check(!history::retained(0, 65536), "stale datagram was admitted");
	check(history::retained(1, 65536), "retained datagram was rejected");
	check(history::retained(0xffffffffu, 0xffffffffu), "retention arithmetic overflowed");
	arrivals.resize(72000);
	check(arrivals.size() == 72000, "logical extent changed with physical capacity");
	arrivals.resize(0x7fff0000u);
	check(arrivals.size() == 0x7fff0000u && &arrivals[0] == allocation, "continuous history grew its physical allocation");
	pbd::session_history::buffer<std::uint16_t, 16> inputs;
	inputs[0] = 123;
	const auto& published = inputs;
	check(published[0] == 123 && inputs.contains(0), "published read changed the row identity");
	check(inputs[16] == 0 && !inputs.contains(0), "new input retained an old controller mask");
	std::istringstream script("DR2026_UPDATE_TAPE 1\ncount 72000\n0 0 0\n65535 1 2\n70000 3 4\n71000 0 0\n");
	const auto input_sequence = pbd::parse_update_script(script);
	check(input_sequence.size() == 72000 && input_sequence[65535].p1 == 1 && input_sequence[70000].p2 == 4 &&
			  input_sequence[71000].p1 == 0,
		"input script did not preserve changes beyond 65535");
}

PBD_TEST(PbdSessionHistory, FiniteReplayRetainsEarlyAndLateRowsThroughTheContinuationTail)
{
	using history = pbd::session_history::buffer<pbd::update_inputs, 65536>;
	const auto extent = pbd::replay_standby::pbd_history_extent;
	std::istringstream script("DR2026_UPDATE_TAPE 1\ncount " + std::to_string(extent) +
							  "\n0 64 32\n65536 16 8\n99999 4 2\n100000 0 0\n");
	const auto tape = pbd::parse_update_script(script);
	history rows(0);
	rows.resize(tape.size(), true);
	for (std::uint32_t index = 0; index < tape.size(); ++index)
		rows[index] = tape[index];
	for (const auto index : {0u, 65535u, 65536u, 70000u, 99999u, 100000u, extent - 1})
	{
		check(rows.contains(index), "finite playback discarded a restorable input row");
		check(rows[index].p1 == tape[index].p1 && rows[index].p2 == tape[index].p2,
			"late playback overwrote an earlier recorded input");
	}
	rows[99999] = {};
	check(rows[70000].p1 == 16 && rows[70000].p2 == 8 && rows[0].p1 == 64,
		"replacing a late row changed an earlier retained input");
	rows.resize(tape.size());
	rows[0] = {1, 2};
	rows[65536] = {4, 8};
	check(!rows.contains(0) && rows[65536].p1 == 4, "online history stopped using its bounded ring");
}

PBD_TEST(PbdSessionTrace, SlowAndFailedStorageNeverOwnsReusedHistory)
{
	struct row
	{
		unsigned index;
		std::array<unsigned, 8> values;
	};
	pbd::session_trace::buffer<row, 2> trace;
	row original{1, {17}};
	check(trace.push(original) && trace.push({2, {23}}), "trace did not accept its bounded window");
	std::promise<void> entered, release;
	auto resume = release.get_future();
	bool intact = false;
	std::thread writer([&]
		{
			trace.consume([&](const row& captured)
				{
					entered.set_value();
					resume.wait();
					intact = captured.index == 1 && captured.values[0] == 17;
					return false; // The diagnostic file returned a short write.
				});
		});
	entered.get_future().wait();
	original.values[0] = 999;
	const bool admitted = trace.push({3, {31}});
	const bool bounded = !trace.push({4, {41}});
	release.set_value();
	writer.join();
	check(intact && admitted && bounded, "a stalled trace writer held or observed reused game storage");
	check(trace.disabled() && !trace.push({5, {51}}) && trace.dropped() == 2, "failed diagnostics did not disable themselves with counted loss");
	unsigned writes = 0;
	trace.consume([&](const row&)
		{
			++writes;
			return true;
		});
	check(!writes, "failed diagnostic output was retried on the game path");
}
