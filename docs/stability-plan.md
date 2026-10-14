# Stability: the parked freezes and crashes

Status: plan (2026-10-14), branch `stability` (from main b3835b1). The user: "revisit the crashes we were running into
in our testing that I decided to hold off. See if it's still relevant."

## Triage (2026-10-14, from logs/wine-*.txt since 2026-10-10)

| issue | still relevant? | evidence |
|---|---|---|
| **Zone-load freeze** ("loader-lock freeze") | **yes, the main one** | 7 of ~256 sessions since 10-11: 10-11 11:21, 11:46, 16:23; 10-13 09:44, 19:19, 22:15, 22:27. None yet on 10-14. |
| Waterfall dangling pointers | fixed on main (c07abc9); **released ao-vk v0.4.0 = v0.4.1 (adb6c67) still have it** | git merge-base |
| Shadow-only draws corrupting the grass tile map | fixed today (DrawShadowOnly, rvk/draw.cpp:2099); watch only | user report 10-14 |
| Heap corruption at ground radius 67 (10-09) | unknown, not seen since; may be the same family as the freeze | [[ao-draw-distance]] memory |
| Driver crash in vkCreateGraphicsPipelines (10-13, demo) | no (transient, fixed by reboot) | |

### The freeze, read closer

It is not one Wine loader bug. Every case is a chain of threads waiting on a lock whose owner never releases:

- 10-13 22:15 and 22:27: thread A waits for `loader_section`, held by B; **B waits for the main process heap lock**,
  held by C (0118 in both). C never wrote a line to the log.
- 10-10 15:22: the process-heap holder had just hit `stack overflow ... addr 0x7bccaaf2` (inside ntdll) - a thread
  that **faulted while holding the heap lock**.
- The waterfall freeze (10-12) had the same outcome: the render thread faulted, Miles' (mss32) exception filter hung,
  and the game froze instead of crashing.
- All 7 happen around a playfield load (RDB reads, "Creating object 51103:<pf>"), which allocates a lot and loads DLLs.

Working hypothesis: **these freezes are crashes that never surface**. A thread faults inside the heap or loader code
(heap corruption, or a bad pointer from our code), and the exception path then blocks on the lock that thread already
holds, or mss32's filter hangs. Every other thread then piles up behind it.

Why we have no stack: `CrashLogger` (proxy/native/hook.cpp) logs only faults in randy31 / randy31_orig /
DisplaySystem / Gamecode. It calls `GetModuleHandleExA`, which takes the loader lock, and `Log`, which may allocate.
A fault in ntdll's heap is therefore filtered out, and even a logged one can deadlock in the same locks.
randy-vk.log is also rotated to one `.old`, so the frozen sessions' render logs are gone. The wine logs don't record
which staged DLL was running.

## P0 - make the next freeze report itself (no debugger, no pop-ups)

1. **A lock-free fault reporter.** At startup, snapshot the module list (base, size, name) into a fixed table. The
   vectored handler resolves addresses from that table, formats into a stack buffer, and writes with `WriteFile` to a
   handle opened at startup. It makes no loader calls and no heap calls. It reports faults in **every** module
   (ntdll's included), up to N, with registers and the scanned return addresses (as now).
2. **A hang watchdog thread** in randy-vk. It watches a heartbeat from the game thread (EndFrame) and the render
   thread. If one stalls more than ~10 s (outside a known long load), it suspends the game's threads and reads each
   one's context and stack (`SuspendThread`/`GetThreadContext`, stack scan as in 1). It writes them to
   `logs/hang-<time>.txt`, resumes the threads, and does nothing else. It is still lock-free, so it works when
   C holds the heap.
3. **Keep the evidence.** Per-session randy-vk logs (`randy-vk-<time>.log`, keep the last ~20) instead of one `.old`.
   ao-wine.sh echoes `AO_RANDYVK_DLL` and its `.map` path into the wine log.
4. Test 1 and 2 in the harness with a forced fault, a forced fault while holding the process heap lock, and a stalled
   frame. Exit quietly with no dialogs ([[no-test-popups]]).

Then the user plays normally. A freeze then leaves a `hang-*.txt` that names the thread holding the lock and where
it stopped.

## P1 - fix what P0 catches

- If the holder faulted in our code: fix it directly, as with the waterfall.
- If it faulted in the heap (corruption): reproduce with Wine's heap checking on (`WINEDEBUG=warn+heap`) in a test
  session at the zone that froze. Suspects: the 10-09 ground radius 67 corruption, and anything that writes into
  game-owned buffers (AOReloaded ground / char limits, statels, mesh ids).
- If the holder is only *waiting* (a true lock-order deadlock): see who took the loader lock (DllMain work, a
  `LoadLibrary` on the render thread, Vulkan ICD loading at zone load) and move that work out of it.

## P2 - patch release

Cherry-pick c07abc9 (waterfall) onto v0.4.1 -> **v0.4.2**, plus any P1 fix that is small and safe. Main itself is
far ahead (185 commits); a full release is a separate decision.

## P3 - smaller items

- The game's "Setting states while states are already set" spam (10-13 22:27, just before that freeze): find the
  caller and whether our state-block emulation triggers it.
- Watch for the grass tile-map crash coming back after today's DrawShadowOnly change.

## User reports (2026-10-14, ~/Downloads/aovk-user-logs, all on v0.4.0/0.4.1 = adb6c67, Windows)

All four have every `[Native]` part on (Skin/Anim/Visuals/Cat*/Device/Scene), though the code defaults them off
(README's example block shows them all `on`). None of the logs has a `fault` line.

- **qnix (crash, RTX 4090 laptop, two clients in one folder sharing one randy-vk.log):** Windows report: write to
  0x00000014 in ntdll's heap, game thread, from ACE.dll <- ResourceManager <- Interfaces <- AFCM <- AnarchyOnline.exe.
  That's **heap corruption found at a free**. The last log line is a static-arena reset = a zone change. Same family
  as our Wine freezes (fault inside the heap at zone load), on Windows it crashes instead of hanging.
- **ben (crash entering AI missions, consistent, RTX 4080):** two sessions; one ends right after the first world load
  (before any frame), the other ends mid-play with no line after its last 600-frame stats -> during a zone load. Fits
  the same family; also possible: new PRK mission content our code doesn't expect. Repro target: enter an AI mission.
- **cirassel (stutter while attacking, RTX 4080S 4K):** the log has only 600-frame averages (120-140 fps), so the
  hitches are invisible. Per-second point-shadow churn while fighting ("N got a cube, N lost one") is the main lead;
  particles, character mesh lookups next. Needs a per-frame hitch log (P0 addition).
- **pirate (fps tanks scrolling chat, RX 9070 XT):** worst block = 64 UpdateTexture a frame and 11.3 MB a frame
  through the hand-off (0.3 MB idle); fps 60 -> 48. Each texture update copies the whole texture (Upload: full
  level, no dirty rect), and since 0.4 rvk scans each full upload 3 times on the render thread (opaque, thumbnail,
  alpha mask). The render thread is still idle 10 ms, so the game thread is the limit - measure the game-thread
  side (Lock/Unlock, Upload's copy/convert) in a local repro: hit a dummy in Newland, scroll the chat.
- **kerri (no log, spike opening the planet map since 0.4, even with enhancements off):** enhancements off still uses
  rvk; suspects: the per-upload scans above on a big map texture, AttachMaterialMaps. The F10 spike the user saw:
  AOReloaded serves OptionPanel/Root.xml by writing a delete-on-close temp file per open (on Windows, AV scans it).

### Found while reading: the frame inspector runs in every build
proxy/trace.cpp captures a whole frame **every 60 s** (`now - g_lastAuto > 60000`) in release and main: std::string
and map work plus `GetModuleHandleExA` (loader lock) per traced call, and a block appended to randy-vk.log (qnix: 330
captures). That's a guaranteed hitch a minute for every user, and loader-lock traffic on the game thread. Make the
auto capture opt-in (env/ini), keep Ctrl+Shift+F12.
