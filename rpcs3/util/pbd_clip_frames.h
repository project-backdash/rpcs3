#pragma once

// The hardware video encoder of util/pbd_clips.cpp on the recorder's graphics card, and its input frames.
#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace pbd::clips
{
	// Hardware H.264 encoders in Media Foundation's order, through `list` and `count` as MFTEnumEx returns them; the caller
	// releases each and frees the array. An encoder fed textures must sit on the card that made them, so only the encoders of
	// `device`'s card are listed (`on_card`). Every hardware encoder is listed when the card names none, or when Windows has no
	// MFTEnum2.
	inline HRESULT h264_encoders(ID3D11Device* device, IMFActivate*** list, UINT32* count, bool& on_card)
	{
		*list = nullptr;
		*count = 0;
		on_card = false;
		const MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
		constexpr UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
		// Looked up at run time: the headers declare MFTEnum2 and MFT_ENUM_ADAPTER_LUID only for Windows 10 1607 and later.
		using enum_for_card = HRESULT(STDAPICALLTYPE*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*,
			IMFAttributes*, IMFActivate***, UINT32*);
		static constexpr GUID adapter_luid{0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};
		const HMODULE platform = GetModuleHandleW(L"mfplat.dll");
		const auto for_card = platform ? reinterpret_cast<enum_for_card>(GetProcAddress(platform, "MFTEnum2")) : nullptr;
		Microsoft::WRL::ComPtr<IDXGIDevice> graphics;
		Microsoft::WRL::ComPtr<IDXGIAdapter> card;
		Microsoft::WRL::ComPtr<IMFAttributes> attributes;
		DXGI_ADAPTER_DESC desc{};
		if (for_card && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&graphics))) && SUCCEEDED(graphics->GetAdapter(&card)) &&
			SUCCEEDED(card->GetDesc(&desc)) && SUCCEEDED(MFCreateAttributes(&attributes, 1)) &&
			SUCCEEDED(attributes->SetBlob(adapter_luid, reinterpret_cast<const UINT8*>(&desc.AdapterLuid), sizeof(desc.AdapterLuid))) &&
			SUCCEEDED(for_card(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &output, attributes.Get(), list, count)))
		{
			if (*count)
			{
				on_card = true;
				return S_OK;
			}
			CoTaskMemFree(*list);
			*list = nullptr;
		}
		return MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &output, list, count);
	}

	// NV12 frames from a fixed pool. A frame goes back to the pool when the encoder releases it, so recording creates no
	// graphics memory per frame. Direct3D 11 destroys a released texture only once its device is flushed, and an encoder
	// need not flush it: with a new texture for every frame the game grew by about 80 MB a second on some drivers.
	class input_frames
	{
		template <class T>
		using ptr = Microsoft::WRL::ComPtr<T>;
		ptr<IMFVideoSampleAllocatorEx> m_pool;
		ptr<ID3D11DeviceContext> m_context;
		// Written in turn, so a frame never waits for the GPU to finish copying the one before.
		std::array<ptr<ID3D11Texture2D>, 2> m_uploads;
		unsigned m_upload = 0;
		UINT32 m_height = 0;

	public:
		// A low-latency encoder holds a few frames. The pool grows to what the encoder keeps, up to this many (22 MB at
		// 1280 x 720); a frame that finds none free waits in the recorder's queue until one comes back.
		static constexpr DWORD most = 16;

		input_frames() = default;
		input_frames(const input_frames&) = delete;
		input_frames& operator=(const input_frames&) = delete;
		~input_frames()
		{
			close();
		}

		// `manager` holds `device`; `type` is the encoder's NV12 input type.
		HRESULT open(IMFDXGIDeviceManager* manager, ID3D11Device* device, IMFMediaType* type)
		{
			close();
			UINT32 width = 0;
			HRESULT result = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &m_height);
			ptr<IMFAttributes> textures;
			if (SUCCEEDED(result))
				result = MFCreateAttributes(&textures, 2);
			if (SUCCEEDED(result))
				result = textures->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET);
			if (SUCCEEDED(result))
				result = textures->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
			if (SUCCEEDED(result))
				result = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&m_pool));
			if (SUCCEEDED(result))
				result = m_pool->SetDirectXManager(manager);
			if (SUCCEEDED(result))
				result = m_pool->InitializeSampleAllocatorEx(4, most, textures.Get(), type);
			D3D11_TEXTURE2D_DESC upload{};
			upload.Width = width;
			upload.Height = m_height;
			upload.MipLevels = 1;
			upload.ArraySize = 1;
			upload.Format = DXGI_FORMAT_NV12;
			upload.SampleDesc.Count = 1;
			upload.Usage = D3D11_USAGE_STAGING;
			upload.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			for (auto& texture : m_uploads)
			{
				if (SUCCEEDED(result))
					result = device->CreateTexture2D(&upload, nullptr, &texture);
			}
			if (FAILED(result))
			{
				close();
				return result;
			}
			device->GetImmediateContext(&m_context);
			return S_OK;
		}

		// Fills a free frame through write(luma, chroma, pitch), which writes `height` rows of luma and then height / 2
		// rows of interleaved chroma, each row `pitch` bytes after the last, and returns whether it did. It must not
		// throw. While the encoder holds every frame this returns MF_E_SAMPLEALLOCATOR_EMPTY without calling write.
		template <typename Write>
		HRESULT next(ptr<IMFSample>& frame, Write&& write)
		{
			frame.Reset();
			ptr<IMFSample> sample;
			HRESULT result = m_pool->AllocateSample(&sample);
			ptr<IMFMediaBuffer> buffer;
			ptr<IMFDXGIBuffer> surface;
			ptr<IMF2DBuffer> planes;
			ptr<ID3D11Texture2D> texture;
			UINT index = 0;
			DWORD length = 0;
			if (SUCCEEDED(result))
				result = sample->GetBufferByIndex(0, &buffer);
			if (SUCCEEDED(result))
				result = buffer.As(&surface);
			if (SUCCEEDED(result))
				result = surface->GetResource(IID_PPV_ARGS(&texture));
			if (SUCCEEDED(result))
				result = surface->GetSubresourceIndex(&index);
			// Encoders read the length an input buffer reports.
			if (SUCCEEDED(result))
				result = buffer.As(&planes);
			if (SUCCEEDED(result))
				result = planes->GetContiguousLength(&length);
			if (SUCCEEDED(result))
				result = buffer->SetCurrentLength(length);
			ID3D11Texture2D* const upload = m_uploads[m_upload].Get();
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(result))
				result = m_context->Map(upload, 0, D3D11_MAP_WRITE, 0, &mapped);
			if (FAILED(result))
				return result;
			// An NV12 upload texture holds its chroma rows right after its `height` luma rows.
			auto* const luma = static_cast<std::uint8_t*>(mapped.pData);
			const std::size_t pitch = mapped.RowPitch;
			const bool written = write(luma, luma + pitch * m_height, pitch);
			m_context->Unmap(upload, 0);
			if (!written)
				return E_FAIL;
			m_upload ^= 1;
			m_context->CopySubresourceRegion(texture.Get(), index, 0, 0, 0, upload, 0, nullptr);
			// The copy reaches the GPU before the encoder learns of the frame, and the device frees what was released
			// since the last frame, whichever driver the encoder runs on.
			m_context->Flush();
			frame = std::move(sample);
			return S_OK;
		}

		// Samples the encoder still holds are freed when it releases them.
		void close()
		{
			if (m_pool)
				m_pool->UninitializeSampleAllocator();
			m_pool.Reset();
			m_context.Reset();
			for (auto& texture : m_uploads)
				texture.Reset();
			m_upload = 0;
		}
	};
} // namespace pbd::clips
#endif
