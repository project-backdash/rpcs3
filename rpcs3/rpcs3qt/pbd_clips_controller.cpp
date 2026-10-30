#include "stdafx.h"
#include "pbd_clips_controller.h"
#include "gs_frame.h"
#include "util/pbd_clips.h"
#include "util/video_provider.h"
#include "Emu/Cell/Modules/cellAudio.h"
#include "Emu/Cell/lv2/sys_rsxaudio.h"
#include "Emu/IdManager.h"
#include "Emu/system_config.h"
#include "Emu/RSX/Overlays/overlay_message.h"
#include "Emu/PBD/pbd_env.h"
#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QWindow>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

extern atomic_t<recording_mode> g_recording_mode;

namespace pbd::clips
{
	namespace
	{
		QString environment(const char* key)
		{
			const auto* value = pbd::env::find(key); return value ? QString::fromUtf8(value) : QString{};
		}
		struct updates { std::mutex mutex; status latest; bool changed = false; };
#ifdef _WIN32
		std::pair<UINT, UINT> shortcut(QString text)
		{
			UINT modifiers = MOD_NOREPEAT;
			const auto parts = text.split('+');
			for (int i = 0; i + 1 < parts.size(); ++i)
			{
				if (parts[i] == "Ctrl") modifiers |= MOD_CONTROL;
				else if (parts[i] == "Alt") modifiers |= MOD_ALT;
				else if (parts[i] == "Shift") modifiers |= MOD_SHIFT;
				else return {};
			}
			const auto key = parts.back();
			if (key.startsWith('F'))
			{
				bool ok = false; const auto number = key.mid(1).toUInt(&ok);
				if (ok && number >= 1 && number <= 24) return {modifiers, VK_F1 + number - 1};
			}
			if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9')))
				return {modifiers, key[0].unicode()};
			return {};
		}
#endif
	}

	struct controller::impl
	{
		gs_frame* window;
		QTimer timer;
		QString configuration = environment("PBD_CLIPS_CONFIG_FILE"), output = environment("PBD_CLIPS_STATUS_FILE");
		QByteArray previous;
		std::shared_ptr<updates> update = std::make_shared<updates>();
		std::shared_ptr<recorder> capture;
		std::shared_ptr<atomic_t<u32>> red_dot;
		QString saved;
		std::string shown_error;
		options config;
		unsigned sequence = 0;
#ifdef _WIN32
		static constexpr int save_id = 0x5042, record_id = 0x5043;
		HWND registered_window = nullptr;
		bool keys_ready = false, keys_warned = false, keys_shown = false;
		QJsonObject key_settings;
		// Delivery stays on Qt's thread, but visibility and foreground ownership belong to the actual render HWND.
		// A parked game or another program in front must never lose keys to a background recorder.
		bool window_shown() const
		{
			const auto handle = static_cast<GSFrameBase*>(window)->handle();
			const auto root = GetAncestor(handle, GA_ROOT);
			return handle && IsWindowVisible(handle) && !IsIconic(root) && GetForegroundWindow() == root;
		}
		void release_keys()
		{
			if (registered_window)
			{
				UnregisterHotKey(registered_window, save_id); UnregisterHotKey(registered_window, record_id);
			}
			registered_window = nullptr; keys_ready = false;
		}
		void hotkeys(const QJsonObject& settings)
		{
			key_settings = settings;
			release_keys();
			keys_shown = window_shown();
			if (!keys_shown) return;
			const auto handle = reinterpret_cast<HWND>(window->winId());
			registered_window = handle; keys_ready = true;
			for (const auto& [id, key] : {std::pair{save_id, settings.value("saveKey").toString("Ctrl+F10")},
				std::pair{record_id, settings.value("recordKey").toString("Ctrl+Shift+F11")}})
			{
				const auto [modifiers, code] = shortcut(key);
				if (!code || !RegisterHotKey(handle, id, modifiers, code)) keys_ready = false;
			}
			if (!keys_ready && !keys_warned)
				rsx::overlays::queue_message(std::string("A Clips key is already in use. Choose another in Settings."), 2'000'000);
			keys_warned = !keys_ready;
		}
#endif
		QJsonObject settings()
		{
			QFile file(configuration);
			if (configuration.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 16384) return {};
			const auto bytes = file.readAll();
			const auto document = QJsonDocument::fromJson(bytes);
			if (!document.isObject()) return {};
			previous = bytes;
			return document.object();
		}
		void configure(const QJsonObject& settings)
		{
			config.background = settings.value("background").toBool(true);
			const auto seconds = settings.value("seconds").toInt(30);
			config.seconds = seconds == 15 || seconds == 30 || seconds == 60 || seconds == 120 || seconds == 300 ? seconds : 30;
			const auto selected = settings.value("folder").toString();
			config.folder = !selected.isEmpty() && QFileInfo(selected).isAbsolute() ? selected.toUtf8().toStdString() : std::string{};
			// What the player is doing, as the launcher last wrote it ("Ranked Jin Kazama vs Kazuya Mishima"): it follows the date
			// in the next file's name (file_label keeps it to safe characters).
			config.label = settings.value("label").toString().toUtf8().toStdString();
#ifdef _WIN32
			// The launcher rewrites this file whenever the label changes. Registering the same keys again would drop them for a
			// moment, so they are registered again only when they change (or are not registered yet).
			if (!keys_ready || settings.value("saveKey") != key_settings.value("saveKey") || settings.value("recordKey") != key_settings.value("recordKey"))
				hotkeys(settings);
#endif
			if (capture) capture->configure(config);
		}
		explicit impl(gs_frame* parent) : window(parent)
		{
			switch (g_cfg.audio.provider)
			{
			case audio_provider::none: config.channels = 0; break;
			case audio_provider::cell_audio:
				config.sample_rate = g_fxo->get<cell_audio>().cfg.audio_sampling_rate;
				config.channels = g_fxo->get<cell_audio>().cfg.audio_channels; break;
			case audio_provider::rsxaudio:
				config.sample_rate = g_fxo->get<rsx_audio_backend>().get_sample_rate();
				config.channels = g_fxo->get<rsx_audio_backend>().get_channel_count(); break;
			}
			configure(settings());
			const auto state = update;
			capture = std::make_shared<recorder>(config, [state](status value)
			{
				std::lock_guard lock(state->mutex); state->latest = std::move(value); state->changed = true;
			});
			auto& provider = g_fxo->get<utils::video_provider>();
			if (provider.set_video_sink(capture, recording_mode::rpcs3))
			{
				provider.set_pause_time_us(0); g_recording_mode = recording_mode::rpcs3;
			}
			QObject::connect(&timer, &QTimer::timeout, window, [this] { tick(); }); timer.start(100);
		}
		~impl()
		{
			timer.stop();
#ifdef _WIN32
			release_keys();
#endif
			if (red_dot) *red_dot = 0;
			capture->stop();
		}
		bool ready()
		{
			std::lock_guard lock(update->mutex);
			if (update->latest.available) return true;
			rsx::overlays::queue_message(update->latest.error.empty() ? std::string("The clip buffer is getting ready.") : update->latest.error, 2'000'000);
			return false;
		}
		void save() { if (ready()) capture->save_clip(); }
		void record() { if (ready()) capture->toggle_recording(); }
		void tick()
		{
#ifdef _WIN32
			if (window_shown() != keys_shown) hotkeys(key_settings);
#endif
			static constexpr unsigned reload_interval = 10;
			if (++sequence % reload_interval == 0)
			{
				const auto before = previous;
				const auto latest = settings();
				if (before != previous) configure(latest);
#ifdef _WIN32
				else if (window_shown() != keys_shown || (keys_shown && (!keys_ready || registered_window != reinterpret_cast<HWND>(window->winId()))))
					hotkeys(key_settings);
#endif
			}
			const auto command = take_requests();
			if (command & 1) save();
			if (command & 2) record();
			status state;
			{
				std::lock_guard lock(update->mutex);
				if (!update->changed) return;
				state = update->latest; update->changed = false;
			}
			if (state.recording && !red_dot)
			{
				red_dot = std::make_shared<atomic_t<u32>>(1);
				auto icon = std::make_shared<rsx::overlays::rounded_rect>(); icon->set_size(10, 10); icon->back_color = {1.f, 0.f, 0.f, 1.f};
				rsx::overlays::queue_message(std::string("Recording"), umax, red_dot, rsx::overlays::message_pin_location::top_right, icon);
			}
			if (!state.recording && red_dot) { *red_dot = 0; red_dot.reset(); }
			if (state.available && !state.error.empty() && shown_error != state.error)
				rsx::overlays::queue_message(state.error, 2'000'000, {}, rsx::overlays::message_pin_location::top_right);
			shown_error = state.error;
			if (!state.saved.empty() && saved != QString::fromUtf8(state.saved.c_str()))
			{
				saved = QString::fromUtf8(state.saved.c_str());
				rsx::overlays::queue_message(std::string("Clip saved"), 2'000'000, {}, rsx::overlays::message_pin_location::top_right);
			}
			QJsonObject value{{"schema", "pbd-clips-1"}, {"pid", static_cast<qint64>(QCoreApplication::applicationPid())},
				{"available", state.available}, {"background", state.background}, {"recording", state.recording},
				{"encoder", QString::fromUtf8(state.encoder.c_str())}, {"saved", saved}, {"error", QString::fromUtf8(state.error.c_str())},
				{"dropped", static_cast<qint64>(state.dropped)}};
			if (!output.isEmpty())
			{
				QSaveFile file(output);
				if (file.open(QIODevice::WriteOnly)) { file.write(QJsonDocument(value).toJson(QJsonDocument::Compact)); file.commit(); }
			}
		}
	};
	controller::controller(gs_frame* window) : m(std::make_unique<impl>(window)) {}
	controller::~controller() = default;
	void controller::save() { m->save(); }
	void controller::record() { m->record(); }
	bool controller::available() const { std::lock_guard lock(m->update->mutex); return m->update->latest.available; }
	bool controller::recording() const { std::lock_guard lock(m->update->mutex); return m->update->latest.recording; }
	bool controller::native_event(void* message)
	{
#ifdef _WIN32
		const auto* event = static_cast<MSG*>(message);
		if (event->message == WM_HOTKEY && event->wParam == impl::save_id) { if (m->window_shown()) save(); return true; }
		if (event->message == WM_HOTKEY && event->wParam == impl::record_id) { if (m->window_shown()) record(); return true; }
#endif
		return false;
	}
}
