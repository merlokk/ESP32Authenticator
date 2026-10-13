#pragma once

// Which URL names a page, and what it is served as (ported from
// approver-esp32). <cstddef> only: host-tested.
//
// A whitelist of extensions, not a blacklist of names: `/` is index.html,
// otherwise one flat name of [A-Za-z0-9._-] (no second '/', no leading '.',
// nothing percent-decoded) ending in a served extension. Query and fragment
// are ignored.

#include <cstddef>

namespace web {

constexpr size_t kMaxNameLength = 32;

constexpr const char *kIndexName = "index.html";
constexpr const char *kNotFoundName = "404.html";
constexpr const char *kUnauthorisedName = "401.html";

// True only for a query carrying `confirm=reboot` as a whole parameter.
bool ConfirmsReboot(const char *uri);

// The page name for `uri` into `out`; false when it is not one.
bool UriToName(const char *uri, char *out, size_t capacity);

// Content type by extension ("application/octet-stream" otherwise).
const char *ContentType(const char *name);

}  // namespace web
