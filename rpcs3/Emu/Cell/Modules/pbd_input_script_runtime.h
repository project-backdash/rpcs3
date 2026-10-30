#pragma once

#include "Emu/PBD/pbd_env.h"
#include "pbd_input_script.h"
#include "Emu/System.h"
#include "Utilities/File.h"
#include "util/logs.hpp"
#include "Emu/PBD/pbd_t5dr_memory.h"

#include <cstdlib>
#include <filesystem>

LOG_CHANNEL(pbd_input_script_log, "PBD input tape");

namespace pbd
{
	inline pad_input_script& get_input_script()
	{
		// Parse the tape once for the emulator lifetime.
		static pad_input_script input_sequence = []
		{
			try
			{
#ifdef _WIN32
				const wchar_t* requested = pbd::env::find_wide("PBD_INPUT_TAPE");
#else
				const char* requested = pbd::env::find("PBD_INPUT_TAPE");
#endif
				if (!requested || !*requested)
					return pad_input_script{};
				if (Emu.GetTitleID() != pbd::t5dr::title_id)
					throw input_script_error{"Diagnostic tape is restricted to NPUB30009"};
				const std::filesystem::path path(requested);
				if (!path.is_absolute())
					throw input_script_error{"PBD_INPUT_TAPE must be an absolute path"};
				const auto utf8 = path.u8string();
				fs::file file(std::string(utf8.begin(), utf8.end()), fs::read + fs::isfile);
				if (!file)
					throw input_script_error{"Input tape missing"};
				const auto size = file.size();
				if (size > pad_input_script::max_bytes)
					throw input_script_error{"Input tape exceeds 64 KiB"};
				std::string text(static_cast<std::size_t>(size), '\0');
				if (file.read(text.data(), text.size()) != text.size())
					throw input_script_error{"Short input tape read"};
				if (file.size() != size)
					throw input_script_error{"Input tape size changed during read"};
				auto result = pad_input_script::parse(text);
				pbd_input_script_log.notice("Diagnostic input tape enabled. Indices count successful pad polls, NOT simulation frames.");
				return result;
			}
			catch (const input_script_error& error)
			{
				pbd_input_script_log.error("Diagnostic input tape disabled: %s", error.message);
				return pad_input_script{};
			}
			catch (...)
			{
				pbd_input_script_log.error("Diagnostic input tape disabled after a file, path, or allocation failure.");
				return pad_input_script{};
			}
		}();
		return input_sequence;
	}
} // namespace pbd
