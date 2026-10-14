#include "totp_cmd.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "clock_math.h"
#include "codes.h"
#include "crypto.h"

namespace console {

int CmdTotp(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: totp <base32 secret> | totp test\n");
        return 1;
    }
    if (strcmp(argv[1], "test") == 0) {
        crypto::Init();
        const esp_err_t err = crypto::TotpSelfTest();
        printf("RFC 6238 SHA-1 vectors: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    const time_t now = time(nullptr);
    if (!rtc::Believable(now)) {
        printf("no time: 'date set' or a sync first\n");
        return 1;
    }
    totp::Codes codes = {};
    const esp_err_t err = totp::Generate(argv[1], now, &codes);
    if (err == ESP_ERR_INVALID_ARG) {
        printf("not a Base32 secret (A-Z, 2-7, up to %u bytes)\n",
               static_cast<unsigned>(totp::kMaxKeyBytes));
        return 1;
    }
    if (err != ESP_OK) {
        printf("totp failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    struct tm local = {};
    localtime_r(&now, &local);
    char text[24];
    strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &local);
    const rtc::DateTime u = rtc::FromUnix(now);
    printf("time       %s (UTC %02u:%02u:%02u), %u s left\n", text, u.hour, u.minute, u.second,
           static_cast<unsigned>(codes.windows.remaining));
    printf("previous   %06lu\n", static_cast<unsigned long>(codes.code[0]));
    printf("current    %06lu\n", static_cast<unsigned long>(codes.code[1]));
    printf("next       %06lu\n", static_cast<unsigned long>(codes.code[2]));
    return 0;
}

}  // namespace console
