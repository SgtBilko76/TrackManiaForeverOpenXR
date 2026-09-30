#pragma once

namespace tmoxr {
// Starts sampling the calling thread (TrackMania's render thread) about 500
// times a second when TMFOXR-profile.txt exists beside the DLL. Every 15 s
// the log shows where it spent its time, by module, and for time in other
// modules (Direct3D, system code) whether TrackMania or the mod called them.
// Diagnostic only: suspending the thread this often once deadlocked Wine at
// startup on the headset (stuck in NtUserMessageCall), so it is off unless
// the file exists.
void StartRenderThreadProfilerIfRequested();
} // namespace tmoxr
