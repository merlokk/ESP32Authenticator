#include "server_cmd.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "server.h"
#include "wifi_manager.h"

namespace console {

namespace {

constexpr time_t kTimeFloor = 1704067200;  // 2024-01-01, rtc::Believable's floor

void PrintStatus() {
    const config::Server &c = config::Get().server;
    const server::Status s = server::Get();
    const time_t now = time(nullptr);
    char ago[40];
    printf("host        %s\n", c.host[0] != '\0' ? c.host : "(none)");
    printf("email       %s\n", c.email[0] != '\0' ? c.email : "(none)");
    printf("registered  %s\n", s.registered ? "yes" : s.has_key ? "no (a key, not taken)" : "no");
    server::Ago(now, s.registered_at, ago, sizeof ago);
    if (s.registered) {
        printf("since       %s\n", ago);
    }
    printf("link        %s",
           s.link == server::Link::kOk       ? "connected"
           : s.link == server::Link::kFailed ? "no answer"
                                             : "not tried since the boot");
    if (s.http_status != 0) {
        printf(" (HTTP %d)", s.http_status);
    }
    printf("\n");
    if (s.has_offset) {
        server::OffsetText(s.offset, ago, sizeof ago);
        printf("clock       %s\n", ago);
    }
    char id[server::kClientIdSize + 1];
    if (server::ClientId(id, sizeof id)) {
        printf("client id   %s\n", id);
    } else if (s.registered) {
        printf("client id   none: register again to sign in\n");
    }
    if (s.token_left > 0) {
        printf("signed in   %lld s left\n", static_cast<long long>(s.token_left));
    } else {
        printf("signed in   no\n");
    }
    server::Ago(now, s.last_sync, ago, sizeof ago);
    printf("status sent %s", ago);
    if (c.status_minutes != 0) {
        printf(" (every %u min)\n", static_cast<unsigned>(c.status_minutes));
    } else {
        printf(" (never: server.statusMinutes 0)\n");
    }
    if (s.update[0] != '\0') {
        printf("update      %s\n", s.update);
    }
    if (s.said[0] != '\0') {
        printf("last        %s%s\n", s.said, s.busy ? " (under way)" : "");
    }
    for (const server::sync::Store store : server::sync::kAll) {
        const server::StoreStatus &t = s.stores[static_cast<size_t>(store)];
        printf("%-11s %s", server::sync::StoreName(store), server::sync::PhaseText(t.phase));
        if (t.local_version != 0) {
            printf(", v%u here", static_cast<unsigned>(t.local_version));
        }
        if (t.server_version != 0) {
            printf(", v%u %s", static_cast<unsigned>(t.server_version),
                   t.other_store ? "in another store" : "on the server");
        }
        if (t.server_at[0] != '\0') {
            printf(" (%s UTC)", t.server_at);
        }
        printf("\n");
    }
    server::Ago(now, s.synced_at, ago, sizeof ago);
    printf("synced      %s", ago);
    if (c.sync_minutes != 0) {
        printf(" (on changes, every %u min)\n", static_cast<unsigned>(c.sync_minutes));
    } else {
        printf(" (never on its own: server.syncMinutes 0)\n");
    }
    if (s.sync_said[0] != '\0') {
        printf("sync said   %s\n", s.sync_said);
    }
    char pub[server::Base64UrlLength(server::kPublicSize) + 1];
    if (server::PublicKey(pub, sizeof pub)) {
        printf("public key  %s\n", pub);
    }
}

bool Ready() {
    if (config::Get().server.host[0] == '\0') {
        printf("no server: config set server.host\n");
        return false;
    }
    if (time(nullptr) < kTimeFloor) {
        printf("the clock is not set (date / Wi-Fi sync): TLS needs it\n");
        return false;
    }
    if (wifimgr::Get().state != wifimgr::State::kOnline) {
        printf("no network: Wi-Fi is not connected\n");
        return false;
    }
    return true;
}

}  // namespace

int CmdServer(int argc, char **argv) {
    if (argc == 1) {
        PrintStatus();
        return 0;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "code") == 0 && argc == 2) {
        uint32_t code = 0;
        if (!server::Code(time(nullptr), &code)) {
            printf("not registered\n");
            return 1;
        }
        char text[8];
        server::CodeText(code, text);
        printf("%s\n", text);
        return 0;
    }
    if (strcmp(sub, "host") == 0 && argc == 3) {
        if (!config::ValidHost(argv[2]) || argv[2][0] == '\0') {
            printf("a host name or IPv4: a-z 0-9 '-' '.'\n");
            return 1;
        }
        snprintf(config::Get().server.host, sizeof config::Get().server.host, "%s", argv[2]);
        const bool saved = config::Save() == ESP_OK;
        printf("%s\n", saved ? "saved" : "in use, but config.json is not saved");
        return saved ? 0 : 1;
    }
    if (strcmp(sub, "check") == 0 && argc == 2) {
        if (!Ready()) {
            return 1;
        }
        const esp_err_t err = server::StartCheck(config::Get().server.host);
        printf("%s\n", err == ESP_OK ? "checking: `server` shows the outcome"
                                     : err == ESP_ERR_INVALID_STATE ? "busy" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "report") == 0 && argc == 2) {
        if (!Ready()) {
            return 1;
        }
        const esp_err_t err = server::StartStatus();
        printf("%s\n", err == ESP_OK                ? "sending the status: `server` shows the outcome"
                       : err == ESP_ERR_NOT_FOUND     ? "not registered (or no client id): register"
                       : err == ESP_ERR_INVALID_STATE ? "busy"
                                                      : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "sync") == 0 && argc == 2) {
        if (!Ready()) {
            return 1;
        }
        const esp_err_t err = server::StartSync();
        printf("%s\n", err == ESP_OK                ? "syncing: `server` shows the outcome"
                       : err == ESP_ERR_NOT_FOUND     ? "not registered (or no client id): register"
                       : err == ESP_ERR_INVALID_STATE ? "busy"
                                                      : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "sync") == 0 && (argc == 4 || argc == 5) &&
        (strcmp(argv[2], "keep") == 0 || strcmp(argv[2], "take") == 0)) {
        const bool passwords = strcmp(argv[3], "passwords") == 0;
        if (!passwords && strcmp(argv[3], "passkeys") != 0) {
            printf("usage: server sync keep|take passwords|passkeys [pin]\n");
            return 1;
        }
        if (!Ready()) {
            return 1;
        }
        const bool take = argv[2][0] == 't';
        const esp_err_t err = server::StartResolve(
            passwords ? server::sync::Store::kVault : server::sync::Store::kFido,
            take ? server::Choice::kTakeServer : server::Choice::kKeepMine,
            argc == 5 ? argv[4] : nullptr);
        printf("%s\n", err == ESP_OK                ? "resolving: `server` shows the outcome"
                       : err == ESP_ERR_NOT_FOUND     ? "not registered"
                       : err == ESP_ERR_INVALID_STATE ? "busy, or that store does not wait"
                                                      : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "register") == 0 && argc <= 3) {
        config::Server &c = config::Get().server;
        if (argc == 3) {
            if (!config::ValidEmail(argv[2]) || argv[2][0] == '\0') {
                printf("not an e-mail address\n");
                return 1;
            }
            snprintf(c.email, sizeof c.email, "%s", argv[2]);
            if (config::Save() != ESP_OK) {
                printf("config.json is not saved\n");
            }
        }
        if (c.email[0] == '\0') {
            printf("usage: server register <email>\n");
            return 1;
        }
        if (!Ready()) {
            return 1;
        }
        const server::Status s = server::Get();
        if (s.has_offset && server::ClockTooFar(s.offset)) {
            printf("the clock is %+lld s off the server's (10 min at most): fix it\n",
                   static_cast<long long>(s.offset));
            return 1;
        }
        const esp_err_t err = server::StartRegister(c.host, c.email);
        printf("%s\n", err == ESP_OK ? "registering: `server` shows the outcome"
                                     : err == ESP_ERR_INVALID_STATE ? "busy" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: server [check | report | sync [keep|take passwords|passkeys [pin]] |\n"
           "               register [email] | code | host <name>]\n");
    return 1;
}

}  // namespace console
