#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#endif

// The hosted game's keyboard, read as raw input by a message-only window on a thread of its own. The hosted child window's
// thread shares the launcher's input queue, so its key messages wait whenever the launcher's UI thread is busy; this
// thread's queue is its own. Each report becomes the key message the child window would have received, so the game sees
// the same keys, scan codes and repeats as from the window (docs/design/game-hosting.md).
namespace pbd::raw_keyboard
{
	// Win32 RAWKEYBOARD flags and key-message values.
	inline constexpr std::uint16_t key_break = 1, key_e0 = 2, key_e1 = 4;
	inline constexpr std::uint32_t wm_keydown = 0x100, wm_keyup = 0x101, wm_syskeydown = 0x104, wm_syskeyup = 0x105;
	inline constexpr std::uint16_t vk_shift = 0x10, vk_control = 0x11, vk_menu = 0x12, vk_pause = 0x13, vk_lwin = 0x5b,
								   vk_rwin = 0x5c, vk_f4 = 0x73, vk_f11 = 0x7a, vk_numlock = 0x90, vk_filler = 0xff;
	// A keyboard overrun, not a key.
	inline constexpr std::uint16_t overrun_make_code = 0xff;

	// One RAWKEYBOARD report.
	struct report
	{
		std::uint16_t make_code = 0, flags = 0, vkey = 0;
		std::uint32_t message = 0;
	};

	// Return and keypad Enter share a virtual key; raw scans keep their sampled levels separate.
	class return_keys
	{
		std::array<std::atomic<bool>, 2> m_down{};

	public:
		void observe(const report& key)
		{
			if (key.vkey == 0x0d && key.make_code == 0x1c && !(key.flags & key_e1))
				m_down[(key.flags & key_e0) != 0].store(!(key.flags & key_break), std::memory_order_relaxed);
		}
		void clear()
		{
			for (auto& down : m_down)
				down.store(false, std::memory_order_relaxed);
		}
		template <typename Query>
		bool pressed(std::string_view name, Query query) const
		{
			if (name == "Return" || name == "Num+Enter")
				return m_down[name == "Num+Enter"].load(std::memory_order_relaxed);
			return query(name);
		}
	};

	// A key message as the window procedure receives it: lparam carries the repeat count (1), the scan code in bits 16-23,
	// the extended-key flag (bit 24), Alt held (bit 29), the previous key state (bit 30) and the transition (bit 31).
	struct key_message
	{
		std::uint32_t message = 0;
		std::uint16_t vkey = 0;
		std::uint32_t lparam = 0;

		bool pressed() const
		{
			return message == wm_keydown || message == wm_syskeydown;
		}
		bool repeat() const
		{
			return pressed() && (lparam & (1u << 30));
		}
		bool extended() const
		{
			return (lparam & (1u << 24)) != 0;
		}
		// The scan code with the extended flag as 0x100, the form the keyboard pad handler names keys by.
		std::uint32_t scan() const
		{
			return ((lparam >> 16) & 0xff) | (extended() ? 0x100u : 0u);
		}
	};

	struct modifiers
	{
		bool shift = false, control = false, alt = false, meta = false;
	};

	class translator
	{
		// Physical scans include the E0 bit; keys sharing a virtual key retain independent press and release state.
		std::bitset<512> m_down;
		std::array<std::uint16_t, 512> m_vkey{};

		static key_message make(std::uint32_t message, std::uint16_t vkey, std::uint16_t scan, bool alt, bool previous)
		{
			const bool released = message == wm_keyup || message == wm_syskeyup;
			return {message, vkey,
				1u | (std::uint32_t{scan & 0xffu} << 16) | ((scan & 0x100u) ? 1u << 24 : 0u) | (alt ? 1u << 29 : 0u) |
					(previous ? 1u << 30 : 0u) | (released ? 1u << 31 : 0u)};
		}

		std::optional<key_message> translate(const report& key)
		{
			// Fillers of escaped sequences (the fake shifts around navigation keys, Pause's second half) and overruns.
			if (key.vkey >= vk_filler || key.make_code == overrun_make_code ||
				((key.flags & key_e0) && (key.make_code == 0x2a || key.make_code == 0x36)))
				return std::nullopt;
			const std::uint16_t vk = key.vkey;
			const bool pressed = !(key.flags & key_break);
			// Window messages name Pause by 0x45 (raw input sends E1 1D) and Num Lock by an extended 0x45.
			std::uint16_t scan = static_cast<std::uint16_t>((key.make_code & 0xff) | ((key.flags & key_e0) ? 0x100 : 0));
			if (vk == vk_pause)
				scan = 0x45;
			else if (vk == vk_numlock)
				scan = 0x145;
			// F11 and Alt+F4 stay with the game window's own messages (fullscreen and close): a press never comes here, nor
			// the release of a key this side did not press.
			if (pressed ? (vk == vk_f11 || (vk == vk_f4 && held().alt)) : ((vk == vk_f11 || vk == vk_f4) && !m_down[scan]))
				return std::nullopt;
			const bool previous = m_down[scan];
			m_down[scan] = pressed;
			if (pressed)
				m_vkey[scan] = vk;
			const bool alt = held().alt;
			std::uint32_t message = key.message;
			if (message != wm_keydown && message != wm_keyup && message != wm_syskeydown && message != wm_syskeyup)
				message = pressed ? (alt ? wm_syskeydown : wm_keydown) : (alt ? wm_syskeyup : wm_keyup);
			return make(message, vk, scan, alt, pressed ? previous : true);
		}

	public:
		// The messages for one report: its key message while the game has the keyboard (a press of a key already down is
		// a repeat), or, the first time a report comes while it does not, the release of every key still down.
		std::vector<key_message> input(const report& key, bool focused)
		{
			if (!focused)
				return release_all();
			std::vector<key_message> out;
			if (const auto message = translate(key))
				out.push_back(*message);
			return out;
		}

		// Releases every key still down, as the game would see them released: the focus left, or a secure desktop (Win+L,
		// a UAC prompt) swallowed the key-ups.
		std::vector<key_message> release_all()
		{
			std::vector<key_message> out;
			for (std::uint16_t scan = 0; scan < m_down.size(); ++scan)
				if (m_down[scan])
					out.push_back(make(wm_keyup, m_vkey[scan], scan, false, true));
			m_down.reset();
			return out;
		}

		modifiers held() const
		{
			return {m_down[0x2a] || m_down[0x36], m_down[0x1d] || m_down[0x11d],
				m_down[0x38] || m_down[0x138], m_down[0x15b] || m_down[0x15c]};
		}
	};

#ifdef _WIN32
	// The message-only window and its thread. It registers for the keyboard with RIDEV_INPUTSINK, so reports come whatever
	// window has the focus; `focused` decides whether they reach the game. Raw input registration is per process: if any
	// other window takes the keyboard registration, the sink notices within a poll, reports `lost` once and stays inactive,
	// and the game reads keys from its window messages again.
	class sink
	{
	public:
		using deliver_fn = std::function<void(const key_message&, const modifiers&)>;

		sink(deliver_fn deliver = {}, std::function<bool()> focused = {}, std::function<void()> lost = {})
			: m_deliver(std::move(deliver)), m_focused(std::move(focused)), m_lost(std::move(lost))
		{
			m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			std::promise<void> ready;
			auto started = ready.get_future();
			m_thread = std::jthread([this, ready = std::move(ready)]() mutable
				{
					run(ready);
				});
			started.wait();
		}
		~sink()
		{
			if (m_stop)
				SetEvent(m_stop);
			if (m_thread.joinable())
				m_thread.join();
			if (m_stop)
				CloseHandle(m_stop);
		}
		sink(const sink&) = delete;
		sink& operator=(const sink&) = delete;

		// The hosted window consumes messages; controller polling reads the same registration's physical Return levels.
		void route(deliver_fn deliver, std::function<bool()> focused, std::function<void()> lost)
		{
			std::lock_guard lock(m_route_mutex);
			this->deliver(m_keys.release_all());
			m_deliver = std::move(deliver);
			m_focused = std::move(focused);
			m_lost = std::move(lost);
		}
		const return_keys& returns() const
		{
			return m_returns;
		}

		// Registered, and still the process's keyboard target.
		bool active() const
		{
			return m_active.load();
		}
		HWND window() const
		{
			return m_window.load();
		}
		// Releases every held key on the sink's thread (the game window lost the focus).
		void release() const
		{
			if (const HWND target = m_window.load())
				PostMessageW(target, release_message, 0, 0);
		}

	private:
		static constexpr UINT release_message = WM_APP + 0x44;
		static constexpr DWORD poll_ms = 250;
		static constexpr const wchar_t* class_name = L"PbdRawKeyboard";

		deliver_fn m_deliver;
		std::function<bool()> m_focused;
		std::function<void()> m_lost;
		std::mutex m_route_mutex;
		translator m_keys;
		return_keys m_returns;
		HANDLE m_stop = nullptr;
		std::atomic<HWND> m_window{nullptr};
		std::atomic<bool> m_active{false};
		std::jthread m_thread;

		static bool registered_to(HWND target)
		{
			UINT count = 0;
			if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1) || !count)
				return false;
			std::vector<RAWINPUTDEVICE> devices(count);
			if (GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1))
				return false;
			for (const auto& device : devices)
				if (device.usUsagePage == 0x01 && device.usUsage == 0x06)
					return device.hwndTarget == target;
			return false;
		}

		void deliver(const std::vector<key_message>& messages)
		{
			if (m_deliver)
				for (const auto& message : messages)
					m_deliver(message, m_keys.held());
		}

		void on_input(LPARAM handle)
		{
			RAWINPUT input{};
			UINT size = sizeof(input);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(handle), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) ==
					static_cast<UINT>(-1) ||
				input.header.dwType != RIM_TYPEKEYBOARD || !m_active.load())
				return;
			const auto& key = input.data.keyboard;
			const report raw{key.MakeCode, key.Flags, key.VKey, key.Message};
			m_returns.observe(raw);
			std::lock_guard lock(m_route_mutex);
			deliver(m_keys.input(raw, m_focused && m_focused()));
		}

		static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
		{
			auto* self = reinterpret_cast<sink*>(GetWindowLongPtrW(window, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				self = static_cast<sink*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
				SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			}
			if (self && message == WM_INPUT)
				self->on_input(lparam);
			if (self && message == release_message)
			{
				self->m_returns.clear();
				std::lock_guard lock(self->m_route_mutex);
				self->deliver(self->m_keys.release_all());
				return 0;
			}
			// WM_INPUT also goes on, so the system frees the report.
			return DefWindowProcW(window, message, wparam, lparam);
		}

		// Checked every poll_ms, between messages; once taken, the held keys are released and the sink goes quiet.
		void check_registration(HWND window)
		{
			if (!m_active.load() || registered_to(window))
				return;
			m_active.store(false);
			m_returns.clear();
			std::lock_guard lock(m_route_mutex);
			deliver(m_keys.release_all());
			if (m_lost)
				m_lost();
		}

		void run(std::promise<void>& ready)
		{
			WNDCLASSW type{};
			type.lpfnWndProc = procedure;
			type.hInstance = GetModuleHandleW(nullptr);
			type.lpszClassName = class_name;
			RegisterClassW(&type);
			const HWND window = CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, this);
			const RAWINPUTDEVICE keyboard{0x01, 0x06, RIDEV_INPUTSINK, window};
			m_active.store(window && m_stop && RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)) && registered_to(window));
			m_window.store(window);
			ready.set_value();
			if (!window)
				return;
			MSG message{};
			ULONGLONG next_check = GetTickCount64() + poll_ms;
			while (MsgWaitForMultipleObjects(1, &m_stop, FALSE, poll_ms, QS_ALLINPUT) != WAIT_OBJECT_0)
			{
				while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
					DispatchMessageW(&message);
				if (GetTickCount64() >= next_check)
				{
					check_registration(window);
					next_check = GetTickCount64() + poll_ms;
				}
			}
			if (registered_to(window))
			{
				const RAWINPUTDEVICE remove{0x01, 0x06, RIDEV_REMOVE, nullptr};
				RegisterRawInputDevices(&remove, 1, sizeof(remove));
			}
			m_active.store(false);
			m_window.store(nullptr);
			DestroyWindow(window);
		}
	};

	// Raw keyboard registration is process-wide, shared by the hosted window and controller backend.
	inline std::shared_ptr<sink> shared_sink()
	{
		static std::mutex mutex;
		static std::weak_ptr<sink> current;
		std::lock_guard lock(mutex);
		auto value = current.lock();
		if (!value)
		{
			value = std::make_shared<sink>();
			current = value;
		}
		return value;
	}
#endif
} // namespace pbd::raw_keyboard
