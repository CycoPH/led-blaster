# LED Blaster — ESP32 LED Strip Game

---

## Table of Contents

1. [Getting Connected](#getting-connected)
2. [Wiring Quick Reference](#wiring-quick-reference)
3. [ASCII Pinout Diagram](#ascii-pinout-diagram)
4. [Audio File Format](#audio-file-format)
5. [Game Features](#game-features)
6. [How to Build and Upload](#how-to-build-and-upload)
7. [Hardware Description](#hardware-description)

---

## Getting Connected

1. Upload the code to your ESP32.
2. On your phone or laptop, scan for Wi-Fi networks.
3. Connect to **LED_BLASTER** using password `password123`.
4. Open your web browser and navigate to `http://192.168.4.1`
5. You should see the LED Blaster web UI.

---

## Wiring Quick Reference

### Main Components

| Component | Pin on Component | Pin on ESP32 | Note |
|-----------|-----------------|--------------|------|
| LED Strip | Data In (DIN) | GPIO 5 | Use a 330 Ω resistor in series |
| | 5V / VCC | Ext. 5V (+) | Connect to external power supply |
| | GND | GND | Connect to Ext. Power (−) AND ESP32 GND |
| Red Button | One Leg | GPIO 18 | Internal pull-ups enabled |
| | Other Leg | GND | |
| Green Button | One Leg | GPIO 19 | Internal pull-ups enabled |
| | Other Leg | GND | |
| Blue Button | One Leg | GPIO 21 | Internal pull-ups enabled |
| | Other Leg | GND | |

### Audio Amplifier (MAX98357A)

| Pin | Connects to | Note |
|-----|-------------|------|
| VIN / VCC | Ext. 5V (+) | Same source as LED strip |
| GND | GND | Shared GND |
| LRC | GPIO 25 | Left/Right Clock |
| BCLK | GPIO 26 | Bit Clock |
| DIN | GPIO 22 | Audio Data |
| GAIN | GND | 12 dB gain (loudest). Leave unconnected for 9 dB. |

### Menu Buttons

| Button | GPIO | Function |
|--------|------|----------|
| Restart | GPIO 13 | Short press = restart game. Hold 2 s = start Survival Mode. |
| Level Up | GPIO 12 | Skip to the next level |
| Game Type | GPIO 14 | Cycle active LED count: 120 → 180 → 240 → 300 |

### Status LEDs

| Colour | GPIO | Resistor | Indicates |
|--------|------|----------|-----------|
| Red | GPIO 33 | 220–330 Ω | Remaining lives |
| Green | GPIO 32 | 220–330 Ω | Current combo multiplier |
| Blue | GPIO 27 | 220–330 Ω | Power-up / charge availability |

### Full Wiring Summary

```
LED Strip:      Data Pin → GPIO 5
Shoot Buttons:  Red → GPIO 18, Green → GPIO 19, Blue → GPIO 21
Menu Buttons:   Restart → GPIO 13, Level Up → GPIO 12, Game Type → GPIO 14
MAX98357A:      LRC → GPIO 25, BCLK → GPIO 26, DIN → GPIO 22
Status LEDs:    Red → GPIO 33, Green → GPIO 32, Blue → GPIO 27
                (each through a 220–330 Ω resistor to GND)
```

---

## ASCII Pinout Diagram

```
           +------------------------------------------------+
           |                  ESP32 DEVKIT V1               |
           |                                                |
           | [USB PORT]                                     |
           |                                                |
  +5V/VIN--+ VIN                                        GND +--+---(GND Logic)
           |                                                |  |
           |                                                |  |
  [GAME BUTTONS - Connect other side to GND]                |  |
           |                                                |  |
  Red Btn--+ D18                                        D22 +--+--> I2S DIN  (Audio)
  Grn Btn--+ D19                                        D26 +--+--> I2S BCLK (Audio)
  Blu Btn--+ D21                                        D25 +--+--> I2S LRC  (Audio)
  Restart--+ D13                                            |
  LevelUp--+ D12                                            |
  GmeType--+ D14                                            |
           |                                                |
  [WS2812 LED STRIP]                                        |
           |                                                |
  Data In--+ D5                                             |
           |                                                |
  [STATUS LEDs - Connect Anode(+), Cathode(-) to Resistor]  |
           |                                                |
  Red LED--+ D33 ----[330Ω]----(GND)                        |
  Grn LED--+ D32 ----[330Ω]----(GND)                        |
  Blu LED--+ D27 ----[330Ω]----(GND)                        |
           +------------------------------------------------+

  * AUDIO NOTE: I2S pins connect to MAX98357A Amp or DAC
  * POWER NOTE: Ensure LED strip has external 5V if using > 30 LEDs
```

---

## Audio File Format

Use a tool like Audacity to export sounds with these exact settings:

| Setting | Value |
|---------|-------|
| Format | WAV (Microsoft) |
| Sample Rate | 22050 Hz (reduces I2S/LED interference) |
| Channels | Mono |
| Bit Depth | Signed 16-bit PCM |

### Recommended Sound Style

| Event | Style |
|-------|-------|
| Start Level | Classic 8-bit "Ready" or "Go" chime |
| Fire | Short, sharp "pew" or high-frequency click (< 50 ms) |
| Fire Power Show | Bass-heavy synth boom or distorted laser shot |
| Explosion | Bit-crushed glass break or crunch — keep it short |
| Level Done | Rising arpeggio or major-key "success" jingle |
| Game Over | Epic "Victory" melody or dramatic "Game Over" siren |

---

## Game Features

### Core Gameplay

- 120 WS2812B LEDs (expandable to 180 / 240 / 300 via GAME TYPE button)
- Red, Green, and Blue enemies spawn at the far end and march toward the player
- Player fires coloured projectiles with R / G / B buttons (physical or web UI)
- Matching colour destroys the enemy; mismatched colour causes the enemy to split into two slower enemies (penalty for wrong shots)
- Enemy reaches position 0 → lose a life
- Bullet travel speed scales with strip length (faster on shorter strips)

### Lives System

- Player starts with 3 lives (shown as hearts ♥♥♥ in the web UI)
- Losing a life resets enemy positions for the current level (speed also resets to the level baseline)
- All 3 lives lost → GAME OVER

### Level Progression

- Levels increase enemy speed and spawn count automatically
- Completing a level awards a time bonus (bigger bonus = faster clear)
- Level number shown on the LED strip at level start (1 white LED per level)
- Comet celebration animation plays on level clear
- Level 15+ introduces **ARMORED** enemies (must hit twice to destroy)
- Every 3rd level spawns a **BOSS** enemy (3 hits, 3× score)

### Scoring

| Event | Points |
|-------|--------|
| Enemy destroyed | 10 pts × combo multiplier |
| Boss kill | 30 pts × combo |
| Gold enemy kill | normal pts × 5 |
| Level complete bonus | up to 200 pts (based on time remaining) |

- Combo multiplier increases with consecutive correct hits; resets on a miss
- Accuracy percentage tracked and shown in the web UI
- High score saved to SPIFFS (`/highscore.json`) and survives power cycles

### Game Type / Strip Length

Press the GAME TYPE button (GPIO 14) or the web UI button to cycle:

```
120 LEDs → 180 LEDs → 240 LEDs → 300 LEDs → (wraps back to 120)
```

Active count is applied immediately; the web strip display resizes automatically.

### Enemy Types

| Type | Description |
|------|-------------|
| **Standard** | Random R / G / B colour. Destroyed by matching shot. |
| **Power-Up** | Rainbow colour. Any shot collects a power-up charge for that colour. |
| **Armored** | Grey-tinted. Requires 2 hits. Appears from level 15 onwards. |
| **Boss** | Large bright enemy every 3rd level. 3 hits to kill, 3× score. |
| **Gold** | Amber/yellow pulsing (5% spawn chance). Requires 2 **different** colour hits within 2 seconds for a 5× score reward. Wrong second hit spawns 3 penalty enemies and resets your combo. |
| **Bomb** | Magenta pulsing (4% spawn chance, level 5+). Any hit triggers an explosion clearing all enemies within ±5 LEDs (score per kill). Reaching position 0 = instant game over. |

### Special Mechanics

| Mechanic | Description |
|----------|-------------|
| **❄ Freeze** | Hold all 3 shoot buttons simultaneously (or press ❄ FREEZE in the web UI). Freezes all enemies for 3 seconds. Available **once per level**. Glows blue in the UI when ready. |
| **Reinforcements** | When 60% of the starting enemies are cleared, a wave of 5 new enemies spawns from the rear (once per round). |
| **Speed Ramp** | Every 10 seconds of active play enemy speed increases by 5%. Resets on life loss. |
| **Danger Zone** | When any enemy is within the first 5 LEDs a red pulse overlays those LEDs as a warning. |

### Survival Mode

- Hold the RESTART button for 2 seconds (or press **∞ SURVIVAL** in the web UI) to start an endless Survival session.
- Player has **1 life**. A new enemy spawns every 2.5 seconds (cap: 15 on strip).
- No levels — score as high as you can before losing.
- Separate **SURVIVAL HIGH SCORE** saved to SPIFFS (`/survival_hs.json`).
- Web UI shows `[ ∞ SURVIVAL MODE ]` banner and **HI SURV** stat.

**How Survival Mode ends:**
- Survival mode has only 1 life. When any enemy reaches position 0 (or a Bomb enemy reaches the front), that life is lost and the game enters GAME OVER.
- There is no mid-session escape — you must play until you die.
- On the GAME OVER screen, press any R / G / B button (physical or web UI) to return to a normal game starting at level 1. Survival mode is fully cleared at that point.
- If a new Survival high score was set, it is saved automatically before the GAME OVER screen is shown.

### Status LEDs (Physical)

| LED | GPIO | Indicates |
|-----|------|-----------|
| Red | GPIO 33 | Remaining lives (PWM brightness) |
| Green | GPIO 32 | Current combo level (PWM brightness) |
| Blue | GPIO 27 | Charge / power-up availability (PWM brightness) |

### Auto-Start

After the level intro animation, a short countdown begins automatically so the round starts without requiring a button press.

### Web UI Controls

| Button | Action |
|--------|--------|
| RED / GRN / BLU | Fire coloured shots (short press) or charged shot (long press) |
| RESTART | Restart the game |
| LEVEL UP | Skip to the next level |
| GAME TYPE | Cycle LED strip length |
| ❄ FREEZE | Activate enemy freeze (enabled when available; glows blue) |
| ∞ SURVIVAL | Start survival endless mode |

---

## How to Build and Upload

### Step 1 — Install Required Software

You need two things installed on your PC:

**1. Visual Studio Code (VS Code)**
Download from: https://code.visualstudio.com/ — install with default settings.

**2. PlatformIO IDE extension for VS Code**

1. Open VS Code.
2. Click the **Extensions** icon in the left sidebar (four squares), or press `Ctrl+Shift+X`.
3. Search for **PlatformIO IDE**.
4. Click **Install** on the result published by *PlatformIO*.
5. Wait for it to finish — it downloads the PlatformIO core tools automatically (may take several minutes the first time).
6. Restart VS Code when prompted.

> You do **not** need to install the Arduino IDE or any ESP32 board packages manually. PlatformIO handles all of that via `platformio.ini`.

---

### Step 2 — Open the Project

1. In VS Code choose **File → Open Folder**.
2. Navigate to and select the `LedBlaster.Optimized` folder.
3. VS Code detects `platformio.ini` and loads the project. The first time it opens it may download ESP32 toolchains — let it finish.
4. You should see the PlatformIO sidebar icon (alien head) appear on the left.

---

### Step 3 — Build the Firmware (compile only)

This step checks that the code compiles without errors before touching the ESP32.

| Method | How |
|--------|-----|
| Toolbar | Click the **✓ tick** icon in the blue status bar at the bottom of VS Code |
| Sidebar | PlatformIO icon → **PROJECT TASKS → esp32dev → General → Build** |
| Terminal | `pio run` |

A successful build ends with: `====== [SUCCESS] ======`

---

### Step 4 — Upload the Firmware to the ESP32

1. Connect the ESP32 to your PC via USB.
2. Make sure the **Serial Monitor is closed** — the upload fails if the COM port is busy.
3. Upload:

| Method | How |
|--------|-----|
| Toolbar | Click the **→ arrow** icon in the blue status bar |
| Sidebar | **esp32dev → General → Upload** |
| Terminal | `pio run --target upload` |

4. The upload shows a progress percentage, then `====== [SUCCESS] ======`.
5. The ESP32 resets automatically and runs the new firmware.

**Troubleshooting:**
- **Wrong COM port** — check Device Manager for the port and add `upload_port = COM3` (your port) under `[env:esp32dev]` in `platformio.ini`.
- **"Could not open port"** — close the Serial Monitor or any other program using the port.
- **Some boards** require holding the **BOOT** button when the upload starts; release once the dots appear.

---

### Step 5 — Build the Filesystem Image (SPIFFS)

The web UI (`index.html`) and all audio (`.wav`) files are stored in SPIFFS flash and are **not** included in the firmware upload.

Files packaged into the SPIFFS image (everything in the `data/` folder):
```
data/index.html     ← the web UI
data/*.wav          ← all sound effect files
```

1. Make sure the **Serial Monitor is closed**.
2. Build the image:

| Method | How |
|--------|-----|
| Sidebar | **esp32dev → Platform → Build Filesystem Image** |
| Terminal | `pio run --target buildfs` |

---

### Step 6 — Upload the Filesystem Image to the ESP32

1. ESP32 must be connected via USB.
2. **Serial Monitor must be closed.**
3. Upload:

| Method | How |
|--------|-----|
| Sidebar | **esp32dev → Platform → Upload Filesystem Image** |
| Terminal | `pio run --target uploadfs` |

The ESP32 resets after upload. The web UI and sounds are now live on the device.

> **Important:** Repeat this step any time you edit `index.html` or replace a `.wav` file. Firmware uploads do **not** update the filesystem.

---

### Step 7 — Connect and Play

1. The ESP32 broadcasts its own Wi-Fi access point:
   - **SSID:** `LED_BLASTER`
   - **Password:** `password123`
2. Connect your phone, tablet, or laptop to that network.
3. Open a browser and go to `http://192.168.4.1`
4. The web UI loads and connects via WebSocket automatically.

> **Note:** The connected device will lose normal internet access while on the ESP32 access point. This is expected.

---

### Quick Reference

| Task | Sidebar Path | Terminal Command |
|------|-------------|-----------------|
| Compile only | General → Build | `pio run` |
| Upload firmware | General → Upload | `pio run --target upload` |
| Open Serial Monitor | General → Monitor | `pio device monitor` |
| Build SPIFFS image | Platform → Build Filesystem Image | `pio run --target buildfs` |
| Upload SPIFFS image | Platform → Upload Filesystem Image | `pio run --target uploadfs` |
| Erase all flash | Platform → Erase Flash | *(use sidebar)* |

---

## Hardware Description

### 1. Microcontroller — ESP32 DevKit V1

**What it is:**
A development board built around Espressif's ESP32 dual-core 32-bit microcontroller. Runs at up to 240 MHz, 520 KB SRAM, 4 MB Flash, built-in Wi-Fi (802.11 b/g/n) and Bluetooth.

**Why this board:**
- Dual-core allows the game loop (Core 1) and audio output (Core 0) to run simultaneously without interfering with each other.
- Built-in Wi-Fi hosts the game's own access point and web UI — no router needed.
- Enough GPIO pins for LEDs, buttons, I2S audio, and status indicators.
- Supported by PlatformIO with a mature Arduino-compatible framework.

**Key specs:**

| Spec | Value |
|------|-------|
| Operating voltage | 3.3 V logic |
| Input voltage | 5 V via USB or VIN pin |
| Flash | 4 MB (part used for SPIFFS — web UI + sounds) |
| GPIO current limit | 40 mA per pin, 1200 mA total |
| Wi-Fi | 802.11 b/g/n, access point mode |

**USB connection:**
The board uses a Micro-USB connector. The onboard USB-to-Serial chip (CP2102 or CH340 depending on board revision) provides the COM port. If Windows does not see the port, install the CP2102 or CH340 driver from the chip manufacturer's website.

**GPIO pinout used in this project:**

| GPIO | Function |
|------|----------|
| 5 | WS2812B LED strip data |
| 12 | Level Up button |
| 13 | Restart button |
| 14 | Game Type button |
| 18 | Red shoot button |
| 19 | Green shoot button |
| 21 | Blue shoot button |
| 22 | I2S DIN (audio data to amplifier) |
| 25 | I2S LRC (left/right word select clock) |
| 26 | I2S BCLK (bit clock) |
| 27 | Blue status LED (PWM) |
| 32 | Green status LED (PWM) |
| 33 | Red status LED (PWM) |

---

### 2. LED Strip — WS2812B (120–300 LEDs)

**What it is:**
A strip of individually addressable RGB LEDs. Each LED contains a tiny controller chip that receives colour data over a single serial data line and passes the remaining data to the next LED in the chain.

**Why this component:**
- Single data wire makes wiring simple.
- Full 24-bit colour (8 bits each R, G, B) per LED.
- FastLED library provides high-level control and proven timing on ESP32.

**Electrical requirements:**
- Each LED draws up to 60 mA at full white brightness.
- 120 LEDs at full brightness: up to 7.2 A. In practice game colours are never full white simultaneously, so average draw is 0.5–2 A for game use.
- Strip is powered at 5 V. The ESP32 outputs 3.3 V logic — a **330 Ω resistor in series on the data line** is strongly recommended to prevent ringing and glitches.
- **Critical:** Connect the LED strip GND to the same GND as the ESP32.

**Wiring:**
```
LED Strip DIN  →  330 Ω resistor  →  ESP32 GPIO 5
LED Strip 5V   →  External 5V power supply (+)
LED Strip GND  →  External 5V power supply (−)  AND  ESP32 GND
```

**Power supply sizing:**

| Strip Length | Minimum Supply |
|-------------|---------------|
| 120 LEDs | 3 A / 5 V (15 W) |
| 180 LEDs | 4 A / 5 V (20 W) |
| 240 LEDs | 5 A / 5 V (25 W) |
| 300 LEDs | 6 A / 5 V (30 W) |

> **Do not** power the strip from the ESP32's 5V/VIN pin — it cannot supply enough current.

---

### 3. Shoot Buttons — Red, Green, Blue

Standard momentary tactile push buttons (normally open), one per game colour.

**Wiring:**
```
One leg   →  ESP32 GPIO (18 = Red, 19 = Green, 21 = Blue)
Other leg →  GND
```

**Pull-ups:** Internal pull-up resistors are enabled in software — no external resistors needed. Pin reads HIGH when not pressed; LOW when pressed.

**Debouncing:** Handled with the ezButton library (50 ms debounce window).

**Short vs. long press:** Short press (< 300 ms) = normal shot. Long press (≥ 300 ms) = charged power shot.

---

### 4. Menu Buttons — Restart, Level Up, Game Type

Same button type as shoot buttons.

| Button | GPIO | Behaviour |
|--------|------|-----------|
| Restart | 13 | Short press = restart game. Hold 2 s = start Survival Mode. |
| Level Up | 12 | Short press = skip to next level. |
| Game Type | 14 | Short press = cycle LED count 120 → 180 → 240 → 300. |

**Wiring (all identical):**
```
One leg   →  ESP32 GPIO (13 / 12 / 14)
Other leg →  GND
```
Internal pull-ups enabled — no external resistors needed.

---

### 5. Audio Amplifier — MAX98357A I2S DAC + Amplifier

**What it is:**
A breakout module containing the Maxim MAX98357A chip. Receives digital audio over I2S and drives a small speaker directly — up to 3.2 W into a 4 Ω speaker.

**Why I2S:**
I2S transfers audio as serial data on three dedicated lines. No analogue signal on any GPIO means no microcontroller noise or built-in DAC noise.

**Wiring:**

| MAX98357A Pin | Connects to | Note |
|--------------|-------------|------|
| VIN / VCC | Ext. 5V (+) | Same supply as LED strip |
| GND | GND | Shared ground |
| LRC | GPIO 25 | Word clock (left/right select) |
| BCLK | GPIO 26 | Bit clock |
| DIN | GPIO 22 | Audio data |
| GAIN | GND | 12 dB gain — loudest setting |

**GAIN pin options:**

| GAIN connection | Gain |
|----------------|------|
| Not connected | 9 dB (default) |
| GND | 12 dB (used in this project) |
| VCC | 6 dB |

**Speaker:** Connect a 4 Ω or 8 Ω speaker (0.5–3 W, 2–3 inch diameter) to the output terminals. Do not short the output terminals — the chip has no short-circuit protection.

**Audio format:** WAV, 22050 Hz, Mono, 16-bit signed PCM. Files stored in SPIFFS and played via arduino-audio-tools on Core 0 (dedicated FreeRTOS audio task).

---

### 6. Status LEDs — Red, Green, Blue

Three individual through-hole LEDs (3 mm or 5 mm) driven by PWM via the ESP32's LEDC peripheral.

**What each indicates:**

| Colour | GPIO | Indicates |
|--------|------|-----------|
| Red | 33 | Remaining lives — bright = 3 lives, dim = 1 life |
| Green | 32 | Current combo multiplier — brighter as combo grows |
| Blue | 27 | Power-up / charge availability |

**Wiring:**
```
ESP32 GPIO pin  →  Anode (+, longer leg)
Cathode (−, shorter leg)  →  330 Ω resistor  →  GND
```

**Resistor selection:**
At 3.3 V with a ~2.0 V LED forward voltage and ~8 mA target current:  
R = (3.3 − 2.0) / 0.008 ≈ 163 Ω minimum.  
**220 Ω** (~6 mA) or **330 Ω** (~4 mA) both work well.

**PWM control:** LEDC peripheral, 5000 Hz, 8-bit resolution (0 = off, 255 = full brightness).

---

### 7. Power Supply Summary

**5 V rail (external supply):**
- Powers the WS2812B LED strip and MAX98357A module.
- Requirement: 3–6 A depending on strip length.
- Recommended: regulated 5 V / 6 A DC supply or USB-C PD at 5 V / 5 A+.

**3.3 V rail (onboard ESP32 regulator):**
- Powers ESP32 logic, buttons (via pull-ups), and status LEDs.
- Supplied by the ESP32's onboard LDO from the 5 V USB/VIN input.
- Maximum ~600 mA — more than adequate for GPIO logic and status LEDs.

> **Critical:** The GND of the LED strip, MAX98357A, status LEDs, all buttons, and the ESP32 must all share a **common GND bus**.

**Tip:** Run a thick (≥ 22 AWG) wire from the external 5 V supply negative terminal to the ESP32 GND pin for a solid common ground reference.

---

### 8. Complete Bill of Materials

| Qty | Component | Notes |
|-----|-----------|-------|
| 1 | ESP32 DevKit V1 | 38-pin version recommended |
| 1 | WS2812B LED strip | 120 LEDs minimum; IP30 (bare) or IP65 (waterproof) |
| 1 | External 5 V DC supply | 6 A rating covers all strip lengths |
| 1 | MAX98357A I2S amplifier module | Adafruit or compatible breakout |
| 1 | Small speaker | 4 Ω or 8 Ω, 0.5–3 W, 2–3 inch diameter |
| 3 | Momentary push buttons | Shoot buttons (Red / Green / Blue) |
| 3 | Momentary push buttons | Menu buttons (Restart / Level Up / Game Type) |
| 3 | Individual LEDs (3 mm or 5 mm) | Red, Green, Blue status indicators |
| 3 | 330 Ω resistors | Status LED current limiting |
| 1 | 330 Ω resistor | WS2812B data line protection |
| 1 | 100–1000 µF capacitor | Across 5 V rail near LED strip connector (prevents power-on surge from damaging the first LED) |
| — | Jumper wires / proto board | For connections |
| — | Micro-USB cable | ESP32 programming and fallback power |
