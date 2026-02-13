# M5StickC Plus - WLED Audio Sync Source

This firmware for M5StickC Plus (version 1) uses the built-in SPM1423 PDM microphone to capture audio and sends it to WLED-enabled ESP devices via UDP for synchronized audio-reactive lighting effects.

The code has been fully generated with Claude AI and has been tested for bugs.

## Features

**Automatic Gain Control (AGC)** - Automatically adapts to room volume  
**Built-in Microphone** - Uses M5StickC Plus internal SPM1423 PDM mic  
**WiFi Configuration Portal** - Easy setup like WLED (no code editing needed)  
**Real-time Display** - Audio levels and frequency spectrum visualization  
**Manual Controls** - Adjust gain, brightness, and transmission on/off  

## Requirements

- **M5StickC Plus** 
- **WLED Device** 
- **WiFi Network** - 2.4GHz 

## Installation

### Using Arduino Cloud Editor 

1. **Go to Arduino Cloud Editor**  
   Visit: https://create.arduino.cc/

2. **Create Account** (if you don't have one)  
   Free account required

3. **Install Arduino Create Agent**  
   - Small plugin (~30MB) that enables USB communication

6. **Create New Sketch**
   - Click **"New Sketch"**
   - Delete any default code

8. **Connect M5StickC Plus**
   - Connect via USB-C cable
   - Select board: ESP32 Dev Module

7. **Copy the Firmware**
   - Copy the entire contents of `M5StickCPlus_WLED_Audio.ino`
   - Paste into the editor

10. **Upload**
    - Click the **Upload** button (→ arrow)
    - Wait ~30-60 seconds
    - Look for "Success" message

### Alternative Installation Methods

This firmware can also be installed using ArduinoIDE or PlatformIO

## First Time Setup

### 1. Powering up
- Replug the USB C cable to power up
- Note that the buttons and audio levels are not responsive before connecting to an external network

### 2. Connect to Hotspot

- Using your phone or computer, connect to the WiFi network **"M5Stick-Audio-xxxxx"**
- No password required

### 3. Configure WiFi

- A web page should open automatically (captive portal)
- If not, open your browser and go to: `192.168.4.1`
- Enter your WiFi network credentials:
  - **SSID**: Your WiFi network name
  - **Password**: Your WiFi password
- Click **"Connect"**

### 4. M5Stick Restarts

- The device will restart and connect to your WiFi
- The display will show:
  - Your IP address
  - "Mic: PDM SPM1423" 
  - Audio level indicator
  - Frequency spectrum bars

## WLED Configuration

### 1. Install WLED on Your LED Controller

- Flash the **Sound Reactive** build of WLED to your ESP32/ESP8266  
(New WLED builds have audio reactive included by default)
- Make sure your WLED is in the same network as the sound module

### 2. Configure WLED to Receive Audio

1. Open WLED web interface (http://[your-wled-ip])
2. Go to **Settings** → **Sound Settings**
3. Set **Sync mode** to **"Receive"**
4. Ensure **UDP Sound Sync** is enabled
5. Port should be **11988** (default)
6. Click **Save**
7. **Power cycle** the WLED device (important!)

### 3. Test Audio Sync

1. Play music near the M5StickC Plus
2. Watch the M5Stick display - audio levels should move
3. In WLED, go to **Info** tab
4. Look for "Audio source" showing the M5Stick's IP
5. Verify packet counter is increasing
6. Try audio-reactive effects in WLED!

## Usage

### Display Information

```
TX ON                  ← Transmission status
IP: 192.168.1.55      ← M5Stick IP address
Mic: PDM SPM1423      ← Microphone type
AGC: ON (45.2x)       ← Auto gain (current multiplier)
Lvl: 175/180          ← Current level / Target level
Pkts: 1234            ← Packets sent

Signal:
████████░░░░          ← Audio level bar (with blue target line)

Freq:
▂▃▅▇█▇▅▃▂            ← Frequency spectrum (16 bins)
```

### Button Controls

#### Button A (Front M5 Button)
- **Single press**: Toggle transmission ON/OFF
- **Hold 4 seconds**: Reset WiFi settings (returns to setup mode)

#### Button B (Side Button)
- **Single press**: Toggle AGC ON/OFF or cycle manual gain presets
  - AGC ON → Manual 10x → 20x → 30x → 50x → 80x → 120x → AGC ON
- **Hold 1 second**: Adjust backlight brightness (3 → 6 → 9 → 12 → 15)

AGC automatically adjusts microphone sensitivity based on room volume:

The blue vertical line on the signal bar shows the AGC target level (180/255).

**Status Colors:**
- 🟢 **Green bar** (0-85): Good signal level
- 🟡 **Yellow bar** (85-200): Optimal range
- 🔴 **Red bar** (200-255): Too loud, may clip

## Troubleshooting

### Can't Connect to WLED

- Verify both devices are on the **same WiFi network**
- Check WLED is set to **"Receive"** mode
- Make sure you power cycled WLED after changing settings
- Check WLED **Info** tab for "Audio source" field

### WiFi Connection Failed

- M5StickC Plus only supports **2.4GHz WiFi** (not 5GHz)
- Double-check password (case-sensitive)
- Hold Button A for 2 seconds to reset WiFi and try again
- Try connecting to a simpler network (no special characters)

### Battery

- I haven't been able to test this using the internal battery, I'm not sure if it's a problem with the code or my device which is quite old

## Specifications

- **Sample Rate**: 16 kHz (optimized for SPM1423 PDM microphone)
- **Frequency Bins**: 16 (for WLED spectrum effects)
- **UDP Port**: 11988 (WLED default)
- **Protocol**: WLED Audio Sync v2
- **Latency**: 15-25ms typical
- **Range**: Standard WiFi range
- **Supported WLED**: Unlimited receivers simultaneously


## Advanced Configuration

### Adjusting AGC Parameters

Edit these values in the code for different scenarios:

```cpp
#define AGC_TARGET_LEVEL 180    // Target level (150-220)
#define AGC_MIN_GAIN 5.0        // Minimum gain multiplier
#define AGC_MAX_GAIN 200.0      // Maximum gain multiplier
#define AGC_ATTACK_RATE 0.05    // How fast gain increases
#define AGC_DECAY_RATE 0.001    // How fast gain decreases
```

**Presets:**
- **Live Music**: Target 200, Attack 0.1, Decay 0.005
- **Ambient/Background**: Target 160, Attack 0.03, Decay 0.001
- **Movies/TV**: Target 170, Attack 0.08, Decay 0.003

### Multiple WLED Devices

The M5Stick broadcasts to **all WLED devices** on your network. To control multiple LED strips:

1. Set all WLED devices to "Receive" mode
2. All will sync to the same audio source automatically
3. No additional configuration needed!

### Specific IP Target (Optional)

To send only to one specific WLED device instead of broadcast:

```cpp
// Change this line:
IPAddress broadcastIP(255, 255, 255, 255);

// To your WLED device IP:
IPAddress broadcastIP(192, 168, 1, 100);
```
