#pragma once

#ifdef _WIN32
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QPlatformSurfaceEvent>
#include <QSet>
#include <QWindow>

#include "pbd_game_host_shell.h"

namespace pbd::game_host
{
	// Qt can recreate a native window or replace its styles after creation. Keep every Qt top-level helper out of the
	// shell for the whole process lifetime, including parentless dialogs and prepared games which have never been shown.
	// The render surface lives on its own Win32 thread and retains its separate Steam/owned-window policy.
	class qt_shell_filter final : public QObject, public QAbstractNativeEventFilter
	{
		QCoreApplication& m_app;
		QSet<HWND> m_windows;

		bool eventFilter(QObject* object, QEvent* event) override
		{
			if (event->type() != QEvent::PlatformSurface)
				return false;
			auto* window = qobject_cast<QWindow*>(object);
			if (!window || !window->isTopLevel())
				return false;
			const auto handle = reinterpret_cast<HWND>(window->winId());
			if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated)
			{
				m_windows.insert(handle);
				if (!exclude_from_shell(handle))
					qFatal("The game helper window could not be configured.");
			}
			else
				m_windows.remove(handle);
			return false;
		}

		bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override
		{
			const auto* msg = static_cast<MSG*>(message);
			if (msg && msg->message == WM_STYLECHANGING && msg->wParam == static_cast<WPARAM>(GWL_EXSTYLE) &&
				m_windows.contains(msg->hwnd))
			{
				auto* styles = reinterpret_cast<STYLESTRUCT*>(msg->lParam);
				styles->styleNew = qt_shell_ex_style(styles->styleNew);
			}
			return false;
		}

	public:
		explicit qt_shell_filter(QCoreApplication& app) : m_app(app)
		{
			m_app.installEventFilter(this);
			m_app.installNativeEventFilter(this);
		}

		~qt_shell_filter() override
		{
			m_app.removeEventFilter(this);
			m_app.removeNativeEventFilter(this);
		}
	};
} // namespace pbd::game_host
#endif
