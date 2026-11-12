#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// What the RSX thread is doing about a shader the current draw needs, shared with the session's liveness watch and
// freeze probe without any renderer header. A draw whose compiled program and interpreter variant are both still
// building holds the RSX thread, and the game's main thread waits on the RSX thread, so ticks stop although nothing is
// stuck. The interpreter variants a game needs are recorded on disk and built again before play by later processes.
namespace pbd::shader_build
{
	enum class kind : std::uint32_t
	{
		none = 0,
		variant = 1, // the RSX thread builds an interpreter variant itself
		wait = 2,    // the RSX thread waits for a program building on another thread
	};

	struct facts
	{
		kind current = kind::none;
		std::uint64_t started_ns = 0; // steady clock; 0 while the RSX thread is not inside a build or wait
		std::uint64_t options = 0, state_hash = 0;
		std::uint32_t vertex_id = 0, fragment_id = 0; // the program pair the draw is waiting for
		std::uint32_t builds = 0;                     // builds and waits this process
		std::uint64_t longest_ns = 0, ended_ns = 0;   // ended_ns: steady clock end of the last one; 0 before the first
		std::uint32_t prebuild_queued = 0, prebuild_built = 0;
	};

	inline std::uint64_t steady_ns() noexcept
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// Single writer (the RSX thread) for the build fields; any thread may read. The end stamp is stored before the start
	// is cleared, so a reader that sees no build in progress also sees the end of the one that just finished.
	class live
	{
		std::atomic<std::uint64_t> m_started{0}, m_options{0}, m_state{0}, m_longest{0}, m_ended{0};
		std::atomic<std::uint32_t> m_kind{0}, m_vertex{0}, m_fragment{0}, m_builds{0};
		std::atomic<std::uint32_t> m_prebuild_queued{0}, m_prebuild_built{0};
		std::atomic<bool> m_fight{false};

	public:
		void begin(kind what, std::uint64_t options, std::uint64_t state_hash, std::uint32_t vertex_id, std::uint32_t fragment_id, std::uint64_t now) noexcept
		{
			m_kind.store(static_cast<std::uint32_t>(what), std::memory_order_relaxed);
			m_options.store(options, std::memory_order_relaxed);
			m_state.store(state_hash, std::memory_order_relaxed);
			m_vertex.store(vertex_id, std::memory_order_relaxed);
			m_fragment.store(fragment_id, std::memory_order_relaxed);
			m_started.store(std::max<std::uint64_t>(now, 1), std::memory_order_release);
		}

		void end(std::uint64_t now) noexcept
		{
			const std::uint64_t started = m_started.load(std::memory_order_relaxed);
			if (!started)
				return;
			const std::uint64_t took = now > started ? now - started : 0;
			if (took > m_longest.load(std::memory_order_relaxed))
				m_longest.store(took, std::memory_order_relaxed);
			m_builds.fetch_add(1, std::memory_order_relaxed);
			m_ended.store(std::max<std::uint64_t>(now, 1), std::memory_order_release);
			m_started.store(0, std::memory_order_release);
		}

		void prebuild_queued(std::uint32_t count) noexcept
		{
			m_prebuild_queued.store(count, std::memory_order_relaxed);
		}

		void prebuilt() noexcept
		{
			m_prebuild_built.fetch_add(1, std::memory_order_relaxed);
		}

		// Set once the session publishes its first battle boundary; prebuilding stops there.
		void mark_fight() noexcept
		{
			m_fight.store(true, std::memory_order_relaxed);
		}

		bool fight_started() const noexcept
		{
			return m_fight.load(std::memory_order_relaxed);
		}

		facts read() const noexcept
		{
			facts result;
			result.started_ns = m_started.load(std::memory_order_acquire);
			result.ended_ns = m_ended.load(std::memory_order_acquire);
			result.current = result.started_ns ? static_cast<kind>(m_kind.load(std::memory_order_relaxed)) : kind::none;
			result.options = m_options.load(std::memory_order_relaxed);
			result.state_hash = m_state.load(std::memory_order_relaxed);
			result.vertex_id = m_vertex.load(std::memory_order_relaxed);
			result.fragment_id = m_fragment.load(std::memory_order_relaxed);
			result.builds = m_builds.load(std::memory_order_relaxed);
			result.longest_ns = m_longest.load(std::memory_order_relaxed);
			result.prebuild_queued = m_prebuild_queued.load(std::memory_order_relaxed);
			result.prebuild_built = m_prebuild_built.load(std::memory_order_relaxed);
			return result;
		}
	};

	inline live& renderer() noexcept
	{
		static live state;
		return state;
	}

	// Marks the RSX thread inside a build or wait for its lifetime.
	class scope
	{
		live& m_state;

	public:
		scope(live& state, kind what, std::uint64_t options, std::uint64_t state_hash, std::uint32_t vertex_id = 0, std::uint32_t fragment_id = 0) noexcept
			: m_state(state)
		{
			m_state.begin(what, options, state_hash, vertex_id, fragment_id, steady_ns());
		}
		~scope()
		{
			m_state.end(steady_ns());
		}
		scope(const scope&) = delete;
		scope& operator=(const scope&) = delete;
	};

	inline const char* kind_text(kind what) noexcept
	{
		switch (what)
		{
		case kind::variant: return "interpreter variant build";
		case kind::wait: return "wait for a program building elsewhere";
		default: return "none";
		}
	}

	inline std::string prebuild_text(const facts& value)
	{
		return "interpreter variants prebuilt " + std::to_string(value.prebuild_built) + " of " + std::to_string(value.prebuild_queued);
	}

	// One line for the freeze probe and the stall stop.
	inline std::string describe(const facts& value, std::uint64_t now_ns)
	{
		const auto ms = [](std::uint64_t ns)
		{
			return std::to_string(ns / 1'000'000);
		};
		std::string text = "RSX shader build: ";
		if (value.current != kind::none)
		{
			char ids[96];
			std::snprintf(ids, sizeof(ids), "options 0x%04llx, state %016llx", static_cast<unsigned long long>(value.options), static_cast<unsigned long long>(value.state_hash));
			text += "inside " + std::string(kind_text(value.current)) + " for " + ms(now_ns > value.started_ns ? now_ns - value.started_ns : 0) + " ms (" + ids;
			if (value.current == kind::wait)
				text += ", vp " + std::to_string(value.vertex_id) + " fp " + std::to_string(value.fragment_id);
			text += ")";
		}
		else
		{
			text += "none";
			if (value.ended_ns)
				text += " (last ended " + ms(now_ns > value.ended_ns ? now_ns - value.ended_ns : 0) + " ms ago)";
		}
		text += ", builds " + std::to_string(value.builds) + ", longest " + ms(value.longest_ns) + " ms, " + prebuild_text(value);
		return text;
	}

	// A record names one interpreter variant a draw needed: compiler options and pipeline state. The renderer writes the
	// state bytes after this header; the name repeats the options and the state hash so a damaged file is skipped.
	struct record_header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t vendor_id;
		std::uint32_t device_id;
		std::uint64_t options;
		std::uint32_t state_bytes;
		std::uint32_t reserved;
	};
	static_assert(sizeof(record_header) == 32);

	inline constexpr std::uint32_t record_magic = 0x49444250; // "PBDI"
	inline constexpr std::uint32_t record_version = 1;
	// Records built per process. Each costs one driver compile of the interpreter (0.1 s to about 6 s on the slowest
	// drivers seen), spread over boot, standby and the VS load; prebuilding stops when the fight begins.
	inline constexpr std::size_t prebuild_limit = 64;

	inline std::uint64_t fnv1a64(const void* data, std::size_t size) noexcept
	{
		std::uint64_t hash = 0xcbf29ce484222325ull;
		const auto* bytes = static_cast<const unsigned char*>(data);
		for (std::size_t i = 0; i < size; ++i)
			hash = (hash ^ bytes[i]) * 0x100000001b3ull;
		return hash;
	}

	// 16 option bits, of which at most one alpha test comparison.
	inline bool options_valid(std::uint64_t options) noexcept
	{
		return options <= 0xffff && std::popcount(options & 0xfc00) <= 1;
	}

	inline std::string record_name(std::uint64_t options, std::uint64_t state_hash)
	{
		char name[48];
		std::snprintf(name, sizeof(name), "%04llx-%016llx.bin", static_cast<unsigned long long>(options), static_cast<unsigned long long>(state_hash));
		return name;
	}

	// The checks that need no renderer: size, magic, version, the same GPU, options, and a name matching the contents.
	inline bool header_valid(const record_header& header, std::uint64_t file_bytes, std::uint32_t vendor_id, std::uint32_t device_id,
		std::uint32_t state_bytes) noexcept
	{
		return file_bytes == sizeof(record_header) + state_bytes && header.magic == record_magic && header.version == record_version &&
		       header.vendor_id == vendor_id && header.device_id == device_id && header.state_bytes == state_bytes && options_valid(header.options);
	}

	inline bool record_valid(const std::string& name, const record_header& header, std::uint64_t file_bytes, std::uint32_t vendor_id,
		std::uint32_t device_id, const void* state, std::uint32_t state_bytes) noexcept
	{
		return header_valid(header, file_bytes, vendor_id, device_id, state_bytes) && name == record_name(header.options, fnv1a64(state, state_bytes));
	}

	struct record_file
	{
		std::string name;
		std::int64_t mtime = 0;
		std::uint64_t bytes = 0;
	};

	// The newest records of the expected size, newest first (ties by name), each offered to accept() until limit are
	// accepted. accept() reads and validates one record and keeps it.
	template <typename Accept>
	std::size_t take_newest(std::vector<record_file> files, std::uint64_t record_bytes, std::size_t limit, Accept&& accept)
	{
		std::erase_if(files, [&](const record_file& file)
			{
				return file.bytes != record_bytes || file.name.size() != record_name(0, 0).size() || !file.name.ends_with(".bin");
			});
		std::sort(files.begin(), files.end(), [](const record_file& a, const record_file& b)
			{
				return a.mtime != b.mtime ? a.mtime > b.mtime : a.name < b.name;
			});
		std::size_t accepted = 0;
		for (const auto& file : files)
		{
			if (accepted >= limit)
				break;
			if (accept(file.name))
				++accepted;
		}
		return accepted;
	}
} // namespace pbd::shader_build
