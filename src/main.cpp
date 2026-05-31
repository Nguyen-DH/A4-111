// Enable debug console
#define ERA_DEBUG

/* Define MQTT host */
#define DEFAULT_MQTT_HOST "mqtt1.eoh.io"

// You should get Auth Token in the ERa App or ERa Dashboard
#define ERA_AUTH_TOKEN "f771e9a9-b8cf-48dd-9630-5e34c3ac2c13"

#include <Arduino.h>
#include <WiFi.h>
#include <ERa.hpp>
#include <Wire.h>
#include <BH1750.h>

#include "Connect_Wed.h"

Connect_Wed myConfig("A4-111"); // Tên WiFi phát ra khi cấu hình
bool era_started = false;
unsigned long time_lost_wifi = 0;
#define WIFI_TIMEOUT 60000 // 60 giây

WiFiClient mbTcpClient;

// Khởi tạo đối tượng 2 cảm biến
BH1750 lightMeter1(0x23);
BH1750 lightMeter2(0x5C);

bool sensor1_ready = false;
bool sensor2_ready = false;

// ==========================================
// CẤU HÌNH RELAY VÀ BIẾN ÁNH SÁNG
// ==========================================

const int relayPins[8] = {12, 14, 27, 26, 25, 33, 32, 23}; 

#define RELAY_ON  HIGH
#define RELAY_OFF LOW

// Biến lưu giá trị đặt (Nhận từ ERa)
float setpointLux = 300.0; 

// Khoảng trễ (Hysteresis) để tránh nhiễu (Ví dụ: 50 lux)
const float HYSTERESIS = 50.0; 

// Thời gian chờ giữa mỗi lần bật/tắt (10 giây)
const unsigned long ACTION_DELAY = 10000;

// Trạng thái hệ thống
bool isAutoMode = true; // V2: 1 = Auto, 0 = Manual
bool relayState[4] = {false, false, false, false}; // Lưu trạng thái RL1, RL2, RL3, RL4

// Biến theo dõi thời gian ánh sáng thấp/cao cho CB1
unsigned long lowLightStartCB1 = 0;
unsigned long highLightStartCB1 = 0;
bool isLowLightCB1 = false;
bool isHighLightCB1 = false;

// Biến theo dõi thời gian ánh sáng thấp/cao cho CB2
unsigned long lowLightStartCB2 = 0;
unsigned long highLightStartCB2 = 0;
bool isLowLightCB2 = false;
bool isHighLightCB2 = false;

// ==========================================
// HÀM ĐIỀU KHIỂN RELAY CHUNG
// ==========================================

void setRelay(int idx, bool state) {
    digitalWrite(relayPins[idx], state ? RELAY_ON : RELAY_OFF);
    
    // Ghi đè trạng thái thực tế, KHÔNG dùng digitalRead trên chân OUTPUT của ESP32 
    // vì một số chân (như GPIO12) nếu bị nhiễu điện áp sẽ đọc ngược về LOW dù đã xuất HIGH
    relayState[idx] = state;

    // Đồng bộ lên App
    if (idx == 0) ERa.virtualWrite(V4, state);
    else if (idx == 1) ERa.virtualWrite(V5, state);
    else if (idx == 2) ERa.virtualWrite(V6, state);
    else if (idx == 3) ERa.virtualWrite(V7, state);
}

// ==========================================
// XỬ LÝ NHẬN LỆNH TỪ ERA
// ==========================================

// Đồng bộ trạng thái thực tế lên App khi thiết bị (ESP) kết nối thành công với máy chủ ERa
ERA_CONNECTED() {
    ERA_LOG("ERa", "Da ket noi! Dong bo trang thai relay...");
    
    // Đẩy giá trị mặc định của mạch lên ERa
    ERa.virtualWrite(V3, setpointLux);
    
    ERa.virtualWrite(V2, isAutoMode);
    ERa.virtualWrite(V4, relayState[0]);
    ERa.virtualWrite(V5, relayState[1]);
    ERa.virtualWrite(V6, relayState[2]);
    ERa.virtualWrite(V7, relayState[3]);
}

// Nhận giá trị cài đặt ánh sáng từ chân V3
ERA_WRITE(V3) {
    setpointLux = param.getFloat();
    ERA_LOG("Setting", "Da nhan gia tri dat moi: %.2f lx", setpointLux);
}

// Chuyển đổi Auto/Manual từ chân V2
ERA_WRITE(V2) {
    isAutoMode = param.getInt();
    ERa.virtualWrite(V2, isAutoMode); // Cập nhật xác nhận trạng thái nút Auto trên App
    ERA_LOG("Mode", "Chuyen sang che do: %s", isAutoMode ? "AUTO" : "MANUAL");
}

// Điều khiển Manual RL1 (V4)
ERA_WRITE(V4) {
    if (!isAutoMode) {
        setRelay(0, param.getInt());
        ERA_LOG("Manual", "RL1 -> %d", param.getInt());
    } else {
        ERa.virtualWrite(V4, relayState[0]); // Đồng bộ lại nếu đang ở Auto
    }
}

// Điều khiển Manual RL2 (V5)
ERA_WRITE(V5) {
    if (!isAutoMode) {
        setRelay(1, param.getInt());
        ERA_LOG("Manual", "RL2 -> %d", param.getInt());
    } else {
        ERa.virtualWrite(V5, relayState[1]);
    }
}

// Điều khiển Manual RL3 (V6)
ERA_WRITE(V6) {
    if (!isAutoMode) {
        setRelay(2, param.getInt());
        ERA_LOG("Manual", "RL3 -> %d", param.getInt());
    } else {
        ERa.virtualWrite(V6, relayState[2]);
    }
}

// Điều khiển Manual RL4 (V7)
ERA_WRITE(V7) {
    if (!isAutoMode) {
        setRelay(3, param.getInt());
        ERA_LOG("Manual", "RL4 -> %d", param.getInt());
    } else {
        ERa.virtualWrite(V7, relayState[3]);
    }
}


// ==========================================
// HÀM ĐIỀU KHIỂN LOGIC AUTO
// ==========================================

// CB1 điều khiển RL1 (index 0) và RL3 (index 2)
void controlCB1(float lux) {
    unsigned long currentMillis = millis();
    float thresholdON = setpointLux - HYSTERESIS;
    float thresholdOFF = setpointLux + HYSTERESIS;
    
    if (lux < thresholdON) {
        isHighLightCB1 = false; // Reset bộ đếm tắt
        if (!isLowLightCB1) {
            isLowLightCB1 = true;
            lowLightStartCB1 = currentMillis;
        } else if (currentMillis - lowLightStartCB1 >= ACTION_DELAY) {
            if (!relayState[0] || !relayState[2]) {
                setRelay(0, true);
                setRelay(2, true);
                ERA_LOG("Relay", "CB1: Sang YEU qua 10s -> Bat RL1, RL3 cung luc");
            }
        }
    } else if (lux > thresholdOFF) {
        isLowLightCB1 = false; // Reset bộ đếm bật
        if (!isHighLightCB1) {
            isHighLightCB1 = true;
            highLightStartCB1 = currentMillis;
        } else if (currentMillis - highLightStartCB1 >= ACTION_DELAY) {
            if (relayState[0] || relayState[2]) {
                setRelay(0, false);
                setRelay(2, false);
                ERA_LOG("Relay", "CB1: Sang MANH qua 10s -> Tat RL1, RL3 cung luc");
            }
        }
    } else {
        isLowLightCB1 = false;
        isHighLightCB1 = false;
    }
}

// CB2 điều khiển RL2 (index 1) và RL4 (index 3)
void controlCB2(float lux) {
    unsigned long currentMillis = millis();
    float thresholdON = setpointLux - HYSTERESIS;
    float thresholdOFF = setpointLux + HYSTERESIS;
    
    if (lux < thresholdON) {
        isHighLightCB2 = false; // Reset bộ đếm tắt
        if (!isLowLightCB2) {
            isLowLightCB2 = true;
            lowLightStartCB2 = currentMillis;
        } else if (currentMillis - lowLightStartCB2 >= ACTION_DELAY) {
            if (!relayState[1] || !relayState[3]) {
                setRelay(1, true);
                setRelay(3, true);
                ERA_LOG("Relay", "CB2: Sang YEU qua 10s -> Bat RL2, RL4 cung luc");
            }
        }
    } else if (lux > thresholdOFF) {
        isLowLightCB2 = false; // Reset bộ đếm bật
        if (!isHighLightCB2) {
            isHighLightCB2 = true;
            highLightStartCB2 = currentMillis;
        } else if (currentMillis - highLightStartCB2 >= ACTION_DELAY) {
            if (relayState[1] || relayState[3]) {
                setRelay(1, false);
                setRelay(3, false);
                ERA_LOG("Relay", "CB2: Sang MANH qua 10s -> Tat RL2, RL4 cung luc");
            }
        }
    } else {
        isLowLightCB2 = false;
        isHighLightCB2 = false;
    }
}


// ==========================================
// HÀM TIMER CỦA ERA ĐỂ ĐỌC CẢM BIẾN
// ==========================================

void timerEvent() {
    if (sensor1_ready) {
        float lux1 = lightMeter1.readLightLevel();
        // Bộ lọc: Giá trị hợp lệ trong khoảng 0 - 1000 lux
        if (lux1 >= 0.0 && lux1 <= 1000.0) {
            ERa.virtualWrite(V0, lux1); 
            // ERA_LOG("Sensor", "Cam bien 1: %.2f lx", lux1); 
            
            if (isAutoMode) {
                controlCB1(lux1);
            }
        } else {
            Serial.printf("-> CB1 loi hoac nhieu: %.2f lx\n", lux1);
        }
    } else {
        Serial.println("-> CB1 chua san sang!");
    }

    if (sensor2_ready) {
        float lux2 = lightMeter2.readLightLevel();
        // Bộ lọc: Giá trị hợp lệ trong khoảng 0 - 1000 lux
        if (lux2 >= 0.0 && lux2 <= 1000.0) {
            ERa.virtualWrite(V1, lux2); 
            // ERA_LOG("Sensor", "Cam bien 2: %.2f lx", lux2);
            
            if (isAutoMode) {
                controlCB2(lux2);
            }
        } else {
            Serial.printf("-> CB2 loi hoac nhieu: %.2f lx\n", lux2);
        }
    } else {
        Serial.println("-> CB2 chua san sang!");
    }
}

void setup() {
#if defined(ERA_DEBUG)
    Serial.begin(115200); 
#endif

    /* KHỞI TẠO 8 CHÂN RELAY */
    for (int i = 0; i < 8; i++) {
        pinMode(relayPins[i], OUTPUT);
        digitalWrite(relayPins[i], RELAY_OFF); 
    }

    /* KHỞI TẠO CẢM BIẾN I2C */
    Wire.begin(); 
    Serial.println("Dang khoi tao 2 cam bien BH1750...");

    if (lightMeter1.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23)) {
        Serial.println("-> Cam bien 1 (0x23) OK!");
        sensor1_ready = true;
    } else {
        Serial.println("-> LOI: Khong thay cam bien 1");
    }

    if (lightMeter2.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C)) {
        Serial.println("-> Cam bien 2 (0x5C) OK!");
        sensor2_ready = true;
    } else {
        Serial.println("-> LOI: Khong thay cam bien 2");
    }

    ERa.setModbusClient(mbTcpClient);
    ERa.setScanWiFi(true);

    // Chờ kết nối WiFi qua Web Server
    myConfig.begin();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(">> Da co WiFi, khoi dong ERa...");
        String s = myConfig.getSSID();
        String p = myConfig.getPass();
        ERa.begin(s.c_str(), p.c_str());
        era_started = true;
        
        ERa.addInterval(1000L, timerEvent);
    } else {
        Serial.println(">> Khong co WiFi, vui long ket noi vao 'ESP_8Relay' de cau hinh (IP: 192.168.4.1)");
    }
}

void loop() {
    myConfig.loop();

    if (era_started && WiFi.status() == WL_CONNECTED) {
        time_lost_wifi = 0; // Reset biến đếm mất mạng
        ERa.run();
    } else {
        if (era_started == false) {
            return; // Thoát ngay, không làm gì thêm
        }
        
        if (time_lost_wifi == 0) {
            time_lost_wifi = millis();
            Serial.println(">> Mat WiFi! Dang cho ket noi lai...");
        }

        // Nếu mất mạng quá 60 giây -> Tự Reset
        if (millis() - time_lost_wifi > WIFI_TIMEOUT) {
            Serial.println(">> Qua thoi gian cho! Reset de phat WiFi cau hinh...");
            ESP.restart(); // <--- KHỞI ĐỘNG LẠI
        }
    }
}