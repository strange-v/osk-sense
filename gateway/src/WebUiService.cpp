#include "WebUiService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "FirmwareVersion.h"
#include "GatewayStatus.h"

namespace gateway::web_ui {
namespace {

constexpr char kPartitionLabel[] = "web";
constexpr char kManifestPath[] = "/ui-manifest.json";
constexpr char kIndexPath[] = "/index.html";
constexpr char kIndexGzipPath[] = "/index.html.gz";

// The build ships index.html gzipped; AsyncFileResponse serves the .gz variant
// transparently, so either name counts as a present page.
bool indexPresent() {
    return LittleFS.exists(kIndexPath) || LittleFS.exists(kIndexGzipPath);
}
constexpr size_t kMaximumManifestBytes = 1024;
constexpr size_t kMaximumVersionLength = 31;
constexpr uint32_t kUnmountDrainMs = 1000;

State currentState = State::Unavailable;
char currentVersion[kMaximumVersionLength + 1]{};
char currentRequiredFirmware[kMaximumVersionLength + 1]{};
bool mounted = false;

constexpr char kRecoveryPage[] PROGMEM = R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OSK Sense Hub</title><style>:root{--bg:#141a24;--surface:#1a2130;--line:#29323f;--line-strong:#3a4553;--ink:#e5eaf1;--muted:#94a2b5;--primary:#4c9fe0;--primary-strong:#2272c0;--primary-strong-hover:#2678c8;color-scheme:dark}@media (prefers-color-scheme:light){:root{--bg:#f4f6f9;--surface:#fff;--line:#e0e5ec;--line-strong:#c9d2dc;--ink:#16202c;--muted:#5b6879;--primary:#1a72c0;--primary-strong:#1a72c0;--primary-strong-hover:#145d9e;color-scheme:light}}body{margin:0;background:var(--bg);color:var(--ink);font:14px system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}main{max-width:40rem;margin:12vh auto;padding:0 16px}section{background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:24px}h1{margin:0 0 12px;font-size:20px}p{line-height:1.6;color:var(--muted)}code{color:var(--primary);font-family:ui-monospace,Consolas,monospace}form{display:grid;gap:12px;margin-top:20px}input,button{font:inherit;min-height:38px;padding:8px 12px;border-radius:5px;box-sizing:border-box}input{border:1px solid var(--line-strong);outline:none;color:var(--ink);background:var(--bg);transition:border-color 120ms ease}input::placeholder{color:var(--muted)}input:focus{border-color:var(--primary)}button{border:1px solid var(--primary-strong);color:#fff;background:var(--primary-strong);font-weight:600;cursor:pointer}button:hover:not(:disabled){background:var(--primary-strong-hover);border-color:var(--primary-strong-hover)}button:focus-visible{outline:2px solid var(--primary);outline-offset:2px}button:disabled{opacity:.6;cursor:default}#s{min-height:1.6em;margin:0}</style></head>
<body><main><section><h1>Web UI unavailable</h1><p>The installed Web UI is missing, damaged, or incompatible with this gateway firmware. The gateway can download the Web UI released with its firmware.</p><p>Firmware: <code>%FIRMWARE%</code> &middot; UI state: <code>%STATE%</code></p>
<form id="f" data-setup="%SETUP%"><input name="username" autocomplete="username" placeholder="Admin username" required><input name="password" type="password" autocomplete="current-password" placeholder="Password" required><button id="b">Install Web UI</button><p id="s" role="status"></p></form></section></main>
<script>
const f=document.getElementById('f'),b=document.getElementById('b'),s=document.getElementById('s'),setup=f.dataset.setup==='1';
const say=t=>{s.textContent=t};
const wait=m=>new Promise(r=>setTimeout(r,m));
const post=(u,h,body)=>fetch(u,{method:'POST',headers:h,body});
const boot=()=>fetch('/health',{cache:'no-store'}).then(r=>r.json()).then(j=>j.boot_id).catch(()=>null);
if(setup){f.username.remove();f.password.remove();say('No admin exists yet: press the gateway button, then install.')}
f.onsubmit=async e=>{e.preventDefault();b.disabled=true;
try{let h={};
if(!setup){const r=await post('/ui/session',{'Content-Type':'application/json'},JSON.stringify({username:f.username.value,password:f.password.value}));
if(!r.ok)throw new Error(r.status===401?'Wrong username or password.':'Sign-in failed ('+r.status+').');
h={'X-CSRF-Token':(await r.json()).csrf_token}}
const before=await boot();
const r=await post('/ui/update/repair-ui',h);
if(!r.ok){const j=await r.json().catch(()=>({}));throw new Error(j.error==='physical_setup_required'?'Press the gateway button first.':'Install refused: '+(j.error||r.status)+'.')}
for(;;){await wait(2000);
const u=await fetch('/ui/update',{cache:'no-store'}).then(x=>x.ok?x.json():null).catch(()=>null);
if(u&&u.state==='failed')throw new Error('Install failed: '+u.error+'.');
if(u&&u.state==='installing'){say('Installing… '+u.progress+'%');continue}
const now=await boot();
if(now&&now!==before){location.reload();return}
say('Restarting…')}
}catch(err){say(err.message);b.disabled=false}};
</script></body></html>)html";

void clearManifest() {
    currentVersion[0] = '\0';
    currentRequiredFirmware[0] = '\0';
}

// The Web UI is flashed as its own LittleFS image, so it can be older or newer
// than the firmware beneath it. It depends on /ui/*, which moves with the
// firmware, so the gate is the firmware series rather than `api_version` --
// that number describes the client contract the UI barely touches.
//
// The patch level is ignored: it does not reshape /ui/*, and rejecting a good
// UI over one would only tempt someone to skip the check.
bool firmwareSeriesMatches(const char *const required) {
    const char *const minorDot = strchr(required, '.');
    if (minorDot == nullptr)
        return false;
    const char *const patchDot = strchr(minorDot + 1, '.');
    const size_t length =
        patchDot == nullptr ? strlen(required) : static_cast<size_t>(patchDot - required);
    return length != 0 && strncmp(required, firmware::version, length) == 0 &&
           (firmware::version[length] == '\0' || firmware::version[length] == '.');
}

void inspectManifest() {
    clearManifest();
    if (!LittleFS.exists(kManifestPath)) {
        currentState = State::MissingManifest;
        return;
    }

    File manifest = LittleFS.open(kManifestPath, "r");
    if (!manifest || manifest.size() == 0 || manifest.size() > kMaximumManifestBytes) {
        currentState = State::InvalidManifest;
        return;
    }

    JsonDocument document;
    if (deserializeJson(document, manifest) != DeserializationError::Ok) {
        currentState = State::InvalidManifest;
        return;
    }

    const char *uiVersion = document["ui_version"].as<const char *>();
    const char *requiredFirmware = document["required_firmware"].as<const char *>();
    if (uiVersion == nullptr || uiVersion[0] == '\0' || strlen(uiVersion) > kMaximumVersionLength ||
        requiredFirmware == nullptr || requiredFirmware[0] == '\0' ||
        strlen(requiredFirmware) > kMaximumVersionLength || !indexPresent()) {
        currentState = State::InvalidManifest;
        return;
    }

    strlcpy(currentVersion, uiVersion, sizeof(currentVersion));
    strlcpy(currentRequiredFirmware, requiredFirmware, sizeof(currentRequiredFirmware));
    currentState = firmwareSeriesMatches(requiredFirmware) ? State::Ready : State::Incompatible;
}

bool acceptsHtml(AsyncWebServerRequest *request) {
    if (!request->hasHeader("Accept"))
        return false;
    const String value = request->getHeader("Accept")->value();
    return value.indexOf("text/html") >= 0;
}

} // namespace

void begin() {
    mounted = LittleFS.begin(false, "/littlefs", 10, kPartitionLabel);
    if (!mounted) {
        currentState = State::Unavailable;
        clearManifest();
        Serial.println("Web UI: LittleFS unavailable");
        return;
    }
    inspectManifest();
    Serial.printf("Web UI: %s%s%s\n",
                  stateName(),
                  currentVersion[0] == '\0' ? "" : ", version=",
                  currentVersion);
}

void addRoutes(AsyncWebServer &server) {
    if (currentState == State::Ready) {
        // Only the hashed build output is reachable over HTTP. Serving the
        // filesystem root instead would also expose /ui-manifest.json, which
        // no browser reads -- the firmware parses it locally at boot -- and
        // which carries the build SHA. Every name here contains a content
        // hash, so the response can be cached permanently; index.html is sent
        // by handlePageRequest with its own revalidating header.
        server.serveStatic("/assets/", LittleFS, "/assets/")
            .setCacheControl("public, max-age=31536000, immutable");
    }
}

void prepareForFilesystemUpdate() {
    currentState = State::Updating;
    clearManifest();
    if (mounted) {
        // New page requests now get the recovery page; let responses already
        // streaming a file finish before the filesystem goes away under them.
        delay(kUnmountDrainMs);
        LittleFS.end();
        mounted = false;
    }
}

void recoverAfterFailedFilesystemUpdate() {
    begin();
}

bool handlePageRequest(AsyncWebServerRequest *request) {
    if (request->method() != HTTP_GET || !acceptsHtml(request))
        return false;
    if (currentState == State::Ready && mounted) {
        AsyncWebServerResponse *response =
            request->beginResponse(LittleFS, kIndexPath, "text/html");
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    } else {
        sendRecoveryPage(request);
    }
    return true;
}

State state() {
    return currentState;
}

const char *stateName() {
    switch (currentState) {
    case State::Unavailable:
        return "unavailable";
    case State::MissingManifest:
        return "missing_manifest";
    case State::InvalidManifest:
        return "invalid_manifest";
    case State::Incompatible:
        return "incompatible";
    case State::Ready:
        return "ready";
    case State::Updating:
        return "updating";
    }
    return "unknown";
}

const char *version() {
    return currentVersion;
}
const char *requiredFirmware() {
    return currentRequiredFirmware;
}

void sendRecoveryPage(AsyncWebServerRequest *request) {
    String page{kRecoveryPage};
    page.replace("%FIRMWARE%", firmware::version);
    page.replace("%STATE%", stateName());
    page.replace("%SETUP%", status::setupRequired() ? "1" : "0");
    AsyncWebServerResponse *response = request->beginResponse(503, "text/html", page);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

} // namespace gateway::web_ui
