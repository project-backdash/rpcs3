#include "pbd_test.h"

#include "Emu/PBD/pbd_output_gain.h"
#include "Emu/PBD/pbd_replay_standby.h"

#include <cmath>
#include <cstring>
#include <vector>

using pbd::test::check;

namespace
{
	using namespace pbd::output_gain;

	// cellAudio's block: 256 frames of stereo.
	constexpr std::uint32_t block = 256, channels = 2;

	// A block of a full-scale tone: every sample 1, so each output sample is the gain of its frame.
	std::vector<float> tone()
	{
		return std::vector<float>(block * channels, 1.0f);
	}

	// The gain of every frame over `blocks` blocks toward `target`.
	std::vector<float> run(gate& g, std::uint32_t blocks, float target)
	{
		std::vector<float> gains;
		for (std::uint32_t b = 0; b < blocks; ++b)
		{
			auto samples = tone();
			g.apply(samples.data(), channels, block, target);
			for (std::uint32_t frame = 0; frame < block; ++frame)
			{
				check(samples[frame * channels] == samples[frame * channels + 1], "both channels get the same gain");
				gains.push_back(samples[frame * channels]);
			}
		}
		return gains;
	}

	// Opens a gate and lets it settle at `target`.
	void settle(gate& g, float target)
	{
		g.open(fade_in_ms);
		run(g, frames_in(fade_in_ms) / block + 1, target);
	}

	// The first frame at which the gain equals `value`, counted from 1.
	std::size_t reaches(const std::vector<float>& gains, float value)
	{
		for (std::size_t i = 0; i < gains.size(); ++i)
			if (gains[i] == value)
				return i + 1;
		return 0;
	}

	void check_monotonic(const std::vector<float>& gains, bool rising, float largest_step)
	{
		for (std::size_t i = 1; i < gains.size(); ++i)
		{
			const float change = gains[i] - gains[i - 1];
			check(rising ? change >= 0.0f : change <= 0.0f, "the ramp never turns back");
			check(std::fabs(change) <= largest_step, "no frame jumps further than one ramp step");
		}
	}
} // namespace

PBD_TEST(PbdOutputGain, AClosedGateOutputsSilenceWhateverTheVolume)
{
	gate g;
	for (const float target : {1.0f, 0.36f, 4.0f})
	{
		auto samples = tone();
		g.apply(samples.data(), channels, block, target);
		for (const float sample : samples)
			check(sample == 0.0f, "nothing is heard before the gate opens");
	}
	check(!g.opened());
}

PBD_TEST(PbdOutputGain, TheFirstFrameOpensOnlyARunWithoutTheInputHook)
{
	gate hooked;
	hooked.first_frame(true);
	check(!hooked.opened(), "the input hook opens the gate itself, after the start sync");
	gate plain;
	plain.first_frame(false);
	check(plain.opened());
}

PBD_TEST(PbdOutputGain, HiddenPreparationAndReturningToTheLauncherStaySilent)
{
	check(initially_visible(nullptr, nullptr) && initially_visible("", "0"));
	check(!initially_visible("host", "0") && !initially_visible(nullptr, "1"));
	gate g(nullptr, false);
	g.first_frame(false);
	for (const auto sample : run(g, 300, level(65)))
		check(sample == 0.0f);
	g.visible(true);
	check(run(g, frames_in(fade_in_ms) / block + 2, level(65)).back() == level(65));
	g.visible(false);
	for (const auto sample : run(g, 1, level(65)))
		check(sample == 0.0f);
	// A changed volume while hidden is restored without changing the player's choice or other host mutes.
	for (const auto sample : run(g, 2, level(40)))
		check(sample == 0.0f);
	g.visible(true);
	check(run(g, frames_in(fade_in_ms) / block + 2, level(40)).back() == level(40));
	g.visible(false);
	g.visible(true);
	check(run(g, 2, 0.0f).back() == 0.0f);
}

PBD_TEST(PbdOutputGain, TheFirstOpeningFadesInOverItsFadeLength)
{
	gate g;
	g.open(fade_in_ms);
	const auto gains = run(g, frames_in(fade_in_ms) / block + 2, 1.0f);
	check(reaches(gains, 1.0f) == frames_in(fade_in_ms), "the fade lands on the volume at its last frame");
	check(gains.front() > 0.0f && gains.front() < 0.0001f, "the fade starts from silence");
	check_monotonic(gains, true, 1.0f / static_cast<float>(frames_in(fade_in_ms)) * 1.01f);
	g.open(10);
	check(g.opened(), "a second open changes nothing");
}

PBD_TEST(PbdOutputGain, AMuteReachesSilenceIn20msAndARestoreReturnsIn250ms)
{
	gate g;
	settle(g, 1.0f);
	const auto down = run(g, 8, 0.0f);
	check(reaches(down, 0.0f) == frames_in(fall_ms), "a mute is silent after 20 ms");
	check(frames_in(fall_ms) == 960);
	check_monotonic(down, false, 1.0f / 960.0f * 1.01f);
	const auto up = run(g, frames_in(rise_ms) / block + 2, 1.0f);
	check(reaches(up, 1.0f) == frames_in(rise_ms), "a restore is back at the volume after 250 ms");
	check(frames_in(rise_ms) == 12000);
	check_monotonic(up, true, 1.0f / 12000.0f * 1.01f);
}

PBD_TEST(PbdOutputGain, AMuteDuringTheFadeInKeepsTheFadeForTheFirstRise)
{
	gate g;
	g.open(fade_in_ms);
	run(g, 10, 1.0f);
	run(g, 10, 0.0f);
	const auto again = run(g, frames_in(fade_in_ms) / block + 2, 1.0f);
	check(reaches(again, 1.0f) == frames_in(fade_in_ms), "the player hears the whole fade-in once");
}

PBD_TEST(PbdOutputGain, AVolumeStepGlidesFromWhereTheGainIs)
{
	gate g;
	settle(g, level(60));
	const auto louder = run(g, frames_in(rise_ms) / block + 2, level(65));
	check(reaches(louder, level(65)) == frames_in(rise_ms));
	check(louder.front() > level(60) && louder.front() < level(65));
	// A change in the middle of a ramp starts the next ramp from the gain reached, without a jump.
	const auto half = run(g, 2, 0.0f);
	check(half.back() > 0.0f, "two blocks are less than the 20 ms of a fall");
	const auto back = run(g, 2, level(65));
	check(back.front() > half.back(), "the rise starts from the gain the fall reached");
	check(back.front() - half.back() <= level(65) / 12000.0f * 1.01f);
}

PBD_TEST(PbdOutputGain, AtASettledGainOfOneTheMixIsLeftBitForBit)
{
	gate g;
	settle(g, 1.0f);
	std::vector<float> mixed(block * channels);
	for (std::size_t i = 0; i < mixed.size(); ++i)
		mixed[i] = std::sin(static_cast<float>(i) * 0.37f) * 0.8f + 1e-30f * static_cast<float>(i % 3);
	auto out = mixed;
	g.apply(out.data(), channels, block, 1.0f);
	check(std::memcmp(out.data(), mixed.data(), mixed.size() * sizeof(float)) == 0, "the output is the mix, unchanged");
	// Any other settled gain is one multiply per sample, as the mix's master volume was.
	gate half;
	settle(half, level(50));
	out = mixed;
	half.apply(out.data(), channels, block, level(50));
	for (std::size_t i = 0; i < out.size(); ++i)
		check(out[i] == mixed[i] * 0.25f);
}

PBD_TEST(PbdOutputGain, TheVolumeCurveIsSquaredUpTo100)
{
	check(level(0) == 0.0f);
	check(level(100) == 1.0f);
	check(level(50) == 0.25f);
	check(std::fabs(level(60) - 0.36f) < 1e-6f, "the launcher's default is about 9 dB below full");
	check(std::fabs(20.0f * std::log10(level(60)) + 8.87f) < 0.01f);
	for (int v = 5; v <= 100; v += 5)
		check(level(v) > level(v - 5), "every step is louder than the one below");
	check(level(150) == 1.5f && level(200) == 2.0f, "above 100 the gain stays linear");
	check(level(-5) == 0.0f && level(250) == 2.0f, "out-of-range values are clamped");
}

PBD_TEST(PbdOutputGain, ReplayChecksDoNotUnhideOrUnmuteStandby)
{
	pbd::held_presentation::lifecycle presentation(true);
	gate g(&presentation);
	settle(g, 1.0f);
	check(!presentation.can_show() && presentation.hold_audio());
	check(presentation.bound());
	check(!presentation.bound(), "one resident runtime cannot accept a second bind");
	presentation.picture(true);
	presentation.audio(1.0f);
	presentation.submitted(true);
	check(!presentation.ready() && !presentation.can_show(), "file admission cannot acknowledge picture or sound");
	const auto silent = run(g, 2, 1.0f);
	for (const auto sample : silent)
		check(sample == 0.0f);
}

PBD_TEST(PbdOutputGain, ReplaySoundWaitsForVisiblePictureThenRestoresTheCurrentVolume)
{
	pbd::held_presentation::lifecycle presentation(true);
	gate g(&presentation);
	check(presentation.bound());
	presentation.play(1234);
	check(presentation.can_show() && presentation.hold_audio());
	presentation.picture(false);
	g.open(fade_in_ms);
	const auto silent = run(g, 2, level(80));
	for (const auto sample : silent)
		check(sample == 0.0f);
	presentation.picture(true);
	check(!presentation.ready(), "a picture alone does not acknowledge restored sound");
	const auto restored = run(g, frames_in(fade_in_ms) / block + 2, level(65));
	check(reaches(restored, level(65)) == frames_in(fade_in_ms));
	check(!presentation.ready(), "mixing alone does not prove the output backend consumed sound");
	presentation.submitted(false);
	check(!presentation.ready(), "a silent output buffer does not prove restored nonzero volume");
	presentation.submitted(true);
	check(presentation.ready() && presentation.boundary() == 1234);
	check(presentation.gain() == level(65) && presentation.audio_us() >= presentation.picture_us());
}

PBD_TEST(PbdOutputGain, ReplayReadinessPreservesAnIntentionalPlayerMute)
{
	pbd::held_presentation::lifecycle presentation(true);
	gate g(&presentation);
	check(presentation.bound());
	presentation.play(5);
	presentation.picture(true);
	g.open(fade_in_ms);
	run(g, 1, 0.0f);
	presentation.submitted(false);
	check(presentation.ready() && presentation.gain() == 0.0f);
}

PBD_TEST(PbdOutputGain, EndingReplayStandbyIsTerminalAndImmediatelySilent)
{
	pbd::held_presentation::lifecycle presentation(true);
	gate g(&presentation);
	check(presentation.bound());
	presentation.play(3);
	presentation.picture(true);
	settle(g, 1.0f);
	presentation.submitted(true);
	check(presentation.ready());
	presentation.end();
	presentation.play(4);
	presentation.picture(true);
	check(!presentation.can_show() && !presentation.ready() && !presentation.bound());
	for (const auto sample : run(g, 1, 1.0f))
		check(sample == 0.0f, "a stale callback or previous open audio gate cannot revive a closed standby");
}

PBD_TEST(PbdOutputGain, OrdinaryPlaybackNeedsNoStandbyAcknowledgement)
{
	pbd::held_presentation::lifecycle presentation;
	gate g(&presentation);
	check(!presentation.active() && presentation.can_show() && !presentation.hold_audio());
	presentation.end();
	settle(g, 1.0f);
	check(!presentation.active() && presentation.can_show());
	for (const auto sample : run(g, 1, 1.0f))
		check(sample == 1.0f);
}

PBD_TEST(PbdOutputGain, HeldMatchWaitsForItsOwnBindAndVisiblePicture)
{
	namespace held = pbd::held_presentation;
	held::lifecycle presentation(held::requested(nullptr, "C:/private/match"));
	gate g(&presentation);
	g.open(fade_in_ms);
	presentation.play(0);
	presentation.picture(true);
	check(!presentation.can_show() && !presentation.bound(), "replay admission cannot release a held match");
	for (const auto sample : run(g, 2, level(80)))
		check(sample == 0.0f);
	check(presentation.bound(held::purpose::match));
	check(!presentation.can_show() && presentation.hold_audio(), "match admission alone is not play");
	presentation.play(7);
	check(!presentation.can_show());
	pbd::frame_presentation::frame_tag_queue::record token;
	token.boundary = 7;
	token.session_epoch = 42;
	token.scene = 8;
	token.live = token.accepted = token.flip_head_verified = true;
	token.command = pbd::frame_presentation::command_state::exact;
	check(presentation.play_image(token, 42, true));
	presentation.picture(false);
	for (const auto sample : run(g, 1, level(65)))
		check(sample == 0.0f);
	presentation.picture(true);
	const auto restored = run(g, frames_in(fade_in_ms) / block + 2, level(65));
	check(reaches(restored, level(65)) == frames_in(fade_in_ms), "restore uses the current saved volume");
	check(!presentation.ready());
	presentation.submitted(true);
	check(presentation.ready() && presentation.gain() == level(65) && presentation.boundary() == 7);
	presentation.end();
	check(!presentation.bound(held::purpose::match), "a held fighter is single-use per pristine generation");
	for (const auto sample : run(g, 1, level(65)))
		check(sample == 0.0f);
}

PBD_TEST(PbdOutputGain, HeldMatchRejectsCachedSelectAndUnprovedOrOldVsImages)
{
	namespace held = pbd::held_presentation;
	held::lifecycle presentation(held::purpose::match);
	check(presentation.bound(held::purpose::match));
	pbd::frame_presentation::frame_tag_queue::record token;
	token.boundary = 3;
	token.session_epoch = 42;
	token.scene = 3;
	token.live = token.accepted = token.flip_head_verified = true;
	token.command = pbd::frame_presentation::command_state::via_chain;
	check(!presentation.play_image(token, 42, true));
	token.scene = 8;
	check(!presentation.play_image(token, 41, true));
	check(!presentation.play_image(token, 42, false));
	token.live = false;
	check(!presentation.play_image(token, 42, true));
	token.live = true;
	token.command = pbd::frame_presentation::command_state::mismatch;
	check(!presentation.play_image(token, 42, true));
	token.command = pbd::frame_presentation::command_state::exact;
	token.flip_head_verified = false;
	check(!presentation.play_image(token, 42, true));
	check(!presentation.can_show() && presentation.hold_audio());
	token.flip_head_verified = true;
	check(presentation.play_image(token, 42, true));
	check(presentation.can_show() && presentation.boundary() == 3);
	presentation.rearm();
	check(!presentation.play_image(token, 42, true));
	check(presentation.bound(held::purpose::match));
	token.scene = 12;
	check(presentation.play_image(token, 42, true));
}

PBD_TEST(PbdOutputGain, AStandbyAdmittedAsASpectatorCanJoinDuringAFight)
{
	namespace held = pbd::held_presentation;
	held::lifecycle presentation(held::purpose::match);
	check(presentation.bound(held::purpose::match, false));
	presentation.play(782);
	check(presentation.can_show() && presentation.boundary() == 782);
	presentation.rearm();
	check(presentation.bound(held::purpose::match));
	presentation.play(0);
	check(!presentation.can_show());
}

PBD_TEST(PbdOutputGain, AColdFixedMatchUsesTheSameVsImageGate)
{
	namespace held = pbd::held_presentation;
	check(held::requested(nullptr, nullptr, "C:/private/set", "1") == held::purpose::match);
	check(held::requested(nullptr, nullptr, "C:/private/set", "0") == held::purpose::none);
	check(held::requested(nullptr, nullptr, "", "1") == held::purpose::none);
	check(held::requested("1", nullptr, "C:/private/set", "1") == held::purpose::conflict);
	held::lifecycle presentation(held::requested(nullptr, nullptr, "C:/private/set", "1"));
	check(!presentation.can_show() && presentation.bound(held::purpose::match));
	presentation.play(0);
	check(!presentation.can_show() && presentation.hold_audio());
}

PBD_TEST(PbdOutputGain, ConflictingHeldPurposesStayHiddenAndSilent)
{
	namespace held = pbd::held_presentation;
	held::lifecycle presentation(held::requested("1", "C:/private/match"));
	gate g(&presentation);
	check(!presentation.bound() && !presentation.bound(held::purpose::match) && !presentation.bound(held::purpose::conflict));
	presentation.play(1);
	presentation.picture(true);
	settle(g, 1.0f);
	presentation.submitted(true);
	check(!presentation.can_show() && !presentation.ready());
	for (const auto sample : run(g, 1, 1.0f))
		check(sample == 0.0f);
}

PBD_TEST(PbdOutputGain, MatchAndReplayCallbacksShareOnePresentationGate)
{
	namespace held = pbd::held_presentation;
	check(held::requested(nullptr, nullptr) == held::purpose::none);
	check(held::requested("0", "") == held::purpose::none);
	check(held::requested("1", "") == held::purpose::replay);
	held::lifecycle replay(held::purpose::replay);
	check(!replay.bound(held::purpose::match) && replay.bound(), "match admission cannot release a replay hold");
}

PBD_TEST(PbdOutputGain, StandbyPresentationAcknowledgesOnlyTheAdmittedReplay)
{
	namespace standby = pbd::replay_standby;
	const standby::pbd_binding binding{123, 456, {}, std::string(64, 'a'), std::string(64, 'b'),
		std::string(64, 'c'), std::string(64, 'd')};
	pbd::held_presentation::lifecycle presentation(true);
	check(presentation.bound());
	check(standby::pbd_presentation_text(binding, presentation).empty());
	check(standby::pbd_picture_text(binding, presentation).empty());
	presentation.play(1234);
	presentation.picture(false);
	check(standby::pbd_picture_text(binding, presentation).empty(), "hidden picture is not presentation");
	presentation.picture(true);
	const auto picture = pbd::set_channel::detail::json_parser(standby::pbd_picture_text(binding, presentation), 64).parse();
	check(picture.value && pbd::set_channel::detail::is(picture.value->find("schema"), standby::pbd_picture_schema) &&
			  pbd::set_channel::detail::is(picture.value->find("type"), "picture-ready"),
		"first picture must not wait for audio");
	check(pbd::set_channel::detail::serialize(*picture.value->find("startBoundary")) == "1234");
	check(pbd::set_channel::detail::serialize(*picture.value->find("pictureElapsedUs")) == std::to_string(presentation.picture_us()));
	presentation.audio(0.36f);
	check(standby::pbd_presentation_text(binding, presentation).empty(), "mixing is not a device acknowledgement");
	presentation.submitted(true);
	const auto bound = pbd::set_channel::detail::json_parser(standby::pbd_bound_text(binding), 64).parse();
	const auto shown = pbd::set_channel::detail::json_parser(standby::pbd_presentation_text(binding, presentation), 64).parse();
	check(bound.value && shown.value &&
			  pbd::set_channel::detail::is(shown.value->find("schema"), standby::pbd_presentation_schema) &&
			  pbd::set_channel::detail::is(shown.value->find("type"), "presentation-ready"),
		"presentation report does not parse");
	for (const auto* key : {"nativeGeneration", "seed", "checkpointSha256", "emulatorSha256", "recipeSha256", "replaySha256"})
		check(pbd::set_channel::detail::serialize(*shown.value->find(key)) ==
				  pbd::set_channel::detail::serialize(*bound.value->find(key)),
			"presentation acknowledged another binding");
	for (const auto* key : {"nativeGeneration", "seed", "checkpointSha256", "emulatorSha256", "recipeSha256", "replaySha256"})
		check(pbd::set_channel::detail::serialize(*picture.value->find(key)) ==
				  pbd::set_channel::detail::serialize(*bound.value->find(key)),
			"picture acknowledged another binding");
	presentation.end();
	check(standby::pbd_presentation_text(binding, presentation).empty(), "ended standby publishes ready");
	check(standby::pbd_picture_text(binding, presentation).empty(), "ended standby publishes picture");
}
