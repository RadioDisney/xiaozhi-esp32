# M5Stack StopWatch

本仓库已包含 StopWatch 板卡支持：ESP32-S3R8、16 MB Flash、8 MB Octal PSRAM、
466x466 CO5300 圆形 AMOLED 和 ES8311 音频编解码器。

现有功能包括 Wi-Fi 语音对话、麦克风录音、扬声器播放、圆屏表情与字幕、亮度控制、
按键控制和基于电压估算的电量显示。黄色 KEYA（G2）切换对话，蓝色 KEYB（G1）
将音量每次增加 10%，超过 100% 后回到 0%。配网模式下黄色键切换音频测试。

新增 `m5stack-stopwatch-rear-buttons` 扩展版支持背部三个按键，顶部按键仍保留原功能：

| 背部信号 | 接口针脚 | 单击功能 |
| --- | --- | --- |
| GPIO3 | 2 | 切换语音对话（与黄色 KEYA 相同） |
| GPIO19 / MUX_IO_2 | 5 | 音量加 10%，最高 100% |
| GPIO20 / MUX_IO_1 | 3 | 音量减 10%，最低 0% |

按键接在 GPIO 与 GND 之间，软件启用内部上拉，松开为高电平、按下为低电平，
复用现有按键驱动的消抖。GPIO19/20 不再接外部上拉，以免给 USB 数据线增加负载；
GPIO3 原有的 10 kΩ 上拉可以保留。音量变化时显示百分比。
GPIO3 的语音行为包含原黄色键的配网/音频测试逻辑。按键处理调度到应用主任务。

背部 MUX_IO_1/2 默认连接 UART0（GPIO43/44），扩展版将 M5IOE1 G1 置高，
切换到 GPIO20/19，并关闭 USB Serial/JTAG 控制台及驱动；此时背部 MUX 针脚
不能用于 UART，GPIO19/20 也不能同时用于 USB 数据通信。USB 电源仍可使用。
移除 GPIO19/20 的外部上拉后，可保持松开的按键连接并进入 ROM 下载模式烧录。
USB 模式下两颗背部音量键不使用。GPIO3 是 ESP32-S3 的启动配置
引脚，建议上电和复位时保持语音按键松开。

构建扩展版（独立固件名称，使用相同的 S3 / 16 MiB Flash / 8 MiB PSRAM 配置）：

```bash
python3 scripts/build.py m5stack/stopwatch --name m5stack-stopwatch-rear-buttons --language zh-CN --zip
```

输出 `releases/v<project-version>_m5stack-stopwatch-rear-buttons.zip`。
原 `m5stack-stopwatch` 版本不会接管背部 GPIO。手动 menuconfig 构建三按键模式时，需关闭
`StopWatch: reserve GPIO19/20 for USB logging`，关闭
USB 主/副控制台和 USB Serial/JTAG 驱动，再启用 `StopWatch: three rear expansion buttons` 选项；发布
构建应使用上述命令，以保留正确的扩展版身份。

触摸控制器的复位线在初始化时释放，但尚未接入触摸事件；IMU、RTC、振动反馈和
专门的低功耗策略也尚未接入本板卡的应用流程。

硬件资料：[StopWatch 官方文档](https://docs.m5stack.com/en/core/StopWatch)。
无需编译即可体验的官方固件见
[M5Burner 小智教程](https://docs.m5stack.com/en/guide/stopwatch/xiaozhi_voice_assistant)。

-----------
## hardware

* MCU:ESP32-S3
    * Flash: 16MiB
    * PSRAM: 8MiB
    * SYS_I2C（本实现使用 I2C_NUM_0）
        * SCL -- G48
        * SDA -- G47
* PMIC: M5PM1@0x6E
    * Interface: SYS_I2C
* IO Expander: M5IOE1@0x4F
    * Interface: SYS_I2C
* Power
    * Charge Enable -- M5PM1_CHG_EN_PP
    * Charge State -- M5PM1_G2
    * Charge Current Prog -- M5PM1_G3
    * Motor -- M5IOE1_G9
    * Ext output -- M5PM1_BOOST5V_EN_PP
    * Ext output voltage -- M5PM1_5VOUT_ADC_IN
* Display
    * Power -- M5IOE1_G8
    * Interface: QSPI
    * Driver: CO5300
    * Resolution: 466x466
    * 圆形显示屏 1.75 Inch Amoled
    * Pin Map
        * RST      -- M5IOE1_G5
        * TE       -- G38
        * QSPI_CS  -- G39
        * QSPI_SCK -- G40
        * QSPI_D0  -- G41
        * QSPI_D1  -- G42
        * QSPI_D2  -- G46
        * QSPI_D3  -- G45
* CTP: CST820B@0x15
    * RST -- M5IOE1_G4
    * INT -- G13
    * Interface: SYS_I2C
* Audio
    * Power -- M5IOE1_G3
    * CODEC: ES8311@0x18
        * Control Interface: SYS_I2C
        * Data Interface (codec perspective):
            * I2S_MCLK -- G18
            * I2S_BCLK -- G17
            * I2S_LRCK -- G15
            * I2S_DSDIN  -- G21 (ESP32 output)
            * I2S_ASDOUT -- G16 (ESP32 input)
    * PA -- M5IOE1_G10
* Buttons
    * KEYA (yellow) - G2
    * KEYB (blue) - G1
* RTC: RX8130CE@0x32
    * Interface: SYS_I2C
* Grove
    * GND
    * 5V
    * G10
    * G11

-----------
## 快速构建

从仓库根目录执行。先加载完整的 ESP-IDF 环境，优先使用 `release/v6.1`，
最低支持 v6.0.1，不支持 IDF 5.x。下面的 SDK 路径需替换为实际安装位置：

```bash
source /path/to/esp-idf/export.sh
idf.py --version
python3 scripts/build.py m5stack/stopwatch --name m5stack-stopwatch --language zh-CN --zip
```

这里第一个参数是板卡目录 `m5stack/stopwatch`，`--name` 是固件身份
`m5stack-stopwatch`，二者不同。`--language zh-CN` 同时选择中文提示音和默认
“你好小智”唤醒词；省略语言参数时默认英语。

脚本会选择 ESP32-S3、16 MB Flash、`partitions/v2/16m.csv`、Octal PSRAM 80 MHz，
并生成合并固件及压缩包（版本号来自根目录 `CMakeLists.txt`）：

```text
build/merged-binary.bin
releases/v<project-version>_m5stack-stopwatch.zip
```

构建脚本会更新本地 sdkconfig 和 build 状态，切换其他板卡后需重新运行上述命令。

## 外部 NAND 扩展

GD5F1GM7UEYIGR（128 MiB）扩展支持独立 SPI3、Dhara FTL 和 LittleFS，
提供默认输出 USB 日志的 SIO 5 MHz 和四线 QOUT 40 MHz 工作版；更换 NAND 后已逐级验证
1、5、10、20、40 MHz，新接线仍应先降到 1–5 MHz 验证。GPIO3 语音键和顶部按键可用，
这些硬件诊断变体停用 GPIO19/20 两颗音量键，并将引脚保留给 USB。
80 MHz 尝试未通过文件自检，已恢复 40 MHz 并重新验证原文件及读写。
接线、首次初始化、构建、
后台测试和实机验收见 [NAND 扩展说明](NAND.md)。原版与仅三按键版不接管 NAND 引脚。

## NAND MJPEG 表情与自动按键模式

当前设备资源为 **12 fps 高清版**，26.34 MB、21 个表情。
资源包为 `releases/firecat-nand-466-12fps-hq.pack`，验证结果见
[12 fps 资源验证记录](NAND_EMOJI_12FPS_TEST_RESULTS.md)。以下 24 fps 数据为首次迁移说明。

`m5stack-stopwatch-nand-mjpeg-auto` 使用外部 NAND 中的 21 个全屏 MJPEG 表情，
不再把旧表情包放入 NOR 资源。新资源共 47,406,634 字节、2085 帧，保留原始 466×466
画面和 24 fps 时间线，后台按帧读取并使用 ESP32-S3 软件 JPEG 解码。

开机连接电脑时保留 USB 调试；电池或纯供电充电器开机时，GPIO19/20 使用内部上拉
作为音量加/减。模式固定到下一次重启。GPIO3 和顶部按键继续可用。

```bash
python scripts/build.py m5stack/stopwatch \
  --name m5stack-stopwatch-nand-mjpeg-auto --language zh-CN --zip
```

安装方法和维护命令见 [NAND 表情说明](NAND_EMOJIS.md)，
实机结果见 [NAND 表情验收记录](NAND_EMOJI_TEST_RESULTS.md)。

## 旧版内置 Firecat 表情

以下资源用于原版及硬件诊断变体；NAND MJPEG 自动模式使用上一节的外部资源。

`emoji/` 保存 21 个原始 466×466 透明 GIF，`mjpeg/` 是固件打包输入，包含
6 个 MJPEG 动画、15 个 JPEG 静态图及各自的 `.idx` 帧索引。动画包括 `neutral`、
`happy`、`sad`、`loving`、`surprised`、`thinking`，保留原有 10 FPS 时间信息和
无限循环设置；静态图解码一次后保持显示。

ESP32-S3 使用 `esp_new_jpeg` 软件解码，没有硬件 JPEG 解码外设。低优先级后台
任务复用 JPEG 解码器、压缩数据缓存和两块 RGB565 缓冲，解码和帧等待在 GUI 锁外
完成；界面仅交换已完成的帧。切换表情通过代次丢弃旧帧，过期帧会跳过，不累积
播放队列。摄像头预览期间暂停播放，结束后恢复。

背景预先合成到每帧。角色保持原尺寸，仅截取 `(16,80)` 开始的 432×304 区域，
减少约 40% 的像素解码和刷新量。状态、图标、字幕仍独立绘制在原有位置。
JPEG 默认质量 82、4:4:4、baseline；资源共 5,171,675 字节（另有小型帧索引），
构建时会检查包含字体和语音模型的完整资源包是否超过 8 MiB。

本板卡使用 LVGL 系统 malloc，较大的播放器缓冲明确分配在 PSRAM，避免额外保留
GIF 所需的大型 LVGL 专用池。播放器上限为每片段 64 帧、1 MiB 压缩数据、每帧
128 KiB；索引检查尺寸、偏移、长度、时长和 JPEG 边界。PNG 解码器保持关闭。

修改 GIF 或背景后，用安装了 Pillow 的 Python 重新转换，再运行上面的构建命令：

```bash
python3 scripts/convert_stopwatch_mjpeg.py \
    --source main/boards/m5stack/stopwatch/emoji \
    --background main/boards/m5stack/stopwatch/assets/stopwatch_background.jpg \
    --output main/boards/m5stack/stopwatch/mjpeg \
    --quality 82
```

转换保留原始 GIF 文件，`conversion.json` 记录裁切、时间、质量及源文件校验值。
`.mjpeg` 文件是连续 JPEG 帧，`.idx` 使用小端 `MJI1` 头（宽、高、坐标、帧数、
流长度、循环次数）及每帧偏移/长度/毫秒时长。`.jpg` 也有单帧索引。
固件更新需包含资源分区，使用合并固件即可；只烧录 `xiaozhi.bin` 不会更新表情。

## 背景图片

背景使用 `assets/stopwatch_background.jpg`（466×466），保留提供的 JPEG 原文件。
它仅嵌入 StopWatch 的应用程序分区，不占用接近上限的表情资源分区。
开机时使用现有 JPEG 解码功能转换为 RGB565 并保留，约占 424 KiB 内存；
切换主题时不会重复解码，也不需要启用 PNG 解码器。

表情帧预合成相同背景，状态文字、图标及字幕使用深蓝色，上下栏透明。
浅色和深色主题都保留这张背景，字体资源加载或主题刷新后也会重新应用。
替换背景时保持文件名和 466×466 尺寸，并使用 baseline JPEG（非渐进式），
重新转换表情并构建后烧录合并固件。

-----------
## 手动配置

配置编译目标：

```bash
idf.py set-target esp32s3
```

打开配置菜单：

```bash
idf.py menuconfig
```

选择板卡：

```text
Xiaozhi Assistant -> Board Type -> M5Stack Stopwatch
```

配置 Flash 大小：

```text
Serial flasher config -> Flash size -> 16 MB
```

配置分区表：

```text
Partition Table -> Custom partition CSV file -> partitions/v2/16m.csv
```

配置 PSRAM：

```text
Component config -> ESP PSRAM -> Support for external, SPI-connected RAM -> Select
Component config -> ESP PSRAM -> SPI RAM config -> Mode (QUAD/OCT) -> Octal Mode PSRAM
Component config -> ESP PSRAM -> SPI RAM config -> Set RAM clock speed -> 80MHz clock speed
```

配置唤醒词实现方式：

```text
Xiaozhi Assistant -> Wake Word Implementation Type -> Wakenet model with AFE
```

编译：

```bash
idf.py build
```

## 烧录与首次使用

使用 USB Type-C 数据线连接电脑。按住电源键约 2 秒，绿灯亮后松开，进入下载模式。
将下面的 `PORT` 替换为实际串口（macOS 常见 `/dev/cu.usbmodem...`）：

```bash
idf.py -p PORT flash monitor
```

手动构建后若需要单个合并固件，使用 `idf.py merge-bin`，由工具读取实际分区和
烧录参数，输出 `build/merged-binary.bin`。使用合并固件的烧录工具时，起始地址为 0x0。

首次启动时，按屏幕提示连接 `Xiaozhi-` 开头的热点，访问 `http://192.168.4.1`，
配置 2.4 GHz Wi-Fi。联网后按屏幕或语音播报的验证码，在
[小智控制台](https://xiaozhi.me) 添加设备。随后可使用黄色键或“你好小智”开始对话。

## 实机验收

编译通过不能替代硬件验证。烧录后需检查：圆屏显示、字幕边缘、亮度调节、顶部两颗按键、扩展版背部三颗按键（含音量上下限和快速交替点击）、
录音与播放、唤醒与打断、Wi-Fi 断线重连，以及 USB/电池供电和充电状态。
当前电量百分比为电压线性估算，不能视为精确的电池容量测量。
