#include "nvs_cmd.h"

#include <cstdio>
#include <cstring>

#include "nvs.h"
#include "subcommand.h"

namespace console {

namespace {

const char *TypeName(nvs_type_t type) {
    switch (type) {
        case NVS_TYPE_U8: return "u8";
        case NVS_TYPE_I8: return "i8";
        case NVS_TYPE_U16: return "u16";
        case NVS_TYPE_I16: return "i16";
        case NVS_TYPE_U32: return "u32";
        case NVS_TYPE_I32: return "i32";
        case NVS_TYPE_U64: return "u64";
        case NVS_TYPE_I64: return "i64";
        case NVS_TYPE_STR: return "str";
        case NVS_TYPE_BLOB: return "blob";
        default: return "?";
    }
}

int CmdLs(int argc, char **argv) {
    if (argc > 2) {
        printf("usage: nvs ls [namespace]\n");
        return 1;
    }
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, argc == 2 ? argv[1] : nullptr,
                                   NVS_TYPE_ANY, &it);
    unsigned count = 0;
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        printf("%-15s %-15s %s\n", info.namespace_name, info.key, TypeName(info.type));
        ++count;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    printf("%u key(s)\n", count);
    return 0;
}

int CmdRm(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: nvs rm <namespace> <key>\n");
        return 1;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(argv[1], NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, argv[2]);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        printf("%s/%s: %s\n", argv[1], argv[2], esp_err_to_name(err));
        return 1;
    }
    printf("erased %s/%s\n", argv[1], argv[2]);
    return 0;
}

constexpr Subcommand kNvsSubcommands[] = {
    {"ls", &CmdLs},
    {"rm", &CmdRm},
};

}  // namespace

int CmdNvs(int argc, char **argv) {
    return Dispatch(kNvsSubcommands, argc, argv, "nvs ls [namespace] | rm <namespace> <key>");
}

}  // namespace console
