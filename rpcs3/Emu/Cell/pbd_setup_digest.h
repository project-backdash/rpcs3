#pragma once

// The game loads SYS-DATA (0x218c0 bytes) at 0x10684960, then copies it to each player's record at
// 0x1054ae60 + side * 0x431b0 + 0x10 on entering VS. The start digest includes the save image and both records
// because they feed fight state; mismatched saves must be refused before the first game.

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace pbd::setup_digest
{
	using u32 = std::uint32_t;
	using u8 = std::uint8_t;

	inline constexpr u32 save_image = 0x10684960;
	inline constexpr u32 save_size = 0x218c0;
	inline constexpr u32 records = 0x1054ae60;
	inline constexpr u32 record_stride = 0x431b0;
	// Offset of the save copy inside a record.
	inline constexpr u32 record_save = 0x10;

	// Offsets into the save image, end exclusive.
	struct span
	{
		u32 begin, end;
	};

	// Exclude the host-derived creation stamp (also used in the CN<stamp> save folder) and its 176-byte derived block.
	inline constexpr std::array<span, 2> creation{{{0x2d14, 0x2d30}, {0x1d8c0, 0x1d970}}};
	// Speaker, BGM, SE, stage effects and subtitles: each player's own Sound Options, set in the save image after the checkpoint
	// loads (pbd_sound_options.h). The records keep the checkpoint's values.
	inline constexpr span sound{0x0a, 0x0f};

	struct parts
	{
		u32 save = 0, record0 = 0, record1 = 0;
		// False when any chunk could not be read. Such a digest says nothing about this PC's setup, so a session start
		// refuses it (setup_unreadable_guard) instead of comparing it.
		bool complete = true;
	};

	// FNV-1a over [address, address + size) except the skipped spans (offsets relative to `skip_base`). An unreadable
	// chunk clears `complete` and hashes as 0xee bytes, so the logged values of two PCs stay comparable.
	template <class Read>
	u32 hash_range(Read&& read, u32 address, u32 size, std::span<const span> skip, u32 skip_base, bool& complete)
	{
		u32 h = 0x811c9dc5u;
		std::array<u8, 4096> chunk{};
		for (u32 offset = 0; offset < size; offset += static_cast<u32>(chunk.size()))
		{
			const u32 count = std::min<u32>(static_cast<u32>(chunk.size()), size - offset);
			const bool readable = read(address + offset, std::span<u8>(chunk.data(), count));
			complete = complete && readable;
			for (u32 i = 0; i < count; i++)
			{
				const u32 at = offset + i;
				if (std::any_of(skip.begin(), skip.end(), [&](const span& s)
						{
							return at >= skip_base + s.begin && at < skip_base + s.end;
						}))
					continue;
				h = (h ^ (readable ? chunk[i] : 0xeeu)) * 0x01000193u;
			}
		}
		return h;
	}

	// `read(address, bytes)` fills `bytes` from guest memory and returns false when it cannot.
	template <class Read>
	parts digest(Read&& read)
	{
		constexpr std::array<span, 3> image_skip{creation[0], creation[1], sound};
		parts out;
		out.save = hash_range(read, save_image, save_size, image_skip, 0, out.complete);
		out.record0 = hash_range(read, records, record_stride, creation, record_save, out.complete);
		out.record1 = hash_range(read, records + record_stride, record_stride, creation, record_save, out.complete);
		return out;
	}

	// The guard a segment digest difference ends the session with. Segment 0 (keyed 1) is the session start, which includes the
	// setup parts above: there a difference means the two PCs' setups differ, and running the setup again is what fixes it.
	constexpr const char* segment_mismatch_guard(u32 key)
	{
		return key == 1 ? "session_setup_mismatch" : "session_segment_state_mismatch";
	}

	inline constexpr const char* setup_refusal = "The two players' setups differ; both need to run the setup again.";

	// The guard a peer session ends with at its start when its own setup digest is incomplete, and what the player reads.
	inline constexpr const char* setup_unreadable_guard = "session_setup_unreadable";
	inline constexpr const char* setup_unreadable_refusal = "This PC's game setup could not be read; run the setup again.";
} // namespace pbd::setup_digest
