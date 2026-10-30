#pragma once

// Checks for the PBD unit tests. Unlike <cassert>'s assert they also run in release builds, and a failed check fails
// the running test and ends it, since the checks after it usually build on what it covered.
//
// The PBD tests build with C++ exceptions (a failed check throws to end its test); the rest of rpcs3_test builds
// without them, so GoogleTest is told the same in every file and its inline code is the same everywhere.
#ifndef GTEST_HAS_EXCEPTIONS
#define GTEST_HAS_EXCEPTIONS 0
#endif

#include <gtest/gtest.h>

#include <exception>
#include <source_location>
#include <string>
#include <string_view>

namespace pbd::test
{
	// Thrown by a failed check to end its test; PBD_TEST catches it once the failure is recorded.
	struct stop
	{
	};

	[[noreturn]] inline void fail(std::string_view what, const char* file, int line)
	{
		ADD_FAILURE_AT(file, line) << what;
		throw stop{};
	}

	// What a check tests: anything that converts to bool, as assert takes it (a comparison, an optional, a pointer).
	struct condition
	{
		bool passed;

		template <typename Value>
		condition(const Value& value)
			: passed(static_cast<bool>(value))
		{
		}
	};

	// One check; `what` says what failed, and the caller's file and line are reported.
	inline void check(condition tested, std::string_view what, std::source_location where = std::source_location::current())
	{
		if (!tested.passed)
			fail(what, where.file_name(), static_cast<int>(where.line()));
	}

	// The same without a description, where the caller's line says what is checked.
	inline void check(condition tested, std::source_location where = std::source_location::current())
	{
		check(tested, "check failed", where);
	}

	// Runs one test body: a failed check has already been recorded and ends it; any other exception fails it.
	inline void run(void (*body)())
	{
		try
		{
			body();
		}
		catch (const stop&)
		{
		}
		catch (const std::exception& error)
		{
			ADD_FAILURE() << "unexpected exception: " << error.what();
		}
	}
} // namespace pbd::test

// A GoogleTest test whose body may end at a failed check from any function it calls.
#define PBD_TEST(suite, name)                     \
	void suite##_##name##_body();                 \
	TEST(suite, name)                             \
	{                                             \
		::pbd::test::run(&suite##_##name##_body); \
	}                                             \
	void suite##_##name##_body()
