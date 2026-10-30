#include "stdafx.h"
#include "pbd_live_replay.h"
#include "pbd_fn_hook.h"
#include "../PBD/pbd_character_rule_native.h"
#include "../PBD/pbd_env.h"
#include "../PBD/pbd_t5dr_memory.h"
#include "pbd_practice_native.h"
#include "Emu/PBD/pbd_anchor_dump.h"
#include "Emu/PBD/pbd_replay_carry.h"
#include "Emu/PBD/pbd_replay_carry_codec.h"
#include "Emu/PBD/pbd_rank_screens.h"
#include "Emu/PBD/pbd_replay_notices.h"
#include "pbd_set_channel.h"
#include "pbd_lobby_set_driver.h"
#include "PPUThread.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"
#include "Utilities/File.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <vector>
#include "Emu/PBD/pbd_stall_metrics.h"
#include "Emu/PBD/pbd_replay_journal.h"

LOG_CHANNEL(pbd_record_log, "PBD_REPLAY_RECORD");

namespace
{
	using json = pbd::set_channel::json_value;
	std::atomic<pbd_live_replay*> active{nullptr};
	constexpr const char* capsule_schema = "pbd-game-start-capsule-draft-2";
	constexpr const char* elf_sha = "b56e25e9210ed3c4fc19f3dfae686b7246aef386cdd4f88c80e0b89141ff887d";
	bool word(u32 address, u32& value) noexcept
	{
		be_t<u32> bytes{};
		if (!vm::try_access(address, &bytes, sizeof(bytes), false))
			return false;
		value = bytes;
		return true;
	}
	json bytes(u32 address, usz count, bool& read)
	{
		std::vector<u8> buffer(count);
		if (!vm::try_access(address, buffer.data(), count, false))
		{
			read = false;
			return {};
		}
		return json::string(pbd::json::lower_hex(buffer));
	}
	json number(u32 address, bool& read)
	{
		u32 value = 0;
		if (!word(address, value))
		{
			read = false;
			return {};
		}
		return json::number(value);
	}
	json shared_libc(ppu_thread& owner, bool& read)
	{
		const u32 tls = static_cast<u32>(owner.gpr[13]);
		auto seed = bytes(tls - 0x7000, 4, read), tail = bytes(tls + 0x9094, 136, read);
		if (seed.kind != pbd::set_channel::json_kind::string || tail.kind != pbd::set_channel::json_kind::string)
			return {};
		return json::string(seed.text + tail.text);
	}
	json generators(ppu_thread& owner, bool& read)
	{
		u32 index = 0, table = 0;
		if (!word(pbd::t5dr::toc - 0x7028, index) || !word(pbd::t5dr::toc - 0x702c, table) ||
			index != 0x10053578 || table != 0x10095168)
			read = false;
		// A stable owner sample does not assert that another guest producer was parked.
		std::vector<be_t<u32>> mt(624), again(624);
		u32 first = 0, last = 0;
		const bool stable = index && table && word(index, first) &&
		                    vm::try_access(table, mt.data(), 624 * 4, false) &&
		                    vm::try_access(table, again.data(), 624 * 4, false) && word(index, last) && mt == again && first == last;
		if (!stable || first > 624)
			read = false;
		std::vector<json> words;
		for (const auto value : mt)
			words.push_back(json::number(u32{value}));
		return json::object({{"sharedLibc140Hex", shared_libc(owner, read)},
			{"flowLcg", json::array({number(0x1008f0c4, read), number(0x1008f0d0, read)})},
			{"mt", json::object({{"index", json::number(first)}, {"words", json::array(std::move(words))},
					   {"stableSample", json::boolean(stable)}, {"globallyCoherent", json::boolean(false)}})},
			{"network", json::object({{"matchSeedLcg", number(0x1061d47c, read)},
							{"state", number(0x1061d480, read)}, {"count", number(0x1061d484, read)}})},
			{"mokujin", json::object({{"stylesHex", bytes(0x10093470, 18, read)},
							{"eligibilityHex", bytes(0x100934a0, 8, read)}})},
			{"inventoryExhaustive", json::boolean(false)}});
	}
} // namespace

struct pbd_live_replay::state
{
	u64 generation = 0, written = 0;
	u32 seed = 0, game = 0, anchor = 0, next = 0;
	std::filesystem::path directory;
	std::array<player, 2> players;
	pbd::replay::journal journal;
	std::atomic<bool> failed{false};
	std::atomic<bool> closed{false};
	bool battle_seen = false, checkpoint_source = false;
	std::mutex mutex;
	json style_producer;
	std::shared_ptr<const std::string> pbd_start_record;
	u32 style_producer_boundary = 0;
	std::array<json, 32> round_cleanup;
	u32 round_written = 0;
	json end;
	u32 end_boundary = ~0u;
	bool append(json row, bool flush = false)
	{
		pbd::stall_metrics::scope measure_write("replay_journal_write");
		auto text = pbd::set_channel::detail::serialize(row) + '\n';
		const auto size = text.size();
		if (failed || !journal || size > 65536 || written > 64ull * 1024 * 1024 - size ||
			!journal.append(std::move(text), flush))
		{
			failed = true;
			return false;
		}
		written += size;
		return true;
	}
	bool open_game_journal()
	{
		anchor = pbd::fn_hook::current_tick.load();
		next = anchor + 1;
		battle_seen = false;
		end_boundary = ~0u;
		end = {};
		round_cleanup = {};
		round_written = 0;
		const auto file = directory / fmt::format("game-%u.partial.jsonl", ++game);
		if (!journal.open(file))
			return false;
		written = 0;

		return true;
	}
	bool start(ppu_thread& owner, u32 kind)
	{
		if (journal)
			return false; // A new initializer cannot silently splice an unfinished game.
		bool read = true;
		u32 left = 0, right = 0, stage = 0, counter = 0, time = 0, rounds = 0, mode = 0;
		u32 input_counter_pointer = 0, input_counter = 0;
		std::array<be_t<u16>, 2> handicaps{};
		if (!word(0x10093f50, left) || !word(0x10093f54, right) || !word(0x1008f088, stage) ||
			!word(0x1008f07c, counter) || !word(0x1008f09c, time) || !word(0x1008f0a0, rounds) ||
			!word(pbd::t5dr::game_mode_address, mode) || mode != 4 || left >= 85 || right >= 85 || stage >= 40 ||
			time > 4 || rounds < 1 || rounds > 5)
			return false;
		// These applied VS percentages survive selection and determine native maximum vitality at12544.
		// Capture the live settings, never infer them from observed health or a preceding game.
		if (!vm::try_access(0x1008ebf6, &handicaps[0], 2, false) ||
			!vm::try_access(0x1008ebfa, &handicaps[1], 2, false) ||
			!handicaps[0] || handicaps[0] > 200u || !handicaps[1] || handicaps[1] > 200u)
			return false;
		// The native key-read clock is distinct from the host reseed counter. Scene 8 can consume its inherited value
		// before 0x120764 resets it, so capture it at game start rather than infer it later.
		if (!word(pbd::t5dr::toc - 0x4ad0, input_counter_pointer) || input_counter_pointer != 0x104cce40 ||
			!word(input_counter_pointer, input_counter))
			return false;
		std::vector<json> selected;
		for (u32 side = 0; side < 2; ++side)
		{
			const u32 row = side ? right : left;
			std::array<be_t<u16>, 2> definition{};
			u8 rank = 0;
			if (!vm::try_access(0x4403c0 + row * 0x24, definition.data(), sizeof(definition), false))
				return false;
			if (!vm::try_access(pbd::game_profile::rank_address(side), &rank, sizeof(rank), false))
				return false;
			selected.push_back(json::object({{"id", json::string(players[side].id)}, {"name", json::string(players[side].name)},
				{"fighter", json::number(u16{definition[0]})}, {"costume", json::number(u16{definition[1]})}, {"row", json::number(row)},
				{"rank", json::number(pbd::game_profile::rank_byte(rank))}}));
		}
		const auto epoch = pbd::fn_hook::takeover_counters();
		auto rng = generators(owner, read);
		const auto saved_carry = pbd::replay_carry::capture_game_values(owner, generation);
		if (!saved_carry)
			return false;
		auto carry = pbd::replay_carry::encode_game_values(*saved_carry);
		const std::vector<std::pair<std::string, json>> rules_carry{
			{"nativeOrdinal", number(0x1008f0e4, read)},
			{"appliedHandicapPercent", json::array({json::number(u16{handicaps[0]}), json::number(u16{handicaps[1]})})},
			{"nativeInputContext", json::object({{"schema", json::string("dr-native-keyread-clock-draft-1")},
									   {"counter", json::number(input_counter)}})},
			{"nativeReplayFlag", number(0x10425ce0, read)}, {"styleProducer", style_producer},
			{"styleProducerBoundary", json::number(style_producer_boundary)},
			{"roundOptionBytesHex", bytes(0x1008ebb0, 13, read)},
			{"roundsOptionHex", bytes(0x1008ebd4, 1, read)},
			{"gameRuleWordsHex", bytes(0x1008f098, 44, read)}};
		carry.fields.insert(carry.fields.end(), rules_carry.begin(), rules_carry.end());
		if (!read || style_producer.kind == pbd::set_channel::json_kind::null)
			return false;
		if (!open_game_journal())
			return false;
		auto record = json::object({{"type", json::string("game_start")}, {"draftRevision", json::number(2)},
			{"source", json::string(checkpoint_source ? "fixture-live-runtime" : "live-game")},
			{"recordedAt", json::string(pbd::set_channel::utc_now())},
			{"syncEncoding", json::string("pbd-sync-facts-v1")}, {"syncDefinition", json::string("pbd-peer-sync-v3")},
			{"game", json::number(game)}, {"generation", json::number(generation)}, {"boundary", json::number(anchor)},
			{"gameExecutableSha256", json::string(elf_sha)},
			{"capsule", json::object({{"schema", json::string(capsule_schema)},
							{"anchor", json::string("before-native-match-initializer")}, {"initializer", json::number(kind)},
							{"sessionSeed", json::number(seed)}, {"players", json::array(std::move(selected))},
							{"selection", json::object({{"leftRow", json::number(left)}, {"rightRow", json::number(right)},
											  {"allowJinpachi", json::boolean(!pbd::character_rule::banned(generation))},
											  {"stage", json::number(stage)}, {"roundsToWin", json::number(rounds)}, {"roundTimeIndex", json::number(time)}})},
							{"hostContext", json::object({{"matchReseedsBefore", json::number(epoch.first)},
												{"mobBatchesBefore", json::number(epoch.second)},
												{"sourceSegment", json::number(pbd::fn_hook::keyed_segment.load())}, {"segmentCounter", json::number(counter)}})},
							{"generatorContext", std::move(rng)}, {"carryOver", std::move(carry)},
							{"nativeResourceState", json{}}, {"qualified", json::boolean(false)}})}});
		// The values the game's own Ranked screens write in this game, so the replay viewer writes the same (pbd::rank_screens).
		if (const auto values = pbd::rank_screens::armed(); pbd::rank_screens::configured().screens && values.any())
			for (auto& [name, value] : record.fields)
				if (name == "capsule")
					value.fields.emplace_back("rankScreens", pbd::rank_stakes::values_json(values));
		if (const auto placements = pbd::replay_notices::capture(pbd::set_notices::load(), generation))
			for (auto& [name, value] : record.fields)
				if (name == "capsule")
					value.fields.emplace_back("placements", *placements);
		pbd_start_record.reset();
		if (!append(record, true))
			return false;
		pbd_start_record = std::make_shared<const std::string>(pbd::set_channel::detail::serialize(record));
		return true;
	}
	bool capture_round(ppu_thread& owner, u32 kind)
	{
		if (!journal || end_boundary != ~0u || get_current_cpu_thread() != &owner || owner.gpr[2] != pbd::t5dr::toc ||
			pbd::practice::native::in_native_call())
			return true;
		u32 ordinal = 0;
		const u32 boundary = pbd::fn_hook::current_tick.load();
		if (!word(0x1008f0e4, ordinal) || ordinal >= round_cleanup.size() || boundary <= anchor ||
			boundary - anchor >= 100000 || (round_written & (u32{1} << ordinal)))
			return true;
		bool read = true;
		std::array<std::array<be_t<s16>, 2>, 2> scores{};
		if (!vm::try_access(0x100900d0, scores[0].data(), sizeof(scores[0]), false) ||
			!vm::try_access(0x100909a0, scores[1].data(), sizeof(scores[1]), false))
			return true;
		for (const auto& score : scores)
			if (score[0] < 0 || score[0] > 5 || score[1] < -1 || score[1] > 2)
				return true;
		const auto signed_number = [](s16 value)
		{
			auto out = json::number(0);
			out.text = std::to_string(value);
			return out;
		};
		auto carry = pbd::replay_carry::capture_game_values(owner, generation);
		auto point = json::object({{"initializer", json::number(kind)}, {"nativeOrdinal", json::number(ordinal)},
			{"frameCounter", number(0x1008f07c, read)}, {"inputOffset", json::number(boundary - anchor)},
			{"spawnSelector", number(0x10091cf0, read)},
			{"sourceSegment", json::number(pbd::fn_hook::keyed_segment.load())},
			{"scoreResult", json::array({json::array({signed_number(scores[0][0]), signed_number(scores[0][1])}),
								json::array({signed_number(scores[1][0]), signed_number(scores[1][1])})})},
			{"generatorContext", generators(owner, read)}});
		if (carry)
			point.fields.emplace_back("carryOver", pbd::replay_carry::encode_game_values(*carry));
		else
			pbd_record_log.warning("Round %u retained values unavailable; the recording keeps its round coordinates.", ordinal + 1);
		if (!read)
		{
			pbd_record_log.warning("Round %u snapshot unavailable; the live game recording continues.", ordinal + 1);
			return true;
		}
		if (kind == 6)
		{
			round_cleanup[ordinal] = std::move(point);
			return true;
		}
		if ((kind == 4 && ordinal != 0) || (kind == 5 && (ordinal == 0 || round_cleanup[ordinal].kind == pbd::set_channel::json_kind::null)))
			return true;
		const bool complete = carry && (kind == 4 || round_cleanup[ordinal].find("carryOver"));
		if (!append(json::object({{"type", json::string("round_start")}, {"game", json::number(game)},
						{"boundary", json::number(boundary)}, {"capsule", json::object({{"schema", json::string(complete ? "pbd-round-start-capsule-draft-2" : "pbd-round-start-capsule-draft-1")}, {"anchor", json::string("before-native-round-initializer")}, {"round", json::number(ordinal + 1)}, {"cleanup", kind == 4 ? json{} : round_cleanup[ordinal]}, {"ready", std::move(point)}, {"qualified", json::boolean(false)}})}}),
				true))
			return false;
		round_written |= u32{1} << ordinal;
		return true;
	}
};

pbd_live_replay::pbd_live_replay() noexcept = default;
void pbd_live_replay::drain() const noexcept
{
	if (data)
		data->journal.drain();
}
bool pbd_live_replay::enabled() const noexcept
{
	// A failed observer must leave the confirmed-prefix path before it can turn journal refusal into session failure.
	return data && !data->failed.load(std::memory_order_acquire) && !data->closed.load(std::memory_order_acquire) && data->journal.healthy();
}
std::shared_ptr<const std::string> pbd_live_replay::pbd_game_start_record(u64 generation) const noexcept
{
	try
	{
		if (!data || data->generation != generation || data->checkpoint_source)
			return {};
		std::lock_guard lock(data->mutex);
		if (data->failed || data->closed)
			return {};
		return data->pbd_start_record;
	}
	catch (...)
	{
		return {};
	}
}
pbd_live_replay::~pbd_live_replay()
{
	auto* self = this;
	active.compare_exchange_strong(self, nullptr);
	close_incomplete("runtime_closed");
}
void pbd_live_replay::close_incomplete(const char* reason) noexcept
{
	if (!data)
		return;
	try
	{
		std::lock_guard lock(data->mutex);
		data->closed = true;
		if (!data->journal)
			return;
		data->append(json::object({{"type", json::string("game_incomplete")}, {"game", json::number(data->game)},
			{"confirmedThrough", json::number(data->next)}, {"reason", json::string(reason)}}));
		data->journal.close();
	}
	catch (...)
	{
		data->failed = true;
	}
}
bool pbd_live_replay::requested() noexcept
{
#ifdef _WIN32
	const auto text = pbd::env::find_wide("PBD_REPLAY_V2_RECORD");
	return text && std::wstring_view(text) == L"1";
#else
	const auto text = pbd::env::find("PBD_REPLAY_V2_RECORD");
	return text && std::string_view(text) == "1";
#endif
}
bool pbd_live_replay::configure(u64 generation, const std::filesystem::path& run, bool session, bool lifecycle,
	bool playback_or_practice, u32 seed, std::array<player, 2> players, bool checkpoint_source) noexcept
{
	if (!requested())
		return true;
	try
	{
		if (data || !session || !lifecycle || playback_or_practice || !seed || !run.is_absolute() ||
			Emu.GetTitleID() != pbd::t5dr::title_id || Emu.GetExecutableHash() != pbd::t5dr::executable_hash)
			return false;
		for (const auto& value : players)
			if (value.id.empty() || value.id.size() > 128 || value.name.empty() || value.name.size() > 128 ||
				!pbd::set_channel::detail::valid_utf8(value.id) || !pbd::set_channel::detail::valid_utf8(value.name))
				return false;
		auto s = std::make_unique<state>();
		s->generation = generation;
		s->seed = seed;
		s->players = std::move(players);
		s->checkpoint_source = checkpoint_source;
		s->directory = run / "replay-v2-live";
		s->journal.start();
		if (!std::filesystem::create_directory(s->directory))
			return false;
		auto* empty = static_cast<pbd_live_replay*>(nullptr);
		data = std::move(s);
		if (!active.compare_exchange_strong(empty, this))
		{
			data.reset();
			return false;
		}
		return true;
	}
	catch (...)
	{
		data.reset();
		return false;
	}
}
bool pbd_live_replay::function_event(ppu_thread& owner, u32 address) noexcept
{
	auto* recorder = active.load(std::memory_order_acquire);
	if (!recorder || !recorder->data || owner.id != pbd::t5dr::main_ppu_id)
		return true;
	auto& s = *recorder->data;
	try
	{
		if (s.generation != static_cast<u64>(Emu.GetEmulationIdentifier()) || pbd::fn_hook::current_pass.load())
			return true;
		std::lock_guard lock(s.mutex);
		if (s.closed)
			return true;
		if (s.failed || !s.journal.healthy())
			return false;
		if (address == 0x5a554)
		{
			bool read = true;
			s.style_producer = json::object({{"generatorContext", generators(owner, read)},
				{"nativeOrdinal", number(0x1008f0e4, read)}, {"nativeReplayFlag", number(0x10425ce0, read)},
				{"leftRow", number(0x10093f50, read)}, {"rightRow", number(0x10093f54, read)}});
			s.style_producer_boundary = pbd::fn_hook::current_tick.load();
			if (!read)
			{
				s.failed = true;
				return false;
			}
		}
		if (address == 0x159f0 && (owner.gpr[3] == 2 || owner.gpr[3] == 8))
			if (!s.start(owner, static_cast<u32>(owner.gpr[3])))
			{
				s.failed = true;
				pbd_record_log.warning("Game-start capture refused at 0x%x; replay recording stopped, gameplay continues.", address);
				return false;
			}
		if (address == 0x159f0 && owner.gpr[3] == 2)
			pbd::anchor_dump::write(s.game, "live");
		if (address == 0x159f0 && (owner.gpr[3] == 4 || owner.gpr[3] == 5 || owner.gpr[3] == 6))
			if (!s.capture_round(owner, static_cast<u32>(owner.gpr[3])))
			{
				s.failed = true;
				return false;
			}
		return true;
	}
	catch (...)
	{
		s.failed = true;
		return false;
	}
}
void pbd_live_replay::observe_boundary(u32 boundary, u32 scene) noexcept
{
	if (!data)
		return;
	auto& s = *data;
	try
	{
		std::lock_guard lock(s.mutex);
		if (!s.journal || s.failed || boundary <= s.anchor || s.end_boundary != ~0u)
			return;
		if (scene == 16 || scene == 17)
			s.battle_seen = true;
		else if (s.battle_seen)
		{
			// Preserve results at the actual owner boundary, not when a delayed confirmed prefix catches up.
			bool read = true;
			auto left = bytes(0x100900d0, 4, read), right = bytes(0x100909a0, 4, read);
			if (!read)
			{
				s.failed = true;
				return;
			}
			s.end_boundary = boundary;
			s.end = json::object({{"type", json::string("game_end")}, {"game", json::number(s.game)},
				{"boundary", json::number(boundary)}, {"scoreLeftHex", std::move(left)},
				{"scoreRightHex", std::move(right)}, {"resultQualified", json::boolean(false)}});
		}
	}
	catch (...)
	{
		s.failed = true;
	}
}
bool pbd_live_replay::confirmed(u32 boundary, u32 segment, u32 counter, std::array<u16, 2> input,
	u32 scene, bool executed, const pbd::lobby_set::sync_facts& sync) noexcept
{
	if (!data)
		return true;
	auto& s = *data;
	try
	{
		std::lock_guard lock(s.mutex);
		if (s.failed || !s.journal.healthy())
			return false;
		if (!s.journal || boundary < s.next)
			return true;
		if (boundary > s.next)
			return false;
		++s.next;
		if (boundary == s.end_boundary)
		{
			if (!s.append(std::move(s.end), true))
				return false;
			s.journal.close();
			return true;
		}
		if (!executed)
			return s.append(json::object({{"type", json::string("alignment_gap")},
				{"boundary", json::number(boundary)}, {"sourceSegment", json::number(segment)}}));
		if (scene != 8 && scene != 16 && scene != 17)
			return false; // No character-select input in a game recording.
		if (!sync.verified || sync.boundary != boundary || sync.segment != segment || sync.scene != scene ||
			((scene == 16 || scene == 17) && sync.mask != 7))
			return false;
		const auto encoded = pbd::lobby_set::sync_record(sync);
		const std::vector<u8> facts(encoded.begin(), encoded.end());
		// Persistence runs on the journal writer; this callback only queues the captured immutable record.
		return s.append(json::object({{"type", json::string("input")}, {"boundary", json::number(boundary)},
			{"sourceSegment", json::number(segment)}, {"counter", json::number(counter)},
			{"leftDigital", json::number(input[0])}, {"rightDigital", json::number(input[1])},
			{"syncFacts", json::string(pbd::lobby_set::base64url(facts))}}));
	}
	catch (...)
	{
		s.failed = true;
		return false;
	}
}
