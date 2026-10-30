#include "pbd_test.h"

#include "Emu/Cell/pbd_setup_digest.h"

#include <cstring>
#include <string_view>
#include <vector>

using pbd::test::check;

namespace
{
	using namespace pbd::setup_digest;

	// Guest memory holding the save image and both records; everything else is unmapped.
	struct memory
	{
		std::vector<u8> image = std::vector<u8>(save_size), table = std::vector<u8>(2 * record_stride);
		bool readable = true;
		u32 hole = 0; // a chunk that starts here cannot be read either (0: none)
		u8* at(u32 address)
		{
			if (address >= save_image && address < save_image + save_size)
				return image.data() + (address - save_image);
			if (address >= records && address < records + 2 * record_stride)
				return table.data() + (address - records);
			return nullptr;
		}
		bool read(u32 address, std::span<u8> out)
		{
			u8* source = at(address);
			if (!readable || (hole && address == hole) || !source || !at(address + static_cast<u32>(out.size()) - 1))
				return false;
			std::memcpy(out.data(), source, out.size());
			return true;
		}
		parts now()
		{
			return digest([this](u32 address, std::span<u8> out)
				{
					return read(address, out);
				});
		}
	};

	bool same(const parts& a, const parts& b)
	{
		return a.save == b.save && a.record0 == b.record0 && a.record1 == b.record1;
	}

	// Copies the image into both records, as the game does when VS starts.
	void enter_vs(memory& m)
	{
		for (u32 side = 0; side < 2; side++)
			std::memcpy(m.table.data() + side * record_stride + record_save, m.image.data(), save_size);
	}

	// A new install's save, copied into both records.
	memory installed()
	{
		memory m;
		for (u32 i = 0; i < save_size; i++)
			m.image[i] = static_cast<u8>(i * 31 + 7);
		enter_vs(m);
		return m;
	}
} // namespace

PBD_TEST(PbdSetupDigest, Checks)
{
	// A new install's checkpoint: default preferences, keys and last characters 5/6, copied into both records.
	memory fresh;
	const u8 prefs[] = {1, 2, 2, 0, 0, 0, 1, 12, 12, 12, 0};
	std::memcpy(fresh.image.data() + 4, prefs, sizeof(prefs));
	fresh.image[0x33] = 5;
	fresh.image[0x37] = 6;
	std::memcpy(fresh.image.data() + 0x2d14, "2026093018293960265", 19);
	for (u32 i = 0x1d8c0; i < 0x1d970; i++)
		fresh.image[i] = static_cast<u8>(i * 7);
	enter_vs(fresh);
	const parts base = fresh.now();

	// Another new install: its save was created at another time, so the stamp and the block derived from it differ.
	memory other = fresh;
	std::memcpy(other.image.data() + 0x2d14, "2026100613454712345", 19);
	for (u32 i = 0x1d8c0; i < 0x1d970; i++)
		other.image[i] = static_cast<u8>(i * 13 + 1);
	enter_vs(other);
	check(same(other.now(), base));

	// Each player's Sound Options land in the save image after the checkpoint loads; they never refuse a session.
	other.image[0x0a] = 0;
	other.image[0x0b] = 3;
	other.image[0x0e] = 1;
	check(same(other.now(), base));

	// A PC that had finished matches: its save holds other last characters, play counts and flags.
	for (const u32 offset : {0x2cu, 0x33u, 0x4fu, 0x46bbu, 0x4867u})
	{
		memory played = fresh;
		played.image[offset] ^= 1;
		enter_vs(played);
		const parts p = played.now();
		check(p.save != base.save && p.record0 != base.record0 && p.record1 != base.record1);
	}

	// The records are compared on their own: the save copy, the record head and the record tail.
	for (const u32 offset : {0u, 0xfu, record_save + 0x33, record_save + 0x2d13, record_save + 0x2d30, record_save + 0x1d8bf,
			 record_save + 0x1d970, record_save + 0x0a, record_stride - 1})
	{
		memory changed = fresh;
		changed.table[offset] ^= 0x80;
		const parts p = changed.now();
		check(p.save == base.save && p.record0 != base.record0 && p.record1 == base.record1);
		changed = fresh;
		changed.table[record_stride + offset] ^= 0x80;
		const parts q = changed.now();
		check(q.save == base.save && q.record0 == base.record0 && q.record1 != base.record1);
	}

	// The excluded spans end exactly at their edges in the image too.
	for (const u32 offset : {0x09u, 0x0fu, 0x2d13u, 0x2d30u, 0x1d8bfu, 0x1d970u, save_size - 1})
	{
		memory changed = fresh;
		changed.image[offset] ^= 0x80;
		check(changed.now().save != base.save);
	}
	for (const u32 offset : {0x0au, 0x0eu, 0x2d14u, 0x2d2fu, 0x1d8c0u, 0x1d96fu})
	{
		memory changed = fresh;
		changed.image[offset] ^= 0x80;
		check(same(changed.now(), base));
	}

	// Only the session start's segment names the setup.
	check(std::string_view(segment_mismatch_guard(1)) == "session_setup_mismatch");
	check(std::string_view(segment_mismatch_guard(2)) == "session_segment_state_mismatch");
	check(std::string_view(segment_mismatch_guard(9)) == "session_segment_state_mismatch");
}

PBD_TEST(PbdSetupDigest, ADigestOfReadableMemoryIsComplete)
{
	memory m = installed();
	check(m.now().complete, "every chunk of the save image and both records was read");
}

PBD_TEST(PbdSetupDigest, AnUnreadableChunkMarksTheDigestIncompleteWithItsOwnEndReason)
{
	memory m = installed();
	const parts base = m.now();
	m.hole = records + record_stride + 4096; // the second record's second chunk
	const parts p = m.now();
	check(!p.complete, "one unreadable chunk is enough");
	check(p.save == base.save && p.record0 == base.record0 && p.record1 != base.record1, "only the hash over the hole changes");
	m.hole = 0;
	m.readable = false;
	check(!m.now().complete, "nothing readable is incomplete too");
	check(std::string_view(setup_unreadable_guard) == "session_setup_unreadable" &&
			  std::string_view(setup_unreadable_guard) != segment_mismatch_guard(1),
		"an incomplete digest has its own end reason, not the setup mismatch");
}

PBD_TEST(PbdSetupDigest, AnIncompleteDigestStillLogsComparableValues)
{
	memory a = installed(), b = installed();
	a.readable = b.readable = false;
	const parts pa = a.now(), pb = b.now();
	check(same(pa, pb), "unreadable bytes hash as 0xee on every PC, so two logs can be compared");
	check(!same(pa, installed().now()), "and never as the readable values");
}
