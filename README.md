# ESP32-S3 HID Keyboard -wifi版（蓝牙版见https://github.com/neoguo85/esp32-s3-ble-hid）

把一块 ESP32-S3 做成标准 USB 键盘，将预置文本逐字符「敲」进目标机器 —— 用于**禁止剪贴板 / 禁止粘贴 / 禁止文件拖拽**的受控环境（客户 VPC、跳板机、KVM、远程桌面）灌入数，代码据或长文本。

不碰系统、不注入进程、不读剪贴板，对目标机而言就是一个普通外接键盘。

## 硬件

| 项 | 值 |
|---|---|
| 模组 | 乐鑫 ESP32-S3-WROOM-1-**N16R8**（16MB Flash + 8MB **OPI** PSRAM） |
| 开发板 | 兼容官方 ESP32-S3-DevKitC-1，建议选已焊接排针版 |
| 工具链 | arduino-cli 1.5.1 + 板包 `esp32:esp32@3.3.10-cn` |

任意 ESP32-S3 N16R8 开发板均可。别用 N8R2 / N4R2：Flash 只有 8MB/4MB，分区表和 PSRAM 参数都得跟着改。

## 目录结构

```
.
├── esp32-hid-guide.html              # 精简版手册（单文件，代码块可一键复制）
├── ESP32-S3-HID-Keyboard-Guide.md    # 完整版手册（含参数说明与排错）
└── esp32_hid_key/
    └── esp32_hid_key.ino             # 固件源码，398 行，实测编译通过
```

## 快速开始

**1. 装环境**（国内镜像）

```bash
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json
arduino-cli config save
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.10-cn
arduino-cli lib install "Adafruit TinyUSB Library"
```

**2. 改配置**：编辑 `esp32_hid_key.ino` 顶部，把 `YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD` 换成真实值。同房间有多台设备时，`AP_SSID`、`AP_PASS`、`MDNS_NAME` 也必须各自改掉，否则连错板子毫无感知。

**3. 编译烧录**

```bash
cd esp32_hid_key
arduino-cli compile --output-dir=build \
  --fqbn esp32:esp32:esp32s3 \
  --build-property build.partitions=default_16MB \
  --build-property build.flash_size=16MB \
  --build-property build.psram_type=opi \
  --build-property build.usb_cdc_on_boot=0 \
  --build-property build.flashmode=qio \
  --build-property build.flashfreq=80m \
  --build-property "build.extra_flags=-DHTTP_MAX_POST_SIZE=65536" .

arduino-cli board list
arduino-cli upload -p /dev/cu.wchusbserial1420 --fqbn esp32:esp32:esp32s3 --input-dir=build .
```

进下载模式：数据线插**右侧 UART 口** → 按住 **BOOT** → 点一下 **EN** → 松开 EN → 保持 BOOT 约 1 秒再松开。

参数一项都不能省：`psram_type=opi` 写错会 boot loop；`usb_cdc_on_boot=0` 不关，原生 USB 会枚举成串口，目标机认不到键盘。

**4. 用**

1. 右口插电，手机/Mac 连同一个热点，浏览器打开 `esp32-hid.local`
2. 粘贴文本 → **Save Text**（写进 Flash，断电不丢）
3. 拔右口 → **左口**插目标机 → 点目标输入框 → 按一下 **BOOT** 开始敲

BOOT 单击 = 开始 / 暂停；网页上 **Stop Typing** 随时中止。

## 接口说明

板子有两个 Type-C：

| 口 | 角色 |
|---|---|
| 右侧 **UART** | 下载固件 + 串口调试（115200） |
| 左侧 **USB** | 原生 USB，纯 HID 键盘输出 |

## 已知限制

- **只支持 ASCII**：中文、日文、emoji、Tab、方向键、Ctrl 组合键都不支持，固件是逐字符映射 US 键盘 scancode。
- **目标机必须处于英文输入法**：停在拼音输入法上，敲 ASCII 也会进候选框。
- **ESP32 只支持 2.4GHz WiFi**：手机热点开了「5GHz 优先」可能连不上。
- **漏字符**：远程桌面 / VPC 里把 `CHAR_DELAY` 从 80 调到 120~200。
- **文本明文存在板子 Flash**，用完记得清空。
- macOS 首次插入会弹键盘类型识别向导并抢焦点，按 `Esc` 关掉即可。

## 编译占用

```
Sketch uses 1087131 bytes (82%) of program storage space. Maximum is 1310720 bytes.
Global variables use 81376 bytes (24%) of dynamic memory.
```

`default_16MB` 给 app 只有 1.25MB，已占 82%，别再往这个 sketch 里堆功能。要扩就换 `huge_app` 分区表。

## 合规

仅在你被授权的设备与账号上使用。客户环境使用前，确认 HID 外设接入符合该环境的安全策略。

## License

MIT
