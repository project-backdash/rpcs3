#include "pbd_test.h"

#include "Emu/Cell/pbd_corner_chat.h"
#include <string>

using pbd::test::check;

PBD_TEST(PbdCornerChat, Checks)
{
	using namespace pbd::corner_chat;
	state s;
	check(s.apply(1, true, {{"a", "Rival", "<b>plain</b>"}}, 100));
	check(s.load(100).lines[0].text == "Rival: <b>plain</b>");
	check(s.apply(1, true, {{"a", "Rival", "duplicate"}}, 200));
	check(s.load(200).count == 1);
	check(s.load(8100).lines[0].opacity == 1.f);
	check(s.load(9100).lines[0].opacity == .5f);
	check(s.load(10100).count == 0);
	check(s.apply(1, true, {{"b", "Rival", "visible"}}, 11000));
	const auto copied = s.load(11000);
	check(s.apply(1, false, {}, 11001));
	check(s.load(11001).count == 0);
	check(s.load(11001).revision != copied.revision);
	check(s.apply(1, true, {}, 11002));
	check(s.load(11002).count == 0);
	check(s.apply(1, true, {{"b", "Rival", "stale"}}, 11003));
	check(s.load(11003).count == 0);
	for (int i = 0; i < 8; ++i)
		check(s.apply(1, true, {{"id" + std::to_string(i), "", std::to_string(i)}}, 12000));
	check(s.load(12000).count == max_lines);
	check(s.load(12000).lines[0].text == "3");
	const auto before = s.load(12000);
	check(!s.apply(1, true, {{"good", "", "admissible"}, {"bad", "", "newline\n"}}, 12001));
	check(s.load(12001).count == before.count);
	check(!s.apply(2, true, {{"invalid", "", std::string("\xf0\x80\x80\x80", 4)}}, 12002));
	check(s.load(12002).generation == 1);
	check(!s.apply(2, true, std::vector<message>(21, {"a", "", "x"}), 12002));
	check(s.apply(2, true, {{"a", "", "new epoch"}}, 12003));
	check(s.load(12003).count == 1 && s.load(12003).lines[0].text == "new epoch");
	std::string truncated;
	const std::string emoji("\xf0\x9f\x8e\xae", 4);
	check(display_text(emoji + emoji, truncated, 1));
	check(truncated == emoji + "...");
	check(!display_text(std::string("\xed\xa0\x80", 3), truncated, 1));
	check(!display_text(std::string("\xf4\x90\x80\x80", 4), truncated, 1));
	check(!display_text(std::string("\xc2", 1), truncated, 1));
	s.clear();
	check(s.load(12003).count == 0 && s.load(12003).generation == 0);
	check(publish(10, true, {{"global", "", "old surface"}}));
	const auto old = load();
	check(compile_if_current(old, []
			  {
				  return 1;
			  }) == 1);
	check(publish(10, false, {}));
	check(compile_if_current(old, []
			  {
				  return 1;
			  }) == 0);
	check(publish(10, true, {}));
	check(compile_if_current(old, []
			  {
				  return 1;
			  }) == 0);
	clear();
}
