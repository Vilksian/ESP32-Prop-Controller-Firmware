/*
  SD (SPI) WAV playback with two-voice mixing: ESP32 + PCM5102A / PCM5100A

  Library: "ESP8266Audio" by Earle F. Philhower III (Arduino Library Manager)

  SD card files (PCM WAV, 16-bit, SAME sample rate and channel count on both):
    /audio/ambient01.wav   looping background bed
    /audio/thunder01.wav   one-shot, triggered from the Serial Monitor

  Serial Monitor at 115200 baud:
    t = play thunder     s = stop thunder     h = print free heap

  Not tested on hardware by the author. Start by checking: both files play
  together, thunder retriggers cleanly several times, and free heap stays flat.
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "AudioOutputMixer.h"

// ---------------- Pins: change to match your wiring ----------------
#define SD_SCK    18
#define SD_MISO   19
#define SD_MOSI   23
#define SD_CS      5

#define I2S_BCLK  27
#define I2S_LRC   26   // word select / LRCK
#define I2S_DOUT  25

// ---------------- Files ----------------
static const char *AMBIENT_PATH = "/audio/ambience01.wav";
static const char *THUNDER_PATH = "/audio/thunder01.wav";

// ---------------- Mix settings ----------------
// Keep the per-voice gains summing to <= 1.0 so peaks don't clip.
static const float    AMBIENT_GAIN = 0.85f;
static const float    THUNDER_GAIN = 0.15f;
static const float    MASTER_GAIN  = 0.8f;
static const uint32_t SD_SPI_HZ    = 20000000;  // drop to 10000000 on read errors
static const uint32_t BUFFER_BYTES = 16384;      // read-ahead buffer per voice

// One "voice" = one file being decoded into one mixer input.
struct Voice {
  const char            *path;
  float                  gain;
  bool                   repeat;
  AudioOutputMixerStub  *stub;
  AudioFileSourceSD     *file;
  AudioFileSourceBuffer *buf;
  AudioGeneratorWAV     *wav;
};

AudioOutputI2S   *out;
AudioOutputMixer *mixer;
Voice ambient;
Voice thunder;

void printHeap() {
  Serial.printf("Heap: free %u B, largest block %u B, lowest ever %u B\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
}

void stopVoice(Voice &v) {
  if (v.wav) {
    if (v.wav->isRunning()) v.wav->stop();
    delete v.wav;
    v.wav = nullptr;
  }
  if (v.stub) v.stub->stop();
  if (v.buf)  { delete v.buf;  v.buf  = nullptr; }
  if (v.file) { delete v.file; v.file = nullptr; }
}

bool startVoice(Voice &v) {
  stopVoice(v);

  v.file = new AudioFileSourceSD(v.path);
  if (!v.file->isOpen()) {
    Serial.printf("Could not open %s\n", v.path);
    stopVoice(v);
    return false;
  }
  v.buf = new AudioFileSourceBuffer(v.file, BUFFER_BYTES);

  if (!v.stub) v.stub = mixer->NewInput();
  v.stub->SetGain(v.gain);

  v.wav = new AudioGeneratorWAV();
  if (!v.wav->begin(v.buf, v.stub)) {
    Serial.printf("WAV begin failed for %s (PCM 16-bit WAV?)\n", v.path);
    stopVoice(v);
    return false;
  }
  Serial.printf("Playing %s\n", v.path);
  return true;
}

// Call every loop(). Restarts the voice when it ends if it is a repeating one.
void pumpVoice(Voice &v) {
  if (!v.wav) return;
  if (v.wav->isRunning()) {
    if (!v.wav->loop()) {          // end of file
      stopVoice(v);
      if (v.repeat) startVoice(v); // note: a short gap at the loop point
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  audioLogger = &Serial;   // library messages (format errors, etc.)

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, SD_SPI_HZ)) {
    Serial.println("SD mount failed: check wiring, FAT32 format, 3.3 V supply");
    while (true) delay(1000);
  }
  Serial.printf("SD card size: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));

  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(MASTER_GAIN);
  mixer = new AudioOutputMixer(32, out);

  ambient = { AMBIENT_PATH, AMBIENT_GAIN, true,  nullptr, nullptr, nullptr, nullptr };
  thunder = { THUNDER_PATH, THUNDER_GAIN, false, nullptr, nullptr, nullptr, nullptr };

  startVoice(ambient);
  printHeap();
}

void loop() {
  pumpVoice(ambient);
  pumpVoice(thunder);

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 't') { startVoice(thunder); printHeap(); }
    if (c == 's') { stopVoice(thunder); }
    if (c == 'h') { printHeap(); }
  }
}
