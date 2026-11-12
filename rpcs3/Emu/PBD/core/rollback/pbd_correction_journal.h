#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

// tick-correction.jsonl. The owner thread copies each finished row into a fixed ring and returns; only a worker thread
// touches the disk, so a stalled write can never hold a boundary or a correction. A row that does not fit is dropped
// whole and counted, never waited for, so the file holds complete lines only. Accepted rows reach the file byte for
// byte and in order. Nothing reads this file back during a session.
namespace pbd::correction_journal
{
	// The row kinds that drops are counted by.
	enum class kind : std::uint8_t
	{
		predicted_update,
		corrected_update,
		correction,
		correction_end,
		fault,
		host_timing,
		undo_policy,
	};
	inline constexpr std::size_t kind_count = 7;
	inline constexpr std::array<const char*, kind_count> kind_names{"predicted_update", "corrected_update", "correction", "correction_end",
		"fault", "host_timing", "undo_policy"};

	// The ring holds about a minute of rows at the heaviest rate seen (0.6 to 1 MiB per minute). A larger ring would sit
	// untouched for minutes between wraps, long enough for Windows to page it out under memory pressure; the owner's next
	// copy would then wait on the pagefile.
	inline constexpr std::size_t default_capacity = 1024 * 1024;

	// Worker write times above these are counted: 1 ms, 5 ms, one 60 Hz frame, 50 ms and 100 ms.
	inline constexpr std::array<std::uint64_t, 5> slow_write_ns{1'000'000, 5'000'000, 16'700'000, 50'000'000, 100'000'000};
	inline constexpr std::array<const char*, 5> slow_write_names{"1", "5", "16.7", "50", "100"};

	// The session end asks for the summary and waits this long for it before reporting the end.
	inline constexpr std::chrono::milliseconds end_wait{100};
	// Teardown waits this long for the final drain, then cancels a stalled write up to twice, waiting cancel_wait after
	// each, and then leaves a still stalled writer behind instead of holding the game's exit.
	inline constexpr std::chrono::milliseconds close_wait{100};
	inline constexpr std::chrono::milliseconds cancel_wait{50};

	struct totals
	{
		std::uint64_t rows_written = 0, bytes_written = 0, rows_dropped = 0, bytes_dropped = 0, rows_unwritten = 0;
		std::array<std::uint64_t, kind_count> dropped_by_kind{};
		std::uint64_t capacity = 0, high_water = 0, enqueue_max_ns = 0, write_max_ns = 0;
		std::array<std::uint64_t, slow_write_ns.size()> slow_writes{};
		bool write_failed = false;
	};

	// The closing row. rows_* count newline-terminated rows, so a correction_end record (its correction_clock row and
	// the correction_end row) counts two. Drops per kind are flat dropped_<kind> keys, so no row type name appears quoted
	// here and a tool that picks rows by a quoted type name never takes this row for one of them.
	inline std::string summary_row(const totals& t)
	{
		const auto n = [](std::uint64_t value)
		{
			return static_cast<unsigned long long>(value);
		};
		char text[1024];
		int at = std::snprintf(text, sizeof(text),
			"{\"type\":\"correction_journal_end\",\"rows_written\":%llu,\"bytes_written\":%llu,\"rows_dropped\":%llu,\"bytes_dropped\":%llu",
			n(t.rows_written), n(t.bytes_written), n(t.rows_dropped), n(t.bytes_dropped));
		for (std::size_t k = 0; k < kind_count; ++k)
			at += std::snprintf(text + at, sizeof(text) - at, ",\"dropped_%s\":%llu", kind_names[k], n(t.dropped_by_kind[k]));
		at += std::snprintf(text + at, sizeof(text) - at,
			",\"rows_unwritten\":%llu,\"queue_capacity_bytes\":%llu,\"queue_high_water_bytes\":%llu,\"owner_enqueue_max_ns\":%llu,\"worker_write_max_ns\":%llu,\"worker_writes_over_ms\":{",
			n(t.rows_unwritten), n(t.capacity), n(t.high_water), n(t.enqueue_max_ns), n(t.write_max_ns));
		for (std::size_t i = 0; i < slow_write_ns.size(); ++i)
			at += std::snprintf(text + at, sizeof(text) - at, "%s\"%s\":%llu", i ? "," : "", slow_write_names[i], n(t.slow_writes[i]));
		at += std::snprintf(text + at, sizeof(text) - at, "},\"write_failed\":%s}\n", t.write_failed ? "true" : "false");
		return std::string(text, static_cast<std::size_t>(at));
	}

	inline std::uint64_t count_rows(const char* bytes, std::size_t size) noexcept
	{
		return static_cast<std::uint64_t>(std::count(bytes, bytes + size, '\n'));
	}

	// The journal's file, written through the OS handle alone. A writer left stalled in a write at teardown then holds no
	// C runtime stream lock, which the process's normal exit would wait on while it flushes and closes every open stream.
	class os_file
	{
#ifdef _WIN32
		HANDLE handle = INVALID_HANDLE_VALUE;
#else
		int descriptor = -1;
#endif

	public:
		os_file() = default;
		os_file(const os_file&) = delete;
		os_file& operator=(const os_file&) = delete;
		~os_file()
		{
#ifdef _WIN32
			if (handle != INVALID_HANDLE_VALUE)
				CloseHandle(handle);
#else
			if (descriptor >= 0)
				::close(descriptor);
#endif
		}

		// Creates the file; an existing one is never opened or truncated. Readers may open it while it is written.
		bool open(const std::string& path)
		{
#ifdef _WIN32
			const std::filesystem::path name(std::u8string(path.begin(), path.end()));
			handle = CreateFileW(name.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			return handle != INVALID_HANDLE_VALUE;
#else
			descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
			return descriptor >= 0;
#endif
		}

		// The bytes written before a failure or a cancelled write (CancelSynchronousIo).
		std::size_t write(const void* data, std::size_t size) noexcept
		{
			const auto* bytes = static_cast<const char*>(data);
			std::size_t done = 0;
			while (done < size)
			{
#ifdef _WIN32
				DWORD wrote = 0;
				const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, std::size_t{1} << 30));
				if (!WriteFile(handle, bytes + done, chunk, &wrote, nullptr) || !wrote)
					break;
#else
				const auto wrote = ::write(descriptor, bytes + done, size - done);
				if (wrote < 0 && errno == EINTR)
					continue;
				if (wrote <= 0)
					break;
#endif
				done += static_cast<std::size_t>(wrote);
			}
			return done;
		}
	};

	// One producer at a time: the setup thread before play, then the owner thread, then teardown after the owner has
	// joined. finish() may run beside the producer; close() runs only after the last row.
	template <typename File = os_file, std::size_t Capacity = default_capacity>
	class sink
	{
		static_assert(Capacity > 0);
		using clock = std::chrono::steady_clock;

		// Owned jointly by the sink and its worker, so a worker left stalled in the OS at teardown never outlives its
		// queue or file.
		struct shared
		{
			File output;
			// Zeroed when the sink opens, so the owner's copies never take a first-touch page fault.
			std::array<char, Capacity> bytes{};
			alignas(64) std::atomic<std::uint64_t> written{0};
			alignas(64) std::atomic<std::uint64_t> read{0};
			// Producer counters. With one producer a load and a store replace a locked add.
			alignas(64) std::atomic<std::uint64_t> offered{0};
			std::atomic<std::uint64_t> bytes_dropped{0}, high_water{0}, enqueue_max_ns{0};
			std::array<std::atomic<std::uint64_t>, kind_count> dropped_by_kind{};
			std::atomic<bool> failed{false}, end{false}, stopping{false}, aborting{false};
			// Worker only.
			std::uint64_t rows_written = 0, bytes_written = 0, rows_unwritten = 0, write_max_ns = 0;
			std::array<std::uint64_t, slow_write_ns.size()> slow_writes{};
			bool line_open = false;
			// Under mutex.
			std::mutex mutex;
			std::condition_variable wake, changed;
			bool woken = false, exited = false, summarized = false;
			std::uint64_t summary_offered = 0;
		};
		std::shared_ptr<shared> state;
		std::atomic<bool> accepting{false};
		std::thread worker;
		std::function<void(std::thread&)> cancel;

		static void add(std::atomic<std::uint64_t>& counter, std::uint64_t value) noexcept
		{
			counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
		}
		static void raise(std::atomic<std::uint64_t>& counter, std::uint64_t value) noexcept
		{
			if (value > counter.load(std::memory_order_relaxed))
				counter.store(value, std::memory_order_relaxed);
		}

		static std::size_t put(shared& s, const char* bytes, std::size_t size) noexcept
		{
			std::size_t done = 0;
			try
			{
				const auto begin = clock::now();
				done = s.output.write(bytes, size);
				const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - begin).count());
				s.write_max_ns = std::max(s.write_max_ns, ns);
				for (std::size_t i = 0; i < slow_write_ns.size(); ++i)
					s.slow_writes[i] += ns > slow_write_ns[i];
			}
			catch (...)
			{
				done = 0;
			}
			done = std::min(done, size);
			if (done)
				s.line_open = bytes[done - 1] != '\n';
			return done;
		}

		static void drain(shared& s) noexcept
		{
			const auto stop = s.written.load(std::memory_order_acquire);
			auto next = s.read.load(std::memory_order_relaxed);
			while (next < stop && !s.aborting.load(std::memory_order_acquire))
			{
				const auto offset = static_cast<std::size_t>(next % Capacity);
				const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(stop - next, Capacity - offset));
				const char* bytes = s.bytes.data() + offset;
				std::size_t done = 0;
				if (!s.failed.load(std::memory_order_relaxed))
				{
					done = put(s, bytes, size);
					s.rows_written += count_rows(bytes, done);
					s.bytes_written += done;
					if (done != size)
						s.failed.store(true, std::memory_order_relaxed);
				}
				// After a failed write the queued rows are released unwritten, so the owner keeps dropping instead of filling.
				s.rows_unwritten += count_rows(bytes + done, size - done);
				next += size;
				s.read.store(next, std::memory_order_release);
			}
		}

		static void write_summary(shared& s) noexcept
		{
			if (s.aborting.load(std::memory_order_acquire))
				return;
			totals t;
			t.rows_written = s.rows_written;
			t.bytes_written = s.bytes_written;
			t.bytes_dropped = s.bytes_dropped.load(std::memory_order_relaxed);
			for (std::size_t k = 0; k < kind_count; ++k)
				t.rows_dropped += t.dropped_by_kind[k] = s.dropped_by_kind[k].load(std::memory_order_relaxed);
			t.rows_unwritten = s.rows_unwritten;
			t.capacity = Capacity;
			t.high_water = s.high_water.load(std::memory_order_relaxed);
			t.enqueue_max_ns = s.enqueue_max_ns.load(std::memory_order_relaxed);
			t.write_max_ns = s.write_max_ns;
			t.slow_writes = s.slow_writes;
			t.write_failed = s.failed.load(std::memory_order_relaxed);
			try
			{
				// A write cut short mid-row leaves an open line; the summary still starts a line of its own.
				const auto row = (s.line_open ? "\n" : "") + summary_row(t);
				if (put(s, row.data(), row.size()) != row.size())
					s.failed.store(true, std::memory_order_relaxed);
			}
			catch (...)
			{
			}
		}

		// Drains every few milliseconds. Once the end is asked for, each drain that leaves rows offered since the last
		// summary writes a new one, so the file always ends with a summary of everything before it.
		static void run(const std::shared_ptr<shared>& s) noexcept
		{
			for (;;)
			{
				const bool stop = s->stopping.load(std::memory_order_acquire);
				const bool end = stop || s->end.load(std::memory_order_acquire);
				const auto offered = s->offered.load(std::memory_order_acquire);
				drain(*s);
				const bool summarize = end && (!s->summarized || offered != s->summary_offered);
				if (summarize)
					write_summary(*s);
				if (summarize || stop)
				{
					{
						std::lock_guard lock(s->mutex);
						if (summarize)
						{
							s->summarized = true;
							s->summary_offered = offered;
						}
						s->exited = stop;
					}
					s->changed.notify_all();
				}
				if (stop)
					return;
				std::unique_lock lock(s->mutex);
				s->wake.wait_for(lock, std::chrono::milliseconds(5), [&]
					{
						return s->woken;
					});
				s->woken = false;
			}
		}

		void wake_worker() noexcept
		{
			{
				std::lock_guard lock(state->mutex);
				state->woken = true;
			}
			state->wake.notify_one();
		}

	public:
		sink() = default;
		sink(const sink&) = delete;
		sink& operator=(const sink&) = delete;
		~sink()
		{
			close();
		}

		// Opens the file and its queue before play; start() then runs the worker.
		template <typename... Args>
		bool open(Args&&... args)
		{
			close();
			auto opened = std::make_shared<shared>();
			if (!opened->output.open(std::forward<Args>(args)...))
				return false;
			state = std::move(opened);
			accepting.store(true, std::memory_order_release);
			return true;
		}

		// cancel_write unblocks a write stalled in the OS (CancelSynchronousIo on Windows) when teardown has waited for it.
		void start(std::function<void(std::thread&)> cancel_write = {})
		{
			if (!state || worker.joinable())
				return;
			cancel = std::move(cancel_write);
			worker = std::thread([s = state]
				{
					run(s);
				});
		}

		explicit operator bool() const noexcept
		{
			return accepting.load(std::memory_order_acquire);
		}

		// Owner thread: copies the row and returns. It never waits, allocates or enters the OS; false = dropped and counted.
		bool write(kind type, const void* data, std::size_t size) noexcept
		{
			if (!accepting.load(std::memory_order_relaxed) || !size)
				return false;
			shared& s = *state;
			const auto begin = clock::now();
			const auto next = s.written.load(std::memory_order_relaxed);
			const auto queued = next - s.read.load(std::memory_order_acquire);
			const bool accepted = !s.failed.load(std::memory_order_relaxed) && size <= Capacity - queued;
			if (accepted)
			{
				const auto offset = static_cast<std::size_t>(next % Capacity);
				const auto first = std::min(size, Capacity - offset);
				std::memcpy(s.bytes.data() + offset, data, first);
				if (size > first)
					std::memcpy(s.bytes.data(), static_cast<const char*>(data) + first, size - first);
				s.written.store(next + size, std::memory_order_release);
				raise(s.high_water, queued + size);
			}
			else
			{
				add(s.dropped_by_kind[static_cast<std::size_t>(type)], std::max<std::uint64_t>(1, count_rows(static_cast<const char*>(data), size)));
				add(s.bytes_dropped, size);
			}
			s.offered.store(s.offered.load(std::memory_order_relaxed) + 1, std::memory_order_release);
			raise(s.enqueue_max_ns, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - begin).count()));
			return accepted;
		}

		// Asks for the summary row and waits up to `limit` until it covers every row offered so far. The end of a session
		// calls this before it reports the end, so a host that ends the process then still finds the journal complete.
		bool finish(std::chrono::milliseconds limit = end_wait) noexcept
		{
			if (!state || !worker.joinable())
				return false;
			const auto target = state->offered.load(std::memory_order_acquire);
			state->end.store(true, std::memory_order_release);
			wake_worker();
			std::unique_lock lock(state->mutex);
			return state->changed.wait_for(lock, limit, [&]
				{
					return state->exited || (state->summarized && state->summary_offered >= target);
				});
		}

		// After the last producer: drains, writes the summary and closes the file, waiting a bounded time for a stalled disk.
		void close() noexcept
		{
			accepting.store(false, std::memory_order_release);
			if (!worker.joinable())
			{
				state.reset();
				return;
			}
			state->end.store(true, std::memory_order_release);
			state->stopping.store(true, std::memory_order_release);
			wake_worker();
			const auto exited = [&](std::chrono::milliseconds limit)
			{
				std::unique_lock lock(state->mutex);
				return state->changed.wait_for(lock, limit, [&]
					{
						return state->exited;
					});
			};
			bool joined = exited(close_wait);
			for (int attempt = 0; !joined && attempt < 2; ++attempt)
			{
				state->aborting.store(true, std::memory_order_release);
				if (cancel)
					cancel(worker);
				joined = exited(cancel_wait);
			}
			if (joined)
				worker.join();
			else
				worker.detach();
			state.reset();
		}
	};
} // namespace pbd::correction_journal
