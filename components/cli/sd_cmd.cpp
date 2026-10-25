#include "sd_cmd.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "files.h"
#include "ota_update.h"
#include "sd_card.h"
#include "wallpaper_logic.h"

namespace console {

namespace {

constexpr size_t kPathSize = 300;

int CmdLs(const char *rel) {
    if (!sd_card::Mounted()) {
        printf("sd card    %s\n", sd_card::Name(sd_card::Status()));
        return 1;
    }
    // Static, not on the console task's stack (4 KB).
    static char dir_path[kPathSize];
    static char path[kPathSize];
    if (!sd_card::JoinPath(sd_card::kBasePath, rel, dir_path, sizeof dir_path)) {
        printf("bad path '%s'\n", rel);
        return 1;
    }
    DIR *dir = opendir(dir_path);
    if (dir == nullptr) {
        printf("cannot open %s\n", dir_path);
        return 1;
    }
    unsigned files = 0;
    unsigned dirs = 0;
    uint64_t bytes = 0;
    while (const dirent *e = readdir(dir)) {
        struct stat st = {};
        const int n = snprintf(path, sizeof path, "%s/%s", dir_path, e->d_name);
        const bool have = n > 0 && static_cast<size_t>(n) < sizeof path && stat(path, &st) == 0;
        char when[20] = "?";
        if (have) {
            struct tm local = {};
            localtime_r(&st.st_mtime, &local);
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", &local);
        }
        if (e->d_type == DT_DIR) {
            ++dirs;
            printf("dir  %10s  %s  %s/\n", "", when, e->d_name);
        } else {
            ++files;
            bytes += have ? static_cast<uint64_t>(st.st_size) : 0;
            char size[16] = "?";
            if (have) {
                sd_card::FormatBytes(static_cast<uint64_t>(st.st_size), size, sizeof size);
            }
            printf("file %10s  %s  %s\n", size, when, e->d_name);
        }
    }
    closedir(dir);
    char total[16];
    sd_card::FormatBytes(bytes, total, sizeof total);
    printf("%u dir(s), %u file(s), %s in %s\n", dirs, files, total, dir_path);
    return 0;
}

constexpr const char *kFirmwareName = "firmware.bin";
constexpr size_t kMaxFirmwareBytes = 8 * 1024 * 1024;  // an OTA slot is a little less

// Writes and deletes only wallpapers (plain *.png names in /sdcard/wallpapers)
// and firmware.bin in the root (ota sd): the rest of the card, the store files
// above all, stays read-only here.
bool WallpaperPath(const char *name, char *path, size_t size) {
    if (!sd_card::Mounted()) {
        printf("sd card    %s\n", sd_card::Name(sd_card::Status()));
        return false;
    }
    if (strcmp(name, kFirmwareName) == 0) {
        snprintf(path, size, "%s", ota::kSdFile);
        return true;
    }
    if (!ui::wallpaper::IsImageName(name)) {
        printf("name: a plain *.png name (written to %s), or %s\n", ui::wallpaper::kDir,
               kFirmwareName);
        return false;
    }
    const int n = snprintf(path, size, "%s/%s", ui::wallpaper::kDir, name);
    if (n < 0 || static_cast<size_t>(n) >= size) {
        printf("name too long\n");
        return false;
    }
    return true;
}

int CmdPut(const char *name, size_t length, uint32_t crc) {
    static char path[kPathSize];
    if (!WallpaperPath(name, path, sizeof path)) {
        return 1;
    }
    if (strcmp(name, kFirmwareName) == 0) {
        if (length > kMaxFirmwareBytes) {
            printf("over %u MB: larger than an OTA slot\n",
                   static_cast<unsigned>(kMaxFirmwareBytes / (1024 * 1024)));
            return 1;
        }
        return ReceiveFile(path, length, crc, true);
    }
    if (length > ui::wallpaper::kMaxFileBytes) {
        printf("over %u KB: the device reads a wallpaper whole\n",
               static_cast<unsigned>(ui::wallpaper::kMaxFileBytes / 1024));
        return 1;
    }
    struct stat st = {};
    if (stat(ui::wallpaper::kDir, &st) != 0 && mkdir(ui::wallpaper::kDir, 0775) != 0) {
        printf("cannot create %s\n", ui::wallpaper::kDir);
        return 1;
    }
    return ReceiveFile(path, length, crc, true);
}

int CmdRm(const char *name) {
    static char path[kPathSize];
    if (!WallpaperPath(name, path, sizeof path)) {
        return 1;
    }
    if (remove(path) != 0) {
        printf("cannot remove %s\n", path);
        return 1;
    }
    printf("removed %s\n", path);
    return 0;
}

}  // namespace

int CmdSd(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        sd_card::PrintInfo();
        return sd_card::Mounted() ? 0 : 1;
    }
    if (strcmp(argv[1], "ls") == 0 && argc <= 3) {
        return CmdLs(argc == 3 ? argv[2] : "");
    }
    if (strcmp(argv[1], "put") == 0 && argc == 5) {
        return CmdPut(argv[2], strtoul(argv[3], nullptr, 0), strtoul(argv[4], nullptr, 16));
    }
    if (strcmp(argv[1], "rm") == 0 && argc == 3) {
        return CmdRm(argv[2]);
    }
    printf("usage: sd [info] | sd ls [dir] | sd put <name.png|firmware.bin> <length> <crc32> | "
           "sd rm <name.png|firmware.bin>\n");
    return 1;
}

}  // namespace console
