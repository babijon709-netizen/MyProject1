#pragma once

// build.sh generates the ignored local header from BENZWARE_STATUS_URL.
// Without it, native startup is deliberately denied (fail closed).
// A compiler definition takes priority so host tests can supply a fixture URL.
#ifndef BENZWARE_STATUS_URL
#if __has_include("benzware_remote_local.h")
#include "benzware_remote_local.h"
#else
#define BENZWARE_STATUS_URL ""
#endif
#endif
