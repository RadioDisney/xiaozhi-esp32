# M5Stack StopWatch

本仓库已包含 StopWatch 板卡支持：ESP32-S3R8、16 MB Flash、8 MB Octal PSRAM、
466x466 CO5300 圆形 AMOLED 和 ES8311 音频编解码器。

现有功能包括 Wi-Fi 语音对话、麦克风录音、扬声器播放、圆屏表情与字幕、亮度控制、
按键控制和基于电压估算的电量显示。黄色 KEYA（G2）切换对话，蓝色 KEYB（G1）
将音量每次增加 10%，超过 100% 后回到 0%。配网模式下黄色键切换音频测试。

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

## Firecat 动态表情

本板卡默认使用 `emoji/` 中的 21 个 Firecat 透明 GIF，替代原来的 Noto 静态表情。
每个动画为 224×224 像素、24 帧、每帧 100 ms（10 FPS），2.4 秒无限循环。
GIF 总大小为 4,992,756 字节；资源分区还包含字体和唤醒词模型，构建时会检查
总大小是否超过 8 MiB。

文件名需保持小智情绪标识，例如 `neutral.gif`、`happy.gif`、`thinking.gif`。
原始美术文件名中的 `01_` 等序号已在导入时去掉，动画内容保持原样。
以后可直接替换本板卡 `emoji/` 中的文件，再运行上述构建命令；修改 GIF 会自动
重新生成资源包，无需编辑 `managed_components/`。烧录时需包含资源分区，
使用合并固件即可；只烧录 `xiaozhi.bin` 不会更新表情。

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

编译通过不能替代硬件验证。烧录后需检查：圆屏显示、字幕边缘、亮度调节、两颗按键、
录音与播放、唤醒与打断、Wi-Fi 断线重连，以及 USB/电池供电和充电状态。
当前电量百分比为电压线性估算，不能视为精确的电池容量测量。
