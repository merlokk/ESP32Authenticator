#pragma once

// HTTP basic auth over the whole site (ported from approver-esp32).
// <cstddef>/<cstdint> only: host-tested.
//
// - The pair is the switch: both web.user and web.password set = required;
//   either empty = open site.
// - The expected `user:password` is base64-encoded and compared; the header
//   is never decoded (no decoder, no decoder bugs).
// - Fails closed: no header, another scheme, too long, ':' in the user.
// - The comparison runs to the end whatever matches (no early exit).
// Not TLS: the password crosses the network readable.

#include <cstddef>
#include <cstdint>

namespace web {

constexpr const char *kAuthRealm = "esp32-auth";

// user (32) + ':' + password (64) + NUL; tied to config by a static_assert.
constexpr size_t kMaxCredentialSize = 98;
constexpr size_t kMaxEncodedSize = ((kMaxCredentialSize - 1 + 2) / 3) * 4 + 1;
constexpr const char *kAuthScheme = "Basic";

bool AuthRequired(const char *user, const char *password);

// base64("user:password") into `out`. False on a ':' in the user, or when it
// does not fit.
bool BasicCredential(const char *user, const char *password, char *out, size_t capacity);

// True when no credential is required, or `header` (the Authorization value)
// carries the right one. Scheme case-insensitive, surrounding spaces allowed.
bool Authorised(const char *user, const char *password, const char *header);

}  // namespace web
