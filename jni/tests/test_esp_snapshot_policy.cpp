#include "esp_snapshot_policy.h"

#include <cassert>
#include <cstdio>

int main() {
    assert(espsnapshot::within_age(10.10, 10.0, 0.11));
    assert(!espsnapshot::within_age(10.12, 10.0, 0.11));
    assert(!espsnapshot::within_age(9.9, 10.0, 0.5));
    assert(!espsnapshot::within_age(10.1, 0.0, 1.0));

    assert(!espsnapshot::due(10.04, 10.0, 0.05));
    assert(espsnapshot::due(10.05, 10.0, 0.05));
    assert(espsnapshot::due(10.1, 0.0, 0.05));
    assert(espsnapshot::due(9.9, 10.0, 0.05));

    std::puts("ESP snapshot policy tests passed");
    return 0;
}
