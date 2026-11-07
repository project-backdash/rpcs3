#pragma once

#include "pbd_costume_select.h"
#include "pbd_custom_save.h"
#include "pbd_custom_files.h"
#include "pbd_custom_photo.h"

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

namespace pbd::costume_panel
{
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	constexpr bool requested(bool set, bool practice, bool customize, bool offline = false) noexcept
	{
		return set || practice || customize || offline;
	}
	inline std::string_view fighter_name(u32 row)
	{
		constexpr std::array<std::string_view, 35> names{
			"Paul Phoenix", "Marshall Law", "Lei Wulong", "King", "Yoshimitsu", "Nina Williams", "Hwoarang",
			"Ling Xiaoyu", "Christie Monteiro", "Jin Kazama", "Julia Chang", "Kuma", "Bryan Fury", "Heihachi Mishima",
			"Kazuya Mishima", "Lee Chaolan", "Steve Fox", "Craig Marduk", "Mokujin", "JACK-5", "Roger Jr.",
			"Anna Williams", "Wang Jinrei", "Ganryu", "Asuka Kazama", "Bruce Irvin", "Baek Doo San", "Devil Jin",
			"Raven", "Feng Wei", "Armor King", "Lili", "Sergei Dragunov", "Eddy Gordo", "Jinpachi Mishima"};
		const auto* entry = custom_look::catalogue_row(row);
		return row == 31 ? "Panda" : entry && entry->fighter < names.size() ? names[entry->fighter] :
		                                                                      "";
	}
	inline std::string label(const costume_select::card& card)
	{
		const auto* row = custom_look::catalogue_row(card.look.row);
		const u32 number = card.slot ? *card.slot + 1 : row ? row->costume + 1 :
		                                                      1;
		if (!card.display_name.empty())
			return card.display_name;
		return std::string(card.slot ? "CUSTOM " : "DEFAULT ") + (number < 10 ? "0" : "") + std::to_string(number);
	}
	struct snapshot
	{
		u64 generation = 0;
		bool visible = false, open = false;
		u32 side = 0, focus = 0;
		std::vector<costume_select::card> cards;
		bool editor = false, save_ready = false, overwrite = false, replace = false;
		u32 save_slot = 1, save_row = 0;
		bool operator==(const snapshot&) const = default;
	};
	inline snapshot compose(u64 generation, u32 side, const costume_select::row& menu)
	{
		return {generation, side < 2 && !menu.choices().empty(), menu.open(), side, menu.focus(), menu.choices()};
	}
	namespace detail
	{
		inline std::mutex mutex;
		inline snapshot current;
		inline std::atomic<u64> serial{0};
		struct picture_cache
		{
			std::filesystem::path index;
			u64 generation = 0, revision = 0;
			bool open = false;
			std::vector<custom_wire::drawn> identities;
			std::vector<std::vector<std::uint8_t>> pictures;
			void decorate(snapshot& value, const std::filesystem::path& file)
			{
				if (!file.is_absolute() || file.filename() != "picture-looks.json")
					return;
				std::vector<custom_wire::drawn> keys;
				for (const auto& card : value.cards)
					keys.push_back({card.slot ? *card.slot + 1 : 0, custom_wire::pack(card.look)});
				const auto saved = custom_photo::published_pictures.load(std::memory_order_acquire);
				// Reload on opening, identity changes or atomic publication, keeping disk reads out of ordinary pad samples.
				if (file != index || generation != value.generation || saved != revision || keys != identities || (value.open && !open))
				{
					custom_files::read_card_pictures(file, value.cards);
					pictures.clear();
					for (const auto& card : value.cards)
						pictures.push_back(card.picture);
					index = file;
					generation = value.generation;
					revision = saved;
					identities = std::move(keys);
				}
				else
					for (std::size_t i = 0; i < value.cards.size(); ++i)
						value.cards[i].picture = pictures[i];
				open = value.open;
			}
		};
		inline picture_cache local_pictures;
	} // namespace detail
	inline void publish(snapshot value)
	{
		std::lock_guard lock(detail::mutex);
		detail::local_pictures.decorate(value, env::path("PBD_CUSTOM_PICTURES"));
		if (value == detail::current)
			return;
		detail::current = std::move(value);
		detail::serial.fetch_add(1, std::memory_order_release);
	}
	inline void publish_editor(u64 generation, const custom_editor::save_panel& panel)
	{
		snapshot view;
		view.generation = generation;
		view.visible = view.editor = panel.open();
		view.save_ready = panel.can_save();
		view.overwrite = panel.overwrite();
		view.replace = panel.replace_selected();
		view.save_slot = panel.slot();
		view.save_row = panel.row();
		publish(std::move(view));
	}
	inline snapshot load()
	{
		std::lock_guard lock(detail::mutex);
		return detail::current;
	}
	inline u64 revision()
	{
		return detail::serial.load(std::memory_order_acquire);
	}
} // namespace pbd::costume_panel
