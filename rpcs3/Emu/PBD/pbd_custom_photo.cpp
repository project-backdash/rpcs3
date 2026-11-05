#include "stdafx.h"
#include "pbd_custom_photo.h"
#include "png.h"

namespace pbd::custom_photo
{
	std::vector<u8> encode_png(const frame& value)
	{
		const u64 raw_size = u64{value.width} * value.height * 4;
		if (!value.valid || !value.width || !value.height || value.width > 4096 || value.height > 4096 || value.rgba.size() != raw_size)
			return {};
		png_image image{};
		image.version = PNG_IMAGE_VERSION;
		image.width = value.width;
		image.height = value.height;
		image.format = PNG_FORMAT_RGBA;
		png_alloc_size_t size = 0;
		if (!png_image_write_to_memory(&image, nullptr, &size, 0, value.rgba.data(), 0, nullptr) || size > raw_size + 1024 * 1024)
		{
			png_image_free(&image);
			return {};
		}
		std::vector<u8> out(size);
		const bool written = png_image_write_to_memory(&image, out.data(), &size, 0, value.rgba.data(), 0, nullptr) != 0;
		png_image_free(&image);
		if (!written)
			return {};
		out.resize(size);
		return out;
	}
} // namespace pbd::custom_photo
