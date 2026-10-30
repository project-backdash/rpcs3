#include "pbd_test.h"
#include "Emu/PBD/pbd_replay_entry.h"

#include <vector>

using pbd::replay_entry::refusal;
using pbd::test::check;

PBD_TEST(PbdReplayEntry, RecordingRefusalContinuesGuest)
{
	unsigned recording_calls = 0, boot_calls = 0;
	unsigned guest = 0;
	const auto recording = [&](unsigned& owner, std::uint32_t address)
	{
		check(&owner == &guest && address == 0x159f0, "Recording receives the original guest and initializer address");
		++recording_calls;
		return false;
	};
	for (unsigned event = 0; event < 3; ++event)
	{
		const auto result = pbd::replay_entry::enter(pbd::replay_entry::context{guest, 0x159f0}, [](auto&, auto)
			{
				return true;
			},
			recording, [&](auto&, auto)
			{
				++boot_calls;
				return true;
			});
		check(result == refusal::none, "A refused recorder must leave guest execution enabled");
	}
	check(recording_calls == 3, "Each probe observes the recorder's refusal");
	check(boot_calls == 3, "Later guest entry hooks still run after recording refusal");
}

PBD_TEST(PbdReplayEntry, RequiredEntryRefusalsRemainFatal)
{
	unsigned recording_calls = 0, boot_calls = 0;
	unsigned guest = 0;
	const auto recording = [&](auto&, auto)
	{
		++recording_calls;
		return false;
	};
	const auto boot = [&](auto&, auto)
	{
		++boot_calls;
		return false;
	};
	check(pbd::replay_entry::enter(pbd::replay_entry::context{guest, 0x159f0}, [](auto&, auto)
			  {
				  return false;
			  },
			  recording, boot) == refusal::direct_replay,
		"A refused playback entry must still stop the guest");
	check(recording_calls == 0 && boot_calls == 0, "Playback refusal prevents dependent hooks");
	check(pbd::replay_entry::enter(pbd::replay_entry::context{guest, 0x159f0}, [](auto&, auto)
			  {
				  return true;
			  },
			  recording, boot) == refusal::boot_request,
		"A refused native boot must still stop the guest even when recording refused");
	check(recording_calls == 1 && boot_calls == 1, "Recording refusal does not suppress boot validation");
}

PBD_TEST(PbdReplayEntry, SuccessfulHooksKeepOrder)
{
	std::vector<unsigned> order;
	unsigned guest = 0;
	const auto result = pbd::replay_entry::enter(pbd::replay_entry::context{guest, 0x159f0}, [&](auto&, auto)
		{
			order.push_back(1);
			return true;
		},
		[&](auto&, auto)
		{
			order.push_back(2);
			return true;
		},
		[&](auto&, auto)
		{
			order.push_back(3);
			return true;
		});
	check(result == refusal::none, "Successful entry stays enabled");
	check(order == std::vector<unsigned>{1, 2, 3}, "Guest entry keeps playback, recording, boot order");
}
