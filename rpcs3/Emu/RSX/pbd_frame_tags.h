#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace pbd::frame_presentation
{
	enum class counter_select_result
	{
		wait,
		select,
		reject_segment,
		reject_counter,
		reject_epoch_ended
	};

	class counter_epoch_selector
	{
		std::uint32_t requested_segment, requested_counter, last_counter = 0;
		bool epoch_seen = false;

	public:
		counter_epoch_selector(std::uint32_t segment, std::uint32_t counter) : requested_segment(segment), requested_counter(counter) {}
		counter_select_result observe(std::uint64_t context)
		{
			if (context == ~std::uint64_t{0})
				return counter_select_result::wait;
			const auto segment = static_cast<std::uint32_t>(context >> 32);
			const auto counter = static_cast<std::uint32_t>(context);
			if (segment < requested_segment)
				return counter_select_result::wait;
			if (segment > requested_segment)
				return counter_select_result::reject_segment;
			if (counter == requested_counter)
			{
				epoch_seen = true;
				return counter_select_result::select;
			}
			if (!epoch_seen)
			{
				// A session barrier can publish the new segment one live boundary
				// before the game's input counter resets. High values still belong
				// to the preceding counter epoch and cannot prove the target missed.
				if (counter < requested_counter)
				{
					epoch_seen = true;
					last_counter = counter;
				}
				return counter_select_result::wait;
			}
			// The counter restarts at phase changes (round transitions): once the first epoch has ended
			// without the requested value, a later equal value names a different logical tick.
			if (counter < last_counter)
				return counter_select_result::reject_epoch_ended;
			last_counter = counter;
			return counter < requested_counter ? counter_select_result::wait : counter_select_result::reject_counter;
		}
		bool established() const
		{
			return epoch_seen;
		}
	};

	// Command identity. The main-owner thread registers one record when it
	// enters the title's cellGcmSetFlip thunk. That LLE routine (PRX 0x6d23b4 ->
	// 0x6d18bc -> 0x6d52c4) writes GCM_DRIVER_QUEUE+1 (header 0x0004E944) as its
	// FIRST command at the context's current write pointer, after an optional
	// segment-switch callback that leaves only NOPs/JUMPs behind. The RSX, when it
	// executes that DRIVER_QUEUE method, knows the command's effective address and
	// binds the queue head only if a NOP/JUMP-only chain leads from the registered
	// write pointer to exactly that address. Nothing reads a mutable tick label at
	// RSX execution time.
	enum class command_state : std::uint8_t
	{
		pending = 0,    // registered, not yet executed by the RSX
		exact = 1,      // executed command address == registered write pointer
		via_chain = 2,  // reached from the write pointer through NOP/JUMP words only
		mismatch = 3,   // executed command is not the one this record registered
		no_command = 4, // popped outside a FIFO DRIVER_QUEUE method (forced/unqueued flip)
	};

	inline const char* command_state_name(command_state state) noexcept
	{
		switch (state)
		{
		case command_state::pending: return "pending";
		case command_state::exact: return "exact";
		case command_state::via_chain: return "via_chain";
		case command_state::mismatch: return "mismatch";
		case command_state::no_command: return "no_command";
		}
		return "unknown";
	}

	class frame_tag_queue
	{
	public:
		static constexpr std::uint64_t invalid = ~std::uint64_t{0};
		static constexpr std::uint32_t no_address = ~std::uint32_t{0};
		struct record
		{
			std::uint32_t buffer = 0;
			std::uint32_t boundary = ~std::uint32_t{0};
			std::uint64_t context = invalid;
			std::uint64_t sequence = 0;
			bool live = false;
			bool accepted = false;
			// Immutable render token, copied from the main-boundary template.
			std::uint64_t session_epoch = 0;
			std::uint32_t represented_tick = ~std::uint32_t{0};
			std::uint32_t replay_generation = 0;
			std::uint64_t render_serial = 0;
			std::uint32_t fifo_address = no_address; // EA of the executed DRIVER_QUEUE header (RSX-proved)
			std::uint32_t buffer_generation = 0;     // per display buffer id
			std::uint64_t request_serial = 0;
			std::uint32_t caller_thread = 0;
			std::uint32_t token_segment = ~std::uint32_t{0};
			std::uint32_t token_counter = ~std::uint32_t{0};
			// Command binding evidence.
			std::uint32_t entry_current = no_address;         // context->current at thunk entry (EA)
			std::uint32_t gcm_context = no_address;           // EA of the CellGcmContextData that wrote the command (command-context identity)
			std::uint32_t segment_offset = ~std::uint32_t{0}; // represented tick minus the first boundary of its session segment
			std::uint32_t gcm_callback = 0;                   // context callback descriptor (ctx+0xc) at registration
			std::uint32_t rsx_dq_seq = 0;                     // ordinal of the DRIVER_QUEUE method execution that bound this record (RSX)
			std::uint32_t gcm_begin = no_address, gcm_end = no_address;
			std::uint32_t fifo_generation = 0; // per-EA consumption count (RSX side), equals the per-EA write count
			std::uint32_t chain_words = 0;     // NOP/JUMP words between entry_current and fifo_address
			std::uint32_t tick_flip_index = 0; // 0 = first flip registered after this boundary
			std::uint32_t flip_head_address = no_address;
			command_state command = command_state::pending;
			bool flip_head_verified = false;
			std::uint32_t scene = ~std::uint32_t{0};
			bool verified() const noexcept
			{
				return accepted && (command == command_state::exact || command == command_state::via_chain) && flip_head_verified;
			}
		};

	private:
		std::mutex mutex;
		std::deque<record> pending;
		std::unordered_map<std::uint32_t, std::uint32_t> written, executed;
		bool armed = false, aligned = false;
		std::uint64_t produced = 0, consumed = 0, faults = 0, unaligned = 0, overwrites = 0;
		static constexpr std::uint64_t max_unaligned = 8;
		// Savestate-era commands are few (at most one flip sequence was in flight at the save).
		// A head that keeps failing to align is a fault, not an indefinitely silent wait.
		void note_unaligned() noexcept
		{
			if (++unaligned == max_unaligned + 1)
				faults++;
		}

	public:
		struct counters
		{
			std::uint64_t produced, consumed, faults, pending, unaligned, overwrites;
			bool aligned;
		};
		record push_record(std::uint32_t buffer, std::uint64_t context, std::uint32_t boundary = ~std::uint32_t{0}, bool live = false,
			const record* identity = nullptr)
		{
			std::lock_guard lock(mutex);
			record result{buffer, boundary, context, ++produced, live, false};
			if (identity)
			{
				const auto sequence = result.sequence;
				result = *identity;
				result.buffer = buffer;
				result.boundary = boundary;
				result.context = context;
				result.sequence = sequence;
				result.live = live;
			}
			if (buffer > 7 || pending.size() >= 256)
			{
				faults++;
				return result;
			}
			if (result.entry_current != no_address)
			{
				// Writing where an unconsumed command still lies would silently
				// replace the command a pending record is waiting for.
				for (const auto& other : pending)
					if (other.entry_current == result.entry_current)
					{
						overwrites++;
						faults++;
						break;
					}
				written[result.entry_current]++;
			}
			else if (!armed)
			{
				// Legacy path (no address): a savestate may resume with an older
				// flip command in the FIFO; the first pair is an alignment sentinel.
				result.context = invalid;
				result.live = false;
			}
			armed = true;
			result.accepted = true;
			pending.push_back(result);
			return result;
		}
		void push(std::uint32_t buffer, std::uint64_t context)
		{
			push_record(buffer, context);
		}
		// Legacy pop (no command address): positional pairing only.
		record pop_record(std::uint32_t buffer)
		{
			std::lock_guard lock(mutex);
			if (pending.empty())
			{
				if (armed)
					faults++;
				return {buffer};
			}
			const auto tag = pending.front();
			pending.pop_front();
			consumed++;
			if (tag.buffer != buffer)
			{
				faults++;
				auto failed = tag;
				failed.buffer = buffer;
				failed.context = invalid;
				failed.live = false;
				return failed;
			}
			return tag;
		}
		// Pop. `address` is the EA of the DRIVER_QUEUE header the RSX is executing
		// (no_address when the pop is not inside such a method). `chain(record, to)`
		// returns 0 when `to` is the record's write pointer, 1 when the RSX reached `to`
		// through the one segment-switch JUMP the LLE can leave there, else a negative value.
		bool ambiguous_command(record& tag, std::uint32_t address, auto& chain)
		{
			for (std::size_t i = 1; i < pending.size(); i++)
			{
				if (pending[i].entry_current != no_address && chain(pending[i], address) >= 0)
				{
					faults++;
					tag.command = command_state::mismatch;
					tag.context = invalid;
					tag.live = false;
					pending.pop_front();
					consumed++;
					return true;
				}
			}

			return false;
		}

		template <typename Chain>
		record pop_command(std::uint32_t buffer, std::uint32_t address, Chain&& chain)
		{
			std::lock_guard lock(mutex);
			record failed{buffer};
			failed.fifo_address = address;
			if (address == no_address)
			{
				// A forced/unqueued frame end is never a registered command. Before the first
				// bound command it is the resumed tail of a savestate-era flip (the queued-flip
				// state is not serialized); after alignment it is a fault.
				if (aligned)
					faults++;
				else
					note_unaligned();
				failed.command = command_state::no_command;
				return failed;
			}
			// Counted only for commands bound to a record, so that a command resumed
			// from a savestate never shifts the per-address generation.
			const std::uint32_t generation = executed[address] + 1;
			if (pending.empty())
			{
				// Before the first bound command, a command resumed from a
				// savestate may execute with nothing registered for it.
				if (aligned)
					faults++;
				else
					note_unaligned();
				failed.command = command_state::mismatch;
				failed.fifo_generation = generation;
				return failed;
			}
			auto tag = pending.front();
			const auto walked = tag.entry_current == no_address ? -1 : chain(tag, address);
			if (walked >= 0)
			{
				// Two pending registrations that could both have produced this command make it ambiguous.
				if (ambiguous_command(tag, address, chain))
					return tag;
			}
			if (walked < 0 && !aligned)
			{
				note_unaligned();
				failed.command = command_state::mismatch;
				failed.fifo_generation = generation;
				return failed;
			}
			pending.pop_front();
			consumed++;
			executed[address] = generation;
			tag.fifo_address = address;
			tag.fifo_generation = generation;
			tag.chain_words = walked < 0 ? 0 : static_cast<std::uint32_t>(walked);
			if (walked > 0)
			{
				// The registration produced its command further along the chain:
				// count the write at the address where it actually lies.
				if (auto found = written.find(tag.entry_current); found != written.end() && !--found->second)
					written.erase(found);
				written[address]++;
			}
			const bool generation_ok = walked < 0 || written[address] == generation;
			if (walked < 0 || tag.buffer != buffer || !generation_ok)
			{
				faults++;
				tag.command = command_state::mismatch;
				tag.context = invalid;
				tag.live = false;
				return tag;
			}
			aligned = true;
			tag.command = walked == 0 ? command_state::exact : command_state::via_chain;
			return tag;
		}
		std::uint64_t pop(std::uint32_t buffer)
		{
			return pop_record(buffer).context;
		}
		void fault() noexcept
		{
			std::lock_guard lock(mutex);
			faults++;
		}
		counters snapshot()
		{
			std::lock_guard lock(mutex);
			return {produced, consumed, faults, pending.size(), unaligned, overwrites, aligned};
		}
		void reset()
		{
			std::lock_guard lock(mutex);
			pending.clear();
			written.clear();
			executed.clear();
			armed = aligned = false;
			produced = consumed = faults = unaligned = overwrites = 0;
		}
	};
} // namespace pbd::frame_presentation
