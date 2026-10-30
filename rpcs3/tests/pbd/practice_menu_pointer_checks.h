#pragma once

// Private scenarios included inside practice_menu_test.cpp's anonymous namespace.

using pbd::test::check;
void pointer_identity_changes(pbd::practice::mouse::layout& view)
{
	using namespace pbd::practice::mouse;
	using pbd::practice::prompt_action;
	using pbd::practice::prompt_second_key;
	for (const auto dimensions : std::array<std::array<float, 2>, 4>{{{1280, 720}, {1920, 1080}, {1600, 900}, {2240, 1260}}})
	{
		publish(view);
		check(pointer(600 * dimensions[0] / 1280, 260 * dimensions[1] / 720, dimensions[0], dimensions[1], true), "scaled client click must hit the rendered row");
		const auto next = take(view.owner);
		check(next && next->kind == action::choose_row && next->value == 1, "DPI and window size must not change row identity");
	}
	check(pointer(600, 380, 1280, 720, true, 0, 120), "letterboxed window must hit the row inside its game viewport");
	const auto letterboxed = take(view.owner);
	check(letterboxed && letterboxed->value == 1 && !pointer(600, 10, 1280, 720, true, 0, 120), "black bars must not activate menu rows");
	check(pointer(760, 260, 1280, 720, true, 160, 0), "pillarboxed window must subtract the horizontal game margin");
	const auto pillarboxed = take(view.owner);
	check(pillarboxed && pillarboxed->value == 1 && !pointer(10, 260, 1280, 720, true, 160, 0), "horizontal black bars must not activate menu rows");
	check(pointer(520, 180, 1280, 720, false) && hovered_page(view.owner) == 1 && !take(view.owner), "tab hover highlights without changing page");
	check(!pointer(520, -1, 1280, 720, false) && hovered_page(view.owner) == -1, "leaving the game viewport must clear tab hover");
	check(pointer(520, 180, 1280, 720, true), "tab click must hit the rendered tab");
	const auto page = take(view.owner);
	check(page && page->kind == action::choose_page && page->value == 1, "tab click must carry its page identity");
	for (unsigned i = 0; i < 100; ++i)
		pointer(600, 260, 1280, 720, false);
	const auto hover = take(view.owner);
	check(hover && hover->kind == action::hover_row && !take(view.owner), "pointer motion must coalesce without flooding the input boundary");
	pointer(600, 260, 1280, 720, true);
	view.owner.context = 1;
	publish(view);
	check(!take(view.owner), "changing page must discard an old-page click before its row action");
	pointer(600, 260, 1280, 720, true);
	view.owner.generation = 2;
	publish(view);
	check(!take(view.owner), "reopening the menu must discard an old menu's click");
	pointer(600, 260, 1280, 720, true);
	++view.owner.emulation;
	publish(view);
	check(!take(view.owner), "a replacement emulation must reject old clicks even when it reuses menu numbers");
	pointer(600, 260, 1280, 720, true);
	++view.owner.revision;
	publish(view);
	check(!take(view.owner), "a new visit to the same decision must reject its earlier queued click");
}

pbd::practice::mouse::layout pointer_rail_layout()
{
	using namespace pbd::practice::mouse;
	using pbd::practice::prompt_action;
	using pbd::practice::prompt_second_key;
	// Two-column Practice menu: eight rail categories at top 160 + 30n (x 56..372), list rows at x 396..1208, up to
	// twelve rows.
	layout menu;
	menu.owner = {3, 1, 1, 1};
	menu.active = true;
	for (std::uint32_t i = 0; i < pbd::practice::categories::count; ++i)
		menu.pages[menu.page_count++] = {56, 160.f + 30 * i, 316, 28, i};
	for (std::uint32_t i = 0; i < 12; ++i)
		menu.rows[menu.row_count++] = {396, 160.f + 30 * i, 812, 28, i};
	publish(menu);
	check(pointer(800, 160 + 30 * 11 + 14, 1280, 720, true), "the twelfth list row must be clickable");
	const auto twelfth = take(menu.owner);
	check(twelfth && twelfth->kind == action::choose_row && twelfth->value == 11, "list rows keep their absolute values");
	check(pointer(120, 160 + 30 * 2 + 14, 1280, 720, true), "a rail item must be clickable");
	const auto rail = take(menu.owner);
	check(rail && rail->kind == action::choose_page && rail->value == 2, "a rail click switches to its page");
	check(pointer(120, 160 + 30 * 6 + 14, 1280, 720, true), "the seventh rail category (Settings) must be clickable");
	const auto settings_category = take(menu.owner);
	check(settings_category && settings_category->kind == action::choose_page && settings_category->value == pbd::practice::categories::settings,
		"the seventh rail click carries Settings' category");
	check(pointer(120, 160 + 30 * 7 + 14, 1280, 720, true), "the eighth rail category (Exit Practice) must be clickable");
	const auto last_category = take(menu.owner);
	check(last_category && last_category->kind == action::choose_page && last_category->value == pbd::practice::categories::exit_practice,
		"the last rail click carries Exit Practice's category");
	check(!pointer(380, 200, 1280, 720, true) && !take(menu.owner), "the column gap between rail and list must not act");

	return menu;
}

void pointer_control_bar(pbd::practice::mouse::layout& menu)
{
	using namespace pbd::practice::mouse;
	using pbd::practice::prompt_action;
	using pbd::practice::prompt_second_key;
	// Control bar: each clickable group is a target; a pair splits into one target per key. Hover marks the group without
	// queuing anything, and a click queues the group's action.
	using pbd::practice::prompt_action;
	using pbd::practice::prompt_second_key;
	menu.prompts[0] = {{52, 680, 90, 26, static_cast<std::uint32_t>(prompt_action::confirm)}, 0};
	menu.prompts[1] = {{168, 680, 70, 26, static_cast<std::uint32_t>(prompt_action::back)}, 1};
	menu.prompts[2] = {{264, 680, 30, 26, static_cast<std::uint32_t>(prompt_action::frequency)}, 2};
	menu.prompts[3] = {{294, 680, 90, 26, static_cast<std::uint32_t>(prompt_action::frequency) | prompt_second_key}, 2};
	menu.prompt_count = 4;
	publish(menu);
	check(pointer(100, 693, 1280, 720, false) && hovered_prompt(menu.owner) == 0 && !take(menu.owner),
		"hovering a control-bar group highlights it without acting");
	check(pointer(200, 693, 1280, 720, true) && hovered_prompt(menu.owner) == 1, "a control-bar group must be clickable");
	const auto back = take(menu.owner);
	check(back && back->kind == action::prompt && back->value == static_cast<std::uint32_t>(prompt_action::back),
		"a control-bar click carries its group's action");
	check(pointer(270, 693, 1280, 720, true) && pointer(320, 693, 1280, 720, true) && hovered_prompt(menu.owner) == 2,
		"both halves of a pair belong to one group");
	const auto lower = take(menu.owner), higher = take(menu.owner);
	check(lower && higher && lower->value == static_cast<std::uint32_t>(prompt_action::frequency) &&
			  higher->value == (static_cast<std::uint32_t>(prompt_action::frequency) | prompt_second_key),
		"the first key of a pair lowers and the second raises");
	check(!pointer(150, 693, 1280, 720, true) && hovered_prompt(menu.owner) == -1 && !take(menu.owner),
		"the gap between control-bar groups must not act");
	menu.prompt_count = 0;
}

void pointer_frequency_picker(pbd::practice::mouse::layout& menu)
{
	using namespace pbd::practice::mouse;
	using pbd::practice::prompt_action;
	using pbd::practice::prompt_second_key;
	// Frequency picker on the focused filled slot: L1 / left chevron step down, right chevron / R1 step up.
	menu.steppers[0] = {696, 160, 40, 28, 0};
	menu.steppers[1] = {768, 160, 40, 28, 1};
	menu.stepper_count = 2;
	publish(menu);
	check(pointer(700, 174, 1280, 720, true), "the L1 side of the picker must be clickable");
	const auto down = take(menu.owner);
	check(down && down->kind == action::frequency && down->value == 0, "the L1 side lowers the frequency");
	check(pointer(800, 174, 1280, 720, true), "the R1 side of the picker must be clickable");
	const auto up = take(menu.owner);
	check(up && up->kind == action::frequency && up->value == 1, "the R1 side raises the frequency");
	check(pointer(700, 174, 1280, 720, false), "hovering the picker stays on the row");
	check(!take(menu.owner), "hovering the picker must not change the frequency");
	check(pointer(900, 174, 1280, 720, true), "the rest of the slot row still opens its dropdown");
	const auto slot = take(menu.owner);
	check(slot && slot->kind == action::choose_row && slot->value == 0, "a row click outside the picker chooses the row");
}

void pointer_dropdown(pbd::practice::mouse::layout& menu)
{
	using namespace pbd::practice::mouse;
	using pbd::practice::prompt_action;
	using pbd::practice::prompt_second_key;
	// Open dropdown: only its options answer the pointer; a click anywhere else closes it without a change.
	layout open = menu;
	open.owner.context = 129;
	open.modal = true;
	open.stepper_count = 0;
	open.option_count = 2;
	open.options[0] = {849, 159, 409, 30, 0};
	open.options[1] = {849, 189, 409, 30, 1};
	open.prompts[0] = {{52, 680, 90, 26, static_cast<std::uint32_t>(prompt_action::back)}, 1};
	open.prompt_count = 1;
	publish(open);
	check(pointer(100, 693, 1280, 720, true), "the control bar must stay clickable while a dropdown is open");
	const auto cancel = take(open.owner);
	check(cancel && cancel->kind == action::prompt && cancel->value == static_cast<std::uint32_t>(prompt_action::back) && !take(open.owner),
		"a control-bar click over an open dropdown is its action, not a dismiss");
	for (unsigned i = 0; i < 10; ++i)
		pointer(900, 200, 1280, 720, false);
	const auto hover_option = take(open.owner);
	check(hover_option && hover_option->kind == action::hover_option && hover_option->value == 1 && !take(open.owner),
		"option hover must coalesce and carry the option index");
	check(pointer(900, 170, 1280, 720, true), "an option must be clickable");
	const auto pick = take(open.owner);
	check(pick && pick->kind == action::choose_option && pick->value == 0, "an option click picks that option");
	check(!pointer(120, 174, 1280, 720, false) && hovered_page(open.owner) == -1 && !take(open.owner),
		"rail and rows must not react to hover while a dropdown is open");
	check(pointer(120, 174, 1280, 720, true), "a click outside the open dropdown is consumed");
	const auto dismiss = take(open.owner);
	check(dismiss && dismiss->kind == action::dismiss && !take(open.owner), "a click outside the open dropdown only closes it");
	pointer(900, 170, 1280, 720, true);
	publish(menu);
	check(!take(menu.owner), "closing the dropdown discards an option click queued in its context");
}

void pointer_question(pbd::practice::mouse::layout& menu)
{
	using namespace pbd::practice::mouse;
	using pbd::practice::prompt_action;
	using pbd::practice::prompt_second_key;
	// A question stacked over the list (its own context): Yes and No are its options, the control bar still answers,
	// and a click anywhere else closes it unanswered.
	layout question = menu;
	question.owner.context = 256 + pbd::practice::categories::exit_practice;
	question.modal = true;
	question.stepper_count = 0;
	question.option_count = 2;
	question.options[0] = {381, 361, 518, 28, pbd::practice::answer_yes};
	question.options[1] = {381, 391, 518, 28, pbd::practice::answer_no};
	question.prompts[0] = {{52, 680, 90, 26, static_cast<std::uint32_t>(prompt_action::confirm)}, 0};
	question.prompt_count = 1;
	pointer(800, 160 + 30 * 11 + 14, 1280, 720, true);
	publish(question);
	check(!take(question.owner), "a row click queued before the question opened must not answer it");
	check(pointer(640, 400, 1280, 720, false), "hovering an answer must be seen");
	const auto hover_no = take(question.owner);
	check(hover_no && hover_no->kind == action::hover_option && hover_no->value == pbd::practice::answer_no, "hover carries the answer");
	check(pointer(640, 370, 1280, 720, true), "an answer must be clickable");
	const auto yes = take(question.owner);
	check(yes && yes->kind == action::choose_option && yes->value == pbd::practice::answer_yes, "a click on Yes chooses Yes");
	check(!pointer(120, 174, 1280, 720, false) && hovered_page(question.owner) == -1 && !take(question.owner),
		"the rail must not react while a question is open");
	check(pointer(800, 200, 1280, 720, true), "a click on the list behind a question is consumed");
	const auto outside = take(question.owner);
	check(outside && outside->kind == action::dismiss && !take(question.owner), "a click outside the question only closes it");
	check(pointer(100, 693, 1280, 720, true), "the control bar stays clickable over a question");
	const auto confirm = take(question.owner);
	check(confirm && confirm->kind == action::prompt && confirm->value == static_cast<std::uint32_t>(prompt_action::confirm),
		"a control-bar click over a question is its action");
	publish({});
}
