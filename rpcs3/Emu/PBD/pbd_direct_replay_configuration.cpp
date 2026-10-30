#include "stdafx.h"
#include "pbd_direct_replay_state.h"
#include "Crypto/sha256.h"

std::unique_ptr<pbd_direct_match::state::round_start> pbd_direct_match::state::restore_point(const pbd::replay_game_file::round_point& point)
{
	auto saved = std::make_unique<state::round_start>();
	saved->kind = point.kind;
	saved->ordinal = point.ordinal;
	saved->frame_counter = point.counter;
	saved->input_offset = point.input_offset;
	if (point.input_offset < sync.size())
		saved->pbd_entry_counter = sync[point.input_offset].counter;
	saved->keyed_segment = point.segment;
	saved->spawn_selector = point.spawn_selector;
	for (u32 side = 0; side < 2; ++side)
		saved->result[side] = {point.result[side][0], point.result[side][1]};
	saved->rng = std::make_unique<generators>(point.generators);
	saved->carry = point.carry;
	return saved;
}

bool pbd_direct_match::configure(u64 generation, bool isolated, u32 seed) noexcept
{
	if (pbd_live_inputs())
		return data->generation == generation && data->seed == seed && isolated;
	if (!requested())
		return true;
	if (pbd::replay_standby::pbd_requested())
		return isolated && !seed && game_path().empty();
	return pbd_configure_file(generation, isolated, seed, game_path(), {});
}
bool pbd_direct_match::pbd_configure_file(u64 generation, bool isolated, u32 seed,
	const std::filesystem::path& source, std::string_view expected_sha256) noexcept
{
	try
	{
		if (data || !isolated || !seed || !source.is_absolute() || source.extension() != ".pbdreplay" ||
			Emu.GetTitleID() != pbd::t5dr::title_id || Emu.GetExecutableHash() != pbd::t5dr::executable_hash ||
			pbd::anchor_dump::replay_refuses_test_switches())
			return false;
		const auto size = std::filesystem::file_size(source);
		const auto written = std::filesystem::last_write_time(source);
		if (!size || size > pbd::replay_game_file::max_file_bytes)
			return false;
		std::ifstream stream(source, std::ios::binary);
		std::string text(static_cast<usz>(size) + 1, '\0');
		stream.read(text.data(), static_cast<std::streamsize>(text.size()));
		if (stream.bad() || stream.gcount() != static_cast<std::streamsize>(size) ||
			std::filesystem::file_size(source) != size || std::filesystem::last_write_time(source) != written)
			return false;
		text.resize(static_cast<usz>(size));
		if (!expected_sha256.empty())
		{
			std::array<unsigned char, 32> hash{};
			if (expected_sha256.size() != 64 || mbedtls_sha256_ret(
													reinterpret_cast<const unsigned char*>(text.data()), text.size(), hash.data(), 0))
				return false;
			constexpr char hex[] = "0123456789abcdef";
			for (usz i = 0; i < hash.size(); ++i)
				if (expected_sha256[2 * i] != hex[hash[i] >> 4] || expected_sha256[2 * i + 1] != hex[hash[i] & 15])
					return false;
		}
		auto parsed = pbd::replay_game_file::parse(text);
		if (const auto refusal = pbd::replay_game_file::playback_refusal(parsed); !refusal.empty())
		{
			pbd_replay_log.error("Replay file refused: %s", refusal);
			return false;
		}
		auto s = std::make_unique<state>();
		s->generation = generation;
		s->seed = seed;
		s->game = parsed.value->number;
		s->final_score = parsed.value->final_score;
		s->request_file = request_path();
		if (!s->request_file.empty() && !s->request_file.is_absolute())
			return false;
		if (!s->load_capsule(parsed.value->capsule))
			return false;
		for (const auto& input : parsed.value->inputs)
			s->inputs.push_back({input.segment, input.counter, input.words});
		s->sync = std::move(parsed.value->sync);
		const auto battle = std::find_if(s->sync.begin(), s->sync.end(), [](const auto& fact)
			{
				return fact.scene == 16 || fact.scene == 17;
			});
		s->first_battle_input = static_cast<u32>(std::distance(s->sync.begin(), battle));
		s->player_names = std::move(parsed.value->names);
		s->rank_screens = parsed.value->rank_screens;
		for (const auto& cap : parsed.value->rounds)
		{
			auto& slot = s->rounds[cap.number - 1];
			slot.ready = s->restore_point(cap.ready);
			if (cap.cleanup)
				slot.cleanup = s->restore_point(*cap.cleanup);
			s->captured_round_mask.fetch_or(u32{1} << (cap.number - 1));
		}
		auto* empty = static_cast<pbd_direct_match*>(nullptr);
		data = std::move(s);
		if (!active.compare_exchange_strong(empty, this))
		{
			data.reset();
			return false;
		}
		pbd::character_rule::configure(generation, !data->allow_jinpachi);
		data->event("configured", "", {{"runtimeGeneration", json::number(generation)}, {"availableRounds", json::number(data->captured_round_mask.load())}, {"perFrameSync", json::boolean(!data->sync.empty())}, {"playback", json::string("exact")}, {"recordedBy", json::string(parsed.value->emulator)}});
		return true;
	}
	catch (...)
	{
		data.reset();
		return false;
	}
}
bool pbd_direct_match::pbd_live_inputs() const noexcept
{
	return data && data->pbd_external_inputs;
}

u32 pbd_direct_match::pbd_start_boundary() const noexcept
{
	return data && data->redirected && !data->failed ? data->anchor : ~0u;
}

bool pbd_direct_match::pbd_configure_capsule_start(u64 generation, bool isolated, u32 seed, u32 source_game, std::string_view capsule) noexcept
{
	try
	{
		const auto* enabled = pbd::env::find("PBD_SPECTATOR_NATIVE_2");
		if (!enabled || std::string_view(enabled) != "1" || data || !isolated || !generation || !seed ||
			!source_game || source_game > 65535 || capsule.empty() || capsule.size() > 256 * 1024 ||
			Emu.GetTitleID() != pbd::t5dr::title_id || Emu.GetExecutableHash() != pbd::t5dr::executable_hash ||
			pbd::anchor_dump::replay_refuses_test_switches())
			return false;
		auto parsed = pbd::set_channel::detail::json_parser(capsule, 16384, 256 * 1024, 65536).parse();
		if (!parsed.value)
			return false;
		auto s = std::make_unique<state>();
		s->generation = generation;
		s->seed = seed;
		s->game = source_game;
		s->pbd_external_inputs = true;
		if (!s->load_capsule(*parsed.value) || !s->recorded_input_context)
			return false;
		s->rank_screens = pbd::rank_stakes::read_values(parsed.value->find("rankScreens"));
		auto* empty = static_cast<pbd_direct_match*>(nullptr);
		data = std::move(s);
		if (!active.compare_exchange_strong(empty, this))
		{
			data.reset();
			return false;
		}
		pbd::character_rule::configure(generation, !data->allow_jinpachi);
		data->event("capsule_start_configured", "", {{"runtimeGeneration", json::number(generation)}, {"sourceGame", json::number(source_game)}, {"liveInputs", json::boolean(true)}});
		return true;
	}
	catch (...)
	{
		auto* self = this;
		active.compare_exchange_strong(self, nullptr);
		data.reset();
		return false;
	}
}

bool pbd_direct_match::start(ppu_thread& owner, u32 boundary) noexcept
{
	if (!data || data->started)
		return true;
	auto& s = *data;
	try
	{
		s.started = true;
		s.accepted = std::chrono::steady_clock::now();
		const char* why = "stock_resource_initializer_request";
		if (boundary || pbd::fn_hook::current_pass.load())
			return s.refuse(why);
		if (!pbd::replay_native::service_owner(owner, s.generation, &why))
			return s.refuse(why);
		const state::pbd_owner_run admitted(s, owner);
		if (!pbd::replay_native::can_prepare_resources(owner, s.generation, s.choices, &why))
			return s.refuse(why);
		if (!s.carry(owner, s.pre_ordinal))
			return s.refuse("native_start_carry");
		if (!pbd::fn_hook::begin_direct_match_epoch(owner, s.seed, s.prior))
			return s.refuse("native_start_epoch");
		if (!pbd::replay_native::prepare_resources(owner, s.generation, s.choices, &why))
			return s.refuse(why);
		s.begin_loading();
		s.event("resource_initializer_requested");
		return !s.failed;
	}
	catch (...)
	{
		return s.refuse("direct_start_exception");
	}
}
