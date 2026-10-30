#pragma once

#include <memory>
class gs_frame;

namespace pbd::clips
{
	class controller
	{
		struct impl;
		std::unique_ptr<impl> m;

	public:
		explicit controller(gs_frame* window);
		~controller();
		void save();
		void record();
		// What the in-game menu offers: Start recording or Stop recording, only while clips can record on this PC.
		bool available() const;
		bool recording() const;
		bool native_event(void* message);
	};
} // namespace pbd::clips
