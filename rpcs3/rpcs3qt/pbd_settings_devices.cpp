#include "pbd_settings_devices.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/Audio/audio_device_enumerator.h"
#include "util/logs.hpp"

#include <QString>

#include <algorithm>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#endif

LOG_CHANNEL(gui_log, "GUI");

namespace pbd::settings_devices
{
#ifdef _WIN32
	namespace
	{
		struct monitor_entry
		{
			pbd::settings_menu::option entry;
			HMONITOR handle = nullptr;
			bool primary = false;
		};

		BOOL CALLBACK add_monitor(HMONITOR handle, HDC, LPRECT, LPARAM param)
		{
			auto& list = *reinterpret_cast<std::vector<monitor_entry>*>(param);
			MONITORINFOEXW info{};
			info.cbSize = sizeof(info);
			if (!GetMonitorInfoW(handle, &info))
				return TRUE;
			DISPLAY_DEVICEW device{};
			device.cb = sizeof(device);
			const bool found = EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) && device.DeviceID[0];
			const QString identity = QString::fromWCharArray(found ? device.DeviceID : info.szDevice);
			QString number = QString::fromWCharArray(info.szDevice);
			number.remove(QStringLiteral("\\\\.\\DISPLAY"));
			const long width = info.rcMonitor.right - info.rcMonitor.left, height = info.rcMonitor.bottom - info.rcMonitor.top;
			const QString label = QStringLiteral("%1: %2 x %3").arg(number).arg(width).arg(height);
			list.push_back({{(QStringLiteral("monitor:") + identity).toStdString(), label.toStdString()}, handle,
				(info.dwFlags & MONITORINFOF_PRIMARY) != 0});
			return TRUE;
		}
	} // namespace
#endif

	std::vector<pbd::settings_menu::option> audio_outputs()
	{
		std::vector<pbd::settings_menu::option> out{{std::string(pbd::settings_menu::default_audio_output), "Windows default"}};
		try
		{
			if (const auto& factory = Emu.GetCallbacks().get_audio_enumerator)
			{
				if (const auto enumerator = factory(static_cast<u64>(g_cfg.audio.renderer.get())))
				{
					for (const auto& device : enumerator->get_output_devices())
					{
						if (out.size() >= pbd::settings_menu::max_options)
							break;
						if (device.id.empty() || device.id == audio_device_enumerator::DEFAULT_DEV_ID)
							continue;
						out.push_back({device.id, device.name.empty() ? device.id : device.name});
					}
				}
			}
		}
		catch (const std::exception& e)
		{
			gui_log.warning("PBD settings: the audio outputs could not be listed: %s", e.what());
		}
		return out;
	}

	displays game_displays([[maybe_unused]] WId window)
	{
		displays out;
#ifdef _WIN32
		std::vector<monitor_entry> monitors;
		EnumDisplayMonitors(nullptr, nullptr, add_monitor, reinterpret_cast<LPARAM>(&monitors));
		std::stable_sort(monitors.begin(), monitors.end(), [](const monitor_entry& a, const monitor_entry& b)
			{
				return a.primary && !b.primary;
			});
		const HMONITOR here = MonitorFromWindow(reinterpret_cast<HWND>(window), MONITOR_DEFAULTTONEAREST);
		for (const auto& monitor : monitors)
		{
			if (monitor.handle == here)
				out.current = monitor.entry.id;
			if (out.list.size() < pbd::settings_menu::max_options)
				out.list.push_back(monitor.entry);
		}
#endif
		return out;
	}
} // namespace pbd::settings_devices
