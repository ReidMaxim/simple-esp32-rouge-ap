/*
 * funnyportal.ino
 * ESP32 WiFi Captive Portal with 0.96" I2C OLED Display
 *
 * Boots into STA mode if saved credentials exist in SPIFFS,
 * otherwise starts an open AP ("SetupWiFi") with a captive portal.
 *
 * Two-tier web interface:
 *   - Public landing page: Hacker-themed ASCII art splash shown to anyone
 *     who connects. No auth required. This is what phones auto-open.
 *   - Admin panel (/admin): Form-based login (admin / esp32admin) that
 *     gates access to a dashboard showing current config, WiFi setup form,
 *     and a connection log viewer.
 *
 * Hardware:
 *   - ESP32 Dev Module
 *   - SSD1306 128x64 I2C OLED (SDA=GPIO18, SCL=GPIO33, VCC=3.3V)
 *
 * Security note: Admin credentials are hardcoded ("admin" / "esp32admin").
 * This is intentional for a demo/portfolio project and is NOT suitable for
 * production use. Anyone sniffing the unencrypted AP traffic can read them.
 */

// ============================================================
// Includes
// ============================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>

// ============================================================
// OLED Configuration
// ============================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_SDA      18
#define OLED_SCL      33
#define OLED_ADDR_PRIMARY   0x3C
#define OLED_ADDR_FALLBACK  0x3D

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================
// WiFi / AP Configuration
// ============================================================
#define AP_SSID       "SetupWiFi"
#define STA_RETRIES   20
#define STA_RETRY_MS  500

// Admin credentials for the config panel (insecure -- demo only)
#define AUTH_USER     "admin"
#define AUTH_PASS     "esp32admin"

// ============================================================
// Global Objects
// ============================================================
AsyncWebServer server(80);
DNSServer      dnsServer;

bool           apMode          = false;
unsigned long  lastOledRefresh = 0;

// ============================================================
// Shared CSS used across all admin pages (hacker theme)
// ============================================================
const char ADMIN_CSS[] PROGMEM = R"rawliteral(
*{box-sizing:border-box;margin:0;padding:0}
body{
  font-family:'Courier New',monospace;
  background:#0a0a0a;color:#00ff41;
  display:flex;justify-content:center;align-items:flex-start;
  min-height:100vh;padding:1.5rem;
}
.card{
  border:1px solid #00ff41;border-radius:8px;padding:1.5rem;
  width:100%;max-width:440px;background:rgba(0,255,65,0.03);
  position:relative;margin-top:1rem;
}
h1{font-size:1.1rem;margin-bottom:1.2rem;text-align:center;
   text-shadow:0 0 5px #00ff41;}
h2{font-size:.95rem;margin:1.2rem 0 .6rem;color:#7dffaa;
   border-bottom:1px solid #1a4d1a;padding-bottom:.3rem;}
label{display:block;font-size:.8rem;margin-bottom:.3rem;color:#0a8f2a}
input{
  width:100%;padding:.6rem .75rem;margin-bottom:1rem;
  border:1px solid #00ff41;border-radius:4px;
  background:#0a0a0a;color:#00ff41;font-family:inherit;font-size:.95rem;
}
input:focus{outline:none;box-shadow:0 0 8px rgba(0,255,65,0.4)}
.btn{
  display:inline-block;padding:.6rem 1rem;border:1px solid #00ff41;border-radius:4px;
  background:rgba(0,255,65,0.1);color:#00ff41;font-family:inherit;
  font-size:.9rem;cursor:pointer;font-weight:700;text-decoration:none;
  text-shadow:0 0 5px #00ff41;transition:background .2s;
  text-align:center;
}
.btn:hover{background:rgba(0,255,65,0.25)}
.btn-full{width:100%;display:block}
.btn-danger{border-color:#ff4141;color:#ff4141;text-shadow:0 0 5px #ff4141;
            background:rgba(255,65,65,0.08)}
.btn-danger:hover{background:rgba(255,65,65,0.2)}
.row{display:flex;gap:.6rem;margin-top:.8rem}
.row form,.row a{flex:1}
.row .btn{width:100%}
.info{background:rgba(0,255,65,0.05);border:1px solid #1a4d1a;
      border-radius:4px;padding:.6rem .8rem;margin-bottom:.4rem;font-size:.85rem}
.info span{color:#7dffaa}
.dim{color:#0a8f2a}
.err{color:#ff4141;text-align:center;margin-bottom:1rem;font-size:.85rem;
     text-shadow:0 0 5px #ff4141}
.logbox{
  background:#050505;border:1px solid #1a4d1a;border-radius:4px;
  padding:.8rem;font-size:.75rem;line-height:1.5;
  max-height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-all;
}
)rawliteral";

// ============================================================
// Public Landing Page -- hacker-themed ASCII art splash
// Served to everyone who connects (no auth). This is what the
// phone's Captive Network Assistant renders automatically.
// ============================================================
const char LANDING_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>funnyportal</title>
  <style>
    *{margin:0;padding:0;box-sizing:border-box}
    body{
      background:#0a0a0a;color:#00ff41;
      font-family:'Courier New',monospace;
      min-height:100vh;display:flex;flex-direction:column;
      align-items:center;justify-content:center;
      padding:1rem;overflow:hidden;
    }
    .scanline{
      position:fixed;top:0;left:0;width:100%;height:100%;
      background:repeating-linear-gradient(
        0deg,
        rgba(0,0,0,0) 0px,
        rgba(0,0,0,0) 2px,
        rgba(0,255,65,0.03) 2px,
        rgba(0,255,65,0.03) 4px
      );
      pointer-events:none;z-index:10;
    }
    .glow{
      text-shadow:0 0 5px #00ff41,0 0 10px #00ff41,0 0 20px #00ff41;
    }
    pre{
      font-size:clamp(0.45rem,1.8vw,0.85rem);
      line-height:1.2;text-align:center;
      white-space:pre;margin-bottom:1.5rem;
    }
    .terminal{
      border:1px solid #00ff41;border-radius:8px;
      padding:1.5rem;max-width:500px;width:100%;
      background:rgba(0,255,65,0.03);
      position:relative;
    }
    .terminal::before{
      content:'[ SYSTEM ACTIVE ]';
      position:absolute;top:-0.7rem;left:1rem;
      background:#0a0a0a;padding:0 0.5rem;
      font-size:0.7rem;color:#00ff41;
    }
    .line{margin:0.4rem 0;font-size:0.85rem;opacity:0;animation:typeIn 0.3s forwards}
    .line:nth-child(1){animation-delay:0.2s}
    .line:nth-child(2){animation-delay:0.6s}
    .line:nth-child(3){animation-delay:1.0s}
    .line:nth-child(4){animation-delay:1.4s}
    .line:nth-child(5){animation-delay:1.8s}
    .line:nth-child(6){animation-delay:2.2s}
    .line:nth-child(7){animation-delay:2.6s}
    .line:nth-child(8){animation-delay:3.0s}
    @keyframes typeIn{to{opacity:1}}
    .cursor{animation:blink 1s step-end infinite}
    @keyframes blink{50%{opacity:0}}
    .dim{color:#0a8f2a}
    .bright{color:#7dffaa}
    .warn{color:#ffae00;text-shadow:0 0 5px #ffae00}
  </style>
</head>
<body>
  <div class="scanline"></div>

  <pre class="glow">
 _  _   _   ___ _  __  _____ _  _ ___
| || | /_\ / __| |/ / |_   _| || | __|
| __ |/ _ \ (__| ' &lt;    | | | __ | _|
|_||_/_/ \_\___|_|\_\   |_| |_||_|___|
 ___ _      _   _  _ ___ _____
| _ \ |    /_\ | \| | __|_   _|
|  _/ |__ / _ \| .` | _|  | |
|_| |____/_/ \_\_|\_|___| |_|
  </pre>

  <div class="terminal">
    <div class="line"><span class="dim">$</span> connection established</div>
    <div class="line"><span class="dim">$</span> target: <span class="bright">192.168.4.1</span></div>
    <div class="line"><span class="dim">$</span> status: <span class="warn">INTERCEPTED</span></div>
    <div class="line"><span class="dim">$</span> scanning network interfaces...</div>
    <div class="line"><span class="dim">$</span> capturing handshake... <span class="bright">done</span></div>
    <div class="line"><span class="dim">$</span> decrypting payload...</div>
    <div class="line">&nbsp;</div>
    <div class="line"><span class="dim">$</span> <span class="warn">ACCESS GRANTED</span> <span class="cursor">_</span></div>
  </div>
</body>
</html>
)rawliteral";

// ============================================================
// Admin Login Page -- form-based auth (no HTTP Basic Auth)
// ============================================================
const char ADMIN_LOGIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Admin Login</title>
  <style>%CSS%
    .card::before{
      content:'[ AUTHENTICATION ]';
      position:absolute;top:-0.7rem;left:1rem;
      background:#0a0a0a;padding:0 0.5rem;font-size:0.7rem;
    }
  </style>
</head>
<body>
  <div class="card" style="max-width:360px;margin-top:20vh">
    <h1>// ADMIN LOGIN</h1>
    %ERROR%
    <form method="POST" action="/admin">
      <label>username</label>
      <input type="text" name="user" autocomplete="off" required>
      <label>password</label>
      <input type="password" name="pass" required>
      <button class="btn btn-full" type="submit">AUTHENTICATE</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

// ============================================================
// Helper: Show up to 3 lines on the OLED
// ============================================================
void oledMessage(const char* line1, const char* line2 = "", const char* line3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println(line1);

  if (line2[0] != '\0') {
    display.setCursor(0, 16);
    display.println(line2);
  }
  if (line3[0] != '\0') {
    display.setCursor(0, 32);
    display.println(line3);
  }

  display.display();
}

// ============================================================
// Helper: Read /config.txt -> ssid & password
// Returns true if SSID is non-empty after trimming.
// ============================================================
bool readConfig(String &ssid, String &password) {
  if (!SPIFFS.exists("/config.txt")) {
    Serial.println("[CONFIG] /config.txt not found.");
    return false;
  }

  File f = SPIFFS.open("/config.txt", "r");
  if (!f) {
    Serial.println("[CONFIG] Failed to open /config.txt.");
    return false;
  }

  ssid     = f.readStringUntil('\n');
  password = f.readStringUntil('\n');
  f.close();

  ssid.trim();
  password.trim();

  Serial.printf("[CONFIG] SSID: \"%s\"  Password length: %d\n", ssid.c_str(), password.length());
  return (ssid.length() > 0);
}

// ============================================================
// Helper: Read entire /log.txt into a String
// ============================================================
String readLogFile() {
  if (!SPIFFS.exists("/log.txt")) return "(no log entries yet)";

  File f = SPIFFS.open("/log.txt", "r");
  if (!f) return "(failed to read log)";

  String content = f.readString();
  f.close();

  if (content.length() == 0) return "(no log entries yet)";
  return content;
}

// ============================================================
// Helper: Append a line to /log.txt and echo to Serial
// ============================================================
void appendLog(const String &message) {
  Serial.println("[LOG] " + message);

  File f = SPIFFS.open("/log.txt", FILE_APPEND);
  if (f) {
    f.println(message);
    f.close();
  } else {
    Serial.println("[LOG] Failed to open /log.txt for writing.");
  }
}

// ============================================================
// Helper: Serve the admin login page, optionally with an error
// ============================================================
void serveLoginPage(AsyncWebServerRequest *request, bool showError) {
  String html = FPSTR(ADMIN_LOGIN_PAGE);
  html.replace("%CSS%", FPSTR(ADMIN_CSS));
  if (showError) {
    html.replace("%ERROR%", "<p class=\"err\">// INVALID CREDENTIALS</p>");
  } else {
    html.replace("%ERROR%", "");
  }
  request->send(200, "text/html", html);
}

// ============================================================
// Helper: Build and serve the admin dashboard (config + log link)
// Reads /config.txt dynamically so the current SSID is shown.
// ============================================================
void serveAdminDashboard(AsyncWebServerRequest *request) {
  String currentSSID, currentPass;
  bool hasCreds = readConfig(currentSSID, currentPass);

  String html = "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">"
    "<title>Admin Dashboard</title>"
    "<style>" + String(FPSTR(ADMIN_CSS)) + ""
    ".card::before{content:'[ ADMIN DASHBOARD ]';"
    "position:absolute;top:-0.7rem;left:1rem;"
    "background:#0a0a0a;padding:0 0.5rem;font-size:0.7rem;}"
    "</style></head><body><div class=\"card\">"
    "<h1>// ADMIN DASHBOARD</h1>";

  // --- Current config section ---
  html += "<h2>&gt; current config</h2>";
  html += "<div class=\"info\">SSID: <span>";
  html += hasCreds ? currentSSID : "(not configured)";
  html += "</span></div>";
  html += "<div class=\"info\">Password: <span>";
  html += hasCreds ? String(currentPass.length()) + " chars (hidden)" : "(none)";
  html += "</span></div>";
  html += "<div class=\"info\">Status: <span>";
  html += hasCreds ? "configured" : "awaiting setup";
  html += "</span></div>";

  // --- WiFi config form ---
  html += "<h2>&gt; update credentials</h2>"
    "<form method=\"POST\" action=\"/save\">"
    "<input type=\"hidden\" name=\"user\" value=\"admin\">"
    "<input type=\"hidden\" name=\"pass\" value=\"esp32admin\">"
    "<label>target SSID</label>"
    "<input type=\"text\" name=\"ssid\" required autocomplete=\"off\"";
  if (hasCreds) {
    html += " placeholder=\"" + currentSSID + "\"";
  }
  html += ">"
    "<label>passphrase</label>"
    "<input type=\"password\" name=\"wifipass\">"
    "<button class=\"btn btn-full\" type=\"submit\">DEPLOY &amp; REBOOT</button>"
    "</form>";

  // --- Log viewer + clear buttons ---
  html += "<h2>&gt; connection log</h2>"
    "<div class=\"row\">"
    "<form method=\"POST\" action=\"/log\">"
    "<input type=\"hidden\" name=\"user\" value=\"admin\">"
    "<input type=\"hidden\" name=\"pass\" value=\"esp32admin\">"
    "<button class=\"btn\" type=\"submit\">VIEW LOG</button>"
    "</form>"
    "<form method=\"POST\" action=\"/clearlog\">"
    "<input type=\"hidden\" name=\"user\" value=\"admin\">"
    "<input type=\"hidden\" name=\"pass\" value=\"esp32admin\">"
    "<button class=\"btn btn-danger\" type=\"submit\">CLEAR LOG</button>"
    "</form>"
    "</div>";

  html += "</div></body></html>";

  request->send(200, "text/html", html);
}

// ============================================================
// Helper: Build and serve the log viewer page
// ============================================================
void serveLogViewer(AsyncWebServerRequest *request) {
  String logContent = readLogFile();

  // Escape HTML entities in log content
  logContent.replace("&", "&amp;");
  logContent.replace("<", "&lt;");
  logContent.replace(">", "&gt;");

  String html = "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">"
    "<title>Connection Log</title>"
    "<style>" + String(FPSTR(ADMIN_CSS)) + ""
    ".card::before{content:'[ CONNECTION LOG ]';"
    "position:absolute;top:-0.7rem;left:1rem;"
    "background:#0a0a0a;padding:0 0.5rem;font-size:0.7rem;}"
    "</style></head><body><div class=\"card\">"
    "<h1>// CONNECTION LOG</h1>"
    "<div class=\"logbox\">" + logContent + "</div>"
    "<div class=\"row\" style=\"margin-top:1rem\">"
    "<form method=\"POST\" action=\"/admin\">"
    "<input type=\"hidden\" name=\"user\" value=\"admin\">"
    "<input type=\"hidden\" name=\"pass\" value=\"esp32admin\">"
    "<button class=\"btn\" type=\"submit\">&lt; BACK</button>"
    "</form>"
    "<form method=\"POST\" action=\"/clearlog\">"
    "<input type=\"hidden\" name=\"user\" value=\"admin\">"
    "<input type=\"hidden\" name=\"pass\" value=\"esp32admin\">"
    "<button class=\"btn btn-danger\" type=\"submit\">CLEAR LOG</button>"
    "</form>"
    "</div>"
    "</div></body></html>";

  request->send(200, "text/html", html);
}

// ============================================================
// WiFi Event Handler (ESP32 Arduino Core)
// Logs MAC addresses of devices joining/leaving the soft-AP.
// ============================================================
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  uint8_t* mac;
  char macStr[18];

  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      mac = info.wifi_ap_staconnected.mac;
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      appendLog("CONNECTED: " + String(macStr));
      break;

    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      mac = info.wifi_ap_stadisconnected.mac;
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      appendLog("DISCONNECTED: " + String(macStr));
      break;

    default:
      break;
  }
}

// ============================================================
// Start Captive Portal (AP + DNS + Web Server)
// ============================================================
void startCaptivePortal() {
  apMode = true;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  delay(100);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  Serial.printf("[AP] Started \"%s\"  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  oledMessage("AP Mode Active", "SSID: SetupWiFi", WiFi.softAPIP().toString().c_str());

  WiFi.onEvent(onWiFiEvent);

  // DNS catch-all: resolve every domain to our AP IP
  dnsServer.start(53, "*", apIP);

  // ---- Web Server Routes ----

  // Captive portal detection handlers.
  // Returning a 302 redirect to /portal reliably triggers the CNA
  // (Captive Network Assistant) auto-popup on phones and laptops.
  const char* portalRedirect = "http://192.168.4.1/portal";

  server.on("/generate_204", HTTP_GET, [portalRedirect](AsyncWebServerRequest *request) {
    request->redirect(portalRedirect);
  });
  server.on("/gen_204", HTTP_GET, [portalRedirect](AsyncWebServerRequest *request) {
    request->redirect(portalRedirect);
  });
  server.on("/hotspot-detect.html", HTTP_GET, [portalRedirect](AsyncWebServerRequest *request) {
    request->redirect(portalRedirect);
  });
  server.on("/connecttest.txt", HTTP_GET, [portalRedirect](AsyncWebServerRequest *request) {
    request->redirect(portalRedirect);
  });
  server.on("/ncsi.txt", HTTP_GET, [portalRedirect](AsyncWebServerRequest *request) {
    request->redirect(portalRedirect);
  });
  server.on("/redirect", HTTP_GET, [portalRedirect](AsyncWebServerRequest *request) {
    request->redirect(portalRedirect);
  });
  server.on("/success.txt", HTTP_GET, [portalRedirect](AsyncWebServerRequest *request) {
    request->redirect(portalRedirect);
  });

  // /portal serves the public landing page (target of all CNA redirects)
  server.on("/portal", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", LANDING_PAGE);
  });

  // GET /admin -> login form
  server.on("/admin", HTTP_GET, [](AsyncWebServerRequest *request) {
    serveLoginPage(request, false);
  });

  // POST /admin -> validate credentials, show dashboard or error
  server.on("/admin", HTTP_POST, [](AsyncWebServerRequest *request) {
    String user = request->hasParam("user", true) ? request->getParam("user", true)->value() : "";
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";

    if (user == AUTH_USER && pass == AUTH_PASS) {
      serveAdminDashboard(request);
    } else {
      serveLoginPage(request, true);
    }
  });

  // POST /log -> view connection log (auth required)
  server.on("/log", HTTP_POST, [](AsyncWebServerRequest *request) {
    String user = request->hasParam("user", true) ? request->getParam("user", true)->value() : "";
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";

    if (user != AUTH_USER || pass != AUTH_PASS) {
      request->send(403, "text/plain", "Forbidden.");
      return;
    }
    serveLogViewer(request);
  });

  // POST /clearlog -> wipe log.txt, return to dashboard (auth required)
  server.on("/clearlog", HTTP_POST, [](AsyncWebServerRequest *request) {
    String user = request->hasParam("user", true) ? request->getParam("user", true)->value() : "";
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";

    if (user != AUTH_USER || pass != AUTH_PASS) {
      request->send(403, "text/plain", "Forbidden.");
      return;
    }

    SPIFFS.remove("/log.txt");
    Serial.println("[LOG] Log cleared by admin.");
    appendLog("LOG CLEARED BY ADMIN");
    serveAdminDashboard(request);
  });

  // POST /save -> validate admin creds, write config, restart
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    String user = request->hasParam("user", true) ? request->getParam("user", true)->value() : "";
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";

    if (user != AUTH_USER || pass != AUTH_PASS) {
      request->send(403, "text/plain", "Forbidden.");
      return;
    }

    String newSSID    = request->hasParam("ssid", true)     ? request->getParam("ssid", true)->value()     : "";
    String newWifiPass = request->hasParam("wifipass", true) ? request->getParam("wifipass", true)->value() : "";

    newSSID.trim();
    newWifiPass.trim();

    if (newSSID.length() == 0) {
      request->send(400, "text/plain", "SSID cannot be empty.");
      return;
    }

    File f = SPIFFS.open("/config.txt", "w");
    if (f) {
      f.println(newSSID);
      f.println(newWifiPass);
      f.close();
      Serial.printf("[SAVE] New SSID: \"%s\"\n", newSSID.c_str());
      appendLog("CONFIG CHANGED: SSID=" + newSSID);
    } else {
      Serial.println("[SAVE] Failed to write /config.txt");
      request->send(500, "text/plain", "File write error.");
      return;
    }

    request->send(200, "text/html",
      "<html><body style='background:#0a0a0a;color:#00ff41;display:flex;"
      "justify-content:center;align-items:center;height:100vh;"
      "font-family:Courier New,monospace;text-align:center'>"
      "<div><pre style='font-size:1.2rem;text-shadow:0 0 10px #00ff41'>"
      "CREDENTIALS SAVED\n\nREBOOTING...</pre></div></body></html>");

    delay(1500);
    ESP.restart();
  });

  // Catch-all: serve the public landing page directly (200 OK).
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", LANDING_PAGE);
  });

  server.begin();
  Serial.println("[WEB] AsyncWebServer started on port 80.");
}

// ============================================================
// setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n========== funnyportal ==========");

  // --- I2C & OLED ---
  Wire.begin(OLED_SDA, OLED_SCL);

  bool oledOK = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_PRIMARY);
  if (!oledOK) {
    Serial.println("[OLED] 0x3C failed, trying 0x3D...");
    oledOK = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_FALLBACK);
  }
  if (!oledOK) {
    Serial.println("[OLED] FATAL: Display init failed. Check wiring.");
    while (true) { delay(1000); }
  }
  Serial.println("[OLED] Initialized.");
  oledMessage("Starting...");

  // --- SPIFFS ---
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Mount failed even after format.");
    oledMessage("SPIFFS Error!", "Halting.");
    while (true) { delay(1000); }
  }
  Serial.println("[SPIFFS] Mounted.");

  // --- Read saved WiFi credentials ---
  String savedSSID, savedPass;
  bool hasCreds = readConfig(savedSSID, savedPass);

  if (hasCreds) {
    oledMessage("Connecting to:", savedSSID.c_str());
    Serial.printf("[WIFI] Connecting to \"%s\" ...\n", savedSSID.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < STA_RETRIES) {
      delay(STA_RETRY_MS);
      retries++;
      Serial.printf("[WIFI] Attempt %d/%d\n", retries, STA_RETRIES);
    }

    if (WiFi.status() == WL_CONNECTED) {
      String ip = WiFi.localIP().toString();
      Serial.printf("[WIFI] Connected! IP: %s\n", ip.c_str());
      oledMessage("Connected!", ("IP: " + ip).c_str());
      appendLog("STA CONNECTED: " + savedSSID + " IP=" + ip);
      return;
    }

    Serial.println("[WIFI] STA connection failed. Falling back to AP mode.");
  }

  startCaptivePortal();
}

// ============================================================
// loop()
// ============================================================
void loop() {
  if (!apMode) return;

  dnsServer.processNextRequest();

  unsigned long now = millis();
  if (now - lastOledRefresh >= 1000) {
    lastOledRefresh = now;

    int clients = WiFi.softAPgetStationNum();
    char countBuf[24];
    snprintf(countBuf, sizeof(countBuf), "Connected: %d", clients);

    oledMessage("AP Mode", countBuf, "SSID: SetupWiFi");
  }
}
