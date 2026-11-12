#pragma once

#include "pbd_practice_store_codec.h"

namespace pbd::practice_store
{
	inline std::filesystem::path profile_path(const std::filesystem::path& root)
	{
		return root / "profile.json";
	}
	inline std::filesystem::path recordings_path(const std::filesystem::path& root, u32 character)
	{
		return root / "recordings" / std::to_string(character) / "autosave.json";
	}

	// Renames an unreadable file to <name>.unreadable-<ms>[-<n>].json beside it, so a fresh start never overwrites it.
	inline bool set_aside(const std::filesystem::path& file)
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		std::error_code ec;
		for (u32 attempt = 0; attempt < 100; ++attempt)
		{
			auto aside = file.parent_path() / file.stem();
			aside += ".unreadable-" + std::to_string(ms) + (attempt ? "-" + std::to_string(attempt) : std::string());
			aside += file.extension();
			if (std::filesystem::exists(aside, ec))
				continue;
			std::filesystem::rename(file, aside, ec);
			return !ec;
		}
		return false;
	}

	// Writes the whole text beside the destination and renames it over, so a reader never sees half a file.
	inline bool commit_text(const std::filesystem::path& file, const std::string& text)
	{
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		auto temporary = file;
		temporary += ".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			out.flush();
			out.close();
			if (!out)
				return false;
		}
		std::filesystem::rename(temporary, file, ec);
		if (ec)
		{
			std::error_code cleanup;
			std::filesystem::remove(temporary, cleanup);
		}
		return !ec;
	}

	enum class load_state
	{
		absent,
		loaded,
		set_aside
	};
	template <typename T>
	struct loaded
	{
		load_state state = load_state::absent;
		T value{};
		std::string error;
	};

	// A missing file is a fresh start. One that cannot be read or parsed is set aside, and Practice starts fresh too.
	template <typename T, typename Parse>
	loaded<T> load_file(const std::filesystem::path& file, Parse parse, T fresh)
	{
		std::error_code ec;
		if (!std::filesystem::exists(file, ec))
			return {load_state::absent, std::move(fresh), {}};
		auto text = pbd::json::detail::read_text(file);
		auto parsed = text.value ? parse(*text.value) : pbd::json::result<T>{{}, text.error};
		if (parsed.value)
			return {load_state::loaded, std::move(*parsed.value), {}};
		set_aside(file);
		return {load_state::set_aside, std::move(fresh), parsed.error};
	}
	inline loaded<profile> load_profile(const std::filesystem::path& root)
	{
		return load_file<profile>(profile_path(root), parse_profile, profile{});
	}
	inline loaded<recordings> load_recordings(const std::filesystem::path& root, u32 character)
	{
		return load_file<recordings>(recordings_path(root, character), [character](std::string_view text)
			{
				return parse_recordings(text, character);
			},
			empty_recordings(character));
	}

	// Every dummy's saved slots, read once when Practice starts, so a new dummy's slots are ready at its first fight without a
	// read on the emulation thread. Unreadable files are set aside (their errors are returned for the log).
	struct library
	{
		std::map<u32, recordings> dummies;
		std::vector<std::string> set_aside;
	};
	inline library load_library(const std::filesystem::path& root)
	{
		library out;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(root / "recordings", ec))
		{
			const auto name = entry.path().filename().string();
			u32 character = 0;
			const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), character);
			if (error != std::errc{} || end != name.data() + name.size() || (name.size() > 1 && name[0] == '0') ||
				character >= base_characters || !entry.is_directory(ec))
				continue;
			auto loaded = load_recordings(root, character);
			if (loaded.state == load_state::loaded)
				out.dummies[character] = std::move(loaded.value);
			else if (loaded.state == load_state::set_aside)
				out.set_aside.push_back(std::to_string(character) + ": " + loaded.error);
		}
		return out;
	}

	// One background thread writes the store's files, so the emulation thread never waits on the disk. A file submitted again
	// before it was written keeps only its latest text; the text is made on this thread from a copy the caller hands over.
	class writer
	{
	public:
		using commit_function = std::function<bool(const std::filesystem::path&, const std::string&)>;

		explicit writer(commit_function commit = commit_text)
			: m_commit(std::move(commit)), m_thread([this]
											   {
												   run();
											   })
		{
		}
		writer(const writer&) = delete;
		writer& operator=(const writer&) = delete;
		// Whatever is still pending is written before the thread ends.
		~writer()
		{
			{
				std::lock_guard lock(m_mutex);
				m_stopping = true;
			}
			m_wake.notify_all();
			m_thread.join();
		}
		std::future<bool> submit(std::filesystem::path file, std::function<std::string()> text)
		{
			auto done = std::make_shared<std::promise<bool>>();
			auto result = done->get_future();
			{
				std::lock_guard lock(m_mutex);
				if (const auto old = m_pending.find(file); old != m_pending.end())
					old->second.done->set_value(false);
				m_pending[std::move(file)] = {std::move(text), std::move(done)};
			}
			m_wake.notify_all();
			return result;
		}
		// Waits until everything submitted so far is written.
		void wait_idle()
		{
			std::unique_lock lock(m_mutex);
			m_idle.wait(lock, [this]
				{
					return m_pending.empty() && !m_busy;
				});
		}

	private:
		struct pending_write
		{
			std::function<std::string()> text;
			std::shared_ptr<std::promise<bool>> done;
		};
		void run()
		{
			std::unique_lock lock(m_mutex);
			for (;;)
			{
				m_wake.wait(lock, [this]
					{
						return m_stopping || !m_pending.empty();
					});
				if (m_pending.empty())
					return;
				auto next = m_pending.extract(m_pending.begin());
				m_busy = true;
				lock.unlock();
				const bool ok = write_one(next.key(), next.mapped().text);
				next.mapped().done->set_value(ok);
				lock.lock();
				m_busy = false;
				m_idle.notify_all();
			}
		}
		bool write_one(const std::filesystem::path& file, const std::function<std::string()>& text) const
		{
			try
			{
				return m_commit(file, text());
			}
			catch (...)
			{
				return false;
			}
		}

		commit_function m_commit;
		mutable std::mutex m_mutex;
		std::condition_variable m_wake, m_idle;
		std::map<std::filesystem::path, pending_write> m_pending;
		bool m_stopping = false, m_busy = false;
		std::thread m_thread;
	};
} // namespace pbd::practice_store
