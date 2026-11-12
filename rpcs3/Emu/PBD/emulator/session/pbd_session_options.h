#pragma once

struct pbd::rollback_session::session_configuration
{
	storage& s;

	u32 number(const char* name, u32 fallback, unsigned base = 10)
	{
		const auto text = pbd::env::find_wide(name);
		if (!text || !*text)
			return fallback;
		u32 value = 0;
		require(pbd::env::parse_u32(text, value, base), name);
		return value;
	}

	void run()
	{
		if (const auto session = pbd::env::find_wide("PBD_SESSION"); s.launch.direct() || (session && std::wstring_view(session) == L"1"))
		{
			require(s.round_lifecycle, "session_requires_round_lifecycle");
			s.session.enabled = true;
			if (s.launch.direct())
			{
				s.session.start_skip = s.session.failstop = true;
				s.session.gate.enabled = s.launch.exit_gate;
				s.session.gate.lag = pbd::session_policy::gate_lag;
			}
			if (const auto v = pbd::env::find_wide("PBD_SESSION_MASK_START"); v && std::wstring_view(v) == L"1")
				s.session.mask_start = true;
			if (const auto v = pbd::env::find_wide("PBD_SESSION_START_SKIP"); v && std::wstring_view(v) == L"1")
			{
				s.session.start_skip = true;
				s.session.mask_start = false;
			}
			if (const auto v = pbd::env::find_wide("PBD_SESSION_FAILSTOP"); v && std::wstring_view(v) == L"1")
				s.session.failstop = true;
			if (const auto mask = pbd::env::find_wide("PBD_SESSION_MENU_HASH"); mask && *mask)
			{
				s.session.menu_hash_mask = number("PBD_SESSION_MENU_HASH", 7, 0);
				require(s.session.menu_hash_mask <= 7, "session_menu_hash_invalid");
			}
			if (const auto seed = pbd::env::find_wide("PBD_MATCH_SEED"); seed && *seed)
			{
				s.session.seed = number("PBD_MATCH_SEED", 0, 0);
				s.session.seed_set = true;
			}
			if (const auto settle = pbd::env::find_wide("PBD_SESSION_SETTLE_MS"); settle && *settle)
				s.session.gate.settle_ms = number("PBD_SESSION_SETTLE_MS", 0);
			if (const auto dumps = pbd::env::find_wide("PBD_SESSION_DUMP_MATCH"); dumps && std::wstring_view(dumps) == L"1")
				s.session.dump_matches = true;
			configure_match_snapshots();
			if (const auto scene = pbd::env::find_wide("PBD_SESSION_SETTLE_SCENE"); scene && *scene)
				s.session.gate.settle_scene = number("PBD_SESSION_SETTLE_SCENE", 0);
			if (const auto gate = pbd::env::find_wide("PBD_SESSION_EXIT_GATE"); gate && std::wstring_view(gate) == L"1")
				s.session.gate.enabled = true;
			if (const auto opts = pbd::env::find_wide("PBD_SESSION_OPTIONS"); opts && std::wstring_view(opts) == L"0")
				s.session.digests.enabled = false;
			configure_replay();
			configure_controls();
			const auto lag = pbd::session_policy::recipe_number(s.launch.direct(), pbd::env::find_wide("PBD_SESSION_EXIT_GATE_LAG"), s.session.gate.lag, 1, 60);
			require(lag.has_value(), "session_exit_gate_lag_invalid");
			s.session.gate.lag = *lag;
		}
	}

	void configure_match_snapshots()
	{
		if (const auto snap = pbd::env::find_wide("PBD_SESSION_MATCH_SNAPSHOT"); snap && *snap)
		{
			const std::wstring_view text(snap);
			if (text == L"1")
				s.session.snapshot_ranges.push_back({pbd::t5dr::data_block.begin, pbd::t5dr::data_block.end});
			else
			{
				usz pos = 0;
				while (pos < text.size())
				{
					const usz comma = std::min(text.find(L',', pos), text.size());
					const std::wstring item(text.substr(pos, comma - pos));
					const usz dash = item.find(L'-');
					require(dash != std::wstring::npos, "bad_match_snapshot_range");
					u64 begin = 0, end = 0;
					require(pbd::env::parse_unsigned(std::wstring_view(item).substr(0, dash), begin, 0x100000000ull, 16) &&
								pbd::env::parse_unsigned(std::wstring_view(item).substr(dash + 1), end, 0x100000000ull, 16),
						"bad_match_snapshot_range");
					begin &= ~0xfffull;
					require(end > begin && end <= 0x100000000ull, "bad_match_snapshot_range");
					s.session.snapshot_ranges.push_back({begin, end});
					pos = comma + 1;
				}
			}
		}
	}

	void configure_fast_target()
	{
		if (const auto to = pbd::env::find_wide("PBD_REPLAY_FAST_TO"); to && *to)
		{
			std::array<u32, 2> fields{};
			require(pbd::env::parse_decimal_fields(to, fields) && fields[0] >= 1, "replay_fast_to_invalid");
			s.replay.fast.to_match = fields[0];
			s.replay.fast.to_frame = fields[1];
		}
		else if (const auto at = pbd::env::find_wide("PBD_REPLAY_TAKE_AT"); at && *at)
		{
			// Trying a moment again runs to it fast, like watching from it.
			std::array<u32, 3> fields{};
			require(pbd::env::parse_decimal_fields(at, fields) && fields[0] >= 1 && (fields[2] == 1 || fields[2] == 2), "replay_take_at_invalid");
			s.replay.fast.to_match = fields[0];
			s.replay.fast.to_frame = fields[1];
		}
	}

	void configure_fast_playback()
	{
		if (s.replay.fast.to_match)
		{
			if (const auto rate = pbd::env::find_wide("PBD_REPLAY_FAST_RATE"); rate && *rate)
				s.replay.fast.rate = std::clamp<u32>(number("PBD_REPLAY_FAST_RATE", 60), 60u, 600u);
			s.replay.fast.saved_vblank = g_cfg.video.vblank_rate.get();
			g_cfg.video.vblank_rate.set(s.replay.fast.rate);
			pbd::player_volume::mute();
			s.replay.fast.active = true;
			pbd_session_log.warning("Replay: fast forward at %u Hz with the sound off to frame %u of match %u.", s.replay.fast.rate, s.replay.fast.to_frame, s.replay.fast.to_match);
		}
		else if (const auto until = pbd::env::find_wide("PBD_REPLAY_FAST_UNTIL_MATCH"); until && *until)
		{
			const u32 n = number("PBD_REPLAY_FAST_UNTIL_MATCH", 0);
			if (const auto rate = pbd::env::find_wide("PBD_REPLAY_FAST_RATE"); rate && *rate)
				s.replay.fast.rate = std::clamp<u32>(number("PBD_REPLAY_FAST_RATE", 60), 60u, 600u);
			if (n >= 2)
			{
				s.replay.fast.until_match = n;
				s.replay.fast.saved_vblank = g_cfg.video.vblank_rate.get();
				g_cfg.video.vblank_rate.set(s.replay.fast.rate);
				pbd::player_volume::mute();
				s.replay.fast.active = true;
				pbd_session_log.warning("Replay: fast forward at %u Hz with the sound off until match %u starts.", s.replay.fast.rate, n);
			}
		}
	}

	void read_replay(const wchar_t* rep)
	{
		const std::wstring wide(rep);
		const auto u8 = std::filesystem::path(wide).u8string();
		fs::file in(std::string(u8.begin(), u8.end()));
		require(!!in, "session_replay_unreadable");
		const auto bytes = in.size();
		require(bytes <= pbd::input_script::maximum_legacy_bytes, "session_replay_too_large");
		std::string text;
		text.resize(bytes);
		require(in.read(text.data(), text.size()) == text.size(), "session_replay_short_read");
		auto parsed = pbd::input_script::parse_legacy(text);
		if (parsed.error)
			pbd_session_log.error("Session replay line %u: %s", parsed.line, parsed.error);
		require(!parsed.error, parsed.error ? parsed.error : "session_replay_invalid");
		s.replay.inputs = std::move(parsed.inputs);
		s.replay.sync = std::move(parsed.sync);
		s.replay.session_replay = true;
	}

	void configure_viewer()
	{
		if (const auto viewer = pbd::env::find_wide("PBD_REPLAY_VIEWER"); viewer && std::wstring_view(viewer) == L"1")
		{
			s.replay.viewer_enabled = true;
			if (const auto events = pbd::env::find_wide("PBD_REPLAY_VIEWER_EVENTS"); events && *events)
			{
				const std::filesystem::path path(events);
				require(path.is_absolute(), "replay_viewer_events_not_absolute");
				const auto utf8 = path.u8string();
				require(s.replay.viewer_event_file.open(std::string(utf8.begin(), utf8.end()), fs::write + fs::append + fs::isfile), "replay_viewer_events_unavailable");
				require(s.replay.viewer_event_file.size() <= 65536, "replay_viewer_events_too_large");
			}
			if (const auto rounds = pbd::env::find_wide("PBD_REPLAY_ROUNDS"); rounds && *rounds)
			{
				const std::wstring_view wide(rounds);
				require(wide.size() <= 16384, "replay_viewer_rounds_too_large");
				std::string manifest;
				for (const wchar_t c : wide)
				{
					require(c < 128, "replay_viewer_rounds_non_ascii");
					manifest.push_back(static_cast<char>(c));
				}
				require(pbd::replay_viewer::parse_rounds(manifest, s.replay.viewer_rounds), "replay_viewer_rounds_invalid");
			}
		}
	}

	void configure_takeover()
	{
		if (const auto takeover = pbd::env::find_wide("PBD_REPLAY_TAKEOVER"); takeover && *takeover)
		{
			const std::wstring_view mode(takeover);
			require(mode == L"1" || mode == L"relaunch" || mode == L"fast", "replay_takeover_invalid");
			s.replay.takeover = true;
			s.replay.takeover_fast = mode == L"fast";
			if (const auto fast = pbd::env::find_wide("PBD_REPLAY_TAKEOVER_FAST"); fast && std::wstring_view(fast) == L"1")
				s.replay.takeover_fast = true;
			// The first boundary after an in-memory rewind is a rollback replay bridge, not a second live
			// presentation of the anchor frame. Exact withhold is therefore part of fast-mode correctness,
			// even when no optional draw-function skip set is configured.
			if (s.replay.takeover_fast)
			{
				s.correction.replay_skip_draws = true;
				s.correction.withhold_exact = true;
			}
			// Rewind and round jumps restore the same in-memory anchors as Take Control (pbd_replay_rewind.h).
			// PBD_REPLAY_REWIND=0 turns them off; PBD_REPLAY_REWIND_MIB sets their memory budget.
			const auto rewind = pbd::env::find_wide("PBD_REPLAY_REWIND");
			if (s.replay.viewer_enabled && s.replay.takeover_fast && !(rewind && std::wstring_view(rewind) == L"0"))
			{
				s.rewind.enabled = true;
				s.rewind.budget = pbd::replay_rewind::budget_from_mib(number("PBD_REPLAY_REWIND_MIB", 0));
			}
		}
	}

	void configure_control_actions()
	{
		if (const auto test = pbd::env::find_wide("PBD_REPLAY_CONTROL_TEST"); test && *test)
		{
			const auto u8 = std::filesystem::path(std::wstring(test)).u8string();
			std::string text(u8.begin(), u8.end());
			usz pos = 0;
			while (pos < text.size())
			{
				const usz end = std::min(text.find(',', pos), text.size());
				const std::string item = text.substr(pos, end - pos);
				pos = end + 1;
				replay_test_action a;
				char kind[8]{};
				int value = 0;
				unsigned word = 0;
				const int got = std::sscanf(item.c_str(), "%u:%7[a-z]%d=%u", &a.boundary, kind, &value, &word);
				require(got >= 2, "replay_control_test_invalid");
				const std::string_view k(kind);
				a.kind = k == "pause" ? 'p' : k == "step" ? 's' :
				                          k == "speed"    ? 'v' :
				                          k == "take"     ? 't' :
				                          k == "pad"      ? 'w' :
				                          k == "tk"       ? 'k' :
				                          k == "null"     ? 'u' :
				                          k == "l3"       ? 'l' :
				                          k == "r3"       ? 'h' :
				                          k == "again"    ? 'g' :
				                          k == "rewind"   ? 'g' :
				                          k == "return"   ? 'r' :
				                          k == "end"      ? 'e' :
				                          k == "prev"     ? 'b' :
				                          k == "next"     ? 'n' :
				                          k == "seek"     ? 'x' :
				                          k == "seekto"   ? 'y' :
				                          k == "round"    ? 'o' :
				                          k == "control"  ? 'c' :
				                                            0;
				require(got >= 3 || (a.kind != 'p' && a.kind != 's' && a.kind != 'v' && a.kind != 't' && a.kind != 'k' && a.kind != 'u' && a.kind != 'w' &&
										a.kind != 'x' && a.kind != 'y' && a.kind != 'o' && a.kind != 'c'),
					"replay_control_test_invalid");
				require(a.kind, "replay_control_test_invalid");
				require((a.kind != 't' && a.kind != 'k' && a.kind != 'u') || (value >= 0 && value <= 2 && word <= 0xffff), "replay_control_test_invalid");
				require(a.kind != 'u' || value == 1 || value == 2, "replay_control_test_invalid");
				require(a.kind != 'w' || (value >= 0 && value <= 0xffff), "replay_control_test_invalid");
				a.value = value;
				a.word = static_cast<u16>(word);
				s.replay.test.actions.push_back(a);
			}
		}
	}

	void configure_replay()
	{
		if (const auto rep = pbd::env::find_wide("PBD_SESSION_REPLAY"); rep && *rep)
		{
			read_replay(rep);
			configure_fast_target();
			configure_fast_playback();
		}
	}

	void configure_controls()
	{
		if (s.replay.session_replay || pbd_direct_match::requested())
		{
			if (const auto controls = pbd::env::find_wide("PBD_REPLAY_CONTROLS"); controls && std::wstring_view(controls) == L"1")
			{
				auto control_dir = s.checkpoint.capture_directory;
				if (control_dir.empty())
				{
					const auto trace = pbd::env::find_wide("PBD_TICK_TRACE");
					require(trace && *trace, "replay_controls_need_probe_dir");
					control_dir = std::filesystem::path(trace).parent_path();
					require(control_dir.is_absolute(), "replay_control_directory_not_absolute");
				}
				s.replay.controls = true;
				configure_viewer();
				if (!s.replay.fast.active)
				{
					s.replay.fast.saved_vblank = g_cfg.video.vblank_rate.get();
				}
				if (const auto at = pbd::env::find_wide("PBD_REPLAY_TAKE_AT"); at && *at)
				{
					std::array<u32, 3> fields{};
					require(pbd::env::parse_decimal_fields(at, fields) && fields[0] >= 1 && (fields[2] == 1 || fields[2] == 2), "replay_take_at_invalid");
					s.replay.take.match = fields[0];
					s.replay.take.frame = fields[1];
					s.replay.take.port = fields[2];
					if (const auto ms = pbd::env::find_wide("PBD_REPLAY_TAKE_RESUME_MS"); ms && *ms)
						s.replay.take.resume_ms = number("PBD_REPLAY_TAKE_RESUME_MS", 0);
				}
				configure_takeover();
				if (const auto audit = pbd::env::find_wide("PBD_TAKEOVER_AUDIT"); audit && std::wstring_view(audit) == L"1")
					s.takeover.audit = true;
				if (const auto secs = pbd::env::find_wide("PBD_REPLAY_TAKE_SECONDS"); secs && *secs)
				{
					const auto value = number("PBD_REPLAY_TAKE_SECONDS", 0);
					require(value >= 1 && value <= 60, "replay_take_seconds_invalid");
					s.takeover.frames = static_cast<u32>(value * 60);
					s.takeover.timed_take = s.replay.takeover;
				}
				if (const auto end = pbd::env::find_wide("PBD_REPLAY_END_INDEX"); end && *end)
				{
					const auto value = number("PBD_REPLAY_END_INDEX", 0);
					require(value > s.start_index && value <= s.extent(), "replay_end_index_invalid");
					s.replay.end_index = static_cast<u32>(value);
				}
				configure_control_actions();
				const auto control_path = (control_dir / "replay-control.jsonl").u8string();
				s.replay.bookmark_reply = control_dir / "replay-bookmark.json";
				require(s.replay.control_file.open(std::string(control_path.begin(), control_path.end()), fs::write_new + fs::isfile), "cannot_create_replay_control_exclusively");
				pbd_session_log.warning("Replay: controls on (end at boundary %u, %u scripted action(s)).", s.replay.end_index, static_cast<u32>(s.replay.test.actions.size()));
			}
		}
	}
};
