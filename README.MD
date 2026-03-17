

# 🌡️ ESP32 Cloud-Weather Dashboard

基于 **ESP32-2432S028 (CYD)** 的桌面智能终端。实现 LVGL 交互、室内温湿度监控以及数据云端上报。

## 🚀 核心功能
* **GUI 交互**：基于 **LVGL v8.3** 与 SquareLine Studio 设计的现代 UI。
* **环境感知**：通过 GPIO 26 采集 DHT11/AHT20 温湿度数据。
* **云端对时**：支持多组 NTP 服务器（阿里云/腾讯/网易）自动校准时间。
* **物联网上报**：通过 HTTP POST 定时将传感器数据上传至远程 **LNMP** 服务器。
* **可视化后台**：后端基于 **宝塔面板**，使用 PHP + MySQL 存储历史记录。

## 🛠️ 硬件引脚 (Pinout)
| 功能 | 引脚 (GPIO) |
| :--- | :--- |
| **TFT Display** | SCK:14, MOSI:13, CS:15, DC:2, RST:4 |
| **Touch (SPI)** | CLK:25, MISO:39, MOSI:32, CS:33 |
| **Sensor** | Data: 26 |
| **Backlight** | BL: 21 |

## 📂 项目结构
* `/src` : ESP32 核心逻辑 (`main.cpp`) 及 UI 代码。
* `/server` : `upload.php` 及 SQL 数据库建表脚本。
* `platformio.ini` : 环境配置文件（含 `huge_app` 分区表）。

## 🔧 快速开始
1. 在 **PlatformIO** 中导入本项目。
2. 修改 `main.cpp` 中的 WiFi SSID 及服务器 IP。
3. 将 `server/upload.php` 部署至宝塔面板站点根目录。
4. 编译并上传，开启你的桌面气象站！

---
