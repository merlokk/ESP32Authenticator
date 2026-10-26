#include "rng_cmd.h"

#include <cstdio>
#include <cstdlib>

#include "ble.h"
#include "rng.h"
#include "wifi_manager.h"

namespace console {

int CmdRng(int argc, char **argv) {
    constexpr size_t kDefaultChars = 64;
    size_t chars = kDefaultChars;
    if (argc > 2) {
        printf("usage: rng [length]\n");
        return 1;
    }
    if (argc == 2) {
        char *end = nullptr;
        const unsigned long n = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || n == 0 || n > rng::kMaxHexChars) {
            printf("length: 1..%u hex digits\n", static_cast<unsigned>(rng::kMaxHexChars));
            return 1;
        }
        chars = n;
    }
    // On the heap: up to 2 KB, the console's stack is 4 KB.
    char *hex = static_cast<char *>(malloc(chars + 1));
    if (hex == nullptr) {
        printf("no memory\n");
        return 1;
    }
    rng::Hex(chars, hex, chars + 1);
    printf("%s\n", hex);
    free(hex);
    if (!ble::IsOn() && wifimgr::Get().state == wifimgr::State::kOff) {
        printf("radio off: not guaranteed true random (turn BLE or Wi-Fi on)\n");
    }
    return 0;
}

}  // namespace console
