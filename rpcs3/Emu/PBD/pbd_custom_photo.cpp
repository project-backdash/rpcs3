#include "stdafx.h"
#include "pbd_custom_photo.h"
#include "pbd_picture_facing.h"
#include "png.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#include <condition_variable>
#include <deque>
#include <fstream>
#include <thread>

LOG_CHANNEL(pbd_photo_log, "PBD Costume Picture");

namespace pbd::custom_photo
{
	std::vector<u8> encode_jpeg(const frame& value, u32 side)
	{
		if (!value.valid || !value.width || !value.height || value.width > 320 || value.height > 480 ||
			value.rgba.size() != u64{value.width} * value.height * 4)
			return {};
		std::vector<u8> out;
		const auto append = [](void* context, void* bytes, int count)
		{
			auto& buffer = *static_cast<std::vector<u8>*>(context);
			const auto* first = static_cast<const u8*>(bytes);
			buffer.insert(buffer.end(), first, first + count);
		};
		for (int quality : {75, 65, 55, 45, 35, 25, 15, 5})
		{
			out.clear();
			if (stbi_write_jpg_to_func(append, &out, value.width, value.height, 4, value.rgba.data(), quality) && out.size() + picture_facing::marker_bytes <= 30 * 1024)
			{
				picture_facing::record(out, side);
				return out;
			}
		}
		return {};
	}

	bool save_async(frame value, u32 side, std::vector<std::filesystem::path> targets)
	{
		struct job
		{
			frame value;
			u32 side;
			std::vector<std::filesystem::path> targets;
		};
		struct writer
		{
			std::mutex mutex;
			std::condition_variable ready;
			std::deque<job> queue;
			bool stopping = false;
			std::thread thread{[this]
				{
					for (;;)
					{
						job next;
						{
							std::unique_lock lock(mutex);
							ready.wait(lock, [this]
								{
									return stopping || !queue.empty();
								});
							if (queue.empty())
								return;
							next = std::move(queue.front());
							queue.pop_front();
						}
						try
						{
							const auto jpeg = encode_jpeg(portrait(next.value, next.side == 0, next.side == 1), next.side);
							if (jpeg.empty())
								continue;
							for (const auto& target : next.targets)
							{
								std::error_code error;
								std::filesystem::create_directories(target.parent_path(), error);
								auto temporary = target;
								temporary += "." + std::to_string(next.value.generation) + ".tmp";
								std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
								file.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
								file.close();
								if (file && !std::filesystem::exists(target, error))
								{
									std::filesystem::rename(temporary, target, error);
									if (!error)
									{
										published_pictures.fetch_add(1, std::memory_order_release);
										pbd_photo_log.notice("Costume picture saved: %s (%u bytes, flip %u).", target.string(), jpeg.size(), next.value.flip);
									}
								}
								std::filesystem::remove(temporary, error);
							}
						}
						catch (...)
						{
						}
					}
				}};
			~writer()
			{
				{
					std::lock_guard lock(mutex);
					stopping = true;
				}
				ready.notify_one();
				thread.join();
			}
		};
		static writer worker;
		std::lock_guard lock(worker.mutex);
		if (!value.valid || targets.empty() || worker.queue.size() >= 2)
			return false;
		worker.queue.push_back({std::move(value), side, std::move(targets)});
		worker.ready.notify_one();
		return true;
	}

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
