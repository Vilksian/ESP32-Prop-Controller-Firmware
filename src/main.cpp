#include <Arduino.h>
#include "WiFi.h"
#include "Audio.h"

// Digital IO used
#define I2S_DOUT  25  // DIN connection
#define I2S_BCLK  27  // Bit clock
#define I2S_LRC   26  // Word select / left-right clock

Audio audio;

string ssid = "3XKL7VQ";
string password = "42394H}k";

void setup() {
  Serial.begin(115200);
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(10); // 0 - 21

  // audio.connecttohost("http://rogers-hls.leanstream.co/rogers/van969.stream/playlist.m3u8"); // Jack FM - m3u8 streams need psram for some reason, why?
  audio.connecttohost("https://17653.live.streamtheworld.com/CHQMFMAAC.aac"); // MOVE FM
}

void loop() {
  audio.loop();
}

// Optional
void audio_info(const char *info) {
  Serial.print("Info        : "); Serial.println(info);
}

void audio_id3data(const char *info) {
  Serial.print("ID3 Data    : "); Serial.println(info);
}

void audio_eof_mp3(const char *info) {
  Serial.print("MP3 End     : "); Serial.println(info);
}

void audio_showstation(const char *info) {
  Serial.print("Station     : "); Serial.println(info);
}

void audio_showstreaminfo(const char *info) {
  Serial.print("Stream Info : "); Serial.println(info);
}

void audio_showstreamtitle(const char *info) {
  Serial.print("Stream Title: "); Serial.println(info);
}

void audio_bitrate(const char *info) {
  Serial.print("Bitrate     : "); Serial.println(info);
}

void audio_commercial(const char *info) {
  Serial.print("Commercial  : "); Serial.println(info);
}

void audio_icyurl(const char *info) {
  Serial.print("ICY URL     : "); Serial.println(info);
}

void audio_lasthost(const char *info) {
  Serial.print("Last Host   : "); Serial.println(info);
}

void audio_eof_speech(const char *info) {
  Serial.print("Speech End  : "); Serial.println(info);
}

