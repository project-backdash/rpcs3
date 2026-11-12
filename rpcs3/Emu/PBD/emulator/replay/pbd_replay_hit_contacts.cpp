#include "stdafx.h"
#include "pbd_replay_carry.h"
#include "pbd_replay_carry_guard.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Memory/vm.h"

namespace pbd::replay_carry
{
	namespace mem = pbd::replay_memory;
	namespace
	{
		bool contact_context(ppu_thread& owner, u64 generation)
		{
			constexpr std::array signatures{mem::hit_contact_output, mem::hit_contacts_initialize,
				mem::hit_contact_slots};
			return actor_context(owner, generation, signatures);
		}
		bool contact_access(actor_hit_contacts& saved, bool write)
		{
			if (write)
				for (const auto& port : saved.ports)
					for (const auto& contact : port)
						if (contact.sphere_index >= mem::hurt_sphere_count)
							return false;
			for (u32 side = 0; side < saved.ports.size(); ++side)
				for (u32 index = 0; index < saved.ports[side].size(); ++index)
				{
					const u32 base = mem::actors + side * mem::actor_stride + mem::actor_hit_contacts +
					                 index * mem::hit_contact_bytes;
					auto& contact = saved.ports[side][index];
					be_t<u16> sphere = contact.sphere_index;
					if (!vector_access(base + mem::hit_contact_end_offset, contact.end_bits, write) ||
						!vector_access(base + mem::hit_contact_delta_offset, contact.delta_bits, write) ||
						!vm::try_access(base + mem::hit_contact_sphere_offset, &sphere, sizeof(sphere), write))
						return false;
					if (!write)
						contact.sphere_index = sphere;
					if (contact.sphere_index >= mem::hurt_sphere_count)
						return false;
				}
			return true;
		}
	} // namespace
	bool read_actor_hit_contacts(ppu_thread& owner, u64 generation, actor_hit_contacts& out) noexcept
	{
		try
		{
			actor_hit_contacts sample;
			if (!contact_context(owner, generation) || !contact_access(sample, false))
				return false;
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_hit_contacts(ppu_thread& owner, u64 generation, const actor_hit_contacts& saved) noexcept
	{
		try
		{
			if (!owner_ready(owner, generation) || owner.cia != mem::match_initialize || owner.gpr[3] != 2 ||
				!contact_context(owner, generation))
				return false;
			auto copy = saved;
			actor_hit_contacts observed;
			return contact_access(copy, true) && read_actor_hit_contacts(owner, generation, observed) &&
			       observed.ports == saved.ports;
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace pbd::replay_carry
