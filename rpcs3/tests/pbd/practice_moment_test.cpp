#include "pbd_test.h"

#include "Emu/Cell/pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_practice_menu_builder.h"

namespace pbd::test
{
	// Unstarted owner and real guest pages: exercise the same capture and guarded memory restore as a live anchor.
	struct practice_moment_fixture
	{
		rollback_session runtime;
		std::unique_ptr<ppu_thread> owner;
		u32 pc = rollback_session::pcs[0];
		bool saved_anchor_live = g_pbd_takeover_anchor_live.load();
		std::filesystem::path control_path = std::filesystem::temp_directory_path() /
			("pbd-practice-moment-" + std::to_string(session_runtime::session_clock::now().time_since_epoch().count()) + ".jsonl");
		const std::vector<u32> blocks{0x10080000, 0x10090000, 0x104c0000, 0x10740000};

		practice_moment_fixture()
		{
			Emu.SetTestMode();
			vm::init();
			for (const auto base : blocks)
			{
				check(vm::map(base, 0x10000, vm::page_size_64k));
				check(vm::falloc(base, 0x10000));
			}
			struct construction_id
			{
				u32 previous = std::exchange(id_manager::g_id, ppu_thread::id_base);
				~construction_id() { id_manager::g_id = previous; }
			} identity;
			owner = std::make_unique<ppu_thread>(ppu_thread_params{}, "practice-moment-test", 100);
			runtime.m_storage = std::make_unique<rollback_session::storage>();
			auto& data = *runtime.m_storage;
			data.session.enabled = data.session.in_match = true;
			data.session.matches = 1;
			data.practice.rewind = data.replay.takeover_fast = true;
			// Only diagnostic metadata uses this stream; the checkpoint bytes remain in memory.
			const auto path = control_path.u8string();
			check(data.replay.control_file.open(std::string(path.begin(), path.end()), fs::write_new + fs::isfile));
			data.checkpoint.checkpoint_pages = {0x1008e000, 0x1008f000, 0x10090000, 0x10091000, 0x104cc000, 0x10749000};
			data.checkpoint.memory.reset(data.checkpoint.checkpoint_pages);
			data.checkpoint.memory.prepare(data.checkpoint.checkpoint_pages);
		}

		~practice_moment_fixture()
		{
			runtime.m_finalized = true;
			runtime.m_storage.reset();
			owner.reset();
			vm::close();
			g_pbd_takeover_anchor_live.store(saved_anchor_live);
			std::error_code error;
			std::filesystem::remove(control_path, error);
		}

		void repeated_restore()
		{
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			// Non-resting actor state: an attack seven frames in and an airborne opponent in hitstun.
			vm::write32(0x1008f070, 17);
			vm::write32(0x1008f074, 44);
			vm::write32(0x104cce40, 30);
			vm::write32(0x10090044, 0x43960000); // height = 300
			vm::write32(0x10090104, 0x21001000); // current move
			vm::write16(0x100900d6, 7);          // move frame
			vm::write32(0x10090914, 0x43af0000); // opponent height = 350
			vm::write32(0x10090b10, 19);         // retained reaction bytes
			vm::write32(0x10749a00, 0x42700000); // camera context
			call.request_takeover_capture(30);
			check(call.capture_takeover(30));
			auto& saved = data.takeover.keyframe;
			const auto expected = saved.checkpoint.pages;
			const auto digest = session_runtime::hash_hex(expected.data(), expected.size());
			for (u32 pass = 1; pass <= 20; ++pass)
			{
				for (const auto page : data.checkpoint.checkpoint_pages)
					vm::write32(page, pass);
				vm::write32(0x10090044, 0);
				vm::write32(0x10090104, 0x21003000 + pass * 4);
				vm::write16(0x100900d6, static_cast<u16>(7 + pass));
				vm::write32(0x10090914, 0);
				vm::write32(0x10090b10, pass);
				vm::write32(0x10749a00, 0);
				rollback_session::boundary_call::checkpoint_restore_result result;
				check(call.restore_checkpoint_memory(saved.checkpoint, 60 + pass, [] { return true; },
					{true, true, false}, result));
				std::vector<u8> restored;
				check(session_runtime::read_pages(data.checkpoint.checkpoint_pages, restored));
				check(session_runtime::hash_hex(restored.data(), restored.size()) == digest,
					"A repeated moment restore changed the fight-state hash");
				check(saved.checkpoint.pages == expected, "Restoring mutated the remembered checkpoint");
			}
			bool restored = false;
			u32 index = 80;
			rollback_session::practice_steps steps{call, index, restored};
			for (u32 frame = 0; frame < 3; ++frame)
				data.practice.input_delay.next(0x0080, 3);
			call.practice_restart_after_restore(index);
			data.takeover.ignore_pad_until_release = true;
			check(steps.release_practice_input(0x0080, false, 0));
			check(data.practice.dummy.mask_ui && data.takeover.ignore_pad_until_release);
			check(data.practice.input_delay.next(0, 3) == 0, "An input queued before restore survived it");
			check(steps.release_practice_input(0, false, 0));
			check(!steps.release_practice_input(0x0080, false, 0), "The next fresh press could not continue the combo");
			// An unreadable or changed arena/fighter scope cannot install the old checkpoint.
			data.practice.restart.moment_scope = pbd::game_lifecycle::scope{1, 1, 1, 34, {2, 9}};
			check(!steps.remembered_moment_ready());
			check(!data.takeover.keyframe.valid && !data.practice.restart.moment_scope);
			check(!restored);
		}

		void move_picker_feedback()
		{
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			bool restored = false;
			u32 index = 1;
			rollback_session::practice_steps steps{call, index, restored};
			rollback_session::practice_steps::native_menu menu{steps, false, false, false, 0, false};
			namespace view = pbd::practice::move_list_view;
			const std::vector<view::row> rows{{0, "Basic", "", true, false}, {10, "Jab", "1", false, true}, {11, "Right Jab", "2", false, true}};
			data.practice.dummy.slots[0].moves = {11};
			data.practice.dummy.slots[1].moves = {10};
			data.practice.dummy.slots[7].moves = {10};
			auto& list = data.practice.menu.move_list;
			list.open_on(0, rows, {11});
			menu.move_list();
			const auto& shown = data.practice.view.move_list;
			check(shown.lines[0].name == "Basic" && shown.lines[0].header);
			check(shown.lines[1].name == "In slots 2, 8: Jab" && shown.lines[1].selectable && !shown.lines[1].on,
				"Other slots' moves must be marked and remain selectable");
			check(shown.lines[2].name == "Right Jab" && shown.lines[2].on, "The current slot was marked as used elsewhere");
			list.focus = 1;
			menu.move_list();
			check(shown.help.find("replacing the slot's previous move") != std::string::npos);
			check(list.toggle() && list.chosen == std::vector<u32>{10}, "An annotated move could not replace the selected move");
			data.practice.dummy.slots[7].moves.clear();
			menu.move_list();
			check(shown.lines[1].name == "In slot 2: Jab" && shown.lines[1].on);
			list.open_on(view::offense_target, rows, {});
			menu.move_list();
			check(shown.lines[1].name == "Jab" && shown.help.find("at random") != std::string::npos,
				"Offense's multiple-move picker changed");
		}

		void action_intervals()
		{
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			u32 index = 1;
			rollback_session::dummy_word_steps steps{call, index};
			const auto rng = data.practice.dummy.rng;
			constexpr std::array<u32, 3> expected{6, 60, 120};
			for (u32 interval = 0; interval < expected.size(); ++interval)
			{
				data.practice.interval = interval;
				steps.next_cycle();
				check(data.practice.dummy.wait == expected[interval]);
				check(data.practice.dummy.rng == rng, "A fixed action interval drew a random number");
			}
			data.practice.interval = 3;
			for (u32 cycle = 0; cycle < 100; ++cycle)
			{
				steps.next_cycle();
				check(data.practice.dummy.wait >= 30 && data.practice.dummy.wait <= 120,
					"Shuffle's existing interval changed");
			}
		}

		void precision_keeps_movement_toast()
		{
			struct game_identity
			{
				std::string title = Emu.GetTitleID(), hash = Emu.GetExecutableHash();
				~game_identity()
				{
					const_cast<std::string&>(Emu.GetTitleID()) = title;
					Emu.SetExecutableHash(hash);
				}
			} identity;
			const_cast<std::string&>(Emu.GetTitleID()) = pbd::t5dr::title_id;
			Emu.SetExecutableHash(pbd::t5dr::executable_hash);
			owner->cia = pbd::t5dr::boundary_pc;
			owner->gpr[2] = pbd::t5dr::toc;
			owner->state.store({});
			check(pbd::practice::native::boundary_readable(*owner));
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			bool restored = false;
			u32 index = 1;
			rollback_session::practice_steps steps{call, index, restored};
			namespace movement = pbd::practice::movement;
			data.practice.history_sides[0] = movement::screen_side::left;
			data.practice.view.movement_streaks = true;
			for (const u16 word : {movement::back, u16{0}, movement::back, u16{movement::down | movement::back}, movement::back})
			{
				data.practice.history_words[0] = word;
				steps.update_movement_streak();
			}
			check(data.practice.view.toast == "BACKDASH CANCEL - PERFECT");
			const auto until = data.practice.menu.toast_until;
			constexpr u32 actor = 0x10090040, bank = 0x10080000, allocation = 0x10081000, name = 0x10082000;
			vm::write32(actor + 0x50, bank);
			vm::write32(bank + 0x1e0, allocation);
			vm::write32(bank + 0x1e4, 1);
			vm::write16(actor + 0x42, 0x24);
			for (const u16 word : {movement::forward, u16{0}, movement::down})
			{
				data.practice.history_words[0] = word;
				steps.update_input_precision();
			}
			vm::write32(actor + 0xc4, allocation);
			vm::write16(actor + 0x96, 1);
			vm::write32(allocation, name);
			constexpr char internal_name[] = "Jz_shoryu34";
			check(vm::try_access(name, const_cast<char*>(internal_name), sizeof(internal_name), true));
			vm::write16(allocation + 0x24, 30);
			vm::write16(allocation + 0x44, 11);
			vm::write16(allocation + 0x46, 12);
			data.practice.history_words[0] = movement::down | movement::forward | pbd::practice::precision::two;
			steps.update_input_precision();
			check(data.practice.view.toast == "BACKDASH CANCEL - PERFECT", "Move timing replaced the movement streak toast");
			check(data.practice.menu.toast_until == until, "Move timing changed the movement streak duration");
		}
	};
} // namespace pbd::test

PBD_TEST(PbdPracticeMoment, TwentyReturnsPreserveMovesAirborneStateAndCamera)
{
	pbd::test::practice_moment_fixture{}.repeated_restore();
}

PBD_TEST(PbdPracticePrecision, InternalMoveTimingKeepsMovementToast)
{
	pbd::test::practice_moment_fixture{}.precision_keeps_movement_toast();
}

PBD_TEST(PbdPractice, ShortActionIntervalKeepsOtherTimings)
{
	pbd::test::practice_moment_fixture{}.action_intervals();
}

PBD_TEST(PbdPractice, MovePickerMarksOtherSlotsWithoutRestrictingThem)
{
	pbd::test::practice_moment_fixture{}.move_picker_feedback();
}
