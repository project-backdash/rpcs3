#pragma once

#include "../PBD/pbd_t5dr_memory.h"
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class ppu_thread;

// Sound preferences need the game's callbacks: changing saved bytes alone leaves the active mixer unchanged.
// Parsing/planning perform no writes. The opt-in runtime adapter applies the validated plan at a loaded owner boundary.
namespace pbd::sound_options
{
	inline constexpr std::string_view executable_sha256 = "b56e25e9210ed3c4fc19f3dfae686b7246aef386cdd4f88c80e0b89141ff887d";
	inline constexpr std::uint32_t setter_entry = 0x1e9e8;
	inline constexpr std::uint32_t setter_opd = 0x447d18;
	inline constexpr std::size_t max_file_bytes = 1024;

	struct settings
	{
		std::uint32_t speaker = 1;
		std::uint32_t bgm = 12;
		std::uint32_t se = 12;
		std::uint32_t stage = 12;
		std::uint32_t subtitle = 0;
	};

	inline bool valid(const settings& value) noexcept
	{
		return value.speaker <= 1 && value.bgm <= 12 && value.se <= 12 && value.stage <= 12 && value.subtitle <= 1;
	}

	struct parse_result
	{
		std::optional<settings> value;
		std::string_view error;
	};

	inline parse_result parse(std::string_view text) noexcept
	{
		if (text.size() > max_file_bytes)
			return {{}, "file_too_large"};
		const auto line = [&]()
		{
			const auto end = text.find('\n');
			auto result = text.substr(0, end);
			if (!result.empty() && result.back() == '\r')
				result.remove_suffix(1);
			text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			return result;
		};
		if (line() != "PBD-SOUND-1")
			return {{}, "bad_header"};
		settings out;
		std::array<bool, 5> seen{};
		constexpr std::array<std::string_view, 5> keys{"speaker", "bgm", "se", "stage", "subtitle"};
		std::array<std::uint32_t*, 5> values{&out.speaker, &out.bgm, &out.se, &out.stage, &out.subtitle};
		while (!text.empty())
		{
			const auto entry = line();
			const auto separator = entry.find('=');
			if (separator == std::string_view::npos)
				return {{}, "bad_line"};
			const auto key = entry.substr(0, separator), raw = entry.substr(separator + 1);
			std::size_t index = 0;
			for (; index < keys.size() && key != keys[index]; index++)
			{
			}
			if (index == keys.size())
				return {{}, "unknown_key"};
			if (seen[index])
				return {{}, "duplicate_key"};
			if (raw.empty())
				return {{}, "bad_value"};
			for (const char c : raw)
				if (c < '0' || c > '9')
					return {{}, "bad_value"};
			const auto [tail, error] = std::from_chars(raw.data(), raw.data() + raw.size(), *values[index]);
			if (error != std::errc{} || tail != raw.data() + raw.size())
				return {{}, "bad_value"};
			seen[index] = true;
		}
		for (bool present : seen)
			if (!present)
				return {{}, "missing_key"};
		if (!valid(out))
			return {{}, "out_of_range"};
		return {out, {}};
	}

	struct preference_binding
	{
		std::uint32_t saved_byte;
		std::uint32_t item;
		std::uint32_t vtable;
		std::uint32_t callback_opd;
		std::uint32_t callback_entry;
	};

	// Verified against NPUB30009's preference constructor, vtable initializer and callback OPDs.
	inline constexpr std::array<preference_binding, 5> bindings{{
		{0x1068496a, 0x106846e4, 0x10084448, 0x45d2f0, 0x26b44c},
		{0x1068496b, 0x106846f0, 0x1008446c, 0x45d2f8, 0x26b4bc},
		{0x1068496c, 0x106846fc, 0x10084490, 0x45d300, 0x26b534},
		{0x1068496d, 0x10684708, 0x100844b4, 0x45d308, 0x26b5ac},
		{0x1068496e, 0x10684714, 0x10084430, 0x45d368, 0x26b7f8},
	}};

	struct instruction_word
	{
		std::uint32_t address;
		std::uint32_t value;
	};

	inline constexpr std::array<instruction_word, 15> instructions{{
		{0x1ea14, 0x81230008},
		{0x1ea2c, 0x98890000},
		{0x1ea34, 0x8169000c},
		{0x26b478, 0x81230008},
		{0x26b484, 0x4be3ecfd},
		{0x26b4e8, 0x81230008},
		{0x26b4ec, 0x38600000},
		{0x26b4fc, 0x4be3e8f1},
		{0x26b560, 0x81230008},
		{0x26b564, 0x38600003},
		{0x26b574, 0x4be3e879},
		{0x26b5d8, 0x81230008},
		{0x26b5dc, 0x38600005},
		{0x26b5ec, 0x4be3e801},
		{0x26b7f8, 0x4e800020},
	}};

	struct runtime_context
	{
		std::string_view title_id;
		std::string_view loaded_executable_sha256;
		std::uint64_t emulation_generation = 0;
		std::uint64_t checkpoint_loaded_generation = 0;
		std::uint32_t current_ppu_id = 0;
		std::uint32_t owner_ppu_id = 0;
		bool at_input_boundary = false;
		std::uint32_t scene = 0, selector = 0;
	};

	struct setter_request
	{
		std::uint32_t entry;
		std::uint32_t toc;
		std::uint32_t item_argument;
		std::uint32_t value_argument;
		std::uint32_t expected_callback;
	};

	struct application_plan
	{
		std::uint64_t emulation_generation;
		std::uint32_t owner_ppu_id;
		std::array<setter_request, 5> setters;
	};

	struct plan_result
	{
		std::optional<application_plan> value;
		std::string_view error;
	};

	// read_word returns an optional host-order big-endian guest word and must refuse unmapped/unreadable memory.
	// The executor must recheck generation and owner identity, preserve all live CPU state and guard callback reentry.
	template <typename ReadWord>
	plan_result make_plan(const settings& value, const runtime_context& context, ReadWord&& read_word)
	{
		if (!valid(value))
			return {{}, "out_of_range"};
		if (context.title_id != pbd::t5dr::title_id || context.loaded_executable_sha256 != executable_sha256)
			return {{}, "unsupported_game"};
		if (!context.emulation_generation || context.checkpoint_loaded_generation != context.emulation_generation)
			return {{}, "checkpoint_not_loaded"};
		if (!context.at_input_boundary || !context.owner_ppu_id || context.current_ppu_id != context.owner_ppu_id)
			return {{}, "not_owner_boundary"};
		if (context.scene != 3 || context.selector != 44)
			return {{}, "clean_select_scene_required"};
		const auto matches = [&](std::uint32_t address, std::uint32_t expected)
		{
			const auto actual = read_word(address);
			return actual && *actual == expected;
		};
		if (!matches(setter_opd, setter_entry) || !matches(setter_opd + 4, pbd::t5dr::toc))
			return {{}, "setter_signature_mismatch"};
		for (const auto& instruction : instructions)
			if (!matches(instruction.address, instruction.value))
				return {{}, "callback_signature_mismatch"};
		for (const auto& item : bindings)
		{
			if (!matches(item.item, item.vtable) || !matches(item.item + 8, item.saved_byte) ||
				!matches(item.vtable + 12, item.callback_opd) || !matches(item.callback_opd, item.callback_entry) ||
				!matches(item.callback_opd + 4, pbd::t5dr::toc))
				return {{}, "preference_layout_mismatch"};
		}
		application_plan plan{context.emulation_generation, context.owner_ppu_id, {}};
		const std::array<std::uint32_t, 5> values{value.speaker, value.bgm, value.se, value.stage, value.subtitle};
		for (std::size_t index = 0; index < bindings.size(); index++)
			plan.setters[index] = {setter_entry, pbd::t5dr::toc, bindings[index].item, values[index], bindings[index].callback_entry};
		return {plan, {}};
	}

	struct code_span
	{
		std::uint32_t address, bytes;
		std::string_view sha256;
	};

	// The callback bodies have no syscalls, indirect branches, reservation instructions or recursive calls.
	// The setter's one virtual dispatch is admitted separately through the five fixed preference vtables.
	inline constexpr std::array<code_span, 25> callback_code{{
		{0x1e9e8, 0x98, "8691e302b130ecfde8375e9f3d1679715c013a7f46cc414c23b8d1cc1afac108"},
		{0x26b7f8, 0x4, "f332ea5b5437103cbb6f1508679da89eec9288ad775c96c439a17fccabe3de8e"},
		{0x26b5ac, 0x78, "dbbfa957e718dae7a4a0e7a4d8b0b58434d7769a46b52de73211470f4525414a"},
		{0xa9dec, 0x130, "83dcd47100c63fb790f9125bc7ca300eb848d3ffde836593563dcdb3d8e701e9"},
		{0xa9d60, 0x8c, "e12cff1f4d7d3d12e5ebec228727d2adf4cf1c2f3bfb5090342010fc1bfe4465"},
		{0xa9ca0, 0xc0, "8670f60342a51d9cc6ad5f57e82a548d38cf1b8c2f2951d32770b583ddbb4d48"},
		{0x3270e0, 0x2c, "0e2821631fc187ceb88407fbb3db835b7bafaa085edc676ea27e265147054f7a"},
		{0x329660, 0x5c, "24eb59aeb99b1b5a8f5c5271c9b2104c6a062ee95cedbee497511dc511ac3014"},
		{0xa9a7c, 0x64, "5b42afa275899a4e54d8d7011b5fb75f3d6c2e225965290f91f1a51610d876dc"},
		{0xa99b8, 0x14, "375e7dd912b40ce3593078a7dda0bfa8686ee10163e54163c16c9794d96ffbf6"},
		{0x331e20, 0xc4, "8a9bcd33471d832a177c3b4f3b32fdfceeff0aa6bee7f2de44a8f7ae72a18cfd"},
		{0x331c74, 0x4c, "6c4e2c527466f9b013a8f4b9e8f9838a244e315a954bf03430250615b18eb62a"},
		{0x26b534, 0x78, "8a87b9a2e17c3ca656e81a22fc6941555323b84860c6a3f1a0f6041b5fdc9ca1"},
		{0x26b4bc, 0x78, "ceed370093f6b5b4ae1b31e6e618c29d3213d18094b6db992043fa82f0eb7ece"},
		{0x26b44c, 0x70, "37341ba9d5a39ab2b0dda0e49b2c05bcd456932d9bde9796409c2058774b7021"},
		{0xaa180, 0x54, "9724d80b473822ba49a28b10d2b49817c0a9c886e028e1fefe1b865d34d0c0fc"},
		{0xa99cc, 0xb0, "d968901aaedd27c640f22449125666b5f5b30570765d1938530b3738d9db9919"},
		{0x326a70, 0x30, "4f5ab19facf797f553cb4cc5af521c26fc657584f47224caa9cbd4d49a6a0466"},
		{0x32bdb4, 0x74, "5ba041a6082509abe67aefc7ae950a08b6426962e7ed7955bb3263a6953794c1"},
		{0x338f34, 0x20, "d80828d32fbc8f9643db267975f249e6fcb605e1f4277f332594bf1f2494ff5d"},
		{0x32ee6c, 0x2c, "06423aaeda1b0416f5c651542c05650410b63ad39a6568c7b148e015ce132ea8"},
		{0x33113c, 0x48, "95e3e1bd40595efda6866746ac985d4a59ebefdf4a7ec01adb3e7d6f0a98d1a4"},
		{0x330c70, 0xc0, "db122421b526b92acd323e53c74000899f0d77c2a6c423187fe4ca001b2f511f"},
		{0x339fcc, 0xa8, "ec0d4f792d23a0d58ebccba106bf4212d422dd4020c2004f04ccb32ba4f451b5"},
		{0x3309cc, 0xf4, "f64bb687b43b3c4ddfecf022600893fbdd05fc7c9ba78d41f55b17594c6e1e38"},
	}};

	enum class application_status
	{
		disabled,
		waiting,
		applied,
		refused
	};
	struct application_result
	{
		application_status status = application_status::disabled;
		std::string_view reason;
		bool context_preserved = false;
		std::uint32_t setters_completed = 0;
	};

	// Generation-local and owner-thread only. Configure parses launch values without touching guest memory.
	class runtime_application
	{
		bool requested = false, attempted = false, executing = false;
		std::optional<settings> desired;
		std::string checkpoint_sha256;
		std::string_view setup_error;
		std::uint64_t generation = 0;

	public:
		void configure_environment(std::uint64_t emulation_generation) noexcept;
		bool in_guest_call() const noexcept
		{
			return executing;
		}
		bool pending() const noexcept
		{
			return requested && !attempted;
		}
		application_result apply_at_owner_boundary(ppu_thread& ppu, std::uint32_t pc, bool first_boundary) noexcept;
	};
} // namespace pbd::sound_options
