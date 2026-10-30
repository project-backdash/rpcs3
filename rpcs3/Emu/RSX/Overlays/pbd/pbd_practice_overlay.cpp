#include "stdafx.h"
#include "pbd_overlays.h"
#include "overlay_pbd_draw.h"
#include "Emu/System.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>

// Practice's HUD over the fight: the command history, Attack Info and Frame Data, toasts and the recording band.

namespace rsx
{
	namespace overlays
	{
		void pbd_practice_overlay::init()
		{
			// Pack the fixed font sizes and build the atlas now rather than on the first Practice frame.
			pbd::preload_fonts();
			pbd::assets::get();
			m_compiled.clear();
			visible = false;
		}

		void pbd_practice_overlay::set_render_viewport(u16 width, u16 height)
		{
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		using namespace pbd;

		struct practice_hud_drawing
		{
			const ::pbd::practice::snapshot& snap;
			const fonts& type;
			atlas_ref atlas;
			layer out;

			void prompts()
			{
				// The latch sends no control bar while Control Hints is Hide; the scrim goes with it.
				if (snap.hud_visible && snap.prompts.count)
				{
					out.image(layer::b_under, &assets::get().scrim, white, 0.f, 628.f, overlay::virtual_width, overlay::virtual_height);
					std::array<prompt_item, ::pbd::practice::max_prompts> prompts{};
					const u32 count = std::min<u32>(snap.prompts.count, static_cast<u32>(prompts.size()));
					for (u32 i = 0; i < count; i++)
						prompts[i] = {snap.prompts.items[i].keys, snap.prompts.items[i].label};
					prompt_bar(out, atlas, type.caption, 56.f, 693.f, 1224.f, std::span<const prompt_item>(prompts.data(), count));
				}
			}

			void history()
			{
				if (snap.hud_visible)
				{
					// Command history: no panel, columns at the screen edges, newest on top (shared with Replay and Analyze). Each
					// side has its own Display Settings row. Arrows are the screen directions the stick was pushed.
					for (usz side = 0; side < 2; side++)
					{
						if (!snap.command_history[side])
							continue;
						const auto& history = snap.inputs[side];
						history_column(out, atlas, side, std::min<u32>(history.count, static_cast<u32>(::pbd::practice::history_size)),
							[&](u32 row)
							{
								return history_row{history.rows[row].mask, history.rows[row].frames};
							});
					}
				}
			}

			void block(const ::pbd::practice::panel_rect& rect, std::span<const ::pbd::practice::panel_line> lines, usz observed)
			{
				const bool columns = observed < lines.size();
				out.rect(layer::b_under, rgb(0x000000, .34f), rect.x, rect.y, rect.w, rect.h);
				out.rect(layer::b_lines, top_edge, rect.x, rect.y, rect.w, 1.f);
				out.rect(layer::b_lines, hairline, rect.x, rect.y + rect.h - 1.f, rect.w, 1.f);
				if (columns)
					out.rect(layer::b_lines, hairline, rect.x + rect.w / 2.f, rect.y + 4.f, 1.f, rect.h - 8.f);
				for (usz i = 0; i < lines.size(); i++)
				{
					const auto cell = ::pbd::practice::panel_line_rect(rect, i, observed, lines.size());
					const f32 centre = cell.y + cell.h / 2.f;
					const bool unknown = lines[i].value == ::pbd::practice::unknown_value;
					auto label = lines[i].label;
					if (columns && label == "Attack Startup Frames")
						label = "Startup";
					if (columns && label == "Reference Recovery")
						label = "Ref Rec.";
					if (columns && label == "Reference On Block")
						label = "Ref Block";
					if (columns && label == "Reference On Hit")
						label = "Ref Hit";
					const f32 inset = columns ? 6.f : 11.f;
					out.text(type.caption, text_2, cell.x + inset, baseline(type.caption, centre), pbd_panel_label(type.caption, lines[i], label, cell.w - 2.f * inset));
					const auto value = utf8_to_u32string(lines[i].value);
					const f32 value_x = cell.x + cell.w - inset - text_width(type.caption, value), value_y = baseline(type.caption, centre);
					const usz marked_at = std::min<usz>(lines[i].marked_at, value.size());
					const usz marked_size = std::min<usz>(lines[i].marked_size, value.size() - marked_at);
					if (!marked_size)
						out.text(type.caption, unknown ? text_3 : text_1, value_x, value_y, value);
					else
					{
						// The branch the fighter with the choice took (a stun break, a tech, or neither) in gold.
						const std::u32string_view whole = value, head = whole.substr(0, marked_at), mark = whole.substr(marked_at, marked_size);
						const f32 mark_x = value_x + text_width(type.caption, head);
						out.text(type.caption, text_1, value_x, value_y, head);
						out.text(type.caption, gold, mark_x, value_y, mark);
						out.text(type.caption, text_1, mark_x + text_width(type.caption, mark), value_y, whole.substr(marked_at + marked_size));
					}
				}
			}

			void panels()
			{
				if (snap.hud_visible && snap.fight_live && (snap.attack_info || snap.frame_data))
				{
					// Attack Info beside Frame Data along the bottom (pbd::practice::place_panels): P1's attacks at the left, P2's at
					// the right, clear of the game's combo counter and of the command history columns, on screen for as long as
					// Display Settings has them on. A value no reader has established reads "---". A colourless panel with a light
					// shade keeps them readable without tinting the fight.
					for (usz side = 0; side < 2; side++)
					{
						const auto& fighter = snap.fighters[side];
						const auto placed = ::pbd::practice::place_panels(side, snap.command_history[side], snap.attack_info, snap.frame_data);
						if (placed.attack_info)
						{
							const auto lines = ::pbd::practice::attack_info_text(fighter);
							block(*placed.attack_info, lines, lines.size());
						}
						if (placed.frame_data)
						{
							const auto lines = ::pbd::practice::frame_data_text(fighter);
							block(*placed.frame_data, lines, ::pbd::practice::frame_observed_lines);
						}
					}
				}
			}

			void toast()
			{
				if (!snap.toast.empty())
				{
					// Toast: a short top-centre band; title, then an optional detail after a thin separator.
					std::string title = snap.toast, detail;
					for (auto& c : title)
						if (c == '\n')
							c = '\t';
					if (const auto tab = title.find('\t'); tab != std::string::npos)
					{
						detail = title.substr(tab + 1);
						title.resize(tab);
					}
					for (auto& c : detail)
						if (c == '\t')
							c = ' ';
					const rich_style title_style{type.caption, text_1, 16.f}, detail_style{type.body, text_2, 16.f};
					const f32 title_w = rich_text(nullptr, atlas, title_style, 0.f, 173.f, title);
					const f32 detail_w = detail.empty() ? 0.f : rich_text(nullptr, atlas, detail_style, 0.f, 173.f, detail);
					const f32 content = title_w + (detail.empty() ? 0.f : 9.f + 1.f + 9.f + detail_w);
					const f32 left = std::round(640.f - content / 2.f) - 72.f, right = left + content + 144.f;
					band_strip(out, layer::b_under, band_fill, left, right, 156.f, 190.f);
					band_strip(out, layer::b_lines, rgb(0xE9EEED, .16f), left, right, 156.f, 157.f);
					band_strip(out, layer::b_lines, rgb(0xE3304F, .75f), left, right, 188.f, 190.f);
					f32 x = left + 72.f;
					x += rich_text(&out, atlas, title_style, x, 173.f, title);
					if (!detail.empty())
					{
						out.rect(layer::b_lines, separator, x + 9.f, 166.f, 1.f, 14.f);
						rich_text(&out, atlas, detail_style, x + 19.f, 173.f, detail);
					}
				}
			}

			void recording_band(u64 timestamp_us)
			{
				// Recording or playback band, top centre: status dot, state, timer (recording only) and the slot.
				// Ready to record shows the full 30 s and the control tag before the take starts; only recording blinks and drains.
				const bool recording = snap.dummy == ::pbd::practice::dummy_phase::recording;
				const bool ready = snap.dummy == ::pbd::practice::dummy_phase::ready;
				const bool timed = recording || ready;
				const auto state = utf8_to_u32string(recording ? "Recording" : ready                                           ? "Ready to Record" :
																		   snap.dummy == ::pbd::practice::dummy_phase::preview ? "Preview" :
																																 "Playing");
				const auto slot = utf8_to_u32string(snap.dummy_slot);
				const u32 limit = std::max<u32>(1, snap.dummy_limit), frames = std::min(snap.dummy_frames, limit);
				const auto seconds = utf8_to_u32string(std::to_string((limit - frames + 59) / 60));
				constexpr f32 gap = 10.f, timer_box = 44.f, centre = 111.f;
				const f32 content = 9.f + gap + text_width(type.caption, state) + gap + 1.f + gap +
				                    (timed ? timer_box + gap + 1.f + gap : 0.f) + text_width(type.caption, slot);
				const f32 left = std::round(640.f - content / 2.f) - 72.f, right = left + content + 144.f;
				band_strip(out, layer::b_under, band_fill, left, right, 92.f, 130.f);
				band_strip(out, layer::b_lines, rgb(0xE9EEED, .16f), left, right, 92.f, 93.f);
				f32 x = left + 72.f;
				const f32 blink = recording && (timestamp_us / 500000) % 2 ? .35f : 1.f;
				out.sprite(atlas, recording ? pbd_glyphs::rec_dot : pbd_glyphs::rec_ring, x, centre - 4.5f, 9.f, rgb(0xFFFFFF, blink));
				x += 9.f + gap;
				x += out.text(type.caption, timed ? text_1 : text_2, x, baseline(type.caption, centre), state) + gap;
				out.rect(layer::b_lines, separator, x, centre - 8.f, 1.f, 16.f);
				x += 1.f + gap;
				if (timed)
				{
					out.text(type.timer, text_1, x + (timer_box - text_width(type.timer, seconds)) / 2.f, baseline(type.timer, centre), seconds);
					x += timer_box + gap;
					out.rect(layer::b_lines, separator, x, centre - 8.f, 1.f, 16.f);
					x += 1.f + gap;
				}
				out.text(type.caption, text_2, x, baseline(type.caption, centre), slot);
				if (timed)
				{
					// Progress along the band's bottom edge, between the ramps; it drains as the 30 s run out.
					const f32 track = right - left - 144.f;
					out.rect(layer::b_lines, rgb(0xE9EEED, .16f), left + 72.f, 128.f, track, 2.f);
					out.rect(layer::b_lines, crimson, left + 72.f, 128.f, track * (limit - frames) / limit, 2.f);
					pbd::tag(out, type.caption, crimson, white, 1252.f, true, 124.f, U"You control P2");
				}
			}

			void draw(compiled_resource& compiled, bool band, u64 timestamp_us)
			{
				prompts();
				history();
				panels();
				toast();
				if (band)
					recording_band(timestamp_us);
				out.flush(compiled);
			}
		};

		void pbd_practice_overlay::refresh_now(u64 timestamp_us)
		{
			m_snapshot = ::pbd::practice::load();
			// Recording and preview keep the top-centre band; playback runs without one.
			const bool band = m_snapshot.dummy != ::pbd::practice::dummy_phase::idle && m_snapshot.dummy != ::pbd::practice::dummy_phase::playing;
			m_show = m_snapshot.active && !Emu.IsStopped() && (m_snapshot.hud_visible || !m_snapshot.toast.empty() || band);
			visible = m_show;
			m_compiled.clear();
			// The pause menu draws its own control bar and toasts; nothing of the HUD shows through it.
			if (!m_show || m_snapshot.native_menu)
				return;
			assets::get();
			static const fonts type = preload_fonts();
			practice_hud_drawing draw{m_snapshot, type, atlas_ref::pick(m_output_scale, pbd_xbox_glyphs())};
			draw.draw(m_compiled, band, timestamp_us);
		}

		void pbd_practice_overlay::update(u64 timestamp_us)
		{
			if (Emu.IsStopped())
			{
				m_show = false;
				visible = false;
				return;
			}
			// Counts and the recording timer follow the newly presented simulation frame, including slow motion and Restart.
			refresh_now(timestamp_us);
		}

		compiled_resource pbd_practice_overlay::get_compiled()
		{
			if (!visible || !m_show || Emu.IsStopped())
				return {};
			return m_compiled;
		}
	} // namespace overlays
} // namespace rsx
