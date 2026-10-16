/*
  SD (SPI) WAV playback with two-voice mixing: ESP32 + PCM5102A / PCM5100A
  PlatformIO: ESP8266Audio @ 1.9.7 on espressif32@6.9.0 (Arduino core 2.x)

  SD card files (PCM WAV, 16-bit, SAME sample rate and channel count on all of them):
    /audio/ambience01.wav   looping background bed (its sample rate sets the output clock)
    /audio/thunder01.wav    one-shot, triggered from the Serial Monitor

  Serial Monitor at 115200 baud:  t = play thunder   s = stop thunder   h = heap

  v5: the I2S clock is locked to the ambience file's sample rate at boot and
      re-programmed every time the I2S driver (re)starts. The clock the hardware
      reports is printed, so a wrong speed can be traced to the clock or to the file.
  v4/v3: logging, plus timing telemetry once per second.

  Not tested on hardware by the author.
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s.h>
#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "AudioOutputMixer.h"

// ---------------- Pins ----------------
#define SD_SCK    18
#define SD_MISO   19
#define SD_MOSI   23
#define SD_CS      5

#define I2S_BCLK  27
#define I2S_LRC   26   // word select / LRCK
#define I2S_DOUT  25

// ---------------- Files ----------------
static const char *AMBIENT_PATH = "/audio/music01_44k.wav";
static const char *THUNDER_PATH = "/audio/thunder01_44k.wav";

// ---------------- Settings ----------------
static const float    AMBIENT_GAIN = 0.10f;
static const float    THUNDER_GAIN = 0.75f;
static const float    MASTER_GAIN  = 0.8f;
static const uint32_t SD_SPI_HZ    = 20000000;  // drop to 10000000 on read errors
static const uint32_t BUFFER_BYTES = 8192;      // read-ahead buffer per open file
static const int      DMA_BUFFERS  = 32;        // x 64 frames each: roughly 46 ms at 44.1 kHz
static const uint32_t REOPEN_WHEN_BYTES_LEFT = 400000;  // open the next pass of a looping track this close to its end

// The mixer forwards every track's sample rate to the one shared output, and the
// output driver can be stopped and restarted between tracks (for example when the
// ambience loops), which puts its clock back to a default. This class therefore:
//   - ignores all sample-rate requests from the library once lockRate() was called
//   - re-programs the locked rate every time the I2S driver starts
// A track whose rate differs from the locked rate plays at the wrong speed itself,
// but no longer changes the speed of the others.
class FixedRateI2S : public AudioOutputI2S {
 public:
  using AudioOutputI2S::AudioOutputI2S;

  void lockRate(int hz) {
    lockedHz = hz;
    locked = true;
    begin();   // start the I2S driver now and program the clock
  }

  bool begin() override {
    starting = true;
    bool ok = AudioOutputI2S::begin();
    starting = false;
    if (ok && locked) {
      AudioOutputI2S::SetRate(lockedHz);
      Serial.printf("[%lu ms] I2S driver started, clock %d Hz (hardware reports %d Hz)\n",
                    (unsigned long)millis(), lockedHz, (int)i2s_get_clk(I2S_NUM_0));
    }
    return ok;
  }

  bool SetRate(int hz) override {
    if (!locked) return AudioOutputI2S::SetRate(hz);
    if (!starting && hz != lockedHz) {
      Serial.printf("[%lu ms] Ignoring request for %d Hz (output locked to %d Hz)\n",
                    (unsigned long)millis(), hz, lockedHz);
    }
    return true;
  }

 private:
  bool locked   = false;
  bool starting = false;
  int  lockedHz = 0;
};

// One "voice" = one file being decoded into one mixer input.
struct Voice {
  const char            *name;      // for log lines
  const char            *path;
  float                  gain;
  bool                   repeat;
  AudioOutputMixerStub  *stub;
  AudioFileSourceSD     *file;      // currently playing
  AudioFileSourceBuffer *buf;
  AudioGeneratorWAV     *wav;
  AudioFileSourceSD     *nextFile;  // opened ahead of time
  AudioFileSourceBuffer *nextBuf;
  uint32_t               nextCheckMs;  // throttles the "nearly over?" check
  uint32_t               maxLoopUs;    // slowest wav->loop() since the last report
  uint32_t               calls;        // wav->loop() calls since the last report
};

FixedRateI2S     *out;
AudioOutputMixer *mixer;
Voice ambient;
Voice thunder;

void printHeap() {
  Serial.printf("Heap: free %u B, largest block %u B, lowest ever %u B\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
}

// Reads rate / channels / bit depth from a WAV header.
bool readWavFormat(const char *path, uint16_t &ch, uint32_t &rate, uint16_t &bits) {
  File f = SD.open(path);
  if (!f) return false;
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
    f.close();
    return false;
  }
  while (f.available()) {
    uint8_t ck[8];
    if (f.read(ck, 8) != 8) break;
    uint32_t size = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 44);
    if (memcmp(ck, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < 16 || f.read(fmt, 16) != 16) break;
      ch   = fmt[2] | (fmt[3] << 8);
      rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 44);
      bits = fmt[14] | (fmt[15] << 8);
      f.close();
      return true;
    }
    f.seek(f.position() + size + (size & 1));
  }
  f.close();
  return false;
}

// Prints both files' formats. Returns the ambience file's sample rate (0 if unreadable).
uint32_t checkFormats() {
  const char *paths[2] = { AMBIENT_PATH, THUNDER_PATH };
  uint16_t ch[2] = {0, 0}, bits[2] = {0, 0};
  uint32_t rate[2] = {0, 0};
  bool ok[2];
  for (int i = 0; i < 2; i++) {
    ok[i] = readWavFormat(paths[i], ch[i], rate[i], bits[i]);
    if (ok[i]) {
      File f = SD.open(paths[i]);
      uint32_t bytes = f ? (uint32_t)f.size() : 0;
      if (f) f.close();
      uint32_t bytesPerSec = rate[i] * ch[i] * (bits[i] / 8);
      Serial.printf("%s: %u Hz, %u ch, %u-bit, %u bytes (about %u s)\n", paths[i],
                    (unsigned)rate[i], ch[i], bits[i], (unsigned)bytes,
                    bytesPerSec ? (unsigned)(bytes / bytesPerSec) : 0u);
    } else {
      Serial.printf("%s: could not read WAV header (missing file?)\n", paths[i]);
    }
  }
  if (ok[0] && ok[1] && (rate[0] != rate[1] || ch[0] != ch[1] || bits[0] != bits[1])) {
    Serial.println("WARNING: the two files have different formats. The output clock follows the");
    Serial.println("ambience file, so the other file will play at the wrong speed. Re-export both");
    Serial.println("with the same sample rate, channel count and bit depth.");
  }
  return ok[0] ? rate[0] : 0;
}

void releaseNext(Voice &v) {
  if (v.nextBuf)  { delete v.nextBuf;  v.nextBuf  = nullptr; }
  if (v.nextFile) { delete v.nextFile; v.nextFile = nullptr; }
}

// Opens the file and its read-ahead buffer WITHOUT starting playback.
bool prepareNext(Voice &v) {
  if (v.nextBuf) return true;
  v.nextFile = new AudioFileSourceSD(v.path);
  if (!v.nextFile->isOpen()) {
    Serial.printf("Could not open %s\n", v.path);
    releaseNext(v);
    return false;
  }
  v.nextBuf = new AudioFileSourceBuffer(v.nextFile, BUFFER_BYTES);
  return true;
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
  uint32_t t0 = millis();
  if (!prepareNext(v)) return false;   // normally already done ahead of time
  stopVoice(v);

  v.file = v.nextFile;  v.nextFile = nullptr;
  v.buf  = v.nextBuf;   v.nextBuf  = nullptr;

  v.wav = new AudioGeneratorWAV();
  if (!v.wav->begin(v.buf, v.stub)) {
    Serial.printf("WAV begin failed for %s (PCM 16-bit WAV?)\n", v.path);
    stopVoice(v);
    return false;
  }
  Serial.printf("[%lu ms] %s started (start-up took %lu ms)\n",
                (unsigned long)millis(), v.name, (unsigned long)(millis() - t0));
  return true;
}

// Call every loop().
void pumpVoice(Voice &v) {
  if (v.wav && v.wav->isRunning()) {
    uint32_t t0 = micros();
    bool more = v.wav->loop();
    uint32_t dt = micros() - t0;
    v.calls++;
    if (dt > v.maxLoopUs) v.maxLoopUs = dt;

    if (!more) {                                // end of file
      Serial.printf("[%lu ms] %s reached end of file\n", (unsigned long)millis(), v.name);
      stopVoice(v);
      if (v.repeat) startVoice(v);
    } else if (v.repeat && !v.nextBuf && millis() >= v.nextCheckMs) {
      // looping track: open the next pass shortly before this one ends
      v.nextCheckMs = millis() + 250;
      uint32_t size = v.file->getSize();
      uint32_t pos  = v.file->getPos();
      if (size > pos && (size - pos) < REOPEN_WHEN_BYTES_LEFT) prepareNext(v);
    }
  } else if (!v.wav && !v.repeat) {
    prepareNext(v);                             // keep one-shots primed and ready
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
  uint32_t fileRate = checkFormats();

  out = new FixedRateI2S(0, AudioOutputI2S::EXTERNAL_I2S, DMA_BUFFERS);
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(MASTER_GAIN);
  out->lockRate(fileRate ? (int)fileRate : 44100);
  mixer = new AudioOutputMixer(32, out);

  ambient = { "ambient", AMBIENT_PATH, AMBIENT_GAIN, true,  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0 };
  thunder = { "thunder", THUNDER_PATH, THUNDER_GAIN, false, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0 };

  // Both mixer inputs are created once, up front, and reused for every play.
  ambient.stub = mixer->NewInput();
  ambient.stub->SetGain(AMBIENT_GAIN);
  thunder.stub = mixer->NewInput();
  thunder.stub->SetGain(THUNDER_GAIN);

  prepareNext(thunder);
  startVoice(ambient);
  printHeap();
}

void loop() {
  static uint32_t lastReport = 0, loops = 0, slowestUs = 0;
  uint32_t t0 = micros();

  pumpVoice(ambient);
  pumpVoice(thunder);

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 't') {
      Serial.printf("[%lu ms] 't' received\n", (unsigned long)millis());
      startVoice(thunder);
      printHeap();
    }
    if (c == 's') { stopVoice(thunder); }
    if (c == 'h') { printHeap(); }
  }

  uint32_t dt = micros() - t0;
  loops++;
  if (dt > slowestUs) slowestUs = dt;

  if (millis() - lastReport >= 1000) {
    Serial.printf("[%lu ms] I2S clock %d Hz | loop() %lu runs/s, slowest %lu us | ambient decoder: %lu calls, slowest %lu us | thunder decoder: %lu calls, slowest %lu us\n",
                  (unsigned long)millis(), (int)i2s_get_clk(I2S_NUM_0),
                  (unsigned long)loops, (unsigned long)slowestUs,
                  (unsigned long)ambient.calls, (unsigned long)ambient.maxLoopUs,
                  (unsigned long)thunder.calls, (unsigned long)thunder.maxLoopUs);
    lastReport = millis();
    loops = 0;
    slowestUs = 0;
    ambient.calls = 0;  ambient.maxLoopUs = 0;
    thunder.calls = 0;  thunder.maxLoopUs = 0;
  }
}