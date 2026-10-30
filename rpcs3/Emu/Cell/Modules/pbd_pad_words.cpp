#include "stdafx.h"
#include "Emu/Cell/pbd_rollback_session.h"
#include "Emu/Io/pad_types.h"
#include "Input/pad_thread.h"

#include <array>
#include <mutex>
#include <optional>

// Replay controls (pbd::rollback_session): this PC's controller as the game's 16-bit word (digital1 << 8 | digital2, the left stick
// folded into the directions as in the canonical record), read from the host pad state rather than through the game, so it still
// answers while the replay holds the game paused. The first connected real device on the two ports; 0 without one. A reader that
// reads once per frame also gets the buttons tapped since its previous read (pad_thread::pbd_read_taps). The caller holds
// pad::g_pad_mutex.
static u16 pbd_local_pad_word_locked(u32 ports = 3, std::optional<pbd::input_taps::reader> reader = {}) noexcept
{
	const auto handler = pad::get_pad_thread(true);
	if (!handler)
		return 0;
	const auto& pads = handler->GetPads();
	for (u32 port = 0; port < 2; port++)
	{
		if (!(ports & (u32{1} << port)))
			continue;
		const auto& pad = pads[port];
		if (!pad || pad->ldd || pad->is_fake_pad || !pad->is_connected() || pad->m_pad_handler == pad_handler::null)
			continue;
		u16 digital1 = 0, digital2 = 0;
		for (const ButtonExternal& button : pad->m_buttons_external)
		{
			if (!button.m_pressed)
				continue;
			if (button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1)
				digital1 |= static_cast<u16>(button.m_outKeyCode);
			else if (button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2)
				digital2 |= static_cast<u16>(button.m_outKeyCode);
		}
		for (const AnalogStickExternal& stick : pad->m_sticks_external)
		{
			if (stick.m_offset == CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X && stick.m_value < 64)
				digital1 |= CELL_PAD_CTRL_LEFT;
			if (stick.m_offset == CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X && stick.m_value > 192)
				digital1 |= CELL_PAD_CTRL_RIGHT;
			if (stick.m_offset == CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y && stick.m_value < 64)
				digital1 |= CELL_PAD_CTRL_UP;
			if (stick.m_offset == CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y && stick.m_value > 192)
				digital1 |= CELL_PAD_CTRL_DOWN;
		}
		const u16 word = static_cast<u16>((digital1 & 0xff) << 8 | (digital2 & 0xff));
		return reader ? static_cast<u16>(word | handler->pbd_read_taps(*reader, *pad)) : word;
	}
	return 0;
}

u16 pbd_local_pad_word() noexcept
{
	std::lock_guard lock(pad::g_pad_mutex);
	return pbd_local_pad_word_locked();
}

u16 pbd_local_pad_word(pbd::input_taps::reader reader) noexcept
{
	std::lock_guard lock(pad::g_pad_mutex);
	return pbd_local_pad_word_locked(3, reader);
}

std::array<u16, 2> pbd_replay_pad_words() noexcept
{
	std::lock_guard lock(pad::g_pad_mutex);
	constexpr auto replay = pbd::input_taps::reader::replay;
	return {pbd_local_pad_word_locked(1, replay), pbd_local_pad_word_locked(2, replay)};
}

// The in-game Settings menu reads the controller on the game window's GUI thread. The pad thread can hold the pad lock while it
// waits for that thread (a HID handler's Init runs there through a blocking call), so this read never waits for the lock: it
// answers nothing and the menu tries again on its next tick.
std::optional<u16> pbd_local_pad_word_try() noexcept
{
	std::unique_lock lock(pad::g_pad_mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return std::nullopt;
	return pbd_local_pad_word_locked();
}
