#pragma once

// Checks the remote switch before any game/UI initialization, then starts a
// background heartbeat. On revocation or network errors main_thread_flag is
// cleared, allowing normal resource cleanup. An empty URL always denies.
bool RemoteGateStart();
