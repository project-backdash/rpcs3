#pragma once

// Private state and helpers shared by the scenarios in replay_viewer_test.cpp.
namespace
{
	using namespace pbd::replay_viewer;

	static action press(controller& c, word held, const context& cx)
	{
		c.poll(0, cx);
		return c.poll(held, cx);
	}
} // namespace
