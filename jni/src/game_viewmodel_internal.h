#pragma once
#include <mutex>
// Serializes this feature with bind/reset; never hold across UI or rendering.
std::recursive_mutex& viewmodel_session_mutex();
