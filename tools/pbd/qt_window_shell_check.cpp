#include "../../rpcs3/rpcs3qt/pbd_qt_shell.h"

#include <QApplication>
#include <QMessageBox>

#include <iostream>
#include <stdexcept>

namespace
{
	void require(bool value, const char* message)
	{
		if (!value)
			throw std::runtime_error(message);
	}

	HWND native(QWindow& window)
	{
		return reinterpret_cast<HWND>(window.winId());
	}

	void excluded(QWindow& window)
	{
		const auto handle = native(window);
		const auto style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
		require(style & WS_EX_TOOLWINDOW, "Qt window must be a tool window");
		require(!(style & WS_EX_APPWINDOW), "Qt window must not force a shell entry");
		require(!IsWindowVisible(handle), "test windows must stay hidden");
	}
} // namespace

int main(int argc, char** argv)
{
	try
	{
		QApplication app(argc, argv);
		{
			pbd::game_host::qt_shell_filter policy(app);
			for (int cycle = 0; cycle < 30; ++cycle)
			{
				QWindow input, prepared;
				input.create();
				prepared.create();
				excluded(input);
				excluded(prepared);
				input.setFlags(Qt::Window | Qt::WindowStaysOnTopHint);
				excluded(input);
				input.destroy();
				input.create();
				excluded(input);
				QMessageBox dialog;
				dialog.winId();
				require(dialog.windowHandle(), "dialog platform window");
				excluded(*dialog.windowHandle());
				dialog.windowHandle()->setTransientParent(&input);
				excluded(*dialog.windowHandle());
				SetWindowLongPtrW(native(prepared), GWL_EXSTYLE, WS_EX_APPWINDOW | WS_EX_NOACTIVATE);
				excluded(prepared);
				require(GetWindowLongPtrW(native(prepared), GWL_EXSTYLE) & WS_EX_NOACTIVATE, "unrelated styles survive");
				require(dialog.windowHandle()->transientParent() == &input, "Qt ownership stays intact");
				const auto external = CreateWindowExW(WS_EX_APPWINDOW, L"STATIC", L"", WS_POPUP,
					0, 0, 160, 90, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
				require(external, "external render window");
				const auto style = GetWindowLongPtrW(external, GWL_EXSTYLE);
				DestroyWindow(external);
				require((style & WS_EX_APPWINDOW) && !(style & WS_EX_TOOLWINDOW), "external render surface keeps its shell policy");
			}
		}
		QWindow ordinary;
		ordinary.create();
		require(!(GetWindowLongPtrW(native(ordinary), GWL_EXSTYLE) & WS_EX_TOOLWINDOW), "ordinary Qt application is unaffected");
		std::cout << "30 hidden Qt cycles: input, preparation, dialogs, style changes and recreation passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
