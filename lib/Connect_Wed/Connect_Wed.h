#ifndef CONNECT_WED_H
#define CONNECT_WED_H

#include <Arduino.h>
#include <WebServer.h>

class Connect_Wed {
public:
    Connect_Wed(const char* apName);
    void begin();
    void loop();
    void RST_ERROOM();

    // [THÊM MỚI] Hàm để lấy SSID và Pass ra ngoài cho ERa dùng
    String getSSID();
    String getPass();

private:
    const char* _ap_ssid;
    WebServer* _server;

    void _setupAP();
    bool _autoConnectFromEEPROM();
    void _handleRoot();
    void _handleConnect();
    String _buildHtmlPage();
    void _writeStringToEEPROM(int addr, const String& str);
    String _readStringFromEEPROM(int addr);
};

#endif