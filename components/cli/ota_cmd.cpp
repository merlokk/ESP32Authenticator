#include "ota_cmd.h"

#include <cstdio>
#include <cstring>

#include "ota_update.h"
#include "subcommand.h"

namespace console {

namespace {

// Static, not on the console task's stack (4 KB).
ota::Status g_status;
ota::SdFile g_sd;

void PrintSdFile() {
    ota::ReadSdFile(&g_sd);
    if (!g_sd.mounted) {
        printf("sd file    no card\n");
        return;
    }
    if (!g_sd.present) {
        printf("sd file    no %s\n", ota::kSdFile);
        return;
    }
    const bool ok = g_sd.verdict == ota::Verdict::kMore;
    printf("sd file    %s, %u bytes, %s %s %s\n", ota::kSdFile, static_cast<unsigned>(g_sd.size),
           g_sd.info.version, g_sd.info.date, g_sd.info.time);
    printf("sd check   %s%s\n", ok ? "ok (the rest is checked while writing)" : ota::VerdictName(g_sd.verdict),
           g_sd.same_as_running ? ", the running firmware" : "");
}

int CmdInfo(int, char **) {
    ota::GetStatus(&g_status);
    printf("running    %s, %s (%s %s, IDF %s)\n", g_status.running_slot, g_status.running_version,
           g_status.running_date, g_status.running_time, g_status.running_idf);
    if (g_status.trial) {
        printf("state      on trial: confirmed in %u s, rolled back on a reset before\n",
               static_cast<unsigned>(g_status.confirm_in_s));
    } else {
        printf("state      %s\n", g_status.running_state);
    }
    printf("next slot  %s, %u bytes\n", g_status.next_slot, static_cast<unsigned>(g_status.next_size));
    if (g_status.rolled_back[0] != '\0') {
        printf("rolled back %s (did not confirm itself)\n", g_status.rolled_back);
    }
    printf("job        %s", ota::JobName(g_status.job));
    if (g_status.job != ota::Job::kIdle) {
        printf(" %s from %s, %u of %u bytes", g_status.job_version, ota::SourceName(g_status.source),
               static_cast<unsigned>(g_status.written), static_cast<unsigned>(g_status.total));
    }
    if (g_status.error[0] != '\0') {
        printf(": %s", g_status.error);
    }
    printf("\n");
    PrintSdFile();
    return 0;
}

int CmdFromSd(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
        PrintSdFile();
        printf("`ota sd confirm` writes it into the next slot and restarts\n");
        return 0;
    }
    const char *refusal = ota::Refusal();
    if (refusal != nullptr) {
        printf("not now: %s\n", refusal);
        return 1;
    }
    ota::ReadSdFile(&g_sd);
    if (!g_sd.present || g_sd.verdict != ota::Verdict::kMore) {
        PrintSdFile();
        return 1;
    }
    const esp_err_t err = ota::StartFromSd();
    if (err != ESP_OK) {
        printf("not started: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("writing %s in the background (`ota` shows the progress); restarts when done\n",
           g_sd.info.version);
    return 0;
}

}  // namespace

int CmdOta(int argc, char **argv) {
    static const Subcommand kSubs[] = {
        {"info", CmdInfo},
        {"sd", CmdFromSd},
    };
    return Dispatch(kSubs, argc, argv, "ota [info | sd [confirm]]");
}

}  // namespace console
