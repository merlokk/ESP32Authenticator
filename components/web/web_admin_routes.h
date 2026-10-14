#pragma once

// The password and FIDO2 pages' API (inside the web component). Requests:
// web_admin.h. Every route needs basic auth; the POSTs also web.write and a
// JSON content type.
//
//   GET  /api/vault             state and records: no password, no TOTP secret
//                               (only whether one is set)
//   GET  /api/vault/totp?id=N   the code for the current window, seconds left
//   POST /api/vault/add         {site, user, password, otp}
//   POST /api/vault/set         {id, any of site, user, password, otp}
//   POST /api/vault/delete      {id}
//   POST /api/vault/unlock      {pin}; /api/vault/lock {}
//   GET  /api/fido              store info and credentials
//   POST /api/fido/delete       {credential: 64 hex digits}

#include <cstddef>

#include "esp_err.h"
#include "esp_http_server.h"

namespace web {

esp_err_t RegisterAdminRoutes(httpd_handle_t server);

// web_server.cpp's helpers (one httpd task: the buffers are not re-entered).
namespace detail {
size_t Escape(const char *in, char *out, size_t size);
bool Allowed(httpd_req_t *request);
esp_err_t Unauthorised(httpd_req_t *request);
esp_err_t SendJson(httpd_req_t *request, const char *status, const char *json);
esp_err_t Refuse(httpd_req_t *request, const char *status, const char *why);
bool WritingAllowed();
void CountApi();
// The body into the shared buffer (Body()); false when missing or too large.
bool ReadBody(httpd_req_t *request, size_t *length);
const char *Body();
void WipeBody();
// The shared JSON buffer.
char *Scratch(size_t *size);
}  // namespace detail

}  // namespace web
