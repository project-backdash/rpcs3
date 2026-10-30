#pragma once

#include <array>
#include <cstdint>

// Facts about the one supported release of the game. The hooks compare against them to recognise the game, its main
// thread and the instant they may act. The words below are guest state; reseeding and motion initialization also write them.
namespace pbd::t5dr
{
	// Title id in the game's PARAM.SFO; the emulator reports it through Emu.GetTitleID().
	inline constexpr char title_id[] = "NPUB30009";
	// The emulator's hash of the supported EBOOT.BIN (Emu.GetExecutableHash()); any other executable is refused.
	inline constexpr char executable_hash[] = "PPU-3b68a1ecc7531d1ae170fb00e2db03d17bffcd44";

	// The emulator gives the first PPU thread, the game's main thread, the first id of the PPU range.
	inline constexpr std::uint32_t main_ppu_id = 0x01000000;
	// The game's main executable keeps its table of contents here; the main thread runs with r2 equal to it, and the
	// game's global pointers sit at fixed offsets below it.
	inline constexpr std::uint32_t toc = 0x470540;
	// Return from the GPU-duration query inside the load-monitor sampler (0x259dc0).
	inline constexpr std::uint32_t gpu_load_probe = 0x259dfc;

	// The frame boundary: the instruction the main thread reaches once per frame, where the hooks act.
	inline constexpr std::uint32_t boundary_pc = 0x12d74;
	// The return address into the game's main loop that the link register holds at boundary_pc.
	inline constexpr std::uint32_t boundary_lr = 0x10d60;

	// The game mode, the first word of the game's state context (4 in VS, 10 in Practice).
	inline constexpr std::uint32_t game_mode_address = 0x1008ebac;
	// The scene and subscene words, which the game updates as it moves between screens (scene 3 with subscene 44 is
	// the character select).
	inline constexpr std::uint32_t scene_address = 0x1008f070;
	inline constexpr std::uint32_t subscene_address = 0x1008f074;
	// 0x15a8c reads the main-loop counter before the match initializer calls srand at 0x15a98.
	inline constexpr std::uint32_t frame_counter = 0x1008f07c;
	namespace random_tls
	{
		// The libc generator at 0x3afc3c uses r13-relative seed, initialization flag, last value and 32-word table.
		inline constexpr std::uint32_t seed_before_r13 = 0x7000;
		inline constexpr std::uint32_t flag_after_r13 = 0x9094;
		inline constexpr std::uint32_t words_after_r13 = 0x9098;
		inline constexpr std::uint32_t tail_bytes = 0x8c;
	} // namespace random_tls
	namespace wolf_motion
	{
		// 0xc6a28 calls the controller constructor; 0xcb9b4 stores its owner and 0x2979b4 reads the request byte.
		inline constexpr std::uint32_t stage_address = 0x1008f088, stage = 0x20;
		inline constexpr std::uint32_t caller = 0xc6a2c, call_site = 0xc6a28, call_opcode = 0x48004f45;
		inline constexpr std::uint32_t store_site = 0xcb9b4, store_opcode = 0x9083004c;
		inline constexpr std::uint32_t request_site = 0x2979b4, request_opcode = 0x893e0000;
		inline constexpr std::uint32_t actor_controller = 0x8ac, controller_bias = 0xb30, request = 0x1be;
	} // namespace wolf_motion

	// Guest addresses from `begin` up to `end`, `end` excluded.
	struct address_span
	{
		std::uint32_t begin = 0;
		std::uint32_t end = 0;
	};
	// The main executable's data segment, which the loader maps at boot: the game keeps its global state there, the
	// state context and the scene words above and the two fighters' actors among it.
	inline constexpr address_span data_block{0x10000000, 0x10800000};
} // namespace pbd::t5dr

namespace pbd::replay_memory
{

	inline constexpr const auto& title_id = t5dr::title_id;
	inline constexpr const auto& executable_hash = t5dr::executable_hash;
	inline constexpr std::uint32_t main_thread = t5dr::main_ppu_id;
	inline constexpr std::uint32_t toc = t5dr::toc;
	inline constexpr std::uint32_t match_initialize = 0x159f0;

	// The VS dispatcher owns the mode and history; 0x5ef10 and 0x5ef60 advance completed games and wins.
	inline constexpr std::uint32_t flow_mode = t5dr::game_mode_address;
	inline constexpr std::uint32_t flow_toc_slot = toc - 0x7560;
	inline constexpr std::uint32_t completed_games = 0x1008ebe0;
	inline constexpr std::uint32_t left_wins = 0x1008ebf4;
	inline constexpr std::uint32_t right_wins = 0x1008ebf8;
	// Words of the same object that a finished game leaves behind and the next game's initializer keeps. For a win, 0x5f034
	// (reading the outcome at +0x29) passes the winner's side (+0x2a) to 0x4f24c, which stores it at +0x4e0 and the other
	// side at +0x4e4; mode setup resets the pair to 0, 1, so a fresh boot holds 0, 0, 1. After games the right player won
	// they read -5, 1, 0, and with those three words a fresh boot's flow block hashed equal to the recording's at the next
	// game's first input. 0x4f24c also sets the side-change flag at 0x100932ac, which 0x4f3f8 copies to 0x100932a8; both
	// read 1 after a finished game and 0 after a fresh boot, and without them the next game's first frame draws from the
	// shared generator a different number of times.
	inline constexpr std::array<std::uint32_t, 5> previous_game_flow = {
		0x1008ec6c, 0x1008f08c, 0x1008f090, 0x100932a8, 0x100932ac};

	inline constexpr std::uint32_t actors = 0x10090040;
	inline constexpr std::uint32_t actor_stride = 0x8d0;
	// Both fighters' actors, the block the per-frame check hashes as its first region.
	inline constexpr std::uint32_t actor_records_bytes = 2 * actor_stride;
	inline constexpr std::uint32_t actors_toc_slot = toc - 0x7eec;
	// The actor's side, 0 or 1.
	inline constexpr std::uint32_t actor_side = 0x12;
	inline constexpr std::uint32_t actor_style = 0x3e;
	// The fighter (u16) and the row of the pick table at 0x4403c0 it was loaded from (u16). A row names fighter and
	// costume (0x59ab4 reads it), so two picks can share a COM style and still differ here.
	inline constexpr std::uint32_t actor_fighter = 0x40;
	inline constexpr std::uint32_t actor_row = 0x42;
	inline constexpr std::uint32_t actor_com_bank = 0x50;
	inline constexpr std::uint32_t actor_opponent = 0x5c;
	// A link between the two actors that each writes to the other (0x19638/0x1963c, 0xf47bc/0xf47c0).
	inline constexpr std::uint32_t actor_interaction = 0x60;
	// Pointers into a loaded COM image that the move code leaves behind and the next game starts on; 0xf4668 rewrites
	// +0xc0, +0xc4 and +0x320, and 0xc32c4 reads +0xc4 early.
	inline constexpr std::array<std::uint32_t, 6> actor_move_records = {0xc0, 0xc4, 0x320, 0x7e4, 0x818, 0x81c};

	// 0x33490 and 0x335a0 store the quantized transform terms; 0x37130 stores the float sum.
	inline constexpr std::uint32_t actor_planar = 0xe0;
	inline constexpr std::uint32_t actor_transform_sum = 0x12c;
	// 0xf09e4 copies the descriptor code; 0xf33fc and 0xf3404 store descriptor deltas.
	inline constexpr std::uint32_t actor_descriptor_code = 0x17a;
	inline constexpr std::uint32_t actor_descriptor_deltas = 0x2b8;
	// 0x3211c advances a reaction cursor in the opponent's COM bank.
	inline constexpr std::uint32_t actor_reaction_cursor = 0x2bc;
	// The cursor's run: the entry loaders store the head halfword and the run length; 0x320cc consumes the run while
	// the length is above 0, and the actor initializer 0x145c0 clears both (0x14908, 0x1490c) but not the cursor.
	inline constexpr std::uint32_t actor_reaction_head = 0x2b4;
	inline constexpr std::uint32_t actor_reaction_count = 0x2b6;
	// 0x4b340 hands this entry to the partner actor.
	inline constexpr std::uint32_t actor_reaction_entry = 0x300;
	// 0x145c0 starts the current bank and the two banks after +0x50 equal to it; 0x19740 and 0x197cc switch the
	// current bank later. 0x4b2b0 writes the partner's cursor through the partner link.
	inline constexpr std::uint32_t actor_bank_54 = 0x54;
	inline constexpr std::uint32_t actor_bank_58 = 0x58;
	inline constexpr std::uint32_t actor_partner = 0x64;
	inline constexpr std::uint32_t actor_current_bank = 0xbc;
	// 0xe8dd8, 0xe8dec, 0xf1978 and 0xf197c store float motion steps.
	inline constexpr std::uint32_t actor_motion_step = 0x2f8;
	// 0xeea80 and 0xeea9c remember the accepted reaction code for repeated-reaction handling.
	inline constexpr std::uint32_t actor_last_reaction_code = 0x308;
	// 0xeffbc evaluates COM commands; 0xf00f0 and 0xf00e8 retain command 0x11e and 0x22 conditions.
	inline constexpr std::uint32_t actor_reaction_command_11e = 0x318;
	inline constexpr std::uint32_t actor_reaction_command_22 = 0x31a;
	// 0x18cd4 retains word +8 of the returned command group, including an inline record member.
	inline constexpr std::uint32_t actor_reaction_result = 0x324;
	// Native bone transforms retain four segments before the collision spheres at +0x388.
	inline constexpr std::uint32_t actor_bone_count = 0x197;
	inline constexpr std::uint32_t actor_bone_segments = 0x328;
	inline constexpr std::uint32_t bone_segment_bytes = 0x18;

	// 0x486d8 retains contact geometry and a sphere index; 0x16e78 clears only the two slot flags.
	inline constexpr std::uint32_t actor_hit_contacts = 0x530;
	inline constexpr std::uint32_t hit_contact_bytes = 0x50;
	inline constexpr std::uint32_t hit_contact_end_offset = 0x10;
	inline constexpr std::uint32_t hit_contact_delta_offset = 0x20;
	inline constexpr std::uint32_t hit_contact_sphere_offset = 0x38;
	inline constexpr std::uint32_t hurt_sphere_count = 14;
	// 0x16e78 makes the actor the owner (word +0) of both contacts; the selected contact is one of them, the first
	// after the actor initializer (0x14a14).
	inline constexpr std::uint32_t actor_selected_contact = 0x5d0;

	// The native segment helpers retain their own end-minus-start vector and the last copied vector.
	inline constexpr std::uint32_t actor_segment_delta = 0x688;
	inline constexpr std::uint32_t actor_copied_segment_delta = 0x694;
	// Damage processing remembers the prior 16.16 life word before changing current life.
	inline constexpr std::uint32_t actor_prior_life = 0x6b4;
	// A signed reaction-record term is converted to float; blend mode 3 remembers the current weight.
	inline constexpr std::uint32_t actor_reaction_scalar = 0x2ec;
	inline constexpr std::uint32_t actor_previous_blend_weight = 0x804;
	// Six clip slots of 0x14 bytes. Words +0 and +4 of slot i hold pair clip_slot_pairs[i] of the loaded COM image's
	// pair list (com_clip_pairs), copied by the loader (0xd5340..0xd53d0) and by 0x13c34; 0x365ac reads slot 4 every
	// frame. The rest of a slot is its time and flags.
	inline constexpr std::uint32_t actor_clip_slots = 0x828;
	inline constexpr std::uint32_t clip_slot_bytes = 0x14;
	inline constexpr std::array<std::uint32_t, 6> clip_slot_pairs = {2, 1, 1, 3, 4, 5};
	// The actor's word of the side-flag table (side_flags + 4 * side); 0xad4d8 rewrites it at the match initializer.
	inline constexpr std::uint32_t actor_side_flag = 0x8a0;
	inline constexpr std::uint32_t side_flags = 0x1041c170;
	// Words a mode extension sets (0xc6884); a VS game start holds 0 in both.
	inline constexpr std::array<std::uint32_t, 2> actor_mode_extension = {0x8a8, 0x8ac};
	// The bone setup 0x27fab4 stores the fighter's bone-root object here; only the shadow draw 0x159754 and the bone-root
	// cull 0x280b10 read it. The object sits in a pool whose order follows each boot's loading history: at game 1's match
	// initializer the same object, its link back to the actor included, was at 0x106314f0 in a live set and at 0x10631370
	// in a fresh boot, where 0x106314f0 held zeros. The per-frame check leaves this word out for the same reason.
	inline constexpr std::uint32_t actor_bone_root = 0x8c0;
	// The bone-root object's link back to the actor it belongs to.
	inline constexpr std::uint32_t bone_root_actor = 0x10;
	// The stock load descriptor installs the fighter's model object here before the match initializer, whose kind-2 path
	// dereferences it (0x28e0c8). A live set and its fresh-boot replays held the same two models, 0x41b5d590 and
	// 0x41e86b10, at every game start.
	inline constexpr std::uint32_t actor_model = 0x8a4;

	// The COM loader publishes section starts, counts and the next section's start.
	inline constexpr std::uint32_t com_version = 0x100;
	inline constexpr std::uint32_t com_magic = 0x434f4d00;
	inline constexpr std::uint32_t com_payload_offset = 0x274;
	// The directory before the payload; from +0x188 it holds (start, count) pairs, published at 0xd4d78..0xd4d94.
	inline constexpr std::uint32_t com_directory = 0x180;
	inline constexpr std::uint32_t com_records_catalog = 0x188;
	inline constexpr std::uint32_t com_groups_catalog = 0x198;
	inline constexpr std::uint32_t com_samples_catalog = 0x1a8;
	inline constexpr std::uint32_t com_record_bytes = 0x50;
	inline constexpr std::uint32_t com_group_bytes = 12;
	inline constexpr std::uint32_t com_group_result_offset = 8;
	inline constexpr std::uint32_t com_record_result_offset = 16;
	inline constexpr std::uint32_t com_sample_bytes = 2;
	// How far the game reads through a pointer into the payload: a whole record.
	inline constexpr std::uint32_t com_record_span = com_record_bytes;
	// Six (A, B) word pairs naming the image's clips: A in the image, B in the motion archive (0xd4ef0..0xd4f00 store
	// the B words at every load).
	inline constexpr std::uint32_t com_clip_pairs = 0x240;
	inline constexpr std::uint32_t com_clip_pair_count = 6;
	inline constexpr std::uint32_t max_com_bank = 0xfffffe4f;

	// 0x154b0 derives the inherited flag from byte 0x20 of the active descriptor.
	inline constexpr std::uint32_t descriptor_object = 0x10423614;
	inline constexpr std::uint32_t descriptor_pointer = 0x10423618;
	inline constexpr std::uint32_t descriptor_toc_slot = toc - 0x7edc;
	inline constexpr std::uint32_t descriptor_flag = 0x1008f0e0;
	inline constexpr std::uint32_t descriptor_flag_toc_slot = toc - 0x7ea8;
	inline constexpr std::uint32_t descriptor_flag_offset = 0x20;

	struct body_signature
	{
		std::uint32_t address;
		std::uint32_t bytes;
		const char* sha256;
	};

	inline constexpr body_signature history_update{
		0x5eed4, 0x160, "98c8eea8f6d4d080c2bb54e558c73170568cea1deb659c355b4e7e90960016d2"};
	inline constexpr body_signature history_initialize{
		0x4f14c, 0x4c, "4217560b685c7826db94f1131000a2c6d8a143453bde041c872b063f33ee66ca"};
	inline constexpr body_signature planar_x{
		0x33490, 0x1c, "2b4d668826ef296c9e43bfb947a7c6c16965af8fb9d6dd2d2fdd153eba0a6a7a"};
	inline constexpr body_signature planar_z{
		0x335a0, 0x20, "d13ee288a6d4a74ab1076c0aedee4a062bc5742c1b390c8077801dca1016ae9c"};
	inline constexpr body_signature transform_products{
		0x36f34, 0x60, "c7d3edb22d2f806a72efd8e161c6d037c07211e5a6ef09cd39fc07bdde616bee"};
	inline constexpr body_signature transform_sum{
		0x37108, 0x3c, "540c94b33c7b1f798a5a24e191243af67c2566691ebfe1a553ee71b1c588bcd0"};
	inline constexpr body_signature descriptor_code{
		0xf09dc, 0x10, "26ab69023ca4261e615fbfe2f0d2fa203bd28e51741f3a84bff5f4c53564895e"};
	inline constexpr body_signature descriptor_delta_compute{
		0xf3378, 0x2c, "59fd85c3ebb34cdc29bd434798d7b5313d52daae10b479b79f10d66addb02a77"};
	inline constexpr body_signature descriptor_delta_store{
		0xf33fc, 0xc, "c548243c037314e1fcdfec0b8767f05cb56c5d4a7a07473d94afaefa8ca2e07f"};
	inline constexpr body_signature com_records_load{
		0xd5854, 0x38, "dce20e38b3a5832c99673ed0922ddfb9d9529879f6b775dba274c4145c270d71"};
	inline constexpr body_signature com_samples_load{
		0xd5954, 0x54, "347461e095933ebaf2b5e78f5fbc4cfd957386ba375545a2dbcb187371863d26"};
	inline constexpr body_signature opponent_read{
		0x1817c, 0xc, "2d85e30ebf9991731814c76de9e7cd79fc256c9780042f94ee2013ba322b8dcf"};
	inline constexpr body_signature reaction_advance{
		0x32104, 0x1c, "14feb57f859e68f1f723487ae11ddcc1ed23e4b093d0a2a8dd4b023fff925151"};
	inline constexpr body_signature target_motion{
		0xe8dac, 0x48, "3cddbb57b12be8ef64173d369cc6b4d0ac6b0e8afe811fe8ddeedc416dbf2573"};
	inline constexpr body_signature reaction_motion{
		0xf1914, 0x6c, "49ddcfacd5f35312542d4cb9a54c39b231b9ac99cc7efb27afff66347b4a32d2"};
	inline constexpr body_signature last_reaction_update{
		0xeea3c, 0x78, "0d2c8a6e31e6200c7cf848b03633cd462053734dd442f1fd6e0291f404097a32"};
	inline constexpr body_signature reaction_conditions_update{
		0xeffbc, 0x1a0, "ecbc1150c5ff06500d879b42658ac38d340511b37ad16e65e7a7279c12454eb2"};
	inline constexpr body_signature bone_segment_start{
		0x37e6c, 0x1ac, "087e5067f4f6e747b5ce7202b145e9a1984b397479b686c3d5c3979155d967ad"};
	inline constexpr body_signature bone_segment_end{
		0x38f7c, 0x3c, "b5fa7650624ae4d0552c45e3ba1c11d94751bb44a291eb051631808a2caf02c9"};
	inline constexpr body_signature reaction_groups_load{
		0xd571c, 0x68, "855695bf38ff83ace483ed8462bda4990a8f4fcf1749657d74aa6203343669d7"};
	inline constexpr body_signature reaction_record_members_load{
		0xd5854, 0x100, "3223c71282d696ee59f142bac94a703ae5fc908846838b87c85c2fc886065994"};
	inline constexpr body_signature reaction_result_store{
		0x18cbc, 0x20, "996d1a3eb8422eb92ddf4d4c5e8c5174d1d58d5dbd8393d1891b2e1763d21368"};
	inline constexpr body_signature com_payload_relocation{
		0xd4c34, 0x158, "031736018c7fd86502a9871fcc86ccfa09c5f3f874a67bd049c97118405ac6e4"};
	inline constexpr body_signature hit_contact_output{
		0x486d8, 0x54, "708bf724590e25e88079744070ff3c12aeab068b7983c4fb84a7d40acfcf059b"};
	inline constexpr body_signature hit_contacts_initialize{
		0x16e78, 0x64, "8283112eba20ce098b834db062c4028892468901dd508a51869ae0fb0b9dc794"};
	inline constexpr body_signature hit_contact_slots{
		0x17c94, 0x34, "88b6fc421a1637a1538ecd116859248e72a33070b9c15d8d6c51db0c4fc399d6"};
	inline constexpr body_signature segment_delta_store{
		0x31670, 0x34, "9976c7ce506e583f6e3e7f77ef1796a9dbe0bff65f6ac75d31c11cfcd5b1326a"};
	inline constexpr body_signature segment_delta_copy{
		0x316a4, 0x1c, "ac8ddaf65e3bf99eeb3a98d78470f203e4be721e0a8d8677e2c2a9ff62d0b9cd"};
	inline constexpr body_signature prior_life_store{
		0x12720, 0x10, "ae1c9dbc5cb01a8d13291f6df41e5a629007709203929435ee1a1e4679756aea"};
	inline constexpr body_signature reaction_scalar_store{
		0x4b2bc, 0x40, "baee33e6d19b303ec07057cbca443b00908c4d2c6984f7823b72a5ecd3d0dbd8"};
	inline constexpr body_signature previous_blend_store{
		0x28dbd4, 0xc, "d8b034f1d7ae99ee87e83c4d5fd45b3fde06b468fec42d62bccd043a22982b7a"};
	inline constexpr body_signature previous_blend_trigger{
		0x28ce3c, 0xc, "0008a8f69eb6167fe6ba1c7ff1bf17957871239ce6fe1d83ec571f3266f6302b"};
	inline constexpr body_signature previous_blend_read{
		0x28c6d8, 0x64, "bb2ee86ab8fee55bb4b8d151d4bcd1cfc47567f377fb5c1bdd69f287a6ef4295"};
	inline constexpr body_signature descriptor_flag_derive{
		0x154b0, 0x14, "e057f173100b9e774ea1dd4cf5b86839b9516ddf38c4105eb8d21fc76ebdb790"};

} // namespace pbd::replay_memory
