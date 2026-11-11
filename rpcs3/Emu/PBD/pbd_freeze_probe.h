#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

// The freeze probe's view of the geometry request manager (NU::Draw::Geometry::RequestManager, worker PPU 0x01000005).
// The present's request (0x35bf6c) queues one request, posts the worker's semaphore (id at manager+0x50, created by
// the manager's init, so it varies with creation order) when the pending list was empty and
// then spins until the busy count at manager+8 is zero. The worker raises that count after each wake and lowers it when
// it finds the pending list empty, just before it waits again. A freeze in that spin is told apart here: a busy count
// above zero with an empty pending list and the worker asleep on the semaphore is a count the worker will never lower.
namespace pbd::freeze_probe
{
	inline constexpr std::uint32_t manager_address = 0x10742880, manager_bytes = 0xa0;
	// Each list block holds two lists: the free link nodes (count at +0xc) and the queued nodes (count at +0x24).
	// The pending block (manager+0x4060) queues requests for the worker; the request block (manager+0x5894) holds the
	// request pool's free requests.
	inline constexpr std::uint32_t pending_address = 0x107468e0, request_address = 0x10748114, list_bytes = 0x34;
	inline constexpr std::uint32_t semaphore_offset = 0x50;
	// The draw request manager has the same shape, but its present-side wait (the drain at 0x357714) sleeps on the 64-bit
	// event at +0xa8 under the event block's mutex and condition variable instead of spinning; its job lists sit at
	// +0x48..+0x5c.
	inline constexpr std::uint32_t draw_address = 0x1074be80, draw_bytes = 0xb0, draw_event_offset = 0xa8;

	using manager_image = std::array<std::uint8_t, manager_bytes>;
	using draw_image = std::array<std::uint8_t, draw_bytes>;
	using list_image = std::array<std::uint8_t, list_bytes>;

	struct semaphore_facts
	{
		std::int32_t value = 0; // lv2 value: each sleeping waiter takes it one below zero
		std::uint32_t waiters = 0, first_waiter = 0;
	};

	inline std::uint32_t be32(const std::uint8_t* bytes) noexcept
	{
		return std::uint32_t{bytes[0]} << 24 | std::uint32_t{bytes[1]} << 16 | std::uint32_t{bytes[2]} << 8 | bytes[3];
	}

	inline void append_hex(std::string& text, const std::uint8_t* bytes, std::size_t count)
	{
		constexpr char digits[] = "0123456789abcdef";
		for (std::size_t i = 0; i < count; ++i)
		{
			text += digits[bytes[i] >> 4];
			text += digits[bytes[i] & 15];
		}
	}

	inline std::string word(std::uint32_t value)
	{
		const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
			static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
		std::string text;
		append_hex(text, bytes.data(), bytes.size());
		return text;
	}

	inline std::string list_text(const char* name, std::uint32_t address, const char* queued, const std::optional<list_image>& list)
	{
		std::string text = std::string("geometry ") + name + " " + word(address) + ": ";
		if (!list)
			return text + "unreadable\n";
		text += "free_links " + std::to_string(be32(list->data() + 0xc)) + " " + queued + " " + std::to_string(be32(list->data() + 0x24)) + " bytes ";
		append_hex(text, list->data(), list->size());
		return text + "\n";
	}

	// The worker's semaphore id, read from the manager; without the manager there is no semaphore to look up.
	inline std::optional<std::uint32_t> semaphore_id(const std::optional<manager_image>& manager)
	{
		if (!manager)
			return std::nullopt;
		return be32(manager->data() + semaphore_offset);
	}

	// Lines for freeze-probe.txt. Each source is absent when it could not be read at once.
	inline std::string geometry_text(const std::optional<manager_image>& manager, const std::optional<list_image>& pending,
		const std::optional<list_image>& requests, const std::optional<semaphore_facts>& semaphore)
	{
		std::string text = "geometry manager " + word(manager_address) + ": ";
		if (manager)
		{
			const auto* bytes = manager->data();
			const std::uint64_t completed = std::uint64_t{be32(bytes + 0x10)} << 32 | be32(bytes + 0x14);
			text += "busy " + std::to_string(static_cast<std::int32_t>(be32(bytes + 8))) + " stopping " + std::to_string(be32(bytes + 4)) +
			        " completed_frame " + std::to_string(completed) + " semaphore " + word(be32(bytes + semaphore_offset)) + " bytes ";
			append_hex(text, bytes, manager->size());
			text += "\n";
		}
		else
			text += "unreadable\n";
		text += list_text("pending", pending_address, "pending", pending);
		text += list_text("requests", request_address, "free_requests", requests);
		const auto id = semaphore_id(manager);
		text += id ? "geometry semaphore " + word(*id) + ": " : std::string("geometry semaphore: ");
		if (semaphore)
			text += "value " + std::to_string(semaphore->value) + " waiters " + std::to_string(semaphore->waiters) + " first_waiter " +
			        word(semaphore->first_waiter) + "\n";
		else
			text += "unreadable\n";
		return text;
	}

	// The drain waits for the event to be zero, so a drain that waits forever shows here as a nonzero event.
	inline std::string draw_text(const std::optional<draw_image>& draw)
	{
		std::string text = "draw manager " + word(draw_address) + ": ";
		if (!draw)
			return text + "unreadable\n";
		const auto* bytes = draw->data();
		const std::uint64_t event = std::uint64_t{be32(bytes + draw_event_offset)} << 32 | be32(bytes + draw_event_offset + 4);
		text += "event " + std::to_string(event) + " bytes ";
		append_hex(text, bytes, draw->size());
		return text + "\n";
	}
} // namespace pbd::freeze_probe
