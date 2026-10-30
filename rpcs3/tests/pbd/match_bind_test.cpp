#include "../../Emu/PBD/pbd_match_bind.h"
#include "../../Emu/PBD/pbd_held_presentation.h"
#include "pbd_test.h"
#include "../../Emu/PBD/pbd_checkpoint_compatibility.h"
#include "../../Emu/PBD/pbd_match_return.h"
#include "../../Emu/PBD/pbd_match_pad.h"
using pbd::test::check;

PBD_TEST(PbdMatchBind, ControllerReconnectBeforeActivation)
{
	using namespace pbd::match_pad;
	unsigned polls = 0, waits = 0;
	check(await_connection([&]
			  {
				  return ++polls == 3;
			  },
			  []
			  {
				  return false;
			  },
			  [&]
			  {
				  ++waits;
			  }) == connection_result::ready);
	check(polls == 3 && waits == 2);
	polls = waits = 0;
	check(await_connection([&]
			  {
				  ++polls;
				  return true;
			  },
			  []
			  {
				  return true;
			  },
			  [&]
			  {
				  ++waits;
			  }) == connection_result::cancelled);
	check(!polls && !waits);
	check(await_connection([]
			  {
				  return false;
			  },
			  []
			  {
				  return false;
			  },
			  [&]
			  {
				  ++waits;
			  },
			  std::chrono::milliseconds(0)) == connection_result::expired);
	check(!waits);
}

PBD_TEST(PbdMatchBind, NegotiatedDelay)
{
	const pbd::match_bind::identity identity{42, std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c')};
	const auto make = [&](std::string policy, bool activation = true)
	{
		return "{\"schema\":\"pbd-match-bind-1\",\"nativeGeneration\":\"42\",\"checkpointSha256\":" + pbd::json::quoted(identity.checkpoint) +
		       ",\"emulatorSha256\":" + pbd::json::quoted(identity.emulator) + ",\"recipeSha256\":" + pbd::json::quoted(identity.recipe) +
		       ",\"setId\":\"set-1\",\"seed\":1" + (activation ? ",\"spectatorFromStart\":false" : "") +
		       ",\"peer\":{\"role\":0,\"session\":7,\"localPort\":5000,\"remotePort\":5001,\"address\":\"127.0.0.1\"" + policy + "}}";
	};
	for (const auto delay : {"0", "3", "5", "\"auto\""})
	{
		const auto result = pbd::match_bind::parse(make(std::string(",\"inputDelay\":") + delay + ",\"predictionBound\":8"), identity);
		check(result.value.has_value());
		check(result.value->prediction_bound == 8);
		check(result.value->input_delay.has_value() == (std::string_view(delay) != "\"auto\""));
		if (result.value->input_delay)
			check(std::to_string(*result.value->input_delay) == delay);
		const auto bound = pbd::json::detail::json_parser(pbd::match_bind::bound_text(identity, "set-1", &*result.value)).parse();
		check(bound.value);
		const auto* written_delay = bound.value->find("inputDelay");
		std::uint32_t number = 0;
		if (result.value->input_delay)
			check(pbd::json::detail::number32(written_delay, 0, 5, number) && number == *result.value->input_delay);
		else
			check(pbd::json::detail::is(written_delay, "auto"));
		check(pbd::json::detail::number32(bound.value->find("predictionBound"), 2, 8, number) && number == 8);
	}
	const auto legacy = pbd::match_bind::parse(make("", false), identity);
	check(legacy.value.has_value() && !legacy.value->prediction_bound && !legacy.value->input_delay);
	auto equal_ports = make("", false);
	const auto remote_port = equal_ports.find("\"remotePort\":5001");
	check(remote_port != std::string::npos);
	equal_ports.replace(remote_port, std::string("\"remotePort\":5001").size(), "\"remotePort\":5000");
	const auto accepted = pbd::match_bind::parse(equal_ports, identity);
	check(accepted.value && accepted.value->local_port == 5000 && accepted.value->remote_port == 5000);
	for (const auto policy : {
			 ",\"inputDelay\":-1,\"predictionBound\":8", ",\"inputDelay\":6,\"predictionBound\":8",
			 ",\"inputDelay\":1.5,\"predictionBound\":8", ",\"inputDelay\":\"3\",\"predictionBound\":8",
			 ",\"inputDelay\":null,\"predictionBound\":8", ",\"inputDelay\":3,\"predictionBound\":1",
			 ",\"inputDelay\":3,\"predictionBound\":9", ",\"inputDelay\":3,\"predictionBound\":7.5",
			 ",\"inputDelay\":3", ",\"predictionBound\":8"})
		check(!pbd::match_bind::parse(make(policy), identity).value);
	check(!pbd::match_bind::parse(make(",\"inputDelay\":3,\"predictionBound\":8", false), identity).value);
	const auto ready = pbd::json::detail::json_parser(pbd::match_bind::ready_text(identity)).parse();
	check(ready.value);
	const auto* delay_at_bind = ready.value->find("delayAtBind");
	check(delay_at_bind && delay_at_bind->kind == pbd::json::json_kind::boolean && delay_at_bind->truth);
}

PBD_TEST(PbdMatchBind, Activation)
{
	const pbd::match_bind::identity identity{42, std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c')};
	const auto activate = [&](std::string generation, std::string set, std::string checkpoint)
	{
		return "{\"schema\":\"pbd-match-activate-1\",\"nativeGeneration\":\"" + generation +
		       "\",\"setId\":\"" + set + "\",\"checkpointSha256\":\"" + checkpoint + "\"}";
	};
	check(pbd::match_bind::activates(activate("42", "set-1", identity.checkpoint), identity, "set-1"));
	check(!pbd::match_bind::activates(activate("41", "set-1", identity.checkpoint), identity, "set-1"));
	check(!pbd::match_bind::activates(activate("42", "set-2", identity.checkpoint), identity, "set-1"));
	check(!pbd::match_bind::activates(activate("42", "set-1", identity.recipe), identity, "set-1"));
	pbd::held_presentation::lifecycle output(pbd::held_presentation::purpose::match);
	output.park(true);
	check(output.parked() && !output.can_show() && output.hold_audio());
	check(output.bound(pbd::held_presentation::purpose::match));
	check(output.parked() && !output.can_show());
	output.park(false);
	output.play(0);
	check(!output.parked() && !output.can_show());
	output.rearm();
	check(!output.can_show() && output.hold_audio());
	check(output.bound(pbd::held_presentation::purpose::match));
	check(pbd::checkpoint_compatibility::accepts(pbd::checkpoint_compatibility::key));
	check(!pbd::checkpoint_compatibility::accepts("other-capture"));
}

PBD_TEST(PbdMatchBind, ReturnIdentity)
{
#ifdef _WIN32
	const std::string checkpoint = "C:\\game\\clean.SAVESTAT";
#else
	const std::string checkpoint = "/game/clean.SAVESTAT";
#endif
	const auto make = [&](std::string generation, std::string digest, std::string source, std::string extra = "")
	{
		return "{\"schema\":\"pbd-match-return-1\",\"nativeGeneration\":" + pbd::json::quoted(generation) +
		       ",\"checkpointSha256\":" + pbd::json::quoted(digest) + ",\"checkpoint\":" + pbd::json::quoted(source) +
		       ",\"config\":" + pbd::json::quoted(checkpoint + ".yml") + ",\"channel\":" + pbd::json::quoted(checkpoint + ".channel") +
		       ",\"environment\":{\"PBD_MATCH_STANDBY_CHANNEL\":" + pbd::json::quoted(checkpoint + ".channel") + extra + "}}";
	};
	const auto parse = [&](const std::string& text)
	{
		return pbd::match_return::parse(text, "42", std::string(64, 'a'), checkpoint).value.has_value();
	};
	check(parse(make("42", std::string(64, 'a'), checkpoint)));
	check(!parse(make("41", std::string(64, 'a'), checkpoint)));
	check(!parse(make("42", std::string(64, 'b'), checkpoint)));
	check(!parse(make("42", std::string(64, 'a'), checkpoint + ".other")));
	check(!parse(make("42", std::string(64, 'a'), checkpoint, ",\"PBD_PEER_AUTH_NONCE\":\"secret\"")));
	check(!parse(make("42", std::string(64, 'a'), checkpoint, ",\"PBD_PEER_ROLE\":\"0\"")));
	check(!parse(make("42", std::string(64, 'a'), checkpoint, ",\"PATH\":\"other\"")));
}
