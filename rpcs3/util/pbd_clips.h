#pragma once

#include "video_sink.h"
#include <functional>
#include <memory>
#include <string>

namespace pbd::clips
{
	struct options
	{
		bool background = true;
		unsigned seconds = 30;
		unsigned sample_rate = 48000;
		unsigned channels = 2;
		std::string folder;
		// The mode and fighters after the date in a file name ("Ranked Jin Kazama vs Kazuya Mishima"); empty when not known.
		std::string label;
	};
	struct status
	{
		bool available = false;
		bool background = false;
		bool recording = false;
		std::string encoder, saved, error;
		std::uint64_t dropped = 0;
	};

	class recorder final : public utils::video_sink
	{
		struct impl;
		std::unique_ptr<impl> m;
	public:
		recorder(options config, std::function<void(status)> publish);
		~recorder() override;
		void configure(options config);
		void save_clip();
		void toggle_recording();
		bool wants_frame() const override;
		bool asynchronous_capture() const override { return true; }
		bool offer_frame(std::vector<u8>& frame, u32 pitch, u32 width, u32 height, s32 pixel_format, usz timestamp_ms) override;
		bool offer_samples(const u8* buf, u32 sample_count, u16 channels, usz timestamp_us) override;
		void frame_dropped() override;
		void stop(bool flush = true) override;
		void pause(bool flush = true) override;
		void resume() override;
	};
	// Auxiliary pad buttons have no PS3 input bits. These requests do not touch the game's pad state.
	void request_save();
	void request_record();
	unsigned take_requests();
	bool hardware_available();
}
