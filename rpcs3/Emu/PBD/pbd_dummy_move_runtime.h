#pragma once

#include "pbd_dummy_move_channel.h"
#include "pbd_dummy_move_input.h"
#include "pbd_env.h"
#include "pbd_practice_store.h"
#include "pbd_practice_move_list.h"
#include "../Cell/pbd_practice_native.h"
#include "Emu/Memory/vm.h"
#include "Emu/System.h"

#include <chrono>
#include <string>
#include <string_view>

namespace pbd::practice::dummy_move
{
	struct runtime
	{
		struct event
		{
			u32 kind = 0, counter = 0, id = 0, variant = 0, value = 0;
		};
		channel link;
		scheduler selections;
		contact_batch contacts;
		input_player play;
		std::optional<pbd::move_list::catalog> catalog;
		picker_catalog picker;
		std::array<u32, 6> identity{};
		std::uint64_t generation = 0;
		u32 epoch = 0, port = 1, counter = 0, contact_counter = 0;
		u32 open_sequence = 0;
		choice active_choice;
		std::array<event, 64> events{};
		u32 event_count = 0;
		std::optional<u32> prior_counter;
		std::optional<std::array<u32, 3>> recovery;
		settings configured;
		gate configured_gate;
		std::chrono::steady_clock::time_point next_poll{}, next_publish{};
		bool live = false;
		bool react_allowed = false;
		bool open_requested = false;
		bool start_reported = false;
	};
	inline thread_local runtime current;
	inline void observe(u32 kind, u32 counter, u32 value = 0)
	{
		if (current.event_count < current.events.size())
			current.events[current.event_count++] = {kind, counter, current.active_choice.id, current.active_choice.variant, value};
	}
	inline bool picker_available()
	{
		return current.live && current.react_allowed && current.catalog && !current.link.root.empty();
	}
	// The dummy's loaded move list can be shown and its moves chosen (Practice's in-game move list).
	inline bool catalog_available()
	{
		return current.live && current.catalog.has_value();
	}
	inline bool request_picker()
	{
		if (!picker_available())
			return false;
		current.open_requested = true;
		++current.open_sequence;
		return true;
	}
	inline void reset_playback()
	{
		current.play.stop();
		current.selections.reset();
		current.recovery.reset();
	}
	inline u32 native_word(u32 address)
	{
		be_t<u32> value{};
		return vm::try_access(address, &value, sizeof(value), false) ? u32{value} : ~0u;
	}
	inline u32 native_half(u32 address)
	{
		be_t<std::uint16_t> value{};
		return vm::try_access(address, &value, sizeof(value), false) ? std::uint16_t{value} : ~0u;
	}
	inline std::string utf8(std::u16string_view value)
	{
		std::string out;
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			u32 code = value[i];
			if (code >= 0xd800 && code <= 0xdbff)
			{
				if (i + 1 == value.size() || value[i + 1] < 0xdc00 || value[i + 1] > 0xdfff)
					return {};
				code = 0x10000 + ((code - 0xd800) << 10) + value[++i] - 0xdc00;
			}
			if (code < 0x80)
				out += static_cast<char>(code);
			else if (code < 0x800)
			{
				out += static_cast<char>(0xc0 | (code >> 6));
				out += static_cast<char>(0x80 | (code & 63));
			}
			else if (code < 0x10000)
			{
				out += static_cast<char>(0xe0 | (code >> 12));
				out += static_cast<char>(0x80 | ((code >> 6) & 63));
				out += static_cast<char>(0x80 | (code & 63));
			}
			else
			{
				out += static_cast<char>(0xf0 | (code >> 18));
				out += static_cast<char>(0x80 | ((code >> 12) & 63));
				out += static_cast<char>(0x80 | ((code >> 6) & 63));
				out += static_cast<char>(0x80 | (code & 63));
			}
		}
		return out;
	}
	inline const pbd::move_list::entry* find(u32 id)
	{
		if (!current.catalog)
			return nullptr;
		const auto& entries = current.catalog->entries;
		const auto found = std::find_if(entries.begin(), entries.end(), [id](const auto& row)
			{
				return row.id == id;
			});
		return found == entries.end() ? nullptr : &*found;
	}
	inline std::optional<variant_info> lookup(u32 id)
	{
		const auto row = std::find_if(current.picker.moves.begin(), current.picker.moves.end(),
			[id](const auto& value)
			{
				return value.id == id && value.selectable;
			});
		const auto* entry = find(id);
		if (row == current.picker.moves.end() || !entry)
			return {};
		return variant_info{entry->variants, entry->execution};
	}
	// One move of ids (picked at random when several, as a reaction picks) as the dummy's input words, written for a
	// dummy on the left as recorded takes are: a Defense slot or a recorded reaction plays them like a take. Nothing when
	// the loaded move list cannot supply one (another character, or a move the dummy cannot perform from standing).
	inline std::optional<std::vector<std::uint16_t>> move_words(std::vector<u32> ids, u32& rng)
	{
		if (!current.catalog || ids.empty() || ids.size() > 64)
			return {};
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		const auto picked = choose(reaction{source::moves, 0, std::move(ids)}, rng, lookup);
		const auto* row = picked ? find(picked->id) : nullptr;
		if (!row)
			return {};
		const auto command = pbd::move_list::read_command(current.identity[0], row->command_id + picked->variant, native_word, native_half);
		return command ? command_words(*command) : std::nullopt;
	}
	// The moves of the loaded list as Practice's move list shows them: the game's order, its category titles, each move's
	// name and command in Tekken notation, and whether the dummy can perform it.
	inline std::vector<move_list_view::row> list_rows()
	{
		std::vector<move_list_view::row> out;
		if (!current.catalog)
			return out;
		const auto& entries = current.catalog->entries;
		const auto& headers = current.catalog->headers;
		std::size_t next_header = 0;
		for (std::size_t at = 0; at < entries.size(); ++at)
		{
			for (; next_header < headers.size() && headers[next_header].position <= at; ++next_header)
				out.push_back({0, utf8(headers[next_header].name), {}, true, false});
			const auto& entry = entries[at];
			const auto picker = std::find_if(current.picker.moves.begin(), current.picker.moves.end(), [&](const auto& row)
				{
					return row.id == entry.id;
				});
			out.push_back({entry.id, utf8(entry.name), pbd::move_list::notation_text(entry.notation), false,
				picker != current.picker.moves.end() && picker->selectable, pbd::move_list::notation_glyphs(entry.notation)});
		}
		return out;
	}
	inline bool controls_dummy()
	{
		return current.live && current.react_allowed && current.play.active() && current.play.started;
	}
	inline std::optional<std::uint16_t> input(u32 port, bool allowed, bool on_right)
	{
		if (!controls_dummy())
			return {};
		if (!allowed && port == current.port)
			return std::uint16_t{0};
		auto word = current.play.input(port);
		if (!word)
			return {};
		if (current.play.permitted && !current.start_reported)
		{
			current.start_reported = true;
			observe(1, current.counter + 1, *word);
		}
		return on_right ? pbd::practice::movement::mirrored(*word) : *word;
	}
	inline void applied_contact(u32 counter, u32 defender, bool guard)
	{
		if (!current.live || defender != current.port)
			return;
		current.contacts.applied(counter, guard);
		if (!current.react_allowed || !current_settings.enabled())
			return;
		current.play.stop();
		current.recovery.reset();
		current.contact_counter = counter;
		current.selections.applied_contact(counter, guard ? contact::guard : contact::hit, current_settings, lookup, current_gate);
		if (current.selections.pending)
		{
			current.active_choice = *current.selections.pending;
			observe(0, counter, guard ? 0 : 1);
		}
	}
	inline void recovery(u32 counter, u32 side, u32 cursor, u32 first)
	{
		if (!current.live || side != current.port)
			return;
		// Every reaction after a hit or block starts from this reading, not only the moves (contact_batch::free_next).
		current.contacts.terminal(counter, cursor, first);
		if (counter < current.contact_counter || counter - current.contact_counter > 600)
			return;
		current.recovery = std::array<u32, 3>{counter, cursor, first};
		if (cursor <= first)
			current.selections.recovered(counter + first - cursor);
		current.play.recovery(counter, cursor, first);
	}
	inline void committed(u32 side, u32 counter, u32 move)
	{
		if (current.live && side == current.port && current.play.started && current.play.sent &&
			current.play.position >= current.play.first_attack && !current.play.accepted)
		{
			current.play.accepted = true;
			observe(2, counter, move);
		}
	}
	inline bool ensure_catalog(u32 bank, const std::array<u32, 6>& identity, std::uint64_t generation, u32 port)
	{
		if (generation != current.generation || identity != current.identity || !current.catalog)
		{
			current.live = false;
			current.play.stop();
			current.selections.reset();
			current.recovery.reset();
			current.contacts = {};
			current.catalog = pbd::move_list::read_catalog(bank, identity[1], native_word, native_half);
			if (!current.catalog)
				return false;
			current.identity = identity;
			current.generation = generation;
			current.port = port;
			++current.epoch;
			current.picker = {std::to_string(generation) + "-" + std::to_string(current.epoch), identity[1], identity[2]};
			for (const auto& row : current.catalog->entries)
			{
				bool selectable = row.execution && row.variants && row.variants <= 64 &&
				                  (row.conditions == 0 || row.conditions == 4 || row.conditions == 5 || row.conditions == 8 || row.conditions == 9) &&
				                  ((row.execution != 2 && row.execution != 4) || row.variants > 1);
				for (u32 variant = 0; selectable && variant < row.variants; ++variant)
				{
					const auto command = pbd::move_list::read_command(bank, row.command_id + variant, native_word, native_half);
					selectable = command && command_words(*command).has_value();
				}
				current.picker.moves.push_back({row.id, utf8(row.name), utf8(row.notation), selectable, row.variants});
			}
			current.next_publish = {};
		}
		return true;
	}

	inline void publish_picker(practice_store::writer& files, u32 port)
	{
		const char* configured = pbd::env::find("PBD_MENU_CHANNEL");
		if (configured && *configured)
		{
			const std::string_view encoded_path(configured);
			const std::filesystem::path root{std::u8string(encoded_path.begin(), encoded_path.end())};
			if (root.is_absolute() && !std::string_view(configured).starts_with("\\\\") && !std::string_view(configured).starts_with("//"))
			{
				current.link.root = root.parent_path() / "moves";
				const auto now = std::chrono::steady_clock::now();
				if (now >= current.next_poll)
				{
					current.next_poll = now + std::chrono::milliseconds(100);
					if (current.link.poll(current.picker))
						current.next_publish = {};
				}
				if (now >= current.next_publish)
				{
					current.next_publish = now + std::chrono::milliseconds(500);
					const auto player_row = native_half(0x10090040 + (1 - port) * 0x8d0 + 0x42);
					const auto text = current.link.status(current.picker, player_row);
					files.submit(current.link.root / "status.json", [text]
						{
							return text;
						});
				}
				if (current.open_requested)
				{
					using json::json_value;
					const auto text = json::detail::serialize(json_value::object({{"schema", json_value::string("pbd-practice-move-picker-1")},
						{"generation", json_value::string(current.picker.generation)},
						{"id", json_value::string(current.picker.generation + "-" + std::to_string(current.open_sequence))}}));
					files.submit(current.link.root / "open.json", [text]
						{
							return text;
						});
					current.open_requested = false;
				}
			}
		}
	}

	template <class Recording>
	void boundary(ppu_thread& ppu, bool live, bool react_allowed, bool paused, u32 port, u32 counter, bool advanced,
		practice_store::writer* files, Recording recording)
	{
		if (!files || !live || port > 1 || !pbd::practice::native::boundary_readable(ppu))
		{
			current.live = false;
			current.play.stop();
			current.selections.reset();
			current.prior_counter.reset();
			current.contacts = {};
			return;
		}
		const auto generation = static_cast<std::uint64_t>(Emu.GetEmulationIdentifier());
		const u32 actor = 0x10090040 + port * 0x8d0, bank = native_word(actor + 0x50);
		if (bank < 0x10000000 || bank > 0x7ffffd00)
		{
			current.live = false;
			reset_playback();
			return;
		}
		const std::array<u32, 6> identity{bank, native_half(actor + 0x40), native_half(actor + 0x42),
			native_word(bank + 0x218), native_word(bank + 0x21c), port};
		if (native_word(actor + 0x5c) != 0x10090040 + (1 - port) * 0x8d0 || native_half(actor + 0x3c) != port ||
			identity[1] >= 35 || identity[2] >= 85)
		{
			current.live = false;
			current.play.stop();
			return;
		}
		if (!ensure_catalog(bank, identity, generation, port))
			return;
		if (current.prior_counter && counter != *current.prior_counter && counter != *current.prior_counter + 1)
		{
			current.play.stop();
			current.selections.reset();
			current.recovery.reset();
			current.contacts = {};
		}
		current.prior_counter = counter;
		current.counter = counter;
		current.live = true;
		current.react_allowed = react_allowed;
		if (current.play.active() && counter - current.contact_counter > 600)
			reset_playback();
		publish_picker(*files, port);
		if (current.configured != current_settings || current.configured_gate != current_gate)
		{
			current.configured = current_settings;
			current.configured_gate = current_gate;
			current.play.stop();
			current.selections.reset();
			current.recovery.reset();
		}
		current.selections.advance(counter, current_gate);
		if (!react_allowed)
		{
			reset_playback();
			return;
		}
		if (current.selections.pending)
		{
			const auto selected = std::exchange(current.selections.pending, {});
			current.start_reported = false;
			std::optional<std::vector<std::uint16_t>> words;
			if (selected->kind == source::recording)
				words = recording(selected->slot);
			else if (const auto* row = find(selected->id))
			{
				const auto command = pbd::move_list::read_command(bank, row->command_id + selected->variant, native_word, native_half);
				if (command)
					words = command_words(*command);
			}
			if (words && current.play.arm(std::move(*words), port) && current.recovery)
				current.play.recovery((*current.recovery)[0], (*current.recovery)[1], (*current.recovery)[2]);
		}
		current.play.boundary(counter, advanced, !paused);
	}
} // namespace pbd::practice::dummy_move
