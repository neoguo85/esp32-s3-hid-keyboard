#include "USB.h"
#include "USBHIDKeyboard.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>

USBHIDKeyboard Keyboard;
WebServer server(80);

#define BTN_BOOT 0
unsigned long lastPress = 0;
const unsigned long debounce = 300;
const int CHAR_DELAY = 80;
const int HOLD_DELAY = 20;

const char* STA_SSID = "YOUR_WIFI_SSID";
const char* STA_PASS = "YOUR_WIFI_PASSWORD";
const char* AP_SSID = "ESP32_HID_KEYBOARD";   // 同房间多台设备时必须改成互不相同的名字
const char* AP_PASS = "12345678";             // 建议一并改掉，避免连错别人的板子
const char* MDNS_NAME = "esp32-hid";          // 局域网内需唯一，多台时加后缀，如 esp32-hid-neo

String inputText;
int errLine = 0;
int errCol = 0;

bool isTyping = false;
bool isPaused = false;
int typeIndex = 0;
unsigned long typeTimer = 0;
bool keyHeld = false;

const char* PAGE = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HID Keyboard Config</title>
<style>
  body { font-family: system-ui, sans-serif; max-width: 700px; margin: 30px auto; padding: 0 20px; }
  textarea { width: 100%; height: 320px; padding: 10px; font-size: 14px; box-sizing: border-box; white-space: pre; }
  .btn-group { display: flex; gap: 10px; margin-top: 12px; }
  button { flex: 1; padding: 12px; font-size: 15px; border: none; border-radius: 6px; cursor: pointer; color: #fff; }
  .btn-save { background: #2563eb; }
  .btn-send { background: #059669; }
  .btn-stop { background: #dc2626; }
  .btn-locate { background:#d97706; color:#fff; padding:6px 10px; border:none; border-radius:4px; cursor:pointer; margin-left:8px; }
  .tip { color: #666; font-size: 13px; margin-top: 16px; line-height: 1.6; }
  .status-ok { color:#059669; padding:10px; background:#ecfdf5; border-radius:6px; margin-bottom:12px; }
  .status-fail { color:#dc2626; padding:10px; background:#fef2f2; border-radius:6px; margin-bottom:12px; }
  .warn { color:#d97706; padding:10px; background:#fffbeb; border-radius:6px; margin-bottom:12px; display:flex; align-items:center; flex-wrap:wrap; }
  .run-status { padding:8px 10px; background:#f3f4f6; border-radius:6px; margin-bottom:12px; font-weight:bold; }
</style>
</head>
<body>
  <h3>HID Keyboard Text Config</h3>
  <div class="run-status">Status: %RUN_STATUS%</div>
  %TIP%
  %WARN%
  <form method="POST" action="/save" id="form">
    <textarea id="txt" name="text" placeholder="Enter text to type...">%TEXT%</textarea>
    <div class="btn-group">
      <button type="submit" class="btn-save">Save Text</button>
    </div>
  </form>
  <div class="btn-group">
    <form method="POST" action="/send" style="flex:1">
      <button type="submit" class="btn-send">Send Now</button>
    </form>
    <form method="POST" action="/stop" style="flex:1">
      <button type="submit" class="btn-stop">Stop Typing</button>
    </form>
  </div>
  <div class="tip">
    - Click <b>Send Now</b> to start typing immediately via web<br>
    - Single press BOOT: start typing / pause-resume<br>
    - Click <b>Stop Typing</b> to cancel at any time<br>
    - Text saved to flash and persists after power off<br>
    - Unsupported character check runs when you click Save
  </div>

<script>
const targetLine = %ERR_LINE%;
const targetCol = %ERR_COL%;
const ta = document.getElementById('txt');
ta.focus();

function goLine(){
  if(!targetLine) return;
  const lines = ta.value.split('\n');
  let pos = 0;
  for(let i=0; i < targetLine-1; i++){
    pos += lines[i].length + 1;
  }
  ta.focus();
  ta.selectionStart = pos + targetCol - 1;
  ta.selectionEnd = ta.selectionStart + 1;
  ta.scrollTop = ta.scrollHeight * (ta.selectionStart / ta.value.length);
}
</script>
</body>
</html>
)HTML";

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() * 1.2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '"':  out += "&quot;"; break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      default:   out += c;        break;
    }
  }
  return out;
}

void releaseAllKeys() {
  Keyboard.releaseAll();
  keyHeld = false;
}

bool isSupportedChar(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  switch(c) {
    case '!': case '@': case '#': case '$': case '%': case '^': case '&': case '*':
    case '(': case ')': case '_': case '+': case '-': case '=': case '[': case ']':
    case '{': case '}': case ';': case '\'': case ':': case '"': case ',': case '.':
    case '/': case '<': case '>': case '?': case '\\': case '|': case '`': case '~':
    case ' ': case '\n': case '\r': return true;
    default: return false;
  }
}

bool scanFirstUnsupported(const String &txt) {
  errLine = 1;
  errCol = 1;
  for (size_t i = 0; i < txt.length(); i++) {
    char c = txt[i];
    if (c == '\n') {
      errLine++;
      errCol = 1;
      continue;
    }
    if (!isSupportedChar(c)) {
      return true;
    }
    errCol++;
  }
  errLine = 0;
  errCol = 0;
  return false;
}

void startTyping() {
  if (inputText.length() == 0) return;
  releaseAllKeys();
  typeIndex = 0;
  isTyping = true;
  isPaused = false;
  keyHeld = false;
  typeTimer = millis() - CHAR_DELAY;  // 回拨一个间隔，让首字符立即触发
}

void stopTyping() {
  isTyping = false;
  isPaused = false;
  releaseAllKeys();
}

void processTyping() {
  if (!isTyping || isPaused) return;

  unsigned long now = millis();

  if (typeIndex >= (int)inputText.length()) {
    isTyping = false;
    releaseAllKeys();
    return;
  }

  if (!keyHeld) {
    // 字符间隔门禁：上一个字符释放后必须等够 CHAR_DELAY 才按下一个
    if (now - typeTimer < (unsigned long)CHAR_DELAY) return;

    char c = inputText[typeIndex];

    // \r 已在存/取时归一化，这里再兜一层：残留 \r 直接跳过，避免重复回车
    if (c == '\r') {
      typeIndex++;
      typeTimer = now;
      return;
    }

    if (!isSupportedChar(c)) {
      typeIndex++;
      typeTimer = now;
      return;
    }

    if (c >= 'A' && c <= 'Z') {
      Keyboard.press(KEY_LEFT_SHIFT);
      Keyboard.press(c);
    }
    else if(c == '~') {
      Keyboard.press(KEY_LEFT_SHIFT);
      Keyboard.press('`');
    }
    else if(c == '|') {
      Keyboard.press(KEY_LEFT_SHIFT);
      Keyboard.press('\\');
    }
    else if(c == '\n') {
      Keyboard.press(KEY_RETURN);
    }
    else {
      Keyboard.press(c);
    }
    keyHeld = true;
    typeTimer = now;
  } else {
    if (now - typeTimer >= (unsigned long)HOLD_DELAY) {
      releaseAllKeys();
      keyHeld = false;
      typeIndex++;
      typeTimer = now;
    }
  }
}

// 统一换行为 \n。textarea 提交的是 \r\n，若 \r 和 \n 都发 RETURN 会变成每行多一个空行
void normalizeNewlines(String &s) {
  if (s.indexOf('\r') < 0) return;
  s.replace("\r\n", "\n");
  s.replace("\r", "\n");
}

void loadText() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    inputText = "Hello NEO";
    errLine = 0; errCol = 0;
    return;
  }

  File file = LittleFS.open("/text.txt", "r");
  if (file) {
    inputText = file.readString();
    file.close();
    normalizeNewlines(inputText);
    Serial.printf("Loaded text, len: %d\n", inputText.length());
  } else {
    inputText = "Hello NEO";
    Serial.println("No saved text, use default");
  }
  scanFirstUnsupported(inputText);
}

bool saveText(String t) {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return false;
  }

  normalizeNewlines(t);

  File file = LittleFS.open("/text.txt", "w");
  if (!file) {
    Serial.println("Cannot open file for write");
    return false;
  }

  size_t written = file.print(t);
  file.close();

  if (written == t.length()) {
    inputText = t;
    scanFirstUnsupported(inputText);
    Serial.printf("Saved, written %d bytes\n", written);
    return true;
  } else {
    Serial.printf("Save incomplete, want %d, wrote %d\n", t.length(), written);
    return false;
  }
}

void handleRoot() {
  String p = PAGE;
  p.replace("%TEXT%", htmlEscape(inputText));
  p.replace("%ERR_LINE%", String(errLine));
  p.replace("%ERR_COL%", String(errCol));

  String runStatus = "Idle";
  if(isTyping && isPaused) runStatus = "Paused";
  else if(isTyping) runStatus = "Typing";
  p.replace("%RUN_STATUS%", runStatus);

  String tipHtml = "";
  if (server.hasArg("status")) {
    if (server.arg("status") == "ok") {
      tipHtml = "<div class='status-ok'>✅ Text saved</div>";
    } else if (server.arg("status") == "fail") {
      tipHtml = "<div class='status-fail'>❌ Save failed</div>";
    }
  }
  p.replace("%TIP%", tipHtml);

  String warnHtml = "";
  if(errLine > 0){
    warnHtml = "<div class='warn'>⚠ HID unsupported character found at line " + String(errLine) + ", column " + String(errCol) + ", it will be skipped when typing "
    + "<button class='btn-locate' onclick='goLine()'>Locate</button></div>";
  }
  p.replace("%WARN%", warnHtml);

  server.send(200, "text/html; charset=utf-8", p);
}

void handleSave() {
  bool success = false;
  if (server.hasArg("text")) {
    success = saveText(server.arg("text"));
  }
  String redirectUrl = success ? "/?status=ok" : "/?status=fail";
  server.sendHeader("Location", redirectUrl);
  server.send(302, "text/plain", "");
}

void handleSend() {
  startTyping();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleStop() {
  stopTyping();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void initWiFi() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);

  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 10000) {
    delay(500);
    t += 500;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("STA OK, IP: " + WiFi.localIP().toString());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("AP mode, IP: 192.168.4.1");
  }
  MDNS.begin(MDNS_NAME);
  Serial.println("mDNS: " + String(MDNS_NAME) + ".local");
}

void setup() {
  pinMode(BTN_BOOT, INPUT_PULLUP);
  USB.begin();
  Keyboard.begin();
  loadText();
  initWiFi();

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/stop", HTTP_POST, handleStop);

  server.begin();
}

void loop() {
  server.handleClient();
  processTyping();

  unsigned long now = millis();
  bool btnPressed = (digitalRead(BTN_BOOT) == LOW);

  // 单击触发，防抖
  if (btnPressed && (now - lastPress > debounce)) {
    lastPress = now;
    if(isTyping) {
      isPaused = !isPaused;
      Serial.printf("Paused = %d\n", isPaused);
    } else {
      startTyping();
    }
  }
}
