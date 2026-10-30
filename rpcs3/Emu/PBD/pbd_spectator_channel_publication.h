#pragma once

#include "pbd_spectator_local_channel.h"

namespace pbd::spectator
{
	inline void local_channel::draw_ui()
	{
		// Start preview expiry when the watcher UI can be drawn, preserving the full interval for messages
		// received under a cover or outside a fight.
		if (m_ui && m_ui->new_line && !m_preview_since && ui_shown())
			m_preview_since = std::max<u64>(1, spectator_ui::steady_ms());
		spectator_ui::view shown;
		if (m_ui)
		{
			shown.present = true;
			shown.shown = ui_shown();
			shown.bar = m_ui->bar;
			shown.game = m_game;
			shown.first_to = m_ui->first_to;
			shown.left_score = m_presentation.left_score;
			shown.right_score = m_presentation.right_score;
			shown.left_name = m_presentation.left_name;
			shown.right_name = m_presentation.right_name;
			shown.unread = m_ui->unread;
			shown.new_line = m_ui->new_line;
			shown.new_line_since_ms = m_ui->new_line ? m_preview_since : 0;
			shown.chat = m_ui->chat;
			shown.highlight = m_pad.highlight();
			shown.profiles = m_ui->profiles;
		}
		spectator_ui::publish(m_native_generation, std::move(shown));
	}

	inline bool local_channel::write_new(const std::filesystem::path& destination, const std::filesystem::path& temporary, const std::string& text)
	{
#ifdef _WIN32
		const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		// This private channel is recreated after a crash; publish complete bytes without blocking playback on disk durability.
		const bool wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
		const bool closed = CloseHandle(file) != 0;
		if (!wrote || !closed || !MoveFileExW(temporary.c_str(), destination.c_str(), 0))
		{
			DeleteFileW(temporary.c_str());
			return false;
		}
		return true;
#else
		std::error_code ec;
		if (std::filesystem::exists(temporary, ec) || ec)
			return false;
		std::ofstream file(temporary, std::ios::binary | std::ios::out);
		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		file.flush();
		file.close();
		if (!file)
			return false;
		std::filesystem::create_hard_link(temporary, destination, ec);
		const bool published = !ec;
		std::filesystem::remove(temporary, ec);
		return published;
#endif
	}

	inline bool local_channel::publish_pad(const spectator_ui::press& press)
	{
		if (m_pad_serial >= 65536)
			return false;
		value event = value::object({{"schema", value::string("pbd-spectator-pad-1")},
			{"bindingDigest", value::string(hex(m_identity.binding))}, {"generation", value::string(std::to_string(m_identity.generation))},
			{"serial", value::number(m_pad_serial + 1)}, {"button", value::string(std::string(spectator_ui::button_name(press.which)))},
			{"chatOpen", value::boolean(press.chat_open)}, {"shown", value::boolean(ui_shown())}, {"uiRevision", value::number(m_ui ? m_ui->revision : 0)}});
		if (press.quick_line)
			event.fields.emplace_back("quickLine", value::number(*press.quick_line));
		const auto text = pbd::set_channel::detail::serialize(event) + '\n';
		if (!write_new(m_directory / ("spectator-pad-" + std::to_string(m_pad_serial + 1) + ".json"),
				m_directory / (".spectator-pad-" + std::to_string(m_pad_serial + 1) + ".tmp"), text))
			return false;
		++m_pad_serial;
		return true;
	}

	inline bool local_channel::publish(std::string_view type, value payload)
	{
		using namespace pbd::set_channel;
		if (m_failed || m_report_serial >= 131072)
			return false;
		value event = value::object({{"schema", value::string("pbd-spectator-consumer-2")},
			{"bindingDigest", value::string(hex(m_identity.binding))}, {"generation", value::string(std::to_string(m_identity.generation))},
			{"nativeGeneration", value::string(std::to_string(m_native_generation))},
			{"gameNumber", value::number(m_game ? m_game : m_identity.pbd_capsule_game ? m_identity.pbd_capsule_game :
																						 1)},
			{"serial", value::number(m_report_serial + 1)}, {"type", value::string(std::string(type))}});
		for (auto& field : payload.fields)
			event.fields.push_back(std::move(field));
		const auto text = pbd::set_channel::detail::serialize(event) + '\n';
		if (text.size() > 4096)
			return false;
		const auto destination = m_directory / ("spectator-native-" + std::to_string(m_report_serial + 1) + ".json");
		const auto temporary = m_directory / (".spectator-native-" + std::to_string(m_report_serial + 1) + ".tmp");
		if (!write_new(destination, temporary, text))
			return false;
		++m_report_serial;
		return true;
	}

	inline bool local_channel::pbd_set_native_anchor(u32 boundary)
	{
		if (m_identity.pbd_capsule.empty() || boundary > maximum_boundary)
			return false;
		if (m_pbd_native_anchor && *m_pbd_native_anchor != boundary)
			return refuse("capsule_native_anchor_changed");
		if (m_pbd_native_anchor)
			return true;
		m_pbd_native_anchor = boundary;
		return publish("capsule_initialized", value::object({{"proof", value::string("native-capsule-owner-v1")},
												  {"capsuleGameNumber", value::number(m_identity.pbd_capsule_game)},
												  {"sourceBoundary", value::number(m_identity.pbd_source_origin)},
												  {"sourceSegment", value::number(m_identity.pbd_source_segment)},
												  {"nativeBoundary", value::number(boundary)}})) ||
		       refuse("report_publish_failed");
	}

	inline bool local_channel::refuse(std::string_view reason)
	{
		if (!m_failed)
			publish("refused", value::object({{"reason", value::string(std::string(reason))}, {"through", value::number(m_consumed)}}));
		m_reason = reason;
		m_failed = true;
		m_presentation.hidden = true;
		publish_presentation(m_native_generation, m_presentation);
		draw_ui();
		return false;
	}

	inline bool local_channel::close(std::string_view reason)
	{
		if (!healthy())
			return false;
		publish("stopped", value::object({{"reason", value::string(std::string(reason))}, {"through", value::number(m_consumed)}}));
		m_reason = reason;
		m_stopped = true;
		m_presentation.hidden = true;
		publish_presentation(m_native_generation, m_presentation);
		draw_ui();
		return false;
	}

	inline void local_channel::read_pad(std::uint16_t word, bool menu)
	{
		if (!m_prepared || !healthy())
			return;
		if (menu != m_menu)
		{
			m_menu = menu;
			draw_ui();
		}
		const u32 before = m_pad.highlight();
		const auto presses = m_pad.read(word, m_ui ? &*m_ui : nullptr, ui_shown(), menu);
		if (m_pad.highlight() != before)
			draw_ui();
		for (const auto& press : presses)
			publish_pad(press);
	}

	inline bool local_channel::prepare(u64 native_generation, u32 owner, u32 boundary, u32 counter, std::string_view checkpoint, std::string_view emulator)
	{
		if (!healthy() || m_prepared || !native_generation || owner != pbd::t5dr::main_ppu_id || boundary != 0 ||
			(m_identity.pbd_capsule.empty() && counter != (0x80000000u | (m_identity.seed & 0x00ffffffu))) ||
			checkpoint != m_identity.checkpoint || emulator != m_identity.emulator)
			return refuse("clean_start_unverified");
		m_native_generation = native_generation;
		m_prepared = true;
		publish_presentation(native_generation, m_presentation);
		if (!publish("prepared", value::object({{"proof", value::string(m_identity.pbd_capsule.empty() ? "clean-cs-owner-v1" : "capsule-clean-owner-v1")}, {"owner", value::number(owner)},
									 {"cia", value::number(pbd::t5dr::boundary_pc)}, {"origin", value::number(boundary)}, {"through", value::number(0)},
									 {"checkpointSha256", value::string(std::string(checkpoint))}, {"emulatorSha256", value::string(std::string(emulator))},
									 {"recipeSha256", value::string(m_identity.recipe)}, {"seed", value::number(m_identity.seed)}, {"counter", value::number(counter)},
									 {"titleId", value::string(m_identity.title)}, {"scene", value::number(3)}, {"subscene", value::number(44)}})))
			return refuse("report_publish_failed");
		return true;
	}

} // namespace pbd::spectator
