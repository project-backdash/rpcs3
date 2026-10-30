#include "pbd_test.h"

#include "rpcs3qt/pbd_raw_keyboard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using pbd::test::check;

namespace
{
	using namespace pbd::raw_keyboard;

	constexpr std::uint16_t vk_return = 0x0d, vk_left = 0x25, vk_home = 0x24, vk_a = 0x41, vk_numpad7 = 0x67,
							vk_divide = 0x6f;

	report down(std::uint16_t make_code, std::uint16_t vkey, std::uint16_t flags = 0, std::uint32_t message = wm_keydown)
	{
		return {make_code, flags, vkey, message};
	}

	report up(std::uint16_t make_code, std::uint16_t vkey, std::uint16_t flags = 0, std::uint32_t message = wm_keyup)
	{
		return {make_code, static_cast<std::uint16_t>(flags | key_break), vkey, message};
	}

	// The one message a focused report gives.
	key_message only(translator& keys, const report& key)
	{
		const auto out = keys.input(key, true);
		check(out.size() == 1, "one report is one key message");
		return out.front();
	}

	bool dropped(translator& keys, const report& key)
	{
		return keys.input(key, true).empty();
	}
} // namespace

PBD_TEST(PbdRawKeyboard, ExtendedKeysKeepTheirE0ScanCodes)
{
	translator keys;
	const auto right_ctrl = only(keys, down(0x1d, vk_control, key_e0));
	check(right_ctrl.pressed() && right_ctrl.extended() && right_ctrl.scan() == 0x11d && right_ctrl.vkey == vk_control);
	check(right_ctrl.lparam == (1u | (0x1du << 16) | (1u << 24)), "count 1, scan code, extended, first press");
	const auto released = only(keys, up(0x1d, vk_control, key_e0));
	check(!released.pressed() && released.scan() == 0x11d);
	check((released.lparam >> 30) == 3u, "a release has its previous state and transition bits set");
	check(only(keys, down(0x4b, vk_left, key_e0)).scan() == 0x14b, "the arrow keys are extended");
	const auto enter = only(keys, down(0x1c, vk_return, key_e0));
	check(enter.scan() == 0x11c && enter.extended() && !enter.repeat(), "numpad Enter is a fresh extended Enter");
	const auto main_enter = only(keys, down(0x1c, vk_return));
	check(!main_enter.extended() && !main_enter.repeat(), "main Enter is a distinct fresh press");
	check(only(keys, down(0x1c, vk_return, key_e0)).repeat(), "only the same physical Enter repeats");
	only(keys, up(0x1c, vk_return));
	const auto remaining = keys.release_all();
	check(remaining.size() == 2 && std::any_of(remaining.begin(), remaining.end(), [](const auto& key)
									   {
										   return key.vkey == vk_return && key.scan() == 0x11c && !key.pressed();
									   }),
		"releasing main Enter leaves keypad Enter held for focus-loss release");
	check(keys.release_all().empty(), "the remaining physical keys release once");
}

PBD_TEST(PbdRawKeyboard, NumpadKeysMatchTheWindowMessages)
{
	translator keys;
	check(only(keys, down(0x47, vk_numpad7)).scan() == 0x47, "Num Lock on: numpad 7");
	check(only(keys, down(0x47, vk_home)).scan() == 0x47, "Num Lock off: the same key reports Home, not extended");
	check(only(keys, down(0x47, vk_home, key_e0)).scan() == 0x147, "the Home key of the navigation block is extended");
	check(only(keys, down(0x35, vk_divide, key_e0)).scan() == 0x135, "numpad / is the extended 0x35");
	check(only(keys, down(0x45, vk_numlock)).scan() == 0x145, "Num Lock reports as an extended 0x45");
	const auto pause = only(keys, down(0x1d, vk_pause, key_e1));
	check(pause.vkey == vk_pause && pause.scan() == 0x45, "Pause arrives as E1 1D and is named 0x45");
}

PBD_TEST(PbdRawKeyboard, FillersAndOverrunsAreNotKeys)
{
	translator keys;
	check(dropped(keys, down(0x2a, vk_shift, key_e0)), "the fake shift around a navigation key");
	check(dropped(keys, up(0x36, vk_shift, key_e0)), "its right-hand twin");
	check(dropped(keys, down(0x45, vk_filler)), "the second half of an escaped sequence");
	check(dropped(keys, down(overrun_make_code, vk_a)), "a keyboard overrun");
	check(!keys.held().shift, "no filler leaves a key held");
}

PBD_TEST(PbdRawKeyboard, AHeldKeyRepeatsAndARepressIsAFreshPress)
{
	translator keys;
	check(!only(keys, down(0x1e, vk_a)).repeat());
	const auto again = only(keys, down(0x1e, vk_a));
	check(again.repeat() && again.pressed(), "the keyboard's own repeat of a held key");
	check(!only(keys, up(0x1e, vk_a)).pressed());
	check(!only(keys, down(0x1e, vk_a)).repeat(), "a press after the release is not a repeat");
}

PBD_TEST(PbdRawKeyboard, ModifiersComeFromTheKeysThisSideHolds)
{
	translator keys;
	only(keys, down(0x2a, vk_shift));
	check(keys.held().shift && !keys.held().control);
	only(keys, down(0x5b, vk_lwin, key_e0));
	check(keys.held().meta);
	const auto alt = only(keys, down(0x38, vk_menu, 0, 0));
	check(alt.message == wm_syskeydown && (alt.lparam & (1u << 29)), "Alt itself is a system key with Alt held");
	const auto alt_a = only(keys, down(0x1e, vk_a, 0, 0));
	check(alt_a.message == wm_syskeydown, "a key with Alt held is a system key when the report names no message");
	check(only(keys, down(0x1e, vk_a, 0, wm_keydown)).message == wm_keydown, "the report's own message wins");
	only(keys, up(0x2a, vk_shift));
	check(!keys.held().shift && keys.held().alt);
	keys.release_all();
	for (const auto& [vk, first, second] : {std::array<std::uint16_t, 3>{vk_shift, 0x2a, 0x36},
			 std::array<std::uint16_t, 3>{vk_control, 0x1d, 0x11d}, std::array<std::uint16_t, 3>{vk_menu, 0x38, 0x138}})
	{
		check(!only(keys, down(first & 0xff, vk)).repeat());
		check(!only(keys, down(second & 0xff, vk, second & 0x100 ? key_e0 : 0)).repeat(), "each modifier side presses independently");
		check(only(keys, down(first & 0xff, vk)).repeat(), "a repeat belongs to its physical modifier");
		only(keys, up(first & 0xff, vk));
		const auto held = keys.held();
		check(vk == vk_shift ? held.shift : vk == vk_control ? held.control :
															   held.alt,
			"the other modifier remains held");
		const auto released = keys.release_all();
		check(released.size() == 1 && released[0].scan() == second && released[0].vkey == vk,
			"focus loss releases the remaining modifier scan");
		check(keys.release_all().empty());
	}
}

PBD_TEST(PbdRawKeyboard, LosingTheFocusReleasesEveryHeldKeyOnce)
{
	translator keys;
	only(keys, down(0x2a, vk_shift));
	only(keys, down(0x1e, vk_a));
	only(keys, down(0x1d, vk_control, key_e0));
	const auto released = keys.input(down(0x30, 0x42), false);
	check(released.size() == 3, "the unfocused report itself does not reach the game");
	for (const auto& [vk, scan] : {std::array<std::uint16_t, 2>{vk_shift, 0x2a},
			 std::array<std::uint16_t, 2>{vk_control, 0x11d}, std::array<std::uint16_t, 2>{vk_a, 0x1e}})
		check(std::any_of(released.begin(), released.end(), [&](const auto& key)
				  {
					  return key.vkey == vk && key.scan() == scan;
				  }),
			"each key is released with its own virtual key and scan code");
	for (const auto& key : released)
		check(key.message == wm_keyup && (key.lparam >> 30) == 3u);
	check(!keys.held().shift && !keys.held().control);
	check(keys.input(down(0x30, 0x42), false).empty(), "nothing is released twice");
	check(!only(keys, down(0x1e, vk_a)).repeat(), "a key held across the focus change starts again as a press");
	check(keys.release_all().size() == 1, "a secure desktop's lost key-ups are released the same way");
	check(keys.release_all().empty());
}

PBD_TEST(PbdRawKeyboard, F11AndAltF4StayWithTheWindowMessages)
{
	translator keys;
	check(dropped(keys, down(0x57, vk_f11)) && dropped(keys, up(0x57, vk_f11)), "F11 toggles fullscreen there");
	only(keys, down(0x38, vk_menu));
	check(dropped(keys, down(0x3e, vk_f4)), "Alt+F4 closes the game there");
	check(dropped(keys, up(0x3e, vk_f4)), "and its release does not come here either");
	only(keys, up(0x38, vk_menu));
	check(only(keys, down(0x3e, vk_f4)).pressed(), "F4 alone is a game key");
	only(keys, down(0x38, vk_menu));
	check(!only(keys, up(0x3e, vk_f4)).pressed(), "its release comes here even with Alt down by then");
}

#ifdef _WIN32
namespace
{
	struct counters
	{
		std::atomic<int> keys{0}, lost{0};
	};

	sink make_sink(counters& seen)
	{
		return sink(
			[&seen](const key_message&, const modifiers&)
			{
				++seen.keys;
			},
			[]()
			{
				return false;
			},
			[&seen]()
			{
				++seen.lost;
			});
	}

	template <typename Done>
	bool eventually(Done done)
	{
		for (int i = 0; i < 500 && !done(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return done();
	}

	HWND keyboard_target()
	{
		RAWINPUTDEVICE devices[8]{};
		UINT count = 8;
		const UINT listed = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
		for (UINT i = 0; listed != static_cast<UINT>(-1) && i < listed; ++i)
			if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x06)
				return devices[i].hwndTarget;
		return nullptr;
	}
} // namespace

PBD_TEST(PbdRawKeyboard, TheSinkIsAMessageOnlyWindowOnAThreadOfItsOwn)
{
	counters seen;
	{
		sink keys(make_sink(seen));
		const HWND window = keys.window();
		check(window != nullptr);
		check(FindWindowExW(HWND_MESSAGE, nullptr, L"PbdRawKeyboard", nullptr) == window, "its parent is the message-only root");
		check(!IsWindowVisible(window));
		check(GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId(), "its queue is not this thread's");
		check(keys.active() && keyboard_target() == window, "it is the process's keyboard target");
		keys.release();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		check(seen.keys == 0, "with nothing held a release sends nothing");
	}
	check(keyboard_target() == nullptr, "closing the sink removes its registration");
	check(seen.lost == 0);
}

PBD_TEST(PbdRawKeyboard, AnotherKeyboardRegistrationSendsKeysBackToTheWindow)
{
	counters seen;
	sink keys(make_sink(seen));
	check(keys.active());
	const HWND other = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
	const RAWINPUTDEVICE keyboard{0x01, 0x06, RIDEV_INPUTSINK, other};
	check(other && RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)));
	check(eventually([&]()
			  {
				  return !keys.active();
			  }),
		"the sink notices within its poll");
	check(eventually([&]()
			  {
				  return seen.lost.load() != 0;
			  }),
		"the loss callback completes");
	check(seen.lost == 1, "and says so once");
	const RAWINPUTDEVICE remove{0x01, 0x06, RIDEV_REMOVE, nullptr};
	RegisterRawInputDevices(&remove, 1, sizeof(remove));
	DestroyWindow(other);
}

PBD_TEST(PbdRawKeyboard, SharedConsumersKeepOneKeyboardRegistration)
{
	auto controller = shared_sink();
	auto hosted = shared_sink();
	const HWND window = controller->window();
	check(controller == hosted && controller->active() && keyboard_target() == window);
	controller.reset();
	check(hosted->active() && keyboard_target() == window, "the hosted window keeps the shared keyboard alive");
	hosted->route({}, {}, {});
	check(hosted->active() && keyboard_target() == window, "removing delivery does not remove controller sampling");
	hosted.reset();
	check(keyboard_target() == nullptr, "the last consumer releases the keyboard registration");
}
#endif
