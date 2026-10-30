#pragma once

// Diagnostic input indexed by successful cellPadGetData calls for each port.
// These counters are not game frames and do not establish deterministic replay.
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pbd
{
	// Do not derive from STL exception types: RPCS3 mixes MSVC _HAS_EXCEPTIONS
	// settings, which changes the standard exception base class across units.
	struct input_script_error
	{
		const char* message;
	};

	struct input_script_sample
	{
		std::uint8_t digital1 = 0;
		std::uint8_t digital2 = 0;
		std::array<std::uint8_t, 4> sticks{128, 128, 128, 128}; // RX, RY, LX, LY

		bool neutral() const
		{
			return !digital1 && !digital2 && sticks == std::array<std::uint8_t, 4>{128, 128, 128, 128};
		}
	};

	struct input_script_output
	{
		input_script_sample sample;
		std::uint64_t poll = 0;
		bool final = false;
		std::size_t event_index = 0;
		std::size_t completed_action = 0;
		std::size_t completed_actions_total = 0;
	};

	class pad_input_script
	{
		struct event
		{
			std::uint64_t poll;
			input_script_sample sample;
			// One-based only on the neutral transition completing a non-neutral input.
			std::size_t completed_action = 0;
		};
		std::array<std::vector<event>, 2> m_events;
		std::array<std::uint64_t, 2> m_polls{};
		std::array<std::size_t, 2> m_indices{};
		std::array<bool, 2> m_finished{};
		std::array<std::size_t, 2> m_completed_actions{};

	public:
		static constexpr std::size_t max_bytes = 64 * 1024;
		static constexpr std::size_t max_events = 4096;
		static constexpr std::uint64_t max_poll = 10'000'000;

		static pad_input_script parse(std::string_view text)
		{
			if (text.size() > max_bytes)
				throw input_script_error{"Input tape exceeds 64 KiB"};
			std::istringstream stream{std::string(text)};
			std::string line;
			if (!std::getline(stream, line))
				throw input_script_error{"Empty input tape"};
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line != "DR2026-PAD-TAPE 1")
				throw input_script_error{"Invalid input tape header"};
			pad_input_script input_sequence;
			std::size_t count = 0;
			while (std::getline(stream, line))
			{
				line.resize(line.find('#') == std::string::npos ? line.size() : line.find('#'));
				std::istringstream fields(line);
				std::vector<std::uint64_t> values;
				std::string field;
				while (fields >> field)
				{
					std::uint64_t value = 0;
					const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
					if (result.ec != std::errc{} || result.ptr != field.data() + field.size())
						throw input_script_error{"Input tape requires unsigned decimal integers"};
					values.push_back(value);
					if (values.size() > 8)
						throw input_script_error{"Too many input tape fields"};
				}
				if (values.empty())
					continue;
				if (values.size() != 8)
					throw input_script_error{"Expected port poll digital1 digital2 RX RY LX LY"};
				if (++count > max_events)
					throw input_script_error{"Too many input tape events"};
				if (values[0] > 1 || values[1] > max_poll)
					throw input_script_error{"Input tape port or poll out of range"};
				for (std::size_t i = 2; i < values.size(); ++i)
					if (values[i] > 255)
						throw input_script_error{"Input tape button/stick value exceeds 255"};
				auto& events = input_sequence.m_events[static_cast<std::size_t>(values[0])];
				if ((events.empty() && values[1] != 0) || (!events.empty() && values[1] <= events.back().poll))
					throw input_script_error{"Each port must start at poll 0 and increase strictly"};
				input_script_sample sample;
				sample.digital1 = static_cast<std::uint8_t>(values[2]);
				sample.digital2 = static_cast<std::uint8_t>(values[3]);
				for (std::size_t i = 0; i < 4; ++i)
					sample.sticks[i] = static_cast<std::uint8_t>(values[4 + i]);
				const bool completes_input = !events.empty() && !events.back().sample.neutral() && sample.neutral();
				const std::size_t completed_action = completes_input ? ++input_sequence.m_completed_actions[values[0]] : 0;
				events.push_back({values[1], sample, completed_action});
			}
			if (!count)
				throw input_script_error{"Input tape contains no events"};
			for (const auto& events : input_sequence.m_events)
				if (!events.empty() && !events.back().sample.neutral())
					throw input_script_error{"Each input tape port must end with a neutral event"};
			return input_sequence;
		}

		// Called under the existing pad mutex, only after the device checks succeed.
		std::optional<input_script_output> next(std::uint32_t port)
		{
			if (port >= m_events.size() || m_events[port].empty() || m_finished[port])
				return std::nullopt;
			const auto& events = m_events[port];
			const auto poll = m_polls[port]++;
			auto& index = m_indices[port];
			const auto previous_index = index;
			while (index + 1 < events.size() && events[index + 1].poll <= poll)
				++index;
			const bool final = poll == events.back().poll;
			m_finished[port] = final;
			// Report the prior completed action once, after its neutral wait and immediately before the next input transition.
			// The required trailing neutral event observes the last action without injecting another button.
			const auto completed_action = index != previous_index ? events[previous_index].completed_action : 0;
			return input_script_output{events[index].sample, poll, final, index, completed_action, m_completed_actions[port]};
		}

		// A default-constructed tape has no events.
		bool enabled() const
		{
			return !m_events[0].empty() || !m_events[1].empty();
		}

		// A peer's local tape cannot write the other peer's pad record.
		bool supplies_only_port(std::uint32_t port) const
		{
			return port < m_events.size() && !m_events[port].empty() && m_events[port ^ 1].empty();
		}

	};

	// Exact identity of one checkpoint file: normalized path, byte size and SHA-256 of contents.
	struct checkpoint_file
	{
		std::string path;
		std::uint64_t bytes = 0;
		std::array<std::uint8_t, 32> sha256{};

		bool operator==(const checkpoint_file&) const = default;
	};

} // namespace pbd
