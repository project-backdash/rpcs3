# The permanent game window

The launcher owns the permanent Windows window. A hosted game creates its own Win32 child, Vulkan surface and swapchain.
The launcher never copies or presents game frames. The game's hidden Qt window receives asynchronous input and geometry
events, but is not parented to the launcher. Cross-process child parenting joins Windows input queues automatically;
only a small native message thread joins that queue. Qt, rendering, settings writes and monitor-mode discovery stay on
other threads. A delayed Qt GUI therefore does not require killing the game to restore launcher input.

`PBD_GAME_HOST` carries schema `pbd-game-host-1`, decimal `hwnd`, launcher `pid`, and a positive 31-bit message `token`.
Native verifies the window's process, root identity and Windows session. `displayMode` is `fullscreen`, `borderless` or
`windowed`; the launcher applies the saved mode and writes the matching Vulkan fullscreen preference. `presentation`
selects the hosted surface or the game's own window. Preparation does not receive the contract. This boundary does not
change guest state or confirmed inputs.

The hosted child starts hidden. Launcher actions explicitly grant input to `game` or `launcher`; granting the launcher
hides the child and releases game input. Granting the game shows it and focuses it only when the launcher already owns
the foreground. Window focus and asynchronous foreground events update controller focus after Alt+Tab. There is no focus
timer and no background focus reclamation. Menus and chat can retain ownership while the game loads.

The native message thread handles mouse and window messages, and keys when raw input is not in use, without synchronous
calls into Qt or the renderer. It posts Qt events and stores client dimensions in physical pixels. Logical geometry uses
the child's current DPI.
Resize and monitor changes notify the control worker, which discovers display refresh outside the shared input queue.
Fullscreen shortcuts post `WM_APP + 0x42` with the token. Action 2 toggles the launcher's fullscreen state; other actions
include the Native process ID in the upper bits so stale game returns can be rejected.

The keyboard does not use that shared queue. A third thread owns a message-only window (`rpcs3qt/pbd_raw_keyboard.h`),
whose queue is its own, and registers it for the keyboard as raw input with `RIDEV_INPUTSINK`. A report reaches the game
only while the hosted child is shown, has the keyboard focus and its root is the foreground window. Each report becomes
the key message the child would have received: the scan code with the E0 flag as the extended bit, Pause as 0x45, Num
Lock as an extended 0x45, a repeat when the key is already down, Alt in the context bit, and the escape-sequence fillers
dropped. It is posted to Qt by the code the window's messages use, so custom keys see the same keys and scan codes.
Losing the focus, or the first report that arrives without it, releases every key still down; that also covers key-ups
lost to a secure desktop. The child window keeps only F11 and Alt+F4 and swallows every other key message. Raw input
registration is per process: if another handler takes the keyboard, the sink notices within 250 ms, logs it once and
the child window delivers keys again.

For the launcher's run summary, `game-presentation.jsonl` says how frames and keys travelled
(`rpcs3/Emu/PBD/pbd_presentation_journal.h`). It lives in the run folder that holds `peer-transport.jsonl`; a run
without one writes nothing. A present row (`hosted`, `flip_model`, `present_mode`) is appended when the first swap
chain is made and again whenever one of the three changes: a swap chain rebuilt in another mode, or the fallback to the
game's own window. A keyboard row (`raw`) is appended at start and when another handler takes the raw keyboard. A
report only queues its row under a short lock and a writer thread appends it, so the render and input threads never
wait on the disk.

The local named pipe `\\.\pipe\PBD-game-control-<pid>` accepts one bounded request:
`PBD1 <token> <close|resize|launcher|game>\n`. It rejects remote clients, malformed commands and mismatched tokens.
`OK\n` acknowledges that the command was queued, not that the game has exited; the client confirms receipt with `ACK\n`.
Control I/O has a one-second bound and cancellation. Hosted close posts to the existing Qt window and uses the ordinary
process-exit budget. It does not compile PowerShell or C# on the close path.

The worker observes launcher process lifetime. Launcher exit terminates its own orphaned game process; there is no GUI
heartbeat timeout, healthy-load kill rule or hosting priority promotion. Shutdown hides the child and returns launcher
ownership before emulation threads are joined. The render HWND remains valid until the frame is destroyed.
Game-only runs skip emulator playtime bookkeeping, including its periodic QSettings flush, whether hosted or standalone.

Hosting has to keep working with real keyboard and controller input, menus/chat ownership, startup and healthy streaming
load, a delayed hidden Qt GUI, bounded close, display scaling, monitor moves, OBS capture and Discord streams. Hosted and
standalone timings compare only under identical scheduling, playtime policy, resolution, refresh and requested Vulkan
present mode. The Windows presentation path, including independent flip, is observed on each system; child hosting does
not promise it.
