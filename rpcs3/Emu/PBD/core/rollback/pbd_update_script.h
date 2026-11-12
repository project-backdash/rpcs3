#pragma once

// Finite diagnostic input indexed by the selected application-update invocation.
// An invocation is not established to be one combat simulation frame.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace pbd
{
	inline constexpr std::size_t update_script_max_bytes = 64 * 1024;

	// The current boundary may still await its first dispatch. Only an older absent entry proves an alignment hole.
	constexpr bool update_source_was_skipped(std::uint32_t source, std::uint32_t boundary, bool entry_seen) noexcept
	{
		return source < boundary && !entry_seen;
	}

	// Avoid STL exception inheritance across RPCS3's differing _HAS_EXCEPTIONS units.
	struct update_script_error
	{
		const char* message;
	};

	struct update_inputs
	{
		std::uint16_t p1 = 0;
		std::uint16_t p2 = 0;
	};

	namespace update_script_detail
	{
		inline std::size_t update_fields(std::string_view line, std::array<std::string_view, 3>& fields)
		{
			std::size_t count = 0;
			while (!line.empty())
			{
				const auto start = line.find_first_not_of(" \t");
				if (start == std::string_view::npos)
					break;
				line.remove_prefix(start);
				const auto end = line.find_first_of(" \t");
				if (count == fields.size())
					throw update_script_error{"Too many update tape fields"};
				fields[count++] = line.substr(0, end);
				if (end == std::string_view::npos)
					break;
				line.remove_prefix(end);
			}
			return count;
		}

		inline std::uint64_t update_decimal(std::string_view token)
		{
			if (token.empty())
				throw update_script_error{"Update tape requires unsigned decimal integers"};
			std::uint64_t value = 0;
			const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
			if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
				throw update_script_error{"Update tape requires unsigned decimal integers"};
			return value;
		}
	} // namespace update_script_detail

	inline std::vector<update_inputs> parse_update_script(std::istream& input)
	{
		std::string text;
		char byte = 0;
		while (input.get(byte))
		{
			if (text.size() == update_script_max_bytes)
				throw update_script_error{"Update tape exceeds 64 KiB"};
			const auto value = static_cast<unsigned char>(byte);
			if (value > 0x7e || (value < 0x20 && byte != '\t' && byte != '\r' && byte != '\n'))
				throw update_script_error{"Update tape must contain ASCII text"};
			text.push_back(byte);
		}
		if (input.bad() || (!input.eof() && input.fail()))
			throw update_script_error{"Cannot read update tape"};
		std::string_view remaining(text);
		auto next_line = [&]()
		{
			const auto end = remaining.find('\n');
			auto line = remaining.substr(0, end);
			remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			return line;
		};
		if (next_line() != "DR2026_UPDATE_TAPE 1")
			throw update_script_error{"Invalid update tape header"};
		std::array<std::string_view, 3> fields{};
		if (update_script_detail::update_fields(next_line(), fields) != 2 || fields[0] != "count")
			throw update_script_error{"Expected count as the second update tape line"};
		const auto count = update_script_detail::update_decimal(fields[1]);
		if (count < 8 || count > 1048576)
			throw update_script_error{"Update tape count must be 8..1048576"};
		std::vector<update_inputs> rows(static_cast<std::size_t>(count));
		bool found = false;
		std::size_t previous = 0;
		update_inputs current{};
		while (!remaining.empty())
		{
			auto line = next_line();
			line = line.substr(0, line.find('#'));
			const auto field_count = update_script_detail::update_fields(line, fields);
			if (!field_count)
				continue;
			if (field_count != 3)
				throw update_script_error{"Expected index p1 p2 in update tape"};
			const auto index = update_script_detail::update_decimal(fields[0]);
			const auto p1 = update_script_detail::update_decimal(fields[1]);
			const auto p2 = update_script_detail::update_decimal(fields[2]);
			if (index >= count)
				throw update_script_error{"Update tape index must be below count"};
			if (p1 > 65535 || p2 > 65535)
				throw update_script_error{"Update tape masks must fit uint16"};
			if ((!found && index != 0) || (found && index <= previous))
				throw update_script_error{"Update tape indices must begin at zero and increase strictly"};
			const auto next = static_cast<std::size_t>(index);
			std::fill(rows.begin() + previous, rows.begin() + next, current);
			previous = next;
			current = {static_cast<std::uint16_t>(p1), static_cast<std::uint16_t>(p2)};
			found = true;
		}
		if (!found)
			throw update_script_error{"Update tape contains no transitions"};
		if (current.p1 || current.p2)
			throw update_script_error{"Update tape must finish with both ports neutral"};
		std::fill(rows.begin() + previous, rows.end(), current);
		return rows;
	}
} // namespace pbd
