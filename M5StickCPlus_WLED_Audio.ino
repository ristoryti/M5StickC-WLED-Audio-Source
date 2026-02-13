/*
 * M5Stick C Plus - WLED Audio Source with Internal Microphone
 * 
 * CORRECTED VERSION for SPM1423 PDM microphone (I2S)
 * GPIO 0 = CLK
 * GPIO 34 = DATA
 * 
 * Features WiFi portal + AGC
 */

#include <M5StickC.h>  // Note: Using M5StickC.h not M5StickCPlus.h
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <driver/i2s.h>

// ============ CONFIGURATION ============
IPAddress broadcastIP(255, 255, 255, 255);
const uint16_t audioSyncPort = 11988;

// SPM1423 I2S PDM Microphone pins
#define I2S_MODE_PDM_RX
#define I2S_WS_PIN 0      // CLK
#define I2S_SD_PIN 34     // DATA
#define I2S_PORT I2S_NUM_0

// Audio settings
#define SAMPLE_RATE 16000  // PDM mic works best at 16kHz
#define SAMPLES 512
#define SQUELCH 10

// AGC Settings
#define AGC_ENABLED_DEFAULT true
#define AGC_TARGET_LEVEL 180
#define AGC_MIN_GAIN 5.0
#define AGC_MAX_GAIN 200.0
#define AGC_ATTACK_RATE 0.05
#define AGC_DECAY_RATE 0.001
#define AGC_SMOOTHING 0.95

// ============ GLOBAL VARIABLES ============
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;
WiFiUDP udp;

bool transmitting = true;
bool agcEnabled = AGC_ENABLED_DEFAULT;
bool apMode = false;
String ssid = "";
String password = "";
unsigned long lastUpdate = 0;
unsigned long packetCount = 0;
unsigned long buttonHoldTime = 0;

// Audio buffers
int16_t samples[SAMPLES];
uint8_t fftResult[16];
uint8_t samplePeak = 0;

// AGC variables
float currentGain = 30.0;
float smoothedLevel = 0.0;
float peakLevel = 0.0;
unsigned long agcUpdateTimer = 0;

const float gainPresets[] = {10.0, 20.0, 30.0, 50.0, 80.0, 120.0};
int currentGainIndex = 2;
uint8_t brightness = 15; // Backlight brightness (0-15)

// ============ WLED AUDIO SYNC PACKET ============
struct AudioSyncPacket {
    uint8_t header[2] = {'A', 'U'};
    uint8_t version = 2;
    uint8_t sampleRaw;
    uint8_t sampleSmth;
    uint8_t samplePeak;
    uint8_t reserved1 = 0;
    uint8_t fftResult[16];
    uint8_t reserved2[8] = {0};
};

// ============ HTML (same as before) ============
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>M5Stick Audio Setup</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 400px; margin: 50px auto; padding: 20px; background: #f0f0f0; }
        .container { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #333; text-align: center; margin-bottom: 10px; }
        .subtitle { text-align: center; color: #666; font-size: 14px; margin-bottom: 30px; }
        input[type="text"], input[type="password"] { width: 100%; padding: 12px; margin: 8px 0; box-sizing: border-box; border: 2px solid #ddd; border-radius: 5px; font-size: 16px; }
        input[type="submit"] { width: 100%; background-color: #4CAF50; color: white; padding: 14px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; font-weight: bold; margin-top: 10px; }
        input[type="submit"]:hover { background-color: #45a049; }
        label { font-weight: bold; color: #555; display: block; margin-top: 15px; }
        .info { background: #e7f3ff; padding: 15px; border-radius: 5px; margin-top: 20px; font-size: 14px; color: #333; }
    </style>
</head>
<body>
    <div class="container">
        <h1>M5Stick Audio</h1>
        <div class="subtitle">WLED Audio Sync Source</div>
        <form action="/save" method="POST">
            <label for="ssid">WiFi Network:</label>
            <input type="text" id="ssid" name="ssid" placeholder="Your WiFi SSID" required>
            <label for="password">WiFi Password:</label>
            <input type="password" id="password" name="password" placeholder="Password">
            <input type="submit" value="Connect">
        </form>
        <div class="info">
            <strong>Setup Instructions:</strong><br>
            1. Enter your WiFi credentials above<br>
            2. Click "Connect"<br>
            3. M5Stick will restart and connect<br>
            4. Configure WLED to receive on port 11988<br>
            5. Enjoy audio-reactive lighting!
        </div>
    </div>
</body>
</html>
)rawliteral";

const char success_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Connecting...</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 400px; margin: 50px auto; padding: 20px; background: #f0f0f0; }
        .container { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); text-align: center; }
        h1 { color: #4CAF50; }
        .spinner { border: 4px solid #f3f3f3; border-top: 4px solid #4CAF50; border-radius: 50%; width: 40px; height: 40px; animation: spin 1s linear infinite; margin: 30px auto; }
        @keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }
        .info { color: #666; margin-top: 20px; line-height: 1.6; }
    </style>
</head>
<body>
    <div class="container">
        <h1>✓ Settings Saved!</h1>
        <div class="spinner"></div>
        <div class="info">
            M5Stick is connecting to your WiFi...<br><br>
            <strong>Next steps:</strong><br>
            1. Check your router for M5Stick's IP<br>
            2. Or check the M5Stick display<br>
            3. Configure WLED to receive audio on port 11988
        </div>
    </div>
</body>
</html>
)rawliteral";

// ============ SETUP I2S FOR SPM1423 PDM MIC ============
void setupPDMMicrophone() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 2,
        .dma_buf_len = 128,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_PIN_NO_CHANGE,
        .ws_io_num = I2S_WS_PIN,    // CLK - GPIO 0
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD_PIN    // DATA - GPIO 34
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    
    Serial.println("PDM Microphone initialized");
}

// ============ SETUP ============
void setup() {
    M5.begin();
    Serial.begin(115200);
    
    // Proper power management for M5StickC Plus
    M5.Axp.SetLDO2(true);          // Enable backlight power
    M5.Axp.SetLDO3(true);          // Enable display power
    M5.Axp.ScreenBreath(15);       // Set backlight brightness (0-15)
    
    M5.Lcd.setRotation(1);
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    
    // Load WiFi credentials
    preferences.begin("audio-sync", false);
    ssid = preferences.getString("ssid", "");
    password = preferences.getString("password", "");
    preferences.end();
    
    // Setup PDM microphone
    setupPDMMicrophone();
    
    // Try to connect to saved WiFi
    if (ssid.length() > 0) {
        M5.Lcd.println("Connecting");
        M5.Lcd.println("to WiFi...");
        M5.Lcd.setTextSize(1);
        M5.Lcd.println(ssid);
        
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), password.c_str());
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            M5.Lcd.print(".");
            attempts++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            M5.Lcd.fillScreen(TFT_BLACK);
            M5.Lcd.setTextSize(2);
            M5.Lcd.setCursor(0, 0);
            M5.Lcd.setTextColor(TFT_GREEN);
            M5.Lcd.println("CONNECTED!");
            M5.Lcd.setTextColor(TFT_WHITE);
            M5.Lcd.setTextSize(1);
            M5.Lcd.print("IP: ");
            M5.Lcd.println(WiFi.localIP());
            M5.Lcd.println("\nMicrophone: PDM");
            M5.Lcd.println("Starting audio...");
            udp.begin(audioSyncPort);
            apMode = false;
            delay(3000);
        } else {
            M5.Lcd.fillScreen(TFT_BLACK);
            M5.Lcd.setTextSize(2);
            M5.Lcd.setTextColor(TFT_RED);
            M5.Lcd.println("FAILED!");
            M5.Lcd.setTextColor(TFT_WHITE);
            M5.Lcd.setTextSize(1);
            M5.Lcd.println("\nCouldn't connect");
            delay(2000);
            startAPMode();
        }
    } else {
        startAPMode();
    }
    
    updateDisplay();
}

// ============ START AP MODE ============
void startAPMode() {
    apMode = true;
    String apName = "M5Stick-Audio-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("SETUP MODE");
    M5.Lcd.setTextSize(1);
    M5.Lcd.println("\nConnect to WiFi:");
    M5.Lcd.setTextColor(TFT_YELLOW);
    M5.Lcd.println(apName);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.println("\nOpen browser:");
    M5.Lcd.setTextColor(TFT_GREEN);
    M5.Lcd.println("192.168.4.1");
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName.c_str());
    dnsServer.start(53, "*", WiFi.softAPIP());
    
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.onNotFound(handleRoot);
    server.begin();
}

void handleRoot() {
    server.send(200, "text/html", index_html);
}

void handleSave() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
        String newSSID = server.arg("ssid");
        String newPassword = server.arg("password");
        
        preferences.begin("audio-sync", false);
        preferences.putString("ssid", newSSID);
        preferences.putString("password", newPassword);
        preferences.end();
        
        server.send(200, "text/html", success_html);
        delay(2000);
        ESP.restart();
    } else {
        server.send(400, "text/plain", "Missing credentials");
    }
}

// ============ MAIN LOOP ============
void loop() {
    M5.update();
    
    // Keep display awake
    M5.Axp.SetLDO2(true);   // Backlight
    M5.Axp.SetLDO3(true);   // Display power
    
    if (apMode) {
        dnsServer.processNextRequest();
        server.handleClient();
        return;
    }
    
    // WiFi reset (hold Button A for 2 seconds)
    if (M5.BtnA.isPressed()) {
        if (buttonHoldTime == 0) {
            buttonHoldTime = millis();
        } else if (millis() - buttonHoldTime > 4000) {
            preferences.begin("audio-sync", false);
            preferences.clear();
            preferences.end();
            M5.Lcd.fillScreen(TFT_BLACK);
            M5.Lcd.setTextSize(2);
            M5.Lcd.println("RESET!");
            delay(2000);
            ESP.restart();
        }
    } else {
        buttonHoldTime = 0;
    }
    
    // Button controls
    if (M5.BtnA.wasPressed()) {
        transmitting = !transmitting;
        updateDisplay();
    }
    
    if (M5.BtnB.wasPressed()) {
        if (agcEnabled) {
            agcEnabled = false;
            currentGain = gainPresets[currentGainIndex];
        } else {
            currentGainIndex++;
            if (currentGainIndex >= sizeof(gainPresets) / sizeof(gainPresets[0])) {
                currentGainIndex = 0;
                agcEnabled = true;
            } else {
                currentGain = gainPresets[currentGainIndex];
            }
        }
        updateDisplay();
    }
    
    // Hold Button B for 1 second to adjust brightness
    static unsigned long btnBHoldTime = 0;
    if (M5.BtnB.isPressed()) {
        if (btnBHoldTime == 0) {
            btnBHoldTime = millis();
        } else if (millis() - btnBHoldTime > 1000) {
            brightness += 3;
            if (brightness > 15) brightness = 3;
            M5.Axp.ScreenBreath(brightness);
            M5.Lcd.fillScreen(TFT_BLACK);
            M5.Lcd.setTextSize(3);
            M5.Lcd.setCursor(20, 40);
            M5.Lcd.printf("Bright\n  %d/15", brightness);
            delay(500);
            btnBHoldTime = millis(); // Reset for continuous adjustment
            updateDisplay();
        }
    } else {
        btnBHoldTime = 0;
    }
    
    if (transmitting && WiFi.status() == WL_CONNECTED) {
        captureAudio();
        processAudioWithAGC();
        sendAudioSync();
        
        if (millis() - lastUpdate > 100) {
            updateDisplay();
            lastUpdate = millis();
        }
    } else if (WiFi.status() != WL_CONNECTED && !apMode) {
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_RED);
        M5.Lcd.println("WiFi LOST!");
        delay(1000);
    }
    
    delay(10);
}

// ============ CAPTURE AUDIO FROM PDM MIC ============
void captureAudio() {
    size_t bytes_read = 0;
    i2s_read(I2S_PORT, samples, SAMPLES * sizeof(int16_t), &bytes_read, portMAX_DELAY);
}

// ============ PROCESS AUDIO WITH AGC ============
void processAudioWithAGC() {
    int32_t sum = 0;
    int16_t minSample = 32767;
    int16_t maxSample = -32768;
    
    for (int i = 0; i < SAMPLES; i++) {
        sum += samples[i];
        if (samples[i] < minSample) minSample = samples[i];
        if (samples[i] > maxSample) maxSample = samples[i];
    }
    
    int16_t dcOffset = sum / SAMPLES;
    int16_t rawAmplitude = maxSample - minSample;
    
    if (agcEnabled && millis() - agcUpdateTimer > 50) {
        updateAGC(rawAmplitude);
        agcUpdateTimer = millis();
    }
    
    for (int i = 0; i < SAMPLES; i++) {
        samples[i] = (samples[i] - dcOffset) * currentGain / 10.0;
        if (samples[i] > 32767) samples[i] = 32767;
        if (samples[i] < -32768) samples[i] = -32768;
    }
    
    int16_t amplitude = rawAmplitude * currentGain / 10.0;
    if (amplitude < SQUELCH) amplitude = 0;
    
    samplePeak = constrain(amplitude / 128, 0, 255);
    
    if (samplePeak > peakLevel) {
        peakLevel = samplePeak;
    } else {
        peakLevel *= 0.99;
    }
    
    calculateFrequencyBins();
}

void updateAGC(int16_t rawAmplitude) {
    float currentLevel = constrain((rawAmplitude * currentGain / 10.0) / 128.0, 0, 255);
    smoothedLevel = (AGC_SMOOTHING * smoothedLevel) + ((1.0 - AGC_SMOOTHING) * currentLevel);
    
    if (smoothedLevel > SQUELCH * 2) {
        float error = AGC_TARGET_LEVEL - smoothedLevel;
        
        if (error > 0) {
            currentGain += error * AGC_ATTACK_RATE;
        } else {
            currentGain += error * AGC_DECAY_RATE;
        }
        
        currentGain = constrain(currentGain, AGC_MIN_GAIN, AGC_MAX_GAIN);
    }
}

void calculateFrequencyBins() {
    int binSize = SAMPLES / 16;
    
    for (int bin = 0; bin < 16; bin++) {
        int32_t binSum = 0;
        for (int i = 0; i < binSize; i++) {
            int idx = bin * binSize + i;
            binSum += abs(samples[idx]);
        }
        fftResult[bin] = constrain(binSum / binSize / 128, 0, 255);
    }
}

void sendAudioSync() {
    AudioSyncPacket packet;
    
    static uint8_t smoothBuffer[4] = {0};
    static uint8_t smoothIndex = 0;
    
    smoothBuffer[smoothIndex] = samplePeak;
    smoothIndex = (smoothIndex + 1) % 4;
    
    uint16_t smoothSum = 0;
    for (int i = 0; i < 4; i++) {
        smoothSum += smoothBuffer[i];
    }
    
    packet.sampleRaw = samplePeak;
    packet.sampleSmth = smoothSum / 4;
    packet.samplePeak = samplePeak;
    
    memcpy(packet.fftResult, fftResult, 16);
    
    udp.beginPacket(broadcastIP, audioSyncPort);
    udp.write((uint8_t*)&packet, sizeof(packet));
    udp.endPacket();
    
    packetCount++;
}

// ============ DISPLAY UPDATE ============
void updateDisplay() {
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextColor(transmitting ? TFT_GREEN : TFT_RED);
    M5.Lcd.println(transmitting ? "TX ON" : "PAUSED");
    
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.setTextSize(1);
    M5.Lcd.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    M5.Lcd.println("Mic: PDM SPM1423");
    
    if (agcEnabled) {
        M5.Lcd.setTextColor(TFT_GREEN);
        M5.Lcd.printf("AGC: ON (%.1fx)\n", currentGain);
        M5.Lcd.setTextColor(TFT_WHITE);
        M5.Lcd.printf("Lvl: %.0f/%.0f\n", smoothedLevel, (float)AGC_TARGET_LEVEL);
    } else {
        M5.Lcd.setTextColor(TFT_YELLOW);
        M5.Lcd.printf("MANUAL: %.0fx\n", currentGain);
        M5.Lcd.setTextColor(TFT_WHITE);
    }
    
    M5.Lcd.printf("Pkts: %lu\n", packetCount);
    
    // Audio level bar
    M5.Lcd.setCursor(0, 65);
    M5.Lcd.println("Signal:");
    int barWidth = (samplePeak * 120) / 255;
    
    uint16_t barColor;
    if (samplePeak < 85) barColor = TFT_GREEN;
    else if (samplePeak < 200) barColor = TFT_YELLOW;
    else barColor = TFT_RED;
    
    M5.Lcd.fillRect(0, 75, barWidth, 10, barColor);
    M5.Lcd.drawRect(0, 75, 120, 10, TFT_WHITE);
    
    if (agcEnabled) {
        int targetX = (AGC_TARGET_LEVEL * 120) / 255;
        M5.Lcd.drawFastVLine(targetX, 73, 14, TFT_BLUE);
    }
    
    // Frequency visualization
    M5.Lcd.setCursor(0, 90);
    M5.Lcd.println("Freq:");
    for (int i = 0; i < 16; i++) {
        int barHeight = (fftResult[i] * 20) / 255;
        int x = i * 7;
        M5.Lcd.fillRect(x, 115 - barHeight, 6, barHeight, TFT_CYAN);
    }
    
    M5.Lcd.setCursor(0, 120);
    M5.Lcd.setTextSize(1);
    M5.Lcd.println("A:TX B:AGC HoldB:Bri");
}
