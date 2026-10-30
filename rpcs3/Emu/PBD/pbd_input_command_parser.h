#pragma once

#include "pbd_input_command_model.h"
#include <algorithm>

namespace pbd
{
	class input_command_parser : public input_command_model
	{
		const std::string& m_run_id;
		u32 m_seq;
		command& cmd;
		std::string& error;
		bool have_run = false, have_seq = false;

		static std::vector<std::string> command_lines(const std::string& text)
		{
			std::vector<std::string> lines;
			for (usz start = 0; start <= text.size();)
			{
				usz end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				std::string line = text.substr(start, end - start);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (const auto hash = line.find('#'); hash != std::string::npos)
					line.resize(hash);
				while (!line.empty() && line.back() == ' ')
					line.pop_back();
				if (!line.empty())
					lines.push_back(line);
				start = end + 1;
			}
			return lines;
		}

		static bool number(const std::vector<std::string>& w, usz k, u64 max, u64& out)
		{
			if (k >= w.size())
				return false;
			char* end = nullptr;
			out = std::strtoull(w[k].c_str(), &end, 0);
			return end && !*end && out <= max;
		}

		bool acknowledgement(const std::vector<std::string>& w)
		{
			u64 a = 0;
			predicate p;
			const auto f = resolve_field(w[1]);
			if (!f)
			{
				error = "ack_field";
				return false;
			}
			p.field = *f;
			if (w[2] == "eq")
				p.op = cmp::eq;
			else if (w[2] == "ne")
				p.op = cmp::ne;
			else if (w[2] == "changed")
				p.op = cmp::changed;
			else if (w[2] == "lt")
				p.op = cmp::lt;
			else if (w[2] == "gt")
				p.op = cmp::gt;
			else
			{
				error = "ack_op";
				return false;
			}
			if (p.op != cmp::changed && !number(w, 3, 0xffffffff, a))
			{
				error = "ack_value";
				return false;
			}
			p.value = static_cast<u32>(a);
			cmd.ack.push_back(p);
			return true;
		}

		bool field(const std::vector<std::string>& w, usz i)
		{
			u64 a = 0, b = 0;
			if (w[0] == "run" && w.size() == 2)
			{
				if (w[1] != m_run_id)
				{
					error = "run";
					return false;
				}
				have_run = true;
			}
			else if (w[0] == "seq" && number(w, 1, 0xffffffff, a))
			{
				if (a != m_seq)
				{
					error = "seq";
					return false;
				}
				cmd.seq = static_cast<u32>(a);
				have_seq = true;
			}
			else if (w[0] == "op" && w.size() == 2 && (w[1] == "observe" || w[1] == "press" || w[1] == "wait" || w[1] == "checkpoint" || w[1] == "stop"))
				cmd.op = w[1];
			else if (w[0] == "buttons" && w.size() == 3 && number(w, 1, 0xffff, a) && number(w, 2, 0xffff, b))
			{
				cmd.digital1 = static_cast<u16>(a);
				cmd.digital2 = static_cast<u16>(b);
			}
			else if (w[0] == "hold_polls" && number(w, 1, 30, a) && a >= 1)
				cmd.hold_polls = static_cast<u32>(a);
			else if (w[0] == "release_polls" && number(w, 1, 120, a) && a >= 1)
				cmd.release_polls = static_cast<u32>(a);
			else if (w[0] == "stable_polls" && number(w, 1, 120, a) && a >= 1)
				cmd.stable_polls = static_cast<u32>(a);
			else if (w[0] == "timeout_polls" && number(w, 1, 36000, a) && a >= 1)
				cmd.timeout_polls = static_cast<u32>(a);
			else if (w[0] == "ack_mode" && w.size() == 2 && (w[1] == "any" || w[1] == "all"))
				cmd.ack_any = w[1] == "any";
			else if (w[0] == "ack" && (w.size() == 3 || w.size() == 4) && cmd.ack.size() < 16)
			{
				return acknowledgement(w);
			}
			else if (w[0] == "dump" && number(w, 1, 0xffffffff, a) && number(w, 2, 0x80000, b) && b >= 4 && cmd.dumps.size() < 8)
				cmd.dumps.push_back({static_cast<u32>(a), static_cast<u32>(b)});
			else
			{
				error = "line_" + std::to_string(i);
				return false;
			}
			return true;
		}

	public:
		input_command_parser(const std::string& run_id, u32 seq, command& output, std::string& reason)
			: m_run_id(run_id), m_seq(seq), cmd(output), error(reason)
		{
		}

		bool parse(const std::string& text)
		{
			const auto lines = command_lines(text);
			if (lines.empty() || lines.front() != "DR2026-LIVE-CMD 1")
			{
				error = "header";
				return false;
			}
			if (lines.back() != "end")
			{
				error = "missing_end";
				return false;
			}
			for (usz i = 1; i + 1 < lines.size(); i++)
			{
				std::vector<std::string> w;
				for (usz s = 0; s < lines[i].size();)
				{
					const usz e = std::min(lines[i].find(' ', s), lines[i].size());
					if (e > s)
						w.push_back(lines[i].substr(s, e - s));
					s = e + 1;
				}
				if (!field(w, i))
					return false;
			}
			if (!have_run || !have_seq || cmd.op.empty())
			{
				error = "incomplete";
				return false;
			}
			if (cmd.op == "press" && !cmd.digital1 && !cmd.digital2)
			{
				error = "press_without_buttons";
				return false;
			}
			if (cmd.op == "wait" && cmd.ack.empty())
			{
				error = "wait_without_ack";
				return false;
			}
			return true;
		}
	};
} // namespace pbd
