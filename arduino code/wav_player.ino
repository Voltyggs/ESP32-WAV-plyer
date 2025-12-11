#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Audio settings
#define MOSFET_PIN 25
#define PWM_FREQ 62500
#define PWM_RES 8
#define SD_CS 5
#define SAMPLE_RATE 8000
#define BUFFER_SIZE 16384

// Buttons
#define BUTTON_PLAY 26
#define BUTTON_NEXT 27
#define BUTTON_PREV 14
#define BUTTON_VOL_UP 32
#define BUTTON_VOL_DOWN 33

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SDA_PIN 21
#define SCL_PIN 22
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Globals
File wavFile;
uint8_t bufferA[BUFFER_SIZE];
uint8_t bufferB[BUFFER_SIZE];
volatile bool usingA = true;
int bufferIndex = 0;
int currentBufferSize = 0;

uint8_t volume = 120;

unsigned long lastSampleMicros = 0;
const unsigned long sampleInterval = 1000000 / SAMPLE_RATE;

bool isPaused = false;
bool lastPlayButton = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

String trackList[50];
int totalTracks = 0;
int currentTrack = 0;

unsigned long pressStartNext = 0, pressStartPrev = 0;
bool nextHeld = false, prevHeld = false;

unsigned long pressStartVolUp = 0, pressStartVolDown = 0;
bool volUpHeld = false, volDownHeld = false;
unsigned long lastAutoVolUp = 0, lastAutoVolDown = 0;

const unsigned long HOLD_THRESHOLD = 1000;
const unsigned long AUTO_INTERVAL = 50;

const char *SETTINGS_FILE = "/settings.txt";
unsigned long lastSaveTime = 0;
const unsigned long SAVE_INTERVAL = 5000;

bool refilling = false;
unsigned long lastMaintenance = 0;

// OLED Refresh
unsigned long lastOLEDUpdate = 0;
const unsigned long OLED_INTERVAL = 3000;
bool oledNeedsUpdate = true;

// Startup Mute PArt
bool startupMute = true;
unsigned long startupTime = 0;

unsigned long playHoldStart = 0;
bool playHeld = false;
const unsigned long PLAY_HOLD_SAVE_TIME = 3000;


// SETTINGS HANDLING

void saveSettings() {
  File f = SD.open(SETTINGS_FILE, FILE_WRITE);
  if (!f) return;

  unsigned long pos = wavFile ? wavFile.position() : 44;
  f.seek(0);
  f.printf("%d,%d,%lu,%d\n", currentTrack, volume, pos, isPaused ? 1 : 0);
  f.flush();
  f.close();
}

bool openTrack(int index);

void loadSettings() {
  if (!SD.exists(SETTINGS_FILE)) {
    File f = SD.open(SETTINGS_FILE, FILE_WRITE);
    if (f) {
      f.println("0,120,44,0");
      f.close();
    }
    return;
  }

  File f = SD.open(SETTINGS_FILE);
  if (!f) return;

  String line = f.readStringUntil('\n');
  f.close();

  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  int c3 = line.indexOf(',', c2 + 1);
  if (c1 == -1 || c2 == -1 || c3 == -1) return;

  currentTrack = line.substring(0, c1).toInt();
  volume = line.substring(c1 + 1, c2).toInt();
  unsigned long pos = line.substring(c2 + 1, c3).toInt();
  isPaused = line.substring(c3 + 1).toInt() == 1;

  if (volume > 255) volume = 255;
  if (currentTrack < 0 || currentTrack >= totalTracks) currentTrack = 0;

  if (openTrack(currentTrack)) {
    if (pos >= 44 && pos < wavFile.size()) wavFile.seek(pos);
  }
}

// TRACK HANDLINg

void loadTrackList() {
  File root = SD.open("/");
  totalTracks = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    if (!entry.isDirectory() && name.endsWith(".wav")) {
      trackList[totalTracks++] = name;
    }
    entry.close();
  }
  root.close();
  if (totalTracks == 0) while (1);
}

bool openTrack(int index) {
  if (index < 0 || index >= totalTracks) return false;
  if (wavFile) wavFile.close();
  wavFile = SD.open("/" + trackList[index]);
  if (!wavFile) return false;
  wavFile.seek(44);

  currentBufferSize = wavFile.read(bufferA, BUFFER_SIZE);
  yield();
  wavFile.read(bufferB, BUFFER_SIZE);
  usingA = true;
  bufferIndex = 0;

  return true;
}

// BUTTON HANDLERS

void handleVolumeButtons() {
  int up = digitalRead(BUTTON_VOL_UP);
  int down = digitalRead(BUTTON_VOL_DOWN);
  unsigned long now = millis();

  // UP PRESS
  if (up == LOW && pressStartVolUp == 0) pressStartVolUp = now;
  if (up == LOW) {
    if (!volUpHeld && (now - pressStartVolUp) >= HOLD_THRESHOLD) volUpHeld = true;

    if (volUpHeld && (now - lastAutoVolUp) >= AUTO_INTERVAL) {
      int v = volume; v++; if (v > 255) v = 255;
      volume = v;
      lastAutoVolUp = now;
    }

  } else if (up == HIGH && pressStartVolUp != 0) {
    if (!volUpHeld && (now - pressStartVolUp) < HOLD_THRESHOLD) {
      int v = volume; v++; if (v > 255) v = 255;
      volume = v;
    }
    pressStartVolUp = 0;
    volUpHeld = false;
  }

  // DOWN PRESS
  if (down == LOW && pressStartVolDown == 0) pressStartVolDown = now;
  if (down == LOW) {
    if (!volDownHeld && (now - pressStartVolDown) >= HOLD_THRESHOLD) volDownHeld = true;

    if (volDownHeld && (now - lastAutoVolDown) >= AUTO_INTERVAL) {
      int v = volume; v--; if (v < 0) v = 0;
      volume = v;
      lastAutoVolDown = now;
    }

  } else if (down == HIGH && pressStartVolDown != 0) {
    if (!volDownHeld && (now - pressStartVolDown) < HOLD_THRESHOLD) {
      int v = volume; v--; if (v < 0) v = 0;
      volume = v;
    }
    pressStartVolDown = 0;
    volDownHeld = false;
  }
}

void handleTrackButtons() {
  int next = digitalRead(BUTTON_NEXT);
  int prev = digitalRead(BUTTON_PREV);
  unsigned long now = millis();

  if (next == LOW && pressStartNext == 0) pressStartNext = now;
  else if (next == LOW && !nextHeld && (now - pressStartNext) > 2000) {
    nextHeld = true;
    currentTrack = (currentTrack + 1) % totalTracks;
    openTrack(currentTrack);
    oledNeedsUpdate = true;
  } else if (next == HIGH && pressStartNext != 0) {
    if (!nextHeld && (now - pressStartNext) < 2000) {
      unsigned long bytesPerSec = SAMPLE_RATE;
      long newPos = (long)wavFile.position() + (10 * (long)bytesPerSec);
      if (newPos < 44) newPos = 44;
      if ((unsigned long)newPos >= wavFile.size()) newPos = 44;
      wavFile.seek((unsigned long)newPos);
    }
    pressStartNext = 0;
    nextHeld = false;
  }

  if (prev == LOW && pressStartPrev == 0) pressStartPrev = now;
  else if (prev == LOW && !prevHeld && (now - pressStartPrev) > 2000) {
    prevHeld = true;
    currentTrack = (currentTrack - 1 + totalTracks) % totalTracks;
    openTrack(currentTrack);
    oledNeedsUpdate = true;
  } else if (prev == HIGH && pressStartPrev != 0) {
    if (!prevHeld && (now - pressStartPrev) < 2000) {
      unsigned long bytesPerSec = SAMPLE_RATE;
      long newPos = (long)wavFile.position() - (10 * (long)bytesPerSec);
      if (newPos < 44) newPos = 44;
      wavFile.seek((unsigned long)newPos);
    }
    pressStartPrev = 0;
    prevHeld = false;
  }
}

void handlePlayButton() {
  bool reading = digitalRead(BUTTON_PLAY);

  if (reading == LOW && playHoldStart == 0) {
    playHoldStart = millis();
  }

if (reading == LOW && !playHeld &&
      millis() - playHoldStart >= PLAY_HOLD_SAVE_TIME) {
    playHeld = true;

    saveSettings();

    isPaused = false;
    lastSampleMicros = micros();
    oledNeedsUpdate = true;
}


  if (reading != lastPlayButton) lastDebounceTime = millis();

  if ((millis() - lastDebounceTime) > debounceDelay) {
    static bool pressed = false;
    if (reading == LOW && !pressed) {
      if (!playHeld) {
        isPaused = !isPaused;
        oledNeedsUpdate = true;
        if (!isPaused) lastSampleMicros = micros();
      }
      pressed = true;
    } else if (reading == HIGH) {
      pressed = false;
      playHoldStart = 0;
      playHeld = false;
    }
  }

  lastPlayButton = reading;
}


// OLED DISPLAY

void showOLED() {
  unsigned long now = millis();
  if (!oledNeedsUpdate && (now - lastOLEDUpdate < OLED_INTERVAL)) return;

  lastOLEDUpdate = now;
  oledNeedsUpdate = false;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("Track ");
  display.print(currentTrack + 1);
  display.print("/");
  display.print(totalTracks);

  display.setCursor(0, 20);
  display.print("Vol: ");
  display.print(volume);

  int barWidth = map(volume, 0, 255, 0, 100);
  display.drawRect(14, 30, 100, 8, SSD1306_WHITE);
  display.fillRect(14, 30, barWidth, 8, SSD1306_WHITE);

  display.setCursor(0, 45);
  display.print(isPaused ? "Paused" : "Playing");

  float progress = (float)wavFile.position() / wavFile.size();
  int progWidth = (int)(progress * 100);
  display.drawRect(14, 55, 100, 6, SSD1306_WHITE);
  display.fillRect(14, 55, progWidth, 6, SSD1306_WHITE);

  display.display();
}

void setup() {
  Serial.begin(115200);
  Serial.println("initializng the uh esp32");

  pinMode(MOSFET_PIN, OUTPUT);
  pinMode(BUTTON_PLAY, INPUT_PULLUP);
  pinMode(BUTTON_NEXT, INPUT_PULLUP);
  pinMode(BUTTON_PREV, INPUT_PULLUP);
  pinMode(BUTTON_VOL_UP, INPUT_PULLUP);
  pinMode(BUTTON_VOL_DOWN, INPUT_PULLUP);

  ledcAttach(MOSFET_PIN, PWM_FREQ, PWM_RES);
  ledcWrite(MOSFET_PIN, 128);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD mount failed");
    while (1);
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
    while (1);
  }

  loadTrackList();
  loadSettings();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 25);
  display.print("Starting");
  display.display();
  delay(1000);

  startupTime = millis();
}

void loop() {
  handlePlayButton();
  handleTrackButtons();
  handleVolumeButtons();

  if (startupMute) {
    if (millis() - startupTime < 1200) {
      ledcWrite(MOSFET_PIN, 128);
      return;
    } else {
      startupMute = false;
      lastSampleMicros = micros();
    }
  }
  
  showOLED();

  if (isPaused) return;

  unsigned long now = micros();
  if (now - lastSampleMicros >= sampleInterval) {
    lastSampleMicros += sampleInterval;

    uint8_t *active = usingA ? bufferA : bufferB;
    uint8_t *inactive = usingA ? bufferB : bufferA;

    uint8_t sample = active[bufferIndex++];
    int16_t s = (int16_t)sample - 128;
    int32_t scaled = (s * volume) >> 7;
    uint8_t out = (uint8_t)constrain(scaled + 128, 0, 255);
    ledcWrite(MOSFET_PIN, out);

    if (bufferIndex >= currentBufferSize) {
      bufferIndex = 0;
      usingA = !usingA;

      if (wavFile.available()) {
        currentBufferSize = wavFile.read(inactive, BUFFER_SIZE);
      } else {
        openTrack(currentTrack);
      }
    }
  }
}
