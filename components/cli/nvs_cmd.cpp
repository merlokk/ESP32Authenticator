#include "nvs_cmd.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nvs.h"
#include "nvs_show.h"
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

// The value as `nvs ls` shows it (nvs_show.h): numbers, strings unless
// secret, sizes of blobs.
void PrintValue(const nvs_entry_info_t &info) {
    nvs_handle_t h;
    if (nvs_open(info.namespace_name, NVS_READONLY, &h) != ESP_OK) {
        printf("?");
        return;
    }
    const char *k = info.key;
    esp_err_t err = ESP_OK;
    switch (info.type) {
        case NVS_TYPE_U8: { uint8_t v = 0; err = nvs_get_u8(h, k, &v); printf("%u", v); break; }
        case NVS_TYPE_I8: { int8_t v = 0; err = nvs_get_i8(h, k, &v); printf("%d", v); break; }
        case NVS_TYPE_U16: { uint16_t v = 0; err = nvs_get_u16(h, k, &v); printf("%u", v); break; }
        case NVS_TYPE_I16: { int16_t v = 0; err = nvs_get_i16(h, k, &v); printf("%d", v); break; }
        case NVS_TYPE_U32: { uint32_t v = 0; err = nvs_get_u32(h, k, &v); printf("%" PRIu32, v); break; }
        case NVS_TYPE_I32: { int32_t v = 0; err = nvs_get_i32(h, k, &v); printf("%" PRId32, v); break; }
        case NVS_TYPE_U64: { uint64_t v = 0; err = nvs_get_u64(h, k, &v); printf("%" PRIu64, v); break; }
        case NVS_TYPE_I64: { int64_t v = 0; err = nvs_get_i64(h, k, &v); printf("%" PRId64, v); break; }
        case NVS_TYPE_STR: {
            size_t size = 0;
            err = nvs_get_str(h, k, nullptr, &size);
            if (err != ESP_OK) {
                break;
            }
            if (nvs_show::SecretKey(k)) {
                printf("*** (%u chars)", static_cast<unsigned>(size > 0 ? size - 1 : 0));
                break;
            }
            // On the heap: a string may be up to 4000 bytes.
            char *text = static_cast<char *>(malloc(size));
            if (text == nullptr) {
                printf("(%u chars, no memory)", static_cast<unsigned>(size - 1));
                break;
            }
            err = nvs_get_str(h, k, text, &size);
            if (err == ESP_OK) {
                char shown[nvs_show::kMaxShown + 4];
                nvs_show::Printable(text, shown);
                printf("\"%s\"", shown);
            }
            free(text);
            break;
        }
        case NVS_TYPE_BLOB: {
            size_t size = 0;
            err = nvs_get_blob(h, k, nullptr, &size);
            printf("%u bytes", static_cast<unsigned>(size));
            break;
        }
        default: printf("?"); break;
    }
    if (err != ESP_OK) {
        printf(" (%s)", esp_err_to_name(err));
    }
    nvs_close(h);
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
        printf("%-15s %-15s %-4s ", info.namespace_name, info.key, TypeName(info.type));
        PrintValue(info);
        printf("\n");
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
