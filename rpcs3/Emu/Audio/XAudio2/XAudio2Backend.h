#pragma once

#ifndef _WIN32
#error "XAudio2 can only be built on Windows."
#endif

#include "Emu/Audio/AudioBackend.h"

#include <initguid.h>
#include <xaudio2.h>
#include <wrl/client.h>

class XAudio2Backend final : public AudioBackend, public IXAudio2VoiceCallback, public IXAudio2EngineCallback
{
public:
	XAudio2Backend();
	~XAudio2Backend() override;

	XAudio2Backend(const XAudio2Backend&) = delete;
	XAudio2Backend& operator=(const XAudio2Backend&) = delete;

	std::string_view GetName() const override { return "XAudio2"sv; }

	bool Initialized() override;
	bool Operational() override;

	bool Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout) override;
	void Close() override;

	f64 GetCallbackFrameLen() override;

	void Play() override;
	void Pause() override;

private:
	static constexpr u32 INTERNAL_BUF_SIZE_MS = 25;

	Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2_instance{};
	IXAudio2MasteringVoice* m_master_voice{};
	IXAudio2SourceVoice* m_source_voice{};
	bool m_com_init_success = false;

	std::vector<u8> m_data_buf{};
	std::array<u8, sizeof(float) * static_cast<u32>(AudioChannelCnt::SURROUND_7_1)> m_last_sample{};

	atomic_t<bool> m_reset_req = false;

	// XAudio voice callbacks
	void OnVoiceProcessingPassStart(UINT32 BytesRequired) noexcept override;
	void OnVoiceProcessingPassEnd() noexcept override {}
	void OnStreamEnd() noexcept override {}
	void OnBufferStart(void* /* pBufferContext */) noexcept override {}
	void OnBufferEnd(void* /* pBufferContext*/) noexcept override {}
	void OnLoopEnd(void* /* pBufferContext */) noexcept override {}
	void OnVoiceError(void* /* pBufferContext */, HRESULT /* Error */) noexcept override {}

	// XAudio engine callbacks
	void OnProcessingPassStart() noexcept override {};
	void OnProcessingPassEnd() noexcept override {};
	void OnCriticalError(HRESULT Error) noexcept override;

	void CloseUnlocked();
};
