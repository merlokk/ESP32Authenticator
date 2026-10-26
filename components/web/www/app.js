// The stylesheet, the helpers and every page's script in one file, fetched
// after the page (no <link>, one request at a time). Ported from approver-esp32.
(function () {
  "use strict";

  var CSS = [
    ":root{color-scheme:dark;--bg:#0b0d0c;--plate:#141816;--live:#1a221d;",
    "--line:#232825;--text:#d6ded8;--faint:#7a827e;--dim:#4e5451;",
    "--green:#45c46a;--amber:#d69e2e;--red:#e05252}",
    "*{box-sizing:border-box}",
    "body{margin:0;padding:0 16px 40px;background:var(--bg);color:var(--text);",
    "font:16px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;",
    "-webkit-text-size-adjust:100%}",
    ".wrap{max-width:560px;margin:0 auto}",
    "header{display:flex;align-items:baseline;gap:10px;padding:20px 0 14px}",
    "h1{font-size:19px;font-weight:600;margin:0}",
    ".lamp{margin-left:auto;display:inline-flex;align-items:center;gap:7px;",
    "font-size:13px;color:var(--faint);white-space:nowrap}",
    ".lamp::before{content:'';width:9px;height:9px;border-radius:50%;background:var(--dim)}",
    ".lamp.ok::before{background:var(--green)}",
    ".lamp.warn::before{background:var(--amber)}",
    ".lamp.bad::before{background:var(--red)}",
    "nav{display:flex;flex-direction:column;gap:10px;margin:4px 0 26px}",
    "a.btn,button.btn{display:flex;align-items:center;gap:12px;width:100%;",
    "min-height:58px;padding:14px 18px;border:1px solid var(--line);border-radius:12px;",
    "background:var(--plate);color:var(--text);font:inherit;font-weight:500;",
    "text-align:left;text-decoration:none;cursor:pointer;-webkit-tap-highlight-color:transparent}",
    "a.btn:active,button.btn:active{background:var(--live)}",
    "a.btn .sub,button.btn .sub{display:block;font-size:13px;font-weight:400;color:var(--faint)}",
    ".grow{flex:1;min-width:0}.chev{color:var(--dim)}",
    ".btn.warn{border-color:#3a2f14}.btn.warn .name{color:var(--amber)}",
    "button:disabled{opacity:.5;cursor:default}",
    "h2{font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.08em;",
    "color:var(--faint);margin:26px 0 8px}",
    "table{border-collapse:collapse;width:100%}",
    "th,td{text-align:left;padding:9px 0;border-bottom:1px solid var(--line);",
    "font-size:15px;vertical-align:top}",
    "th{color:var(--faint);font-weight:400;width:45%}",
    "td{font-variant-numeric:tabular-nums;overflow-wrap:anywhere}",
    "tr:last-child th,tr:last-child td{border-bottom:0}",
    ".ok{color:var(--green)}.warn{color:var(--amber)}.bad{color:var(--red)}",
    ".faint{color:var(--faint)}",
    ".meter{height:6px;margin-top:6px;border-radius:3px;background:var(--line);overflow:hidden}",
    ".meter>i{display:block;height:100%;background:var(--green)}",
    ".meter>i.warn{background:var(--amber)}.meter>i.bad{background:var(--red)}",
    "pre{margin:0;padding:14px;border:1px solid var(--line);border-radius:10px;",
    "background:#0e1110;color:#a8b0aa;font-size:12px;line-height:1.45;",
    "overflow-x:auto;white-space:pre}",
    "p.lead{margin:0 0 20px;color:var(--faint);font-size:14px}",
    "footer{margin-top:30px;padding-top:14px;border-top:1px solid var(--line);",
    "color:var(--dim);font-size:13px}",
    ".notice{margin:0 0 18px;padding:12px 14px;border:1px solid var(--line);",
    "border-left:3px solid var(--amber);border-radius:8px;background:var(--plate);",
    "font-size:14px;color:var(--text)}",
    ".notice.bad{border-left-color:var(--red)}.notice.ok{border-left-color:var(--green)}",
    ".center{text-align:center}",
    ".big{font-size:64px;line-height:1;margin:40px 0 10px;color:var(--dim)}",
    ".hint{margin:0 0 12px;color:var(--faint);font-size:13px}",
    ".hint code,td code{color:var(--text)}",
    ".field{display:flex;align-items:center;gap:10px;margin:0 0 10px}",
    ".field label{flex:0 0 88px;color:var(--faint);font-size:14px}",
    ".field input{flex:1;min-width:0;min-height:48px;padding:10px 12px;",
    "border:1px solid var(--line);border-radius:10px;background:#0e1110;",
    "color:var(--text);font:inherit;font-size:16px}",
    ".field input:focus{outline:none;border-color:#3a4d42}",
    ".field input:disabled{opacity:.5}",
    ".field input[type=range]{padding:0;border:0;background:none;accent-color:var(--green)}",
    ".field .btn{flex:0 0 auto;width:auto}",
    ".field .value{flex:0 0 48px;text-align:right;color:var(--faint);font-size:14px;",
    "font-variant-numeric:tabular-nums}",
    "button.reveal{flex:0 0 auto;min-height:48px;padding:0 12px;border:1px solid var(--line);",
    "border-radius:10px;background:var(--plate);color:var(--faint);font:inherit;",
    "font-size:13px;cursor:pointer}",
    ".seg{display:flex;gap:8px;margin:0 0 12px}",
    ".seg button{flex:1;min-width:0;min-height:52px;padding:8px 4px;border:1px solid var(--line);",
    "border-radius:10px;background:var(--plate);color:var(--faint);font:inherit;",
    "font-size:14px;cursor:pointer}",
    ".seg button.on{background:#1c2b22;border-color:#3a5c46;color:var(--text);font-weight:600}",
    ".card{padding:12px;margin:0 0 10px;border:1px solid var(--line);",
    "border-radius:12px;background:var(--plate)}",
    ".card .field:last-child{margin-bottom:0}",
    ".card-head{display:flex;align-items:center;gap:8px;margin:0 0 10px;color:var(--faint);font-size:13px}",
    ".card-head button.drop~button.drop{margin-left:0}",
    "button.drop{margin-left:auto;width:40px;height:40px;border:1px solid var(--line);",
    "border-radius:10px;background:transparent;color:var(--faint);font-size:20px;",
    "line-height:1;cursor:pointer}",
    "button.drop.move{width:34px}",
    ".row-actions{display:flex;gap:10px;margin:0 0 14px}",
    ".btn.small{min-height:48px;padding:10px 14px;font-size:14px;font-weight:400}",
    ".row-actions .btn.small{flex:1;justify-content:center}",
    ".sticky{display:flex;flex-direction:column;gap:10px;margin:22px 0 0;",
    "padding:14px 0 0;border-top:1px solid var(--line)}",
    ".sticky .btn{justify-content:center}",
    "#scan-list{display:flex;flex-direction:column;gap:8px;margin:0 0 14px}",
    "#scan-list .btn.small .sub{display:inline;margin-left:10px}",
    ".who{flex:1;min-width:0;color:var(--text);font-size:15px;overflow-wrap:anywhere}",
    ".tags{display:flex;flex-wrap:wrap;gap:4px 14px;margin:0 0 10px;font-size:13px}",
    ".badge{margin-right:6px;padding:0 6px;border:1px solid var(--amber);border-radius:6px;",
    "color:var(--amber);font-size:12px;font-weight:600;letter-spacing:.04em}",
    ".card .row-actions{margin:0}.card .row-actions .btn.small{min-height:42px}",
    ".edit{margin-top:12px;padding-top:12px;border-top:1px solid var(--line)}",
    ".totp:not(:empty){margin-top:10px}",
    ".code{font-size:28px;font-weight:600;letter-spacing:.08em;color:var(--green);",
    "font-variant-numeric:tabular-nums}"
  ].join("");

  var style = document.createElement("style");
  style.textContent = CSS;
  document.head.appendChild(style);

  var by = function (id) { return document.getElementById(id); };

  // Unix seconds as the device's local time (UTC shifted by its zone offset).
  function localTime(utc, offset) {
    return new Date((utc + offset) * 1000).toISOString().slice(0, 19).replace("T", " ");
  }

  function bytes(n) {
    if (typeof n !== "number" || !isFinite(n)) return "-";
    if (n < 1024) return n + " B";
    if (n < 1048576) return (n / 1024).toFixed(1) + " KB";
    if (n < 1073741824) return (n / 1048576).toFixed(2) + " MB";
    return (n / 1073741824).toFixed(2) + " GB";
  }

  function duration(seconds) {
    if (typeof seconds !== "number" || !isFinite(seconds)) return "-";
    var s = Math.floor(seconds), d = Math.floor(s / 86400);
    var h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    if (d > 0) return d + "d " + h + "h " + m + "m";
    if (h > 0) return h + "h " + m + "m";
    if (m > 0) return m + "m " + (s % 60) + "s";
    return s + "s";
  }

  function text(el, value, cls) {
    if (!el) return;
    el.textContent = value === undefined || value === null || value === "" ? "-" : String(value);
    if (cls !== undefined) el.className = cls;
  }

  // The bar is the used part: amber from warnFrom %, red from badFrom %.
  function fill(el, percent, warnFrom, badFrom) {
    if (!el || !el.firstElementChild) return;
    var value = Math.max(0, Math.min(100, percent));
    el.firstElementChild.style.width = value + "%";
    el.firstElementChild.className = value >= badFrom ? "bad" : value >= warnFrom ? "warn" : "";
  }

  function ask(method, url, body, onOk, onFail) {
    var r = new XMLHttpRequest();
    r.open(method, url, true);
    r.timeout = 20000;
    if (body) r.setRequestHeader("Content-Type", "application/json");
    r.onload = function () {
      var parsed = null;
      try { parsed = JSON.parse(r.responseText); } catch (e) { parsed = null; }
      if (r.status >= 200 && r.status < 300) { onOk(parsed || {}, r); return; }
      var why = parsed && parsed.error ? parsed.error : "the device answered " + r.status;
      if (parsed && parsed.detail) why += ": " + parsed.detail;
      onFail(why, r.status);
    };
    r.onerror = function () { onFail("no answer", 0); };
    r.ontimeout = function () { onFail("no answer in 20 s", 0); };
    r.send(body ? JSON.stringify(body) : null);
  }

  function getJson(url, onOk, onFail) { ask("GET", url, null, onOk, onFail || function () {}); }
  function post(url, body, onOk, onFail) { ask("POST", url, body, onOk, onFail); }

  function lamp(cls, word) {
    var el = by("lamp");
    if (!el) return;
    el.className = "lamp " + cls;
    el.textContent = word;
  }

  function said(message, kind, id) {
    var box = by(id || "said");
    if (!box) return;
    box.hidden = false;
    box.className = "notice" + (kind ? " " + kind : "");
    box.textContent = message;
  }

  function el(tag, cls, content) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (content !== undefined) node.textContent = content;
    return node;
  }

  // --- front page -----------------------------------------------------------

  function frontPage() {
    function paint(d) {
      var online = d.wifi === "connected";
      var apUp = d.ap_ip && d.ap_ip !== "0.0.0.0";
      if (online && d.internet === "offline") lamp("warn", "no internet");
      else if (online) lamp("ok", "online");
      else if (apUp) lamp("warn", "access point");
      else lamp("warn", d.wifi);

      text(by("wifi"), d.wifi + " (" + d.wifi_mode + ")", online ? "ok" : "");
      text(by("ssid"), d.ssid);
      var rssi = by("rssi");
      if (online && typeof d.rssi === "number" && d.rssi < 0) {
        text(rssi, d.rssi + " dBm", d.rssi > -67 ? "ok" : d.rssi > -80 ? "warn" : "bad");
      } else { text(rssi, "-", "faint"); }
      text(by("ip"), d.ip === "0.0.0.0" ? "-" : d.ip + (d.ip_static ? " (static)" : " (dhcp)"));
      text(by("mac"), d.mac);
      text(by("internet"), d.internet,
           d.internet === "online" ? "ok" : d.internet === "offline" ? "bad" : "faint");
      text(by("ble"), d.ble + (d.ble === "connected" ? (d.ble_secure ? ", secure" : ", not secure") : ""),
           d.ble === "connected" && d.ble_secure ? "ok" : "");
      text(by("ap"), apUp ? d.ap_ssid + " at " + d.ap_ip + ", " + d.ap_clients + " client(s)" : "off");

      text(by("kb"), d.kb_ready ? "ready" : "not subscribed", d.kb_ready ? "ok" : "faint");
      text(by("fido"), (d.fido_ready ? "ready" : "not subscribed") + ", " + d.fido_requests +
           " request(s), " + d.fido_errors + " error(s)", d.fido_ready ? "ok" : "faint");
      if (d.fido_store) {
        text(by("fido-store"), d.fido_creds + " of " + d.fido_max + " credentials (" +
             d.fido_resident + " resident), " + bytes(d.fido_used) + " of " +
             bytes(d.fido_size));
        text(by("fido-pin"), d.fido_pin ? "set, " + d.fido_pin_retries + " attempt(s) left"
                                        : "not set",
             !d.fido_pin ? "faint" : d.fido_pin_retries <= 3 ? "warn" : "ok");
        text(by("fido-counter"), d.fido_counter + " signature(s)");
        // Version: +1 per write; an older copy is refused (rollback).
        text(by("fido-file"), d.fido_version ? "version " + d.fido_version + ", changed " +
             (d.fido_modified > 0 ? localTime(d.fido_modified, d.time_offset) : "?") : "none yet");
        by("fido-meter").style.display = "";
        fill(by("fido-meter"), (d.fido_creds / d.fido_max) * 100, 80, 90);
      } else {
        // Off: the store file was refused (rollback...); else a request holds it.
        text(by("fido-store"), d.fido_off ? "off: " + d.fido_off : "busy", d.fido_off ? "bad" : "faint");
        text(by("fido-pin"), "-", "faint");
        text(by("fido-counter"), "-", "faint");
        text(by("fido-file"), "-", "faint");
        by("fido-meter").style.display = "none";
      }

      // The vault: locked until the PIN (on the screen or the Passwords page).
      if (d.vault === "open") {
        text(by("vault"), "unlocked, " + d.vault_count + " record(s)", "ok");
      } else if (d.vault === "locked") {
        text(by("vault"), d.pin_lock_s > 0 ? "locked, PIN entry blocked for " + duration(d.pin_lock_s)
             : "locked" + (d.pin_failures ? ", " + d.pin_failures + " wrong PIN(s)" : ""),
             d.pin_lock_s > 0 ? "bad" : "warn");
      } else { text(by("vault"), d.vault || "-", d.vault === "refused" ? "bad" : "faint"); }

      if (d.time_utc > 0) {
        // The device's local time: UTC shifted by its zone, printed as UTC.
        var off = d.time_offset, a = Math.abs(off);
        var local = localTime(d.time_utc, off);
        text(by("time"), local + " " + d.time_zone + " (UTC" + (off < 0 ? "-" : "+") +
             ("0" + Math.floor(a / 3600)).slice(-2) + ":" + ("0" + Math.floor(a % 3600 / 60)).slice(-2) + ")");
      } else { text(by("time"), "not set", "warn"); }
      text(by("time-sync"), d.time_sync + (d.time_sync_ago_s >= 0 ? ", last " +
           duration(d.time_sync_ago_s) + " ago" : ""),
           d.time_sync === "ok" ? "ok" : d.time_sync === "failing" ? "bad" : "faint");
      text(by("uptime"), duration(d.uptime_s));
      text(by("heap"), bytes(d.heap_free) + ", lowest " + bytes(d.heap_low));
      text(by("psram"), bytes(d.psram_free));
      var used = d.spiffs_total > 0 ? (d.spiffs_used / d.spiffs_total) * 100 : 0;
      text(by("spiffs"), d.spiffs_total > 0 ? bytes(d.spiffs_used) + " of " +
           bytes(d.spiffs_total) + " (" + used.toFixed(0) + "%)" : "not mounted");
      fill(by("spiffs-meter"), used, 80, 95);
      // The microSD: hidden on a board without a slot.
      by("sd-row").hidden = d.sd === "none";
      var sdUsed = d.sd_total > 0 ? (1 - d.sd_free / d.sd_total) * 100 : 0;
      if (d.sd === "mounted") {
        text(by("sd"), "connected" + (d.sd_name ? " (" + d.sd_name + ")" : "") +
             (d.sd_total > 0 ? ", " + bytes(d.sd_free) + " free of " + bytes(d.sd_total) : ""), "ok");
      } else { text(by("sd"), d.sd || "-", "warn"); }
      fill(by("sd-meter"), sdUsed, 90, 98);
      text(by("firmware"), d.firmware + " on esp-idf " + d.idf);
    }

    function failed(why) {
      lamp("bad", why);
      var cells = document.querySelectorAll("td, td span");
      for (var i = 0; i < cells.length; i++) {
        if (cells[i].textContent === "…") text(cells[i], "-", "faint");
      }
    }

    function refresh() { getJson("/api/status", paint, failed); }

    refresh();
    setInterval(refresh, 5000);
    // A click on the lamp reads the status now, without waiting for the timer.
    var light = by("lamp");
    light.style.cursor = "pointer";
    light.title = "refresh now";
    light.addEventListener("click", function () { lamp("", "reading…"); refresh(); });
  }

  // --- settings ---------------------------------------------------------------

  // Each control: POST /api/settings, then save; `then` runs on success.
  function setAndSave(body, saidId, okText, then) {
    said("applying…", "", saidId);
    post("/api/settings", body, function () {
      post("/api/action?do=save", null, function () {
        said(okText, "ok", saidId);
        if (then) then();
      }, function (why) { said("set, but not saved: " + why, "bad", saidId); });
    }, function (why) { said(why, "bad", saidId); });
  }

  function settingsPage() {
    function mark(id, attr, value) {
      var buttons = by(id).querySelectorAll("button");
      for (var i = 0; i < buttons.length; i++) {
        buttons[i].className = buttons[i].getAttribute(attr) === value ? "on" : "";
      }
    }
    // A field follows the device until somebody touches it.
    var edited = {};
    function follow(id, value) {
      var input = by(id);
      if (!edited[id] && document.activeElement !== input) input.value = value;
    }
    function showLevel() {
      by("light-brightness-value").textContent = by("light-brightness").value + "%";
      by("light-warmth-value").textContent = by("light-warmth").value + "%";
    }

    function paint(d) {
      lamp(d.ble === "connected" ? "ok" : "warn", "ble " + d.ble);
      // The name field follows the device until somebody types in it.
      follow("ble-name", d.ble_name || "");

      mark("wallpaper", "data-mode", d.wallpaper);
      follow("fast-refreshes", d.fast_refreshes);
      follow("full-from", d.full_from_percent);
      by("light-box").hidden = !d.light;
      if (!d.light) said("this board has no frontlight", "", "said-light");
      mark("light-state", "data-on", d.light_on ? "1" : "0");
      follow("light-brightness", d.light_brightness);
      follow("light-warmth", d.light_warmth);
      showLevel();
      follow("idle", d.idle_minutes);
      mark("rail", "data-hold", d.hold_rail ? "1" : "0");
    }

    function refresh() { getJson("/api/status", paint, function (why) { lamp("bad", why); }); }

    // BLE name: config.json ble.name, applied to the radio at once, then saved.
    by("ble-name").addEventListener("input", function () { edited["ble-name"] = true; });
    by("ble-name-set").addEventListener("click", function () {
      var name = by("ble-name").value.trim();
      if (!name) { said("a name is 1..29 bytes", "bad"); return; }
      setAndSave({ ble: { name: name } }, "said",
                 "name set and saved to config.json; a paired host may show the old one " +
                 "until it reconnects", function () { edited["ble-name"] = false; refresh(); });
    });

    // config.json display.wallpaper: grey (where the panel has it) or bw.
    by("wallpaper").addEventListener("click", function (e) {
      var mode = e.target.getAttribute && e.target.getAttribute("data-mode");
      if (!mode) return;
      setAndSave({ display: { wallpaper: mode } }, "said-wallpaper",
                 mode === "grey" ? "4 grey levels, saved; a panel without them draws black and white"
                                 : "black and white, saved",
                 function () { mark("wallpaper", "data-mode", mode); });
    });

    // config.json display.fastRefreshes / fullFromPercent: read at each refresh.
    [["fast-refreshes", "fastRefreshes", 200, function (n) {
       return n ? n + " fast refreshes between full ones, saved" : "always full, saved"; }],
     ["full-from", "fullFromPercent", 100, function (n) {
       return n ? "full from " + n + "% of the screen changed, saved"
                : "a big change is no reason for a full one, saved"; }]
    ].forEach(function (f) {
      var id = f[0];
      by(id).addEventListener("input", function () { edited[id] = true; });
      by(id + "-set").addEventListener("click", function () {
        var n = Number(by(id).value);
        if (by(id).value === "" || !(n >= 0 && n <= f[2]) || n % 1) {
          said("0.." + f[2], "bad", "said-refresh");
          return;
        }
        var display = {};
        display[f[1]] = n;
        setAndSave({ display: display }, "said-refresh", f[3](n),
                   function () { edited[id] = false; });
      });
    });

    // The light on / off: a state now, not saved.
    by("light-state").addEventListener("click", function (e) {
      var on = e.target.getAttribute && e.target.getAttribute("data-on");
      if (!on) return;
      post("/api/action?do=" + (on === "1" ? "light-on" : "light-off"), null, function () {
        mark("light-state", "data-on", on);
        said(on === "1" ? "on" : "off", "ok", "said-light");
      }, function (why) { said(why, "bad", "said-light"); });
    });
    // config.json frontlight: applied while sliding, saved on release.
    ["light-brightness", "light-warmth"].forEach(function (id) {
      var input = by(id);
      input.addEventListener("input", function () { edited[id] = true; showLevel(); });
      input.addEventListener("change", function () {
        var level = { brightness: +by("light-brightness").value, warmth: +by("light-warmth").value };
        setAndSave({ frontlight: level }, "said-light",
                   "brightness " + level.brightness + "%, warmth " + level.warmth + "%, saved",
                   function () { edited[id] = false; });
      });
    });

    // config.json power.idleMinutes (read live) and power.holdRail (at the next sleep).
    by("idle").addEventListener("input", function () { edited.idle = true; });
    by("idle-set").addEventListener("click", function () {
      var minutes = Number(by("idle").value);
      if (by("idle").value === "" || !(minutes >= 0 && minutes <= 1440) || minutes % 1) {
        said("0..1440 minutes", "bad", "said-power");
        return;
      }
      setAndSave({ power: { idleMinutes: minutes } }, "said-power",
                 minutes ? "off after " + minutes + " min with nothing done, saved"
                         : "never off on its own, saved",
                 function () { edited.idle = false; });
    });
    by("rail").addEventListener("click", function (e) {
      var hold = e.target.getAttribute && e.target.getAttribute("data-hold");
      if (!hold) return;
      setAndSave({ power: { holdRail: hold === "1" } }, "said-power",
                 hold === "1" ? "battery kept through the sleep, saved"
                              : "battery cut in the sleep, saved: the PIN after every wake",
                 function () { mark("rail", "data-hold", hold); });
    });

    refresh();
    setInterval(refresh, 5000);
  }

  // --- the console dump -------------------------------------------------------

  function dumpPage(url) {
    var dump = by("dump"), button = by("again");

    function read() {
      button.disabled = true;
      lamp("", "reading…");
      var started = Date.now();
      ask("GET", url, null, function (ignored, r) {
        button.disabled = false;
        lamp("ok", "read");
        dump.textContent = r.responseText;
        text(by("taken"), (r.responseText.length / 1024).toFixed(1) + " KB in " +
          ((Date.now() - started) / 1000).toFixed(1) + " s");
      }, function (why, status) {
        button.disabled = false;
        lamp(status === 503 ? "warn" : "bad", status === 503 ? "no dump" : why);
        // The last good dump stays.
        if (dump.textContent === "reading…") dump.textContent = "-";
      });
    }

    button.addEventListener("click", read);
    read();
  }

  // --- restart ----------------------------------------------------------------

  function rebootPage() {
    var button = by("go"), name = by("go-name"), sub = by("go-sub");
    var ARMED = 5000, armedUntil = 0, timer = null;

    function disarm() {
      armedUntil = 0;
      if (timer) { clearTimeout(timer); timer = null; }
      name.textContent = "Restart the device";
      sub.textContent = "tap, then confirm";
    }

    function arm() {
      armedUntil = Date.now() + ARMED;
      name.textContent = "Tap again to restart";
      sub.textContent = "or wait five seconds and nothing happens";
      timer = setTimeout(disarm, ARMED);
    }

    // A dropped connection is the success case: poll until it answers.
    function waitForIt() {
      lamp("warn", "waiting for it");
      var deadline = Date.now() + 60000;
      (function poll() {
        setTimeout(function () {
          getJson("/api/status", function (d) {
            lamp("ok", "up again");
            name.textContent = "It is back";
            sub.textContent = "uptime " + duration(d.uptime_s);
          }, function () {
            if (Date.now() < deadline) { poll(); return; }
            lamp("bad", "no answer");
            name.textContent = "No answer in a minute";
            sub.textContent = "it may be on another address, or the server is not up (web auto)";
          });
        }, 3000);
      })();
    }

    button.addEventListener("click", function () {
      if (armedUntil !== 0 && Date.now() < armedUntil) {
        button.disabled = true;
        name.textContent = "Restarting";
        sub.textContent = "the device is going down";
        lamp("warn", "restarting");
        post("/api/reboot?confirm=reboot", null, waitForIt, waitForIt);
        return;
      }
      arm();
    });
  }

  // --- Wi-Fi --------------------------------------------------------------------

  function wifiPage() {
    var state = { mode: "off", networks: [], max: 4, writable: true };

    function drawMode() {
      var buttons = by("mode").querySelectorAll("button");
      for (var i = 0; i < buttons.length; i++) {
        buttons[i].className = buttons[i].getAttribute("data-mode") === state.mode ? "on" : "";
      }
    }

    function revealer(input) {
      var button = el("button", "reveal", "show");
      button.type = "button";
      button.addEventListener("click", function () {
        input.type = input.type === "password" ? "text" : "password";
        button.textContent = input.type === "password" ? "show" : "hide";
      });
      return button;
    }

    function field(label, input) {
      var row = el("div", "field");
      row.appendChild(el("label", "", label));
      row.appendChild(input);
      return row;
    }

    // Built from nodes, never innerHTML: names come from the air.
    function drawNetworks() {
      var box = by("networks");
      box.textContent = "";
      if (state.networks.length === 0) {
        box.appendChild(el("p", "hint", "None configured."));
      }
      state.networks.forEach(function (network, index) {
        var card = el("div", "card");
        var head = el("div", "card-head");
        head.appendChild(el("b", "", String(index + 1)));
        if (network.ip && network.ip.static) {
          head.appendChild(el("span", "", "static " + network.ip.address));
        }
        if (network.failed) head.appendChild(el("span", "bad", "password refused"));
        var drop = el("button", "drop", "×");
        drop.type = "button";
        drop.title = "forget";
        drop.addEventListener("click", function () {
          state.networks.splice(index, 1);
          drawNetworks();
        });
        head.appendChild(drop);
        card.appendChild(head);

        var ssid = el("input", "ssid");
        ssid.type = "text";
        ssid.autocomplete = "off";
        ssid.spellcheck = false;
        ssid.setAttribute("autocapitalize", "off");
        ssid.value = network.ssid;
        ssid.addEventListener("input", function () { network.ssid = ssid.value; });
        card.appendChild(field("Name", ssid));

        var pass = el("input", "pass");
        pass.type = "password";
        pass.autocomplete = "new-password";
        pass.placeholder = network.secured ? "unchanged" : "none";
        if (network.typed) pass.value = network.typed;
        pass.addEventListener("input", function () { network.typed = pass.value; });
        var row = field("Password", pass);
        row.appendChild(revealer(pass));
        card.appendChild(row);
        box.appendChild(card);
      });
      by("add").disabled = !state.writable || state.networks.length >= state.max;
    }

    function collect() {
      var networks = state.networks.map(function (network) {
        var entry = { ssid: network.ssid.trim() };
        if (network.typed) entry.password = network.typed;
        return entry;
      });
      var ap = { ssid: by("ap-ssid").value.trim(), channel: parseInt(by("ap-channel").value, 10) };
      if (by("ap-pass").value !== "") ap.password = by("ap-pass").value;
      return { wifi: { mode: state.mode, networks: networks, ap: ap } };
    }

    function load() {
      getJson("/api/settings", function (d) {
        var wifi = d.wifi || {};
        state.mode = wifi.mode || "off";
        state.max = d.max || 4;
        state.writable = d.writable !== false;
        state.networks = wifi.networks || [];
        by("ap-ssid").value = (wifi.ap && wifi.ap.ssid) || "";
        by("ap-channel").value = (wifi.ap && wifi.ap.channel) || 6;
        by("ap-pass").value = "";
        by("ap-pass").placeholder = wifi.ap && wifi.ap.secured ? "unchanged" : "none";
        drawMode();
        drawNetworks();

        var link = d.state || {};
        var online = link.wifi === "connected";
        lamp(online ? "ok" : "warn", link.wifi || "?");
        var line = online
          ? "Connected to " + link.ssid + " at " + link.ip + ", " + link.rssi + " dBm."
          : "Not connected" + (link.failure && link.failure !== "none"
              ? " (last attempt: " + link.failure + ")" : "") + ".";
        if (link.ap_up) line += " Access point up, " + link.ap_clients + " client(s).";
        by("state").textContent = line;

        if (!state.writable) {
          said("read-only: web.write is false in config.json", "bad");
          var all = document.querySelectorAll("button:not(.reveal), input");
          for (var i = 0; i < all.length; i++) all[i].disabled = true;
        }
      }, function (why) { lamp("bad", why); });
    }

    function apply(alsoSave) {
      said("applying…");
      post("/api/settings", collect(), function () {
        if (!alsoSave) {
          said("applied in memory: 'Reconnect now' puts it on the radio", "ok");
          load();
          return;
        }
        post("/api/action?do=save", null, function () {
          said("saved to config.json: 'Reconnect now' puts it on the radio", "ok");
          load();
        }, function (why) { said(why, "bad"); });
      }, function (why) { said(why, "bad"); });
    }

    by("mode").addEventListener("click", function (event) {
      var mode = event.target.getAttribute("data-mode");
      if (!mode || !state.writable) return;
      state.mode = mode;
      drawMode();
      said("not applied yet: press Apply");
    });

    by("add").addEventListener("click", function () {
      if (state.networks.length >= state.max) return;
      state.networks.push({ ssid: "", secured: false });
      drawNetworks();
    });

    by("scan").addEventListener("click", function () {
      said("looking… a second or two");
      getJson("/api/wifi/scan", function (d) {
        var box = by("scan-list");
        box.textContent = "";
        if (!d.networks || d.networks.length === 0) { said("nothing on the air", "warn"); return; }
        said(d.networks.length + " on the air: pick one to add it", "ok");
        d.networks.sort(function (a, b) { return b.rssi - a.rssi; });
        d.networks.forEach(function (found) {
          var pick = el("button", "btn small");
          pick.type = "button";
          pick.appendChild(el("span", "grow", found.ssid));
          pick.appendChild(el("span", "sub", found.rssi + " dBm, ch " + found.channel +
                              (found.secured ? ", secured" : ", open")));
          pick.addEventListener("click", function () {
            for (var i = 0; i < state.networks.length; i++) {
              if (state.networks[i].ssid.trim() === "") {
                state.networks[i].ssid = found.ssid;
                drawNetworks();
                return;
              }
            }
            if (state.networks.length >= state.max) {
              said("all " + state.max + " slots are taken: drop one first", "warn");
              return;
            }
            state.networks.push({ ssid: found.ssid, secured: false });
            drawNetworks();
          });
          box.appendChild(pick);
        });
      }, function (why) { said(why, "bad"); });
    });

    document.querySelector(".reveal[data-for='ap-pass']").addEventListener("click", function () {
      var input = by("ap-pass");
      input.type = input.type === "password" ? "text" : "password";
      this.textContent = input.type === "password" ? "show" : "hide";
    });

    by("apply").addEventListener("click", function () { apply(false); });
    by("save").addEventListener("click", function () { apply(true); });
    by("retry").addEventListener("click", function () {
      said("reconnecting: this page may go quiet if the link changes");
      post("/api/action?do=retry", null, function () { setTimeout(load, 3000); },
           function (why) { said(why, "bad"); });
    });

    load();
  }

  // --- shared by the password and FIDO2 pages -----------------------------------

  function revealer(input) {
    var button = el("button", "reveal", "show");
    button.type = "button";
    button.addEventListener("click", function () {
      input.type = input.type === "password" ? "text" : "password";
      button.textContent = input.type === "password" ? "show" : "hide";
    });
    return button;
  }

  function field(label, input) {
    var row = el("div", "field");
    row.appendChild(el("label", "", label));
    row.appendChild(input);
    return row;
  }

  function input(type, value, placeholder) {
    var node = el("input");
    node.type = type;
    node.autocomplete = type === "password" ? "new-password" : "off";
    node.spellcheck = false;
    node.setAttribute("autocapitalize", "off");
    if (value) node.value = value;
    if (placeholder) node.placeholder = placeholder;
    return node;
  }

  // Tap, then tap again within 4 s: a delete never happens on one stray tap.
  function twoTap(button, word, action) {
    var armed = 0;
    button.addEventListener("click", function () {
      if (armed && Date.now() < armed) { armed = 0; action(); return; }
      armed = Date.now() + 4000;
      var was = button.textContent;
      button.textContent = word;
      setTimeout(function () { armed = 0; button.textContent = was; }, 4000);
    });
  }

  function when(utc, offset) { return utc > 0 ? localTime(utc, offset) : "?"; }

  // --- passwords ------------------------------------------------------------------

  function passwordsPage() {
    var state = { records: [], writable: true, offset: 0, open: {}, adding: false };
    var ticker = null;  // the TOTP code on show: { id, node, until, timer }

    function stopCode() {
      if (ticker) { clearInterval(ticker.timer); ticker = null; }
    }

    // The current window's code; asked again when the window turns.
    function showCode(record, box) {
      if (ticker && ticker.id === record.id) { stopCode(); box.textContent = ""; return; }
      stopCode();
      box.textContent = "…";
      getJson("/api/vault/totp?id=" + record.id, function (d) {
        var mine = { id: record.id, node: box, until: Date.now() + d.remaining * 1000 };
        ticker = mine;
        function paint() {
          if (ticker !== mine) return;
          var left = Math.max(0, Math.ceil((mine.until - Date.now()) / 1000));
          box.textContent = "";
          box.appendChild(el("span", "code", d.code.slice(0, 3) + " " + d.code.slice(3)));
          box.appendChild(el("span", "faint", " " + left + " s"));
          if (left === 0) { stopCode(); showCode(record, box); }
        }
        mine.timer = setInterval(paint, 500);
        paint();
      }, function (why) { box.textContent = why; box.className = "totp bad"; });
    }

    function send(url, body, done, okWord) {
      said("writing…");
      post(url, body, function () {
        said(okWord, "ok");
        if (done) done();
        load();
      }, function (why) { said(why, "bad"); });
    }

    // One record's edit box (or the new record's, without `record`).
    function editor(record) {
      var box = el("div", "edit");
      var site = input("text", record ? record.site : "");
      var user = input("text", record ? record.user : "");
      var pass = input("password", "", record ? (record.password ? "unchanged" : "none") : "none");
      var otp = input("password", "", record ? (record.otp ? "unchanged" : "none") : "none if empty");
      box.appendChild(field("Site", site));
      box.appendChild(field("User", user));
      var row = field("Password", pass);
      row.appendChild(revealer(pass));
      box.appendChild(row);
      row = field("TOTP", otp);
      // The secret as given: Base32 (authenticator apps) or Base64; the
      // device keeps Base32 either way.
      var format = el("button", "reveal", "Base32");
      format.type = "button";
      format.title = "the secret's encoding";
      format.addEventListener("click", function () {
        format.textContent = format.textContent === "Base32" ? "Base64" : "Base32";
      });
      row.appendChild(format);
      row.appendChild(revealer(otp));
      box.appendChild(row);
      function secret() {
        var value = otp.value.trim();
        return value && format.textContent === "Base64" ? "base64:" + value : value;
      }

      var actions = el("div", "row-actions");
      var save = el("button", "btn small", record ? "Save" : "Add");
      save.type = "button";
      save.addEventListener("click", function () {
        if (!record) {
          send("/api/vault/add", { site: site.value.trim(), user: user.value.trim(),
                                   password: pass.value, otp: secret() },
               function () { state.adding = false; }, "added");
          return;
        }
        var body = { id: record.id }, any = false;
        if (site.value.trim() !== record.site) { body.site = site.value.trim(); any = true; }
        if (user.value.trim() !== record.user) { body.user = user.value.trim(); any = true; }
        if (pass.value !== "") { body.password = pass.value; any = true; }
        if (otp.value.trim() !== "") { body.otp = secret(); any = true; }
        if (!any) { said("nothing changed", "warn"); return; }
        send("/api/vault/set", body, function () { delete state.open[record.id]; }, "saved");
      });
      actions.appendChild(save);
      if (record && record.otp) {
        var drop = el("button", "btn small", "Remove TOTP");
        drop.type = "button";
        twoTap(drop, "Tap again to remove", function () {
          send("/api/vault/set", { id: record.id, otp: "" },
               function () { delete state.open[record.id]; }, "TOTP removed");
        });
        actions.appendChild(drop);
      }
      var cancel = el("button", "btn small", "Cancel");
      cancel.type = "button";
      cancel.addEventListener("click", function () {
        if (record) delete state.open[record.id]; else state.adding = false;
        draw();
      });
      actions.appendChild(cancel);
      box.appendChild(actions);
      if (!state.writable) {
        var all = box.querySelectorAll("button:not(.reveal), input");
        for (var i = 0; i < all.length; i++) all[i].disabled = true;
      }
      return box;
    }

    // The device's list order: up / down move a record by one place, the
    // double arrows to the first / last.
    function mover(text, title, record, index, position) {
      var b = el("button", "drop move", text);
      b.type = "button";
      b.title = title;
      b.disabled = !state.writable || position < 0 || position >= state.records.length ||
                   position === index;
      b.addEventListener("click", function () {
        send("/api/vault/move", { id: record.id, position: position }, null, "moved");
      });
      return b;
    }

    function card(record, index) {
      var node = el("div", "card");
      var head = el("div", "card-head");
      // Its place on the device: 6 a page.
      head.appendChild(el("span", "faint", (index + 1) + "."));
      var name = el("span", "who");
      name.appendChild(el("b", "", record.site));
      if (record.user) name.appendChild(el("span", "", " / " + record.user));
      head.appendChild(name);
      head.appendChild(mover("⇈", "move to the top", record, index, 0));
      head.appendChild(mover("↑", "move up", record, index, index - 1));
      head.appendChild(mover("↓", "move down", record, index, index + 1));
      head.appendChild(mover("⇊", "move to the bottom", record, index, state.records.length - 1));
      var drop = el("button", "drop", "×");
      drop.type = "button";
      drop.title = "delete";
      drop.disabled = !state.writable;
      twoTap(drop, "?", function () {
        send("/api/vault/delete", { id: record.id }, function () { delete state.open[record.id]; },
             "deleted " + record.site);
      });
      head.appendChild(drop);
      node.appendChild(head);

      var line = el("div", "tags");
      line.appendChild(el("span", record.password ? "ok" : "faint",
                          record.password ? "password set" : "no password"));
      line.appendChild(el("span", "faint", "changed " + when(record.modified, state.offset)));
      node.appendChild(line);

      var actions = el("div", "row-actions");
      var edit = el("button", "btn small", "Edit");
      edit.type = "button";
      edit.addEventListener("click", function () {
        if (state.open[record.id]) delete state.open[record.id]; else state.open[record.id] = true;
        draw();
      });
      actions.appendChild(edit);
      var code = el("div", "totp");
      if (record.otp) {
        var show = el("button", "btn small", "TOTP code");
        show.type = "button";
        show.addEventListener("click", function () { showCode(record, code); });
        actions.appendChild(show);
      }
      node.appendChild(actions);
      if (record.otp) node.appendChild(code);
      if (state.open[record.id]) node.appendChild(editor(record));
      return node;
    }

    // Built from nodes, never innerHTML: names are whatever was typed.
    function draw() {
      stopCode();
      var box = by("records");
      box.textContent = "";
      if (state.adding) {
        var fresh = el("div", "card");
        fresh.appendChild(el("div", "card-head", "New record"));
        fresh.appendChild(editor(null));
        box.appendChild(fresh);
      }
      var words = by("filter").value.trim().toLowerCase();
      var shown = 0;
      state.records.forEach(function (record, index) {
        if (words && (record.site + " " + record.user).toLowerCase().indexOf(words) < 0) return;
        box.appendChild(card(record, index));
        ++shown;
      });
      if (shown === 0 && !state.adding) {
        box.appendChild(el("p", "hint", state.records.length ? "Nothing matches." : "No records yet."));
      }
      by("add").disabled = !state.writable || state.adding;
      by("sort").disabled = !state.writable || state.records.length < 2;
      by("lock").disabled = !state.writable;
    }

    function load() {
      getJson("/api/vault", function (d) {
        state.records = d.records || [];
        state.writable = d.writable !== false;
        state.offset = d.time_offset || 0;
        var open = d.state === "open";
        by("unlock").hidden = d.state !== "locked";
        by("open").hidden = !open;
        if (open) {
          lamp("ok", "open");
          by("state").textContent = d.count + " of " + d.capacity + " records, version " +
            d.version + ", changed " + when(d.modified, state.offset) + ".";
          draw();
        } else if (d.state === "locked") {
          stopCode();
          lamp("warn", "locked");
          by("state").textContent = d.lock_left_s > 0
            ? "Too many wrong PINs: try again in " + duration(d.lock_left_s) + "."
            : d.pin_failures > 0 ? (d.pin_tries - d.pin_failures) + " of " + d.pin_tries + " tries left."
            : "Locked: nothing is decrypted until the PIN.";
          by("pin").focus();
        } else {
          lamp("bad", d.state);
          by("state").textContent = d.state === "refused"
            ? "The store file is refused: " + d.why + ". The console's 'pass reset' replaces it."
            : "No storage.";
        }
        seedState(d, open);
        if (!state.writable) said("read-only: web.write is false in config.json", "bad");
      }, function (why) { lamp("bad", why); });
    }

    // Open: a new phrase re-keys both stores (the PIN); locked: only while
    // there is no store (another device's phrase goes in on the console).
    function seedState(d, open) {
      var can = open || (d.state === "locked" && !d.has_file);
      by("seed").hidden = d.state !== "open" && d.state !== "locked";
      by("seed-pin-row").hidden = !open;
      by("seed-new").disabled = !state.writable || !can;
      var line = d.seed_id ? "This device's phrase: id " + d.seed_id + "." : "No phrase yet.";
      if (d.file_seed_id && d.file_seed_id !== d.seed_id) {
        line += " vault.bin needs another one (id " + d.file_seed_id +
                "): 'seed set <words>' on the console.";
      } else if (!can) {
        line += " Unlock to make a new one.";
      }
      by("seed-state").textContent = line;
    }

    function newSeed() {
      var body = {};
      if (!by("seed-pin-row").hidden) {
        var pin = by("seed-pin").value;
        if (!/^[0-9]{6}$/.test(pin)) { said("the PIN is 6 digits", "warn"); return; }
        body.pin = pin;
      }
      said("making a new phrase and a new key… a few seconds");
      by("seed-new").disabled = true;
      post("/api/vault/seed-new", body, function (d) {
        by("seed-pin").value = "";
        var box = by("seed-words");
        box.hidden = false;
        box.textContent = "Write these down now (shown once; every device of this store " +
                          "needs them): " + d.seed;
        said("new seed phrase: vault.bin and fido.bin have a new key", "ok");
        load();
      }, function (why) {
        by("seed-pin").value = "";
        said(why, "bad");
        load();
      });
    }

    function unlock() {
      var pin = by("pin").value;
      if (!/^[0-9]{6}$/.test(pin)) { said("the PIN is 6 digits", "warn"); return; }
      said("checking… about a second");
      by("pin-go").disabled = true;
      post("/api/vault/unlock", { pin: pin }, function () {
        by("pin").value = "";
        by("pin-go").disabled = false;
        said("unlocked", "ok");
        load();
      }, function (why) {
        by("pin").value = "";
        by("pin-go").disabled = false;
        said(why, "bad");
        load();
      });
    }

    by("pin-go").addEventListener("click", unlock);
    by("pin").addEventListener("keydown", function (event) { if (event.key === "Enter") unlock(); });
    by("filter").addEventListener("input", draw);
    by("add").addEventListener("click", function () { state.adding = true; draw(); });
    twoTap(by("sort"), "Tap again: A to Z", function () {
      send("/api/vault/sort", {}, null, "sorted by site, then user");
    });
    by("lock").addEventListener("click", function () {
      post("/api/vault/lock", {}, function () { said("locked", "ok"); load(); },
           function (why) { said(why, "bad"); });
    });
    twoTap(by("seed-new"), "Tap again: replace the phrase", newSeed);
    load();
  }

  // --- FIDO2 ------------------------------------------------------------------------

  function fidoPage() {
    function load() {
      getJson("/api/fido", function (d) {
        lamp("ok", d.count + " stored");
        by("state").textContent = "Credentials made by sites through this key (WebAuthn).";
        text(by("stored"), d.count + " of " + d.max + " (" + d.resident + " resident), " +
             bytes(d.used) + " of " + bytes(d.size));
        text(by("pin"), d.pin_set ? "set, " + d.pin_retries + " attempt(s) left" : "not set",
             !d.pin_set ? "faint" : d.pin_retries <= 3 ? "warn" : "ok");
        text(by("counter"), d.counter + " signature(s)");
        text(by("file"), d.version ? "version " + d.version + ", changed " +
             when(d.modified, d.time_offset) : "none yet");

        var box = by("credentials");
        box.textContent = "";
        if (!d.credentials.length) box.appendChild(el("p", "hint", "None."));
        d.credentials.forEach(function (c) {
          var node = el("div", "card");
          var head = el("div", "card-head");
          var name = el("span", "who");
          if (c.u2f) {
            // no site name: the start of the AppID's SHA-256 ("U2F 3dec69ac")
            name.appendChild(el("span", "badge", "U2F"));
            var app = el("b", "", c.rp.replace(/^U2F /, ""));
            app.title = "AppID hash (first 4 bytes)";
            name.appendChild(app);
          } else {
            name.appendChild(el("b", "", c.rp + (c.rp_cut ? "…" : "")));
          }
          var who = c.user || "";
          if (c.display && c.display !== c.user) who += (who ? " " : "") + "(" + c.display + ")";
          if (who) name.appendChild(el("span", "", " / " + who));
          head.appendChild(name);
          var drop = el("button", "drop", "×");
          drop.type = "button";
          drop.title = "delete";
          drop.disabled = !d.writable;
          twoTap(drop, "?", function () {
            said("deleting…");
            post("/api/fido/delete", { credential: c.id }, function () {
              said("deleted " + c.rp, "ok");
              load();
            }, function (why) { said(why, "bad"); });
          });
          head.appendChild(drop);
          node.appendChild(head);
          var line = el("div", "tags");
          line.appendChild(el("span", c.resident ? "ok" : "faint", c.resident ? "resident" : "not resident"));
          line.appendChild(el("span", "faint", { "-7": "ES256", "-8": "EdDSA", "-19": "Ed25519" }[c.alg] || "alg " + c.alg));
          line.appendChild(el("span", "faint", "made " + when(c.created, d.time_offset)));
          if (c.host) line.appendChild(el("span", "faint", "by " + c.host));
          line.appendChild(el("span", "faint", "id " + c.id.slice(0, 8)));
          node.appendChild(line);
          box.appendChild(node);
        });
        if (!d.writable) said("read-only: web.write is false in config.json", "bad");
      }, function (why, status) {
        lamp(status === 409 ? "warn" : "bad", status === 409 ? "off" : "error");
        said(why, status === 409 ? "" : "bad");
        ["stored", "pin", "counter", "file"].forEach(function (id) { text(by(id), "-", "faint"); });
        by("credentials").textContent = "";
        if (status === 409) {
          by("state").textContent = "FIDO2 is off until the PIN: unlock on the Passwords page.";
        }
      });
    }
    load();
  }

  // --- firmware update ----------------------------------------------------------

  function firmwarePage() {
    var ARMED = 5000;
    var last = null, polling = null, restarting = false;

    // Tap, then tap again within five seconds: no stray click flashes the device.
    function armable(button, nameId, subId, idleName, onGo) {
      var armedUntil = 0, timer = null, idleSub = "";
      function disarm() {
        armedUntil = 0;
        if (timer) { clearTimeout(timer); timer = null; }
        by(nameId).textContent = idleName;
        by(subId).textContent = idleSub;
      }
      button.addEventListener("click", function () {
        if (armedUntil !== 0 && Date.now() < armedUntil) { disarm(); onGo(); return; }
        armedUntil = Date.now() + ARMED;
        idleSub = by(subId).textContent;
        by(nameId).textContent = "Tap again to install";
        by(subId).textContent = "or wait five seconds and nothing happens";
        timer = setTimeout(disarm, ARMED);
      });
    }

    function paint(d) {
      last = d;
      var r = d.running;
      text(by("version"), r.version);
      text(by("built"), r.date + ", IDF " + r.idf);
      text(by("slot"), r.slot);
      if (d.trial) text(by("state"), "on trial: confirmed in " + d.confirm_in + " s", "warn");
      else text(by("state"), r.state, r.state === "valid" ? "ok" : "");
      text(by("next"), d.next.slot + ", " + bytes(d.next.size));
      by("rolled-row").hidden = !d.rolled_back;
      text(by("rolled"), d.rolled_back ? d.rolled_back + " did not confirm itself" : "");

      var sd = d.sd;
      if (!sd.mounted) { text(by("sd"), "no card"); text(by("sd-check"), "-", ""); }
      else if (!sd.present) { text(by("sd"), "not in the card's root"); text(by("sd-check"), "-", ""); }
      else {
        text(by("sd"), sd.version + " (" + sd.date + "), " + bytes(sd.size));
        text(by("sd-check"), !sd.ok ? sd.check : sd.same ? "ok: the running firmware" :
          "ok so far: the rest is checked while writing", sd.ok ? "ok" : "bad");
      }

      var j = d.job, busy = j.state === "writing";
      if (j.state === "idle") text(by("job"), "none", "");
      else if (busy) text(by("job"), "writing " + (j.version || "") + " from " + j.source + ": " +
        bytes(j.written) + " of " + bytes(j.total), "warn");
      else if (j.state === "done") text(by("job"), j.version + " written: restarting", "ok");
      else text(by("job"), "failed: " + j.error, "bad");
      fill(by("meter"), j.total ? 100 * j.written / j.total : 0, 101, 101);

      var refusal = d.refusal;
      by("sd-go").disabled = !!refusal || !sd.ok || restarting;
      text(by("sd-sub"), refusal || (!sd.ok ? "nothing to install" :
        sd.same ? "the same as the running one" : "tap, then confirm"));
      var file = by("file").files[0];
      by("upload").disabled = !!refusal || !file || restarting;
      text(by("upload-sub"), refusal ||
        (file ? file.name + ", " + bytes(file.size) + ": tap, then confirm" : "choose a file first"));
      if (busy) lamp("warn", "writing");
      else if (!restarting || d.trial) lamp(refusal ? "warn" : "ok", d.trial ? "on trial" : refusal ? shortWhy(refusal) : "ready");
      var why = by("why");
      why.hidden = !refusal;
      why.textContent = refusal ? "No update now: " + refusal + "." : "";
    }

    // The lamp's word for the server's refusal (the notice has it whole).
    function shortWhy(refusal) {
      if (refusal.indexOf("locked") === 0) return "locked";
      if (refusal.indexOf("read-only") === 0) return "read-only";
      if (refusal.indexOf("on trial") >= 0) return "on trial";
      if (refusal.indexOf("already running") >= 0) return "busy";
      return "not now";
    }

    function load(onFail) {
      getJson("/api/firmware", paint, onFail || function (why) { lamp("bad", why); });
    }

    // The device restarts into the new slot: ask until it answers, then show the trial.
    function waitForIt() {
      restarting = true;
      if (polling) { clearInterval(polling); polling = null; }
      lamp("warn", "restarting");
      var deadline = Date.now() + 120000;
      (function poll() {
        setTimeout(function () {
          load(function () {
            if (Date.now() < deadline) { poll(); return; }
            lamp("bad", "no answer");
            said("No answer in two minutes: it may be on another address, " +
              "or the server is not up (web auto).", "bad");
          });
        }, 3000);
      })();
    }

    // While an SD job runs: progress every second; a dropped link means it restarted.
    function follow() {
      if (polling) clearInterval(polling);
      polling = setInterval(function () {
        getJson("/api/firmware", function (d) {
          paint(d);
          if (d.job.state !== "failed") return;
          clearInterval(polling);
          polling = null;
          said("Not installed: " + d.job.error, "bad");
        }, function () { waitForIt(); });
      }, 1000);
    }

    by("file").addEventListener("change", function () { if (last) paint(last); });

    armable(by("upload"), "upload-name", "upload-sub", "Upload and install", function () {
      var file = by("file").files[0];
      if (!file) return;
      by("upload").disabled = true;
      lamp("warn", "uploading");
      said("Uploading " + file.name + ": keep this page open.", "");
      var r = new XMLHttpRequest();
      r.open("POST", "/api/firmware/upload?confirm=update", true);
      r.timeout = 600000;
      r.setRequestHeader("Content-Type", "application/octet-stream");
      r.upload.onprogress = function (e) {
        if (!e.lengthComputable) return;
        fill(by("meter"), 100 * e.loaded / e.total, 101, 101);
        text(by("job"), "uploading: " + bytes(e.loaded) + " of " + bytes(e.total), "warn");
      };
      r.onload = function () {
        var d = null;
        try { d = JSON.parse(r.responseText); } catch (e) { d = null; }
        if (r.status >= 200 && r.status < 300) {
          said((d && d.version ? d.version : "The firmware") +
            " installed: the device restarts and runs it on trial.", "ok");
          waitForIt();
          return;
        }
        said("Not installed: " + (d && d.error ? d.error : "the device answered " + r.status), "bad");
        load();
      };
      r.onerror = function () { said("The upload broke off.", "bad"); load(); };
      r.ontimeout = function () { said("No answer in ten minutes.", "bad"); load(); };
      r.send(file);
    });

    armable(by("sd-go"), "sd-name", "sd-sub", "Install firmware.bin", function () {
      by("sd-go").disabled = true;
      lamp("warn", "starting");
      post("/api/firmware/sd?confirm=update", null, function () {
        said("Writing firmware.bin from the card: the device restarts when it is done.", "");
        follow();
      }, function (why) { said("Not started: " + why, "bad"); load(); });
    });

    load();
    // The trial countdown.
    setInterval(function () { if (!polling && last && last.trial) load(function () {}); }, 5000);
  }

  var pages = {
    front: frontPage,
    devstatus: function () { dumpPage("/api/devstatus"); },
    hwinfo: function () { dumpPage("/api/hwinfo"); },
    reboot: rebootPage,
    wifi: wifiPage,
    passwords: passwordsPage,
    fido: fidoPage,
    settings: settingsPage,
    firmware: firmwarePage
  };
  var which = document.body.getAttribute("data-page");
  if (pages[which]) pages[which]();
})();
