#include "pbd_test.h"
#include "Crypto/package_path.h"

PBD_TEST(PbdPackagePath, UnicodeAndContainment)
{
	using pbd::test::check;
	const std::string base = "parent/\xd8\xaa\xd8\xac\xd8\xb1\xd8\xa8\xd8\xa9\xe4\xb8\xad\xe6\x96\x87/game/";
	const auto directory = pkg_path::from_utf8(base).lexically_normal();
	const auto child = pkg_path::from_utf8(base + "USRDIR/EBOOT.BIN").lexically_normal();
	check(pkg_path::from_utf8(pkg_path::to_utf8(child)) == child, "Unicode path round trips");
	check(pkg_path::inside(directory, child), "ordinary entry remains inside");
	check(!pkg_path::inside(directory, pkg_path::from_utf8(base + "../other/file").lexically_normal()), "parent escape is rejected");
	check(!pkg_path::inside(directory, pkg_path::from_utf8(base + "../../games/file").lexically_normal()), "sibling prefix is rejected");
}
