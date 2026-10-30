#pragma once

namespace pbd::move_list
{
	template <class Word, class Half>
	auto readiness_walk<Word, Half>::follow(const part& p, u32 from, bool checks, std::optional<u32>* own, std::optional<u32>* inputs) const -> std::optional<course>
	{
		const u32 length = p.length;
		from = (std::min)(from, length);
		std::optional<u32> guard;
		const auto cursors = follow_cursors(p, from, length, checks, guard);
		const auto freed = [&](u32 cursor, state at)
		{
			return course{course::kind::free, cursor, cursor, 0, 0, 0, via::command, at};
		};
		bool chained = false;
		std::optional<course> through;
		for (const u32 cursor : cursors)
		{
			if (guard && *guard <= cursor)
				return freed(*guard, standing(p.at));
			const entry* picked = nullptr;
			for (const entry& command : p.entries)
			{
				if (command.first > cursor || command.last < cursor)
					continue;
				if (command.input)
				{
					if (inputs && (!*inputs || cursor < **inputs))
						*inputs = cursor;
					if (!action(command))
						continue;
					const u32 at = executes(command, cursor, length);
					if (checks)
						return freed(at, standing(p.at));
					if (own && (!*own || at < **own))
						*own = at;
					continue;
				}
				if (!command.plain)
					continue;
				picked = &command;
				break;
			}
			if (picked && picked->data != 0x3fff)
			{
				const u32 at = executes(*picked, cursor, length);
				if (guard && *guard <= at)
					return freed(*guard, standing(p.at));
				return course{course::kind::hand_over, at, cursor, picked->target, picked->data,
					picked->execute == 0xff ? length : picked->execute, via::command};
			}
			const entry& end = p.end;
			if (end.first && end.first <= cursor && cursor <= end.last)
			{
				if (const auto stance = neutral(end.target))
					return freed(cursor, *stance);
				if (!chained)
				{
					through = chain(end);
					chained = true;
				}
				if (through)
				{
					course out = *through;
					out.cursor = out.left = cursor;
					return out;
				}
			}
			if (cursor == length)
				return course{course::kind::hand_over, length, length, end.target, end.data, length, via::natural};
		}
		return {};
	}

	template <class Word, class Half>
	auto readiness_walk<Word, Half>::run(u32 at, u32 cursor, u32 offset, bool committed, reading* out) const -> std::optional<stop>
	{
		if (committed && (at == record(0x8001) || at == record(0x8002)))
			return stop{offset + cursor, standing(at)};
		std::array<visit, max_parts> visits{};
		std::size_t parts = 0;
		const bool committed_attack = committed && attack(at);
		bool own_part = committed, stun = committed && reaction && !committed_attack && !down_bits(word(at + 0x0c));
		part p;
		for (;;)
		{
			for (std::size_t index = 0; index < parts; ++index)
				if (visits[index].at == at && visits[index].cursor == cursor)
					return loop(at, cursor, visits[index].offset);
			if (parts == max_parts)
				return loop(at, cursor, offset);
			if (!load(at, p))
				return {};
			std::optional<u32> own;
			const bool noting = own_part && !committed_attack && !reaction && out;
			const auto found = follow(p, cursor, !own_part && !p.attack, noting ? &own : nullptr, nullptr);
			if (!found)
				return {};
			if (own && (!out->own || offset + *own < *out->own))
				out->own = offset + *own;
			visits[parts++] = {at, cursor, offset, found->left};
			if (found->is == course::kind::free)
				return stop{offset + found->cursor, found->at};
			if (found->is == course::kind::down)
			{
				if (out)
					note(*out, record(found->target));
				return finish(stop{offset + found->cursor, state::down}, visits, parts, committed_attack, out);
			}
			const u32 e = found->cursor;
			if (stun && property_flags(at, 1, true).value_or(e + 1) <= e)
				stun = false;
			if (const auto stance = neutral(found->target))
				return stop{offset + e + 1, *stance};
			const u32 next = record(found->target);
			if (floor_alias(found->target))
			{
				if (out)
					note(*out, next);
				return finish(stop{offset + e + 1, state::down}, visits, parts, committed_attack, out);
			}
			if (!next)
				return {};
			const auto entered = entry_cursor(next, found->data, e, found->execute, found->how);
			const u32 next_offset = offset + e + 1 - entered.cursor;
			if (next_offset > 32767)
				return {};
			if (stun && down_bits(word(next + 0x0c)))
			{
				const stop collapse{offset + e + 1, state::stun};
				if (out)
				{
					reading escapes;
					collect(visits, parts, committed_attack, collapse.free, escapes);
					if (escapes.escape)
					{
						copy_escapes(escapes, *out);
						note(*out, next);
						return collapse;
					}
				}
				stun = false;
			}
			// A transition that keeps the cursor continues the same motion in a twin part.
			own_part = own_part && entered.kept;
			if (out)
				note(*out, next);
			at = next;
			cursor = entered.cursor;
			offset = next_offset;
		}
	}

	template <class Word, class Half>
	auto readiness_walk<Word, Half>::collect(const std::array<visit, max_parts>& visits, std::size_t parts, bool committed_attack, u32 before, reading& out) const -> void
	{
		std::array<u32, 4> totals{};
		part p;
		for (std::size_t index = 0; index < parts; ++index)
		{
			const visit& place = visits[index];
			if ((index == 0 && committed_attack) || !load(place.at, p) || p.attack)
				continue;
			for (const entry& command : p.entries)
			{
				if (!command.input || !command.escape || command.data == 0x3fff || floor_alias(command.target))
					continue;
				const u32 taken = (std::max<u32>)(command.first, place.cursor), latest = (std::min<u32>)(command.last, place.left);
				const u32 target = record(command.target);
				if (taken > latest || !target || attack(target))
					continue;
				const u32 e = executes(command, taken, p.length);
				std::optional<stop> rest;
				entered_at entered;
				if (const auto stance = neutral(command.target))
					rest = stop{1, *stance};
				else
				{
					entered = entry_cursor(target, command.data, e, command.execute == 0xff ? p.length : command.execute, via::command);
					rest = run(target, entered.cursor, 0, false, nullptr);
				}
				if (!rest || rest->at == state::down || rest->at == state::stun)
					continue;
				const u32 total = place.offset + e + 1 - entered.cursor + rest->free;
				if (total > 32767 || total >= before)
					continue;
				add_route(out, totals, {target, rest->free, place.offset + executes(command, latest, p.length) + 1}, total);
			}
		}
	}
} // namespace pbd::move_list
