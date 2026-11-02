#include "pbd_test.h"

#ifdef _WIN32
#include "util/pbd_clip_frames.h"

#include <d3d10.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <set>
#include <string>
#include <utility>
#include <vector>

using pbd::test::check;

namespace
{
	template <class T>
	using ptr = Microsoft::WRL::ComPtr<T>;
	constexpr UINT32 width = 1280, height = 720;

	std::uint8_t luma_at(unsigned frame, unsigned x, unsigned y)
	{
		return static_cast<std::uint8_t>(frame * 7 + x + y * 3);
	}
	std::uint8_t chroma_at(unsigned frame, unsigned x, unsigned y)
	{
		return static_cast<std::uint8_t>(frame * 11 + x * 5 + y);
	}

	auto picture(unsigned frame)
	{
		return [frame](std::uint8_t* luma, std::uint8_t* chroma, std::size_t pitch)
		{
			for (unsigned y = 0; y < height; ++y)
				for (unsigned x = 0; x < width; ++x)
					luma[y * pitch + x] = luma_at(frame, x, y);
			for (unsigned y = 0; y < height / 2; ++y)
				for (unsigned x = 0; x < width; ++x)
					chroma[y * pitch + x] = chroma_at(frame, x, y);
			return true;
		};
	}

	// The texture and subresource behind a frame: a pool may hand out slices of one texture array.
	std::pair<ID3D11Texture2D*, UINT> where(IMFSample* sample)
	{
		ptr<IMFMediaBuffer> buffer;
		ptr<IMFDXGIBuffer> surface;
		ptr<ID3D11Texture2D> texture;
		UINT index = 0;
		check(SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer.As(&surface)) &&
				  SUCCEEDED(surface->GetResource(IID_PPV_ARGS(&texture))) && SUCCEEDED(surface->GetSubresourceIndex(&index)),
			"a frame is a texture the encoder can read");
		return {texture.Get(), index};
	}

	// A released frame returns to its pool from a Media Foundation work queue, a moment after the release.
	template <typename Write>
	HRESULT next_soon(pbd::clips::input_frames& frames, ptr<IMFSample>& sample, Write write)
	{
		for (unsigned tries = 0;; ++tries)
		{
			const HRESULT result = frames.next(sample, write);
			if (result != MF_E_SAMPLEALLOCATOR_EMPTY || tries == 1000)
				return result;
			Sleep(1);
		}
	}

	bool shows(ID3D11Device* device, ID3D11DeviceContext* context, IMFSample* sample, unsigned frame)
	{
		const auto [texture, index] = where(sample);
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_NV12;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ptr<ID3D11Texture2D> readback;
		check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)), "a readback texture");
		context->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, texture, index, nullptr);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		check(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "the readback maps");
		const auto* luma = static_cast<const std::uint8_t*>(mapped.pData);
		const auto* chroma = luma + std::size_t{mapped.RowPitch} * height;
		bool same = true;
		for (unsigned y = 0; y < height && same; ++y)
			for (unsigned x = 0; x < width && same; ++x)
				same = luma[y * mapped.RowPitch + x] == luma_at(frame, x, y);
		for (unsigned y = 0; y < height / 2 && same; ++y)
			for (unsigned x = 0; x < width && same; ++x)
				same = chroma[y * mapped.RowPitch + x] == chroma_at(frame, x, y);
		context->Unmap(readback.Get(), 0);
		return same;
	}

	void frame_pool_checks()
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		check(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)), "Media Foundation starts");
		{
			// WARP stands in for the graphics card, so this needs none.
			ptr<ID3D11Device> device;
			ptr<ID3D11DeviceContext> context;
			check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
					  nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)),
				"a WARP device");
			ptr<ID3D10Multithread> multithread;
			if (SUCCEEDED(device.As(&multithread)))
				multithread->SetMultithreadProtected(TRUE);
			UINT token = 0;
			ptr<IMFDXGIDeviceManager> manager;
			check(SUCCEEDED(MFCreateDXGIDeviceManager(&token, &manager)) && SUCCEEDED(manager->ResetDevice(device.Get(), token)), "a device manager");
			ptr<IMFMediaType> type;
			check(SUCCEEDED(MFCreateMediaType(&type)) && SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
					  SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) &&
					  SUCCEEDED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) &&
					  SUCCEEDED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height)) &&
					  SUCCEEDED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 60, 1)) &&
					  SUCCEEDED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)),
				"the encoder's input type");

			pbd::clips::input_frames frames;
			check(SUCCEEDED(frames.open(manager.Get(), device.Get(), type.Get())), "the frame pool opens");
			constexpr unsigned most = pbd::clips::input_frames::most;
			std::vector<ptr<IMFSample>> held;
			std::set<std::pair<ID3D11Texture2D*, UINT>> textures;
			for (unsigned frame = 0; frame < most; ++frame)
			{
				ptr<IMFSample> sample;
				check(SUCCEEDED(frames.next(sample, picture(frame))) && sample, "the pool grows to what the encoder holds");
				textures.insert(where(sample.Get()));
				held.push_back(std::move(sample));
			}
			check(textures.size() == most, "every frame the encoder holds has its own texture");
			ptr<IMFMediaBuffer> buffer;
			ptr<IMF2DBuffer> planes;
			DWORD current = 0, contiguous = 0;
			check(SUCCEEDED(held[0]->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer->GetCurrentLength(&current)) &&
					  SUCCEEDED(buffer.As(&planes)) && SUCCEEDED(planes->GetContiguousLength(&contiguous)),
				"a frame reports its length");
			check(current == contiguous && current >= width * height * 3 / 2, "a frame reports a full NV12 picture to the encoder");

			bool written = false;
			ptr<IMFSample> extra;
			const HRESULT full = frames.next(extra, [&](std::uint8_t*, std::uint8_t*, std::size_t)
				{
					written = true;
					return true;
				});
			check(full == MF_E_SAMPLEALLOCATOR_EMPTY && !extra && !written, "with every frame held, a new one is dropped before any work");

			check(shows(device.Get(), context.Get(), held[0].Get(), 0) && shows(device.Get(), context.Get(), held[most - 1].Get(), most - 1),
				"each frame holds the picture written into it, luma and chroma");

			held.erase(held.begin() + 3);
			ptr<IMFSample> again;
			check(SUCCEEDED(next_soon(frames, again, picture(100))) && again, "a frame the encoder released is free again");
			check(textures.count(where(again.Get())) == 1, "a free frame reuses a pooled texture");
			check(shows(device.Get(), context.Get(), again.Get(), 100), "a reused frame shows the new picture");
			again.Reset();
			held.clear();

			// Ten seconds at 60 frames a second, each released as an encoder would. The pictures were checked above.
			const auto blank = [](std::uint8_t*, std::uint8_t*, std::size_t)
			{
				return true;
			};
			for (unsigned frame = 0; frame < 600; ++frame)
			{
				ptr<IMFSample> sample;
				check(SUCCEEDED(next_soon(frames, sample, blank)) && sample, "a steady stream always finds a free frame");
				textures.insert(where(sample.Get()));
			}
			check(textures.size() == most, "a steady stream creates no new textures");

			ptr<IMFSample> failed;
			check(frames.next(failed, [](std::uint8_t*, std::uint8_t*, std::size_t)
					  {
						  return false;
					  }) == E_FAIL &&
					  !failed,
				"a failed write hands out no frame");
			frames.close();
		}
		MFShutdown();
		if (SUCCEEDED(com))
			CoUninitialize();
	}
	struct encoder_list
	{
		IMFActivate** items = nullptr;
		UINT32 count = 0;
		encoder_list() = default;
		encoder_list(const encoder_list&) = delete;
		encoder_list& operator=(const encoder_list&) = delete;
		~encoder_list()
		{
			for (UINT32 i = 0; i < count; ++i)
				items[i]->Release();
			CoTaskMemFree(items);
		}
		// "VEN_1002" and the like, as a hardware encoder names its maker.
		std::vector<std::wstring> vendors() const
		{
			std::vector<std::wstring> out;
			for (UINT32 i = 0; i < count; ++i)
			{
				WCHAR* value = nullptr;
				UINT32 length = 0;
				out.emplace_back(SUCCEEDED(items[i]->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &value, &length)) ? value : L"");
				CoTaskMemFree(value);
			}
			return out;
		}
	};

	std::wstring vendor_of(ID3D11Device* device)
	{
		ptr<IDXGIDevice> graphics;
		ptr<IDXGIAdapter> card;
		DXGI_ADAPTER_DESC desc{};
		check(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&graphics))) && SUCCEEDED(graphics->GetAdapter(&card)) && SUCCEEDED(card->GetDesc(&desc)),
			"the device names its card");
		wchar_t text[16]{};
		swprintf_s(text, L"VEN_%04X", desc.VendorId);
		return text;
	}

	void encoder_choice_checks()
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		check(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)), "Media Foundation starts");
		{
			const MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
			encoder_list every;
			check(SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &output,
					  &every.items, &every.count)),
				"every hardware encoder is listed");

			// WARP has no encoder of its own: every hardware encoder is listed instead, as before.
			ptr<ID3D11Device> software;
			check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &software, nullptr, nullptr)),
				"a WARP device");
			encoder_list fallback;
			bool on_card = true;
			check(SUCCEEDED(pbd::clips::h264_encoders(software.Get(), &fallback.items, &fallback.count, on_card)), "a card without an encoder lists encoders");
			check(!on_card && fallback.vendors() == every.vendors(), "a card without an encoder gets every hardware encoder");

			// On a real card, its own encoder is found by the card's identity, and no other card's encoder is listed.
			ptr<ID3D11Device> hardware;
			if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
					D3D11_SDK_VERSION, &hardware, nullptr, nullptr)))
			{
				const std::wstring vendor = vendor_of(hardware.Get());
				const auto makers = every.vendors();
				encoder_list mine;
				check(SUCCEEDED(pbd::clips::h264_encoders(hardware.Get(), &mine.items, &mine.count, on_card)), "a card lists encoders");
				if (std::find(makers.begin(), makers.end(), vendor) != makers.end())
				{
					check(on_card && mine.count, "the card's own encoder is found");
					for (const auto& maker : mine.vendors())
						check(maker == vendor, "only the card's encoders are listed");
				}
			}
		}
		MFShutdown();
		if (SUCCEEDED(com))
			CoUninitialize();
	}
} // namespace

PBD_TEST(PbdClips, EncoderFramesComeFromAFixedPool)
{
	frame_pool_checks();
}

PBD_TEST(PbdClips, EncoderSitsOnTheRecordersCard)
{
	encoder_choice_checks();
}
#endif
