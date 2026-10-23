// Host test entry point: one binary, one Register...Tests() per suite.
// No arguments runs everything; arguments run the suites whose name contains
// one of them (`run.cmd config`).

#include <cstring>

#include "unity.h"

void RegisterConfigTests(void);
void RegisterWifiPolicyTests(void);
void RegisterReachabilityTests(void);
void RegisterWebAuthTests(void);
void RegisterWebPathTests(void);
void RegisterWebSettingsTests(void);
void RegisterWebAdminTests(void);
void RegisterClockTests(void);
void RegisterRtcTests(void);
void RegisterBoardTests(void);
void RegisterTimesyncTests(void);
void RegisterTimezoneTests(void);
void RegisterLedTests(void);
void RegisterButtonTests(void);
void RegisterBatteryTests(void);
void RegisterPinPolicyTests(void);
void RegisterTotpTests(void);
void RegisterStorefileTests(void);
void RegisterVaultTests(void);

void setUp(void) {}

void tearDown(void) {}

namespace {

int filter_count = 0;
char **filters = nullptr;

bool Wanted(const char *suite) {
    if (filter_count == 0) {
        return true;
    }
    for (int i = 0; i < filter_count; ++i) {
        if (std::strstr(suite, filters[i]) != nullptr) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    filter_count = argc - 1;
    filters = argv + 1;

    UNITY_BEGIN();
    if (Wanted("config")) RegisterConfigTests();
    if (Wanted("wifi_policy")) RegisterWifiPolicyTests();
    if (Wanted("reachability")) RegisterReachabilityTests();
    if (Wanted("web_auth")) RegisterWebAuthTests();
    if (Wanted("web_paths")) RegisterWebPathTests();
    if (Wanted("web_settings")) RegisterWebSettingsTests();
    if (Wanted("web_admin")) RegisterWebAdminTests();
    if (Wanted("clock")) RegisterClockTests();
    if (Wanted("rtc")) RegisterRtcTests();
    if (Wanted("boards")) RegisterBoardTests();
    if (Wanted("timesync")) RegisterTimesyncTests();
    if (Wanted("timezone")) RegisterTimezoneTests();
    if (Wanted("led")) RegisterLedTests();
    if (Wanted("buttons")) RegisterButtonTests();
    if (Wanted("battery")) RegisterBatteryTests();
    if (Wanted("pin_policy")) RegisterPinPolicyTests();
    if (Wanted("totp")) RegisterTotpTests();
    if (Wanted("storefile")) RegisterStorefileTests();
    if (Wanted("vault")) RegisterVaultTests();
    return UNITY_END();
}
