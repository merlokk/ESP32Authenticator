#pragma once

// Shared dispatch for commands with subcommands (ble, wifi, config, spiffs).
// All of them behave the same:
//   <cmd>                usage, then `<cmd> info`; returns what info returns
//   <cmd> help | -h      usage, returns 0
//   <cmd> <unknown>      "unknown subcommand", usage, returns 1

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace console {

struct Subcommand {
    const char *name;
    int (*func)(int, char **);
};

template <size_t N>
int Dispatch(const Subcommand (&subs)[N], int argc, char **argv, const char *usage) {
    if (argc < 2) {
        printf("usage: %s\n", usage);
        for (const Subcommand &sub : subs) {
            if (strcmp(sub.name, "info") == 0) {
                printf("\n");
                return sub.func(argc, argv);
            }
        }
        return 0;
    }
    for (const Subcommand &sub : subs) {
        if (strcmp(argv[1], sub.name) == 0) {
            return sub.func(argc - 1, argv + 1);
        }
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("usage: %s\n", usage);
        return 0;
    }
    printf("unknown subcommand '%s'\nusage: %s\n", argv[1], usage);
    return 1;
}

}  // namespace console
