#pragma once

// Bounded, owner-thread-only evidence from the original live pass.
// A changed-input replay can take a new branch; this guard is conservative evidence,
// not a replacement for the stock oracle and confirmed generator hash.
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace pbd::fn_hook
{
	inline constexpr std::array<std::uint32_t, 14> universal_skip_entries{
		0x95d94, 0x9f8d0, 0xc24c8, 0x1118e0, 0x158cbc, 0x158d6c, 0x1ec6fc,
		0x1eed14, 0x26097c, 0x260b70, 0x260d44, 0x262ccc, 0x2649a8, 0x268778};

	class skip_guard_history
	{
	public:
		using key = std::array<std::uint32_t, 3>; // entry, caller LR, task/object pointer (r3)
		using tls_state = std::array<std::uint8_t, 137>;
		enum refusal : std::uint32_t
		{
			allowed,
			missing_tick,
			unfinished_tick,
			missing_call,
			ambiguous_call,
			unmatched_return,
			generator_draw,
			unreadable_state,
			changed_stack,
			extra_call,
			count
		};
		inline static constexpr std::array<const char*, count> names{"allowed", "missing_tick", "unfinished_tick", "missing_call",
			"ambiguous_call", "unmatched_return", "generator_draw", "unreadable_state", "changed_stack", "extra_call"};
		std::array<std::uint64_t, count> decisions{};
		std::map<std::uint32_t, std::uint64_t> skipped_entries;
		std::uint64_t live_calls = 0, overflows = 0, abandoned = 0;

	private:
		struct call
		{
			std::uint32_t calls = 0, closes = 0, stack = 0, drew = 0;
			bool readable = true, matched = true;
		};
		struct tick_record
		{
			std::uint32_t tag = ~0u;
			bool sealed = false;
			std::map<key, call> calls;
		};
		struct frame
		{
			key identity{};
			std::uint32_t tick = 0, stack = 0, drew = 0;
			bool readable = false;
			tls_state tls{};
		};
		std::array<tick_record, 256> history{};
		std::vector<frame> stack;
		std::set<key> replay_seen;
		std::uint32_t current = ~0u;
		bool initialized = false, replaying = false;

		void close(const frame& f, bool matched, bool readable, const tls_state& tls)
		{
			auto& t = history[f.tick % history.size()];
			if (t.tag != f.tick)
				return;
			auto& c = t.calls[f.identity];
			c.closes++;
			c.matched &= matched;
			c.readable &= f.readable && readable;
			c.drew |= f.drew;
			if (matched && f.readable && readable && f.tls != tls)
				c.drew |= 8;
		}

	public:
		void boundary(std::uint32_t tick, bool replay)
		{
			if (initialized && tick == current && replay == replaying)
				return;
			if (initialized && !replaying)
			{
				for (const auto& f : stack)
				{
					close(f, false, false, {});
					abandoned++;
				}
				stack.clear();
				auto& previous = history[current % history.size()];
				// A correction begins at a provisional live boundary. Only advancing to
				// the next LIVE boundary proves the preceding original frame finished.
				if (previous.tag == current && !replay && tick > current)
					previous.sealed = true;
			}
			initialized = true;
			current = tick;
			replaying = replay;
			replay_seen.clear();
			if (!replay)
			{
				auto& t = history[tick % history.size()];
				if (t.tag != tick)
				{
					t.calls.clear();
					t.tag = tick;
					t.sealed = false;
				}
				t.sealed = false;
			}
		}

		void observe(std::uint32_t pc, std::uint32_t lr, std::uint32_t node, std::uint32_t sp,
			bool candidate, std::uint32_t core, bool readable, const tls_state& tls, bool test_taint = false)
		{
			if (!initialized || replaying)
				return;
			while (!stack.empty())
			{
				const auto& top = stack.back();
				if (pc == top.identity[1] && sp == top.stack)
				{
					close(top, true, readable, tls);
					stack.pop_back();
					break;
				}
				if (sp >= top.stack)
				{
					close(top, false, readable, tls);
					stack.pop_back();
					continue;
				}
				break;
			}
			if (core)
				for (auto& f : stack)
					f.drew |= core;
			if (!candidate)
				return;
			auto& t = history[current % history.size()];
			if (t.calls.size() >= 2048 || stack.size() >= 64)
			{
				// A truncated history authorizes nothing in this tick, including earlier calls.
				t.sealed = false;
				t.tag = ~0u;
				overflows++;
				stack.clear();
				return;
			}
			if (t.tag != current)
				return; // overflow poisoned this tick
			key identity{pc, lr, node};
			auto& c = t.calls[identity];
			c.calls++;
			c.stack = sp;
			c.readable &= readable && node != 0 && lr != 0;
			live_calls++;
			frame f{identity, current, sp, test_taint ? 16u : 0u, readable, tls};
			stack.push_back(f);
		}

		refusal allow(std::uint32_t tick, const key& identity, std::uint32_t sp, bool test_missing = false)
		{
			auto reason = missing_tick;
			const auto& t = history[tick % history.size()];
			if (!test_missing && initialized && replaying && tick == current && t.tag == tick)
			{
				const auto it = t.calls.find(identity);
				if (!t.sealed)
					reason = unfinished_tick;
				else if (it == t.calls.end())
					reason = missing_call;
				else
				{
					const auto& c = it->second;
					if (c.calls != 1)
						reason = ambiguous_call;
					else if (!c.matched || c.closes != 1)
						reason = unmatched_return;
					else if (c.drew)
						reason = generator_draw;
					else if (!c.readable)
						reason = unreadable_state;
					else if (c.stack != sp)
						reason = changed_stack;
					else if (!replay_seen.insert(identity).second)
						reason = extra_call;
					else
						reason = allowed;
				}
			}
			decisions[reason]++;
			if (reason == allowed)
				skipped_entries[identity[0]]++;
			return reason;
		}
	};
} // namespace pbd::fn_hook
