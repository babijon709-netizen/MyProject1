#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
// Serializes this feature with bind/reset; never hold across UI or rendering.
std::recursive_mutex& viewmodel_session_mutex();
// The single managed-heap write gate used by viewmodel and remote hit-volume
// features. It always writes through the already selected g_mem transport.
bool viewmodel_write_heap(uint64_t address, const void* data, std::size_t size);
