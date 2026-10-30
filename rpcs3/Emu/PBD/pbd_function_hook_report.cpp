#include "stdafx.h"
#include "pbd_function_hook_state.h"
#include <stdexcept>

namespace pbd::fn_hook
{
	using namespace detail;
	namespace
	{
		struct hook_report
		{
			config& c;
			const std::filesystem::path& directory;
			bool timeline;
			std::string summary, fences, draws;
			void publish(const std::u8string& path, std::string_view text)
			{
				const std::string name(path.begin(), path.end());
				fs::file out(name, fs::rewrite);
				if (!out)
					throw std::runtime_error(fmt::format("cannot open %s", name));
				if (out.write(text.data(), text.size()) != text.size())
					throw std::runtime_error(fmt::format("short write to %s", name));
			}

			void events()
			{
				std::string text = timeline ? "address,tick,replay,skipped,ns,reg0,reg1,cyc\n" : "address,tick,replay,skipped,ns,reg0,reg1\n";
				text.reserve(c.used * 64 + 256);
				for (usz i = 0; i < c.used; i++)
				{
					const auto& r = c.events[i];
					fmt::append(text, "0x%x,%u,%u,%u,%u,0x%x,0x%x", r.address, r.tick, r.flags & 1u, (r.flags >> 1) & 1u, r.ns, r.reg0, r.reg1);
					if (timeline)
						fmt::append(text, ",%u", r.cyc);
					text += '\n';
				}
				// PBD_PPU_JIT_MAP=1 (or the owner timeline): host entry -> guest address map of the recompiled PPU code (sampling attribution).
				if (timeline || parse_number("PBD_PPU_JIT_MAP", 0) == 1)
				{
					const auto jpath = (directory / "ppu-jit-map.csv").u8string();
					::pbd_dump_ppu_jit_map(std::string(jpath.begin(), jpath.end()));
				}
				const auto path = (directory / "fn-probe.csv").u8string();
				publish(path, text);
				if (timeline)
				{
					// Owner timeline (pbd_owner_timeline.h): ns on the steady clock's epoch; fn-probe ns = ns - origin_steady_ns.
					const u64 n = std::min<u64>(pbd::owner_timeline::used.load(), pbd::owner_timeline::ring.size());
					std::string tl = "kind,a,ns,b,c\n";
					tl.reserve(n * 40 + 64);
					for (u64 i = 0; i < n; i++)
					{
						const auto& e = pbd::owner_timeline::ring[i];
						fmt::append(tl, "%u,0x%x,%u,%u,%d\n", e.kind, e.a, e.ns, e.b, static_cast<s64>(e.c));
					}
					const auto tpath = (directory / "owner-timeline.csv").u8string();
					publish(tpath, tl);
				}
			}

			void base_summary()
			{
				summary = fmt::format("{\"origin_steady_ns\":%u,\"tsc_freq\":%u,\"owner_timeline\":%s,\"owner_timeline_used\":%u,\"owner_timeline_dropped\":%u,",
					c.origin_set ? static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(c.origin.time_since_epoch()).count()) : 0ull,
					utils::get_tsc_freq(), timeline ? "true" : "false", pbd::owner_timeline::used.load(), pbd::owner_timeline::dropped.load());
				summary += fmt::format("\"calls\":%u,\"skips\":%u,\"skip_ignored\":%u,\"skip_vetoed\":%u,\"gate_player_skipped\":%u,\"gate_player_vetoed\":%u,\"gate_player_live_calls\":%u,\"gate_player_live_would_veto\":[%u,%u,%u],\"gate_player_veto_reason\":[%u,%u,%u],\"gate_player_first_live_veto_tick\":%d,\"gate_stage_skipped\":%u,\"gate_stage_vetoed\":%u,\"gate_stage_live_mode2\":%u,\"test_stage_camera_forced\":%u,\"gate_stage_first_mode2_tick\":%d,\"gate_effects_skipped\":%u,\"gate_effects_vetoed\":%u,\"gate_effects_live_calls\":%u,\"gate_effects_live_would_veto\":[%u,%u,%u],\"gate_effects_veto_reason\":[%u,%u,%u],\"foreign\":%u,\"events\":%u,\"overflow\":%s,\"probes\":[",
					c.calls, c.skips, c.skip_ignored, c.skip_vetoed, c.gate_player_skipped, c.gate_player_vetoed, c.gate_player_live_calls, c.gate_player_live_would_veto[1], c.gate_player_live_would_veto[2], c.gate_player_live_would_veto[3], c.gate_player_veto_reason[1], c.gate_player_veto_reason[2], c.gate_player_veto_reason[3], static_cast<s32>(c.gate_player_first_live_veto_tick), c.gate_stage_skipped, c.gate_stage_vetoed, c.gate_stage_live_mode2, c.test_stage_camera_forced, static_cast<s32>(c.gate_stage_first_mode2_tick), c.gate_effects_skipped, c.gate_effects_vetoed, c.gate_effects_live_calls, c.gate_effects_live_would_veto[1], c.gate_effects_live_would_veto[2], c.gate_effects_live_would_veto[3], c.gate_effects_veto_reason[1], c.gate_effects_veto_reason[2], c.gate_effects_veto_reason[3], c.foreign.load(), c.used, c.overflow ? "true" : "false");
				bool first = true;
				for (u32 a : c.observed_sites)
				{
					fmt::append(summary, "%s\"0x%x\"", first ? "" : ",", a);
					first = false;
				}
				summary += "],\"skippable\":[";
				first = true;
				for (u32 a : c.skippable)
				{
					fmt::append(summary, "%s\"0x%x\"", first ? "" : ",", a);
					first = false;
				}
				summary += "],\"skip\":[";
				first = true;
				for (u32 a : c.skip)
				{
					fmt::append(summary, "%s\"0x%x\"", first ? "" : ",", a);
					first = false;
				}
				summary += "],\"skip_tasks\":[";
				first = true;
				for (u32 a : c.skip_tasks)
				{
					fmt::append(summary, "%s\"0x%x\"", first ? "" : ",", a);
					first = false;
				}
				summary += "],\"dispatch\":[";
				first = true;
				for (const auto& d : c.dispatch)
				{
					fmt::append(summary, "%s\"0x%x:%x\"", first ? "" : ",", d.first, d.second);
					first = false;
				}
				fmt::append(summary, "],\"task_dispatch\":\"0x%x\",\"regs\":[%d,%d],\"rng_repair\":\"0x%x\",\"rng_recorded\":%u,\"rng_applied\":%u,\"rng_missing\":%u,\"variant\":\"%s\"",
					c.dispatch.empty() ? 0u : c.dispatch[0].first, c.reg0, c.reg1, random_runtime().rng_repair,
					random_runtime().rng_recorded, random_runtime().rng_applied, random_runtime().rng_missing, c.tag);
			}

			void fence_summary()
			{
				for (const auto& f : random_runtime().fence)
					fmt::append(fences, "%s\"0x%x:0x%x\"", fences.empty() ? "" : ",", f.first, f.second);
				for (u32 n = 0; n < 66; n++)
					if (random_runtime().fence_draws[n])
						fmt::append(draws, "%s\"%u\":%u", draws.empty() ? "" : ",", n, random_runtime().fence_draws[n]);
				fmt::append(summary, ",\"keyed_in_replay\":%u,\"thread_detaches\":%u", keyed_in_replay.load(), thread_detaches.load());
			}

			void voice_summary()
			{
				fmt::append(summary, ",\"voice_test_delay_us\":%u,\"voice_test_delays\":%u,\"voice_test_stale\":%u", voice_runtime().test_delay_us, voice_runtime().test_delays.load(), voice_runtime().test_stale.load());
				fmt::append(summary, ",\"voice_test_rendezvous_us\":%u,\"voice_test_rendezvous_tick\":%u,\"voice_test_rendezvous_entered\":%u,\"voice_test_rendezvous_released\":%u",
					voice_runtime().test_rendezvous_us, voice_runtime().test_rendezvous_tick, voice_runtime().test_rendezvous_entered.load(), voice_runtime().test_rendezvous_released.load());
				fmt::append(summary, ",\"voice_lock\":%s,\"voice_claims\":%u,\"voice_contentions\":%u,\"voice_timeouts\":%u,\"voice_faults\":%u,\"voice_fail_stops\":%u,\"voice_restores\":%u,\"voice_restore_refused\":%u,\"voice_activity\":%u", voice_runtime().lock ? "true" : "false", voice_runtime().claims.load(), voice_runtime().contentions.load(), voice_runtime().timeouts.load(), voice_runtime().faults.load(), voice_runtime().fail_stops.load(), voice_runtime().restores.load(), voice_runtime().restore_refused.load(), voice_runtime().activity.load());
				fmt::append(summary, ",\"voice_fatal_timeout\":%s,\"voice_fatal_deadline_ms\":%u,\"voice_fatal_armed\":%s,\"voice_fatal_thread\":%u,\"voice_fatal_instruction\":\"0x%x\",\"voice_fatal_outcomes\":%u,\"voice_fatal_late_events\":%u,\"voice_fatal_watchdog_exits\":%u",
					voice_runtime().fatal_timeout ? "true" : "false", voice_runtime().fatal_deadline_ms, voice_runtime().fatal_armed.load() ? "true" : "false",
					voice_runtime().fatal_thread.load(), voice_runtime().fatal_instruction.load(), voice_runtime().fatal_outcomes.load(),
					voice_runtime().fatal_late_events.load(), voice_runtime().fatal_watchdog_exits.load());
				fmt::append(summary, ",\"voice_test_drop_main_stop\":%s,\"voice_main_stop_dropped\":%u",
					voice_runtime().test_drop_main_stop ? "true" : "false", voice_runtime().main_stop_dropped.load());
				fmt::append(summary, ",\"voice_restore_waits\":%u,\"voice_foreign_claim_rewound\":%u,\"voice_balance_errors\":%u", voice_runtime().restore_waits.load(), voice_runtime().foreign_claim_rewound.load(), voice_runtime().balance_errors.load());
				{
					std::lock_guard lock(voice_runtime().lease_mutex);
					const auto& health = voice_runtime().health;
					fmt::append(summary, ",\"voice_allocation_monitor\":true,\"voice_allocation_successes\":%u,\"voice_allocation_failures\":%u,\"voice_allocation_max_failure_streak\":%u,\"voice_pool_exhaustions\":%u",
						health.successes, health.failures, health.max_streak, health.exhausted);
					fmt::append(summary, ",\"voice_foreign_active\":%u,\"voice_foreign_sequence\":%u,\"voice_foreign_events_overflow\":%s", voice_runtime().foreign_active, voice_runtime().foreign_sequence, voice_runtime().foreign_sequence > voice_runtime().foreign_events.size() ? "true" : "false");
					std::string events = "{\"schema\":1,\"events\":[";
					for (const auto& e : voice_runtime().foreign_events)
						fmt::append(events, "%s{\"sequence\":%u,\"thread\":%u,\"tick\":%u,\"replay\":%u,\"kind\":\"%s\",\"slot\":%u}", e.sequence == 1 ? "" : ",", e.sequence, e.thread, e.tick, e.replay, e.kind ? "leave" : "enter", e.slot);
					events += "]}\n";
					const auto marker_path = (directory / "voice-foreign-claims.json").u8string();
					publish(marker_path, events);
				}
			}

			void render_summary()
			{
				fmt::append(summary, ",\"skip_guard\":%s,\"skip_guard_record\":%s", c.skip_guard ? "true" : "false", c.skip_guard_record ? "true" : "false");
				const auto frame_tags = pbd::frame_presentation::frame_tags.snapshot();
				fmt::append(summary, ",\"frame_tags_enabled\":%s,\"frame_tags_produced\":%u,\"frame_tags_consumed\":%u,\"frame_tags_faults\":%u,\"frame_tags_pending\":%u,\"frame_tags_unaligned\":%u,\"frame_tags_overwrites\":%u,\"frame_tags_aligned\":%s,\"frame_tags_identity\":\"fifo_command_address\"",
					pbd::frame_presentation::queue_tags_enabled() ? "true" : "false", frame_tags.produced, frame_tags.consumed, frame_tags.faults, frame_tags.pending,
					frame_tags.unaligned, frame_tags.overwrites, frame_tags.aligned ? "true" : "false");
				fmt::append(summary, ",\"skip_guard_subset\":%s,\"skip_guard_entries\":[", c.skip_guard_subset ? "true" : "false");
				for (usz i = 0; i < c.guard_entries.size(); i++)
					fmt::append(summary, "%s\"0x%x\"", i ? "," : "", c.guard_entries[i]);
				summary += "]";
				fmt::append(summary, ",\"fence\":[%s],\"fence_opened\":%u,\"fence_restored\":%u,\"fence_idle\":%u,\"fence_faults\":%u,\"fence_draws\":{%s},\"test_early_flushes\":%u,\"test_early_flush_refused\":%u}\n",
					fences, random_runtime().fence_opened, random_runtime().fence_restored, random_runtime().fence_idle, random_runtime().fence_faults, draws, c.test_early_flushes, c.test_early_flush_refused);
			}

			void write_summary()
			{
				const auto spath = (directory / "fn-probe-summary.json").u8string();
				publish(spath, summary);
			}

			void skip_guard()
			{
				if (c.skip_guard_record)
				{
					std::string decisions, entries, eligible;
					for (u32 i = 0; i < skip_guard_history::count; i++)
						fmt::append(decisions, "%s\"%s\":%u", i ? "," : "", skip_guard_history::names[i], c.guard_history.decisions[i]);
					for (const auto& [entry, n] : c.guard_history.skipped_entries)
						fmt::append(entries, "%s\"0x%x\":%u", entries.empty() ? "" : ",", entry, n);
					for (u32 entry : c.guard_entries)
						fmt::append(eligible, "%s\"0x%x\"", eligible.empty() ? "" : ",", entry);
					const auto text = fmt::format("{\"schema\":1,\"enabled\":%s,\"key\":\"tick,entry,LR,task_or_object_r3\",\"history_ticks\":256,\"eligible\":[%s],\"decisions\":{%s},\"skipped_entries\":{%s},\"live_calls\":%u,\"overflows\":%u,\"abandoned\":%u,\"test_taint\":%s,\"test_missing\":%s}\n",
						c.skip_guard ? "true" : "false", eligible, decisions, entries, c.guard_history.live_calls, c.guard_history.overflows,
						c.guard_history.abandoned, c.skip_guard_test_taint ? "true" : "false", c.skip_guard_test_missing ? "true" : "false");
					const auto path = (directory / "fn-skip-guard.json").u8string();
					publish(path, text);
				}
			}

			void random_audit()
			{
				if (!random_runtime().rng_audit.empty())
				{
					std::string rows;
					for (const auto& [key, s] : random_runtime().audit_stats)
						fmt::append(rows, "%s{\"entry\":\"0x%x\",\"match\":%u,\"calls\":%u,\"matched\":%u,\"unmatched\":%u,\"advanced\":%u,\"steps\":%u,\"max_steps\":%u,\"drew_rand\":%u,\"drew_lcg\":%u,\"drew_mt\":%u,\"first_advanced_tick\":%d}",
							rows.empty() ? "" : ",\n", key.first, key.second, s.calls, s.matched, s.unmatched, s.advanced, s.steps, s.max_steps, s.drew_rand, s.drew_lcg, s.drew_mt,
							s.first_advanced_tick == ~0u ? -1 : static_cast<s32>(s.first_advanced_tick));
					std::string returns;
					for (const auto& [entry, set] : random_runtime().audit_returns)
					{
						std::string list;
						for (u32 a : set)
							fmt::append(list, "%s\"0x%x\"", list.empty() ? "" : ",", a);
						fmt::append(returns, "%s\"0x%x\":[%s]", returns.empty() ? "" : ",", entry, list);
					}
					std::string mt;
					for (const auto& [caller, n] : random_runtime().mt_in_fight)
						fmt::append(mt, "%s\"0x%x\":%u", mt.empty() ? "" : ",", caller, n);
					const auto text = fmt::format("{\"audit\":[\n%s\n],\"returns\":{%s},\"overflow\":%u,\"unreadable\":%u,\"open_at_end\":%u,\"mt_in_fight\":{%s}}\n",
						rows, returns, random_runtime().audit_overflow, random_runtime().audit_unreadable, random_runtime().audit_stack.size(), mt);
					const auto apath = (directory / "fn-rng-audit.json").u8string();
					publish(apath, text);
				}
			}
		};
	} // namespace
	void write_report(const std::filesystem::path& given) noexcept
	{
		auto& c = cfg();
		if (!requested())
			return;
		auto expected = config::report_outcome::unattempted;
		if (!c.report.compare_exchange_strong(expected, config::report_outcome::attempted))
			return;
		// Reports are attempted once after execution. Failure is retained and logged, without retrying guest work.
		try
		{
			const auto directory = given.empty() ? pbd::env::path("PBD_ORDINARY_OUTPUT") : given;
			if (!directory.is_absolute())
				throw std::runtime_error("hook report requires an absolute output directory");
			hook_report report{c, directory, pbd::owner_timeline::enabled};
			report.events();
			report.base_summary();
			report.fence_summary();
			report.voice_summary();
			report.render_summary();
			report.write_summary();
			report.skip_guard();
			report.random_audit();
			c.report.store(config::report_outcome::succeeded);
		}
		catch (const std::exception& error)
		{
			c.report.store(config::report_outcome::failed);
			pbd_fn_log.error("Requested hook report failed: %s", error.what());
		}
		catch (...)
		{
			c.report.store(config::report_outcome::failed);
			pbd_fn_log.error("Requested hook report failed during publication.");
		}
	}
} // namespace pbd::fn_hook
