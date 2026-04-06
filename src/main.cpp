#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ezButton.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// --- AUDIO LIBRARY ---
#include "AudioTools.h"

// Forward declarations for audio task
void audioTask(void *parameter);
TaskHandle_t audioTaskHandle = NULL;
QueueHandle_t soundQueue = NULL;
uint32_t soundQueueDrops = 0;
uint32_t soundQueueEnqueued = 0;

#define SOUND_QUEUE_LEN 16
#define AUDIO_TASK_STACK_SIZE 4096
#define AUDIO_TASK_PRIORITY 1
#define AUDIO_TASK_CORE 1

// --- AP CONFIGURATION ---
const char *ssid = "LED_BLASTER";
const char *password = "password123";

// --- HARDWARE PINS ---
#define LED_PIN 5
#define NUM_LEDS 300   // Maximum strip capacity (array size)
#define BRIGHTNESS 80
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

// Physical Game Buttons
#define BTN_PIN_R 18
#define BTN_PIN_G 19
#define BTN_PIN_B 21
#define BTN_RESTART 13
#define BTN_LEVELUP 12
#define BTN_GAMETYPE 14  // Cycles active LED count: 120→180→240→300→120

// Physical Status LEDs
#define PIN_STAT_R 33
#define PIN_STAT_G 32
#define PIN_STAT_B 27

// Audio I2S Pins
#define I2S_LRC 25
#define I2S_BCLK 26
#define I2S_DIN 22

// Audio assets in /data are WAV PCM 22.05kHz, mono, 16-bit.
#define AUDIO_SAMPLE_RATE 22050

// --- GAME SETTINGS ---
#define MAX_ENEMIES 100
#define MAX_SHOTS 20
#define SHOT_SPEED 20        // Shot move interval (ms) at 120 LEDs; scales down to 10ms at 300 LEDs
#define BASE_ENEMY_SPEED 1000
#define SPEED_INC_PER_LVL 30
#define SCORE_PER_KILL 5
#define SCORE_PER_LEVEL 50
#define POWERUP_CHANCE 5
#define SUPER_SHOT_MIN_KILLS 2
#define SUPER_SHOT_MAX_KILLS 5
#define BOSS_HITS 3          // Hits required to destroy a boss enemy
#define STARTING_LIVES 3
#define DANGER_ZONE_DIST 10          // Trigger alarm when enemy is within this distance of pos 0
#define SCORE_MILESTONE_INTERVAL 250 // Points between guaranteed power-up drops
#define ARMOR_START_LEVEL 15         // Level at which armored enemies appear
#define ARMOR_CHANCE 10              // Percent chance an enemy spawns armored (level 15+)
#define COUNTDOWN_MS 3000UL          // SHOWING_LEVEL auto-start countdown
#define LEVEL_TIME_BASE_MS 45000UL  // Base time limit per level (ms)
#define LEVEL_TIME_MIN_MS  15000UL  // Minimum time limit at high levels (ms)
#define LEVEL_TIME_BONUS_MAX 100    // Max bonus points awarded for a fast clear
#define LONG_PRESS_MS 300
#define MAX_GAME_LEVEL 34
#define NUM_STARTING_ENEMIES 10
#define GOLD_ENEMY_CHANCE 5            // % chance normal enemy becomes a gold two-hit enemy
#define GOLD_ENEMY_TIMEOUT_MS 2000UL   // Time window for landing the second hit on a gold enemy
#define BOMB_ENEMY_CHANCE 4            // % chance (level 5+) enemy becomes a bomb
#define BOMB_EXPLOSION_RADIUS 5        // Pixels cleared on each side of bomb on detonation
#define FREEZE_DURATION_MS 3000UL      // Duration all enemies are frozen (all-3-button press)
#define REINFORCE_THRESHOLD 40         // % enemies remaining that triggers a reinforcement wave
#define REINFORCE_COUNT 5              // Enemies in a reinforcement wave
#define SPEED_RAMP_INTERVAL_MS 10000UL // How often enemy speed increases within a round (ms)
#define SURVIVAL_SPAWN_INTERVAL 2500UL // Survival mode: new enemy spawn interval (ms)
#define SURVIVAL_HOLD_MS 2000UL        // Hold RESTART this long on SHOWING_LEVEL to enter survival

// --- BUTTON OBJECTS & TIMING ---
ezButton btnR(BTN_PIN_R);
ezButton btnG(BTN_PIN_G);
ezButton btnB(BTN_PIN_B);
ezButton btnRestart(BTN_RESTART);
ezButton btnLevelUp(BTN_LEVELUP);
ezButton btnGameType(BTN_GAMETYPE);

unsigned long pressTimeR = 0;
unsigned long pressTimeG = 0;
unsigned long pressTimeB = 0;

bool handledR = true;
bool handledG = true;
bool handledB = true;

bool chargingR = false;
bool chargingG = false;
bool chargingB = false;

unsigned long pressTimeRestart = 0;
bool handledRestart = true;

bool updateWsLeds = false; // Flag to indicate when to send LED state updates to clients
bool updateWsInfo = false;  // Flag to indicate when to send game info updates to clients (score, level, multiplier, etc)

// --- CUSTOM MIXER CLASS ---
//
// WavMixer — the game's sound engine, all in one tidy class.
//
// A real arcade machine has lots of sounds playing at once — the pew of a laser,
// the crunch of a hit, a power-up jingle — all at the same time.  Mixing means adding
// several audio streams together so you hear them all through one speaker.
//
// Each "channel" is just a slot holding a pointer to a block of raw audio samples
// (the numbers that describe the sound wave).  The readBytes() method is called many
// times a second by the audio driver; it sums every active channel into one output
// buffer, clipping the result so it never exceeds the 16-bit integer limit and causes
// ugly distortion.
//
// To avoid reading the same file from flash storage every time a sound plays, the four
// most-used sounds are *preloaded* into RAM at startup (cached).  The less-frequent
// win/win_game sounds are loaded on demand and freed when finished.
//
// Interesting reading:
//   Digital audio fundamentals — https://www.soundonsound.com/techniques/digital-audio-basics
//   How WAV files work       — https://docs.fileformat.com/audio/wav/
//   I2S (the wire protocol)  — https://www.electronicshub.org/i2s-protocol/
#define MAX_AUDIO_CHANNELS 4
class WavMixer : public AudioStream 
{
    static const int CACHED_SOUND_COUNT = 4;

    struct CachedSound
    {
        const char *name;
        int16_t *samples;
        size_t sampleCount;
        bool loaded;
    };

    struct Channel 
    {
        const int16_t *samples;
        size_t sampleCount;
        size_t position;
        bool active;
        bool ownsBuffer;
    };

    CachedSound cache[CACHED_SOUND_COUNT];
    Channel channels[MAX_AUDIO_CHANNELS];

public:
    WavMixer() 
    {
        for (int i = 0; i < CACHED_SOUND_COUNT; i++)
        {
            cache[i].name = nullptr;
            cache[i].samples = nullptr;
            cache[i].sampleCount = 0;
            cache[i].loaded = false;
        }

        // Initialize all audio channels to inactive
        for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) 
        {
            channels[i].samples = nullptr;
            channels[i].sampleCount = 0;
            channels[i].position = 0;
            channels[i].active = false;
            channels[i].ownsBuffer = false;
        }
    }

    ~WavMixer()
    {
        releaseCache();
    }

    // releaseCache() — free all sounds that were loaded into RAM.
    // Called before a fresh preload so we don't keep paying for memory we no longer need.
    void releaseCache()
    {
        for (int i = 0; i < CACHED_SOUND_COUNT; i++)
        {
            if (cache[i].samples != nullptr)
            {
                free(cache[i].samples);
                cache[i].samples = nullptr;
            }
            cache[i].sampleCount = 0;
            cache[i].loaded = false;
        }
    }

    // loadWavToBuffer() — read a WAV file from flash into a freshly allocated block of RAM.
    //
    // WAV files start with a 44-byte header describing sample rate, bit depth, etc.
    // Everything after byte 44 is raw PCM audio — just a big list of 16-bit signed integers,
    // one per sample, at 22 050 samples per second (CD quality is 44 100, so this is half-speed
    // to save space on the tiny 4 MB SPIFFS flash).
    //
    // Returns true and fills outSamples/outSampleCount on success; returns false on any error
    // so callers know not to try playing a nullptr.
    bool loadWavToBuffer(const char *path, int16_t *&outSamples, size_t &outSampleCount)
    {
        outSamples = nullptr;
        outSampleCount = 0;

        if (!SPIFFS.exists(path))
        {
            return false;
        }

        File f = SPIFFS.open(path, "r");
        if (!f)
        {
            return false;
        }

        size_t size = f.size();
        if (size <= 44)
        {
            f.close();
            return false;
        }

        size_t dataBytes = size - 44;
        if ((dataBytes & 1U) != 0)
        {
            dataBytes -= 1;
        }
        size_t sampleCount = dataBytes / 2;
        if (sampleCount == 0)
        {
            f.close();
            return false;
        }

        int16_t *samples = (int16_t *)malloc(sampleCount * sizeof(int16_t));
        if (samples == nullptr)
        {
            f.close();
            return false;
        }

        f.seek(44);
        size_t bytesRead = f.read((uint8_t *)samples, sampleCount * sizeof(int16_t));
        f.close();

        size_t actualSamples = bytesRead / sizeof(int16_t);
        if (actualSamples == 0)
        {
            free(samples);
            return false;
        }

        outSamples = samples;
        outSampleCount = actualSamples;
        return true;
    }

    // preloadAll() — load the four most-used sounds into RAM at startup.
    //
    // Reading a file from SPI flash while the game is already running would cause
    // glitches — the CPU has to stop and wait.  Loading everything up-front into
    // regular RAM means play() never touches the filesystem mid-game, so audio is
    // always glitch-free.  Think of it like buffering a YouTube video before pressing play.
    void preloadAll()
    {
        releaseCache();

        const char *assets[CACHED_SOUND_COUNT] = {
            "/shoot.wav",
            "/hit.wav",
            "/power.wav",
            "/fail.wav"
        };

        for (int i = 0; i < CACHED_SOUND_COUNT; i++)
        {
            cache[i].name = assets[i];

            int16_t *samples = nullptr;
            size_t actualSamples = 0;
            if (!loadWavToBuffer(assets[i], samples, actualSamples))
            {
                Serial.printf("[AUDIO] Cache skip: %s\n", assets[i]);
                continue;
            }

            cache[i].samples = samples;
            cache[i].sampleCount = actualSamples;
            cache[i].loaded = true;
            Serial.printf("[AUDIO] Cached %s (%u samples)\n", assets[i], (unsigned)actualSamples);
        }
    }

    size_t getCachedBytes() const
    {
        size_t total = 0;
        for (int i = 0; i < CACHED_SOUND_COUNT; i++)
        {
            if (cache[i].loaded)
            {
                total += cache[i].sampleCount * sizeof(int16_t);
            }
        }
        return total;
    }

    int getCachedCount() const
    {
        int count = 0;
        for (int i = 0; i < CACHED_SOUND_COUNT; i++)
        {
            if (cache[i].loaded)
            {
                count++;
            }
        }
        return count;
    }

    int getActiveChannels() const
    {
        int count = 0;
        for (int i = 0; i < MAX_AUDIO_CHANNELS; i++)
        {
            if (channels[i].active)
            {
                count++;
            }
        }
        return count;
    }

    const CachedSound *findCached(const char *filename) const
    {
        for (int i = 0; i < CACHED_SOUND_COUNT; i++)
        {
            if (cache[i].loaded && cache[i].name != nullptr && strcmp(cache[i].name, filename) == 0)
            {
                return &cache[i];
            }
        }
        return nullptr;
    }

    // readBytes() — the heart of the mixer; called automatically by the audio driver.
    //
    // Every ~23 ms the I2S driver asks for a fresh buffer of audio data.  This method
    // loops through every active channel, reading its samples and *adding* them to the
    // output buffer.  Adding is how mixing works — the waves combine mathematically
    // just like two ripples on a pond adding together.
    //
    // The int32_t intermediate prevents overflow: two 16-bit values added together can
    // exceed 16 bits, so we clamp (clip) the result back into the valid range before
    // writing it out.  Without this you'd hear ugly crackling distortion on overlapping
    // sounds.
    //
    // Interesting reading:
    //   How digital mixing works — https://www.izotope.com/en/learn/digital-audio-basics.html
    size_t readBytes(uint8_t *data, size_t len) override 
    {
        // Clear out the buffer before mixing
        memset(data, 0, len);
        int16_t *outBuffer = (int16_t *)data;
        size_t samplesToWrite = len / 2;

        for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) 
        {
            if (channels[i].active) 
            {
                size_t remaining = channels[i].sampleCount - channels[i].position;
                size_t samplesRead = remaining < samplesToWrite ? remaining : samplesToWrite;
                const int16_t *src = channels[i].samples + channels[i].position;

                // Additive mixing for concurrent sounds
                for (size_t k = 0; k < samplesRead; k++) 
                {
                    int32_t mixed = outBuffer[k] + src[k];
                    
                    // Prevent audio clipping (distortion)
                    if (mixed > 32767) 
                    {
                        mixed = 32767;
                    }
                    if (mixed < -32768) 
                    {
                        mixed = -32768;
                    }
                    outBuffer[k] = (int16_t)mixed;
                }

                channels[i].position += samplesRead;

                // If the file is done, close it
                if (channels[i].position >= channels[i].sampleCount)
                {
                    if (channels[i].ownsBuffer && channels[i].samples != nullptr)
                    {
                        free((void *)channels[i].samples);
                    }
                    channels[i].samples = nullptr;
                    channels[i].sampleCount = 0;
                    channels[i].position = 0;
                    channels[i].active = false;
                    channels[i].ownsBuffer = false;
                }
            }
        }
        return len;
    }

    // play() — start playing a sound effect.
    //
    // Checks the cache first; if found, points a free channel at those samples and sets
    // it active.  If ALL channels are busy (four sounds already playing) the new sound is
    // silently dropped — better to miss an effect than to stomp on another channel's data.
    //
    // The win/win_game clips are loaded on demand because they're too long and infrequent
    // to justify keeping in RAM the whole time.
    void play(const char *filename) 
    {
        const CachedSound *sound = findCached(filename);
        int16_t *ownedSamples = nullptr;
        size_t ownedCount = 0;
        bool ownsBuffer = false;

        if (sound == nullptr)
        {
            if (strcmp(filename, "/win.wav") == 0 || strcmp(filename, "/win_game.wav") == 0)
            {
                if (!loadWavToBuffer(filename, ownedSamples, ownedCount))
                {
                    return;
                }
                ownsBuffer = true;
            }
            else
            {
                return;
            }
        }

        // Find the first available open channel
        for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) 
        {
            if (!channels[i].active) 
            {
                if (sound != nullptr)
                {
                    channels[i].samples = sound->samples;
                    channels[i].sampleCount = sound->sampleCount;
                    channels[i].ownsBuffer = false;
                }
                else
                {
                    channels[i].samples = ownedSamples;
                    channels[i].sampleCount = ownedCount;
                    channels[i].ownsBuffer = ownsBuffer;
                }
                channels[i].position = 0;
                channels[i].active = true;
                return;
            }
        }

        if (ownsBuffer && ownedSamples != nullptr)
        {
            free(ownedSamples);
        }
    }
    
    size_t write(const uint8_t *data, size_t len) override 
    { 
        return 0; 
    }
    
    // available() — tell the audio driver how many bytes of data are ready to be read.
    //
    // StreamCopy (from the AudioTools library) calls available() before calling readBytes().
    // If we returned 0 it would stop pumping the I2S stream.  Returning the largest
    // remaining channel size keeps the stream flowing until all sounds have finished.
    int available() override 
    { 
        // StreamCopy relies on available() to decide if it should read.
        // Report pending bytes whenever at least one channel is active.
        int maxRemaining = 0;
        for (int i = 0; i < MAX_AUDIO_CHANNELS; i++)
        {
            if (channels[i].active)
            {
                int remaining = (int)(channels[i].sampleCount - channels[i].position);
                if (remaining > maxRemaining)
                {
                    maxRemaining = remaining;
                }
            }
        }
        return maxRemaining * (int)sizeof(int16_t);
    }
};

// --- AUDIO & WEB OBJECTS ---
I2SStream i2s;
WavMixer mixer;
StreamCopy copier;
CRGB leds[NUM_LEDS];
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// --- STATE DATA ---
struct Enemy 
{
    bool active;
    int position;
    CRGB color;
    bool isPowerUp;
    bool isBoss;      // Boss enemies require BOSS_HITS hits to destroy
    int hitsLeft;     // Remaining hits before death (1 for normal enemies)
    bool isArmored;    // Any-color hit strips armor; then matching hit kills
    bool isGold;               // Needs 2 different-color hits; worth 5× points; wrong 2nd spawns 3 enemies
    bool isBomb;               // Any hit explodes ±BOMB_EXPLOSION_RADIUS; reaching pos 0 = instant game over
    CRGB goldHit1Color;        // Color of first gold hit (CRGB::Black = not yet hit)
    unsigned long goldHit1Time; // Timestamp of first gold hit
};

struct Projectile 
{
    bool active;
    int position;
    CRGB color;
    bool isSuper;
    int hitsLeft;
};

struct FlashFX 
{
    bool active;
    int position;
    unsigned long startTime;
    CRGB color;
};

struct InputQueue 
{
    bool redShort; 
    bool redLong;
    bool grnShort; 
    bool grnLong;
    bool bluShort; 
    bool bluLong;
    bool restart;  
    bool levelUp;
    bool gameType;
    bool freeze;         // All-3-button press: freeze enemies for FREEZE_DURATION_MS
    bool startSurvival;  // Long-hold RESTART from SHOWING_LEVEL: enter survival endless mode
} inputs;

Enemy enemies[MAX_ENEMIES];
Projectile shots[MAX_SHOTS];
FlashFX hitEffect;

// Active LED count — cycles between 120, 180, 240, 300 via BTN_GAMETYPE
const int LED_COUNTS[] = {120, 180, 240, 300};
int ledCountIndex = 0;
int activeLeds = LED_COUNTS[0];

int currentLevel = 1;
int score = 0;
int comboMultiplier = 1;
int enemiesToSpawn = 10;
unsigned long enemyMoveDelay = BASE_ENEMY_SPEED;

bool hasPowerUpR = false;
bool hasPowerUpG = false;
bool hasPowerUpB = false;

unsigned long lastEnemyMove = 0;
unsigned long lastShotMove = 0;
unsigned long lastDraw = 0;
const unsigned long FRAME_DELAY = 33;
unsigned long lastShowingLevelFx = 0;
unsigned long lastShowingLevelSound = 0;
unsigned long nextShowingLevelSoundDelay = 3000;
uint8_t showingLevelHue = 0;
unsigned long levelClearedTime = 0;
unsigned long levelStartTime = 0;         // Timestamp when current round began
unsigned long levelTimeLimit = LEVEL_TIME_BASE_MS; // Time limit for this round
CRGB levelClearColor = CRGB::White;       // Color of last enemy killed (celebration)
int highScore = 0;                        // All-time high score (persisted to SPIFFS)
int lives = STARTING_LIVES;
int totalShots = 0;
int totalHits = 0;
int maxCombo = 0;
unsigned long showingLevelEnterTime = 0;

bool freezeActive = false;
unsigned long freezeEndTime = 0;
bool freezeUsedThisLevel = false;
int initialEnemyCount = 0;
bool reinforcementSent = false;
unsigned long lastSpeedRampTime = 0;
bool survivalMode = false;
int highScoreSurvival = 0;
unsigned long lastSurvivalSpawn = 0;

// Websocket update rate limiting
unsigned long lastWsUpdate = 0;
const unsigned long WS_UPDATE_INTERVAL = 50;

enum State 
{ 
    SHOWING_LEVEL, 
    PLAYING, 
    LEVEL_CLEARED,
    GAME_OVER,
	GAME_WON
};
State gameState = SHOWING_LEVEL;

// --- PROTOTYPES ---
void setupWiFi(); 
void setupWebRoutes(); 
void createWebPageIfMissing();
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void notifyClients(); 
void sendLedState(); 
void processGameLogic();
void resetLevel(bool fullReset); 
void showLevelIndicator(); 
void readInputs();
void clearInputs(); 
void updateStatusLeds(); 
void triggerShoot(CRGB color, bool isSuper);
void gameLoop(); 
void startGameRound(); 
void playSound(const char *filename);
bool isPositionOccupied(int pos);
void addEnemyAtFront(int pos, CRGB col); 
void drawGame();
int countActiveEnemies(); 
int findFrontEnemyIndex();
void handleCollision(int shotIdx, int enemyIdx);
void checkGlobalCollisions(); 
CRGB getRandomColor();
void logAudioAssetStatus();
void handleSerialDebugCommands();
void updateShowingLevelState();
void loadHighScore();
void saveHighScore();
void checkHighScore();
void loseLife();
void loadSurvivalHighScore();
void saveSurvivalHighScore();

// safeFastLEDShow() — send the current LED colour data to the physical strip.
//
// FastLED.show() translates the leds[] array into a precisely-timed stream of pulses
// on the data wire.  WS2812B LEDs use a 1-wire protocol where a "1" bit and a "0" bit
// are distinguished purely by pulse width — impressive that the chip decodes it!
//
// On some microcontrollers you must disable interrupts while sending to avoid timing
// glitches, but the ESP32's RMT (Remote Control) peripheral handles timing in hardware,
// so we can just call FastLED.show() directly.
//
// Interesting reading:
//   WS2812B datasheet & protocol — https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf
//   FastLED library home          — https://fastled.io
// Interrupt-safe FastLED.show() wrapper
inline void safeFastLEDShow() {
    // On ESP32/RMT, disabling global interrupts during show can cause worse timing artifacts.
    FastLED.show();
}

// ================= SETUP =================
//
// setup() — runs exactly once when the ESP32 powers on or resets.
//
// Think of this as the game cartridge loading: every system must be initialised in the
// right order before the game loop can start.
//
//  1. Serial  — opens the USB debug console (like console.log in JavaScript).
//  2. SPIFFS  — mounts the tiny filesystem stored in flash memory.  This is where the
//               web page files, audio WAVs, and high-score JSON live.
//               SPIFFS stands for Serial Peripheral Interface Flash File System.
//  3. Status LEDs — three small SMD LEDs driven by PWM (Pulse Width Modulation);
//               brightness is set by varying the duty cycle of a fast square wave.
//  4. I2S audio — sets up the Inter-IC Sound bus that talks to the DAC/amplifier board.
//  5. Sound queue — a FreeRTOS message queue so the game loop can request sounds without
//               blocking; the audio task drains it on a different CPU core.
//  6. FastLED — attaches to the LED data pin and records how many LEDs to drive.
//  7. Wi-Fi AP — creates a hotspot so phones can connect and use the web UI.
//  8. Web server — registers URL routes and starts listening on port 80.
//  9. Audio task — spawns the audio processing task on CPU core 0 (the game runs on core 1).
// 10. resetLevel  — puts the game into its initial state.
//
// Interesting reading:
//   ESP32 dual-core architecture — https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/freertos-smp.html
//   FreeRTOS queues             — https://www.freertos.org/Embedded-RTOS-Queues.html
//   PWM & duty cycle            — https://learn.sparkfun.com/tutorials/pulse-width-modulation
void setup() 
{
    Serial.begin(115200);
    Serial.printf("[BOOT] Free heap at start: %u\n", (unsigned)ESP.getFreeHeap());
    bool spiffsMounted = false;

    // Initialize SPIFFS for web server files and audio
    if (!SPIFFS.begin(false, "/spiffs", 10)) 
    {
        Serial.println("SPIFFS Mount Failed!");
    }
    else
    {
        spiffsMounted = true;
        logAudioAssetStatus();
        loadHighScore();
        loadSurvivalHighScore();
    }

    Serial.printf("[BOOT] Free heap after SPIFFS: %u\n", (unsigned)ESP.getFreeHeap());

    // Setup physical status LEDs as PWM outputs for charge fade and combo blink
    ledcAttach(PIN_STAT_R, 5000, 8);
    ledcAttach(PIN_STAT_G, 5000, 8);
    ledcAttach(PIN_STAT_B, 5000, 8);

    // Configure I2S audio output
    AudioLogger::instance().begin(Serial, AudioLogger::Error);
    auto config = i2s.defaultConfig(TX_MODE);
    config.pin_bck = I2S_BCLK;
    config.pin_ws = I2S_LRC;
    config.pin_data = I2S_DIN;
    config.sample_rate = AUDIO_SAMPLE_RATE;
    config.bits_per_sample = 16;
    config.channels = 1;      
    config.buffer_size = 1024;
    config.buffer_count = 4;
    i2s.begin(config);
    copier.begin(i2s, mixer);

    // Route all sound triggers through a queue so mixer state is only touched on the audio task.
    soundQueue = xQueueCreate(SOUND_QUEUE_LEN, sizeof(const char *));

    // Initialize the LED strip
    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear();

    // Setup ezButton Debouncing times
    btnR.setDebounceTime(30);
    btnG.setDebounceTime(30);
    btnB.setDebounceTime(30);
    btnRestart.setDebounceTime(30);
    btnLevelUp.setDebounceTime(30);
    btnGameType.setDebounceTime(30);

    // Boot up networking
    setupWiFi();
    setupWebRoutes();
    Serial.printf("[BOOT] Free heap after web start: %u\n", (unsigned)ESP.getFreeHeap());

    // Start audio processing task after networking so AsyncTCP can create its internal task first.
    BaseType_t audioTaskOk = xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        AUDIO_TASK_STACK_SIZE,
        NULL,
        AUDIO_TASK_PRIORITY,
        &audioTaskHandle,
        AUDIO_TASK_CORE
    );
    if (audioTaskOk != pdPASS)
    {
        Serial.println("[AUDIO] Failed to create audio task");
    }

    // Defer large audio allocations until after AsyncTCP/WebServer task setup.
    if (spiffsMounted)
    {
        mixer.preloadAll();
        Serial.printf("[AUDIO] Cache ready: %d files, %u bytes\n", mixer.getCachedCount(), (unsigned)mixer.getCachedBytes());
        Serial.println("[AUDIO] Serial debug: send 'a' for mixer/queue stats");
    }
    Serial.printf("[BOOT] Free heap after audio preload: %u\n", (unsigned)ESP.getFreeHeap());
    
    // Start game in fresh state
    resetLevel(true);
	notifyClients();
}

// ================= AUDIO TASK (Core 0) =================
//
// audioTask() — the dedicated sound-processing task running on CPU core 0.
//
// The ESP32 is a *dual-core* processor — two independent CPUs sharing the same chip.
// Running audio on its own core means the game logic (on core 1) can never stall the
// sound, and the sound can never stall the rendering.  Both cores tick along at full
// speed simultaneously.
//
// Each iteration:
//   1. Drain every pending sound name out of the soundQueue and tell the WavMixer to start
//      playing it.  Using a queue is the thread-safe way for core 1 to pass data to core 0.
//   2. If any channel is still active, call copier.copy() to push audio samples to I2S.
//   3. vTaskDelay(1) yields for 1 ms so the FreeRTOS scheduler can run Wi-Fi housekeeping
//      tasks on the same core without starving.
//
// Interesting reading:
//   FreeRTOS tasks & scheduling — https://www.freertos.org/taskandcr.html
//   Producer-consumer queues   — https://en.wikipedia.org/wiki/Producer%E2%80%93consumer_problem
void audioTask(void *parameter) 
{
    const char *pendingSound = nullptr;

    while (true) 
    {
        // Drain all pending sound events and start them from the audio thread only.
        while (soundQueue != NULL && xQueueReceive(soundQueue, &pendingSound, 0) == pdTRUE)
        {
            mixer.play(pendingSound);
        }

        if (mixer.available() > 0)
        {
            copier.copy();  // Keep audio stream pumping while any channel is active
        }

        vTaskDelay(1);
    }
}

// ================= MAIN LOOP (Core 1) =================
//
// loop() — Arduino's main loop, called repeatedly forever on CPU core 1.
//
// Every iteration (roughly 30+ times per second) it:
//  1. Checks for serial commands typed in the debug console.
//  2. Tells the WebSocket server to drop disconnected clients (housekeeping).
//  3. Runs the game engine — the big processGameLogic() function.
//  4. Updates the three physical status LEDs on the hardware.
//
// The loop itself has no delay; instead, each sub-function uses timestamps to decide
// whether its own time slice has come.  This is called "cooperative non-blocking" design
// and is how most embedded games avoid the dangers of blocking calls.
//
// Interesting reading:
//   Arduino loop basics          — https://docs.arduino.cc/learn/programming/arduino-sketches
//   Non-blocking timing patterns — https://www.arduino.cc/en/Tutorial/BuiltInExamples/BlinkWithoutDelay
void loop() 
{
    handleSerialDebugCommands();
    ws.cleanupClients();       // Maintain clean websocket connections
    processGameLogic();        // Run the main game engine
    updateStatusLeds();        // Update external hardware LEDs
}

// handleSerialDebugCommands() — a tiny cheat-code console for debugging.
//
// While the game runs you can open a serial monitor (e.g. in VS Code or Arduino IDE) at
// 115 200 baud and type single characters.  Pressing 'a' dumps a snapshot of the audio
// engine's state: how many channels are active, how many sounds have been requested and
// played, how much RAM is used, and so on.  Invaluable for tracking down audio stutters
// or queue overflows without needing to attach a debugger.
void handleSerialDebugCommands()
{
    while (Serial.available() > 0)
    {
        int cmd = Serial.read();
        if (cmd == 'a' || cmd == 'A')
        {
            UBaseType_t qDepth = 0;
            UBaseType_t qFree = 0;
            if (soundQueue != NULL)
            {
                qDepth = uxQueueMessagesWaiting(soundQueue);
                qFree = uxQueueSpacesAvailable(soundQueue);
            }

            Serial.printf(
                "[AUDIO] active_ch=%d queued=%u free=%u enqueued=%u dropped=%u cached_files=%d cached_bytes=%u heap=%u\n",
                mixer.getActiveChannels(),
                (unsigned)qDepth,
                (unsigned)qFree,
                (unsigned)soundQueueEnqueued,
                (unsigned)soundQueueDrops,
                mixer.getCachedCount(),
                (unsigned)mixer.getCachedBytes(),
                (unsigned)ESP.getFreeHeap());
        }
    }
}

// updateStatusLeds() — drive the three small RGB hardware LEDs on the controller.
//
// Each LED has three jobs depending on what the player is doing:
//
//  CHARGING  — while a button is held, the LED fades from dim (20) to full (255) over
//              LONG_PRESS_MS milliseconds.  This gives the player visual feedback that
//              they're charging a super shot.  The brightness is calculated with map(),
//              which linearly maps one range of numbers onto another.
//
//  POWER-UP  — if a power-up of that colour is ready, the LED glows solid full brightness.
//
//  HIGH COMBO — if the multiplier is ≥ 2 and no power-up is held, the LED blinks at a
//              speed that increases with the combo tier — a subtle but satisfying reward.
//
// The ESP32's LEDC peripheral (LED Control) generates the PWM signal in hardware so the
// CPU does not have to bit-bang the pin.
//
// Interesting reading:
//   ESP32 LEDC PWM — https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/ledc.html
//   map() function  — https://www.arduino.cc/reference/en/language/functions/math/map/
void updateStatusLeds() 
{
    unsigned long currentMillis = millis();

    // Determine blink period from combo tier — blinks faster as multiplier grows
    int blinkMs = 0;
    if      (comboMultiplier >= 20) blinkMs = 80;
    else if (comboMultiplier >= 10) blinkMs = 150;
    else if (comboMultiplier >= 5)  blinkMs = 300;
    else if (comboMultiplier >= 2)  blinkMs = 600;
    bool blinkOn = (blinkMs > 0) && ((currentMillis / blinkMs) % 2 == 0);

    // Compute PWM duty for one status LED:
    //  - Charging: fade 20→255 over LONG_PRESS_MS (feature 11)
    //  - Powerup held: solid 255
    //  - High combo, no powerup: blink at tier rate (feature 8)
    //  - Otherwise: off
    auto computeDuty = [&](bool charging, unsigned long pressTime, bool hasPowerUp) -> uint8_t
    {
        if (charging)
        {
            unsigned long el = currentMillis - pressTime;
            if (el >= LONG_PRESS_MS) return 255;
            return (uint8_t)map(el, 0, LONG_PRESS_MS, 20, 255);
        }
        if (hasPowerUp) return 255;
        if (blinkMs > 0) return blinkOn ? 80 : 0;
        return 0;
    };

    ledcWrite(PIN_STAT_R, computeDuty(chargingR, pressTimeR, hasPowerUpR));
    ledcWrite(PIN_STAT_G, computeDuty(chargingG, pressTimeG, hasPowerUpG));
    ledcWrite(PIN_STAT_B, computeDuty(chargingB, pressTimeB, hasPowerUpB));
}

// playSound() — the game's one-liner for requesting a sound effect.
//
// Rather than calling mixer.play() directly (which would touch audio state from the
// game/network task on core 1 while the audio task on core 0 is reading it — a data
// race), this function places the filename pointer into a thread-safe FreeRTOS queue.
// The audio task picks it up and calls mixer.play() safely on its own core.
//
// xQueueSend(..., 0) is non-blocking (timeout=0).  If the queue is full the sound is
// counted as dropped and skipped; the game never stalls waiting for audio.
void playSound(const char *filename) 
{
    if (soundQueue != NULL)
    {
        const char *msg = filename;
        BaseType_t ok = xQueueSend(soundQueue, &msg, 0);

        if (ok == pdTRUE)
        {
            soundQueueEnqueued++;
        }
        else
        {
            soundQueueDrops++;
        }
    }
}

// logAudioAssetStatus() — scan SPIFFS and print a health-check for every audio file.
//
// Called once at boot so you can immediately see in the serial console if any WAV is
// missing or corrupt (zero-length).  A WAV file must be at least 45 bytes — 44 for the
// standard RIFF header plus at least 1 byte of audio data.
void logAudioAssetStatus()
{
    const char *assets[] = {
        "/shoot.wav",
        "/hit.wav",
        "/power.wav",
        "/win.wav",
        "/fail.wav",
        "/win_game.wav"
    };

    Serial.println("Checking SPIFFS audio assets...");
    for (size_t i = 0; i < (sizeof(assets) / sizeof(assets[0])); i++)
    {
        const char *path = assets[i];
        if (!SPIFFS.exists(path))
        {
            Serial.printf("[AUDIO] Missing: %s\n", path);
            continue;
        }

        File f = SPIFFS.open(path, "r");
        if (!f)
        {
            Serial.printf("[AUDIO] Open failed: %s\n", path);
            continue;
        }

        size_t size = f.size();
        f.close();
        if (size <= 44)
        {
            Serial.printf("[AUDIO] Invalid/empty WAV: %s (%u bytes)\n", path, (unsigned)size);
        }
        else
        {
            Serial.printf("[AUDIO] OK: %s (%u bytes)\n", path, (unsigned)size);
        }
    }
}

// loadHighScore() — read the all-time high score from a tiny JSON file in flash.
//
// Persistent storage on a microcontroller is tricky — there's no hard drive!
// This game uses SPIFFS to store a one-key JSON document: {"hs": 1234}.
// ArduinoJson parses it in a stack-allocated StaticJsonDocument (no heap needed).
// If the file doesn't exist yet (first boot) the high score simply starts at 0.
//
// Interesting reading:
//   ArduinoJson library — https://arduinojson.org/
//   Persistent storage on ESP32 — https://randomnerdtutorials.com/esp32-flash-memory/
void loadHighScore()
{
    if (!SPIFFS.exists("/highscore.json")) { highScore = 0; return; }
    File f = SPIFFS.open("/highscore.json", "r");
    if (!f) { highScore = 0; return; }
    StaticJsonDocument<64> doc;
    if (!deserializeJson(doc, f))
    {
        highScore = doc["hs"] | 0;
    }
    f.close();
    Serial.printf("[GAME] Loaded high score: %d\n", highScore);
}

// saveHighScore() — write the current high score back to flash storage.
//
// Opens highscore.json in write mode (creating it if absent), serialises the score
// into JSON, and closes the file.  SPIFFS handles wear-levelling automatically so
// repeated writes don't burn out the flash cells too quickly.
void saveHighScore()
{
    File f = SPIFFS.open("/highscore.json", "w");
    if (!f) return;
    StaticJsonDocument<64> doc;
    doc["hs"] = highScore;
    serializeJson(doc, f);
    f.close();
    Serial.printf("[GAME] Saved high score: %d\n", highScore);
}

// checkHighScore() — compare the current score against the stored record and save if beaten.
// Also sets the updateWsInfo flag so the new record is immediately broadcast to any
// connected web clients — they'll see the trophy animation without needing to refresh.
void checkHighScore()
{
    if (score > highScore)
    {
        highScore = score;
        saveHighScore();
        updateWsInfo = true;
    }
}

// loadSurvivalHighScore() — same as loadHighScore() but for the endless survival mode.
// Stored separately so classic-mode and survival-mode records don't overwrite each other.
void loadSurvivalHighScore()
{
    if (!SPIFFS.exists("/survival_hs.json")) { highScoreSurvival = 0; return; }
    File f = SPIFFS.open("/survival_hs.json", "r");
    if (!f) { highScoreSurvival = 0; return; }
    StaticJsonDocument<64> doc;
    if (!deserializeJson(doc, f))
        highScoreSurvival = doc["hs"] | 0;
    f.close();
    Serial.printf("[GAME] Loaded survival high score: %d\n", highScoreSurvival);
}

// saveSurvivalHighScore() — persist the survival-mode record to flash.
void saveSurvivalHighScore()
{
    File f = SPIFFS.open("/survival_hs.json", "w");
    if (!f) return;
    StaticJsonDocument<64> doc;
    doc["hs"] = highScoreSurvival;
    serializeJson(doc, f);
    f.close();
    Serial.printf("[GAME] Saved survival high score: %d\n", highScoreSurvival);
}

// ================= GAME LOGIC =================
//
// processGameLogic() — the top-level game brain, called every iteration of loop().
//
// This is the state machine dispatcher.  A *state machine* is a design pattern where
// a program can only be in one "state" at a time (e.g. SHOWING_LEVEL, PLAYING, GAME_OVER)
// and transitions between them in response to events.  It's used everywhere from traffic
// lights to video games to airplane autopilots.
//
// Each state gets its own branch:
//   SHOWING_LEVEL — displays a countdown animation while waiting for the player to press
//                   a button (or auto-starts after 3 s).
//   PLAYING       — runs the full game engine via gameLoop().
//   LEVEL_CLEARED — plays a comet celebration animation then advances the level.
//   GAME_OVER     — shows a high-score visualisation; any button restarts.
//   GAME_WON      — rainbow victory dance; player has beaten all 34 levels!
//
// At the end, WebSocket updates are rate-limited to one packet per 50 ms to avoid
// flooding connected clients with too much data.
//
// Interesting reading:
//   Finite state machines — https://en.wikipedia.org/wiki/Finite-state_machine
//   Game loop pattern     — https://gameprogrammingpatterns.com/game-loop.html
void processGameLogic() 
{
    readInputs();

    // Handle global control overrides
    if (inputs.restart) 
    { 
        inputs.restart = false; 
        resetLevel(true); 
		updateWsInfo = true;
		updateWsLeds = true;
    }
    else if (inputs.levelUp) 
    { 
        inputs.levelUp = false;
        currentLevel++;
		// Rollover to level 1 after reaching max level to allow continuous play
		if (currentLevel > MAX_GAME_LEVEL) 
		{
			currentLevel = 1;
		}
        resetLevel(false); 
		updateWsInfo = true;
		updateWsLeds = true;
    }
    else if (inputs.gameType)
    {
        inputs.gameType = false;
        ledCountIndex = (ledCountIndex + 1) % 4;
        activeLeds = LED_COUNTS[ledCountIndex];
        // Clear any LEDs that are now outside the active range
        for (int i = activeLeds; i < NUM_LEDS; i++)
        {
            leds[i] = CRGB::Black;
        }
        resetLevel(true);
        updateWsInfo = true;
        updateWsLeds = true;
    }
    else if (inputs.startSurvival)
    {
        inputs.startSurvival = false;
        survivalMode = true;
        lives = 1;
        score = 0;
        totalShots = 0;
        totalHits = 0;
        maxCombo = 0;
        comboMultiplier = 1;
        hasPowerUpR = false; hasPowerUpG = false; hasPowerUpB = false;
        for (int i = 0; i < MAX_ENEMIES; i++) enemies[i].active = false;
        for (int s = 0; s < MAX_SHOTS; s++) shots[s].active = false;
        hitEffect.active = false;
        freezeUsedThisLevel = false;
        freezeActive = false;
        reinforcementSent = false;
        initialEnemyCount = 8;
        enemyMoveDelay = BASE_ENEMY_SPEED;
        for (int i = 0; i < 8; i++)
        {
            enemies[i].active = true;
            enemies[i].position = (activeLeds - 1) - i;
            enemies[i].color = getRandomColor();
            enemies[i].isBoss = false; enemies[i].hitsLeft = 1;
            enemies[i].isPowerUp = false; enemies[i].isGold = false;
            enemies[i].isBomb = false;  enemies[i].goldHit1Color = CRGB::Black;
            enemies[i].goldHit1Time = 0; enemies[i].isArmored = false;
        }
        gameState = PLAYING;
        levelStartTime = millis();
        levelTimeLimit = 0;  // No time limit in survival
        lastSurvivalSpawn = millis();
        lastSpeedRampTime = millis();
        updateWsInfo = true;
        updateWsLeds = true;
    }

    // State Machine
    switch (gameState) 
    {
        case SHOWING_LEVEL:
        {
            updateShowingLevelState();

            // Auto-start after countdown; any button press skips immediately
            bool timedOut = (millis() - showingLevelEnterTime >= COUNTDOWN_MS);
            if (timedOut || inputs.redShort || inputs.redLong || inputs.grnShort || inputs.grnLong || inputs.bluShort || inputs.bluLong) 
            {
                clearInputs(); 
                startGameRound();
            }
            break;
        }
        case PLAYING:
        {
            gameLoop();
            break;
        }
        case LEVEL_CLEARED:
        {
            // Comet celebration: white-headed comet in levelClearColor sweeps enemy-end → player-end
            const unsigned long celebDuration = 600;
            unsigned long elapsed = millis() - levelClearedTime;

            FastLED.clear();
            int cometHead = (int)map(elapsed, 0, celebDuration, activeLeds - 1, -12);
            for (int t = 0; t < 12; t++)
            {
                int pos = cometHead + t;  // tail trails to the right (higher positions)
                if (pos < 0 || pos >= activeLeds) continue;
                CRGB c = (t == 0) ? CRGB::White : levelClearColor;
                c.nscale8(t == 0 ? 255 : (uint8_t)map(t, 1, 12, 220, 0));
                leds[pos] = c;
            }
            safeFastLEDShow();
            updateWsLeds = true;

            if (elapsed >= celebDuration)
            {
                if (currentLevel >= MAX_GAME_LEVEL)
                {
                    checkHighScore();
                    gameState = GAME_WON;
                }
                else
                {
                    currentLevel++;
                    resetLevel(false);
                }
            }
            break;
        }
        case GAME_OVER:
        {
            // Show high score as white LEDs on dim red background, refreshed every 100ms
            EVERY_N_MILLISECONDS(100)
            {
                int hiLeds = min(highScore / 10, activeLeds);
                fill_solid(leds, activeLeds, CRGB(30, 0, 0));
                for (int i = 0; i < hiLeds; i++) leds[i] = CRGB::White;
                safeFastLEDShow();
                updateWsLeds = true;
            }

            // Any button press restarts; survival mode is cleared
            if (inputs.redShort || inputs.redLong || inputs.grnShort || inputs.grnLong || inputs.bluShort || inputs.bluLong) 
            {
                survivalMode = false;
                resetLevel(true);
            }
            break;
        }
		case GAME_WON:
		{
			// Make the LEDs do a victory dance
			EVERY_N_MILLISECONDS(100)
			{
				fill_rainbow(leds, activeLeds, millis() / 10, 5);
				safeFastLEDShow();
				updateWsLeds = true;
			}

			// Allow restart on button press
			if (inputs.redShort || inputs.redLong || inputs.grnShort || inputs.grnLong || inputs.bluShort || inputs.bluLong)
			{
				resetLevel(true);
				updateWsInfo = true;
				updateWsLeds = true;
			}
			break;
		}
	}

	// Centralized websocket update - rate limited to once per 50ms
	unsigned long currentMillis = millis();
	if (currentMillis - lastWsUpdate >= WS_UPDATE_INTERVAL)
	{
		bool didUpdate = false;
		if (updateWsInfo)
		{
			notifyClients();
			updateWsInfo = false;
			didUpdate = true;
		}
		if (updateWsLeds)
		{
			sendLedState();
			updateWsLeds = false;
			didUpdate = true;
		}
		if (didUpdate)
		{
			lastWsUpdate = currentMillis;
		}
	}
}

// resetLevel() — wipe the slate and prepare for the next round.
//
// Called when:
//   * The player presses RESTART  (fullReset = true  — also resets score and lives)
//   * A level is cleared or skipped (fullReset = false — score and lives keep their values)
//
// It disables every enemy and every shot (setting active = false is faster and simpler
// than actually deleting objects — this is the classic "object pool" pattern used in
// most game engines to avoid slow dynamic memory allocation during gameplay).
//
// Enemy speed is recalculated from the base speed minus a fixed amount per level,
// clamped to a minimum of 50 ms so the game stays physically winnable.
//
// Interesting reading:
//   Object pool pattern — https://gameprogrammingpatterns.com/object-pool.html
void resetLevel(bool fullReset) 
{
    if (fullReset) 
    {
        currentLevel = 1; 
        score = 0; 
        comboMultiplier = 1;
        hasPowerUpR = false; 
        hasPowerUpG = false; 
        hasPowerUpB = false;
        lives = STARTING_LIVES;
        totalShots = 0;
        totalHits = 0;
        maxCombo = 0;
        survivalMode = false;
    }

    freezeUsedThisLevel = false;
    freezeActive = false;
    reinforcementSent = false;
    initialEnemyCount = 0;
    lastSpeedRampTime = 0;
    
    // Speed increases slightly every level
	// 1000ms at level 1, down to 30ms at level 20+ (which is basically unplayable but fun to watch)
    enemyMoveDelay = BASE_ENEMY_SPEED - ((currentLevel - 1) * SPEED_INC_PER_LVL);
    
    // Enforce a maximum speed cap so it doesn't break
    if (enemyMoveDelay < 50) 
    {
        enemyMoveDelay = 50;
    }
    
    // More enemies spawn per level
	// Start with 10 enemies at level 1, and add 2 more every level (10 at lvl 1, 12 at lvl 2, 14 at lvl 3, etc)
    enemiesToSpawn = NUM_STARTING_ENEMIES + ((currentLevel - 1) * 2);
	if (enemiesToSpawn > MAX_ENEMIES)
		enemiesToSpawn = MAX_ENEMIES;

    // Clear all game objects out of the arrays
    for (int i = 0; i < MAX_SHOTS; i++) 
    {
        shots[i].active = false;
    }
    
    hitEffect.active = false;
    
    for (int i = 0; i < MAX_ENEMIES; i++) 
    {
        enemies[i].active = false;
    }

	gameState = SHOWING_LEVEL;

    showLevelIndicator();
    updateWsInfo = true;
    updateWsLeds = true;
}

// showLevelIndicator() — paint the LED strip with a "level preview" display.
//
// The strip is divided into three sections:
//   [0 … level-1]      — white (or orange on boss levels) dots, one per level reached.
//   [level]            — a dark spacer.
//   [level+1 … end-4]  — a rainbow animation (updated each frame in updateShowingLevelState).
//   [end-3 … end-1]    — red, green, blue colour indicator dots.
//
// This gives the player an at-a-glance progress bar and a 3-second countdown before
// the action starts.  Boss levels (every 3rd) glow orange as a warning.
void showLevelIndicator() 
{
    gameState = SHOWING_LEVEL;
    FastLED.clear();
    
    // Draw level indicators: orange on boss levels, white otherwise
    CRGB levelColor = (currentLevel % 3 == 0) ? CRGB(255, 80, 0) : CRGB::White;
    int ledsToList = min(currentLevel, activeLeds);
    for (int i = 0; i < ledsToList; i++) 
    {
        leds[i] = levelColor;
    }
    
    // Draw the 3 color indicators at the far end
    leds[activeLeds - 1] = CRGB::Red; 
    leds[activeLeds - 2] = CRGB::Green; 
    leds[activeLeds - 3] = CRGB::Blue;
    
    safeFastLEDShow();
    updateWsLeds = true;

    // Re-seed idle SHOWING_LEVEL timers so effects start cleanly.
    lastShowingLevelFx = 0;
    lastShowingLevelSound = millis();
    nextShowingLevelSoundDelay = random(2000, 5001);
    showingLevelEnterTime = millis();
}

// updateShowingLevelState() — animate the SHOWING_LEVEL screen each frame.
//
// Two things happen here:
//  1. Every 2–5 seconds (random interval) a random sound plays to keep the idle
//     screen lively.
//  2. Every 80 ms the rainbow section is redrawn with fill_rainbow() from FastLED,
//     shifting the starting hue each frame to create scrolling motion.  fill_rainbow
//     uses the HSV colour model — Hue, Saturation, Value — which wraps colours smoothly
//     around a circle rather than jumping abruptly between R, G, B values.
//
// Interesting reading:
//   HSV colour model      — https://en.wikipedia.org/wiki/HSL_and_HSV
//   FastLED colour tricks — https://fastled.io/docs/group__colorutils.html
void updateShowingLevelState()
{
    unsigned long currentMillis = millis();

    // Randomly play one cached sound every 2-5 seconds while waiting to start.
    if (currentMillis - lastShowingLevelSound >= nextShowingLevelSoundDelay)
    {
        const char *idleSounds[] = {
            "/shoot.wav",
            "/hit.wav",
            "/power.wav",
            "/fail.wav"
        };
        int choice = random(0, 4);
        playSound(idleSounds[choice]);

        lastShowingLevelSound = currentMillis;
        nextShowingLevelSoundDelay = random(2000, 5001);
    }

    // Animate rainbow colors on all LEDs not part of the level display.
    // Level display: 0 to currentLevel-1 (white)
    // Spacer: currentLevel (off)
    // Rainbow: currentLevel+1 to NUM_LEDS-4 (before the 3 color indicators)
    // Color indicators: NUM_LEDS-3, NUM_LEDS-2, NUM_LEDS-1
    const unsigned long fxFrameMs = 80;
    if (currentMillis - lastShowingLevelFx >= fxFrameMs)
    {
        int rainbowStart = currentLevel + 1;
        int rainbowEnd = activeLeds - 3;
        int rainbowLen = rainbowEnd - rainbowStart;

        if (rainbowLen > 0)
        {
            fill_rainbow(&leds[rainbowStart], rainbowLen, showingLevelHue, 5);
        }
        showingLevelHue += 3;

        safeFastLEDShow();
        updateWsLeds = true;
        lastShowingLevelFx = currentMillis;
    }
}

// startGameRound() — spawn all enemies and officially begin a level.
//
// Enemies are placed in consecutive positions starting from the far end of the strip
// (position activeLeds-1) so they march toward the player (position 0) over time.
//
// Each enemy is given a random colour (Red / Green / Blue) and a random *type*:
//   Normal    — one matching hit kills it.
//   Power-up  — killing it grants a super-shot charge for that colour.
//   Armored   — first hit strips the grey armour; second (matching) hit kills it.
//   Gold      — needs two *different* coloured hits within 2 seconds; worth 5× points.
//   Bomb      — any hit explodes a 5-pixel radius; also explodes reaching the player.
//   Boss      — appears every 3rd level; 3 matching hits to kill; worth triple points.
//
// The time limit shrinks by 1 second per level (from 45 s down to a minimum of 15 s),
// rewarding fast players with a time bonus.
//
// Interesting reading:
//   Enemy archetypes in game design — https://www.gamedeveloper.com/design/the-art-of-enemy-design
void startGameRound() 
{
    gameState = PLAYING;
    FastLED.clear();

    bool isBossLevel = (currentLevel % 3 == 0);
    int startSlot = 0;

    // On boss levels, spawn the boss as the frontmost enemy
    if (isBossLevel)
    {
        enemies[0].active = true;
        enemies[0].position = activeLeds - 1;
        enemies[0].color = getRandomColor();
        enemies[0].isPowerUp = false;
        enemies[0].isBoss = true;
        enemies[0].hitsLeft = BOSS_HITS;
        enemies[0].isArmored = false;
        startSlot = 1;
    }

    // Spawn regular enemies stacked behind the boss (or from the front on non-boss levels)
    for (int i = 0; i < enemiesToSpawn; i++) 
    {
        int slot = startSlot + i;
        if (slot >= MAX_ENEMIES) break;

        enemies[slot].active = true;
        enemies[slot].position = (activeLeds - 1) - (startSlot + i);
        enemies[slot].color = getRandomColor();
        enemies[slot].isBoss = false;
        enemies[slot].hitsLeft = 1;
        
        // Random chance to become a powerup enemy
        if (random(100) < (POWERUP_CHANCE + (currentLevel / 4)))
        {
            enemies[slot].isPowerUp = true;
        }
        else
        {
            enemies[slot].isPowerUp = false;
        }
        // Armored (level 15+): ARMOR_CHANCE% chance, not combined with powerup
        enemies[slot].isArmored = (currentLevel >= ARMOR_START_LEVEL) && (random(100) < ARMOR_CHANCE) && !enemies[slot].isPowerUp;
        // Gold: GOLD_ENEMY_CHANCE%, not combined with powerup or armored
        enemies[slot].isGold = !enemies[slot].isPowerUp && !enemies[slot].isArmored && (random(100) < GOLD_ENEMY_CHANCE);
        if (enemies[slot].isGold) enemies[slot].color = CRGB(255, 180, 0); // Override color to gold
        // Bomb (level 5+): BOMB_ENEMY_CHANCE%, not combined with anything else
        enemies[slot].isBomb = !enemies[slot].isPowerUp && !enemies[slot].isArmored && !enemies[slot].isGold
                               && currentLevel >= 5 && (random(100) < BOMB_ENEMY_CHANCE);
        if (enemies[slot].isBomb) enemies[slot].color = CRGB(180, 0, 180); // Override color to magenta
        enemies[slot].goldHit1Color = CRGB::Black;
        enemies[slot].goldHit1Time = 0;
    }
    levelStartTime = millis();
    // currentLevel * 1000 < LEVEL_TIME_BASE_MS for all valid levels (max level 34)
    levelTimeLimit = max(LEVEL_TIME_MIN_MS, LEVEL_TIME_BASE_MS - (unsigned long)(currentLevel * 1000));

    // Count initial enemies for reinforcement trigger
    initialEnemyCount = countActiveEnemies();
    reinforcementSent = false;
    lastSpeedRampTime = millis();

    playSound("/power.wav");
}

// gameLoop() — ticks every frame while the game is in the PLAYING state.
//
// Seven mini-systems run in sequence each frame:
//
//  0a. Freeze power-up — if the player pressed all three buttons at once, all enemy
//      movement halts for FREEZE_DURATION_MS.  Only usable once per level.
//  0b. Gold enemy timeout — if a gold enemy's first-hit window expires before a second
//      hit lands, the first-hit colour is cleared and the player must start again.
//  0c. Speed ramp — every 10 s the enemy move interval shrinks by 5%, making the game
//      get gradually harder the longer a level drags on.
//  0d. Reinforcements — when ≤40% of the original enemies survive, a fresh wave of five
//      spawns from the far end.  No position overlap is allowed (isPositionOccupied check).
//  0e. Survival spawns — in endless mode a new enemy appears every 2.5 s.
//
//  1.  Shoot  — translate button inputs into new Projectile objects.
//  2.  Move shots — advance each active shot one pixel per shotMoveInterval.
//      The interval scales with strip length so longer strips don't feel sluggish.
//  3.  Move enemies — advance each active enemy one pixel toward the player.
//      If an enemy exits position 0, a life is lost.
//  4.  Render — call drawGame() to paint the current frame to the LED strip.
//
// The timing of moves is controlled by comparing millis() (milliseconds since boot)
// against the last-move timestamp.  This decouples game speed from loop speed — the
// loop runs as fast as possible, but things only move when their time slice arrives.
//
// Interesting reading:
//   Fixed timestep game loops — https://gameprogrammingpatterns.com/game-loop.html
void gameLoop() 
{
	updateWsLeds = false;
	updateWsInfo = false;

    unsigned long currentMillis = millis();

    // 0a. Handle freeze input
    if (inputs.freeze && gameState == PLAYING)
    {
        freezeActive = true;
        freezeEndTime = currentMillis + FREEZE_DURATION_MS;
        freezeUsedThisLevel = true;
        playSound("/power.wav");
        // Brief white flash
        fill_solid(leds, activeLeds, CRGB::White);
        safeFastLEDShow();
        updateWsLeds = true;
    }
    inputs.freeze = false;

    // 0b. Unfreeze when timer expires
    if (freezeActive && currentMillis >= freezeEndTime)
        freezeActive = false;

    // 0c. Expire gold enemy two-hit window
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        if (enemies[i].active && enemies[i].isGold &&
            !(enemies[i].goldHit1Color == CRGB::Black) &&
            currentMillis - enemies[i].goldHit1Time >= GOLD_ENEMY_TIMEOUT_MS)
        {
            enemies[i].goldHit1Color = CRGB::Black;
        }
    }

    // 0d. Progressive speed ramp: every SPEED_RAMP_INTERVAL_MS, enemies speed up 5%
    if (lastSpeedRampTime > 0 && currentMillis - lastSpeedRampTime >= SPEED_RAMP_INTERVAL_MS)
    {
        enemyMoveDelay = max(50UL, enemyMoveDelay * 95UL / 100UL);
        lastSpeedRampTime = currentMillis;
    }

    // 0e. Reinforcement wave: fires once when ≤40% of initial enemies remain
    if (!reinforcementSent && initialEnemyCount > 0 && !survivalMode)
    {
        int active = countActiveEnemies();
        if (active > 0 && active <= (initialEnemyCount * REINFORCE_THRESHOLD / 100))
        {
            reinforcementSent = true;
            // Scan from the far end and skip positions already occupied
            int nextReinforcePos = activeLeds - 1;
            for (int r = 0; r < REINFORCE_COUNT; r++)
            {
                while (nextReinforcePos >= 0 && isPositionOccupied(nextReinforcePos))
                    nextReinforcePos--;
                if (nextReinforcePos < 0) break;
                for (int j = 0; j < MAX_ENEMIES; j++)
                {
                    if (!enemies[j].active)
                    {
                        enemies[j].active = true;
                        enemies[j].position = nextReinforcePos;
                        enemies[j].color = getRandomColor();
                        enemies[j].isBoss = false; enemies[j].hitsLeft = 1;
                        enemies[j].isPowerUp = false; enemies[j].isGold = false;
                        enemies[j].isBomb = false; enemies[j].isArmored = false;
                        enemies[j].goldHit1Color = CRGB::Black;
                        enemies[j].goldHit1Time = 0;
                        nextReinforcePos--;
                        break;
                    }
                }
            }
        }
    }

    // 0f. Survival mode: continuously spawn enemies
    if (survivalMode && currentMillis - lastSurvivalSpawn >= SURVIVAL_SPAWN_INTERVAL)
    {
        if (countActiveEnemies() < 15)
        {
            // Find the highest unoccupied position from the far end
            int survivalSpawnPos = activeLeds - 1;
            while (survivalSpawnPos >= 0 && isPositionOccupied(survivalSpawnPos))
                survivalSpawnPos--;
            if (survivalSpawnPos >= 0)
            {
                for (int j = 0; j < MAX_ENEMIES; j++)
                {
                    if (!enemies[j].active)
                    {
                        enemies[j].active = true;
                        enemies[j].position = survivalSpawnPos;
                        enemies[j].color = getRandomColor();
                        enemies[j].isBoss = false; enemies[j].hitsLeft = 1;
                        enemies[j].isPowerUp = (random(100) < 10);
                        enemies[j].isGold = !enemies[j].isPowerUp && (random(100) < GOLD_ENEMY_CHANCE);
                        if (enemies[j].isGold) enemies[j].color = CRGB(255, 180, 0);
                        enemies[j].isBomb = !enemies[j].isPowerUp && !enemies[j].isGold && (random(100) < BOMB_ENEMY_CHANCE);
                        if (enemies[j].isBomb) enemies[j].color = CRGB(180, 0, 180);
                        enemies[j].isArmored = false;
                        enemies[j].goldHit1Color = CRGB::Black;
                        enemies[j].goldHit1Time = 0;
                        break;
                    }
                }
            }
        }
        lastSurvivalSpawn = currentMillis;
    }

    // 1. Handle shooting inputs
    if (inputs.redShort) 
    {
        triggerShoot(CRGB::Red, false);
    }
    if (inputs.redLong)  
    {
        triggerShoot(CRGB::Red, true);
    }
    if (inputs.grnShort) 
    {
        triggerShoot(CRGB::Green, false);
    }
    if (inputs.grnLong)  
    {
        triggerShoot(CRGB::Green, true);
    }
    if (inputs.bluShort) 
    {
        triggerShoot(CRGB::Blue, false);
    }
    if (inputs.bluLong)  
    {
        triggerShoot(CRGB::Blue, true);
    }
    clearInputs();

    // 2. Move Shots
    // Shot interval scales with strip length: 20ms at 120 LEDs, 10ms at 300 LEDs
    unsigned long shotMoveInterval = (unsigned long)map(activeLeds, 120, 300, SHOT_SPEED, SHOT_SPEED / 2);
    if (currentMillis - lastShotMove > shotMoveInterval) 
    {
        for (int s = 0; s < MAX_SHOTS; s++) 
        {
            if (shots[s].active) 
            {
                shots[s].position++;

                // Kill the shot if it runs off the strip
                if (shots[s].position >= activeLeds) 
                {
                    shots[s].active = false;
                    comboMultiplier = 1; // Missed shot breaks combo
                }
            }
        }
        checkGlobalCollisions();
        lastShotMove = currentMillis;
    }

    // 3. Move Enemies (skip entirely if frozen)
    if (!freezeActive && currentMillis - lastEnemyMove >= enemyMoveDelay) 
    {
        bool lifeLost = false;
        bool bombReachedFront = false;
        for (int i = 0; i < MAX_ENEMIES; i++) 
        {
            if (enemies[i].active) 
            {
                enemies[i].position--;
                
                // If an enemy reaches position 0, a life is lost
                if (enemies[i].position < 0) 
                {
                    enemies[i].active = false;
                    if (enemies[i].isBomb)
                        bombReachedFront = true;
                    else
                        lifeLost = true;
                }
            }
        }
        if (bombReachedFront)
        {
            // Bomb at the front: instant game over regardless of lives
            lives = 0;
            loseLife();
        }
        else if (lifeLost)
        {
            loseLife();
        }
        if (gameState == PLAYING)
        {
            checkGlobalCollisions();
        }
        lastEnemyMove = currentMillis;
    }

    // 4. Render to LEDs
    if (currentMillis - lastDraw > FRAME_DELAY) 
    {
        drawGame();
        lastDraw = currentMillis;
		updateWsInfo = true;
	}

	// Flags will be processed by centralized websocket update in processGameLogic()
	// Rate limiting happens there to ensure max 1 packet per 50ms
}

// === BUTTON INPUT LOGIC ===
//
// readInputs() — translate raw button states into high-level game events.
//
// This function runs every frame and manages three kinds of press:
//
//  SHORT press  — detected on button *release* if the button was held for less than
//                 LONG_PRESS_MS.  Fires a normal single-hit shot.
//
//  LONG press   — detected while the button is *still held* and the hold duration
//                 exceeds LONG_PRESS_MS.  Fires a super shot (if a power-up is charged).
//                 A flag (handledX) prevents the press from firing twice.
//
//  FREEZE combo — if all three game buttons are held simultaneously, the freeze
//                 power-up is triggered.  This is checked *before* the individual
//                 button handlers so a simultaneous press doesn't accidentally fire
//                 three shots at once.
//
// The ezButton library handles hardware debouncing — physical button contacts bounce
// for a few milliseconds when pressed, generating a burst of false edges; ezButton
// ignores edges that are too close together.
//
// Interesting reading:
//   Button debouncing         — https://www.allaboutcircuits.com/technical-articles/how-to-debounce-a-switch/
//   Pull-up resistors & INPUT — https://learn.sparkfun.com/tutorials/pull-up-resistors
void readInputs() 
{
    // Process ezButton state changes
    btnR.loop(); 
    btnG.loop(); 
    btnB.loop(); 
    btnRestart.loop(); 
    btnLevelUp.loop();
    btnGameType.loop();

    unsigned long currentMillis = millis();

    // FREEZE: detect all 3 game buttons held simultaneously (checked first to pre-empt shot inputs)
    {
        static bool freezeButtonHandled = false;
        bool allHeld = (btnR.getState() == LOW && btnG.getState() == LOW && btnB.getState() == LOW);
        if (allHeld && !freezeButtonHandled && !freezeUsedThisLevel && !freezeActive)
        {
            inputs.freeze = true;
            freezeButtonHandled = true;
            handledR = true; handledG = true; handledB = true;
            chargingR = false; chargingG = false; chargingB = false;
        }
        if (!allHeld) freezeButtonHandled = false;
    }

    // RED BUTTON
    if (btnR.isPressed()) 
    { 
        pressTimeR = currentMillis; 
        handledR = false; 
        chargingR = true; 
    }
    if (btnR.isReleased()) 
    { 
        chargingR = false;
        if (!handledR) 
        { 
            inputs.redShort = true; 
            handledR = true; 
        }
    }
    if (btnR.getState() == LOW && !handledR) 
    {
        if (currentMillis - pressTimeR >= LONG_PRESS_MS) 
        {
            inputs.redLong = true; 
            handledR = true; 
            chargingR = false;
        }
    }

    // GREEN BUTTON
    if (btnG.isPressed()) 
    { 
        pressTimeG = currentMillis; 
        handledG = false; 
        chargingG = true; 
    }
    if (btnG.isReleased()) 
    { 
        chargingG = false;
        if (!handledG) 
        { 
            inputs.grnShort = true; 
            handledG = true; 
        }
    }
    if (btnG.getState() == LOW && !handledG) 
    {
        if (currentMillis - pressTimeG >= LONG_PRESS_MS) 
        {
            inputs.grnLong = true; 
            handledG = true; 
            chargingG = false;
        }
    }

    // BLUE BUTTON
    if (btnB.isPressed()) 
    { 
        pressTimeB = currentMillis; 
        handledB = false; 
        chargingB = true; 
    }
    if (btnB.isReleased()) 
    { 
        chargingB = false;
        if (!handledB) 
        { 
            inputs.bluShort = true; 
            handledB = true; 
        }
    }
    if (btnB.getState() == LOW && !handledB) 
    {
        if (currentMillis - pressTimeB >= LONG_PRESS_MS) 
        {
            inputs.bluLong = true; 
            handledB = true; 
            chargingB = false;
        }
    }

    // CONTROL BUTTONS
    if (btnRestart.isPressed())
    {
        pressTimeRestart = currentMillis;
        handledRestart = false;
    }
    if (btnRestart.isReleased())
    {
        if (!handledRestart)
        {
            inputs.restart = true;
            handledRestart = true;
        }
    }
    // Long-hold RESTART on SHOWING_LEVEL → enter survival mode
    if (btnRestart.getState() == LOW && !handledRestart)
    {
        if (currentMillis - pressTimeRestart >= SURVIVAL_HOLD_MS)
        {
            inputs.startSurvival = true;
            handledRestart = true;
        }
    }
    if (btnLevelUp.isPressed()) 
    {
        inputs.levelUp = true;
    }
    if (btnGameType.isPressed())
    {
        inputs.gameType = true;
    }
}

// clearInputs() — reset all game-button flags to false after they've been processed.
//
// The inputs struct acts as a single-frame message box: a flag is set to true when an
// event occurs, processed by the game in the same frame, then cleared here.  Not clearing
// would cause the same input to trigger on every subsequent frame indefinitely.
void clearInputs() 
{
    inputs.redShort = false; 
    inputs.redLong = false;
    inputs.grnShort = false; 
    inputs.grnLong = false;
    inputs.bluShort = false; 
    inputs.bluLong = false;
    inputs.freeze = false;
}

// checkGlobalCollisions() — test every active shot against every active enemy.
//
// This is a classic O(n × m) collision check — for each of the MAX_SHOTS slots,
// we compare its position against every one of the MAX_ENEMIES slots.
// With small counts (≤20 shots, ≤100 enemies) it runs in microseconds.
//
// For larger games you'd use spatial partitioning (quad-trees, grids) to avoid
// checking every pair, but here 20 × 100 = 2000 comparisons is negligible.
//
// Position equality (==) is exact because both shots and enemies sit on integer LED
// indices with no sub-pixel precision — no floating-point headaches.
//
// Interesting reading:
//   Collision detection fundamentals — https://developer.mozilla.org/en-US/docs/Games/Techniques/2D_collision_detection
//   Spatial partitioning            — https://gameprogrammingpatterns.com/spatial-partition.html
void checkGlobalCollisions() 
{
	// Run over all short
    for (int s = 0; s < MAX_SHOTS; s++) 
    {
        if (!shots[s].active) 
        {
            continue;
        }
        
		// Run over all enemies to check for collisions
        for (int e = 0; e < MAX_ENEMIES; e++) 
        {
            if (!enemies[e].active) 
            {
                continue;
            }
            
			// Check if a shot has hit an enemy
			// The shot and enemy must be in the same position for a collision
            if (shots[s].position == enemies[e].position) 
            {
                handleCollision(s, e);
                if (!shots[s].active) 
                {
                    // Shot destroyed, stop checking it against other enemies
                    break; 
                }
            }
        }
    }
}

// handleCollision() — resolve what happens when a shot and an enemy occupy the same pixel.
//
// This is the richest function in the game; it implements five distinct outcomes:
//
//  1. ARMORED    — any shot strips the grey shell; the enemy survives but is now vulnerable.
//
//  2. GOLD       — two-hit mechanic:
//                  * First hit records the shot colour and starts a 2-second timer.
//                  * Second hit must be a *different* colour within the window → kill for 5× points.
//                  * Wrong colour or timeout → penalty: 3 random enemies spawn, combo resets.
//
//  3. BOMB       — any hit triggers a chain explosion that destroys all enemies within ±5 pixels.
//                  Score is awarded per enemy destroyed in the blast radius.
//
//  4. COLOUR MATCH — shot colour == enemy colour → enemy defeated.  If it's a boss, decrement
//                  hitsLeft; kill only when it reaches zero.  Power-up enemies grant a charge.
//                  Super shots chain to additional front-of-queue enemies of the same colour.
//
//  5. MISMATCH  — shot colour ≠ enemy colour → penalty.  The enemy *splits*, spawning a copy
//                  of itself adjacent (in a free position); enemies get faster; combo breaks.
//
// The defeatEnemy lambda is an inline helper that handles the shared bookkeeping of killing an
// enemy (score, combo, hit effect, power-up grants) to avoid copy-pasting it
// for normal kills and for super-shot chain kills.
//
// Interesting reading:
//   Game mechanics design  — https://www.gamedeveloper.com/design/designing-game-mechanics
//   Lambda functions in C++ — https://en.cppreference.com/w/cpp/language/lambda
void handleCollision(int shotIdx, int enemyIdx) 
{
    // Armored enemies: any shot strips armor first. No penalty; shot consumed.
    if (enemies[enemyIdx].isArmored)
    {
        enemies[enemyIdx].isArmored = false;
        shots[shotIdx].active = false;
        hitEffect.active = true;
        hitEffect.position = enemies[enemyIdx].position;
        hitEffect.startTime = millis();
        hitEffect.color = CRGB(100, 100, 100);
        playSound("/hit.wav");
        updateWsInfo = true;
        return;
    }

    // Gold enemy: requires 2 different-color hits in quick succession
    if (enemies[enemyIdx].isGold)
    {
        if (enemies[enemyIdx].goldHit1Color == CRGB::Black)
        {
            // First hit: record color, flash gold
            enemies[enemyIdx].goldHit1Color = shots[shotIdx].color;
            enemies[enemyIdx].goldHit1Time = millis();
            shots[shotIdx].active = false;
            hitEffect.active = true;
            hitEffect.position = enemies[enemyIdx].position;
            hitEffect.startTime = millis();
            hitEffect.color = CRGB(255, 180, 0);
            playSound("/hit.wav");
            updateWsInfo = true;
            return;
        }
        bool inWindow = (millis() - enemies[enemyIdx].goldHit1Time < GOLD_ENEMY_TIMEOUT_MS);
        bool diffColor = (shots[shotIdx].color != enemies[enemyIdx].goldHit1Color);
        if (inWindow && diffColor)
        {
            // Correct second hit: kill for 5x points
            levelClearColor = CRGB(255, 180, 0);
            enemies[enemyIdx].active = false;
            totalHits++;
            score += SCORE_PER_KILL * 5 * comboMultiplier;
            comboMultiplier = min(comboMultiplier + 1, 99);
            maxCombo = max(maxCombo, comboMultiplier);
            enemyMoveDelay = min(enemyMoveDelay + 5UL, (unsigned long)BASE_ENEMY_SPEED);
            hitEffect.active = true;
            hitEffect.position = enemies[enemyIdx].position;
            hitEffect.startTime = millis();
            hitEffect.color = CRGB(255, 180, 0);
            shots[shotIdx].active = false;
            playSound("/power.wav");
            if (countActiveEnemies() == 0)
            {
                score += SCORE_PER_LEVEL;
                if (levelStartTime > 0 && levelTimeLimit > 0)
                {
                    unsigned long te = millis() - levelStartTime;
                    if (te < levelTimeLimit)
                        score += (int)((long)(levelTimeLimit - te) * LEVEL_TIME_BONUS_MAX / (long)levelTimeLimit);
                }
                gameState = LEVEL_CLEARED;
                levelClearedTime = millis();
                playSound("/win.wav");
            }
            else { updateWsInfo = true; }
            return;
        }
        // Wrong second hit or timed out: penalty, spawn 3 enemies of the enemy's base color
        CRGB penaltyColor = enemies[enemyIdx].goldHit1Color; // spawn enemies matching the first hit color
        enemies[enemyIdx].goldHit1Color = CRGB::Black;
        enemies[enemyIdx].goldHit1Time = 0;
        comboMultiplier = 1;
        playSound("/fail.wav");
        for (int k = 0; k < 3; k++) addEnemyAtFront((activeLeds - 1) - k, getRandomColor());
        shots[shotIdx].active = false;
        enemyMoveDelay -= 20;
        if (enemyMoveDelay < 100) enemyMoveDelay = 100;
        updateWsInfo = true;
        return;
    }

    // Bomb enemy: any hit triggers explosion
    if (enemies[enemyIdx].isBomb)
    {
        int bombPos = enemies[enemyIdx].position;
        enemies[enemyIdx].active = false;
        shots[shotIdx].active = false;
        int killCount = 0;
        for (int b = 0; b < MAX_ENEMIES; b++)
        {
            if (enemies[b].active && abs(enemies[b].position - bombPos) <= BOMB_EXPLOSION_RADIUS)
            {
                enemies[b].active = false;
                score += SCORE_PER_KILL * comboMultiplier;
                totalHits++;
                killCount++;
            }
        }
        if (killCount > 0)
        {
            comboMultiplier = min(comboMultiplier + killCount, 99);
            maxCombo = max(maxCombo, comboMultiplier);
        }
        enemyMoveDelay = min(enemyMoveDelay + (unsigned long)(killCount * 5), (unsigned long)BASE_ENEMY_SPEED);
        hitEffect.active = true;
        hitEffect.position = bombPos;
        hitEffect.startTime = millis();
        hitEffect.color = CRGB(255, 0, 255);
        playSound("/power.wav");
        if (countActiveEnemies() == 0)
        {
            score += SCORE_PER_LEVEL;
            if (levelStartTime > 0 && levelTimeLimit > 0)
            {
                unsigned long te = millis() - levelStartTime;
                if (te < levelTimeLimit)
                    score += (int)((long)(levelTimeLimit - te) * LEVEL_TIME_BONUS_MAX / (long)levelTimeLimit);
            }
            gameState = LEVEL_CLEARED;
            levelClearedTime = millis();
            playSound("/win.wav");
        }
        else { updateWsInfo = true; }
        return;
    }

    // Match! Target destroyed.
    if (shots[shotIdx].color == enemies[enemyIdx].color) 
    {
        bool grantedPowerup = false;
        bool bossKilled = false;

        // Returns true if the enemy was destroyed, false if it was a boss that survived the hit.
        auto defeatEnemy = [&](int idx) -> bool
        {
            // Boss absorbs hits; only dies when hitsLeft reaches zero.
            if (enemies[idx].isBoss && enemies[idx].hitsLeft > 1)
            {
                enemies[idx].hitsLeft--;
                hitEffect.active = true;
                hitEffect.position = enemies[idx].position;
                hitEffect.startTime = millis();
                hitEffect.color = CRGB::White; // White flash on non-lethal boss hit
                return false;
            }

            levelClearColor = enemies[idx].color;  // capture before clearing active flag
            enemies[idx].active = false;
            totalHits++;  // successful match and kill

            // Boss is worth triple points
            int scoreGain = enemies[idx].isBoss ? (SCORE_PER_KILL * 3) : SCORE_PER_KILL;
            score += (scoreGain * comboMultiplier);

            if (enemies[idx].isBoss)
            {
                bossKilled = true;
            }

            // Slow enemies slightly for each successful kill, up to the base delay cap.
            enemyMoveDelay = min(enemyMoveDelay + 5UL, (unsigned long)BASE_ENEMY_SPEED);

            // Cap the multiplier at 99
            comboMultiplier = min(comboMultiplier + 1, 99);
            maxCombo = max(maxCombo, comboMultiplier);

            // Populate new hit effect data
            hitEffect.active = true;
            hitEffect.position = enemies[idx].position;
            hitEffect.startTime = millis();
            hitEffect.color = enemies[idx].color;

            if (enemies[idx].isPowerUp)
            {
                // Grant the correct color powerup
                if (enemies[idx].color == CRGB::Red)
                {
                    hasPowerUpR = true;
                }
                else if (enemies[idx].color == CRGB::Green)
                {
                    hasPowerUpG = true;
                }
                else if (enemies[idx].color == CRGB::Blue)
                {
                    hasPowerUpB = true;
                }
                grantedPowerup = true;
            }
            return true;
        };

        // Always attempt to defeat the collided enemy first.
        int enemiesDefeated = 0;
        if (defeatEnemy(enemyIdx))
        {
            enemiesDefeated = 1;
        }

        if (shots[shotIdx].isSuper)
        {
            // Super shots clear 2-5 continuous front-of-queue enemies of the same color.
            int targetKills = shots[shotIdx].hitsLeft;
            while (enemiesDefeated < targetKills)
            {
                int frontIdx = findFrontEnemyIndex();
                if (frontIdx < 0)
                {
                    break;
                }
                if (enemies[frontIdx].color != shots[shotIdx].color)
                {
                    break;
                }

                if (!defeatEnemy(frontIdx))
                {
                    break; // Hit a boss but didn't kill it; stop the chain
                }
                enemiesDefeated++;
            }
            shots[shotIdx].active = false;
        }
        else
        {
            shots[shotIdx].hitsLeft--;
            if (shots[shotIdx].hitsLeft <= 0)
            {
                shots[shotIdx].active = false;
            }
        }

        if (bossKilled || grantedPowerup)
        {
            playSound("/power.wav");
        }
        else
        {
            playSound("/hit.wav");
        }

        // Did we clear the board?
        if (countActiveEnemies() == 0)
		{
			score += SCORE_PER_LEVEL;

			// Time bonus: proportional to how quickly the level was cleared
			if (levelStartTime > 0 && levelTimeLimit > 0)
			{
				unsigned long elapsed = millis() - levelStartTime;
				if (elapsed < levelTimeLimit)
				{
					int timeBonus = (int)((long)(levelTimeLimit - elapsed) * LEVEL_TIME_BONUS_MAX / (long)levelTimeLimit);
					score += timeBonus;
				}
			}

			// Transition to LEVEL_CLEARED state to run celebration animation
			gameState = LEVEL_CLEARED;
			levelClearedTime = millis();
			playSound("/win.wav");
		}
		else 
        {
            updateWsInfo = true; // Update score and multiplier info on clients
        }
    } 
    else 
    {
        // Mismatch! Penalty.
        playSound("/fail.wav"); 
        comboMultiplier = 1; // Break the combo
        
        // Feature 2: enemy splits — spawn same-color enemy adjacent to the hit enemy
        addEnemyAtFront(enemies[enemyIdx].position - 1, enemies[enemyIdx].color);
        shots[shotIdx].active = false; // Destroy the shot

		// Make the enemies move faster as a penalty, down to a reasonable cap so it doesn't become impossible
		enemyMoveDelay -= 20;
		if (enemyMoveDelay < 100)
        {
            enemyMoveDelay = 100;
        }

        updateWsInfo = true; // Update score and multiplier info on clients
    }
}

// triggerShoot() — create a new projectile and send it flying toward the enemies.
//
// Projectiles are stored in a fixed-size array of MAX_SHOTS (20) slots — the
// "object pool" pattern again, avoiding heap allocation at runtime.
// If all 20 slots are occupied the shot is silently dropped; an unlikely edge case.
//
// Normal shot  (attemptSuper = false) — colour matches the button pressed; hitsLeft = 1.
//
// Super shot   (attemptSuper = true, power-up held) — fires a beam that can chain
//              through 2–5 enemies of the same colour.  Uses up the stored power-up.
//              If the player attempts a super shot without a charge, a normal shot fires.
//
// CRGB colours are compared by value.  CRGB::Red == {255, 0, 0}, and FastLED's ==
// operator compares all three channels, so this "just works" without special enums.
void triggerShoot(CRGB color, bool attemptSuper) 
{
    int freeSlot = -1;
    
    // Find an empty slot in the projectile array
    for (int i = 0; i < MAX_SHOTS; i++) 
    {
        if (!shots[i].active) 
        { 
            freeSlot = i; 
            break; 
        }
    }

    if (freeSlot != -1) 
    {
        shots[freeSlot].active = true;
        shots[freeSlot].position = 0;
        shots[freeSlot].color = color;
        shots[freeSlot].isSuper = false;
        shots[freeSlot].hitsLeft = 1;
        totalShots++;  // track accuracy

        if (attemptSuper) 
        {
            // Verify they actually have the powerup to use it
            bool authorized = false;
            if (color == CRGB::Red && hasPowerUpR) 
            {
                authorized = true;
            }
            else if (color == CRGB::Green && hasPowerUpG) 
            {
                authorized = true;
            }
            else if (color == CRGB::Blue && hasPowerUpB) 
            {
                authorized = true;
            }

            if (authorized) 
            {
                shots[freeSlot].isSuper = true;
                shots[freeSlot].hitsLeft = SUPER_SHOT_MAX_KILLS;
                playSound("/shoot.wav");
                
                // Consume the powerup
                if (color == CRGB::Red) 
                {
                    hasPowerUpR = false;
                }
                else if (color == CRGB::Green) 
                {
                    hasPowerUpG = false;
                }
                else 
                {
                    hasPowerUpB = false;
                }
            } 
            else 
            {
                // Tried to super shoot without a charge, regular shot instead
                playSound("/shoot.wav");
            }
        } 
        else 
        {
            playSound("/shoot.wav");
        }
        updateWsInfo = true;
    }
}

// === ENEMY GENERATION HELPERS ===

// getRandomColor() — return Red, Green, or Blue with equal probability.
//
// Uses Arduino's random() which produces a pseudorandom integer.  The underlying
// algorithm is a linear congruential generator (LCG) — fast and simple, though not
// cryptographically secure (not that we need that for a game!).
// The ESP32's boot ROM seeds the RNG from hardware noise on first use.
//
// Only three pure colours are used so the player always has exactly a 1-in-3 chance
// of guessing the right button without looking — fair and consistent.
//
// Interesting reading:
//   Linear congruential generator — https://en.wikipedia.org/wiki/Linear_congruential_generator
CRGB getRandomColor() 
{
    // Otherwise, generate a completely random color
    int r = random(3);
    CRGB newColor;
    
    if (r == 0) 
    {
        newColor = CRGB::Red;
    }
    else if (r == 1) 
    {
        newColor = CRGB::Green;
    }
    else 
    {
        newColor = CRGB::Blue;
    }
    
    return newColor;
}

// === DRAWING LOGIC ===
//
// drawGame() — render the entire game state to the LED strip each frame.
//
// FastLED's programming model is dead simple: fill the leds[] array with CRGB colour
// values, then call show() to push it to the hardware.  There's no "draw call" API;
// you just set array elements and show the result.
//
// Layers are painted in order — earlier layers can be overwritten.  The += operator
// additively blends (like overlapping coloured torches) while = replaces outright.
//
//  Layer 0 — Time bar: 0-to-4 dim pixels at the player end show time remaining.
//  Layer 1 — Charge animation: a growing glow at position 0 while a button is held.
//  Layer 2 — Enemies with type-specific rendering:
//            * Boss        — pulsing white core + coloured glow on neighbours.
//            * Gold        — amber, flashes faster after first hit lands.
//            * Bomb        — pulsing magenta + glow.
//            * Armored     — grey with a faint colour hint underneath.
//            * Power-up    — pulsing at a hardcoded brightness sequence.
//            * Normal      — solid colour.
//  Layer 3 — Shots: a coloured dot with a fading tail (18 pixels for super shots).
//  Layer 4 — Hit effect: a brief expanding flash at the collision point.
//  Layer 5 — Danger zone: additive red pulse on the first 5 LEDs when an enemy is close.
//  Layer 6 — Freeze overlay: a low-level blue tint while enemies are frozen.
//
// All enemies are now drawn in *reverse* index order so the lowest-index enemy
// (the one that collision logic targets first) paints on top and is always visible.
//
// Interesting reading:
//   WS2812B LED strip protocol — https://wp.josh.com/2014/05/13/ws2812-neopixels-are-not-so-bitmask-addressable/
//   FastLED colour mixing      — https://fastled.io/docs/group__colorutils.html
void drawGame() 
{
    FastLED.clear();

    // 0. Draw time remaining bar: up to 4 dim pixels at positions 0-3.
    //    Drawn first so enemies/shots paint on top of it.
    if (levelStartTime > 0 && levelTimeLimit > 0)
    {
        unsigned long elapsed = millis() - levelStartTime;
        if (elapsed < levelTimeLimit)
        {
            int barLen = (int)((levelTimeLimit - elapsed) * 4 / levelTimeLimit); // 0-4
            for (int i = 0; i < barLen; i++)
            {
                if      (barLen >= 3) leds[i] = CRGB(0, 50, 0);   // dim green
                else if (barLen == 2) leds[i] = CRGB(50, 30, 0);  // dim amber
                else                 leds[i] = CRGB(60, 0, 0);    // dim red
            }
        }
    }

    // Hardcoded pulse animation sequence for powerup enemies
    const uint8_t pupBrightnesses[] = {255, 255, 255, 255, 191, 127, 76, 127, 191};
    int seqStep = (millis() / 100) % 9;
    uint8_t currentPupBrightness = pupBrightnesses[seqStep];

    // 1. Draw Charging Animation
    if (chargingR || chargingG || chargingB) 
    {
        unsigned long startTime;
        CRGB chargeColor;
        
        if (chargingR)
        {
            startTime = pressTimeR;
            chargeColor = CRGB::Red;
        }
        else if (chargingG)
        {
            startTime = pressTimeG;
            chargeColor = CRGB::Green;
        }
        else
        {
            startTime = pressTimeB;
            chargeColor = CRGB::Blue;
        }
        
        int elapsed = millis() - startTime;
        
        // Expand from 0 to 5 LEDs over the duration of the long press
        int numChargeLeds = map(elapsed, 0, LONG_PRESS_MS, 0, 5); 
        
        for (int i = 0; i < numChargeLeds && i < NUM_LEDS; i++) 
        {
            leds[i] += chargeColor;
            
            // Bright leading edge to show charge progress
            if (i == numChargeLeds - 1) 
            {
                leds[i] += CRGB::White; 
            }
        }
    }

    // 2. Draw Enemies
    // Iterate in reverse so that the lowest-index enemy (which collision logic hits first)
    // is painted last and therefore visible when multiple enemies share a pixel.
    for (int i = MAX_ENEMIES - 1; i >= 0; i--) 
    {
        if (enemies[i].active) 
        {
            if (enemies[i].position < 0 || enemies[i].position >= activeLeds)
            {
                enemies[i].active = false;
                continue;
            }

            CRGB c = enemies[i].color;
            if (enemies[i].isBoss)
            {
                // White core with color glow on adjacent pixels.
                // Pulse rate increases as the boss takes damage (fewer hitsLeft = faster flash).
                int pulseMs = 40 + (enemies[i].hitsLeft * 60); // 220ms full hp, 160ms, 100ms at 1 hit left
                uint8_t bossBrightness = ((millis() / pulseMs) % 2 == 0) ? 255 : 80;
                CRGB bossCore = CRGB::White;
                bossCore.nscale8(bossBrightness);
                leds[enemies[i].position] = bossCore;
                CRGB glow = c;
                glow.nscale8(160);
                if (enemies[i].position - 1 >= 0)           leds[enemies[i].position - 1] += glow;
                if (enemies[i].position + 1 < activeLeds)   leds[enemies[i].position + 1] += glow;
            }
            else if (enemies[i].isGold)
            {
                // Gold enemy: bright amber, flashes faster once first hit lands
                unsigned long goldenMs = (enemies[i].goldHit1Color != CRGB::Black) ? 100UL : 250UL;
                uint8_t goldBright = ((millis() / goldenMs) % 2 == 0) ? 255 : 160;
                CRGB goldColor(255, 180, 0);
                goldColor.nscale8(goldBright);
                leds[enemies[i].position] = goldColor;
            }
            else if (enemies[i].isBomb)
            {
                // Bomb enemy: pulsing magenta
                uint8_t bombBright = ((millis() / 350UL) % 2 == 0) ? 255 : 80;
                CRGB bombColor(180, 0, 180);
                bombColor.nscale8(bombBright);
                leds[enemies[i].position] = bombColor;
                // Small glow on adjacent pixels
                CRGB glow(60, 0, 60);
                if (enemies[i].position - 1 >= 0)         leds[enemies[i].position - 1] += glow;
                if (enemies[i].position + 1 < activeLeds) leds[enemies[i].position + 1] += glow;
            }
            else if (enemies[i].isArmored)
            {
                // Grey armor with a faint color hint revealing the underlying color
                uint8_t base = currentPupBrightness / 2;
                CRGB armorColor(
                    base + enemies[i].color.r / 8,
                    base + enemies[i].color.g / 8,
                    base + enemies[i].color.b / 8
                );
                leds[enemies[i].position] = armorColor;
            }
            else if (enemies[i].isPowerUp) 
            {
                c.nscale8(currentPupBrightness);
                leds[enemies[i].position] = c;
            }
            else
            {
                leds[enemies[i].position] = c;
            }
        }
    }

    // 3. Draw Shots
    for (int i = 0; i < MAX_SHOTS; i++) 
    {
        if (shots[i].active) 
        {
            int headPos = shots[i].position;
            
            // Super shots get extremely long tails for motion blur
            int tailLen = shots[i].isSuper ? 18 : 5; 
            
            for (int t = 0; t <= tailLen; t++) 
            {
                int pixelPos = headPos - t;
                
                if (pixelPos >= 0 && pixelPos < activeLeds) 
                {
                    uint8_t fadeAmount = 255 - (t * (255 / (tailLen + 1)));
                    CRGB trailColor = shots[i].color;
                    
                    if (shots[i].isSuper) 
                    {
                        if (t == 0) 
                        {
                            trailColor = CRGB::White; 
                        }
                        else 
                        {
                            trailColor += CRGB(50, 50, 50); 
                        }
                    }
                    
                    trailColor.nscale8(fadeAmount);
                    leds[pixelPos] += trailColor; 
                }
            }
        }
    }

    // 4. Draw Expanded Hit Effects
    if (hitEffect.active) 
    {
        unsigned long elapsed = millis() - hitEffect.startTime;
        const int duration = 250; 

        if (elapsed < duration) 
        {
            // Shrapnel expands up to 4 pixels outward
            int radius = map(elapsed, 0, duration, 0, 4);
            
            // Fades from full brightness to zero
            uint8_t alpha = map(elapsed, 0, duration, 255, 0);

            for (int r = -radius; r <= radius; r++) 
            {
                int p = hitEffect.position + r;
                if (p >= 0 && p < activeLeds) 
                {
                    CRGB sparkCol;
                    if (r == 0)
                    {
                        sparkCol = CRGB::White;
                    }
                    else
                    {
                        sparkCol = hitEffect.color;
                    }
                    
                    sparkCol.nscale8(alpha);
                    leds[p] += sparkCol; 
                }
            }
        } 
        else 
        {
            hitEffect.active = false;
        }
    }

    // 5. Danger zone warning: additive red pulse on first 5 LEDs when any enemy is close
    {
        bool dangerClose = false;
        for (int i = 0; i < MAX_ENEMIES && !dangerClose; i++)
        {
            if (enemies[i].active && enemies[i].position <= DANGER_ZONE_DIST)
                dangerClose = true;
        }
        if (dangerClose)
        {
            uint8_t dangerPulse = ((millis() / 100) % 2 == 0) ? 80 : 0;
            for (int i = 0; i < 5 && i < activeLeds; i++)
                leds[i] += CRGB(dangerPulse, 0, 0);
        }
    }

    // 6. Freeze ice overlay: subtle blue tint pulsing while enemies are frozen
    if (freezeActive)
    {
        uint8_t iceAlpha = ((millis() / 200UL) % 2 == 0) ? 25 : 8;
        for (int i = 0; i < activeLeds; i++)
            leds[i] += CRGB(0, 0, iceAlpha);
    }

    safeFastLEDShow();
    updateWsLeds = true;
}

// ================= WEB SERVER & SOCKETS =================
//
// setupWiFi() — start a Wi-Fi Access Point (hotspot) on the ESP32.
//
// In AP mode the ESP32 itself creates the wireless network — no router needed.
// Any phone or laptop can connect to "LED_BLASTER" and then browse to 192.168.4.1
// to reach the game's web UI.  The IP address is assigned automatically by the ESP32's
// built-in DHCP server.
//
// Interesting reading:
//   Wi-Fi modes (STA vs AP)     — https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/wifi.html
//   DHCP explained simply       — https://www.howtogeek.com/680740/what-is-dhcp/
void setupWiFi() 
{
    WiFi.softAP(ssid, password);
    Serial.print("AP Started. IP: "); 
    Serial.println(WiFi.softAPIP());
}

// onWsEvent() — callback fired whenever a WebSocket event occurs.
//
// WebSockets are a browser technology that keeps a persistent two-way connection open
// between the web page and the server (the ESP32 here), so the page can send button
// presses instantly and receive LED updates without polling.
//
// Recognised text messages map directly to the same inputs struct used by the physical
// buttons — the physical buttons and the web UI are perfectly interchangeable.
//
// On WS_EVT_CONNECT (a client just connected) we immediately send current game state
// so the web page isn't blank while it waits for the next periodic update.
//
// Interesting reading:
//   WebSockets explained — https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API
//   ESPAsyncWebServer    — https://github.com/me-no-dev/ESPAsyncWebServer
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) 
{
    if (type == WS_EVT_DATA) 
    {
        AwsFrameInfo *info = (AwsFrameInfo *)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) 
        {
            String msg((const char *)data, len);
            
            if (msg == "BTN_R_SHORT") 
            {
                inputs.redShort = true;
            }
            else if (msg == "BTN_R_LONG") 
            {
                inputs.redLong = true;
            }
            else if (msg == "BTN_G_SHORT") 
            {
                inputs.grnShort = true;
            }
            else if (msg == "BTN_G_LONG") 
            {
                inputs.grnLong = true;
            }
            else if (msg == "BTN_B_SHORT") 
            {
                inputs.bluShort = true;
            }
            else if (msg == "BTN_B_LONG") 
            {
                inputs.bluLong = true;
            }
            else if (msg == "RESET") 
            {
                inputs.restart = true;
            }
            else if (msg == "LEVELUP") 
            {
                inputs.levelUp = true;
            }
            else if (msg == "GAMETYPE")
            {
                inputs.gameType = true;
            }
            else if (msg == "SURVIVAL")
            {
                inputs.startSurvival = true;
            }
            else if (msg == "FREEZE")
            {
                inputs.freeze = true;
            }
        }
    } 
    else if (type == WS_EVT_CONNECT) 
    {
        notifyClients();
    }
}

// setupWebRoutes() — register all HTTP URL handlers and start the web server.
//
// The ESPAsyncWebServer library lets us serve files directly from SPIFFS, so the
// game's web UI (HTML/CSS/JS in /data/index.html) loads as a normal webpage.
// Only one route is needed: GET / → serve index.html.  Everything else flows over
// the WebSocket connection once the page is loaded.
void setupWebRoutes() 
{
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r)
    { 
        r->send(SPIFFS, "/index.html", "text/html"); 
    });
    
    server.begin();
}

// notifyClients() — broadcast current game state to every connected browser.
//
// Serialises a compact JSON object containing score, level, combo, lives, power-ups,
// accuracy, active-LED count, time remaining, and survival/high-score values.
// JSON is used because JavaScript in the browser can parse it trivially with JSON.parse().
//
// ws.textAll() sends the string to every connected WebSocket client at once — handy if
// multiple phones are watching the same game simultaneously.
void notifyClients() 
{
    int timePct = 0;
    if (gameState == PLAYING && levelStartTime > 0 && levelTimeLimit > 0)
    {
        unsigned long elapsed = millis() - levelStartTime;
        if (elapsed < levelTimeLimit)
            timePct = (int)((levelTimeLimit - elapsed) * 100UL / levelTimeLimit);
    }

    StaticJsonDocument<640> doc;
    doc["score"] = score; 
    doc["level"] = survivalMode ? 0 : currentLevel; 
    doc["combo"] = comboMultiplier;
    doc["p_r"] = hasPowerUpR; 
    doc["p_g"] = hasPowerUpG; 
    doc["p_b"] = hasPowerUpB;
    doc["leds"] = activeLeds;
    doc["boss"] = (!survivalMode && currentLevel % 3 == 0);
    doc["hi"] = highScore;
    doc["time_pct"] = timePct;
    doc["lives"] = max(0, lives);
    doc["accuracy"] = totalShots > 0 ? (totalHits * 100 / totalShots) : 0;
    doc["max_combo"] = maxCombo;
    doc["survival"] = survivalMode;
    doc["hi_survival"] = highScoreSurvival;
    doc["freeze_ok"] = (!freezeUsedThisLevel && !freezeActive && gameState == PLAYING);
    
    String output; 
    serializeJson(doc, output); 
    ws.textAll(output);
}

// sendLedState() — push the raw pixel colour data to every browser as a binary WebSocket frame.
//
// JSON is too verbose for 300 pixels × 3 bytes (R,G,B) = 900 bytes per frame at up to
// 20 fps.  Instead we send a raw binary buffer: byte 0 = R of pixel 0, byte 1 = G of
// pixel 0, byte 2 = B of pixel 0, byte 3 = R of pixel 1 … and so on.
// The browser unpacks this with a Uint8Array and draws the pixels on a <canvas> element.
//
// The buffer is declared static so it lives in RAM permanently (no re-allocation each call)
// and is sized for the maximum possible strip (NUM_LEDS * 3 = 900 bytes).
void sendLedState() 
{
    // Static buffer sized for the maximum strip (900 bytes). Only send activeLeds worth.
    static uint8_t buffer[NUM_LEDS * 3];
    for (int i = 0; i < activeLeds; i++) 
    {
        buffer[i * 3] = leds[i].r; 
        buffer[i * 3 + 1] = leds[i].g; 
        buffer[i * 3 + 2] = leds[i].b;
    }
    ws.binaryAll(buffer, activeLeds * 3);
}

// isPositionOccupied() — return true if any active enemy already sits at this LED position.
//
// Used whenever new enemies are about to spawn to guarantee that no two enemies ever
// share the same pixel.  Without this check, two enemies overlapping at one position
// would cause a colour mismatch: the player sees one colour drawn on top but shooting
// it hits the *other* enemy underneath — a very confusing bug!
bool isPositionOccupied(int pos)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        if (enemies[i].active && enemies[i].position == pos) return true;
    }
    return false;
}

// addEnemyAtFront() — spawn a penalty or split enemy as close to the requested position
//                     as possible without overlapping any existing enemy.
//
// Called when:
//   * A colour mismatch occurs — a copy of the hit enemy spawns adjacent to it.
//   * A gold enemy penalty fires — 3 enemies spawn at the far end.
//
// Position search strategy: try toward the far end first (pos → activeLeds-1) so
// spawned enemies don't crowd the player; if the far side is full, fall back toward
// the player end (pos-1 → 0).  If every slot is occupied the spawn is skipped.
void addEnemyAtFront(int pos, CRGB col) 
{
    if (pos < 0 || pos >= activeLeds)
    {
        return;
    }

    // Find a free position: search toward the far end first, then toward the player end
    int freePos = -1;
    for (int p = pos; p < activeLeds; p++)
    {
        if (!isPositionOccupied(p)) { freePos = p; break; }
    }
    if (freePos < 0)
    {
        for (int p = pos - 1; p >= 0; p--)
        {
            if (!isPositionOccupied(p)) { freePos = p; break; }
        }
    }
    if (freePos < 0) return; // No free position available

    for (int i = 0; i < MAX_ENEMIES; i++) 
    {
        if (!enemies[i].active) 
        {
            enemies[i].active = true; 
            enemies[i].position = freePos;
            enemies[i].color = col; 
            enemies[i].isPowerUp = false;
            enemies[i].isBoss = false;
            enemies[i].hitsLeft = 1;
            enemies[i].isArmored = false;
            enemies[i].isGold = false;
            enemies[i].isBomb = false;
            enemies[i].goldHit1Color = CRGB::Black;
            enemies[i].goldHit1Time = 0;
            return;
        }
    }
}

// loseLife() — handle an enemy reaching the player's end of the strip.
//
// Steps:
//  1. Decrement lives and break the combo.
//  2. If lives reach 0 → GAME_OVER; save the high score.
//  3. Otherwise, *repack* surviving enemies tightly from the far end.
//     This gives the player a moment to breathe and resets positions to the same
//     layout as the initial spawn, making the game fair rather than punishing a
//     near-miss with an instantly adjacent enemy.
//  4. Clear all in-flight shots (they'd wrongly fly into the reformed lineup).
//  5. Reset enemy speed to the level's starting value (cancel any speed-ramp penalty).
void loseLife()
{
    lives--;
    comboMultiplier = 1;
    playSound("/fail.wav");

    // Brief red flash
    fill_solid(leds, activeLeds, CRGB::Red);
    safeFastLEDShow();

    if (lives <= 0)
    {
        gameState = GAME_OVER;
        checkHighScore();
        if (survivalMode && score > highScoreSurvival)
        {
            highScoreSurvival = score;
            saveSurvivalHighScore();
        }
        updateWsInfo = true;
        return;
    }

    // Repack all active enemies tightly from the far end of the strip (same as initial spawn)
    int resetPos = activeLeds - 1;
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        if (enemies[i].active)
        {
            enemies[i].position = resetPos--;
        }
    }

    // Clear active shots so nothing flies into the reset formation
    for (int s = 0; s < MAX_SHOTS; s++) shots[s].active = false;

    // Reset enemy speed to the level's base speed and cancel freeze/ramp
    freezeActive = false;
    enemyMoveDelay = BASE_ENEMY_SPEED - ((currentLevel - 1) * SPEED_INC_PER_LVL);
    if (enemyMoveDelay < 50) enemyMoveDelay = 50;
    lastSpeedRampTime = millis();

    updateWsInfo = true;
    updateWsLeds = true;
}

// countActiveEnemies() — return how many enemies are currently alive.
//
// A simple linear scan of the enemies[] object-pool array.  Called to detect level
// completion (count == 0), to decide whether to send reinforcements, and to cap
// survival-mode enemy population.
int countActiveEnemies() 
{
    int count = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) 
    {
        if (enemies[i].active) 
        {
            count++;
        }
    }
    return count;
}

// findFrontEnemyIndex() — find the enemy that is closest to the player (lowest position value).
//
// Used by the super-shot chain mechanic: after killing the directly-hit enemy, the shot
// looks for the next closest enemy and tries to hit that too, chaining up to hitsLeft times.
// Returns -1 if no enemies are active.
int findFrontEnemyIndex()
{
    int frontIdx = -1;
    int frontPos = NUM_LEDS + 1;

    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        if (enemies[i].active && enemies[i].position < frontPos)
        {
            frontPos = enemies[i].position;
            frontIdx = i;
        }
    }

    return frontIdx;
}