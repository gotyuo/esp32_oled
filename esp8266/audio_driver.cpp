/*
 * EnvMon ESP8266 - MAX98357 I2S 音频驱动
 *
 * 目标硬件:
 *   - MAX98357 I2S Class-D amplifier
 *   - ESP8266Audio AudioOutputI2S
 *
 * 接线:
 *   BCLK -> GPIO15 (D8)
 *   LRC  -> GPIO2  (D4)
 *   DIN  -> GPIO3  (RX)
 */

#include <Arduino.h>
#include <math.h>
#include <ESP8266Audio.h>
#include "AudioFileSourceFunction.h"
#include "AudioGeneratorWAV.h"

#include "envmon_esp8266.h"

enum AudioSound {
  AUDIO_SOUND_NONE = 0,
  AUDIO_SOUND_WIFI_ON,
  AUDIO_SOUND_OTA_START,
  AUDIO_SOUND_ERROR,
  AUDIO_SOUND_ALARM
};

static AudioSound audioSound = AUDIO_SOUND_NONE;
static unsigned long audioSoundEnd = 0;
static unsigned long audioAlarmStart = 0;
static AudioGeneratorWAV *audioWav = nullptr;
static AudioFileSourceFunction *audioFile = nullptr;
static AudioOutputI2S *audioOut = nullptr;

static const char WIFI_ON_RTTTL[] PROGMEM = "D:4 O:5 N:G";
static const char OTA_RTTTL[] PROGMEM = "D:4 O:5 N:E";
static const char ERROR_RTTTL[] PROGMEM = "D:4 O:5 N:C";
static const char ALARM_RTTTL[] PROGMEM = "D:4 O:5 N:C,C,C,C,C,C,C,C";

static void audioClearPlayback() {
  if (audioWav) {
    audioWav->stop();
    delete audioWav;
    audioWav = nullptr;
  }
  if (audioFile) {
    delete audioFile;
    audioFile = nullptr;
  }
}

static void audioPlayWavTone(unsigned long durationMs, unsigned int hz, float amp) {
  if (!audioOut) return;
  audioClearPlayback();
  float durationSec = (durationMs < 20) ? 0.020f : (durationMs / 1000.0f);
  audioFile = new AudioFileSourceFunction(durationSec, 1, 22050, 16);
  audioFile->addAudioGenerators([hz, amp](float t) -> float {
    return amp * sin(TWO_PI * (float)hz * t);
  });
  audioWav = new AudioGeneratorWAV();
  audioWav->begin(audioFile, audioOut);

  unsigned long start = millis();
  while (audioWav && audioWav->isRunning()) {
    audioWav->loop();
    if (millis() - start >= durationMs) break;
  }
  audioClearPlayback();
}

void audioInit() {
  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(15, 2, 3, -1); // BCLK, LRC, DIN, no MCLK
  audioOut->SetRate(22050);
  audioOut->SetChannels(1);
  audioOut->SetOutputModeMono(true);
  audioOut->SetGain(0.25);

  Serial.printf("[Audio] MAX98357 I2S initialized: BCLK=%d LRC=%d DIN=%d sample=22050\n", 15, 2, 3);
  Serial.println("[Audio] 若无声，检查 VIN/GND/SPK 和 BCLK/LRC/DIN 接线");

  audioClearPlayback();
  audioPlayWavTone(120, 800, 0.18f);
}

void audioStartWiFiOn() {
  audioClearPlayback();
  audioPlayWavTone(120, 800, 0.18f);
}

void audioStartBoot() {
  audioClearPlayback();
  audioPlayWavTone(180, 800, 0.25f);
}

void audioStartOtaStart() {
  audioClearPlayback();
  audioPlayWavTone(160, 1000, 0.25f);
}

void audioStartError() {
  audioClearPlayback();
  audioPlayWavTone(240, 600, 0.25f);
}

void audioSetAlarm(bool active) {
  if (active) {
    if (audioSound != AUDIO_SOUND_ALARM) {
      audioClearPlayback();
      audioAlarmStart = millis();
      audioSound = AUDIO_SOUND_ALARM;
      audioSoundEnd = 0;
    }
  } else {
    audioClearPlayback();
    audioSound = AUDIO_SOUND_NONE;
    audioSoundEnd = 0;
    audioAlarmStart = 0;
  }
}

bool audioAlarmActive() {
  return audioSound == AUDIO_SOUND_ALARM;
}

bool audioBusy() {
  unsigned long now = millis();
  if (audioSound == AUDIO_SOUND_ALARM) return true;
  return audioSoundEnd > 0 && now < audioSoundEnd;
}

void audioTick() {
  unsigned long now = millis();

  if (audioSound == AUDIO_SOUND_ALARM) {
    unsigned long elapsed = now - audioAlarmStart;
    unsigned long cycle = elapsed % BUZZER_INTERVAL_MS;
    bool shouldOn = cycle <= BUZZER_ON_MS;

    if (shouldOn) {
      audioClearPlayback();
      audioPlayWavTone(BUZZER_ON_MS, 900, 0.35f);
    } else {
      audioClearPlayback();
    }
    return;
  }

  if (audioWav && audioWav->isRunning()) {
    audioWav->loop();
  }

  if (audioSoundEnd > 0 && now >= audioSoundEnd) {
    audioClearPlayback();
    audioSound = AUDIO_SOUND_NONE;
    audioSoundEnd = 0;
  }
}
