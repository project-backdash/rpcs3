#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"
#include "Emu/Io/pad_types.h"
#include "Emu/Io/pad_config.h"
#include "Emu/Io/pad_config_types.h"
#include "Input/mouse_gyro_handler.h"
#include "Emu/PBD/pbd_input_taps.h"
#include "Input/pbd_pad_session.h"
#include "Input/pbd_controller_ports.h"
#include "Utilities/mutex.h"

#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <string>

class PadHandlerBase;
namespace pbd::pads
{
	class controller_ports;
	class takeover_watch;
	class spectator_watch;
}

class pad_thread
{
public:
	pad_thread(void* curthread, void* curwindow, std::string_view title_id); // void * instead of QThread * and QWindow * because of include in emucore
	pad_thread(const pad_thread&) = delete;
	pad_thread& operator=(const pad_thread&) = delete;
	~pad_thread();

	void operator()();

	PadInfo& GetInfo() { return m_info; }
	std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM>& GetPads() { return m_pads; }
	void SetRumble(u32 pad, u8 large_motor, u8 small_motor);
	void SetIntercepted(bool intercepted);
	// PROJECT BACKDASH, called under g_pad_mutex: the match sides (Input/pbd_pad_session.h) and this PC's controller's taps
	// (Input/pbd_controller_ports.h).
	bool pbd_match_side_available() const noexcept;
	bool pbd_bind_match_side(u32 role) noexcept;
	u16 pbd_read_taps(pbd::input_taps::reader reader, const Pad& pad);
	void pbd_game_read(const Pad& pad);

	s32 AddLddPad();
	void UnregisterLddPad(u32 handle);

	void open_home_menu();

	std::map<pad_handler, std::shared_ptr<PadHandlerBase>>& get_handlers() { return m_handlers; }

	static std::shared_ptr<PadHandlerBase> GetHandler(pad_handler type);
	static void InitPadConfig(cfg_pad& cfg, pad_handler type, std::shared_ptr<PadHandlerBase>& handler);

	static auto constexpr thread_name = "Pad Thread"sv;

	mouse_gyro_handler& get_mouse_gyro() { return m_mouse_gyro; }

protected:
	void Init();
	void InitLddPad(u32 handle, const u32* port_status);

	// List of all handlers
	std::map<pad_handler, std::shared_ptr<PadHandlerBase>> m_handlers;

	// Used for pad_handler::keyboard
	void* m_curthread = nullptr;
	void* m_curwindow = nullptr;

	PadInfo m_info{ 0, 0, false };
	std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM> m_pads{};
	std::array<bool, CELL_PAD_MAX_PORT_NUM> m_pads_connected{};

	u32 num_ldd_pad = 0;

private:
	// PROJECT BACKDASH: its controllers, the match sides, controller takeover and a watcher's controllers; null when unused.
	std::unique_ptr<pbd::pads::controller_ports> m_pbd_ports;
	pbd::pads::match_sides m_pbd_match;
	std::unique_ptr<pbd::pads::takeover_watch> m_pbd_takeover;
	std::unique_ptr<pbd::pads::spectator_watch> m_pbd_spectator;
	pbd::pads::controller_ports::ports pbd_configure_ports(const std::function<bool(u32)>& is_virtual);
	void pbd_refresh_connection(u32 port, Pad& pad, pbd::pads::device_identity previous);
	void pbd_release_controllers();
	void pbd_open_home_menu();
	void pbd_create_controllers();
	void pbd_poll_controllers();
	void pbd_update_controllers();
	void pbd_note_controller_poll();
	void apply_copilots();
	void update_pad_states();

	u32 m_mask_start_press_to_resume = 0;
	u64 m_track_start_press_begin_timestamp = 0;
	bool m_resume_emulation_flag = false;
	bool m_ps_button_pressed = false;
	atomic_t<bool> m_home_menu_open = false;

	mouse_gyro_handler m_mouse_gyro;
};

namespace pad
{
	extern atomic_t<pad_thread*> g_pad_thread;
	extern shared_mutex g_pad_mutex;
	extern std::string g_title_id;
	extern atomic_t<bool> g_enabled;
	extern atomic_t<bool> g_reset;
	extern atomic_t<bool> g_started;
	extern atomic_t<bool> g_home_menu_requested;

	static inline class pad_thread* get_pad_thread(bool relaxed = false)
	{
		if (relaxed)
		{
			return g_pad_thread.observe();
		}

		return ensure(g_pad_thread.load());
	}

	static inline void set_enabled(bool enabled)
	{
		g_enabled = enabled;
	}

	static inline void reset(std::string_view title_id)
	{
		g_title_id = title_id;
		g_reset = true;
	}

	static inline void SetIntercepted(bool intercepted)
	{
		std::lock_guard lock(g_pad_mutex);
		const auto handler = get_pad_thread();
		handler->SetIntercepted(intercepted);
	}
}
