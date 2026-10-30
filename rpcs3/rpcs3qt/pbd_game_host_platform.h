#pragma once

// Private Windows contract and event conversion helpers for pbd_game_host.cpp.
namespace pbd::game_host
{
#ifdef _WIN32
	namespace
	{
		constexpr UINT host_message = WM_APP + 0x42;
		constexpr UINT control_message = WM_APP + 0x43;
		const int geometry_event = QEvent::registerEventType();

		struct handle
		{
			HANDLE value = nullptr;
			~handle()
			{
				if (value && value != INVALID_HANDLE_VALUE)
					CloseHandle(value);
			}
		};
		struct geometry_change : QEvent
		{
			QSize size;
			explicit geometry_change(QSize logical) : QEvent(static_cast<Type>(geometry_event)), size(logical) {}
		};
		bool foreground_within(HWND parent)
		{
			const auto foreground = GetForegroundWindow();
			return foreground && GetAncestor(foreground, GA_ROOT) == parent;
		}
		HWND read_contract(DWORD& pid, WPARAM& token, bool& hosted, bool& painted_return, bool& external_overlay)
		{
			const QByteArray bytes(pbd::env::find("PBD_GAME_HOST"));
			if (bytes.isEmpty())
				return nullptr;
			QJsonParseError error{};
			const auto object = QJsonDocument::fromJson(bytes, &error).object();
			bool valid_handle = false;
			const auto hwnd = object["hwnd"].toString().toULongLong(&valid_handle);
			const auto process = object["pid"].toInteger();
			const auto secret = object["token"].toInteger();
			const auto presentation = object["presentation"].toString("hosted");
			const auto mode = object["displayMode"].toString("windowed");
			if (error.error != QJsonParseError::NoError || object["schema"] != "pbd-game-host-1" || !valid_handle ||
				!hwnd || process <= 0 || process > MAXDWORD || secret <= 0 || secret > INT_MAX ||
				(presentation != "hosted" && presentation != "standalone") ||
				(mode != "fullscreen" && mode != "borderless" && mode != "windowed"))
				throw std::runtime_error("The PROJECT BACKDASH game window could not be opened. Restart the launcher.");
			pid = static_cast<DWORD>(process);
			token = static_cast<WPARAM>(secret);
			hosted = presentation == "hosted";
			painted_return = object["paintedReturn"].toBool(false);
			external_overlay = object["externalOverlay"].toBool(false) && hosted;
			const auto parent = reinterpret_cast<HWND>(hwnd);
			DWORD actual = 0, parent_session = 0, game_session = 0;
			GetWindowThreadProcessId(parent, &actual);
			if (actual != pid || pid == GetCurrentProcessId() || GetAncestor(parent, GA_ROOT) != parent ||
				!ProcessIdToSessionId(pid, &parent_session) ||
				!ProcessIdToSessionId(GetCurrentProcessId(), &game_session) || parent_session != game_session)
				throw std::runtime_error(
					"The PROJECT BACKDASH launcher window is no longer available. Restart the launcher.");
			return parent;
		}
		Qt::KeyboardModifiers modifiers()
		{
			Qt::KeyboardModifiers value{};
			if (GetKeyState(VK_SHIFT) & 0x8000)
				value |= Qt::ShiftModifier;
			if (GetKeyState(VK_CONTROL) & 0x8000)
				value |= Qt::ControlModifier;
			if (GetKeyState(VK_MENU) & 0x8000)
				value |= Qt::AltModifier;
			if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
				value |= Qt::MetaModifier;
			return value;
		}
		Qt::KeyboardModifiers modifiers(const raw_keyboard::modifiers& held)
		{
			return (held.shift ? Qt::ShiftModifier : Qt::NoModifier) | (held.control ? Qt::ControlModifier : Qt::NoModifier) |
			       (held.alt ? Qt::AltModifier : Qt::NoModifier) | (held.meta ? Qt::MetaModifier : Qt::NoModifier);
		}
		int qt_key(WPARAM key, LPARAM flags)
		{
			if (key >= '0' && key <= 'Z')
				return static_cast<int>(key);
			if (key >= VK_F1 && key <= VK_F24)
				return Qt::Key_F1 + static_cast<int>(key - VK_F1);
			if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
				return Qt::Key_0 + static_cast<int>(key - VK_NUMPAD0);
			switch (key)
			{
			case VK_RETURN: return flags & (1 << 24) ? Qt::Key_Enter : Qt::Key_Return;
			case VK_ESCAPE: return Qt::Key_Escape;
			case VK_BACK: return Qt::Key_Backspace;
			case VK_TAB: return Qt::Key_Tab;
			case VK_SPACE: return Qt::Key_Space;
			case VK_LEFT: return Qt::Key_Left;
			case VK_RIGHT: return Qt::Key_Right;
			case VK_UP: return Qt::Key_Up;
			case VK_DOWN: return Qt::Key_Down;
			case VK_HOME: return Qt::Key_Home;
			case VK_END: return Qt::Key_End;
			case VK_PRIOR: return Qt::Key_PageUp;
			case VK_NEXT: return Qt::Key_PageDown;
			case VK_INSERT: return Qt::Key_Insert;
			case VK_DELETE: return Qt::Key_Delete;
			case VK_SHIFT: return Qt::Key_Shift;
			case VK_CONTROL: return Qt::Key_Control;
			case VK_MENU: return Qt::Key_Alt;
			case VK_MULTIPLY: return Qt::Key_Asterisk;
			case VK_ADD: return Qt::Key_Plus;
			case VK_SUBTRACT: return Qt::Key_Minus;
			case VK_DECIMAL: return Qt::Key_Period;
			case VK_DIVIDE: return Qt::Key_Slash;
			default: return static_cast<int>(MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_CHAR) & 0x7fffffff);
			}
		}
	} // namespace

#endif
} // namespace pbd::game_host
