#include "files.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "driver/usb_serial_jtag.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
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
        printf("usage: spiffs %s <file>\n", argv[0]);
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

int CmdFormat(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        printf("erases every file on the '%s' partition (on the X4 Pro: stock data).\n"
               "run 'spiffs format confirm' to proceed\n",
               spiffs_fs::kPartitionLabel);
        return 1;
    }
    const esp_err_t err = spiffs_fs::Format();
    if (err != ESP_OK) {
        printf("format failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    size_t total = 0;
    size_t used = 0;
    spiffs_fs::Info(&total, &used);
    printf("formatted, %u of %u bytes used\n", static_cast<unsigned>(used),
           static_cast<unsigned>(total));
    return 0;
}

// Base64 alphabet value of `c`, -1 if not in the alphabet.
int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Streaming base64 decoder writing into a file, with length and CRC32.
class Base64Writer {
   public:
    explicit Base64Writer(FILE *f) : f_(f) {}

    // Feeds one base64 character. Returns false on a malformed stream.
    bool Put(char c) {
        if (c == '=') {
            if (n_ < 2) return false;
            ++pad_;
            quad_[n_++] = 0;
        } else {
            const int v = Base64Value(c);
            if (v < 0 || pad_ > 0) return false;  // no data after padding
            quad_[n_++] = static_cast<uint8_t>(v);
        }
        if (n_ == 4) {
            const uint32_t v = (quad_[0] << 18) | (quad_[1] << 12) | (quad_[2] << 6) | quad_[3];
            const uint8_t bytes[3] = {static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                                      static_cast<uint8_t>(v)};
            if (!Emit(bytes, 3 - pad_)) return false;
            n_ = 0;
        }
        return true;
    }

    // Flushes buffered bytes. Returns false on a truncated stream or write error.
    bool Finish() { return n_ == 0 && Flush(); }

    size_t length() const { return length_; }
    uint32_t crc() const { return crc_; }

   private:
    bool Emit(const uint8_t *bytes, int count) {
        for (int i = 0; i < count; ++i) {
            if (used_ == sizeof chunk && !Flush()) return false;
            chunk[used_++] = bytes[i];
        }
        return true;
    }

    bool Flush() {
        if (used_ == 0) return true;
        crc_ = esp_rom_crc32_le(crc_, chunk, used_);
        length_ += used_;
        const bool ok = fwrite(chunk, 1, used_, f_) == used_;
        used_ = 0;
        return ok;
    }

    FILE *f_;
    uint8_t quad_[4] = {};
    int n_ = 0;
    int pad_ = 0;
    size_t used_ = 0;
    size_t length_ = 0;
    uint32_t crc_ = 0;  // same as zlib.crc32()
};

// No data for this long aborts `spiffs write`.
constexpr int kWriteIdleTimeoutS = 10;

// Flow control: the USB Serial/JTAG driver drops input once its 256-byte RX
// ring buffer is full, and SPIFFS writes are slow. So after every kAckBlock
// base64 chars consumed the device prints '.', and a sender must not have more
// than one block in flight (utils/spiffs.py waits for each '.').
constexpr int kAckBlock = 192;

// Reads one base64 line from the console into `writer`, in small chunks.
// Leading CR/LF (left over from the command line) are skipped; the next CR/LF
// ends the file. Reads the USB Serial/JTAG driver directly to get an idle
// timeout. Sets `acked` if any '.' was printed.
// Returns nullptr on success, or an error text.
const char *ReceiveBase64Line(Base64Writer &writer, bool *acked) {
    char in[64];
    bool started = false;
    int block = 0;
    while (true) {
        const int n = usb_serial_jtag_read_bytes(in, sizeof in,
                                                 pdMS_TO_TICKS(kWriteIdleTimeoutS * 1000));
        if (n <= 0) {
            return "timeout waiting for data";
        }
        for (int i = 0; i < n; ++i) {
            const char c = in[i];
            if (c == '\r' || c == '\n') {
                if (started) {
                    return writer.Finish() ? nullptr : "truncated base64 or write error";
                }
                continue;
            }
            started = true;
            if (!writer.Put(c)) {
                return "bad base64 or write error";
            }
            if (++block == kAckBlock) {
                block = 0;
                *acked = true;
                fputc('.', stdout);
                fflush(stdout);
            }
        }
    }
}

// spiffs write <file> [<length> <crc32>]; commas work as separators too.
int CmdWrite(int argc, char **argv) {
    static char args[128];
    args[0] = '\0';
    for (int i = 1; i < argc; ++i) {
        strlcat(args, argv[i], sizeof args);
        strlcat(args, ",", sizeof args);
    }
    const char *tokens[3] = {};
    int count = 0;
    char *save = nullptr;
    for (char *t = strtok_r(args, ",", &save); t != nullptr && count < 3;
         t = strtok_r(nullptr, ",", &save)) {
        tokens[count++] = t;
    }
    if (count != 1 && count != 3) {
        printf("usage: spiffs write <file> [<length> <crc32>]\n");
        return 1;
    }
    const bool check = count == 3;
    const size_t want_length = check ? strtoul(tokens[1], nullptr, 0) : 0;
    const uint32_t want_crc = check ? strtoul(tokens[2], nullptr, 16) : 0;

    if (!CheckMounted()) {
        return 1;
    }
    char path[kPathSize];
    if (!spiffs_fs::FullPath(tokens[0], path, sizeof path)) {
        printf("path too long\n");
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        printf("cannot create %s\n", path);
        return 1;
    }

    printf("ready: send base64, end with Enter\n");
    fflush(stdout);
    Base64Writer writer(f);
    bool acked = false;
    const char *error = ReceiveBase64Line(writer, &acked);
    fclose(f);
    if (acked) {
        printf("\n");  // end the line of '.' acks
    }

    if (error == nullptr && check &&
        (writer.length() != want_length || writer.crc() != want_crc)) {
        error = "length/crc32 mismatch";
    }
    if (error != nullptr) {
        remove(path);
        printf("error: %s (got %u bytes, crc32 %08" PRIx32 "), file removed\n", error,
               static_cast<unsigned>(writer.length()), writer.crc());
        return 1;
    }
    printf("ok %u bytes, crc32 %08" PRIx32 "\n", static_cast<unsigned>(writer.length()),
           writer.crc());
    return 0;
}

struct Subcommand {
    const char *name;
    int (*func)(int, char **);
};

constexpr Subcommand kSubcommands[] = {
    {"ls", &CmdLs},
    {"cat", &CmdCat},
    {"catbase64", &CmdCatBase64},
    {"write", &CmdWrite},
    {"format", &CmdFormat},
};

}  // namespace

int CmdSpiffs(int argc, char **argv) {
    if (argc >= 2) {
        for (const Subcommand &sub : kSubcommands) {
            if (strcmp(argv[1], sub.name) == 0) {
                return sub.func(argc - 1, argv + 1);
            }
        }
    }
    printf("usage: spiffs ls | cat <file> | catbase64 <file> |"
           " write <file> [<length> <crc32>] | format confirm\n");
    return 1;
}

}  // namespace console
