#include "pbd_test.h"

#include "Emu/PBD/pbd_json_reader.h"
#include "Emu/PBD/pbd_presentation_rows.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

using pbd::test::check;

namespace
{
	using namespace pbd::presentation;
	using namespace pbd::json::detail;

	// VkPresentModeKHR values, spelled out so the test needs no Vulkan header.
	constexpr std::int32_t immediate = 0, mailbox = 1, fifo = 2;

	pbd::json::json_value row_of(const std::optional<std::string>& row)
	{
		check(row.has_value(), "a change is one row");
		check(row->ends_with('\n') && row->find('\n') == row->size() - 1, "each JSON document occupies one complete line");
		auto decoded = json_parser(*row).parse();
		check(decoded.value);
		return std::move(*decoded.value);
	}
} // namespace

PBD_TEST(PbdPresentationRows, RowsCarryTheRunSummaryFields)
{
	for (const auto path : {present_path{true, false, immediate}, present_path{false, true, fifo}})
	{
		const auto row = row_of(present_row(path));
		bool flag = false;
		std::uint32_t mode = 0;
		check(is(row.find("type"), "present"));
		check(boolean(row.find("hosted"), flag) && flag == path.hosted);
		check(boolean(row.find("flip_model"), flag) && flag == path.flip_model);
		check(number32(row.find("present_mode"), 0, 3, mode) && mode == static_cast<std::uint32_t>(path.present_mode));
	}
	for (const bool raw : {true, false})
	{
		const auto row = row_of(keyboard_row(raw));
		bool flag = false;
		check(is(row.find("type"), "keyboard") && boolean(row.find("raw"), flag) && flag == raw);
	}
	check(file_name == "game-presentation.jsonl", "the launcher reads this name in the run folder");
}

PBD_TEST(PbdPresentationRows, APresentRowWaitsForTheFirstSwapChain)
{
	rows state;
	check(!state.hosted(true), "where the game is drawn is not a row before a swap chain exists");
	const auto row = row_of(state.swapchain(false, immediate));
	bool flag = false;
	std::uint32_t mode = 0;
	check(is(row.find("type"), "present") && boolean(row.find("hosted"), flag) && flag &&
			  boolean(row.find("flip_model"), flag) && !flag && number32(row.find("present_mode"), 0, 3, mode) && mode == 0,
		"the first swap chain writes the path with the remembered host");
}

PBD_TEST(PbdPresentationRows, OnlyAChangeOfPathIsANewRow)
{
	rows state;
	state.hosted(true);
	state.swapchain(false, immediate);
	check(!state.swapchain(false, immediate), "a swap chain rebuilt for a resize in the same mode is no row");
	check(!state.hosted(true), "the same host again is no row");
	std::uint32_t mode = 0;
	bool flag = false;
	check(number32(row_of(state.swapchain(false, mailbox)).find("present_mode"), 0, 3, mode) && mode == 1, "a new mode is a row");
	const auto row = row_of(state.hosted(false));
	check(is(row.find("type"), "present") && boolean(row.find("hosted"), flag) && !flag &&
			  boolean(row.find("flip_model"), flag) && !flag && number32(row.find("present_mode"), 0, 3, mode) && mode == 1,
		"the fallback to the game's own window keeps the mode");
	check(!state.hosted(false), "a second fallback report is no row");
	check(boolean(row_of(state.swapchain(true, fifo)).find("flip_model"), flag) && flag, "a flip-model path is a row");
}

PBD_TEST(PbdPresentationRows, AStandaloneGameReportsItsWindowOnce)
{
	rows state;
	check(!state.hosted(false), "the game's own window before any swap chain");
	const auto row = row_of(state.swapchain(false, fifo));
	bool flag = false;
	std::uint32_t mode = 0;
	check(is(row.find("type"), "present") && boolean(row.find("hosted"), flag) && !flag &&
		  boolean(row.find("flip_model"), flag) && !flag && number32(row.find("present_mode"), 0, 3, mode) && mode == 2);
}

PBD_TEST(PbdPresentationRows, TheKeyboardPathIsARowWhenItChanges)
{
	rows state;
	bool flag = false;
	auto row = row_of(state.keyboard(true));
	check(is(row.find("type"), "keyboard") && boolean(row.find("raw"), flag) && flag, "the first report is a row");
	check(!state.keyboard(true), "the same path again is no row");
	row = row_of(state.keyboard(false));
	check(is(row.find("type"), "keyboard") && boolean(row.find("raw"), flag) && !flag, "the fall back to window keys is a row");
	check(!state.keyboard(false));
	check(state.swapchain(false, immediate).has_value(), "keyboard rows do not stand in for the present row");
}

PBD_TEST(PbdPresentationRows, TheRunFolderNeedsBothPathsAbsolute)
{
	const auto output = std::filesystem::absolute("run");
	const auto pages = std::filesystem::absolute("pages.bin");
	check(run_folder(pages, output) == output, "the session's run folder");
	check(run_folder(pages, "run").empty(), "a relative run folder is refused by the session, so there is none");
	check(run_folder("pages.bin", output).empty(), "a relative page set is refused by the session as well");
	check(run_folder({}, output).empty(), "without a page set the session writes no run files");
	check(run_folder(pages, {}).empty(), "without a run folder nothing is written");
}
