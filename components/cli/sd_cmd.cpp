#include "sd_cmd.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "sd_card.h"

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

}  // namespace

int CmdSd(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        sd_card::PrintInfo();
        return sd_card::Mounted() ? 0 : 1;
    }
    if (strcmp(argv[1], "ls") == 0 && argc <= 3) {
        return CmdLs(argc == 3 ? argv[2] : "");
    }
    printf("usage: sd [info] | sd ls [dir]\n");
    return 1;
}

}  // namespace console
