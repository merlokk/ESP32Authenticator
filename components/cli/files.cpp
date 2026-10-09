#include "files.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "spiffs_fs.h"

namespace console {

namespace {

// Room for "/spiffs/" plus a SPIFFS object name.
constexpr size_t kPathSize = 64;

// Static, not on the REPL task stack.
uint8_t chunk[513];

bool CheckMounted() {
    if (!spiffs_fs::Mounted()) {
        printf("spiffs not mounted\n");
        return false;
    }
    return true;
}

FILE *OpenArg(int argc, char **argv, char *path) {
    if (argc != 2) {
        printf("usage: %s <file>\n", argv[0]);
        return nullptr;
    }
    if (!CheckMounted()) {
        return nullptr;
    }
    if (!spiffs_fs::FullPath(argv[1], path, kPathSize)) {
        printf("path too long\n");
        return nullptr;
    }
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        printf("cannot open %s\n", path);
    }
    return f;
}

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encodes `len` bytes (a multiple of 3, except for the last call) and prints them.
void PrintBase64(const uint8_t *data, size_t len) {
    char out[5] = {};
    for (size_t i = 0; i < len; i += 3) {
        const size_t left = len - i;
        const uint32_t v = (data[i] << 16) | (left > 1 ? data[i + 1] << 8 : 0) |
                           (left > 2 ? data[i + 2] : 0);
        out[0] = kBase64[(v >> 18) & 0x3f];
        out[1] = kBase64[(v >> 12) & 0x3f];
        out[2] = left > 1 ? kBase64[(v >> 6) & 0x3f] : '=';
        out[3] = left > 2 ? kBase64[v & 0x3f] : '=';
        fputs(out, stdout);
    }
}

}  // namespace

int CmdLs(int, char **) {
    if (!CheckMounted()) {
        return 1;
    }
    // SPIFFS is flat: "dirs" are just '/' inside file names.
    DIR *dir = opendir(spiffs_fs::kBasePath);
    if (dir == nullptr) {
        printf("cannot open %s\n", spiffs_fs::kBasePath);
        return 1;
    }
    unsigned count = 0;
    size_t bytes = 0;
    char path[kPathSize];
    for (dirent *e = readdir(dir); e != nullptr; e = readdir(dir)) {
        struct stat st = {};
        if (!spiffs_fs::FullPath(e->d_name, path, sizeof path) || stat(path, &st) != 0) {
            printf("%10s  %-19s  %s\n", "?", "?", e->d_name);
            continue;
        }
        char when[20] = "-";
        if (st.st_mtime > 0) {
            struct tm tm = {};
            localtime_r(&st.st_mtime, &tm);
            strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
        }
        printf("%10ld  %-19s  %s\n", static_cast<long>(st.st_size), when, e->d_name);
        ++count;
        bytes += st.st_size;
    }
    closedir(dir);

    size_t total = 0;
    size_t used = 0;
    spiffs_fs::Info(&total, &used);
    printf("%u file(s), %u bytes; fs %u of %u bytes used\n", count, static_cast<unsigned>(bytes),
           static_cast<unsigned>(used), static_cast<unsigned>(total));
    return 0;
}

int CmdCat(int argc, char **argv) {
    char path[kPathSize];
    FILE *f = OpenArg(argc, argv, path);
    if (f == nullptr) {
        return 1;
    }
    size_t n = 0;
    bool newline = true;
    while ((n = fread(chunk, 1, sizeof chunk - 1, f)) > 0) {
        fwrite(chunk, 1, n, stdout);
        newline = chunk[n - 1] == '\n';
    }
    fclose(f);
    if (!newline) {
        printf("\n");
    }
    return 0;
}

int CmdCatBase64(int argc, char **argv) {
    char path[kPathSize];
    FILE *f = OpenArg(argc, argv, path);
    if (f == nullptr) {
        return 1;
    }
    // 57 input bytes = one 76-char line; the chunk holds 9 whole lines.
    constexpr size_t kLineBytes = 57;
    constexpr size_t kReadBytes = kLineBytes * 9;
    size_t n = 0;
    while ((n = fread(chunk, 1, kReadBytes, f)) > 0) {
        for (size_t i = 0; i < n; i += kLineBytes) {
            PrintBase64(chunk + i, n - i < kLineBytes ? n - i : kLineBytes);
            fputc('\n', stdout);
        }
    }
    fclose(f);
    return 0;
}

}  // namespace console
