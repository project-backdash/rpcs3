#define NOMINMAX
#include "../rpcs3/Emu/Audio/WindowsAudioSessionIdentity.h"
#include <audioclient.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

static void check(HRESULT hr)
{
	if (FAILED(hr))
		throw std::runtime_error("COM failure " + std::to_string(static_cast<unsigned>(hr)));
}

class silent_session
{
	ComPtr<IAudioClient> client;
	ComPtr<IAudioSessionControl> session;
	ComPtr<ISimpleAudioVolume> volume;
	ComPtr<IMMDevice> endpoint;
	UINT32 rate = 0, frames = 0;

public:
	void open()
	{
		volume.Reset();
		session.Reset();
		client.Reset();
		endpoint.Reset();
		ComPtr<IMMDeviceEnumerator> devices;
		check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices)));
		check(devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint));
		check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
		WAVEFORMATEX* format = nullptr;
		check(client->GetMixFormat(&format));
		GUID id{};
		check(CoCreateGuid(&id));
		rate = format->nSamplesPerSec;
		const auto hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST, 1000000, 0, format, &id);
		CoTaskMemFree(format);
		check(hr);
		check(client->GetBufferSize(&frames));
		check(client->GetService(IID_PPV_ARGS(&session)));
		check(client->GetService(IID_PPV_ARGS(&volume)));
	}

	void set_volume(float value)
	{
		check(volume->SetMasterVolume(value, nullptr));
	}
	void set_mute(bool value)
	{
		check(volume->SetMute(value, nullptr));
	}

	bool grouped()
	{
		GUID group{};
		check(session->GetGroupingParam(&group));
		return group == pbd::audio_session::grouping;
	}

	std::string snapshot()
	{
		ComPtr<IAudioSessionControl2> identity;
		check(session.As(&identity));
		LPWSTR own_id = nullptr;
		check(identity->GetSessionInstanceIdentifier(&own_id));
		const std::wstring wanted(own_id);
		CoTaskMemFree(own_id);
		ComPtr<IAudioSessionManager2> manager;
		ComPtr<IAudioSessionEnumerator> sessions;
		check(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager));
		check(manager->GetSessionEnumerator(&sessions));
		int size = 0;
		check(sessions->GetCount(&size));
		ComPtr<IAudioSessionControl> enumerated;
		for (int i = 0; i < size; ++i)
		{
			ComPtr<IAudioSessionControl> candidate;
			ComPtr<IAudioSessionControl2> candidate_id;
			check(sessions->GetSession(i, &candidate));
			check(candidate.As(&candidate_id));
			LPWSTR id = nullptr;
			check(candidate_id->GetSessionInstanceIdentifier(&id));
			const bool matches = id && wanted == id;
			CoTaskMemFree(id);
			if (matches)
			{
				enumerated = candidate;
				break;
			}
		}
		if (!enumerated)
			throw std::runtime_error("session absent from IAudioSessionManager2 enumeration");
		GUID group{};
		check(enumerated->GetGroupingParam(&group));
		wchar_t guid[40]{};
		StringFromGUID2(group, guid, 40);
		LPWSTR name = nullptr, icon = nullptr;
		check(enumerated->GetDisplayName(&name));
		check(enumerated->GetIconPath(&icon));
		float level = 0;
		BOOL muted = FALSE;
		check(volume->GetMasterVolume(&level));
		check(volume->GetMute(&muted));
		std::wostringstream json;
		json << L"{\"pid\":" << GetCurrentProcessId() << L",\"group\":" << std::quoted(guid)
			 << L",\"name\":" << std::quoted(name ? name : L"") << L",\"icon\":" << std::quoted(icon ? icon : L"")
			 << L",\"volume\":" << level << L",\"mute\":" << (muted ? L"true" : L"false")
			 << L",\"rate\":" << rate << L",\"frames\":" << frames << L"}";
		CoTaskMemFree(name);
		CoTaskMemFree(icon);
		const auto wide = json.str();
		const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		std::string result(count, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), count, nullptr, nullptr);
		return result;
	}
};

int main(int argc, char** argv)
{
	try
	{
		check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
		{
			silent_session session;
			session.open();
			const bool self_test = argc > 1 && std::string_view(argv[1]) == "--self-test";
			if (self_test)
			{
				session.set_volume(0.37f);
				session.set_mute(true);
				pbd::audio_session::identity_worker worker;
				for (int round = 0; round < 2; ++round)
				{
					for (int i = 0; i < 30 && !session.grouped(); ++i)
						Sleep(100);
					if (!session.grouped())
						throw std::runtime_error("session was not grouped");
					const auto json = session.snapshot();
					if (json.find("Project Backdash") == std::string::npos)
						throw std::runtime_error("display name missing");
					std::cout << json << std::endl;
					if (!round && (json.find("\"volume\":0.37") == std::string::npos || json.find("\"mute\":true") == std::string::npos))
						throw std::runtime_error("identity changed volume or mute");
					if (!round)
						session.open();
				}
			}
			else
			{
				std::cout << session.snapshot() << std::endl;
				std::string line;
				while (std::getline(std::cin, line) && line != "quit")
				{
					if (line == "reopen")
						session.open();
					else if (line == "identity")
						pbd::audio_session::start();
					else if (line.starts_with("volume:"))
						session.set_volume(std::stof(line.substr(7)));
					else if (line.starts_with("mute:"))
						session.set_mute(line.substr(5) == "1");
					std::cout << session.snapshot() << std::endl;
				}
			}
		}
		CoUninitialize();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
