#pragma once
#include <string>
#include <vector>

namespace prot {
namespace http {

// Simple HTTP client for Android native binary
// Tries multiple methods: curl binary, wget, raw socket (HTTP only)
// Returns body as string, empty on fail

std::string Get(const std::string& url, int timeout_sec = 5);

// Helper to parse JSON field "enabled": true/false
// Returns: 1 = enabled, 0 = disabled, -1 = parse fail
int ParseEnabled(const std::string& json);

// URL encoding helper
std::string UrlEncode(const std::string& s);

// Get device HWID (simple - android id / random)
std::string GetHWID();

} // namespace http
} // namespace prot
