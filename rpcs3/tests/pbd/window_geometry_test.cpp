#include "pbd_test.h"

#include "rpcs3qt/window_geometry.h"

using gui::utils::centered_window_geometry;
using gui::utils::window_geometry;
using pbd::test::check;

namespace
{
	void expect_geometry(window_geometry actual, window_geometry expected)
	{
		check(actual.x == expected.x);
		check(actual.y == expected.y);
		check(actual.width == expected.width);
		check(actual.height == expected.height);
	}
} // namespace

PBD_TEST(PbdWindowGeometry, PreservesNormalCentering)
{
	expect_geometry(centered_window_geometry({0, 0, 1920, 1040}, {0, 0, 1920, 1080}, 1280, 720), {320, 180, 1280, 720});
	expect_geometry(centered_window_geometry({0, 0, 1920, 1040}, {400, 200, 1000, 700}, 800, 600), {500, 250, 800, 600});
}

PBD_TEST(PbdWindowGeometry, FitsOversizedTargetOnSmallLaptop)
{
	// A 1366x768 display with a 40-pixel taskbar must contain the whole client rectangle.
	expect_geometry(centered_window_geometry({0, 0, 1366, 728}, {0, 0, 1366, 768}, 1920, 1080), {36, 0, 1294, 728});
	// Width can be the limiting dimension instead of height.
	expect_geometry(centered_window_geometry({0, 0, 1280, 1000}, {0, 0, 1280, 1000}, 1920, 1080), {0, 140, 1280, 720});
}

PBD_TEST(PbdWindowGeometry, UsesScaledLogicalWorkArea)
{
	// The same small display at 125% scaling, already converted by QScreen to logical units.
	expect_geometry(centered_window_geometry({0, 0, 1093, 582}, {0, 0, 1093, 614}, 1920, 1080), {29, 0, 1034, 582});
	expect_geometry(centered_window_geometry({0, 30, 1280, 690}, {0, 0, 1280, 720}, 2560, 1440), {27, 30, 1226, 690});
}

PBD_TEST(PbdWindowGeometry, SupportsNegativeMonitorOrigins)
{
	expect_geometry(centered_window_geometry({-1366, -768, 1366, 728}, {-1366, -768, 1366, 768}, 1920, 1080), {-1330, -768, 1294, 728});
	expect_geometry(centered_window_geometry({-1920, 0, 1920, 1040}, {0, 0, 1920, 1080}, 1280, 720), {-1280, 180, 1280, 720});
}

PBD_TEST(PbdWindowGeometry, PreservesFittingSavedGeometry)
{
	const window_geometry saved{120, 80, 960, 540};
	expect_geometry(centered_window_geometry({0, 0, 1366, 728}, saved, saved.width, saved.height), saved);
}

PBD_TEST(PbdWindowGeometry, RecoversSavedGeometryFromRemovedMonitor)
{
	const window_geometry saved{-1920, -1080, 1920, 1080};
	expect_geometry(centered_window_geometry({0, 0, 1366, 728}, saved, saved.width, saved.height), {0, 0, 1294, 728});
	expect_geometry(centered_window_geometry({0, 0, 1366, 728}, {2000, 1500, 960, 540}, 960, 540), {406, 188, 960, 540});
}

PBD_TEST(PbdWindowGeometry, HandlesExactFitAndInvalidSizes)
{
	expect_geometry(centered_window_geometry({10, 20, 1366, 728}, {0, 0, 1920, 1080}, 1366, 728), {10, 20, 1366, 728});
	expect_geometry(centered_window_geometry({0, 0, 1366, 728}, {0, 0, 1366, 728}, 0, -1), {682, 363, 1, 1});
	expect_geometry(centered_window_geometry({0, 0, 0, 0}, {0, 0, 1366, 728}, 1920, 1080), {});
}
