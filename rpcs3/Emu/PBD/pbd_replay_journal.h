#pragma once

#include "pbd_set_publication.h"
#include <cstdio>
#include <memory>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace pbd::replay
{
	// Semantic capture stays on the owner. Only this writer opens, appends, syncs and closes recording files.
	class journal
	{
		enum class kind
		{
			open,
			append,
			close
		};
		struct operation
		{
			kind type;
			std::filesystem::path path;
			std::string bytes;
			bool sync = false;
		};
		std::unique_ptr<std::FILE, int (*)(std::FILE*)> file{nullptr, &std::fclose};
		bool opened = false, refused = false;
		std::atomic<std::uint64_t> sequence{0};
		std::unique_ptr<pbd::set_channel::publication_writer<256, operation>> writer;

		bool sync() const noexcept
		{
			if (!file || std::fflush(file.get()) != 0)
				return false;
#ifdef _WIN32
			return _commit(_fileno(file.get())) == 0;
#else
			return fsync(fileno(file.get())) == 0;
#endif
		}
		pbd::set_channel::publication_result write(const operation& value)
		{
			if (value.type == kind::open)
			{
				if (file)
					return {false, false, "replay_journal_already_open"};
#ifdef _WIN32
				file.reset(_wfopen(value.path.c_str(), L"wbx"));
#else
				file.reset(std::fopen(value.path.c_str(), "wbx"));
#endif
				if (file)
					std::setvbuf(file.get(), nullptr, _IONBF, 0);
				return {static_cast<bool>(file), false, "replay_journal_open_failed"};
			}
			if (!file)
				return {false, false, "replay_journal_not_open"};
			if (value.type == kind::close)
			{
				const bool synced = sync();
				const bool closed = std::fclose(file.release()) == 0;
				return {synced && closed, false, "replay_journal_close_failed"};
			}
			if (std::fwrite(value.bytes.data(), 1, value.bytes.size(), file.get()) != value.bytes.size() ||
				(value.sync && !sync()))
				return {false, false, "replay_journal_write_failed"};
			return {true, false, nullptr};
		}
		bool submit(operation value)
		{
			const auto next = sequence.load(std::memory_order_relaxed) + 1;
			if (!healthy() || !writer->submit(next, std::move(value)))
			{
				refused = true;
				return false;
			}
			sequence.store(next, std::memory_order_release);
			return true;
		}

	public:
		void start()
		{
			writer = std::make_unique<pbd::set_channel::publication_writer<256, operation>>([this](std::uint64_t, const operation& value)
				{
					return write(value);
				});
		}
		bool healthy() const noexcept
		{
			return writer && !refused && !writer->error();
		}
		explicit operator bool() const noexcept
		{
			return opened;
		}
		bool open(const std::filesystem::path& path)
		{
			if (opened || !submit({kind::open, path, {}}))
				return false;
			opened = true;
			return true;
		}
		bool append(std::string bytes, bool flush)
		{
			return opened && bytes.size() <= 65536 && submit({kind::append, {}, std::move(bytes), flush});
		}
		bool close()
		{
			if (!opened)
				return true;
			opened = false;
			return submit({kind::close, {}, {}});
		}
		bool drain() const noexcept
		{
			return writer && writer->flush(sequence.load(std::memory_order_acquire));
		}
	};
} // namespace pbd::replay
