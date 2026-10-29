# ESP32-S3 HID 键盘输入器 · 从刷机到落地全流程

> 用途：在**禁止剪贴板 / 禁止粘贴 / 禁止文件拖拽**的受控环境（客户 VPC、跳板机、KVM、远程桌面）中，用一块 ESP32-S3 伪装成标准 USB 键盘，把预置文本逐字符"敲"进目标输入框，替代人工手打测试数据。
>
> 版本：v1.1 · 2026-10-29 · 适用于 ESP32-S3-DevKitC-1 兼容板（N16R8）
>
> v1.1 修订：修复固件 `CHAR_DELAY` 字符间隔失效（打字过快易漏字）、`\r\n` 被当成两次回车（行间多空行）两处缺陷；第 4.2 节代码为最终可用版本。

---

## 0. 使用边界（先看这段）

这个工具本质是一块**普通 USB 键盘**，不碰系统、不注入进程、不读剪贴板，因此不触发 DLP / 剪贴板管控策略。但：

- 只在**你被授权操作**的设备与账号上使用；客户环境用之前，确认 HID 外设接入在安全策略允许范围内。
- 输入内容会**明文存放在板子 Flash 里**，用完记得清空（网页里把文本改成空串再 Save）。
- 固件里硬编码了 WiFi 账号密码，别把这块板子当公共设备借人。

---

## 1. 硬件要求

### 1.1 板子规格

| 项 | 值 |
|---|---|
| 模组 | 乐鑫 ESP32-S3-WROOM-1-**N16R8** |
| 开发板 | 任意兼容官方 ESP32-S3-DevKitC-1 的板子 |
| 建议 | **已焊接排针**版，方便反复插拔与按 BOOT / EN |

### 1.2 为什么是 N16R8

- **N16** = 16MB Flash → 用 `default_16MB` 分区表，LittleFS 能存很长的文本（几万字没问题）。
- **R8** = 8MB **OPI** PSRAM → 编译时必须指定 `psram_type=opi`，否则起不来或 PSRAM 不可用。
- **N8R2 / N4R2** 也能跑，但 Flash 只有 8MB/4MB，长文本容易存不下，且分区表参数要跟着改。

### 1.3「已焊接排针」的作用

BOOT / EN 两个按键的触发、以及双 Type-C 口的插拔都要手上直接操作；排针焊好省一次动手，且模块本体裸露容易压坏。

### 1.4 收货检查

1. 板子两个 Type-C 口：丝印 **UART**（串口下载口，板载 USB-UART 芯片）和 **USB**（原生 USB，HID 输出口）。
2. 上电后电源灯亮；插 UART 口时 Mac「系统信息 → USB」能看到串口芯片（CH340 / CP2102 / CH343）。
3. 附送的数据线**必须是数据线**，纯充电线认不到设备——这是后面 `board list` 空白的头号原因。

---

## 2. 环境准备（macOS）

```bash
# 安装 arduino-cli
brew install arduino-cli

# 验证
arduino-cli version
```

串口驱动：

| 板载芯片 | macOS 处理 |
|---|---|
| CH340 / CH341 | Ventura 及以上免驱 |
| CP2102 / CP2102N | Ventura 及以上免驱 |
| CH343 / CH9102 | 需装 WCH 官方驱动（wch.cn） |

装完驱动插线，用 `ls /dev/cu.*` 应能看到 `cu.wchusbserial*` / `cu.usbserial-*` / `cu.SLAB_USBtoUART`。

---

## 3. arduino-cli 初始化与板包安装

### 3.1 初始化配置

```bash
arduino-cli config init
```

### 3.2 写入 ESP32 板包地址（国内镜像）

```bash
arduino-cli config add board_manager.additional_urls \
  https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json
arduino-cli config save
```

### 3.3 更新索引、安装板包

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.10-cn
```

> 首次安装会拉 1GB+ 工具链（xtensa 编译器、esptool 等），镜像下 5~15 分钟。中断了直接重跑同一条命令，已下载的包不会重复拉。
>
> 指定 `3.3.10-cn` 是为了锁死版本——Arduino 生态 API 变动频繁，`USBHIDKeyboard` 在 3.x 前后签名不同，跟着最新版走随时可能编译不过。

### 3.4 安装 Adafruit TinyUSB 库

```bash
arduino-cli lib install "Adafruit TinyUSB Library"
```

> 说明：本工程实际用的是 ESP32 core 内置的 `USB.h` / `USBHIDKeyboard.h`（走 core 自带的 TinyUSB 后端），装 Adafruit TinyUSB 是为了补齐依赖与头文件路径。别同时装别的 USB HID 库，会撞符号。

---

## 4. 工程与固件

### 4.1 建目录

```bash
mkdir -p ~/esp32_hid_key
cd ~/esp32_hid_key
nano esp32_hid_key.ino
```

> Arduino 要求 **文件夹名 == .ino 文件名**，否则 compile 报 "main file missing"。这里 `esp32_hid_key/esp32_hid_key.ino` 是对的。
>
> 本文档同目录已附带可直接烧录的 `esp32_hid_key/esp32_hid_key.ino`（与 4.2 节代码逐字节一致，已实测编译通过），可直接跳到第 5 章。

### 4.2 完整固件代码

```cpp
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
```

### 4.3 上机前必改的配置

```cpp
const char* STA_SSID  = "YOUR_WIFI_SSID";     // 必改：你自己的手机热点 / 局域网 SSID
const char* STA_PASS  = "YOUR_WIFI_PASSWORD"; // 必改：对应密码
const char* AP_SSID   = "ESP32_HID_KEYBOARD"; // 同房间多台时必须改，见下
const char* AP_PASS   = "12345678";           // 建议一并改掉
const char* MDNS_NAME = "esp32-hid";          // 局域网内需唯一，多台时加后缀
```

前两个占位符**必须替换成真实值**再编译。不替换也能烧进去，但板子连不上热点，10 秒后自动退到 AP 模式（`ESP32_HID_KEYBOARD` / `12345678` / `192.168.4.1`）——能用，只是每次配文本都得手动连板子的热点。

建议用**手机热点**而不是公司 WiFi：客户现场 / 办公网常年开着 802.1X 或客户端隔离，ESP32 连不上也 ping 不通。

#### ⚠️ 多人同房间使用：AP 名称必须各自改掉

默认的 `ESP32_HID_KEYBOARD` / `esp32-hid` 是 **所有人都一样**的。如果一间办公室/会议室里有两块以上这样的板子同时上电：

- 手机 WiFi 列表里出现**两个同名热点** `ESP32_HID_KEYBOARD`，分不清哪个是自己的；
- AP 模式下的 IP 又都是 `192.168.4.1` —— 连错了完全无感，你在别人的板子上配了一通文本，自己这块压根没收到；
- 若都接在同一局域网，`esp32-hid.local` 的 mDNS 名字也会撞，谁先广播谁占住，另一块的网页可能根本打不开。

**约定一个命名规则再动手**，三条都改：

| 常量 | 改成 | 示例 |
|---|---|---|
| `AP_SSID` | `ESP32_HID_<你的标识>` | `ESP32_HID_NEO`、`ESP32_HID_LX` |
| `AP_PASS` | 别都用 `12345678` | `neo12345` |
| `MDNS_NAME` | 同上加后缀 | `esp32-hid-neo` |

改完配文本就访问 `http://esp32-hid-neo.local`（AP 模式下仍是 `192.168.4.1`，但热点名不会连错）。建议在板子上贴个标签写上自己的 AP 名，插一排板子时最省事。

---

## 5. 编译

### 5.1 参数逐项对照

| 参数 | 值 | 为什么必须是这个 |
|---|---|---|
| `--fqbn` | `esp32:esp32:esp32s3` | ESP32-S3 Dev Module |
| `build.partitions` | `default_16MB` | 匹配 N16 的 16MB Flash，给 LittleFS 留足空间 |
| `build.flash_size` | `16MB` | 同上，写错会烧完起不来 |
| `build.psram_type` | `opi` | R8 = **Octal** PSRAM。写成 `qspi` 会导致 PSRAM 初始化失败甚至 boot loop |
| `build.usb_cdc_on_boot` | `0` | **最关键的一项**。设为 1 时原生 USB 会枚举成 CDC 串口，HID 键盘要么不出现要么和串口抢枚举，目标机认不到键盘 |
| `build.flashmode` | `qio` | 常规四线模式，兼容性最好 |
| `build.flashfreq` | `80m` | 稳定值，120m 部分板子不稳 |
| `build.extra_flags` | `-DHTTP_MAX_POST_SIZE=65536` | WebServer 默认 POST body 上限只有 16KB，长文本 Save 会静默失败 |

> ⚠️ `build.extra_flags=` 会**整体覆盖**板包原有的 extra_flags。若之后出现奇怪的编译/运行行为，改用追加式写法更安全：
> ```
> --build-property "compiler.cpp.extra_flags=-DHTTP_MAX_POST_SIZE=65536"
> ```

### 5.2 编译命令

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3 \
  --build-property build.partitions=default_16MB \
  --build-property build.flash_size=16MB \
  --build-property build.psram_type=opi \
  --build-property build.usb_cdc_on_boot=0 \
  --build-property build.flashmode=qio \
  --build-property build.flashfreq=80m \
  --build-property "build.extra_flags=-DHTTP_MAX_POST_SIZE=65536" \
  ~/esp32_hid_key
```

首次编译约 1~3 分钟。**本代码实测输出**（arduino-cli 1.5.1 + esp32 3.3.10-cn，2026-10-29）：

```
Sketch uses 1087131 bytes (82%) of program storage space. Maximum is 1310720 bytes.
Global variables use 81376 bytes (24%) of dynamic memory, leaving 246304 bytes for local variables. Maximum is 327680 bytes.
```

> ⚠️ `default_16MB` 分区表给 app 的只有 1.25MB，当前已占 **82%**。这版够用，但**别再往这个 sketch 里堆功能**（加 WebSocket、OTA、TLS 之类会直接超）。真要扩，把 `build.partitions` 换成 `huge_app`，代价是失去 OTA 分区。

出现 `error: 'USBHIDKeyboard' was not declared` 之类，八成是板包版本不对（不是 `3.3.10-cn`）——用 `arduino-cli core list` 核对。

---

## 6. 烧录

### 6.1 硬件操作顺序

1. 数据线插板子**右侧 UART 口**（下载/调试口，同时供电）。
2. **按住 BOOT 不放 → 点一下 EN（复位）→ 松开 EN → 保持 BOOT 约 1 秒再松开** → 进入下载模式。
3. 查端口：

```bash
arduino-cli board list
```

看到类似 `/dev/cu.wchusbserial1420  esp32  ESP32-S3 Dev Module` 即可。

> 端口名随板载芯片不同：`cu.wchusbserial*`（CH340）、`cu.usbserial-*` / `cu.SLAB_USBtoUART`（CP2102）、`cu.usbmodem*`（原生 USB）。

### 6.2 上传

```bash
arduino-cli upload \
  -p /dev/cu.wchusbserial1420 \
  --fqbn esp32:esp32:esp32s3 \
  ~/esp32_hid_key
```

> upload 阶段如果报 `Failed to connect`，说明没进下载模式，回 6.1 第 2 步重来（这步失败率最高，别怀疑代码）。
>
> 如果 upload 用的参数和 compile 不一致，烧进去的分区表/PSRAM 配置会错。要么两次命令都带全套 `--build-property`，要么先 `compile --output-dir=build` 再 `upload --input-dir=build`。**推荐后者**：
> ```bash
> arduino-cli compile --output-dir=build <全套参数> ~/esp32_hid_key
> arduino-cli upload -p /dev/cu.wchusbserial1420 --fqbn esp32:esp32:esp32s3 --input-dir=build ~/esp32_hid_key
> ```

### 6.3 烧完

1. 按一下 EN 复位。
2. **拔掉右口**（UART）。
3. **左口（原生 USB）**只剩 HID 键盘功能 —— 插到目标机，系统识别为普通 USB Keyboard。

---

## 7. 使用方法

### 7.1 配文本（在 Mac 上做）

1. 板子插右口 UART 口供电（此时可看串口日志：`arduino-cli monitor -p /dev/cu.wchusbserial1420 -c baudrate=115200`）。
2. 手机/Mac 连到固件里配置的那个热点，和板子同网段。
3. 浏览器打开 `http://esp32-hid.local`（mDNS，改成自定义名字后相应变成 `esp32-hid-neo.local` 之类），不通就用串口日志里打印的 IP。
   - 没连上热点时板子自动开 AP：SSID `ESP32_HID_KEYBOARD`，密码 `12345678`，访问 `http://192.168.4.1`。
   - **多人同房间时，这几项默认值会撞车** —— 各自改掉，见 [4.3](#43-上机前必改的配置)。
4. 粘贴文本 → **Save Text**（写入 Flash，断电不丢）。
   - 若文本含 HID 打不出的字符，页面顶部会黄条提示「第 X 行第 Y 列」，点 **Locate** 直接定位。
5. 也可以在页面上直接点 **Send Now** 让板子立刻开始敲（此时焦点必须在目标机的输入框里）。

### 7.2 实战投递（关键）

标准流程是**先配文本、再把板子插到目标机**，这样不用在客户环境里开网页：

```
Mac 配好文本 → 拔线 → 左口插目标机 → 点目标输入框 → 按 BOOT 开始敲
```

远程场景（VPC / 跳板机 / 云桌面）：

| 场景 | 做法 |
|---|---|
| VPC 客户端支持 USB 重定向（Citrix / Horizon / RDP USB 重定向） | 把这块 HID 设备重定向进远端，输入直接进 VPC |
| 不支持 USB 重定向 | 让 VPC 窗口全屏并保持焦点，HID 输入随焦点进入远端；**中途别切窗口，切了字符会打到本地** |
| KVM / iLO / iDRAC / 带外管理 | 走虚拟键盘通道，等同物理键盘 |

### 7.3 按键操作

| 操作 | 行为 |
|---|---|
| 单击 BOOT | 空闲时：开始输入；输入中：暂停 / 继续 |
| 网页 **Stop Typing** | 立即中止并释放所有按键 |
| 网页 **Send Now** | 从头开始输入（不重新配文本） |

> BOOT 是电平触发 + 300ms 防抖，**按住不放会每 300ms 切换一次状态**（暂停→继续→暂停…）。点一下松手即可。

### 7.4 字符能力边界

| 支持 | 不支持 |
|---|---|
| A–Z / a–z / 0–9 | 中文、日文、emoji、任何非 ASCII 字符 |
| `! @ # $ % ^ & * ( ) _ + - = [ ] { } ; ' : " , . / < > ? \ | ` ~` 空格 换行 | Tab、方向键、Home/End、Ctrl/Alt 组合键、功能键 |
| 大写自动加 Shift | — |

两个直接后果：

1. **中文内容打不了**。本固件是"逐字符映射 US 键盘 scancode"，没有输入法能力。要打中文得另做方案（拼音输入法 + 候选词选择，极不稳定，不建议）。
2. **目标机必须处于英文输入状态**。中文 Windows 若停在微软拼音上，即使你敲的是 ASCII，也会进候选框变成中文。开工前手动 `Shift` 或 `Win+Space` 切到英文键盘。

### 7.5 打字节奏调参

| 参数 | 含义 | 建议值 |
|---|---|---|
| `CHAR_DELAY` | 相邻两个字符之间的间隔（ms） | 本地直连 60~80；远程桌面 / VPC 120~200 |
| `HOLD_DELAY` | 单键从按下到释放的保持时间（ms） | 20~30；低于 15 部分系统会漏键 |

「远程桌面漏字符」优先调大 `CHAR_DELAY`，代价是总耗时线性增长：5000 字符 × 150ms ≈ 12.5 分钟。页面顶部实时显示 `Typing / Paused / Idle`，反复丢字时先降速再怀疑目标端。

---

## 8. 环境排查（实测）

### 8.1 macOS 首次插入弹“键盘设置助手”

HID 设备没有厂商字符串，macOS 首次接入会弹键盘类型识别向导，**抢走焦点**导致前几个字符丢失。

处理：插入后按 `Esc` 或点关闭，系统会记住该设备，之后不再弹。别在这个弹窗出现时按 BOOT。

### 8.2 网页打不开

| 现象 | 原因 |
|---|---|
| `esp32-hid.local` 无响应 | mDNS 在部分安卓/Windows 环境不可靠 → 直接用 IP |
| 两个都打不开 | 板子和手机不在同一网段；或连的是公司 WiFi（客户端隔离）→ 换手机热点 |
| 页面能开但 Save 失败 | POST body 超 16KB → 确认编译时带了 `HTTP_MAX_POST_SIZE=65536` |
| 只能进 192.168.4.1 | 板子没连上热点，退到 AP 模式了 → 检查 SSID/密码，注意 5G 频段 2.4G-only 兼容性（ESP32 只支持 2.4GHz） |
| 热点列表里有两个一模一样的 `ESP32_HID_KEYBOARD` | 同房间多台板子没改 `AP_SSID` → 见 4.3，各自改成 `ESP32_HID_<标识>` |
| 明明改了文本，自己这块板子没反应 | 大概率连到了别人的板子（AP 同名 + 都是 192.168.4.1）→ 改名后重试 |

**ESP32 只支持 2.4GHz**，这点务必确认——手机热点若开了"5GHz 优先"可能连不上。

### 8.3 `board list` 看不到端口

按概率排：① 线是充电线 → 换数据线；② 没进下载模式；③ 缺 CH343 驱动；④ 插的是左口（原生 USB）而 `usb_cdc_on_boot=0` 时该口不枚举串口。

---

## 9. 一页速查

```bash
# 一次性环境
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json
arduino-cli config save
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.10-cn
arduino-cli lib install "Adafruit TinyUSB Library"

# 每次改代码后
cd ~/esp32_hid_key
arduino-cli compile --output-dir=build \
  --fqbn esp32:esp32:esp32s3 \
  --build-property build.partitions=default_16MB \
  --build-property build.flash_size=16MB \
  --build-property build.psram_type=opi \
  --build-property build.usb_cdc_on_boot=0 \
  --build-property build.flashmode=qio \
  --build-property build.flashfreq=80m \
  --build-property "build.extra_flags=-DHTTP_MAX_POST_SIZE=65536" .

# 烧录（先按住 BOOT + 点 EN）
arduino-cli board list
arduino-cli upload -p /dev/cu.wchusbserial1420 --fqbn esp32:esp32:esp32s3 --input-dir=build .

# 看日志
arduino-cli monitor -p /dev/cu.wchusbserial1420 -c baudrate=115200
```

**物理口诀**：右口下载调试，左口纯 HID；BOOT+EN 进下载；烧完拔右口，留左口插目标机。
