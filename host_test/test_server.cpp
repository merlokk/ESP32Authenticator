// The authentication server's pure side (components/server/server_logic.h).

#include <cstring>

#include "server_logic.h"
#include "unity.h"

namespace {

void test_server_state_round_trip(void) {
    server::State in = {};
    in.has_key = true;
    in.registered = true;
    for (size_t i = 0; i < server::kKeySize; ++i) {
        in.key[i] = static_cast<uint8_t>(i + 1);
    }
    for (size_t i = 0; i < server::kTotpSize; ++i) {
        in.totp[i] = static_cast<uint8_t>(0xA0 + i);
    }
    in.registered_at = 1761500000;
    in.last_sync = -2;  // any value goes through
    uint8_t bytes[server::kFileSize];
    server::Encode(in, bytes);
    TEST_ASSERT_EQUAL_MEMORY("SRV2", bytes, 4);
    TEST_ASSERT_EQUAL_HEX8(3, bytes[4]);
    server::State out = {};
    TEST_ASSERT_TRUE(server::Decode(bytes, sizeof bytes, &out));
    TEST_ASSERT_TRUE(out.has_key);
    TEST_ASSERT_TRUE(out.registered);
    TEST_ASSERT_EQUAL_MEMORY(in.key, out.key, server::kKeySize);
    TEST_ASSERT_EQUAL_MEMORY(in.totp, out.totp, server::kTotpSize);
    TEST_ASSERT_TRUE(out.registered_at == in.registered_at);
    TEST_ASSERT_TRUE(out.last_sync == in.last_sync);
}

void test_server_state_refused(void) {
    server::State s = {};
    s.has_key = true;
    uint8_t bytes[server::kFileSize];
    server::Encode(s, bytes);
    server::State out = {};
    out.registered_at = 7;
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes - 1, &out));  // cut
    bytes[0] = 'X';
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));  // not ours
    bytes[0] = 'S';
    bytes[4] = 2;  // registered with no key
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));
    bytes[4] = 4;  // an unknown flag
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));
    TEST_ASSERT_TRUE(out.registered_at == 7);  // left as it was
    bytes[4] = 0;
    TEST_ASSERT_TRUE(server::Decode(bytes, sizeof bytes, &out));
    TEST_ASSERT_FALSE(out.has_key);
}

void test_server_base64url(void) {
    // RFC 4648 10, no padding; '-' and '_' for '+' and '/'.
    const char *in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *want[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    char out[16];
    for (size_t i = 0; i < 7; ++i) {
        const size_t n = std::strlen(in[i]);
        const size_t len = server::Base64Url(reinterpret_cast<const uint8_t *>(in[i]), n, out,
                                             sizeof out);
        TEST_ASSERT_EQUAL_STRING(want[i], out);
        TEST_ASSERT_EQUAL_UINT(std::strlen(want[i]), len);
        TEST_ASSERT_EQUAL_UINT(len, server::Base64UrlLength(n));
    }
    const uint8_t url_bytes[] = {0xFB, 0xFF};
    server::Base64Url(url_bytes, 2, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("-_8", out);
    TEST_ASSERT_EQUAL_UINT(0, server::Base64Url(url_bytes, 2, out, 3));  // no room for the NUL
}

void test_server_base32(void) {
    const char *in[] = {"f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *want[] = {"MY", "MZXQ", "MZXW6", "MZXW6YQ", "MZXW6YTB", "MZXW6YTBOI"};
    char out[16];
    for (size_t i = 0; i < 6; ++i) {
        const size_t n = std::strlen(in[i]);
        TEST_ASSERT_EQUAL_UINT(std::strlen(want[i]),
                               server::Base32(reinterpret_cast<const uint8_t *>(in[i]), n, out,
                                              sizeof out));
        TEST_ASSERT_EQUAL_STRING(want[i], out);
    }
    TEST_ASSERT_EQUAL_UINT(32, server::Base32Length(server::kTotpSize));
}

void test_server_token(void) {
    const server::Claims c = {"a\"b\\c@example.org", "00112233445566778899aabbccddeeff",
                              "aa:bb:cc:dd:ee:ff", "MZXW6YTBOI", "BAAA", 1761500000};
    char payload[server::kPayloadSize];
    const size_t n = server::Payload(c, payload, sizeof payload);
    TEST_ASSERT_EQUAL_STRING(
        R"({"email":"a\"b\\c@example.org","serial":"00112233445566778899aabbccddeeff",)"
        R"("mac":"aa:bb:cc:dd:ee:ff","totp":"MZXW6YTBOI","pub":"BAAA","iat":1761500000})",
        payload);
    TEST_ASSERT_EQUAL_UINT(std::strlen(payload), n);
    TEST_ASSERT_EQUAL_UINT(0, server::Payload(c, payload, 40));  // cut: refused

    char token[server::kTokenSize];
    const size_t input = server::SigningInput("{}", 2, token, sizeof token);
    TEST_ASSERT_EQUAL_STRING("eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.e30", token);
    TEST_ASSERT_EQUAL_UINT(std::strlen(token), input);
    uint8_t signature[server::kSignatureSize] = {};
    const size_t total = server::AppendSignature(token, input, sizeof token, signature);
    TEST_ASSERT_EQUAL_UINT(input + 1 + 86, total);
    TEST_ASSERT_EQUAL_UINT(std::strlen(token), total);
    TEST_ASSERT_EQUAL_CHAR('.', token[input]);

    char url[160];
    TEST_ASSERT_TRUE(server::RegisterUrl("auth.merlokk.com", url, sizeof url));
    TEST_ASSERT_EQUAL_STRING("https://auth.merlokk.com/register", url);
    TEST_ASSERT_FALSE(server::RegisterUrl("", url, sizeof url));
    TEST_ASSERT_FALSE(server::RegisterUrl("auth.merlokk.com", url, 20));
}

void test_server_token_fits(void) {
    // The longest claims: a 64-character e-mail, every one escaped.
    char email[65];
    std::memset(email, '"', 64);
    email[64] = '\0';
    char pub[server::Base64UrlLength(server::kPublicSize) + 1];
    std::memset(pub, 'A', sizeof pub - 1);
    pub[sizeof pub - 1] = '\0';
    const server::Claims c = {email, "00112233445566778899aabbccddeeff", "aa:bb:cc:dd:ee:ff",
                              "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", pub, 4102444800};
    char payload[server::kPayloadSize];
    const size_t n = server::Payload(c, payload, sizeof payload);
    TEST_ASSERT_GREATER_THAN(0, n);
    char token[server::kTokenSize];
    const size_t input = server::SigningInput(payload, n, token, sizeof token);
    TEST_ASSERT_GREATER_THAN(0, input);
    const uint8_t signature[server::kSignatureSize] = {};
    TEST_ASSERT_GREATER_THAN(0, server::AppendSignature(token, input, sizeof token, signature));
}

void test_server_texts(void) {
    char out[24];
    server::Ago(1000, 0, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("never", out);
    server::Ago(1000, 990, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("just now", out);
    server::Ago(100, 990, out, sizeof out);  // the clock behind
    TEST_ASSERT_EQUAL_STRING("just now", out);
    server::Ago(1000 + 5 * 60, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("5 min ago", out);
    server::Ago(1000 + 3 * 3600 + 59, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("3 h ago", out);
    server::Ago(1000 + 47 * 3600, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("47 h ago", out);
    server::Ago(1000 + 3 * 86400, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("3 days ago", out);

    char code[8];
    server::CodeText(123456, code);
    TEST_ASSERT_EQUAL_STRING("123 456", code);
    server::CodeText(42, code);
    TEST_ASSERT_EQUAL_STRING("000 042", code);

    TEST_ASSERT_EQUAL_STRING("not registered", server::Summary(false, server::Link::kOk));
    TEST_ASSERT_EQUAL_STRING("registered", server::Summary(true, server::Link::kNone));
    TEST_ASSERT_EQUAL_STRING("registered, offline", server::Summary(true, server::Link::kFailed));

    char hex[5];
    const uint8_t bytes[] = {0x0F, 0xA0};
    server::Hex(bytes, 2, hex);
    TEST_ASSERT_EQUAL_STRING("0fa0", hex);
}

void test_server_ping(void) {
    char url[64];
    TEST_ASSERT_TRUE(server::PingUrl("auth.merlokk.com", url, sizeof url));
    TEST_ASSERT_EQUAL_STRING("https://auth.merlokk.com/api/device/ping", url);
    TEST_ASSERT_FALSE(server::PingUrl("", url, sizeof url));
    TEST_ASSERT_FALSE(server::PingUrl("auth.merlokk.com", url, 30));

    int64_t t = 0;
    const char ok[] = R"({"status": "ok", "time": 1793128050})";
    TEST_ASSERT_TRUE(server::ParsePing(ok, sizeof ok - 1, &t));
    TEST_ASSERT_TRUE(t == 1793128050);
    const char swapped[] = "{\"time\":\n42,\"status\":\"ok\"}";
    TEST_ASSERT_TRUE(server::ParsePing(swapped, sizeof swapped - 1, &t));
    TEST_ASSERT_TRUE(t == 42);
    t = 7;
    const char *bad[] = {
        R"({"status":"down","time":1})",          // not ok
        R"({"status":"ok"})",                     // no time
        R"({"status":"ok","time":"1"})",          // a string
        R"({"status":"ok","time":-1})",           // negative
        R"({"status":"ok","time":99999999999999999999})",  // too big
        R"(<html>ok</html>)",                     // a web page
        "",
    };
    for (const char *b : bad) {
        TEST_ASSERT_FALSE_MESSAGE(server::ParsePing(b, std::strlen(b), &t), b);
    }
    TEST_ASSERT_TRUE(t == 7);  // left as it was
    const char cut[] = R"({"status":"ok","time":17931)";  // the buffer ended
    TEST_ASSERT_FALSE(server::ParsePing(cut, sizeof cut - 1, &t));
}

void test_server_offset(void) {
    char out[24];
    server::OffsetText(0, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("same", out);
    server::OffsetText(-2, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("same", out);
    server::OffsetText(12, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("12 s ahead", out);
    server::OffsetText(-119, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("119 s behind", out);
    server::OffsetText(-15 * 60, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("15 min behind", out);
    server::OffsetText(3 * 3600, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("3 h ahead", out);
    server::OffsetText(-400 * 86400LL, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("400 days behind", out);

    TEST_ASSERT_FALSE(server::ClockTooFar(600));
    TEST_ASSERT_FALSE(server::ClockTooFar(-600));
    TEST_ASSERT_TRUE(server::ClockTooFar(601));
    TEST_ASSERT_TRUE(server::ClockTooFar(-601));
}

void test_server_client_id(void) {
    server::State in = {};
    in.has_key = true;
    in.registered = true;
    std::strcpy(in.client_id, "01a17ec2-98d3-7bbb-a8de-9aa8e0089116");
    uint8_t bytes[server::kFileSize];
    server::Encode(in, bytes);
    TEST_ASSERT_EQUAL_MEMORY("SRV2", bytes, 4);
    server::State out = {};
    TEST_ASSERT_TRUE(server::Decode(bytes, sizeof bytes, &out));
    TEST_ASSERT_EQUAL_STRING(in.client_id, out.client_id);
    // SRV1 (no client id): registered, the id empty.
    bytes[3] = '1';
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));  // SRV1 with SRV2's length
    TEST_ASSERT_TRUE(server::Decode(bytes, server::kFileSizeV1, &out));
    TEST_ASSERT_TRUE(out.registered);
    TEST_ASSERT_EQUAL_STRING("", out.client_id);
    bytes[3] = '2';
    bytes[server::kFileSizeV1 + 8] = 'x';  // not a UUID
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));
    bytes[server::kFileSizeV1 + 8] = '-';
    bytes[server::kFileSizeV1] = 0;  // empty, then something: not padding
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));

    TEST_ASSERT_TRUE(server::ValidClientId("01A17EC2-98d3-7bbb-a8de-9aa8e0089116", 36));
    TEST_ASSERT_FALSE(server::ValidClientId("01a17ec2-98d3-7bbb-a8de-9aa8e008911", 35));
    TEST_ASSERT_FALSE(server::ValidClientId("01a17ec2x98d3-7bbb-a8de-9aa8e0089116", 36));
}

void test_server_answers(void) {
    char out[64];
    const char reg[] = R"({"deviceId":"01a17ec2-98d3-7bbb-a8de-9aa8e0089116",)"
                       R"("clientId":"01a17ec2-98d3-7bbb-a8de-9aa8e0089116"})";
    TEST_ASSERT_EQUAL_INT(36, server::GetString(reg, sizeof reg - 1, "clientId", out, sizeof out));
    TEST_ASSERT_EQUAL_INT(-1, server::GetString(reg, sizeof reg - 1, "clientId", out, 36));  // no room
    TEST_ASSERT_EQUAL_INT(-1, server::GetString(reg, sizeof reg - 1, "nope", out, sizeof out));
    const char escaped[] = R"({"a":"x\"y"})";
    TEST_ASSERT_EQUAL_INT(-1, server::GetString(escaped, sizeof escaped - 1, "a", out, sizeof out));

    const char problem[] = R"({"type":"urn:authserver:problem:device-registration.stale",)"
                           R"("title":"Stale","status":400})";
    TEST_ASSERT_TRUE(server::ErrorCode(problem, sizeof problem - 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("device-registration.stale", out);
    const char oauth[] = R"({"error":"invalid_client","error_description":"x"})";
    TEST_ASSERT_TRUE(server::ErrorCode(oauth, sizeof oauth - 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("invalid_client", out);
    const char blank[] = R"({"type":"about:blank","status":502})";
    TEST_ASSERT_TRUE(server::ErrorCode(blank, sizeof blank - 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("about:blank", out);
    TEST_ASSERT_FALSE(server::ErrorCode("<html>", 6, out, sizeof out));

    char text[48];
    server::RefusalText(400, "device-registration.stale", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("refused: the clock is off", text);
    server::RefusalText(403, "device-registration.refused", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("refused: no open window", text);
    server::RefusalText(409, "device.taken", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("on another account", text);
    server::RefusalText(502, "about:blank", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("the server's backend is down", text);
    server::RefusalText(418, "teapot", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("the server said 418: teapot", text);
    server::RefusalText(500, "", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("the server said 500", text);
    server::RefusalText(400, "device-registration.stalex", text, sizeof text);  // not a prefix match
    TEST_ASSERT_EQUAL_STRING("the server said 400: device-registration.stalex", text);
}

void test_server_sign_in(void) {
    const char *id = "01a17ec2-98d3-7bbb-a8de-9aa8e0089116";
    char payload[384];
    const size_t n = server::AssertionPayload(id, "auth.merlokk.com", "00ff", 1793128050, payload,
                                              sizeof payload);
    TEST_ASSERT_EQUAL_STRING(
        R"({"iss":"01a17ec2-98d3-7bbb-a8de-9aa8e0089116","sub":"01a17ec2-98d3-7bbb-a8de-9aa8e0089116",)"
        R"("aud":"https://auth.merlokk.com/","jti":"00ff","iat":1793128050,"exp":1793128350})",
        payload);
    TEST_ASSERT_EQUAL_UINT(std::strlen(payload), n);
    // The longest host still fits.
    char host[64];
    std::memset(host, 'a', 63);
    host[63] = '\0';
    char jti[33];
    std::memset(jti, 'f', 32);
    jti[32] = '\0';
    TEST_ASSERT_GREATER_THAN(0, server::AssertionPayload(id, host, jti, 4102444800, payload,
                                                         sizeof payload));

    char token[server::kTokenSize];
    const size_t input = server::SigningInput(server::kAssertionHeader,
                                              sizeof server::kAssertionHeader - 1, "{}", 2, token,
                                              sizeof token);
    TEST_ASSERT_EQUAL_STRING("eyJhbGciOiJFUzI1NiIsInR5cCI6ImNsaWVudC1hdXRoZW50aWNhdGlvbitqd3QifQ.e30",
                             token);
    TEST_ASSERT_EQUAL_UINT(std::strlen(token), input);

    char form[256];
    TEST_ASSERT_GREATER_THAN(0, server::TokenForm(id, "a.b.c", form, sizeof form));
    TEST_ASSERT_EQUAL_STRING(
        "grant_type=client_credentials&client_id=01a17ec2-98d3-7bbb-a8de-9aa8e0089116"
        "&client_assertion_type=urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%3Ajwt-bearer"
        "&client_assertion=a.b.c",
        form);
    TEST_ASSERT_EQUAL_UINT(0, server::TokenForm(id, "a.b.c", form, 40));

    char url[64];
    TEST_ASSERT_TRUE(server::Url("h.example", "/connect/token", url, sizeof url));
    TEST_ASSERT_EQUAL_STRING("https://h.example/connect/token", url);

    char access[16];
    int64_t expires = 0;
    const char ok[] = R"({"access_token":"eyJabc","token_type":"Bearer","expires_in":3600})";
    TEST_ASSERT_TRUE(server::ParseToken(ok, sizeof ok - 1, access, sizeof access, &expires));
    TEST_ASSERT_EQUAL_STRING("eyJabc", access);
    TEST_ASSERT_TRUE(expires == 3600);
    TEST_ASSERT_FALSE(server::ParseToken(ok, sizeof ok - 1, access, 4, &expires));  // too long
    const char other[] = R"({"access_token":"x","token_type":"mac","expires_in":1})";
    TEST_ASSERT_FALSE(server::ParseToken(other, sizeof other - 1, access, sizeof access, &expires));
}

void test_server_status(void) {
    const server::Report r = {"0.5.0-174-g2f84ffa", "x4pro", "aa:bb:cc:dd:ee:ff", true, false, 87,
                              3600, 1793128050};
    char body[server::kStatusBodySize];
    const size_t n = server::StatusBody(r, body, sizeof body);
    TEST_ASSERT_EQUAL_STRING(
        R"({"firmware":"0.5.0-174-g2f84ffa","board":"x4pro","mac":"aa:bb:cc:dd:ee:ff",)"
        R"("wifi":true,"ble":false,"battery":87,"uptime":3600,"time":1793128050})",
        body);
    TEST_ASSERT_EQUAL_UINT(std::strlen(body), n);
    server::Report none = r;
    none.battery = -1;
    none.time = 0;
    server::StatusBody(none, body, sizeof body);
    TEST_ASSERT_NULL(std::strstr(body, "battery"));
    TEST_ASSERT_NOT_NULL(std::strstr(body, R"("time":0})"));
    // The longest firmware version fits.
    char version[33];
    std::memset(version, 'v', 32);
    version[32] = '\0';
    none.firmware = version;
    TEST_ASSERT_GREATER_THAN(0, server::StatusBody(none, body, sizeof body));

    char update[server::kVersionSize];
    int64_t t = 0;
    const char offered[] = R"({"update": {"id": "01a17f4a", "version": "0.6.0-0-gabc1234",)"
                           R"( "size": 1843200, "sha256": "9f2c"}, "time": 1793128051})";
    TEST_ASSERT_TRUE(server::ParseStatus(offered, sizeof offered - 1, update, sizeof update, &t));
    TEST_ASSERT_EQUAL_STRING("0.6.0-0-gabc1234", update);
    TEST_ASSERT_TRUE(t == 1793128051);
    const char current[] = R"({"update":null,"time":1793128051})";
    TEST_ASSERT_TRUE(server::ParseStatus(current, sizeof current - 1, update, sizeof update, &t));
    TEST_ASSERT_EQUAL_STRING("", update);
    const char bad[] = R"({"time":1})";
    TEST_ASSERT_FALSE(server::ParseStatus(bad, sizeof bad - 1, update, sizeof update, &t));
}

void test_server_status_due(void) {
    // Not ready, or never: nothing.
    TEST_ASSERT_FALSE(server::StatusDue(false, 10, 100, -1, -1));
    TEST_ASSERT_FALSE(server::StatusDue(true, 0, 100, -1, -1));
    // The first as soon as ready.
    TEST_ASSERT_TRUE(server::StatusDue(true, 10, 5, -1, -1));
    // Sent at 100: the next at 700.
    TEST_ASSERT_FALSE(server::StatusDue(true, 10, 699, 100, 100));
    TEST_ASSERT_TRUE(server::StatusDue(true, 10, 700, 100, 100));
    // Failed at 100 (sent at 0, or never): again in a minute.
    TEST_ASSERT_FALSE(server::StatusDue(true, 10, 159, 100, 0));
    TEST_ASSERT_TRUE(server::StatusDue(true, 10, 160, 100, -1));
    // Every minute: the retry is no longer than the interval.
    TEST_ASSERT_TRUE(server::StatusDue(true, 1, 160, 100, 100));
}

}  // namespace

void RegisterServerTests(void) {
    RUN_TEST(test_server_client_id);
    RUN_TEST(test_server_answers);
    RUN_TEST(test_server_sign_in);
    RUN_TEST(test_server_status);
    RUN_TEST(test_server_status_due);
    RUN_TEST(test_server_ping);
    RUN_TEST(test_server_offset);
    RUN_TEST(test_server_state_round_trip);
    RUN_TEST(test_server_state_refused);
    RUN_TEST(test_server_base64url);
    RUN_TEST(test_server_base32);
    RUN_TEST(test_server_token);
    RUN_TEST(test_server_token_fits);
    RUN_TEST(test_server_texts);
}
