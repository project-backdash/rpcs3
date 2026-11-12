#include "util/types.hpp"
#include "util/endian.hpp"
#include "Utilities/File.h"
#include "../../Emu/savestate_utils.hpp"
#include "Emu/PBD/core/rollback/pbd_checkpoint_compatibility.h"
#include "pbd_test.h"

#include <span>
#include <vector>

using pbd::checkpoint_compatibility::captured_version;
using pbd::test::check;

namespace
{
	bool loads(std::span<const captured_version> versions)
	{
		std::vector<version_entry> data;
		for (const auto& v : versions)
			data.push_back(version_entry{v.type, v.version});
		return is_savestate_version_compatible(data, false);
	}
} // namespace

// Setup redoes its captures only when the key changes, so a build that refuses the captures its key promises would leave
// every updated install unable to start the game.
PBD_TEST(PbdCheckpointVersions, BuildLoadsTheVersionsItsKeyPromises)
{
	check(loads(pbd::checkpoint_compatibility::captured_versions));
}

// Captures under the first key (made by the 0.62 builds on the older RPCS3 base) recorded these versions; this build
// refuses them, so it must not accept that key.
PBD_TEST(PbdCheckpointVersions, OlderBaseCapturesNeedANewKey)
{
	const captured_version first_key[] = {{0, 22}, {7, 1}, {8, 1}, {10, 3}, {13, 1}};
	check(!loads(first_key));
	check(!pbd::checkpoint_compatibility::accepts(
		"PBD_CHECKPOINT_COMPAT_V1_0000000000000000000000000000000000000000000000000000000000000001"));
}
