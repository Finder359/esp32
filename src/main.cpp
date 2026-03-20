#include <Arduino.h>
#include <TFT_eSPI.h> // 引入屏幕库
#include "ui/ui.h" // 引入UI库（如果需要）
#include <lvgl.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <DHT.h>

TFT_eSPI tft = TFT_eSPI(); // 初始化屏幕对象

// 独立触摸SPI引脚（按你提供的XPT2046定义）
static const uint8_t TOUCH_MOSI = 32;
static const uint8_t TOUCH_CLK = 25;
static const uint8_t TOUCH_MISO = 39;
static const uint8_t TOUCH_CS = 33;
static const uint8_t TOUCH_IRQ = 36;

static const char* WIFI_SSID = "K40";
static const char* WIFI_PASS = "12345678";
static const char* SERVER_URL = "http://159.75.181.29/api.php?action=upload";
static const char* API_KEY = "5f8d2b7a9c1e4f6b8d0a3c5e7b9a1d2f";
static const char* DEVICE_ID = "ESP32_Room";
static const char* WEATHER_URL = "https://api.seniverse.com/v3/weather/now.json?key=ScW402483CrcaSFye&location=weihai&language=zh-Hans&unit=c";
#ifndef DHT_PIN
#define DHT_PIN 27
#endif
static const uint8_t DHT_PIN_DEFAULT = DHT_PIN;
static const uint8_t DHT_TYPE = DHT22;

/* 1. 定义显示缓冲区（LVGL 刷屏用） */
static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10]; // 缓冲区大小，通常设为屏幕的 1/10

static uint32_t lastTick = 0;
static uint32_t lastTouchLogMs = 0;
static uint32_t lastClockUiMs = 0;
static uint32_t lastNtpRetryMs = 0;
static uint32_t lastWeatherFetchMs = 0;
static uint32_t lastDhtReadMs = 0;
static uint32_t lastUploadMs = 0;
static bool timeSynced = false;
static bool weatherSynced = false;
static float lastTempC = NAN;
static float lastHumidity = NAN;
static bool dhtBootLogDone = false; // 只在第一次成功读取 DHT22 时打印一次日志，避免后续频繁打印
static const uint32_t SENSOR_UPLOAD_INTERVAL_MS = 60 * 1000;

DHT dht(DHT_PIN_DEFAULT, DHT_TYPE);

static void updateDhtUi(float tempC, float humidity, bool valid) {
    if (ui_Label8 == nullptr) {
        return;
    }

    if (!valid) {
        lv_label_set_text(ui_Label8, "Temp: --.- C\nHumi: --.- %");
        return;
    }

    String uiText = "Temp: " + String(tempC, 1) + " C\nHumi: " + String(humidity, 1) + " %";
    lv_label_set_text(ui_Label8, uiText.c_str());
}

static void updateLastUpdatedLog() {
    if (ui_Label3 == nullptr) {
        return;
    }

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
        lv_label_set_text_fmt(ui_Label3,
                              "Last updated %02d:%02d:%02d",
                              timeinfo.tm_hour,
                              timeinfo.tm_min,
                              timeinfo.tm_sec);
        return;
    }

    uint32_t sec = millis() / 1000;
    lv_label_set_text_fmt(ui_Label3, "Last updated %lus", sec);
}

// 触摸校准参数（原始值），如有偏差可再微调
static const int TOUCH_RAW_X_MIN = 200;
static const int TOUCH_RAW_X_MAX = 3900;
static const int TOUCH_RAW_Y_MIN = 200;
static const int TOUCH_RAW_Y_MAX = 3900;
static const uint16_t TOUCH_PRESSURE_MIN = 250;

static const char* NTP_SERVER_1 = "ntp.aliyun.com";
static const char* NTP_SERVER_2 = "pool.ntp.org";
static const char* NTP_SERVER_3 = "cn.ntp.org.cn";
static const char* NTP_SERVER_4 = "time.windows.com";
static const char* NTP_SERVER_5 = "ntp.tencent.com";
static const char* NTP_SERVER_6 = "time.cloudflare.com";
static const long GMT_OFFSET_SEC = 8 * 3600;
static const int DAYLIGHT_OFFSET_SEC = 0;
static const uint32_t NTP_RETRY_INTERVAL_MS = 60000;
static const uint32_t WEATHER_UPDATE_INTERVAL_MS = 10 * 60 * 1000;
static const uint32_t WEATHER_RETRY_INTERVAL_MS = 60 * 1000;

static uint16_t xpt2046Read12(uint8_t command) {
    uint16_t data = 0;

    digitalWrite(TOUCH_CS, LOW);

    for (int i = 7; i >= 0; --i) {
        digitalWrite(TOUCH_MOSI, (command >> i) & 0x01);
        digitalWrite(TOUCH_CLK, HIGH);
        digitalWrite(TOUCH_CLK, LOW);
    }

    for (int i = 0; i < 16; ++i) {
        digitalWrite(TOUCH_CLK, HIGH);
        data = (data << 1) | (digitalRead(TOUCH_MISO) & 0x01);
        digitalWrite(TOUCH_CLK, LOW);
    }

    digitalWrite(TOUCH_CS, HIGH);
    return (data >> 3) & 0x0FFF;
}

static bool readTouchRaw(uint16_t &x, uint16_t &y, uint16_t &pressure) {
    if (digitalRead(TOUCH_IRQ) == HIGH) {
        return false;
    }

    uint16_t z1 = xpt2046Read12(0xB0);
    uint16_t z2 = xpt2046Read12(0xC0);
    pressure = z1 + (4095 - z2);
    if (pressure < TOUCH_PRESSURE_MIN) {
        return false;
    }

    uint32_t sx = 0;
    uint32_t sy = 0;
    for (int i = 0; i < 3; ++i) {
        sx += xpt2046Read12(0xD0);
        sy += xpt2046Read12(0x90);
    }
    x = sx / 3;
    y = sy / 3;
    return true;
}

/* 2. 冲刷函数：把 LVGL 算的像素画到 TFT 上 */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    LV_UNUSED(indev_driver);

    uint16_t rawX = 0;
    uint16_t rawY = 0;
    uint16_t pressure = 0;
    if (!readTouchRaw(rawX, rawY, pressure)) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // 旋转为 setRotation(1) 后，映射到 320x240
    int mappedX = map(rawY, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, screenWidth - 1);
    int mappedY = map(rawX, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, screenHeight - 1);
    mappedX = constrain(mappedX, 0, screenWidth - 1);
    mappedY = constrain(mappedY, 0, screenHeight - 1);

    data->state = LV_INDEV_STATE_PR;
    data->point.x = mappedX;
    data->point.y = mappedY;

    uint32_t now = millis();
    if (now - lastTouchLogMs > 120) {
        Serial.printf("RAW x=%u y=%u p=%u -> LVGL x=%d y=%d\n", rawX, rawY, pressure, mappedX, mappedY);
        lastTouchLogMs = now;
    }
}

void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting WiFi: %s\n", WIFI_SSID);

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected, IP: ");
        Serial.println(WiFi.localIP());
        if (ui_Label1 != nullptr) {
            lv_label_set_text(ui_Label1, "WiFi OK");
        }
    } else {
        Serial.println("WiFi connect failed");
        if (ui_Label1 != nullptr) {
            lv_label_set_text(ui_Label1, "WiFi FAIL");
        }
    }
}

static bool waitForTimeSync(uint32_t timeoutMs) {
    struct tm timeinfo;
    uint32_t startMs = millis();
    while (millis() - startMs < timeoutMs) {
        if (getLocalTime(&timeinfo, 500)) {
            Serial.printf("Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                          timeinfo.tm_year + 1900,
                          timeinfo.tm_mon + 1,
                          timeinfo.tm_mday,
                          timeinfo.tm_hour,
                          timeinfo.tm_min,
                          timeinfo.tm_sec);
            return true;
        }
        delay(200);
        Serial.print("#");
    }
    Serial.println();
    return false;
}

bool syncNetworkTime() {
    Serial.println("Syncing time via NTP (group A)...");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    if (waitForTimeSync(8000)) {
        return true;
    }

    Serial.println("Syncing time via NTP (group B)...");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_4, NTP_SERVER_5, NTP_SERVER_6);
    if (waitForTimeSync(8000)) {
        return true;
    }

    Serial.println("NTP sync timeout");
    return false;
}

static bool extractJsonStringValue(const String &json, const char *key, String &out) {
    String token = String("\"") + key + "\":\"";
    int start = json.indexOf(token);
    if (start < 0) {
        return false;
    }
    start += token.length();
    int end = json.indexOf('"', start);
    if (end < 0) {
        return false;
    }
    out = json.substring(start, end);
    return true;
}

static const char* mapWeatherTextToAscii(const String &weatherText) {
    if (weatherText == "晴") return "Sunny";
    if (weatherText == "多云") return "Cloudy";
    if (weatherText == "阴") return "Overcast";
    if (weatherText == "小雨") return "LightRain";
    if (weatherText == "中雨") return "Rain";
    if (weatherText == "大雨") return "HeavyRain";
    if (weatherText == "雷阵雨") return "Thunder";
    if (weatherText == "雾") return "Fog";
    if (weatherText == "霾") return "Haze";
    if (weatherText == "小雪") return "LightSnow";
    if (weatherText == "中雪") return "Snow";
    if (weatherText == "大雪") return "HeavySnow";
    return "Unknown";
}

bool fetchWeatherAndUpdateUi() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Weather fetch skipped: WiFi disconnected");
        if (ui_Label7 != nullptr) {
            lv_label_set_text(ui_Label7, "Weather: offline");
        }
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, WEATHER_URL)) {
        Serial.println("Weather HTTP begin failed");
        if (ui_Label7 != nullptr) {
            lv_label_set_text(ui_Label7, "Weather begin failed");
        }
        return false;
    }

    int httpCode = http.GET();
    String response = http.getString();
    http.end();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("Weather HTTP code: %d\n", httpCode);
        if (ui_Label7 != nullptr) {
            lv_label_set_text_fmt(ui_Label7, "Weather HTTP:%d", httpCode);
        }
        return false;
    }

    String temperature;
    String weatherText;
    bool okTemp = extractJsonStringValue(response, "temperature", temperature);
    bool okText = extractJsonStringValue(response, "text", weatherText);

    if (!okTemp) {
        Serial.println("Weather JSON parse failed");
        if (ui_Label7 != nullptr) {
            lv_label_set_text(ui_Label7, "Weather parse failed");
        }
        return false;
    }

    const char *weatherAscii = okText ? mapWeatherTextToAscii(weatherText) : "Unknown";

    Serial.printf("Weather OK: city=%s temp=%s text=%s\n",
                  "Weihai",
                  temperature.c_str(),
                  weatherAscii);

    if (ui_Label7 != nullptr) {
        lv_label_set_text_fmt(ui_Label7, "Weihai %sC %s",
                              temperature.c_str(),
                              weatherAscii);
    }

    return true;
}

void updateDateTimeUi(bool force) {
    uint32_t nowMs = millis();
    if (!force && (nowMs - lastClockUiMs < 1000)) {
        return;
    }
    lastClockUiMs = nowMs;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10)) {
        if (ui_Label2 != nullptr) {
            lv_label_set_text(ui_Label2, "Date --");
        }
        if (ui_Label5 != nullptr) {
            lv_label_set_text(ui_Label5, "--:--:--");
        }
        return;
    }

    if (ui_Label2 != nullptr) {
        lv_label_set_text_fmt(ui_Label2, "%04d-%02d-%02d",
                              timeinfo.tm_year + 1900,
                              timeinfo.tm_mon + 1,
                              timeinfo.tm_mday);
    }
    if (ui_Label5 != nullptr) {
        lv_label_set_text_fmt(ui_Label5, "%02d:%02d:%02d",
                              timeinfo.tm_hour,
                              timeinfo.tm_min,
                              timeinfo.tm_sec);
    }
}

void uploadSensorData() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, skip POST");
        return;
    }

    if (isnan(lastTempC) || isnan(lastHumidity)) {
        Serial.println("No valid DHT22 data yet, skip POST");
        return;
    }

    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String postData;
    postData.reserve(192);
    postData = "action=upload"
               "&api_key=" + String(API_KEY) +
               "&temp=" + String(lastTempC, 1) +
               "&humi=" + String(lastHumidity, 1) +
               "&device_id=" + String(DEVICE_ID);
    int httpCode = http.POST(postData);
    String response = http.getString();

    Serial.printf("POST code: %d\n", httpCode);
    Serial.printf("POST body: %s\n", postData.c_str());
    Serial.printf("Server response: %s\n", response.c_str());

    if (httpCode == 200 && response.indexOf("\"ok\":true") >= 0) {
        updateLastUpdatedLog();
    } else if (httpCode == 401) {
        Serial.println("Upload failed: API_KEY invalid (401)");
    } else {
        Serial.println("Upload failed: backend did not return ok=true");
    }

    http.end();
}

void onSendButtonClicked(lv_event_t *e) {
    LV_UNUSED(e);
    Serial.println("Button PRESSED event captured");
    uploadSensorData();
}

void setup() {
    Serial.begin(115200);
    dht.begin();
    Serial.printf("DHT22 init on GPIO%d\n", DHT_PIN_DEFAULT);

    // 初始化 TFT
    tft.begin();
    tft.setRotation(1); 

    // 初始化XPT2046软件SPI引脚
    pinMode(TOUCH_CS, OUTPUT);
    digitalWrite(TOUCH_CS, HIGH);
    pinMode(TOUCH_CLK, OUTPUT);
    digitalWrite(TOUCH_CLK, LOW);
    pinMode(TOUCH_MOSI, OUTPUT);
    digitalWrite(TOUCH_MOSI, LOW);
    pinMode(TOUCH_MISO, INPUT);
    pinMode(TOUCH_IRQ, INPUT);
    Serial.println("XPT2046 touch init done (software SPI)");
    
    // 初始化 LVGL
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);

    /* 初始化显示驱动 */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* 初始化触摸输入驱动 */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // 3. 调用 SquareLine 生成的 UI 初始化
    ui_init(); 
    updateDhtUi(0.0f, 0.0f, false);
    if (ui_Label3 != nullptr) {
        lv_label_set_text(ui_Label3, "Last updated --:--:--");
    }

    if (ui_Button1 != nullptr) {
        lv_obj_add_event_cb(ui_Button1, onSendButtonClicked, LV_EVENT_PRESSED, NULL);
        Serial.println("Button event callback bound (LV_EVENT_PRESSED)");
    }

    connectWifi();
    lastNtpRetryMs = millis();
    lastWeatherFetchMs = millis();
    lastUploadMs = millis();
    if (WiFi.status() == WL_CONNECTED) {
        timeSynced = syncNetworkTime();
        weatherSynced = fetchWeatherAndUpdateUi();
    }
    if (!timeSynced) {
        Serial.println("Time not synced, UI will show fallback text until sync succeeds");
    }

    updateDateTimeUi(true);
    lastTick = millis();
    
    Serial.println("LVGL UI Started!");
}

void loop() {
    uint32_t now = millis();
    lv_tick_inc(now - lastTick);
    lastTick = now;

    // 4. 给 LVGL 提供时钟心跳（极其重要！）
    lv_timer_handler(); 

    if (!timeSynced && WiFi.status() == WL_CONNECTED) {
        uint32_t nowMs = millis();
        if (nowMs - lastNtpRetryMs >= NTP_RETRY_INTERVAL_MS) {
            Serial.println("NTP retry triggered (1 minute interval)");
            timeSynced = syncNetworkTime();
            lastNtpRetryMs = nowMs;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        uint32_t nowMs = millis();
        uint32_t weatherInterval = weatherSynced ? WEATHER_UPDATE_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS;
        if (nowMs - lastWeatherFetchMs >= weatherInterval) {
            Serial.println("Weather update triggered");
            weatherSynced = fetchWeatherAndUpdateUi();
            lastWeatherFetchMs = nowMs;
        }

        if (nowMs - lastUploadMs >= SENSOR_UPLOAD_INTERVAL_MS) {
            uploadSensorData();
            lastUploadMs = nowMs;
        }
    }

    if (millis() - lastDhtReadMs >= 2000) {
        lastDhtReadMs = millis();
        float humidity = dht.readHumidity();
        float tempC = dht.readTemperature();

        if (isnan(humidity) || isnan(tempC)) {
            lastTempC = NAN;
            lastHumidity = NAN;
            updateDhtUi(0.0f, 0.0f, false);
        } else {
            if (!dhtBootLogDone) {
                Serial.printf("DHT22(GPIO%d) -> Temp: %.1f C, Humi: %.1f %%\n", DHT_PIN_DEFAULT, tempC, humidity);
                dhtBootLogDone = true;
            }
            lastTempC = tempC;
            lastHumidity = humidity;
            updateDhtUi(tempC, humidity, true);
        }
    }

    updateDateTimeUi(false);

    delay(5);
}