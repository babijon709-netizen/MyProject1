#pragma once
#include <string>

enum class AuthHttpResult { allowed, denied, unavailable };

// HTTPS authorization using the bundled TLS implementation and Android's
// system CA certificates. No Termux, curl, external shell or proxy required.
AuthHttpResult PostDeviceAuth(const std::string& url, const std::string& body,
                              const std::string& nonce);
