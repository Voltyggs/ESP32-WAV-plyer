//
//
// THERE ARE PARTS OF THIS CODE THAT ARE PARTS OF DIFFERENT SNIPPETS FROM DIFFERENT WEBSITES (eg. the uSD card nav and wav reading part)
//
//
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define mosfet_pin 25
#define pwm_frequency 62500 // make sure it's aboce 20KHz, i eating my brains out cuz of a constant sound
#define pwm_resoloution 8
#define uSD_resoloution 5
#define sample_rate 8000
#define buffer_size 16384 // in kilobytes btw

#define play_button 26
#define next_button 27
#define previous_buttom 14
#define vol_up_button 32
#define vol_down_button 33

#define s_width 128
#define s_height 64
#define oled_reset -1
#define oled_sda_pin 21
#define oled_scl_pin 22
Adafruit_SSD1306 display(s_width, s_height, &Wire, oled_reset);

unsigned long last_sample_micro = 0;
const unsigned long sample_interval = 1000000 / sample_rate;

uint8_t volume = 120;

bool is_it_paused_or_no = false;
bool last_play_button = HIGH;
unsigned long last_debounce_time = 0;
const unsigned long debounce_delay = 50;

File wavFile;
uint8_t firstbuffer[buffer_size];
uint8_t secondbuffer[buffer_size];
volatile bool usingA = true;
int buffer_index = 0;
int current_buffer-size = 0;

String track_list[50]; // i put 50 cuz i doubt i'll even store more than 20 tracks lmao
int totalTracks = 0;
int currentTrack = 0;

unsigned long press_start_next = 0, press_start_prev = 0;
bool r_u_holding_next_or_nah = false, r_u_holding_prev_or_nah = false; // lil easter eggs ahh variables

unsigned long press_start_vol_up = 0, press_start_vol_down = 0;
bool r_u_holding_vol_up_or_nah = false, r_u_holding_down_vol_down_or_nah = false;
unsigned long last_vol_up = 0, last_vol_down = 0;

const unsigned long hold_threshold = 1000; // ts is for how long u gotta press till the volume constantly changes
const unsigned long auto_interval = 50;

const char *settings_file = "/settings.txt";
unsigned long last_save_time = 0;
const unsigned long ave_interval = 5000;

bool refilling = false;
unsigned long last_maintainence = 0;

unsigned long last_oled_update = 0;
const unsigned long oled_interval = 3000;
bool oled_needs_update = true;

bool startup_mute = true; // had to add ts cuz the samples kept trynna catch up while booting, causing a terrible high pitch sound. It now mutes for a lil on startup if it's not paused
unsigned long startup_time = 0;

unsigned long play_hold_start = 0;
bool play_held = false;
const unsigned long play_hold_save_time = 3000; // u gotta hold the play button for 3 secs to update teh uSD


void saveSettings() {
  File f = SD.open(settings_file, FILE_WRITE);
  if (!f) return;

  unsigned long pos = wavFile ? wavFile.position() : 44;
  f.seek(0);
  f.printf("%d,%d,%lu,%d\n", currentTrack, volume, pos, is_it_paused_or_no ? 1 : 0);
  f.flush();
  f.close();
}

//had to pull up a lotta tutorials to learn the uSD card navigation. part of this is a snippet.
bool openTrack(int index);
void loadSettings() {
  if (!SD.exists(settings_file)) {
    File f = SD.open(settings_file, FILE_WRITE);
    if (f) {
      f.println("0,120,44,0");
      f.close();
    }
    return;
  }

  File f = SD.open(settings_file);
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
  is_it_paused_or_no = line.substring(c3 + 1).toInt() == 1;

  if (volume > 255) volume = 255;
  if (currentTrack < 0 || currentTrack >= totalTracks) currentTrack = 0;

  if (openTrack(currentTrack)) {
    if (pos >= 44 && pos < wavFile.size()) wavFile.seek(pos);
  }
}

void loadtrack_list() {
  File root = SD.open("/");
  totalTracks = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    if (!entry.isDirectory() && name.endsWith(".wav")) {
      track_list[totalTracks++] = name;
    }
    entry.close();
  }
  root.close();
  if (totalTracks == 0) while (1);
}

bool openTrack(int index) {
  if (index < 0 || index >= totalTracks) return false;
  if (wavFile) wavFile.close();
  wavFile = SD.open("/" + track_list[index]);
  if (!wavFile) return false;
  wavFile.seek(44);

  current_buffer-size = wavFile.read(firstbuffer, buffer_size);
  yield();
  wavFile.read(secondbuffer, buffer_size);
  usingA = true;
  buffer_index = 0;

  return true;
}

void handleVolumeButtons() {
  int up = digitalRead(vol_up_button);
  int down = digitalRead(BUTTON_VOL_DOWN);
  unsigned long now = millis();

  if (up == LOW && press_start_vol_up == 0) press_start_vol_up = now;
  if (up == LOW) {
    if (!r_u_holding_vol_up_or_nah && (now - press_start_vol_up) >= hold_threshold) r_u_holding_vol_up_or_nah = true;

    if (r_u_holding_vol_up_or_nah && (now - last_vol_up) >= auto_interval) {
      int v = volume; v++; if (v > 255) v = 255;
      volume = v;
      last_vol_up = now;
    }

  } else if (up == HIGH && press_start_vol_up != 0) {
    if (!r_u_holding_vol_up_or_nah && (now - press_start_vol_up) < hold_threshold) {
      int v = volume; v++; if (v > 255) v = 255;
      volume = v;
    }
    press_start_vol_up = 0;
    r_u_holding_vol_up_or_nah = false;
  }

  if (down == LOW && press_start_vol_down == 0) press_start_vol_down = now;
  if (down == LOW) {
    if (!r_u_holding_down_vol_down_or_nah && (now - press_start_vol_down) >= hold_threshold) r_u_holding_down_vol_down_or_nah = true;

    if (r_u_holding_down_vol_down_or_nah && (now - last_vol_down) >= auto_interval) {
      int v = volume; v--; if (v < 0) v = 0;
      volume = v;
      last_vol_down = now;
    }

  } else if (down == HIGH && press_start_vol_down != 0) {
    if (!r_u_holding_down_vol_down_or_nah && (now - press_start_vol_down) < hold_threshold) {
      int v = volume; v--; if (v < 0) v = 0;
      volume = v;
    }
    press_start_vol_down = 0;
    r_u_holding_down_vol_down_or_nah = false;
  }
}

void handleTrackButtons() {
  int next = digitalRead(next_button);
  int prev = digitalRead(previous_buttom);
  unsigned long now = millis();

  if (next == LOW && press_start_next == 0) press_start_next = now;
  else if (next == LOW && !r_u_holding_next_or_nah && (now - press_start_next) > 2000) {
    r_u_holding_next_or_nah = true;
    currentTrack = (currentTrack + 1) % totalTracks;
    openTrack(currentTrack);
    oled_needs_update = true;
  } else if (next == HIGH && press_start_next != 0) {
    if (!r_u_holding_next_or_nah && (now - press_start_next) < 2000) {
      unsigned long bytesPerSec = sample_rate;
      long newPos = (long)wavFile.position() + (10 * (long)bytesPerSec);
      if (newPos < 44) newPos = 44;
      if ((unsigned long)newPos >= wavFile.size()) newPos = 44;
      wavFile.seek((unsigned long)newPos);
    }
    press_start_next = 0;
    r_u_holding_next_or_nah = false;
  }

  if (prev == LOW && press_start_prev == 0) press_start_prev = now;
  else if (prev == LOW && !r_u_holding_prev_or_nah && (now - press_start_prev) > 2000) {
    r_u_holding_prev_or_nah = true;
    currentTrack = (currentTrack - 1 + totalTracks) % totalTracks;
    openTrack(currentTrack);
    oled_needs_update = true;
  } else if (prev == HIGH && press_start_prev != 0) {
    if (!r_u_holding_prev_or_nah && (now - press_start_prev) < 2000) {
      unsigned long bytesPerSec = sample_rate;
      long newPos = (long)wavFile.position() - (10 * (long)bytesPerSec);
      if (newPos < 44) newPos = 44;
      wavFile.seek((unsigned long)newPos);
    }
    press_start_prev = 0;
    r_u_holding_prev_or_nah = false;
  }
}

void handlePlayButton() {
  bool reading = digitalRead(play_button);

  if (reading == LOW && play_hold_start == 0) {
    play_hold_start = millis();
  }

if (reading == LOW && !play_held &&
      millis() - play_hold_start >= play_hold_save_time) {
    play_held = true;

    saveSettings();

    is_it_paused_or_no = false;
    last_sample_micro = micros();
    oled_needs_update = true;
}


  if (reading != last_play_button) last_debounce_time = millis();

  if ((millis() - last_debounce_time) > debounce_delay) {
    static bool pressed = false;
    if (reading == LOW && !pressed) {
      if (!play_held) {
        is_it_paused_or_no = !is_it_paused_or_no;
        oled_needs_update = true;
        if (!is_it_paused_or_no) last_sample_micro = micros();
      }
      pressed = true;
    } else if (reading == HIGH) {
      pressed = false;
      play_hold_start = 0;
      play_held = false;
    }
  }

  last_play_button = reading;
}

void showOLED() {
  unsigned long now = millis();
  if (!oled_needs_update && (now - last_oled_update < oled_interval)) return;

  last_oled_update = now;
  oled_needs_update = false;

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
  display.print(is_it_paused_or_no ? "Paused" : "Playing");

  float progress = (float)wavFile.position() / wavFile.size();
  int progWidth = (int)(progress * 100);
  display.drawRect(14, 55, 100, 6, SSD1306_WHITE);
  display.fillRect(14, 55, progWidth, 6, SSD1306_WHITE);

  display.display();
}

void setup() {
  Serial.begin(115200);
  Serial.println("initializng the uh esp32");

  pinMode(mosfet_pin, OUTPUT);
  pinMode(play_button, INPUT_PULLUP);
  pinMode(next_button, INPUT_PULLUP);
  pinMode(previous_buttom, INPUT_PULLUP);
  pinMode(vol_up_button, INPUT_PULLUP);
  pinMode(BUTTON_VOL_DOWN, INPUT_PULLUP);

  ledcAttach(mosfet_pin, pwm_frequency, pwm_resoloution);
  ledcWrite(mosfet_pin, 128);

  if (!SD.begin(uSD_resoloution)) {
    Serial.println("SD mount failed");
    while (1);
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
    while (1);
  }

  loadtrack_list();
  loadSettings();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 25);
  display.print("Starting");
  display.display();
  delay(1000);

  startup_time = millis();
}

void loop() {
  handlePlayButton();
  handleTrackButtons();
  handleVolumeButtons();

  if (startup_mute) {
    if (millis() - startup_time < 1200) {
      ledcWrite(mosfet_pin, 128);
      return;
    } else {
      startup_mute = false;
      last_sample_micro = micros();
    }
  }
  
  showOLED();

  if (is_it_paused_or_no) return;

  unsigned long now = micros();
  if (now - last_sample_micro >= sample_interval) {
    last_sample_micro += sample_interval;

    uint8_t *active = usingA ? firstbuffer : secondbuffer;
    uint8_t *inactive = usingA ? secondbuffer : firstbuffer;

    uint8_t sample = active[buffer_index++];
    int16_t s = (int16_t)sample - 128;
    int32_t scaled = (s * volume) >> 7;
    uint8_t out = (uint8_t)constrain(scaled + 128, 0, 255);
    ledcWrite(mosfet_pin, out);

    if (buffer_index >= current_buffer-size) {
      buffer_index = 0;
      usingA = !usingA;

      if (wavFile.available()) {
        current_buffer-size = wavFile.read(inactive, buffer_size);
      } else {
        openTrack(currentTrack);
      }
    }
  }
}