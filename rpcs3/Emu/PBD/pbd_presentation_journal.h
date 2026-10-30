#pragma once

#include "pbd_env.h"
#include "pbd_presentation_rows.h"
#include "util/logs.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

LOG_CHANNEL(pbd_presentation_log, "PBD Presentation");

// Appends game-presentation.jsonl in the run folder (pbd_presentation_rows.h). Reports come from the render thread (a
// new swap chain), the host window's thread (a fallback) and the keyboard's thread. Each one only queues its row under a
// short lock; a writer thread appends the queue, so no caller waits on the disk. Without a run folder nothing is written.
namespace pbd::presentation
{
	class journal
	{
	public:
		// One per process and never destroyed: a report may come from any thread until the process exits, and the writer
		// thread may still be appending then.
		static journal& get()
		{
			static journal* const instance = new journal(folder_from_environment());
			return *instance;
		}

		void hosted(bool value)
		{
			record([value](rows& state)
				{
					return state.hosted(value);
				});
		}

		void swapchain(bool flip_model, std::int32_t present_mode)
		{
			record([flip_model, present_mode](rows& state)
				{
					return state.swapchain(flip_model, present_mode);
				});
		}

		void keyboard(bool raw)
		{
			record([raw](rows& state)
				{
					return state.keyboard(raw);
				});
		}

	private:
		const std::filesystem::path m_file;
		std::mutex m_lock;
		rows m_rows;
		std::string m_queued;
		bool m_writing = false;
		// Touched only by the writer thread; one runs at a time.
		bool m_failed = false;

		explicit journal(const std::filesystem::path& folder)
			: m_file(folder.empty() ? folder : folder / file_name)
		{
		}

		static std::filesystem::path folder_from_environment()
		{
#ifdef _WIN32
			const wchar_t* pages = pbd::env::find_wide("PBD_TICK_PROBE_PAGES");
			const wchar_t* output = pbd::env::find_wide("PBD_TICK_PROBE_OUTPUT");
			return pages && output ? run_folder(pages, output) : std::filesystem::path();
#else
			// Sessions, and so run folders, exist only on Windows.
			return {};
#endif
		}

		template <typename Change>
		void record(Change change)
		{
			if (m_file.empty())
				return;
			{
				std::lock_guard lock(m_lock);
				const auto row = change(m_rows);
				if (!row)
					return;
				m_queued += *row;
				if (std::exchange(m_writing, true))
					return;
			}
			std::thread(&journal::drain, this).detach();
		}

		// Appends until the queue is empty, then lets the next report start a writer again.
		void drain()
		{
			for (std::string text;;)
			{
				{
					std::lock_guard lock(m_lock);
					if (m_queued.empty())
					{
						m_writing = false;
						return;
					}
					text = std::exchange(m_queued, {});
				}
				std::ofstream out(m_file, std::ios::binary | std::ios::app);
				out.write(text.data(), static_cast<std::streamsize>(text.size()));
				if (!out.flush() && !std::exchange(m_failed, true))
					pbd_presentation_log.warning("Could not append to game-presentation.jsonl in the run folder; the run "
												 "summary will not say how frames and keys travelled.");
			}
		}
	};
} // namespace pbd::presentation
