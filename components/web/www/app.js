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
    "button.drop{margin-left:auto;width:40px;height:40px;border:1px solid var(--line);",
    "border-radius:10px;background:transparent;color:var(--faint);font-size:20px;",
    "line-height:1;cursor:pointer}",
    ".row-actions{display:flex;gap:10px;margin:0 0 14px}",
    ".btn.small{min-height:48px;padding:10px 14px;font-size:14px;font-weight:400}",
    ".row-actions .btn.small{flex:1;justify-content:center}",
    ".sticky{display:flex;flex-direction:column;gap:10px;margin:22px 0 0;",
    "padding:14px 0 0;border-top:1px solid var(--line)}",
    ".sticky .btn{justify-content:center}",
    "#scan-list{display:flex;flex-direction:column;gap:8px;margin:0 0 14px}",
    "#scan-list .btn.small .sub{display:inline;margin-left:10px}"
  ].join("");

  var style = document.createElement("style");
  style.textContent = CSS;
  document.head.appendChild(style);

  var by = function (id) { return document.getElementById(id); };

  function bytes(n) {
    if (typeof n !== "number" || !isFinite(n)) return "-";
    if (n < 1024) return n + " B";
    if (n < 1048576) return (n / 1024).toFixed(1) + " KB";
    return (n / 1048576).toFixed(2) + " MB";
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

  function meter(el, percent, warnAt, badAt) {
    if (!el || !el.firstElementChild) return;
    var value = Math.max(0, Math.min(100, percent));
    el.firstElementChild.style.width = value + "%";
    el.firstElementChild.className = value <= badAt ? "bad" : value <= warnAt ? "warn" : "";
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

  function said(message, kind) {
    var box = by("said");
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
      text(by("ap"), apUp ? d.ap_ssid + " at " + d.ap_ip + ", " + d.ap_clients + " client(s)" : "off");

      text(by("ble"), d.ble + (d.ble === "connected" ? (d.ble_secure ? ", secure" : ", not secure") : ""),
           d.ble === "connected" && d.ble_secure ? "ok" : "");
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
        by("fido-meter").style.display = "";
        meter(by("fido-meter"), 100 - (d.fido_creds / d.fido_max) * 100, 20, 10);
      } else {
        text(by("fido-store"), "busy", "faint");  // a request holds the store
        text(by("fido-pin"), "-", "faint");
        text(by("fido-counter"), "-", "faint");
        by("fido-meter").style.display = "none";
      }

      text(by("uptime"), duration(d.uptime_s));
      text(by("heap"), bytes(d.heap_free) + ", lowest " + bytes(d.heap_low));
      text(by("psram"), bytes(d.psram_free));
      var used = d.spiffs_total > 0 ? (d.spiffs_used / d.spiffs_total) * 100 : 0;
      text(by("spiffs"), d.spiffs_total > 0 ? bytes(d.spiffs_used) + " of " +
           bytes(d.spiffs_total) + " (" + used.toFixed(0) + "%)" : "not mounted");
      meter(by("spiffs-meter"), 100 - used, 20, 5);
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
  }

  // --- the console dump -------------------------------------------------------

  function devStatusPage() {
    var dump = by("dump"), button = by("again");

    function read() {
      button.disabled = true;
      lamp("", "reading…");
      var started = Date.now();
      ask("GET", "/api/devstatus", null, function (ignored, r) {
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

  var pages = {
    front: frontPage,
    devstatus: devStatusPage,
    reboot: rebootPage,
    wifi: wifiPage
  };
  var which = document.body.getAttribute("data-page");
  if (pages[which]) pages[which]();
})();
