#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

bool pbd::rollback_session::boundary_call::wait_exit_gate(u32 s, u32 b, u32 lag)
{
	if (!exit_gate_decided(s, b, lag))
	{
		const auto from = session_clock::now();
		const auto pause_credit = peer_pause_credit();
		auto last_send = from;
		bool held = false;
		const u32 announce = b > 0 ? b - 1 : 0;
		bool emu_stopping = false, left_waiting = false, lobby_ended = false;
		session_wait_modal waiting{session_wait_leave()};
		peer_send(announce);
		for (;;)
		{
			peer_poll(announce);
			if (lobby_wait_end())
			{
				lobby_ended = true;
				break;
			}
			if (data.peer.fault || exit_gate_decided(s, b, lag))
				break;
			if (pbd_emulation_stopping(ppu))
			{
				emu_stopping = true;
				break;
			}
			if (!held)
			{
				held = true;
				vm::temporary_unlock(ppu);
			}
			if (waiting.poll())
			{
				left_waiting = true;
				break;
			}
			if (session_clock::now() - last_send > std::chrono::milliseconds(10))
			{
				peer_send(announce);
				last_send = session_clock::now();
			}
			if (peer_wait_expired(from, 60, pause_credit))
			{
				if (!data.peer.fault)
					data.peer.fault = "session_exit_gate_timeout";
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		waiting.end();
		if (lobby_ended)
		{
			data.end_callback();
			return true;
		}
		if (emu_stopping || left_waiting)
		{
			if (emu_stopping)
			{
				pbd_session_log.warning("Emulation stopped at the exit gate of segment %u (boundary %u).", s, b);
				session_runtime.finish(emulation_stopped);
			}
			else
				leave_session_wait(b);
			data.end_callback();
			return true;
		}
		if (data.peer.fault)
		{
			pbd_session_log.error("Guard '%s' failed at the exit gate of segment %u (boundary %u).", data.peer.fault, s, b);
			session_runtime.finish(guard_failed);
			data.end_callback();
			return true;
		}
	}
	return false;
}

bool pbd::rollback_session::boundary_call::exit_gate_decided(u32 s, u32 b, u32 lag)
{
	const u32 ready = data.session.gate.peer_ready_seg[s % 8] == s ? data.session.gate.peer_ready[s % 8] : 0u;
	return ready != 0 || (data.session.gate.peer_through_seg[s % 8] == s ? data.session.gate.peer_through[s % 8] : 0u) + lag > b;
}
