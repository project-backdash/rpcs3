#pragma once

#ifdef _WIN32
#include <Windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace pbd::audio_session
{
	// A stable grouping lets the Windows mixer control all related sound together.
	inline constexpr GUID grouping{0x96f04c3a, 0x9a08, 0x4f3c, {0xb8, 0xf1, 0x72, 0x5d, 0xd8, 0xc5, 0x6e, 0xa1}};
	inline constexpr wchar_t display_name[] = L"Project Backdash";

	inline std::wstring icon_path()
	{
		std::wstring path(32768, L'\0');
		DWORD length = GetEnvironmentVariableW(L"PBD_AUDIO_ICON_PATH", path.data(), static_cast<DWORD>(path.size()));
		if (!length || length >= path.size())
			length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (!length || length >= path.size())
			return {};
		path.resize(length);
		return path;
	}

	inline void apply(const std::wstring& icon)
	{
		using Microsoft::WRL::ComPtr;
		ComPtr<IMMDeviceEnumerator> devices;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))))
			return;
		ComPtr<IMMDeviceCollection> endpoints;
		if (FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)))
			return;
		UINT count = 0;
		if (FAILED(endpoints->GetCount(&count)))
			return;
		for (UINT i = 0; i < count; ++i)
		{
			ComPtr<IMMDevice> endpoint;
			ComPtr<IAudioSessionManager2> manager;
			ComPtr<IAudioSessionEnumerator> sessions;
			if (FAILED(endpoints->Item(i, &endpoint)) ||
				FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) ||
				FAILED(manager->GetSessionEnumerator(&sessions)))
				continue;
			int size = 0;
			if (FAILED(sessions->GetCount(&size)))
				continue;
			for (int j = 0; j < size; ++j)
			{
				ComPtr<IAudioSessionControl> session;
				ComPtr<IAudioSessionControl2> identity;
				DWORD pid = 0;
				if (FAILED(sessions->GetSession(j, &session)) || FAILED(session.As(&identity)) ||
					identity->GetProcessId(&pid) != S_OK || pid != GetCurrentProcessId())
					continue;
				GUID current{};
				if (SUCCEEDED(session->GetGroupingParam(&current)) && current != grouping)
					session->SetGroupingParam(&grouping, nullptr);
				LPWSTR value = nullptr;
				if (SUCCEEDED(session->GetDisplayName(&value)))
				{
					if (!value || std::wstring_view(value) != display_name)
						session->SetDisplayName(display_name, nullptr);
					CoTaskMemFree(value);
				}
				value = nullptr;
				if (!icon.empty() && SUCCEEDED(session->GetIconPath(&value)))
				{
					if (!value || std::wstring_view(value) != icon)
						session->SetIconPath(icon.c_str(), nullptr);
					CoTaskMemFree(value);
				}
			}
		}
	}

	class identity_worker
	{
		std::mutex m_mutex;
		std::condition_variable m_wake;
		bool m_stop = false;
		std::thread m_thread;

	public:
		identity_worker() : m_thread([this]
								{
									if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
										return;
									try
									{
										const auto icon = icon_path();
										// Re-enumeration also covers streams recreated after an output changes.
										std::unique_lock lock(m_mutex);
										while (!m_stop)
										{
											lock.unlock();
											apply(icon);
											lock.lock();
											m_wake.wait_for(lock, std::chrono::milliseconds(500), [this]
												{
													return m_stop;
												});
										}
									}
									catch (...)
									{
									}
									CoUninitialize();
								})
		{
		}

		~identity_worker()
		{
			{
				std::lock_guard lock(m_mutex);
				m_stop = true;
			}
			m_wake.notify_one();
			m_thread.join();
		}
	};

	inline void start() noexcept
	{
		try
		{
			// One independent worker keeps metadata calls away from audio callbacks.
			static identity_worker worker;
		}
		catch (...)
		{
		}
	}
} // namespace pbd::audio_session
#endif
