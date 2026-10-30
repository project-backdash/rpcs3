#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "pbd_json_reader.h"
#include "pbd_session_health.h"
#include "../Cell/pbd_peer_endpoint.h"

#include <mutex>
#include <functional>

namespace pbd::peer_recovery
{
	using endpoint = pbd::peer_endpoint::endpoint;
	inline std::optional<endpoint> resend_target(std::optional<endpoint> candidate, std::optional<endpoint> previous, bool& turn) noexcept
	{
		// At 100Hz a hold must emit at most two authentication sequences per poll. Three routes would age a
		// 250ms candidate out of the 64-packet replay window while the active route is faster.
		if (candidate && previous)
		{
			turn = !turn;
			return turn ? candidate : previous;
		}
		return candidate ? candidate : previous;
	}
	inline bool refresh_at_handshake(bool handshake, bool in_draw, bool recovering, bool modal) noexcept
	{
		return handshake && !in_draw && recovering && modal;
	}
	struct context
	{
		std::string session_id, set_id;
		std::uint32_t peer_session = 0;
		std::uint64_t epoch = 0;
		bool operator==(const context&) const = default;
	};
	enum class operation
	{
		stage,
		commit,
		cancel
	};
	enum class wait_result
	{
		pending,
		deadline,
		recovery_exhausted
	};
	struct command
	{
		context identity;
		std::uint64_t serial = 0;
		operation action = operation::stage;
		std::uint64_t proposal = 0;
		std::string address;
		std::uint16_t port = 0;
	};
	inline pbd::json::result<command> parse(std::string_view text, std::uint64_t expected_serial)
	{
		using namespace pbd::json;
		const auto parsed = detail::json_parser(text, 64, 4096, 256).parse();
		if (!parsed.value || parsed.value->kind != json_kind::object)
			return {{}, "invalid_document"};
		const auto& value = *parsed.value;
		for (const auto& [name, unused] : value.fields)
			if (name != "schema" && name != "sessionId" && name != "setId" && name != "peerSession" && name != "keyEpoch" &&
				name != "serial" && name != "operation" && name != "proposalSerial" && name != "address" && name != "port")
				return {{}, "unknown_field"};
		command out;
		std::uint64_t peer = 0, port = 0;
		if (!detail::is(value.find("schema"), "pbd-network-path-1") || !detail::identifier(value.find("sessionId")) ||
			!detail::identifier(value.find("setId")) || !detail::number(value.find("peerSession"), 1, 0xffffffffu, peer) ||
			!detail::number(value.find("keyEpoch"), 1, max_safe_integer, out.identity.epoch) ||
			!detail::number(value.find("serial"), expected_serial, expected_serial, out.serial) ||
			!detail::number(value.find("proposalSerial"), 1, max_safe_integer, out.proposal))
			return {{}, "invalid_envelope"};
		out.identity.session_id = value.find("sessionId")->text;
		out.identity.set_id = value.find("setId")->text;
		out.identity.peer_session = static_cast<std::uint32_t>(peer);
		if (detail::is(value.find("operation"), "stage"))
		{
			if (out.proposal != out.serial || !detail::plain_text(value.find("address"), 1, 64) ||
				!detail::number(value.find("port"), 1, 65535, port))
				return {{}, "invalid_candidate"};
			out.address = value.find("address")->text;
			out.port = static_cast<std::uint16_t>(port);
		}
		else
		{
			if (out.proposal >= out.serial || value.find("address") || value.find("port"))
				return {{}, "invalid_proposal"};
			if (detail::is(value.find("operation"), "commit"))
				out.action = operation::commit;
			else if (detail::is(value.find("operation"), "cancel"))
				out.action = operation::cancel;
			else
				return {{}, "invalid_operation"};
		}
		return {std::move(out), {}};
	}
	struct status
	{
		context identity;
		std::optional<pbd::session_health::progress> progress;
		endpoint active{}, candidate{}, previous{};
		std::uint64_t previous_until_us = 0;
		std::uint64_t sequence = 0, request_id = 0, command_serial = 0, proposal = 0, verified_proposal = 0, active_proposal = 0;
		std::uint64_t started_us = 0, last_valid_us = 0, recovery_started_us = 0, paused_us = 0;
		std::uint64_t candidate_started_us = 0, candidate_proof_us = 0;
		// Under the current key: this side's first sealed send (sent) and the peer's first valid input (heard), and whether
		// the peer's inputs carry an echo of a send time (echoed) and of one of this side's sends under the key (answered).
		std::uint64_t first_sent_us = 0, heard_us = 0;
		std::uint16_t local_port = 0;
		bool enabled = false, recovering = false, sent = false, heard = false, echoed = false, answered = false;
		const char* command_result = "none";
		const char* reason = "";
	};
	// Only current-key input proves a candidate; staging leaves the active route and game state unchanged.
	class state
	{
		mutable std::mutex mutex;
		status value;
		// Both under the lock.
		bool unanswered() const
		{
			return value.sent && value.echoed && !value.answered;
		}
		bool path_fault(std::uint64_t now) const
		{
			// An unheard peer may still be loading. After contact, 500 ms without input or with echoes acknowledging no
			// current-key send indicates a failed direction.
			const auto quiet = [now](std::uint64_t since)
			{
				return now >= since && now - since >= 500000;
			};
			return value.heard && (quiet(value.last_valid_us) || (unanswered() && quiet(std::max(value.first_sent_us, value.heard_us))));
		}
		static bool unicast(const endpoint& target)
		{
			if (!target.port)
				return false;
			if (target.kind == pbd::peer_endpoint::family::v4)
				return target.address[0] != 0 && target.address[0] < 224;
			if (target.kind != pbd::peer_endpoint::family::v6 || target.address[0] == 0xff)
				return false;
			return std::any_of(target.address.begin(), target.address.end(), [](auto byte)
				{
					return byte != 0;
				});
		}

	public:
		struct traffic_status
		{
			bool enabled, recovering;
			std::uint64_t epoch, sequence, command_serial;
		};
		traffic_status traffic() const
		{
			std::lock_guard lock(mutex);
			return {value.enabled, value.recovering, value.identity.epoch, value.sequence, value.command_serial};
		}
		bool matches(std::string_view session, std::string_view set, std::uint32_t peer, std::uint64_t epoch) const
		{
			std::lock_guard lock(mutex);
			return value.identity.session_id == session && value.identity.set_id == set && value.identity.peer_session == peer && value.identity.epoch == epoch;
		}
		void learned(const endpoint& target)
		{
			std::lock_guard lock(mutex);
			value.active = target;
		}
		void configure(context identity, endpoint active, std::uint64_t now, std::uint16_t local_port = 0)
		{
			if (identity.session_id.size() > 64 || identity.set_id.size() > 64)
				identity = {};
			std::lock_guard lock(mutex);
			if (identity == value.identity)
			{
				value.active = active;
				value.local_port = local_port;
				return;
			}
			const auto serial = value.command_serial, sequence = value.sequence + 1;
			value = {};
			value.identity = std::move(identity);
			value.active = active;
			value.command_serial = serial;
			value.sequence = sequence;
			value.started_us = now;
			value.local_port = local_port;
			value.enabled = value.identity.epoch && value.identity.peer_session && !value.identity.session_id.empty() && !value.identity.set_id.empty();
		}
		status get() const
		{
			// Allocate outside the lock shared with packet reception and sending.
			status copy;
			copy.identity.session_id.reserve(64);
			copy.identity.set_id.reserve(64);
			std::lock_guard lock(mutex);
			copy = value;
			return copy;
		}
		void observe_progress(std::string_view session, std::string_view set, std::uint32_t peer, std::uint64_t epoch,
			std::optional<pbd::session_health::progress> progress)
		{
			std::lock_guard lock(mutex);
			if (!value.enabled || value.identity.session_id != session || value.identity.set_id != set ||
				value.identity.peer_session != peer || value.identity.epoch != epoch)
				return;
			if (progress && !pbd::session_health::valid(*progress))
				progress.reset();
			if (value.progress != progress)
			{
				value.progress = progress;
				++value.sequence;
			}
		}
		// echo_us: the peer's echo of the newest send time it has received from this side (pbd::peer_message::input_echo_us),
		// when known.
		void received(std::uint64_t epoch, std::uint64_t now, std::optional<std::uint64_t> echo_us = {})
		{
			std::lock_guard lock(mutex);
			if (!value.enabled || value.identity.epoch != epoch)
				return;
			if (!value.heard)
			{
				value.heard = true;
				value.heard_us = now;
				++value.sequence;
			}
			value.last_valid_us = now;
			if (echo_us)
			{
				// Send times only grow, so an echo at or after the first send under this key is one of its sends. Once the peer
				// has received one, a reordered older echo does not undo it.
				value.echoed = true;
				value.answered = value.answered || (value.sent && *echo_us >= value.first_sent_us);
			}
			if (value.recovering && !unanswered())
			{
				value.paused_us += now - value.recovery_started_us;
				value.recovering = false;
				value.recovery_started_us = 0;
				++value.sequence;
			}
		}
		void sent(std::uint64_t epoch, std::uint64_t now)
		{
			std::lock_guard lock(mutex);
			if (!value.enabled || value.identity.epoch != epoch || value.sent)
				return;
			// Date local admission and echo checks without treating the unheard peer's loading time as loss.
			value.sent = true;
			value.first_sent_us = now;
			value.started_us = now;
			++value.sequence;
		}
		void poll(std::uint64_t now, bool idle = false)
		{
			std::lock_guard lock(mutex);
			if (!value.enabled)
				return;
			if (value.verified_proposal && now >= value.candidate_proof_us && now - value.candidate_proof_us > 1000000)
			{
				value.verified_proposal = 0;
				++value.sequence;
			}
			if (idle)
			{
				if (value.recovering)
				{
					value.paused_us += now - value.recovery_started_us;
					value.recovering = false;
					++value.sequence;
				}
				return;
			}
			if (!value.recovering && path_fault(now))
			{
				value.recovering = true;
				value.recovery_started_us = now;
				++value.request_id;
				++value.sequence;
			}
		}
		std::uint64_t paused(std::uint64_t now) const
		{
			std::lock_guard lock(mutex);
			return value.paused_us + (value.recovering && now >= value.recovery_started_us ? now - value.recovery_started_us : 0);
		}
		std::uint64_t wait_elapsed(std::uint64_t start, std::uint64_t now, std::uint64_t initial_credit) const
		{
			const auto total = paused(now);
			const auto credit = total >= initial_credit ? total - initial_credit : 0;
			return now >= start && now - start >= credit ? now - start - credit : 0;
		}
		bool expired(std::uint64_t now) const
		{
			std::lock_guard lock(mutex);
			return value.recovering && now >= value.recovery_started_us && now - value.recovery_started_us >= 90000000;
		}
		wait_result timeout(std::uint64_t start, std::uint64_t now, std::uint64_t budget, std::uint64_t initial_credit) const
		{
			if (expired(now))
				return wait_result::recovery_exhausted;
			// A heard peer gives a blocked wait one 90 s lease from the later of wait entry and first contact.
			// Frozen keepalives cannot renew it; an unheard peer remains under the caller's budget.
			if (const auto heard = heard_at())
			{
				const auto from = std::max(start, *heard);
				return now >= from && now - from >= 90000000 ? wait_result::recovery_exhausted : wait_result::pending;
			}
			return wait_elapsed(start, now, initial_credit) > budget ? wait_result::deadline : wait_result::pending;
		}
		// When the peer's first valid input under the current key arrived on a protected link; nothing before that.
		std::optional<std::uint64_t> heard_at() const
		{
			std::lock_guard lock(mutex);
			if (!value.enabled || !value.heard)
				return {};
			return value.heard_us;
		}
		bool allows(const endpoint& source, std::uint64_t epoch, std::uint64_t now) const
		{
			std::lock_guard lock(mutex);
			return value.enabled && value.identity.epoch == epoch &&
			       ((value.proposal && source == value.candidate) ||
					   (value.previous.kind != pbd::peer_endpoint::family::none && now <= value.previous_until_us && source == value.previous));
		}
		std::optional<endpoint> previous(std::uint64_t epoch, std::uint64_t now) const
		{
			std::lock_guard lock(mutex);
			if (value.enabled && value.identity.epoch == epoch && value.previous.kind != pbd::peer_endpoint::family::none &&
				now <= value.previous_until_us && value.previous != value.active && (!value.proposal || value.previous != value.candidate))
				return value.previous;
			return {};
		}
		std::optional<endpoint> probe(std::uint64_t epoch) const
		{
			std::lock_guard lock(mutex);
			if (value.enabled && value.identity.epoch == epoch && value.proposal && value.candidate != value.active)
				return value.candidate;
			return {};
		}
		bool prove(const endpoint& source, std::uint64_t epoch, std::uint64_t now = 0)
		{
			std::lock_guard lock(mutex);
			if (!value.enabled || value.identity.epoch != epoch || !value.proposal || source != value.candidate || now < value.candidate_started_us)
				return false;
			value.candidate_proof_us = now;
			if (value.verified_proposal != value.proposal)
			{
				value.verified_proposal = value.proposal;
				++value.sequence;
			}
			return true;
		}
		void refuse(std::uint64_t serial, const char* reason)
		{
			std::lock_guard lock(mutex);
			if (serial != value.command_serial + 1)
				return;
			value.command_serial = serial;
			value.command_result = "refused";
			value.reason = reason;
			++value.sequence;
		}
		bool apply(const command& request, std::optional<endpoint> target = {}, std::function<void(const endpoint&)> publish = {}, std::uint64_t now = 0)
		{
			std::lock_guard lock(mutex);
			if (request.serial != value.command_serial + 1)
				return false;
			value.command_serial = request.serial;
			++value.sequence;
			value.command_result = "refused";
			const auto deny = [&](const char* reason)
			{
				value.reason = reason;
				return false;
			};
			if (!value.enabled || request.identity != value.identity)
				return deny("stale_context");
			if (request.action == operation::stage)
			{
				if (value.proposal)
					return deny("candidate_pending");
				if (!target || !unicast(*target))
					return deny("invalid_endpoint");
				if (target->kind != value.active.kind)
					return deny("family_mismatch");
				value.candidate = *target;
				value.proposal = request.serial;
				value.verified_proposal = 0;
				value.candidate_started_us = now;
			}
			else
			{
				if (!value.proposal || request.proposal != value.proposal)
					return deny("proposal_mismatch");
				if (request.action == operation::commit)
				{
					if (value.verified_proposal != value.proposal)
						return deny("candidate_unverified");
					if (now >= value.candidate_proof_us && now - value.candidate_proof_us > 1000000)
						return deny("candidate_stale");
					if (publish)
						publish(value.candidate);
					value.previous = value.active;
					value.previous_until_us = now + 60000000;
					value.active = value.candidate;
					value.active_proposal = value.proposal;
				}
				value.proposal = value.verified_proposal = 0;
				value.candidate = {};
			}
			value.command_result = "applied";
			value.reason = "";
			return true;
		}
	};
	// Before peerHeard, lastValidReceiveAgoMs dates from the first send, or key installation if unsent.
	// sent and sentAgoMs let the launcher compare both peers' admission and detect a route carrying no input either way.
	inline std::string status_json(const status& value, std::uint64_t now)
	{
		const auto last = value.last_valid_us ? value.last_valid_us : value.started_us;
		const auto sent_ago = value.sent && now >= value.first_sent_us ? (now - value.first_sent_us) / 1000 : 0;
		return "{\"keyEpoch\":" + std::to_string(value.identity.epoch) + ",\"peerSession\":" + std::to_string(value.identity.peer_session) +
		       ",\"localPort\":" + std::to_string(value.local_port) +
		       ",\"family\":" + std::to_string(static_cast<unsigned>(value.active.kind)) +
		       ",\"sessionId\":" + pbd::json::quoted(value.identity.session_id) + ",\"setId\":" + pbd::json::quoted(value.identity.set_id) +
		       ",\"requestId\":" + std::to_string(value.request_id) + ",\"recovering\":" + (value.recovering ? "true" : "false") +
		       ",\"sent\":" + (value.sent ? "true" : "false") + ",\"sentAgoMs\":" + std::to_string(sent_ago) +
		       ",\"peerHeard\":" + (value.heard ? "true" : "false") + ",\"lastValidReceiveAgoMs\":" + std::to_string(now >= last ? (now - last) / 1000 : 0) +
		       ",\"commandSerial\":" + std::to_string(value.command_serial) + ",\"commandResult\":" + pbd::json::quoted(value.command_result) +
		       ",\"proposalSerial\":" + std::to_string(value.proposal) + ",\"verifiedProposalSerial\":" + std::to_string(value.verified_proposal) +
		       ",\"activeProposalSerial\":" + std::to_string(value.active_proposal) + ",\"reason\":" + pbd::json::quoted(value.reason) + "}";
	}
} // namespace pbd::peer_recovery
