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

// Interrupt-safe FastLED.show() wrapper
inline void safeFastLEDShow() {
    // On ESP32/RMT, disabling global interrupts during show can cause worse timing artifacts.
    FastLED.show();
}

// ================= SETUP =================
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
void loop() 
{
    handleSerialDebugCommands();
    ws.cleanupClients();       // Maintain clean websocket connections
    processGameLogic();        // Run the main game engine
    updateStatusLeds();        // Update external hardware LEDs
}

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

void checkHighScore()
{
    if (score > highScore)
    {
        highScore = score;
        saveHighScore();
        updateWsInfo = true;
    }
}

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
            int spawnPos = activeLeds - 1;
            for (int r = 0; r < REINFORCE_COUNT; r++)
            {
                for (int j = 0; j < MAX_ENEMIES; j++)
                {
                    if (!enemies[j].active)
                    {
                        enemies[j].active = true;
                        enemies[j].position = spawnPos - r;
                        enemies[j].color = getRandomColor();
                        enemies[j].isBoss = false; enemies[j].hitsLeft = 1;
                        enemies[j].isPowerUp = false; enemies[j].isGold = false;
                        enemies[j].isBomb = false; enemies[j].isArmored = false;
                        enemies[j].goldHit1Color = CRGB::Black;
                        enemies[j].goldHit1Time = 0;
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
            for (int j = 0; j < MAX_ENEMIES; j++)
            {
                if (!enemies[j].active)
                {
                    enemies[j].active = true;
                    enemies[j].position = activeLeds - 1;
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

// === NEW ENEMY GENERATION LOGIC ===

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
    for (int i = 0; i < MAX_ENEMIES; i++) 
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
void setupWiFi() 
{
    WiFi.softAP(ssid, password);
    Serial.print("AP Started. IP: "); 
    Serial.println(WiFi.softAPIP());
}

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

void addEnemyAtFront(int pos, CRGB col) 
{
    if (pos < 0 || pos >= activeLeds)
    {
        return;
    }

    for (int i = 0; i < MAX_ENEMIES; i++) 
    {
        if (!enemies[i].active) 
        {
            enemies[i].active = true; 
            enemies[i].position = pos;
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