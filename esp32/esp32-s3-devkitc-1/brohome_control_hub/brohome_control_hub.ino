/*
  BroHome autonomous IR + 433 MHz control hub for ESP32-S3-WROOM-1 N16R8.

  Commands are stored as raw pulse timings in LittleFS. Uploading a new sketch
  does not erase the LittleFS partition. The companion remote_backup.py tool can
  export all learned commands to JSON and restore them after a full flash erase.

  Required libraries:
    IRremote 4.7.1
    RadioLib 7.6.0
*/

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPI.h>
#include <WiFi.h>
#include "secrets.h"
#include <WebServer.h>

// Raw timings are the canonical saved form. NEC and RC6 are decoded so they
// can be reproduced with their protocol-specific repeat and toggle behaviour.
#define RAW_BUFFER_LENGTH 2050
#define USE_16_BIT_TIMING_BUFFER
#define RECORD_GAP_MICROS 50000
#define DECODE_NEC
#define DECODE_RC6
#include <IRremote.hpp>

#include <RadioLib.h>
#include <driver/i2s_std.h>

// ------------------------------ Pin map -----------------------------------
constexpr uint8_t PIN_IR_RX = 4;
constexpr uint8_t PIN_IR_TX = 5;

constexpr uint8_t PIN_CC1101_GDO0 = 9;
constexpr uint8_t PIN_CC1101_CS = 10;
constexpr uint8_t PIN_CC1101_MOSI = 11;
constexpr uint8_t PIN_CC1101_SCK = 12;
constexpr uint8_t PIN_CC1101_MISO = 13;

constexpr uint8_t PIN_I2S_BCLK = 16;
constexpr uint8_t PIN_I2S_LRC = 17;
constexpr uint8_t PIN_I2S_DOUT = 18;

constexpr uint8_t PIN_MIC_BCLK = 6;
constexpr uint8_t PIN_MIC_WS = 7;
constexpr uint8_t PIN_MIC_DIN = 15;

// The installed RGB module has its red and blue leads on GPIO3/GPIO8.
constexpr uint8_t PIN_LED_R = 3;
constexpr uint8_t PIN_LED_G = 14;
constexpr uint8_t PIN_LED_B = 8;

// ----------------------------- Limits ------------------------------------
constexpr uint16_t MAX_PULSES = 2048;
constexpr uint32_t LEARN_TIMEOUT_MS = 15000;
constexpr uint32_t RF_END_GAP_US = 25000;
constexpr uint16_t RF_MIN_PULSES = 24;
constexpr uint32_t RF_CAPTURE_MAX_MS = 700;
constexpr float RF_RSSI_MARGIN_DB = 10.0f;
constexpr float RF_RSSI_MIN_DBM = -85.0f;
constexpr float RF_RSSI_MAX_DBM = -55.0f;
constexpr uint16_t IR_REPEAT_GAP_MS = 90;
constexpr uint32_t IR_SEQUENCE_CAPTURE_MS = 4000;
constexpr uint16_t IR_SEQUENCE_SPLIT_GAP_MS = 250;
constexpr uint8_t IR_SEQUENCE_MARKER = 2;       // Legacy single captured series.
constexpr uint8_t IR_DUAL_SEQUENCE_MARKER = 3;  // Two series with alternating toggle.
constexpr uint8_t IR_RAW_STREAM_MARKER = 4;
constexpr uint8_t IR_SECOND_BANK_SENDS = 4;
constexpr uint32_t IR_RAW_END_GAP_US = 180000;
constexpr uint32_t IR_RAW_CAPTURE_MAX_MS = 1500;
constexpr uint16_t IR_RAW_MIN_DURATIONS = 20;
constexpr uint32_t COMMAND_MAGIC = 0x31435242;  // "BRC1", little endian.
constexpr uint8_t COMMAND_VERSION = 1;
const char* AUTOMATION_FILE = "/automations.tsv";
constexpr uint8_t MAX_AUTOMATIONS = 64;

// Private installation settings live in ignored secrets.h. Start by copying
// secrets.example.h; never commit the real file.
const char* AP_SSID = BROHOME_AP_SSID;
const char* AP_PASSWORD = BROHOME_AP_PASSWORD;
const char* WIFI_SSID = BROHOME_WIFI_SSID;
const char* WIFI_PASSWORD = BROHOME_WIFI_PASSWORD;
const char* BROHOME_HOST = BROHOME_SERVER_HOST;
constexpr uint16_t BROHOME_PORT = 5001;
const char* DEVICE_NAME = BROHOME_DEVICE_NAME;

enum class CommandType : uint8_t { IR = 1, RF = 2 };
enum class LearnJobState : uint8_t { Idle, Running, Success, Error };

struct AutomationRecord {
  String id;
  String device;
  String room;
  String action;
  String type;
  String command;
  String phrases;
};

struct __attribute__((packed)) CommandHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t startLevel;
  uint8_t repeats;
  uint16_t carrierKhz;
  uint16_t pulseCount;
  uint32_t frequencyHz;
};

struct MicrophoneBlock {
  uint16_t count;
  uint16_t samples[256];
};

SPIClass radioSpi(FSPI);
Module radioModule(PIN_CC1101_CS, PIN_CC1101_GDO0, RADIOLIB_NC, RADIOLIB_NC, radioSpi);
CC1101 radio(&radioModule);
bool radioReady = false;

i2s_chan_handle_t audioTx = nullptr;
i2s_chan_handle_t microphoneRx = nullptr;
WebServer webServer(80);
WiFiClient broHomeClient;
QueueHandle_t microphoneQueue = nullptr;

volatile bool microphoneStreaming = false;
volatile bool broHomeSpeaking = false;

volatile bool rfCapturing = false;
volatile bool rfStarted = false;
volatile uint8_t rfStartLevel = LOW;
volatile uint16_t rfPulseCount = 0;
volatile uint32_t rfLastEdgeUs = 0;
volatile uint16_t rfPulses[MAX_PULSES];
uint16_t commandPulseBuffer[MAX_PULSES];
volatile bool irRawCapturing = false;
volatile bool irRawStarted = false;
volatile uint16_t irRawDurationCount = 0;
volatile uint32_t irRawLastEdgeUs = 0;
volatile uint32_t irRawDurations[MAX_PULSES / 2];

volatile LearnJobState learnJobState = LearnJobState::Idle;
CommandType learnJobType = CommandType::IR;
uint8_t learnJobRepeats = 1;
bool learnJobCaptureSequence = false;
uint16_t learnJobCarrierKhz = 38;
char learnJobName[49] = {};
char learnJobMessage[96] = {};

String serialLine;
File audioUpload;
size_t audioUploadExpected = 0;
size_t audioUploadWritten = 0;

// --------------------------- RGB status ---------------------------------
void setStatusColor(uint8_t red, uint8_t green, uint8_t blue) {
  analogWrite(PIN_LED_R, red);
  analogWrite(PIN_LED_G, green);
  analogWrite(PIN_LED_B, blue);
}

void statusBoot() { setStatusColor(80, 80, 80); }          // white
void statusWiFiConnecting() { setStatusColor(0, 0, 120); } // blue
void statusBroHomeOffline() { setStatusColor(80, 0, 100); }// violet
void statusListening() { setStatusColor(0, 90, 0); }       // green
// Use a saturated blue here: on this RGB module the blue crystal is much
// dimmer than green, so a cyan mix looked almost indistinguishable from idle.
void statusSpeaking() { setStatusColor(0, 0, 255); }       // bright blue
void statusLearning() { setStatusColor(120, 45, 0); }      // orange
void statusError() { setStatusColor(130, 0, 0); }          // red

// --------------------------- Helpers -------------------------------------
bool validName(const String& name) {
  if (name.isEmpty() || name.length() > 48) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
  }
  return true;
}

const char* typeText(CommandType type) {
  return type == CommandType::IR ? "ir" : "rf";
}

bool parseType(const String& text, CommandType& type) {
  if (text.equalsIgnoreCase("ir")) {
    type = CommandType::IR;
    return true;
  }
  if (text.equalsIgnoreCase("rf")) {
    type = CommandType::RF;
    return true;
  }
  return false;
}

bool validIrCarrier(uint16_t carrierKhz) {
  return carrierKhz == 36 || carrierKhz == 38 || carrierKhz == 40 || carrierKhz == 56;
}

String commandPath(CommandType type, const String& name) {
  return String("/commands/") + typeText(type) + "_" + name + ".brc";
}

String audioPath(const String& name) {
  return String("/audio/") + name + ".wav";
}

void ensureDirectories() {
  LittleFS.mkdir("/commands");
  LittleFS.mkdir("/audio");

  // Recover an interrupted command replacement. A .bak exists only between
  // moving the old command aside and committing the new command.
  File dir = LittleFS.open("/commands");
  File entry = dir.openNextFile();
  while (entry) {
    String path = entry.path();
    entry.close();
    if (path.endsWith(".bak")) {
      const String original = path.substring(0, path.length() - 4);
      if (LittleFS.exists(original)) LittleFS.remove(path);
      else LittleFS.rename(path, original);
    }
    entry = dir.openNextFile();
  }
  dir.close();
}

bool atomicSaveCommand(const String& path, const CommandHeader& header,
                       const uint16_t* pulses) {
  const String temporary = path + ".tmp";
  const String backup = path + ".bak";
  File file = LittleFS.open(temporary, FILE_WRITE);
  if (!file) return false;

  const bool ok = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                  file.write(reinterpret_cast<const uint8_t*>(pulses),
                             header.pulseCount * sizeof(uint16_t)) ==
                      header.pulseCount * sizeof(uint16_t);
  file.flush();
  file.close();
  if (!ok) {
    LittleFS.remove(temporary);
    return false;
  }
  LittleFS.remove(backup);
  const bool hadPrevious = LittleFS.exists(path);
  if (hadPrevious && !LittleFS.rename(path, backup)) {
    LittleFS.remove(temporary);
    return false;
  }
  if (!LittleFS.rename(temporary, path)) {
    if (hadPrevious) LittleFS.rename(backup, path);
    LittleFS.remove(temporary);
    return false;
  }
  LittleFS.remove(backup);
  return true;
}

bool loadCommand(CommandType type, const String& name, CommandHeader& header,
                 uint16_t* pulses) {
  File file = LittleFS.open(commandPath(type, name), FILE_READ);
  if (!file) return false;
  const bool headerOk = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                        header.magic == COMMAND_MAGIC && header.version == COMMAND_VERSION &&
                        header.type == static_cast<uint8_t>(type) &&
                        header.pulseCount > 0 && header.pulseCount <= MAX_PULSES;
  if (!headerOk) {
    file.close();
    return false;
  }
  const size_t bytes = header.pulseCount * sizeof(uint16_t);
  const bool dataOk = file.read(reinterpret_cast<uint8_t*>(pulses), bytes) == bytes;
  file.close();
  return dataOk;
}

// --------------------------- Audio ---------------------------------------
uint16_t readLe16(File& file) {
  uint8_t b[2] = {};
  if (file.read(b, 2) != 2) return 0;
  return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

uint32_t readLe32(File& file) {
  uint8_t b[4] = {};
  if (file.read(b, 4) != 4) return 0;
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

bool initAudio() {
  i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&channelConfig, &audioTx, nullptr) != ESP_OK) return false;

  i2s_std_config_t config = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = static_cast<gpio_num_t>(PIN_I2S_BCLK),
      .ws = static_cast<gpio_num_t>(PIN_I2S_LRC),
      .dout = static_cast<gpio_num_t>(PIN_I2S_DOUT),
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    },
  };
  return i2s_channel_init_std_mode(audioTx, &config) == ESP_OK &&
         i2s_channel_enable(audioTx) == ESP_OK;
}

bool initMicrophone() {
  i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&channelConfig, nullptr, &microphoneRx) != ESP_OK) return false;

  i2s_std_config_t config = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = static_cast<gpio_num_t>(PIN_MIC_BCLK),
      .ws = static_cast<gpio_num_t>(PIN_MIC_WS),
      .dout = I2S_GPIO_UNUSED,
      .din = static_cast<gpio_num_t>(PIN_MIC_DIN),
      .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    },
  };
  config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  return i2s_channel_init_std_mode(microphoneRx, &config) == ESP_OK &&
         i2s_channel_enable(microphoneRx) == ESP_OK;
}

bool testMicrophone() {
  if (!microphoneRx) return false;
  int32_t samples[256];
  uint64_t absoluteSum = 0;
  uint32_t peak = 0;
  uint32_t sampleCount = 0;
  const uint32_t started = millis();

  while (millis() - started < 2000) {
    size_t bytesRead = 0;
    if (i2s_channel_read(microphoneRx, samples, sizeof(samples), &bytesRead,
                         pdMS_TO_TICKS(250)) != ESP_OK) {
      continue;
    }
    const size_t count = bytesRead / sizeof(samples[0]);
    for (size_t i = 0; i < count; ++i) {
      const int32_t sample = samples[i] >> 8;
      const uint32_t magnitude = sample < 0 ? static_cast<uint32_t>(-sample)
                                            : static_cast<uint32_t>(sample);
      absoluteSum += magnitude;
      peak = max(peak, magnitude);
      ++sampleCount;
    }
  }

  if (sampleCount == 0) {
    Serial.println("MIC no samples");
    return false;
  }
  const uint32_t average = static_cast<uint32_t>(absoluteSum / sampleCount);
  Serial.printf("MIC samples=%lu average=%lu peak=%lu\n",
                static_cast<unsigned long>(sampleCount),
                static_cast<unsigned long>(average),
                static_cast<unsigned long>(peak));
  return peak > 0;
}

void microphoneTask(void*) {
  int32_t rawSamples[256];
  MicrophoneBlock block{};
  MicrophoneBlock stale{};

  while (true) {
    size_t bytesRead = 0;
    if (i2s_channel_read(microphoneRx, rawSamples, sizeof(rawSamples), &bytesRead,
                         portMAX_DELAY) != ESP_OK || bytesRead == 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    block.count = bytesRead / sizeof(rawSamples[0]);
    for (uint16_t i = 0; i < block.count; ++i) {
      int32_t sample = (rawSamples[i] >> 13) + 32768;
      sample = constrain(sample, 0, 65535);
      block.samples[i] = static_cast<uint16_t>(sample);
    }

    if (xQueueSend(microphoneQueue, &block, 0) != pdTRUE) {
      xQueueReceive(microphoneQueue, &stale, 0);
      xQueueSend(microphoneQueue, &block, 0);
    }
  }
}

bool connectHomeWiFi(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  statusWiFiConnecting();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Home Wi-Fi connected: %s, RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  WiFi.disconnect();
  statusBroHomeOffline();
  Serial.println("Home Wi-Fi unavailable; maintenance AP will be started");
  return false;
}

bool writeAllToBroHome(const uint8_t* data, size_t length) {
  size_t sent = 0;
  const uint32_t started = millis();
  while (sent < length && broHomeClient.connected()) {
    const size_t written = broHomeClient.write(data + sent, length - sent);
    if (written > 0) sent += written;
    else vTaskDelay(pdMS_TO_TICKS(1));
    if (millis() - started > 2000) return false;
  }
  return sent == length;
}

void beginNetworkPlayback(uint32_t sampleRate, uint32_t length) {
  if (!audioTx || sampleRate < 8000 || sampleRate > 48000 || length == 0) return;
  Serial.printf("BroHome playback started: %lu Hz, %lu bytes\n",
                static_cast<unsigned long>(sampleRate),
                static_cast<unsigned long>(length));
  i2s_channel_disable(audioTx);
  i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
  if (i2s_channel_reconfig_std_clock(audioTx, &clock) == ESP_OK &&
      i2s_channel_enable(audioTx) == ESP_OK) {
    broHomeSpeaking = true;
    statusSpeaking();
  }
}

void serviceBroHomePlayback() {
  static String header;
  static uint32_t remaining = 0;
  static bool haveLowByte = false;
  static uint8_t lowByte = 0;
  uint8_t bytes[256];
  int16_t stereo[256];

  while (broHomeClient.available()) {
    if (remaining == 0) {
      const char c = static_cast<char>(broHomeClient.read());
      if (c == '\n') {
        uint32_t rate = 0, length = 0;
        if (sscanf(header.c_str(), "BROHOME_PLAY/1 rate=%lu length=%lu",
                   &rate, &length) == 2 && length > 0) {
          remaining = length;
          haveLowByte = false;
          beginNetworkPlayback(rate, length);
        }
        header = "";
      } else if (c != '\r' && header.length() < 160) {
        header += c;
      }
      continue;
    }

    const size_t wanted = min<size_t>(sizeof(bytes), remaining);
    const int got = broHomeClient.read(bytes, wanted);
    if (got <= 0) break;
    remaining -= static_cast<uint32_t>(got);
    size_t stereoSamples = 0;
    for (int i = 0; i < got; ++i) {
      if (!haveLowByte) {
        lowByte = bytes[i];
        haveLowByte = true;
      } else {
        const int16_t sample = static_cast<int16_t>(lowByte | (bytes[i] << 8));
        stereo[stereoSamples++] = sample;
        stereo[stereoSamples++] = sample;
        haveLowByte = false;
      }
    }
    if (stereoSamples > 0) {
      size_t written = 0;
      i2s_channel_write(audioTx, stereo, stereoSamples * sizeof(int16_t), &written,
                        portMAX_DELAY);
    }
    if (remaining == 0) {
      broHomeSpeaking = false;
      statusListening();
      Serial.println("BroHome playback finished");
    }
  }
}

void broHomeNetworkTask(void*) {
  MicrophoneBlock block{};
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      microphoneStreaming = false;
      broHomeClient.stop();
      statusWiFiConnecting();
      connectHomeWiFi(10000);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!broHomeClient.connected()) {
      microphoneStreaming = false;
      statusBroHomeOffline();
      broHomeClient.stop();
      if (!broHomeClient.connect(BROHOME_HOST, BROHOME_PORT)) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      broHomeClient.setNoDelay(true);
      broHomeClient.printf(
          "BROHOME_AUDIO/1 device=%s rate=16000 bits=16 channels=1 encoding=adc_u16le\n",
          DEVICE_NAME);
      xQueueReset(microphoneQueue);
      microphoneStreaming = true;
      statusListening();
      Serial.println("BroHome audio connected");
    }

    serviceBroHomePlayback();
    if (xQueueReceive(microphoneQueue, &block, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (!writeAllToBroHome(reinterpret_cast<const uint8_t*>(block.samples),
                             block.count * sizeof(block.samples[0]))) {
        microphoneStreaming = false;
        broHomeClient.stop();
        statusError();
      }
    }
    serviceBroHomePlayback();
  }
}

bool playWav(const String& name) {
  if (!audioTx || !validName(name)) return false;
  File file = LittleFS.open(audioPath(name), FILE_READ);
  if (!file || file.size() < 44) return false;

  char id[5] = {};
  file.read(reinterpret_cast<uint8_t*>(id), 4);
  if (memcmp(id, "RIFF", 4) != 0) return false;
  (void)readLe32(file);
  file.read(reinterpret_cast<uint8_t*>(id), 4);
  if (memcmp(id, "WAVE", 4) != 0) return false;

  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t sampleRate = 0, dataSize = 0, dataOffset = 0;
  while (file.available() >= 8) {
    file.read(reinterpret_cast<uint8_t*>(id), 4);
    const uint32_t chunkSize = readLe32(file);
    const uint32_t next = file.position() + chunkSize + (chunkSize & 1U);
    if (memcmp(id, "fmt ", 4) == 0 && chunkSize >= 16) {
      format = readLe16(file);
      channels = readLe16(file);
      sampleRate = readLe32(file);
      (void)readLe32(file);
      (void)readLe16(file);
      bits = readLe16(file);
    } else if (memcmp(id, "data", 4) == 0) {
      dataOffset = file.position();
      dataSize = chunkSize;
      break;
    }
    file.seek(next);
  }
  if (format != 1 || channels != 1 || bits != 16 || sampleRate < 8000 ||
      sampleRate > 48000 || dataSize == 0) {
    file.close();
    return false;
  }

  i2s_channel_disable(audioTx);
  i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
  if (i2s_channel_reconfig_std_clock(audioTx, &clock) != ESP_OK ||
      i2s_channel_enable(audioTx) != ESP_OK) {
    file.close();
    return false;
  }
  file.seek(dataOffset);
  int16_t mono[256];
  int16_t stereo[512];
  uint32_t remaining = dataSize;
  while (remaining) {
    const size_t wanted = min<size_t>(sizeof(mono), remaining);
    const size_t got = file.read(reinterpret_cast<uint8_t*>(mono), wanted);
    if (!got) break;
    const size_t samples = got / sizeof(int16_t);
    for (size_t i = 0; i < samples; ++i) {
      stereo[i * 2] = mono[i];
      stereo[i * 2 + 1] = mono[i];
    }
    size_t written = 0;
    i2s_channel_write(audioTx, stereo, samples * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    remaining -= got;
  }
  file.close();
  return remaining == 0;
}

void feedback(const char* name) {
  (void)name;
}

void removeLegacyAudioFiles() {
  const char* names[] = {"ready", "learning_ir", "learning_rf", "saved", "error"};
  for (const char* name : names) LittleFS.remove(audioPath(name));
}

// --------------------------- RF capture ----------------------------------
void ARDUINO_ISR_ATTR onRfEdge() {
  if (!rfCapturing) return;
  const uint32_t now = micros();
  const uint8_t level = digitalRead(PIN_CC1101_GDO0);
  if (!rfStarted) {
    rfStarted = true;
    rfStartLevel = level;
    rfLastEdgeUs = now;
    return;
  }
  const uint32_t elapsed = now - rfLastEdgeUs;
  rfLastEdgeUs = now;
  if (elapsed < 80) return;  // Reject narrow glitches.
  if (rfPulseCount < MAX_PULSES && elapsed <= UINT16_MAX) {
    rfPulses[rfPulseCount++] = static_cast<uint16_t>(elapsed);
  } else {
    rfCapturing = false;
  }
}

bool learnRf(const String& name) {
  if (!radioReady) {
    Serial.println("ERR CC1101 is not available");
    return false;
  }
  feedback("learning_rf");
  Serial.printf("LEARN rf %s: press and hold the original remote button for about 1 second\n", name.c_str());

  // Enter RX first. Switching CC1101 into direct mode creates transitions on
  // GDO0, so capture must remain disabled until the receiver has settled.
  radio.standby();
  pinMode(PIN_CC1101_GDO0, INPUT);
  rfCapturing = false;
  rfStarted = false;
  rfPulseCount = 0;
  const int16_t state = radio.receiveDirectAsync();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("ERR CC1101 receiveDirectAsync=%d\n", state);
    return false;
  }
  delay(120);

  // Measure the local noise floor, then wait for a signal clearly stronger
  // than it. This prevents the OOK data slicer from recording idle RF noise.
  float noiseSum = 0.0f;
  for (uint8_t i = 0; i < 32; ++i) {
    noiseSum += radio.getRSSI();
    delay(3);
  }
  const float noiseFloor = noiseSum / 32.0f;
  const float triggerRssi = max(RF_RSSI_MIN_DBM,
                                min(RF_RSSI_MAX_DBM, noiseFloor + RF_RSSI_MARGIN_DB));
  Serial.printf("RF noise=%.1f dBm, trigger=%.1f dBm\n", noiseFloor, triggerRssi);

  const uint32_t waitStarted = millis();
  uint8_t strongSamples = 0;
  float peakRssi = -120.0f;
  while (millis() - waitStarted < LEARN_TIMEOUT_MS) {
    const float rssi = radio.getRSSI();
    peakRssi = max(peakRssi, rssi);
    if (rssi >= triggerRssi) {
      if (++strongSamples >= 3) break;
    } else {
      strongSamples = 0;
    }
    delay(2);
  }
  if (strongSamples < 3) {
    radio.standby();
    Serial.printf("ERR RF signal not detected, peak RSSI=%.1f dBm\n", peakRssi);
    feedback("error");
    return false;
  }

  noInterrupts();
  rfPulseCount = 0;
  rfStarted = false;
  rfLastEdgeUs = micros();
  interrupts();
  attachInterrupt(digitalPinToInterrupt(PIN_CC1101_GDO0), onRfEdge, CHANGE);
  rfCapturing = true;

  const uint32_t captureStarted = millis();
  bool complete = false;
  while (millis() - captureStarted < RF_CAPTURE_MAX_MS) {
    if (!rfCapturing) break;
    if (rfStarted && rfPulseCount >= RF_MIN_PULSES &&
        micros() - rfLastEdgeUs > RF_END_GAP_US) {
      complete = true;
      break;
    }
    delay(1);
  }
  if (rfStarted && rfPulseCount >= RF_MIN_PULSES) complete = true;
  rfCapturing = false;
  detachInterrupt(digitalPinToInterrupt(PIN_CC1101_GDO0));
  radio.standby();

  noInterrupts();
  const uint16_t count = rfPulseCount;
  const uint8_t startLevel = rfStartLevel;
  for (uint16_t i = 0; i < count; ++i) commandPulseBuffer[i] = rfPulses[i];
  interrupts();

  uint32_t totalUs = 0;
  uint16_t shortest = UINT16_MAX;
  uint16_t longest = 0;
  for (uint16_t i = 0; i < count; ++i) {
    totalUs += commandPulseBuffer[i];
    shortest = min(shortest, commandPulseBuffer[i]);
    longest = max(longest, commandPulseBuffer[i]);
  }
  if (!complete || count < RF_MIN_PULSES || totalUs < 5000) {
    Serial.printf("ERR RF capture rejected: pulses=%u duration=%lu us peak=%.1f dBm\n",
                  count, static_cast<unsigned long>(totalUs), peakRssi);
    feedback("error");
    return false;
  }

  // The stored waveform already contains the repetitions emitted while the
  // original button was held, so replay it once instead of multiplying a
  // possibly multi-frame recording four more times.
  CommandHeader header{COMMAND_MAGIC, COMMAND_VERSION, static_cast<uint8_t>(CommandType::RF),
                       startLevel, 1, 0, count, 433920000UL};
  if (!atomicSaveCommand(commandPath(CommandType::RF, name), header, commandPulseBuffer)) {
    Serial.println("ERR cannot save RF command");
    feedback("error");
    return false;
  }
  Serial.printf("OK learned rf %s pulses=%u duration=%lu us range=%u..%u peak=%.1f dBm start=%u\n",
                name.c_str(), count, static_cast<unsigned long>(totalUs), shortest, longest,
                peakRssi, startLevel);
  feedback("saved");
  return true;
}

// --------------------------- IR capture ----------------------------------
void ARDUINO_ISR_ATTR onIrRawEdge() {
  if (!irRawCapturing) return;
  const uint32_t now = micros();
  const uint8_t newLevel = digitalRead(PIN_IR_RX);
  if (!irRawStarted) {
    // Demodulating IR receivers are idle HIGH. The first falling edge is the
    // exact beginning of the first modulated mark.
    if (newLevel == LOW) {
      irRawStarted = true;
      irRawLastEdgeUs = now;
    }
    return;
  }
  const uint32_t elapsed = now - irRawLastEdgeUs;
  irRawLastEdgeUs = now;
  if (irRawDurationCount < MAX_PULSES / 2) {
    irRawDurations[irRawDurationCount++] = elapsed;
  } else {
    irRawCapturing = false;
  }
}

bool learnIrRawStream(const String& name, uint16_t carrierKhz) {
  Serial.printf("LEARN ir_raw %s carrier=%u kHz: press the original remote button once\n",
                name.c_str(), carrierKhz);
  IrReceiver.stop();
  pinMode(PIN_IR_RX, INPUT);
  noInterrupts();
  irRawCapturing = true;
  irRawStarted = false;
  irRawDurationCount = 0;
  irRawLastEdgeUs = 0;
  interrupts();
  attachInterrupt(digitalPinToInterrupt(PIN_IR_RX), onIrRawEdge, CHANGE);

  const uint32_t waitStarted = millis();
  while (!irRawStarted && millis() - waitStarted < LEARN_TIMEOUT_MS) delay(1);
  if (!irRawStarted) {
    irRawCapturing = false;
    detachInterrupt(digitalPinToInterrupt(PIN_IR_RX));
    IrReceiver.start();
    Serial.println("ERR raw IR learning timeout");
    feedback("error");
    return false;
  }

  const uint32_t captureStarted = millis();
  while (irRawCapturing && millis() - captureStarted < IR_RAW_CAPTURE_MAX_MS) {
    if (irRawDurationCount >= IR_RAW_MIN_DURATIONS &&
        micros() - irRawLastEdgeUs > IR_RAW_END_GAP_US) {
      break;
    }
    delay(1);
  }
  irRawCapturing = false;
  detachInterrupt(digitalPinToInterrupt(PIN_IR_RX));

  noInterrupts();
  const uint16_t durationCount = irRawDurationCount;
  for (uint16_t i = 0; i < durationCount; ++i) {
    const uint32_t duration = irRawDurations[i];
    commandPulseBuffer[i * 2] = duration & 0xFFFF;
    commandPulseBuffer[i * 2 + 1] = duration >> 16;
  }
  interrupts();
  IrReceiver.start();

  if (durationCount < IR_RAW_MIN_DURATIONS) {
    Serial.printf("ERR raw IR signal too short: %u durations\n", durationCount);
    feedback("error");
    return false;
  }
  const uint16_t words = durationCount * 2;
  CommandHeader header{COMMAND_MAGIC, COMMAND_VERSION, static_cast<uint8_t>(CommandType::IR),
                       IR_RAW_STREAM_MARKER, 1, carrierKhz, words, 0};
  const bool saved = atomicSaveCommand(commandPath(CommandType::IR, name), header,
                                       commandPulseBuffer);
  if (!saved) {
    Serial.println("ERR cannot save raw IR stream");
    feedback("error");
    return false;
  }
  uint64_t totalUs = 0;
  for (uint16_t i = 0; i < durationCount; ++i) totalUs += irRawDurations[i];
  Serial.printf("OK learned ir_raw %s durations=%u total=%llu us carrier=%u kHz\n",
                name.c_str(), durationCount, totalUs, carrierKhz);
  feedback("saved");
  return true;
}

bool learnIr(const String& name, uint8_t repeats = 1, bool captureSequence = false,
             uint16_t carrierKhz = 38) {
  feedback("learning_ir");
  if (captureSequence) return learnIrRawStream(name, carrierKhz);
  Serial.printf("LEARN ir %s: press the original remote button\n", name.c_str());
  IrReceiver.start();
  const uint32_t started = millis();
  while (millis() - started < LEARN_TIMEOUT_MS) {
    if (IrReceiver.decode()) {
      const bool overflow = (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_WAS_OVERFLOW) != 0;
      const uint16_t rawLength = IrReceiver.irparams.rawlen;
      if (!overflow && rawLength > 3 && rawLength - 1 <= MAX_PULSES) {
        if (captureSequence) {
          uint16_t used = 0;
          uint8_t frames = 0;
          uint8_t banks = 1;
          const uint32_t sequenceStarted = millis();
          bool haveFrame = true;
          while (millis() - sequenceStarted < IR_SEQUENCE_CAPTURE_MS) {
            if (haveFrame || IrReceiver.decode()) {
              haveFrame = false;
              const bool frameOverflow =
                  (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_WAS_OVERFLOW) != 0;
              const uint16_t frameRawLength = IrReceiver.irparams.rawlen;
              const uint16_t framePulses = frameRawLength > 0 ? frameRawLength - 1 : 0;
              if (!frameOverflow && framePulses > 2 && used + framePulses + 2 <= MAX_PULSES) {
                const uint32_t gapUs =
                    static_cast<uint32_t>(IrReceiver.decodedIRData.initialGapTicks) * MICROS_PER_TICK;
                const uint16_t gapMs =
                    frames == 0 ? 0 : min<uint32_t>(gapUs / 1000, UINT16_MAX);
                if (frames > 0 && gapMs >= IR_SEQUENCE_SPLIT_GAP_MS) ++banks;
                commandPulseBuffer[used++] = framePulses;
                commandPulseBuffer[used++] = gapMs;
                for (uint16_t i = 0; i < framePulses; ++i) {
                  const uint32_t usec = IrReceiver.irparams.rawbuf[i + 1] * MICROS_PER_TICK;
                  commandPulseBuffer[used++] = min<uint32_t>(usec, UINT16_MAX);
                }
                ++frames;
              }
              IrReceiver.resume();
            }
            delay(2);
          }
          if (frames < 4 || banks < 2) {
            Serial.println("ERR IR sequence needs two separate button presses");
            feedback("error");
            return false;
          }
          CommandHeader header{COMMAND_MAGIC, COMMAND_VERSION, static_cast<uint8_t>(CommandType::IR),
                               IR_DUAL_SEQUENCE_MARKER, frames, carrierKhz, used, 0};
          const bool saved = atomicSaveCommand(commandPath(CommandType::IR, name), header,
                                               commandPulseBuffer);
          if (saved) {
            Serial.printf("OK learned dual ir sequence %s banks=%u frames=%u words=%u duration=%lu ms\n",
                          name.c_str(), banks, frames, used,
                          static_cast<unsigned long>(IR_SEQUENCE_CAPTURE_MS));
            feedback("saved");
            return true;
          }
          Serial.println("ERR cannot save IR sequence");
          feedback("error");
          return false;
        }
        const uint16_t count = rawLength - 1;
        for (uint16_t i = 0; i < count; ++i) {
          const uint32_t usec = IrReceiver.irparams.rawbuf[i + 1] * MICROS_PER_TICK;
          commandPulseBuffer[i] = min<uint32_t>(usec, UINT16_MAX);
        }
        CommandHeader header{COMMAND_MAGIC, COMMAND_VERSION, static_cast<uint8_t>(CommandType::IR),
                             HIGH, max<uint8_t>(repeats, 1), carrierKhz, count, 0};
        const bool saved = atomicSaveCommand(commandPath(CommandType::IR, name), header, commandPulseBuffer);
        const String protocol = getProtocolString(IrReceiver.decodedIRData.protocol);
        const uint16_t bits = IrReceiver.decodedIRData.numberOfBits;
        IrReceiver.resume();
        if (saved) {
          Serial.printf("OK learned ir %s pulses=%u protocol=%s bits=%u repeats=%u carrier=%u kHz\n",
                        name.c_str(), count, protocol.c_str(), bits, header.repeats, carrierKhz);
          feedback("saved");
          return true;
        }
        Serial.println("ERR cannot save IR command");
        feedback("error");
        return false;
      }
      IrReceiver.resume();
    }
    delay(2);
  }
  Serial.println("ERR IR learning timeout");
  feedback("error");
  return false;
}

// --------------------------- Sending -------------------------------------
bool decodeStoredNec(const CommandHeader& header, const uint16_t* pulses,
                     uint16_t& address, uint16_t& command) {
  if (header.carrierKhz < 36 || header.carrierKhz > 40 || header.pulseCount < 67 ||
      pulses[0] < 8000 || pulses[0] > 10000 || pulses[1] < 3500 || pulses[1] > 5500) {
    return false;
  }

  uint32_t data = 0;
  for (uint8_t bit = 0; bit < 32; ++bit) {
    const uint16_t mark = pulses[2 + bit * 2];
    const uint16_t space = pulses[3 + bit * 2];
    if (mark < 350 || mark > 800 || space < 350 || space > 2000) return false;
    if (space > 1000) data |= (1UL << bit);
  }

  const uint8_t addressByte = data & 0xFF;
  const uint8_t invertedAddress = (data >> 8) & 0xFF;
  const uint8_t commandByte = (data >> 16) & 0xFF;
  const uint8_t invertedCommand = (data >> 24) & 0xFF;
  if (static_cast<uint8_t>(addressByte ^ invertedAddress) != 0xFF ||
      static_cast<uint8_t>(commandByte ^ invertedCommand) != 0xFF) {
    return false;
  }
  address = addressByte;
  command = commandByte;
  return true;
}

bool sendCommand(CommandType type, const String& name) {
  CommandHeader header{};
  if (!loadCommand(type, name, header, commandPulseBuffer)) {
    Serial.printf("ERR command not found or corrupt: %s %s\n", typeText(type), name.c_str());
    return false;
  }
  if (type == CommandType::IR) {
    IrReceiver.stop();
    uint16_t necAddress = 0;
    uint16_t necCommand = 0;
    if (header.startLevel == IR_RAW_STREAM_MARKER) {
      if ((header.pulseCount & 1) != 0 || header.pulseCount < IR_RAW_MIN_DURATIONS * 2) {
        IrReceiver.start();
        Serial.println("ERR corrupt raw IR stream");
        return false;
      }
      IrSender.enableIROut(header.carrierKhz);
      const uint16_t durationCount = header.pulseCount / 2;
      for (uint16_t i = 0; i < durationCount; ++i) {
        uint32_t remaining = static_cast<uint32_t>(commandPulseBuffer[i * 2]) |
                             (static_cast<uint32_t>(commandPulseBuffer[i * 2 + 1]) << 16);
        while (remaining > 0) {
          const uint16_t chunk = min<uint32_t>(remaining, 60000);
          if ((i & 1) == 0) IrSender.mark(chunk);
          else IrSender.space(chunk);
          remaining -= chunk;
        }
      }
      Serial.printf("IR raw stream durations=%u\n", durationCount);
    } else if (header.startLevel == IR_SEQUENCE_MARKER ||
        header.startLevel == IR_DUAL_SEQUENCE_MARKER) {
      uint8_t sentFrames = 0;
      uint8_t bankCount = 0;
      const bool dualSequence = header.startLevel == IR_DUAL_SEQUENCE_MARKER;
      bool valid = true;
      const uint8_t passes = dualSequence ? IR_SECOND_BANK_SENDS : 1;
      for (uint8_t pass = 0; pass < passes && valid; ++pass) {
        uint16_t position = 0;
        uint8_t parsedFrames = 0;
        uint8_t bank = 0;
        while (position + 2 <= header.pulseCount) {
          const uint16_t framePulses = commandPulseBuffer[position++];
          const uint16_t gapMs = commandPulseBuffer[position++];
          if (framePulses < 3 || position + framePulses > header.pulseCount) {
            valid = false;
            break;
          }
          const bool bankBoundary = parsedFrames > 0 && gapMs >= IR_SEQUENCE_SPLIT_GAP_MS;
          if (bankBoundary) ++bank;
          // First pass sends both states. Later passes reinforce only the second
          // state, which this television needs for reliable power-off.
          if (pass == 0 || bank > 0) {
            if (sentFrames > 0) {
              delay(bankBoundary ? IR_REPEAT_GAP_MS :
                                   (gapMs > 0 ? gapMs : IR_REPEAT_GAP_MS));
            }
            IrSender.sendRaw(commandPulseBuffer + position, framePulses, header.carrierKhz);
            ++sentFrames;
          }
          position += framePulses;
          ++parsedFrames;
        }
        if (position != header.pulseCount) valid = false;
        bankCount = max<uint8_t>(bankCount, bank + 1);
      }
      if (!valid || sentFrames == 0) {
        IrReceiver.start();
        Serial.println("ERR corrupt IR sequence");
        return false;
      }
      Serial.printf("IR raw sequence banks=%u frames=%u second_bank_sends=%u\n",
                    bankCount, sentFrames, dualSequence ? IR_SECOND_BANK_SENDS : 1);
    } else if (decodeStoredNec(header, commandPulseBuffer, necAddress, necCommand)) {
      // Generate a canonical NEC frame and one correctly timed repeat frame.
      IrSender.sendNEC(necAddress, necCommand, 1);
      Serial.printf("IR NEC address=0x%02X command=0x%02X\n", necAddress, necCommand);
    } else {
      for (uint8_t repeat = 0; repeat < max<uint8_t>(header.repeats, 1); ++repeat) {
        IrSender.sendRaw(commandPulseBuffer, header.pulseCount, header.carrierKhz);
        if (repeat + 1 < header.repeats) delay(IR_REPEAT_GAP_MS);
      }
    }
    IrReceiver.start();
  } else {
    if (!radioReady) {
      Serial.println("ERR CC1101 is not available");
      return false;
    }
    radio.standby();
    pinMode(PIN_CC1101_GDO0, OUTPUT);
    digitalWrite(PIN_CC1101_GDO0, LOW);
    const int16_t state = radio.transmitDirectAsync();
    if (state != RADIOLIB_ERR_NONE) {
      Serial.printf("ERR CC1101 transmitDirectAsync=%d\n", state);
      return false;
    }
    // Allow the synthesizer and PA to settle; otherwise the first RF pulses are truncated.
    delayMicroseconds(1500);
    for (uint8_t repeat = 0; repeat < max<uint8_t>(header.repeats, 1); ++repeat) {
      uint8_t level = header.startLevel;
      for (uint16_t i = 0; i < header.pulseCount; ++i) {
        digitalWrite(PIN_CC1101_GDO0, level);
        delayMicroseconds(commandPulseBuffer[i]);
        level = !level;
      }
      digitalWrite(PIN_CC1101_GDO0, LOW);
      if (repeat + 1 < header.repeats) delay(12);
    }
    delayMicroseconds(1000);
    radio.standby();
    pinMode(PIN_CC1101_GDO0, INPUT);
  }
  Serial.printf("OK sent %s %s\n", typeText(type), name.c_str());
  return true;
}

// --------------------------- Export/import -------------------------------
void exportCommands() {
  Serial.println("EXPORT_BEGIN 1");
  File dir = LittleFS.open("/commands");
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      CommandHeader header{};
      if (entry.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
          header.magic == COMMAND_MAGIC && header.version == COMMAND_VERSION &&
          header.pulseCount > 0 && header.pulseCount <= MAX_PULSES) {
        String filename = String(entry.name());
        const int slash = filename.lastIndexOf('/');
        if (slash >= 0) filename = filename.substring(slash + 1);
        const int underscore = filename.indexOf('_');
        const int extension = filename.lastIndexOf(".brc");
        String name = filename.substring(underscore + 1, extension);
        Serial.printf("EXPORT %s %s %u %u %u %lu %u ",
                      header.type == static_cast<uint8_t>(CommandType::IR) ? "ir" : "rf",
                      name.c_str(), header.carrierKhz, header.startLevel, header.repeats,
                      static_cast<unsigned long>(header.frequencyHz), header.pulseCount);
        for (uint16_t i = 0; i < header.pulseCount; ++i) {
          uint16_t pulse = 0;
          entry.read(reinterpret_cast<uint8_t*>(&pulse), sizeof(pulse));
          if (i) Serial.print(',');
          Serial.print(pulse);
        }
        Serial.println();
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  Serial.println("EXPORT_END");
}

bool importCommand(const String& args) {
  String fields[8];
  int start = 0;
  for (size_t i = 0; i < 7; ++i) {
    const int split = args.indexOf(' ', start);
    if (split < 0) return false;
    fields[i] = args.substring(start, split);
    start = split + 1;
  }
  fields[7] = args.substring(start);
  CommandType type;
  if (!parseType(fields[0], type) || !validName(fields[1])) return false;
  CommandHeader header{COMMAND_MAGIC, COMMAND_VERSION, static_cast<uint8_t>(type),
                       static_cast<uint8_t>(fields[3].toInt()),
                       static_cast<uint8_t>(fields[4].toInt()),
                       static_cast<uint16_t>(fields[2].toInt()),
                       static_cast<uint16_t>(fields[6].toInt()),
                       static_cast<uint32_t>(strtoul(fields[5].c_str(), nullptr, 10))};
  if (header.pulseCount == 0 || header.pulseCount > MAX_PULSES) return false;
  int valueStart = 0;
  for (uint16_t i = 0; i < header.pulseCount; ++i) {
    int comma = fields[7].indexOf(',', valueStart);
    if (i + 1 == header.pulseCount) comma = fields[7].length();
    if (comma < valueStart) return false;
    const long value = fields[7].substring(valueStart, comma).toInt();
    if (value <= 0 || value > UINT16_MAX) return false;
    commandPulseBuffer[i] = static_cast<uint16_t>(value);
    valueStart = comma + 1;
  }
  return atomicSaveCommand(commandPath(type, fields[1]), header, commandPulseBuffer);
}

// --------------------- Universal device registry -----------------------
bool validAutomationText(const String& value, size_t maxLength, bool allowEmpty = false) {
  if ((!allowEmpty && value.isEmpty()) || value.length() > maxLength) return false;
  return value.indexOf('\t') < 0 && value.indexOf('\r') < 0 && value.indexOf('\n') < 0;
}

bool validAutomationCommandList(const String& commands) {
  if (commands.isEmpty() || commands.length() > 196) return false;
  int start = 0;
  uint8_t count = 0;
  while (start < static_cast<int>(commands.length())) {
    int split = commands.indexOf('+', start);
    if (split < 0) split = commands.length();
    if (!validName(commands.substring(start, split)) || ++count > 8) return false;
    start = split + 1;
  }
  return count > 0;
}

bool automationCommandsExist(const String& typeTextValue, const String& commands) {
  CommandType type;
  if (!parseType(typeTextValue, type) || !validAutomationCommandList(commands)) return false;
  int start = 0;
  while (start < static_cast<int>(commands.length())) {
    int split = commands.indexOf('+', start);
    if (split < 0) split = commands.length();
    if (!LittleFS.exists(commandPath(type, commands.substring(start, split)))) return false;
    start = split + 1;
  }
  return true;
}

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 12);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '\\' || c == '"') {
      escaped += '\\';
      escaped += c;
    } else if (static_cast<uint8_t>(c) >= 0x20) {
      escaped += c;
    }
  }
  return escaped;
}

bool parseAutomationLine(const String& line, AutomationRecord& record) {
  String* fields[] = {&record.id, &record.device, &record.room, &record.action,
                      &record.type, &record.command, &record.phrases};
  int start = 0;
  for (size_t i = 0; i < 6; ++i) {
    const int split = line.indexOf('\t', start);
    if (split < 0) return false;
    *fields[i] = line.substring(start, split);
    start = split + 1;
  }
  *fields[6] = line.substring(start);
  return validName(record.id) && validAutomationCommandList(record.command) &&
         (record.type == "ir" || record.type == "rf") &&
         validAutomationText(record.device, 64) &&
         validAutomationText(record.room, 64, true) &&
         validAutomationText(record.action, 64) &&
         validAutomationText(record.phrases, 300);
}

String automationToJson(const AutomationRecord& record) {
  return String("{\"id\":\"") + jsonEscape(record.id) +
         "\",\"device\":\"" + jsonEscape(record.device) +
         "\",\"room\":\"" + jsonEscape(record.room) +
         "\",\"action\":\"" + jsonEscape(record.action) +
         "\",\"type\":\"" + record.type +
         "\",\"command\":\"" + record.command +
         "\",\"phrases\":\"" + jsonEscape(record.phrases) + "\"}";
}

String automationsJson() {
  String json = "[";
  File file = LittleFS.open(AUTOMATION_FILE, "r");
  bool first = true;
  while (file && file.available()) {
    String line = file.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    AutomationRecord record;
    if (!parseAutomationLine(line, record)) continue;
    if (!first) json += ',';
    first = false;
    json += automationToJson(record);
  }
  if (file) file.close();
  json += ']';
  return json;
}

uint8_t automationCount() {
  uint8_t count = 0;
  File file = LittleFS.open(AUTOMATION_FILE, "r");
  while (file && file.available()) {
    AutomationRecord record;
    String line = file.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    if (parseAutomationLine(line, record)) ++count;
  }
  if (file) file.close();
  return count;
}

bool saveAutomation(const AutomationRecord& record) {
  const String temporary = String(AUTOMATION_FILE) + ".tmp";
  File output = LittleFS.open(temporary, "w");
  if (!output) return false;
  File input = LittleFS.open(AUTOMATION_FILE, "r");
  while (input && input.available()) {
    String line = input.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    AutomationRecord current;
    if (parseAutomationLine(line, current) && current.id != record.id) output.println(line);
  }
  if (input) input.close();
  output.printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n", record.id.c_str(),
                record.device.c_str(), record.room.c_str(), record.action.c_str(),
                record.type.c_str(), record.command.c_str(), record.phrases.c_str());
  output.close();
  LittleFS.remove(AUTOMATION_FILE);
  return LittleFS.rename(temporary, AUTOMATION_FILE);
}

bool removeAutomation(const String& id) {
  const String temporary = String(AUTOMATION_FILE) + ".tmp";
  File output = LittleFS.open(temporary, "w");
  if (!output) return false;
  bool found = false;
  File input = LittleFS.open(AUTOMATION_FILE, "r");
  while (input && input.available()) {
    String line = input.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    AutomationRecord current;
    if (parseAutomationLine(line, current) && current.id == id) {
      found = true;
    } else if (!line.isEmpty()) {
      output.println(line);
    }
  }
  if (input) input.close();
  output.close();
  if (!found) {
    LittleFS.remove(temporary);
    return false;
  }
  LittleFS.remove(AUTOMATION_FILE);
  return LittleFS.rename(temporary, AUTOMATION_FILE);
}

bool findAutomation(const String& id, AutomationRecord& found) {
  File file = LittleFS.open(AUTOMATION_FILE, "r");
  while (file && file.available()) {
    String line = file.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    AutomationRecord record;
    if (parseAutomationLine(line, record) && record.id == id) {
      found = record;
      file.close();
      return true;
    }
  }
  if (file) file.close();
  return false;
}

// --------------------------- HTTP interface ------------------------------
static const char WEB_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BroHome Control Hub</title>
<style>
:root{color-scheme:dark;--bg:#10141d;--card:#1a2130;--blue:#56a8ff;--red:#ff6b6b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:#edf3ff;font:16px system-ui,sans-serif}
main{max-width:760px;margin:auto;padding:20px}h1{font-size:1.55rem;margin:0 0 6px}p{color:#abb8cc}
.card{background:var(--card);border-radius:14px;padding:16px;margin:14px 0;box-shadow:0 5px 20px #0004}
.row{display:flex;gap:9px;flex-wrap:wrap}input,select,textarea,button,a.button{border:0;border-radius:9px;padding:11px 13px;font:inherit}
input,select,textarea{background:#0d121c;color:white;flex:1;min-width:130px}textarea{width:100%;min-height:72px;resize:vertical}button,a.button{background:var(--blue);color:#07101c;font-weight:700;cursor:pointer;text-decoration:none}
button.secondary{background:#303b50;color:white}button.danger{background:var(--red);color:#250707}
#status{min-height:24px;color:#dce9ff}.cmd{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:center;border-top:1px solid #ffffff18;padding:11px 0}
.cmd small{color:#92a0b5}.actions{display:flex;gap:7px}.actions button{padding:8px 10px}.empty{color:#8f9bad}
</style></head><body><main>
<h1>BroHome Control Hub</h1><p>Автономный ИК и 433 МГц пульт</p>
<section class="card"><h2>Обучить команду</h2><div class="row">
<select id="type"><option value="ir">ИК-пульт</option><option value="ir_rc6">ИК — сырая запись</option><option value="rf">433 МГц</option></select>
<select id="carrier"><option value="36">36 кГц — RC5/RC6</option><option value="38" selected>38 кГц — NEC и большинство</option><option value="40">40 кГц — Sony</option><option value="56">56 кГц</option></select>
<input id="name" maxlength="48" pattern="[A-Za-z0-9_-]+" placeholder="например box_power">
<button onclick="learn()">Начать обучение</button></div>
<p>Частота сохраняется отдельно для каждой ИК-команды. Для режима «сырая запись» один раз коротко нажмите клавишу оригинального пульта.</p><div id="status"></div></section>
<section class="card"><div class="row"><button class="secondary" onclick="loadCommands()">Обновить список</button>
<a class="button" href="/api/backup">Скачать backup JSON</a></div><div id="commands"></div></section>
<section class="card"><h2>Устройства и голосовые действия</h2>
<p>Привяжите сохранённую ИК/RF-команду к устройству. BroHome автоматически получит изменения.</p>
<input id="automationId" type="hidden"><div class="row">
<input id="device" maxlength="64" placeholder="Устройство, например Карниз">
<input id="room" maxlength="64" placeholder="Комната, например Кухня">
<input id="action" maxlength="64" placeholder="Действие, например Открыть"></div>
<div class="row"><select id="command" multiple size="6"></select></div>
<p>Можно выбрать несколько команд одного типа (Ctrl/Command или долгое нажатие).</p>
<textarea id="phrases" maxlength="300" placeholder="Голосовые фразы через запятую: открой шторы, открой карниз на кухне"></textarea>
<div class="row"><button onclick="saveAutomation()">Сохранить действие</button>
<button class="secondary" onclick="clearAutomationForm()">Очистить</button></div>
<div id="automations"></div></section>
</main><script>
const statusEl=document.getElementById('status');
let learnedCommands=[];
function params(type,name){return new URLSearchParams({type,name});}
function syncCarrier(){let type=document.getElementById('type').value,carrier=document.getElementById('carrier');carrier.disabled=type==='rf';if(type==='ir_rc6')carrier.value='36';else if(type==='ir')carrier.value='38'}
async function request(url,options){let r=await fetch(url,options);let text=await r.text();if(!r.ok)throw Error(text);return text;}
async function learn(){let type=document.getElementById('type').value,name=document.getElementById('name').value.trim();
 if(!/^[A-Za-z0-9_-]{1,48}$/.test(name)){statusEl.textContent='Имя: только латинские буквы, цифры, _ и -';return}
 statusEl.textContent='Запускаю обучение…';
  let carrier=document.getElementById('carrier').value;
  try{await request('/api/learn?'+new URLSearchParams({type,name,carrier}),{method:'POST'});statusEl.textContent=type==='ir_rc6'?'Слушаю сырой поток — один раз коротко нажмите кнопку пульта…':'Слушаю эфир — сейчас нажмите кнопку оригинального пульта…';
  for(let i=0;i<50;i++){await new Promise(r=>setTimeout(r,500));try{let job=await (await fetch('/api/learn-status')).json();
   if(job.state==='running')continue;if(job.state==='success'){statusEl.textContent=job.message;await loadCommands();return}
   if(job.state==='error'){statusEl.textContent='Ошибка: '+job.message;return}}catch(e){/* Wi-Fi may be briefly busy; keep polling. */}}
  statusEl.textContent='Нет ответа о результате обучения';
 }catch(e){statusEl.textContent='Ошибка запуска: '+e.message}}
async function send(type,name){statusEl.textContent='Отправляю '+name+'…';try{statusEl.textContent=await request('/api/send?'+params(type,name),{method:'POST'})}catch(e){statusEl.textContent='Ошибка: '+e.message}}
async function removeCommand(type,name){if(!confirm('Удалить '+name+'?'))return;try{statusEl.textContent=await request('/api/remove?'+params(type,name),{method:'POST'});await loadCommands()}catch(e){statusEl.textContent='Ошибка: '+e.message}}
async function loadCommands(){let box=document.getElementById('commands');try{let rows=await (await fetch('/api/commands')).json();learnedCommands=rows;box.innerHTML='';
 let select=document.getElementById('command'),chosen=Array.from(select.selectedOptions).map(o=>o.value);select.innerHTML='';rows.forEach(c=>{let o=document.createElement('option');o.value=c.type+':'+c.name;o.textContent=c.type.toUpperCase()+' — '+c.name;o.selected=chosen.includes(o.value);select.append(o)});
 if(!rows.length){box.innerHTML='<p class="empty">Команд пока нет</p>';return}rows.forEach(c=>{let d=document.createElement('div');d.className='cmd';
 d.innerHTML='<div><b>'+c.name+'</b><br><small>'+c.type.toUpperCase()+(c.type==='ir'?' · '+c.carrier_khz+' кГц':'')+' · '+c.pulses+' импульсов'+(c.repeats>1?' · '+c.repeats+' кадра':'')+'</small></div>';
 let a=document.createElement('div');a.className='actions';let s=document.createElement('button');s.textContent='Отправить';s.onclick=()=>send(c.type,c.name);
 let x=document.createElement('button');x.textContent='Удалить';x.className='danger';x.onclick=()=>removeCommand(c.type,c.name);a.append(s,x);d.append(a);box.append(d)})
 }catch(e){box.textContent='Не удалось загрузить список: '+e.message}}
function clearAutomationForm(){['automationId','device','room','action','phrases'].forEach(id=>document.getElementById(id).value='')}
async function saveAutomation(){let selected=Array.from(document.getElementById('command').selectedOptions).map(o=>o.value);if(!selected.length){statusEl.textContent='Сначала выберите команду';return}
 let parsed=selected.map(v=>{let i=v.indexOf(':');return{type:v.slice(0,i),name:v.slice(i+1)}}),types=new Set(parsed.map(v=>v.type));if(types.size!==1){statusEl.textContent='В одном действии команды должны быть одного типа';return}
 let body=new URLSearchParams({id:document.getElementById('automationId').value,device:document.getElementById('device').value.trim(),room:document.getElementById('room').value.trim(),action:document.getElementById('action').value.trim(),phrases:document.getElementById('phrases').value.trim(),type:parsed[0].type,command:parsed.map(v=>v.name).join('+')});
 try{await request('/api/automation/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});statusEl.textContent='Действие сохранено и будет передано в BroHome';clearAutomationForm();await loadAutomations()}catch(e){statusEl.textContent='Ошибка: '+e.message}}
function editAutomation(a){document.getElementById('automationId').value=a.id;document.getElementById('device').value=a.device;document.getElementById('room').value=a.room;document.getElementById('action').value=a.action;document.getElementById('phrases').value=a.phrases;let wanted=a.command.split('+').map(c=>a.type+':'+c);Array.from(document.getElementById('command').options).forEach(o=>o.selected=wanted.includes(o.value));scrollTo({top:document.body.scrollHeight,behavior:'smooth'})}
async function runAutomation(id){statusEl.textContent='Выполняю действие…';try{statusEl.textContent=await request('/api/automation/run?id='+encodeURIComponent(id),{method:'POST'})}catch(e){statusEl.textContent='Ошибка: '+e.message}}
async function removeAutomation(id){if(!confirm('Удалить голосовое действие?'))return;try{await request('/api/automation/remove?id='+encodeURIComponent(id),{method:'POST'});await loadAutomations()}catch(e){statusEl.textContent='Ошибка: '+e.message}}
async function loadAutomations(){let box=document.getElementById('automations');try{let data=await (await fetch('/api/automations')).json();box.innerHTML='';if(!data.actions.length){box.innerHTML='<p class="empty">Голосовых действий пока нет</p>';return}data.actions.forEach(a=>{let d=document.createElement('div');d.className='cmd';let info=document.createElement('div'),title=document.createElement('b');title.textContent=a.device+' — '+a.action;let detail=document.createElement('small');detail.textContent=(a.room?a.room+' · ':'')+a.type.toUpperCase()+' '+a.command+' · '+a.phrases;info.append(title,document.createElement('br'),detail);let controls=document.createElement('div');controls.className='actions';let test=document.createElement('button');test.textContent='Тест';test.onclick=()=>runAutomation(a.id);let edit=document.createElement('button');edit.textContent='Изменить';edit.className='secondary';edit.onclick=()=>editAutomation(a);let del=document.createElement('button');del.textContent='Удалить';del.className='danger';del.onclick=()=>removeAutomation(a.id);controls.append(test,edit,del);d.append(info,controls);box.append(d)})}catch(e){box.textContent='Не удалось загрузить действия: '+e.message}}
document.getElementById('type').addEventListener('change',syncCarrier);syncCarrier();
Promise.all([loadCommands(),loadAutomations()]);
</script></body></html>
)HTML";

bool commandMetadata(File& entry, CommandHeader& header, String& name, CommandType& type) {
  if (entry.isDirectory() ||
      entry.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
      header.magic != COMMAND_MAGIC || header.version != COMMAND_VERSION ||
      header.pulseCount == 0 || header.pulseCount > MAX_PULSES) return false;
  if (header.type == static_cast<uint8_t>(CommandType::IR)) type = CommandType::IR;
  else if (header.type == static_cast<uint8_t>(CommandType::RF)) type = CommandType::RF;
  else return false;
  String filename = entry.name();
  const int slash = filename.lastIndexOf('/');
  if (slash >= 0) filename = filename.substring(slash + 1);
  const int underscore = filename.indexOf('_');
  const int extension = filename.lastIndexOf(".brc");
  if (underscore < 0 || extension <= underscore) return false;
  name = filename.substring(underscore + 1, extension);
  return validName(name);
}

bool getHttpCommand(CommandType& type, String& name) {
  name = webServer.arg("name");
  return parseType(webServer.arg("type"), type) && validName(name);
}

void handleHttpCommands() {
  String json = "[";
  bool first = true;
  File dir = LittleFS.open("/commands");
  File entry = dir.openNextFile();
  while (entry) {
    CommandHeader header{};
    String name;
    CommandType type;
    if (commandMetadata(entry, header, name, type)) {
      if (!first) json += ',';
      first = false;
      json += "{\"type\":\"" + String(typeText(type)) + "\",\"name\":\"" + name +
              "\",\"pulses\":" + String(header.pulseCount) +
              ",\"repeats\":" + String(max<uint8_t>(header.repeats, 1)) +
              ",\"carrier_khz\":" + String(header.carrierKhz) + "}";
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  json += ']';
  webServer.send(200, "application/json; charset=utf-8", json);
}

void handleHttpAutomations() {
  const String json = String("{\"version\":1,\"hub\":\"") + DEVICE_NAME +
                      "\",\"actions\":" + automationsJson() + "}";
  webServer.send(200, "application/json; charset=utf-8", json);
}

void handleHttpAutomationSave() {
  AutomationRecord record;
  record.id = webServer.arg("id");
  if (record.id.isEmpty()) {
    char generated[20];
    snprintf(generated, sizeof(generated), "auto_%08lx",
             static_cast<unsigned long>(esp_random()));
    record.id = generated;
  }
  record.device = webServer.arg("device");
  record.room = webServer.arg("room");
  record.action = webServer.arg("action");
  record.type = webServer.arg("type");
  record.command = webServer.arg("command");
  record.phrases = webServer.arg("phrases");
  record.device.trim();
  record.room.trim();
  record.action.trim();
  record.phrases.trim();

  const bool valid = validName(record.id) &&
                     validAutomationText(record.device, 64) &&
                     validAutomationText(record.room, 64, true) &&
                     validAutomationText(record.action, 64) &&
                     validAutomationText(record.phrases, 300) &&
                     automationCommandsExist(record.type, record.command);
  if (!valid) {
    webServer.send(400, "text/plain; charset=utf-8",
                   "Проверьте поля и привязанную команду");
    return;
  }
  if (automationCount() >= MAX_AUTOMATIONS && webServer.arg("id").isEmpty()) {
    webServer.send(409, "text/plain; charset=utf-8", "Достигнут лимит действий");
    return;
  }
  if (!saveAutomation(record)) {
    webServer.send(500, "text/plain; charset=utf-8", "Не удалось сохранить действие");
    return;
  }
  webServer.send(200, "text/plain; charset=utf-8", record.id);
}

void handleHttpAutomationRemove() {
  const String id = webServer.arg("id");
  if (!validName(id)) {
    webServer.send(400, "text/plain; charset=utf-8", "Некорректный идентификатор");
    return;
  }
  const bool removed = removeAutomation(id);
  webServer.send(removed ? 200 : 404, "text/plain; charset=utf-8",
                 removed ? "Действие удалено" : "Действие не найдено");
}

void handleHttpAutomationRun() {
  const String id = webServer.arg("id");
  AutomationRecord record;
  if (!validName(id) || !findAutomation(id, record)) {
    webServer.send(404, "text/plain; charset=utf-8", "Действие не найдено");
    return;
  }
  CommandType type;
  if (!parseType(record.type, type) || !automationCommandsExist(record.type, record.command)) {
    webServer.send(409, "text/plain; charset=utf-8", "Связанная команда недоступна");
    return;
  }
  int start = 0;
  bool ok = true;
  while (start < static_cast<int>(record.command.length())) {
    int split = record.command.indexOf('+', start);
    if (split < 0) split = record.command.length();
    ok = sendCommand(type, record.command.substring(start, split)) && ok;
    start = split + 1;
    if (start < static_cast<int>(record.command.length())) delay(150);
  }
  webServer.send(ok ? 200 : 500, "text/plain; charset=utf-8",
                 ok ? "Действие выполнено" : "Не все команды выполнены");
}

void learningTask(void*) {
  const String name(learnJobName);
  const bool ok = learnJobType == CommandType::IR
                      ? learnIr(name, learnJobRepeats, learnJobCaptureSequence, learnJobCarrierKhz)
                      : learnRf(name);
  strlcpy(learnJobMessage, ok ? "Команда сохранена" : "Сигнал не получен — повторите обучение",
          sizeof(learnJobMessage));
  learnJobState = ok ? LearnJobState::Success : LearnJobState::Error;
  if (!ok) {
    statusError();
    delay(700);
  }
  if (broHomeClient.connected()) statusListening();
  else statusBroHomeOffline();
  vTaskDelete(nullptr);
}

void handleHttpLearn() {
  CommandType type;
  const String requestedType = webServer.arg("type");
  const String name = webServer.arg("name");
  uint8_t repeats = 1;
  bool captureSequence = false;
  uint16_t carrierKhz = 38;
  if (requestedType == "ir_rc6") {
    type = CommandType::IR;
    captureSequence = true;
    carrierKhz = 36;
  } else if (!parseType(requestedType, type)) {
    webServer.send(400, "text/plain; charset=utf-8", "Некорректный тип или имя команды");
    return;
  }
  if (!validName(name)) {
    webServer.send(400, "text/plain; charset=utf-8", "Некорректный тип или имя команды");
    return;
  }
  if (type == CommandType::IR && webServer.hasArg("carrier")) {
    carrierKhz = static_cast<uint16_t>(webServer.arg("carrier").toInt());
  }
  if (type == CommandType::IR && !validIrCarrier(carrierKhz)) {
    webServer.send(400, "text/plain; charset=utf-8", "Выберите частоту 36, 38, 40 или 56 кГц");
    return;
  }
  if (learnJobState == LearnJobState::Running) {
    webServer.send(409, "text/plain; charset=utf-8", "Другое обучение уже выполняется");
    return;
  }
  learnJobType = type;
  learnJobRepeats = repeats;
  learnJobCaptureSequence = captureSequence;
  learnJobCarrierKhz = carrierKhz;
  strlcpy(learnJobName, name.c_str(), sizeof(learnJobName));
  strlcpy(learnJobMessage, "Ожидание сигнала", sizeof(learnJobMessage));
  learnJobState = LearnJobState::Running;
  statusLearning();
  const BaseType_t created = xTaskCreatePinnedToCore(
      learningTask, "remoteLearn", 8192, nullptr, 1, nullptr, 1);
  if (created != pdPASS) {
    learnJobState = LearnJobState::Error;
    strlcpy(learnJobMessage, "Не удалось создать задачу обучения", sizeof(learnJobMessage));
    webServer.send(500, "text/plain; charset=utf-8", learnJobMessage);
    return;
  }
  webServer.send(202, "text/plain; charset=utf-8", "Обучение запущено");
}

void handleHttpLearnStatus() {
  const char* state = "idle";
  if (learnJobState == LearnJobState::Running) state = "running";
  else if (learnJobState == LearnJobState::Success) state = "success";
  else if (learnJobState == LearnJobState::Error) state = "error";
  const String json = String("{\"state\":\"") + state + "\",\"message\":\"" +
                      learnJobMessage + "\"}";
  webServer.send(200, "application/json; charset=utf-8", json);
}

void handleHttpSend() {
  if (learnJobState == LearnJobState::Running) {
    webServer.send(409, "text/plain; charset=utf-8", "Дождитесь окончания обучения");
    return;
  }
  CommandType type;
  String name;
  if (!getHttpCommand(type, name)) {
    webServer.send(400, "text/plain; charset=utf-8", "Некорректный тип или имя команды");
    return;
  }
  const bool ok = sendCommand(type, name);
  webServer.send(ok ? 200 : 404, "text/plain; charset=utf-8",
                 ok ? "Команда отправлена" : "Команда не найдена или модуль недоступен");
}

void handleHttpRemove() {
  if (learnJobState == LearnJobState::Running) {
    webServer.send(409, "text/plain; charset=utf-8", "Дождитесь окончания обучения");
    return;
  }
  CommandType type;
  String name;
  if (!getHttpCommand(type, name)) {
    webServer.send(400, "text/plain; charset=utf-8", "Некорректный тип или имя команды");
    return;
  }
  const bool ok = LittleFS.remove(commandPath(type, name));
  webServer.send(ok ? 200 : 404, "text/plain; charset=utf-8", ok ? "Команда удалена" : "Команда не найдена");
}

void handleHttpBackup() {
  webServer.sendHeader("Content-Disposition", "attachment; filename=brohome-remotes.json");
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "application/json; charset=utf-8", "");
  webServer.sendContent("{\"format\":\"brohome-remote-backup\",\"version\":1,\"commands\":[");
  bool firstCommand = true;
  File dir = LittleFS.open("/commands");
  File entry = dir.openNextFile();
  while (entry) {
    CommandHeader header{};
    String name;
    CommandType type;
    if (commandMetadata(entry, header, name, type)) {
      String prefix = firstCommand ? "" : ",";
      firstCommand = false;
      prefix += "{\"type\":\"" + String(typeText(type)) + "\",\"name\":\"" + name +
                "\",\"carrier_khz\":" + String(header.carrierKhz) +
                ",\"start_level\":" + String(header.startLevel) +
                ",\"repeats\":" + String(header.repeats) +
                ",\"frequency_hz\":" + String(header.frequencyHz) + ",\"pulses_us\":[";
      webServer.sendContent(prefix);
      for (uint16_t i = 0; i < header.pulseCount; ++i) {
        uint16_t pulse = 0;
        entry.read(reinterpret_cast<uint8_t*>(&pulse), sizeof(pulse));
        webServer.sendContent((i ? "," : "") + String(pulse));
      }
      webServer.sendContent("]}");
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  webServer.sendContent("],\"automations\":");
  webServer.sendContent(automationsJson());
  webServer.sendContent("}");
  webServer.sendContent("");
}

void startWebInterface() {
  const bool stationReady = WiFi.status() == WL_CONNECTED;
  bool accessPointReady = false;
  if (!stationReady) {
    WiFi.mode(WIFI_AP_STA);
    accessPointReady = WiFi.softAP(AP_SSID, AP_PASSWORD, 6, false, 2);
  }
  webServer.on("/", HTTP_GET, []() { webServer.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  webServer.on("/api/commands", HTTP_GET, handleHttpCommands);
  webServer.on("/api/automations", HTTP_GET, handleHttpAutomations);
  webServer.on("/api/automation/save", HTTP_POST, handleHttpAutomationSave);
  webServer.on("/api/automation/remove", HTTP_POST, handleHttpAutomationRemove);
  webServer.on("/api/automation/run", HTTP_POST, handleHttpAutomationRun);
  webServer.on("/api/learn", HTTP_POST, handleHttpLearn);
  webServer.on("/api/learn-status", HTTP_GET, handleHttpLearnStatus);
  webServer.on("/api/send", HTTP_POST, handleHttpSend);
  webServer.on("/api/remove", HTTP_POST, handleHttpRemove);
  webServer.on("/api/backup", HTTP_GET, handleHttpBackup);
  webServer.on("/favicon.ico", HTTP_GET, []() { webServer.send(204); });
  webServer.onNotFound([]() { webServer.sendHeader("Location", "/"); webServer.send(302); });
  webServer.begin();
  const String address = stationReady ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("HTTP: OK, mode=%s, IP=%s\n",
                stationReady ? "home-wifi" : (accessPointReady ? "maintenance-ap" : "error"),
                address.c_str());
}

// --------------------------- Serial CLI ----------------------------------
void printHelp() {
  Serial.println("BroHome control hub commands:");
  Serial.println("  learn ir|rf NAME");
  Serial.println("  send ir|rf NAME");
  Serial.println("  list");
  Serial.println("  remove ir|rf NAME");
  Serial.println("  play AUDIO_NAME");
  Serial.println("  mic_test");
  Serial.println("  export");
  Serial.println("  import TYPE NAME CARRIER START REPEATS FREQ COUNT p1,p2,...");
  Serial.println("  audio_begin NAME SIZE / audio_chunk HEX / audio_end");
  Serial.println("  status");
}

void listCommands() {
  File dir = LittleFS.open("/commands");
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) Serial.printf("COMMAND %s %u\n", entry.name(), entry.size());
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  Serial.println("OK list complete");
}

void handleLine(String line) {
  line.trim();
  if (line.isEmpty()) return;
  const int firstSpace = line.indexOf(' ');
  String command = firstSpace < 0 ? line : line.substring(0, firstSpace);
  String args = firstSpace < 0 ? "" : line.substring(firstSpace + 1);
  command.toLowerCase();

  if (command == "help") {
    printHelp();
  } else if (command == "learn" || command == "send" || command == "remove") {
    const int split = args.indexOf(' ');
    CommandType type;
    const String name = split < 0 ? "" : args.substring(split + 1);
    if (split < 0 || !parseType(args.substring(0, split), type) || !validName(name)) {
      Serial.println("ERR usage: learn|send|remove ir|rf NAME");
    } else if (command == "learn") {
      if (type == CommandType::IR) learnIr(name); else learnRf(name);
    } else if (command == "send") {
      sendCommand(type, name);
    } else {
      const bool removed = LittleFS.remove(commandPath(type, name));
      Serial.println(removed ? "OK removed" : "ERR command not found");
    }
  } else if (command == "list") {
    listCommands();
  } else if (command == "play") {
    Serial.println(playWav(args) ? "OK played" : "ERR WAV missing or unsupported");
  } else if (command == "mic_test") {
    Serial.println(testMicrophone() ? "OK microphone sampled" : "ERR microphone unavailable");
  } else if (command == "export") {
    exportCommands();
  } else if (command == "import") {
    Serial.println(importCommand(args) ? "OK imported" : "ERR invalid import record");
  } else if (command == "audio_begin") {
    const int split = args.indexOf(' ');
    const String name = split < 0 ? "" : args.substring(0, split);
    audioUploadExpected = split < 0 ? 0 : strtoul(args.substring(split + 1).c_str(), nullptr, 10);
    if (audioUpload) audioUpload.close();
    if (!validName(name) || audioUploadExpected == 0 || audioUploadExpected > 3000000) {
      Serial.println("ERR invalid audio_begin");
    } else {
      audioUpload = LittleFS.open(audioPath(name) + ".tmp", FILE_WRITE);
      audioUploadWritten = 0;
      Serial.println(audioUpload ? "OK audio ready" : "ERR cannot open audio file");
    }
  } else if (command == "audio_chunk") {
    if (!audioUpload || args.length() == 0 || args.length() % 2 != 0) {
      Serial.println("ERR no audio upload or invalid hex");
    } else {
      bool ok = true;
      for (size_t i = 0; i < args.length(); i += 2) {
        char pair[3] = {args[i], args[i + 1], 0};
        char* end = nullptr;
        const long value = strtol(pair, &end, 16);
        if (!end || *end != 0 || audioUpload.write(static_cast<uint8_t>(value)) != 1) {
          ok = false;
          break;
        }
        ++audioUploadWritten;
      }
      Serial.println(ok ? "OK audio chunk" : "ERR audio write");
    }
  } else if (command == "audio_end") {
    if (!audioUpload) {
      Serial.println("ERR no audio upload");
    } else {
      const String temp = String(audioUpload.path());
      audioUpload.flush();
      audioUpload.close();
      const String finalPath = temp.substring(0, temp.length() - 4);
      if (audioUploadWritten == audioUploadExpected) {
        LittleFS.remove(finalPath);
        Serial.println(LittleFS.rename(temp, finalPath) ? "OK audio saved" : "ERR audio rename");
      } else {
        LittleFS.remove(temp);
        Serial.printf("ERR audio size expected=%u got=%u\n", audioUploadExpected, audioUploadWritten);
      }
    }
  } else if (command == "status") {
    Serial.printf("STATUS flash=%lu psram=%lu fs_total=%u fs_used=%u cc1101=%s commands_version=%u\n",
                  static_cast<unsigned long>(ESP.getFlashChipSize()),
                  static_cast<unsigned long>(ESP.getPsramSize()), LittleFS.totalBytes(), LittleFS.usedBytes(),
                  radioReady ? "ok" : "error", COMMAND_VERSION);
  } else {
    Serial.println("ERR unknown command; type help");
  }
}

void serviceSerial() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n') {
      handleLine(serialLine);
      serialLine = "";
    } else if (c != '\r') {
      if (serialLine.length() < 16000) serialLine += c;
      else serialLine = "";
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  statusBoot();
  Serial.println("\nBroHome control hub booting...");

  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
    Serial.println("FATAL LittleFS mount failed");
    statusError();
    while (true) delay(1000);
  }
  removeLegacyAudioFiles();
  ensureDirectories();

  IrSender.begin(PIN_IR_TX);
  IrReceiver.begin(PIN_IR_RX, DISABLE_LED_FEEDBACK);

  radioSpi.begin(PIN_CC1101_SCK, PIN_CC1101_MISO, PIN_CC1101_MOSI, PIN_CC1101_CS);
  int16_t radioState = radio.begin(433.92, 4.8, 5.0, 270.0, 10, 16);
  if (radioState == RADIOLIB_ERR_NONE) radioState = radio.setOOK(true);
  radioReady = radioState == RADIOLIB_ERR_NONE;
  if (radioReady) radio.standby();
  Serial.printf("CC1101: %s (%d)\n", radioReady ? "OK" : "ERROR", radioState);

  const bool audioReady = initAudio();
  const bool microphoneReady = initMicrophone();
  Serial.printf("Audio: %s\n", audioReady ? "OK" : "ERROR");
  Serial.printf("Microphone: %s\n", microphoneReady ? "OK" : "ERROR");
  connectHomeWiFi(15000);
  startWebInterface();
  serialLine.reserve(16000);
  printHelp();

  if (!audioReady || !microphoneReady) {
    statusError();
    return;
  }
  microphoneQueue = xQueueCreate(24, sizeof(MicrophoneBlock));
  if (!microphoneQueue) {
    Serial.println("FATAL microphone queue allocation failed");
    statusError();
    return;
  }
  xTaskCreatePinnedToCore(microphoneTask, "microphone", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(broHomeNetworkTask, "brohomeNet", 6144, nullptr, 2, nullptr, 0);
}

void loop() {
  serviceSerial();
  webServer.handleClient();
  delay(2);
}
