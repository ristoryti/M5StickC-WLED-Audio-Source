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
#include <arduinoFFT.h>  // Add FFT library for proper frequency analysis

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
#define SAMPLES 256        // 256 samples for balanced latency and quality (~16ms)
#define SQUELCH 100        // Increased from 80 - higher = more aggressive noise filtering

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
TFT_eSprite sprite = TFT_eSprite(&M5.Lcd);  // Sprite to prevent flicker

// Web UI state
bool webUIActive = false;
unsigned long lastWebUpdate = 0;

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
uint8_t fftResultSmooth[16] = {0}; // Smoothed FFT for display
uint8_t samplePeak = 0;

// FFT arrays for proper frequency analysis
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLE_RATE);

// AGC variables
float currentGain = 30.0;
float smoothedLevel = 0.0;
float peakLevel = 0.0;
unsigned long agcUpdateTimer = 0;

const float gainPresets[] = {20.0, 40.0, 60.0, 80.0, 100.0, 120.0, 140.0, 160.0, 180.0, 200.0};
int currentGainIndex = 2;  // Start at 60
uint8_t brightness = 15; // Backlight brightness - useful range on M5StickC is 7-15

// User-customizable presets (loaded from preferences)
float userPreset1 = 50.0;
float userPreset2 = 100.0;
float userPreset3 = 150.0;

// ============ WLED AUDIO SYNC PACKET ============
// V2 Format for WLED 0.14.0+ (MoonModules)
struct AudioSyncPacket {
    char header[6] = "00002";        // V2 header (not 'A','U'!)
    float sampleRaw;                 // 4 bytes - raw sample
    float sampleSmth;                // 4 bytes - smoothed sample  
    uint8_t samplePeak;              // 1 byte - peak detection
    uint8_t reserved1 = 0;           // 1 byte - reserved
    uint8_t fftResult[16];           // 16 bytes - FFT results
    float FFT_Magnitude;             // 4 bytes - FFT magnitude
    float FFT_MajorPeak;             // 4 bytes - FFT major peak
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
        <h1>Settings Saved!</h1>
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

const char control_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>M5Stick Audio Control</title>
    <style>
        body { font-family: Arial; max-width: 600px; margin: 20px auto; padding: 20px; background: #1a1a1a; color: #fff; }
        .container { background: #2a2a2a; padding: 20px; border-radius: 10px; }
        h1 { color: #4CAF50; margin: 0 0 20px 0; }
        .status { background: #333; padding: 10px; border-radius: 5px; margin-bottom: 20px; }
        .control-group { margin: 20px 0; }
        label { display: block; margin-bottom: 5px; color: #aaa; }
        input[type="range"] { width: 100%; height: 8px; background: #444; border-radius: 5px; }
        input[type="range"]::-webkit-slider-thumb { width: 20px; height: 20px; background: #4CAF50; border-radius: 50%; cursor: pointer; }
        .value-display { font-size: 24px; color: #4CAF50; text-align: center; margin: 10px 0; }
        .preset-buttons { display: flex; gap: 10px; margin: 20px 0; }
        button { flex: 1; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 14px; }
        button:hover { background: #45a049; }
        button.secondary { background: #666; }
        button.secondary:hover { background: #777; }
        .visualizer { background: #1a1a1a; height: 100px; margin: 20px 0; border-radius: 5px; padding: 10px; display: flex; align-items: flex-end; gap: 2px; }
        .bar { flex: 1; background: linear-gradient(to top, #4CAF50, #8BC34A); min-height: 2px; border-radius: 2px 2px 0 0; transition: height 0.1s; }
        .level-bar { height: 30px; background: #1a1a1a; border-radius: 5px; margin: 10px 0; position: relative; overflow: hidden; }
        .level-fill { height: 100%; background: linear-gradient(to right, #4CAF50, #FFC107, #F44336); transition: width 0.1s; }
        .agc-toggle { background: #666; }
        .agc-toggle.active { background: #4CAF50; }
    </style>
</head>
<body>
    <div class="container">
        <h1>M5Stick Audio Control</h1>
        
        <div class="status">
            <div>Status: <span id="status">Connected</span></div>
            <div>Gain: <span id="currentGain">--</span>x</div>
            <div>Mode: <span id="mode">--</span></div>
            <div>Packets: <span id="packets">--</span></div>
        </div>

        <div class="control-group">
            <label>Manual Gain (20-200)</label>
            <input type="range" id="gainSlider" min="20" max="200" value="100" oninput="updateGain()">
            <div class="value-display"><span id="gainValue">100</span>x</div>
        </div>

        <div class="control-group">
            <label>Quick Presets</label>
            <div class="preset-buttons">
                <button onclick="setGain(userPreset1)">P1: <span id="p1val">50</span></button>
                <button onclick="setGain(userPreset2)">P2: <span id="p2val">100</span></button>
                <button onclick="setGain(userPreset3)">P3: <span id="p3val">150</span></button>
            </div>
        </div>

        <div class="control-group">
            <label>Audio Level</label>
            <div class="level-bar">
                <div class="level-fill" id="levelBar" style="width: 0%"></div>
            </div>
        </div>

        <div class="control-group">
            <label>Frequency Spectrum</label>
            <div class="visualizer" id="spectrum"></div>
        </div>

        <div class="preset-buttons">
            <button id="agcBtn" class="agc-toggle" onclick="toggleAGC()">AGC: OFF</button>
            <button class="secondary" onclick="savePreset(1)">Save P1</button>
            <button class="secondary" onclick="savePreset(2)">Save P2</button>
            <button class="secondary" onclick="savePreset(3)">Save P3</button>
        </div>
    </div>

    <script>
        let userPreset1 = 50, userPreset2 = 100, userPreset3 = 150;
        
        // Create spectrum bars
        const spectrum = document.getElementById('spectrum');
        for(let i = 0; i < 14; i++) {
            const bar = document.createElement('div');
            bar.className = 'bar';
            bar.style.height = '2px';
            spectrum.appendChild(bar);
        }

        function updateGain() {
            const val = document.getElementById('gainSlider').value;
            document.getElementById('gainValue').textContent = val;
            fetch('/api/gain?value=' + val);
        }

        function setGain(val) {
            document.getElementById('gainSlider').value = val;
            document.getElementById('gainValue').textContent = val;
            fetch('/api/gain?value=' + val);
        }

        function toggleAGC() {
            fetch('/api/agc').then(r => r.json()).then(d => {
                document.getElementById('agcBtn').textContent = 'AGC: ' + (d.agc ? 'ON' : 'OFF');
                document.getElementById('agcBtn').classList.toggle('active', d.agc);
            });
        }

        function savePreset(num) {
            const val = document.getElementById('gainSlider').value;
            fetch('/api/preset?num=' + num + '&value=' + val).then(() => {
                if(num == 1) { userPreset1 = val; document.getElementById('p1val').textContent = val; }
                if(num == 2) { userPreset2 = val; document.getElementById('p2val').textContent = val; }
                if(num == 3) { userPreset3 = val; document.getElementById('p3val').textContent = val; }
            });
        }

        function updateStatus() {
            fetch('/api/status').then(r => r.json()).then(d => {
                document.getElementById('currentGain').textContent = d.gain.toFixed(1);
                document.getElementById('mode').textContent = d.agc ? 'AGC' : 'Manual';
                document.getElementById('packets').textContent = d.packets;
                document.getElementById('levelBar').style.width = ((d.level / 255) * 100) + '%';
                
                // Update spectrum
                const bars = spectrum.children;
                for(let i = 0; i < 14 && i < d.fft.length; i++) {
                    bars[i].style.height = ((d.fft[i] / 255) * 100) + '%';
                }
                
                // Update AGC button
                document.getElementById('agcBtn').textContent = 'AGC: ' + (d.agc ? 'ON' : 'OFF');
                document.getElementById('agcBtn').classList.toggle('active', d.agc);
                
                // Update slider if in manual mode
                if(!d.agc) {
                    document.getElementById('gainSlider').value = d.gain;
                    document.getElementById('gainValue').textContent = d.gain.toFixed(0);
                }
            }).catch(() => document.getElementById('status').textContent = 'Disconnected');
        }

        // Load presets on start
        fetch('/api/presets').then(r => r.json()).then(d => {
            userPreset1 = d.p1; userPreset2 = d.p2; userPreset3 = d.p3;
            document.getElementById('p1val').textContent = d.p1;
            document.getElementById('p2val').textContent = d.p2;
            document.getElementById('p3val').textContent = d.p3;
        });

        // Update every 100ms
        setInterval(updateStatus, 100);
        updateStatus();
    </script>
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
        .dma_buf_count = 4,          // Increased from 2
        .dma_buf_len = 256,          // Increased from 128
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_PIN_NO_CHANGE,
        .ws_io_num = I2S_WS_PIN,    // CLK - GPIO 0
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD_PIN    // DATA - GPIO 34
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    
    // Critical: Set PDM RX downsample mode
    i2s_set_pdm_rx_down_sample(I2S_PORT, I2S_PDM_DSR_8S);
    
    Serial.println("PDM Microphone initialized with proper decimation");
}

// ============ SETUP ============
void setup() {
    M5.begin();
    Serial.begin(115200);
    
    // Proper power management for M5StickC Plus
    M5.Axp.SetLDO2(true);          // Enable backlight power
    M5.Axp.SetLDO3(true);          // Enable display power
    M5.Axp.ScreenBreath(15);       // Set backlight brightness to max (0-15)
    
    // Battery charging is handled automatically by AXP192
    // When USB is connected: charges battery + powers device
    // When USB disconnected: runs from battery
    // No additional code needed for battery operation
    
    // Setup display - Portrait orientation (80 wide x 160 tall)
    M5.Lcd.setRotation(0);  // Portrait mode: 80 wide x 160 tall
    M5.Lcd.fillScreen(TFT_BLACK);
    
    // Create sprite buffer to prevent flicker (80x160)
    sprite.createSprite(80, 160);
    sprite.setSwapBytes(false);
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
            M5.Lcd.println("Web UI: http://");
            M5.Lcd.println(WiFi.localIP());
            
            udp.begin(audioSyncPort);
            
            // Load user presets
            preferences.begin("audio-sync", false);
            userPreset1 = preferences.getFloat("preset1", 50.0);
            userPreset2 = preferences.getFloat("preset2", 100.0);
            userPreset3 = preferences.getFloat("preset3", 150.0);
            currentGain = preferences.getFloat("gain", 100.0);
            agcEnabled = preferences.getBool("agc", AGC_ENABLED_DEFAULT);
            brightness = preferences.getUChar("brightness", 15);
            preferences.end();
            
            // Apply loaded brightness
            M5.Axp.ScreenBreath(brightness);
            
            // Setup web control server
            server.on("/", handleControl);
            server.on("/api/status", handleStatus);
            server.on("/api/gain", handleGain);
            server.on("/api/agc", handleAGC);
            server.on("/api/preset", handlePresetSave);
            server.on("/api/presets", handlePresets);
            server.begin();
            
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

void handleControl() {
    server.send(200, "text/html", control_html);
}

void handleStatus() {
    String json = "{";
    json += "\"gain\":" + String(currentGain, 1) + ",";
    json += "\"agc\":" + String(agcEnabled ? "true" : "false") + ",";
    json += "\"packets\":" + String(packetCount) + ",";
    json += "\"level\":" + String(samplePeak) + ",";
    json += "\"fft\":[";
    for (int i = 0; i < 14; i++) {
        json += String(fftResultSmooth[i]);
        if (i < 13) json += ",";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleGain() {
    if (server.hasArg("value")) {
        float newGain = server.arg("value").toFloat();
        newGain = constrain(newGain, 20.0, 200.0);
        currentGain = newGain;
        agcEnabled = false;
        // Save to preferences
        preferences.begin("audio-sync", false);
        preferences.putFloat("gain", currentGain);
        preferences.putBool("agc", false);
        preferences.end();
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        server.send(400, "application/json", "{\"error\":\"missing value\"}");
    }
}

void handleAGC() {
    agcEnabled = !agcEnabled;
    // Save AGC state
    preferences.begin("audio-sync", false);
    preferences.putBool("agc", agcEnabled);
    preferences.end();
    String json = "{\"agc\":" + String(agcEnabled ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

void handlePresetSave() {
    if (server.hasArg("num") && server.hasArg("value")) {
        int num = server.arg("num").toInt();
        float value = server.arg("value").toFloat();
        value = constrain(value, 20.0, 200.0);
        
        preferences.begin("audio-sync", false);
        if (num == 1) {
            userPreset1 = value;
            preferences.putFloat("preset1", value);
        } else if (num == 2) {
            userPreset2 = value;
            preferences.putFloat("preset2", value);
        } else if (num == 3) {
            userPreset3 = value;
            preferences.putFloat("preset3", value);
        }
        preferences.end();
        
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        server.send(400, "application/json", "{\"error\":\"missing parameters\"}");
    }
}

void handlePresets() {
    String json = "{";
    json += "\"p1\":" + String(userPreset1, 0) + ",";
    json += "\"p2\":" + String(userPreset2, 0) + ",";
    json += "\"p3\":" + String(userPreset3, 0);
    json += "}";
    server.send(200, "application/json", json);
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
    
    // WiFi reset (hold Button A for 4 seconds)
    if (M5.BtnA.isPressed()) {
        if (buttonHoldTime == 0) {
            buttonHoldTime = millis();
        } else if (millis() - buttonHoldTime > 4000) {  // Changed from 2000 to 4000
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
            // AGC ON -> switch to manual mode at first preset (20)
            agcEnabled = false;
            currentGainIndex = 0;
            currentGain = gainPresets[currentGainIndex];
        } else {
            // Cycle through manual gains: 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, then back to AGC
            currentGainIndex++;
            if (currentGainIndex >= sizeof(gainPresets) / sizeof(gainPresets[0])) {
                // After 200, go back to AGC
                currentGainIndex = 0;
                agcEnabled = true;
            } else {
                currentGain = gainPresets[currentGainIndex];
            }
        }
        // Save gain and AGC state
        preferences.begin("audio-sync", false);
        preferences.putFloat("gain", currentGain);
        preferences.putBool("agc", agcEnabled);
        preferences.end();
        updateDisplay();
    }
    
    // Hold Button B for 1 second to adjust brightness
    static unsigned long btnBHoldTime = 0;
    if (M5.BtnB.isPressed()) {
        if (btnBHoldTime == 0) {
            btnBHoldTime = millis();
        } else if (millis() - btnBHoldTime > 1000) {
            // Cycle through useful brightness range: 8, 10, 12, 15
            if (brightness < 10) brightness = 10;
            else if (brightness < 12) brightness = 12;
            else if (brightness < 15) brightness = 15;
            else brightness = 8;  // Back to minimum visible
            
            M5.Axp.ScreenBreath(brightness);
            
            // Save brightness to preferences
            preferences.begin("audio-sync", false);
            preferences.putUChar("brightness", brightness);
            preferences.end();
            
            M5.Lcd.fillScreen(TFT_BLACK);
            M5.Lcd.setTextSize(3);
            M5.Lcd.setCursor(10, 40);
            M5.Lcd.printf("Bright\n  %d/15", brightness);
            delay(500);
            btnBHoldTime = millis();
            updateDisplay();
        }
    } else {
        btnBHoldTime = 0;
    }
    
    if (transmitting && WiFi.status() == WL_CONNECTED) {
        server.handleClient();  // Handle web requests
        
        captureAudio();
        processAudioWithAGC();
        calculateFrequencyBins();  // Calculate FFT every time for fresh data
        sendAudioSync();
        
        // Update display less frequently
        if (millis() - lastUpdate > 100) {
            updateDisplay();
            lastUpdate = millis();
        }
        
        // Small delay for proper pacing - prevents UDP flooding
        // This gives ~60 packets/second which is optimal for WLED
        delay(15);
    } else if (WiFi.status() != WL_CONNECTED && !apMode) {
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_RED);
        M5.Lcd.println("WiFi LOST!");
        delay(1000);
    }
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
    
    // Apply squelch/noise gate
    if (amplitude < SQUELCH) {
        amplitude = 0;
    } else {
        // Subtract noise floor
        amplitude -= SQUELCH;
    }
    
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
    // Remove DC offset FIRST (critical for PDM mics!)
    int32_t dcSum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        dcSum += samples[i];
    }
    int16_t dcOffset = dcSum / SAMPLES;
    
    // Copy samples to FFT with DC removal
    for (int i = 0; i < SAMPLES; i++) {
        vReal[i] = (double)(samples[i] - dcOffset);
        vImag[i] = 0.0;
    }
    
    // Perform FFT
    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();
    
    // LOW-PASS FILTER: Zero out everything above 6kHz (bin 96 for 256 samples)
    // This eliminates harmonics and distortion products
    for (int i = 96; i < 128; i++) {
        vReal[i] = 0;
    }
    
    // Debug counter
    static int debugCount = 0;
    debugCount++;
    bool shouldDebug = (debugCount % 50 == 0);
    
    if (shouldDebug) {
        double peakFreq = FFT.majorPeak();
        Serial.printf("\nDominant: %.0f Hz\n", peakFreq);
    }
    
    // UPDATED frequency bands for 256 SAMPLES (128 FFT bins)
    // Freq per bin = 16000/256 = 62.5 Hz
    // Bar 0: ~50-100Hz, Bar 1: ~100-200Hz, rest evenly distributed
    
    const int barBins[16][2] = {
        {1, 2},       // Bar 0: 62-125 Hz
        {2, 3},       // Bar 1: 125-188 Hz
        {3, 5},       // Bar 2: 188-312 Hz
        {5, 7},       // Bar 3: 312-438 Hz
        {7, 10},      // Bar 4: 438-625 Hz
        {10, 14},     // Bar 5: 625-875 Hz
        {14, 18},     // Bar 6: 875-1125 Hz
        {18, 24},     // Bar 7: 1125-1500 Hz
        {24, 31},     // Bar 8: 1500-1938 Hz
        {31, 39},     // Bar 9: 1938-2438 Hz
        {39, 50},     // Bar 10: 2438-3125 Hz
        {50, 63},     // Bar 11: 3125-3938 Hz
        {63, 79},     // Bar 12: 3938-4938 Hz
        {79, 96},     // Bar 13: 4938-6000 Hz
        {96, 96},     // Bar 14: DISABLED (above 6kHz)
        {96, 96}      // Bar 15: DISABLED (above 6kHz)
    };
    
    if (shouldDebug) {
        Serial.print("Bars: ");
    }
    
    // Calculate each bar
    for (int bar = 0; bar < 16; bar++) {
        int binStart = barBins[bar][0];
        int binEnd = barBins[bar][1];
        
        double maxVal = 0;
        
        for (int bin = binStart; bin < binEnd; bin++) {
            if (vReal[bin] > maxVal) {
                maxVal = vReal[bin];
            }
        }
        
        // Scale
        int32_t scaled = (int32_t)(maxVal / 1000.0);
        
        // Noise gate
        if (scaled < SQUELCH / 2) {
            scaled = 0;
        } else {
            scaled -= (SQUELCH / 2);
        }
        
        fftResult[bar] = constrain(scaled, 0, 255);
        
        if (shouldDebug) {
            Serial.printf("%3d ", fftResult[bar]);
        }
        
        // Smooth
        if (fftResult[bar] > fftResultSmooth[bar]) {
            fftResultSmooth[bar] = fftResult[bar];
        } else {
            fftResultSmooth[bar] = (fftResultSmooth[bar] * 7 + fftResult[bar]) / 8;
        }
    }
    
    if (shouldDebug) {
        Serial.println();
    }
}

void sendAudioSync() {
    AudioSyncPacket packet;
    
    // NOISE GATE: Don't send anything if audio level is too low
    if (samplePeak < SQUELCH) {
        // Send zeros to WLED (silence)
        packet.sampleRaw = 0.0;
        packet.sampleSmth = 0.0;
        packet.samplePeak = 0;
        memset(packet.fftResult, 0, 16);
        packet.FFT_Magnitude = 0.0;
        packet.FFT_MajorPeak = 0.0;
        
        udp.beginPacket(broadcastIP, audioSyncPort);
        udp.write((uint8_t*)&packet, sizeof(packet));
        udp.endPacket();
        
        packetCount++;
        return;
    }
    
    static uint8_t smoothBuffer[4] = {0};
    static uint8_t smoothIndex = 0;
    
    smoothBuffer[smoothIndex] = samplePeak;
    smoothIndex = (smoothIndex + 1) % 4;
    
    uint16_t smoothSum = 0;
    for (int i = 0; i < 4; i++) {
        smoothSum += smoothBuffer[i];
    }
    
    // Convert to float format for V2 protocol
    packet.sampleRaw = (float)samplePeak;
    packet.sampleSmth = (float)(smoothSum / 4);
    packet.samplePeak = samplePeak;
    
    // Copy FFT results (calculated less frequently now)
    // Apply additional noise gate to frequency bins
    for (int i = 0; i < 16; i++) {
        if (fftResult[i] < SQUELCH / 4) {
            packet.fftResult[i] = 0;
        } else {
            packet.fftResult[i] = fftResult[i];
        }
    }
    
    // Simple FFT stats (don't recalculate, use cached fftResult)
    packet.FFT_Magnitude = 0.0;
    packet.FFT_MajorPeak = 0.0;
    uint8_t maxBin = 0;
    int maxBinIndex = 0;
    
    for (int i = 0; i < 16; i++) {
        if (packet.fftResult[i] > maxBin) {
            maxBin = packet.fftResult[i];
            maxBinIndex = i;
        }
        packet.FFT_Magnitude += packet.fftResult[i];
    }
    packet.FFT_MajorPeak = maxBinIndex * (SAMPLE_RATE / 2.0 / 16.0);
    packet.FFT_Magnitude /= 16.0;
    
    // Send packet - this is now FAST!
    udp.beginPacket(broadcastIP, audioSyncPort);
    udp.write((uint8_t*)&packet, sizeof(packet));
    udp.endPacket();
    
    packetCount++;
}

// ============ DISPLAY UPDATE ============
void updateDisplay() {
    // M5StickC display in portrait: 80 pixels wide x 160 pixels tall
    // Draw to sprite first (prevents flicker), then push to screen
    
    sprite.fillSprite(TFT_BLACK);
    sprite.setTextSize(1);
    sprite.setTextDatum(TL_DATUM);
    
    // Add 2 pixel margins on left and top
    int marginLeft = 2;
    int marginTop = 2;
    
    // Top section: Status info
    sprite.setCursor(marginLeft, marginTop);
    sprite.setTextColor(transmitting ? TFT_GREEN : TFT_RED);
    sprite.print(transmitting ? "TX " : "OFF");
    
    sprite.setTextColor(TFT_WHITE);
    String ip = WiFi.localIP().toString();
    int lastDot = ip.lastIndexOf('.');
    sprite.print(".");
    sprite.println(ip.substring(lastDot + 1));
    
    // Gain info
    sprite.setCursor(marginLeft, marginTop + 10);
    if (agcEnabled) {
        sprite.setTextColor(TFT_GREEN);
        sprite.printf("AGC:%.0f\n", currentGain);
    } else {
        sprite.setTextColor(TFT_YELLOW);
        sprite.printf("MAN:%.0f\n", currentGain);
    }
    
    sprite.setCursor(marginLeft, marginTop + 18);
    sprite.setTextColor(TFT_WHITE);
    sprite.printf("Pkt:%lu", packetCount);
    
    // Audio level bar (horizontal, thicker - 12 pixels tall instead of 6)
    int barY = marginTop + 28;
    int barWidth = (samplePeak * 74) / 255;  // 74 pixels (80 - 2 margins - 4 padding)
    uint16_t barColor;
    if (samplePeak < 85) barColor = TFT_GREEN;
    else if (samplePeak < 200) barColor = TFT_YELLOW;
    else barColor = TFT_RED;
    
    sprite.fillRect(marginLeft + 1, barY, barWidth, 12, barColor);
    sprite.drawRect(marginLeft + 1, barY, 74, 12, TFT_WHITE);
    
    if (agcEnabled) {
        int targetX = marginLeft + 1 + (AGC_TARGET_LEVEL * 74) / 255;
        sprite.drawFastVLine(targetX, barY - 1, 14, TFT_BLUE);
    }
    
    // Frequency visualizer - starts at y=45, reduced height
    sprite.setCursor(marginLeft, 44);
    sprite.setTextColor(TFT_CYAN);
    sprite.println("FREQ");
    
    // 16 frequency bars, 95 pixels tall max (reduced from 105)
    // Bars start at x=0 (full width, bass on left, treble on right)
    for (int i = 0; i < 16; i++) {
        // Reduce boost from 15x to 5x since FFT is already sensitive
        int boostedValue = min(255, fftResultSmooth[i] * 5);
        int barHeight = (boostedValue * 95) / 255;  // Reduced from 105 to 95
        int x = i * 5;
        
        if (barHeight > 1) {
            sprite.fillRect(x, 157 - barHeight, 4, barHeight, TFT_CYAN);
        }
    }
    
    // Baseline for freq bars
    sprite.drawFastHLine(0, 157, 80, TFT_WHITE);
    
    // Push sprite to LCD (single operation, no flicker!)
    sprite.pushSprite(0, 0);
}
