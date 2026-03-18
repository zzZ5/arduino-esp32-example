#include "sensor.h"

#include <DHT.h>

namespace {

  // Keep private sensor drivers in this file so the public API stays small.
  class MHZ16Sensor {
  public:
    struct Data {
      uint16_t co2ppm;
    };

    MHZ16Sensor(HardwareSerial& serial, int rxPin, int txPin)
      : serial_(&serial), rxPin_(rxPin), txPin_(txPin) {
    }

    bool begin(uint32_t baud = 9600) {
      serial_->begin(baud, SERIAL_8N1, rxPin_, txPin_);
      updateCommandChecksum(cmdReadCO2_, sizeof(cmdReadCO2_));
      delay(200);
      clearBuffer();
      return true;
    }

    bool readCO2(uint16_t& co2ppm) {
      Data data{};
      if (!readData(data)) {
        return false;
      }
      co2ppm = data.co2ppm;
      return true;
    }

  private:
    static constexpr uint8_t kFrameLen = 9;

    HardwareSerial* serial_;
    int rxPin_;
    int txPin_;
    uint8_t cmdReadCO2_[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    bool readData(Data& data) {
      uint8_t frame[kFrameLen] = { 0 };
      clearBuffer();
      sendCommand(cmdReadCO2_, sizeof(cmdReadCO2_));
      if (!readFrame(frame, kFrameLen, 500)) {
        return false;
      }
      return parseFrame(frame, data);
    }

    void clearBuffer() {
      while (serial_->available()) {
        serial_->read();
      }
    }

    void sendCommand(const uint8_t* cmd, uint8_t len) {
      serial_->write(cmd, len);
      serial_->flush();
    }

    bool readFrame(uint8_t* buffer, uint8_t len, uint32_t timeoutMs) {
      uint8_t index = 0;
      unsigned long startTime = millis();
      while (millis() - startTime < timeoutMs) {
        if (!serial_->available()) {
          delay(1);
          continue;
        }
        uint8_t b = serial_->read();
        if (index == 0 && b != 0xFF) {
          continue;
        }
        buffer[index++] = b;
        if (index >= len) {
          return true;
        }
      }
      return false;
    }

    uint8_t calcChecksum(const uint8_t* data, uint8_t len) {
      uint8_t sum = 0;
      for (uint8_t i = 1; i < len - 1; ++i) {
        sum += data[i];
      }
      return static_cast<uint8_t>((~sum) + 1);
    }

    void updateCommandChecksum(uint8_t* cmd, uint8_t len) {
      cmd[len - 1] = calcChecksum(cmd, len);
    }

    bool parseFrame(const uint8_t* frame, Data& data) {
      if (frame[0] != 0xFF || frame[1] != 0x86) {
        return false;
      }
      if (calcChecksum(frame, kFrameLen) != frame[8]) {
        return false;
      }
      data.co2ppm = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
      return true;
    }
  };

  class ZCE04BSensor {
  public:
    struct GasData {
      float co;
      float h2s;
      float o2;
      float ch4;
    };

    ZCE04BSensor(HardwareSerial& serial, int rxPin, int txPin)
      : serial_(&serial), rxPin_(rxPin), txPin_(txPin) {
    }

    bool begin(uint32_t baud = 9600) {
      serial_->begin(baud, SERIAL_8N1, rxPin_, txPin_);
      updateCommandChecksum(cmdSetQueryMode_, sizeof(cmdSetQueryMode_));
      updateCommandChecksum(cmdReadGas_, sizeof(cmdReadGas_));
      delay(200);
      return setQueryMode();
    }

    bool readGasData(GasData& gas) {
      uint8_t frame[kFrameLen] = { 0 };
      clearBuffer();
      sendCommand(cmdReadGas_, sizeof(cmdReadGas_));
      if (!readFrame(frame, kFrameLen, 500)) {
        return false;
      }
      return parseFrame(frame, gas);
    }

  private:
    static constexpr uint8_t kFrameLen = 11;
    static constexpr float kCOResolution = 1.0f;
    static constexpr float kH2SResolution = 1.0f;
    static constexpr float kO2Resolution = 0.1f;
    static constexpr float kCH4Resolution = 1.0f;

    HardwareSerial* serial_;
    int rxPin_;
    int txPin_;
    uint8_t cmdSetQueryMode_[9] = { 0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t cmdReadGas_[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    bool setQueryMode() {
      clearBuffer();
      sendCommand(cmdSetQueryMode_, sizeof(cmdSetQueryMode_));
      delay(500);
      clearBuffer();
      return true;
    }

    void clearBuffer() {
      while (serial_->available()) {
        serial_->read();
      }
    }

    void sendCommand(const uint8_t* cmd, uint8_t len) {
      serial_->write(cmd, len);
      serial_->flush();
    }

    bool readFrame(uint8_t* buffer, uint8_t len, uint32_t timeoutMs) {
      uint8_t index = 0;
      unsigned long startTime = millis();
      while (millis() - startTime < timeoutMs) {
        if (!serial_->available()) {
          delay(1);
          continue;
        }
        uint8_t b = serial_->read();
        if (index == 0 && b != 0xFF) {
          continue;
        }
        buffer[index++] = b;
        if (index >= len) {
          return true;
        }
      }
      return false;
    }

    uint8_t calcChecksum(const uint8_t* data, uint8_t len) {
      uint8_t sum = 0;
      for (uint8_t i = 1; i < len - 1; ++i) {
        sum += data[i];
      }
      return static_cast<uint8_t>((~sum) + 1);
    }

    void updateCommandChecksum(uint8_t* cmd, uint8_t len) {
      cmd[len - 1] = calcChecksum(cmd, len);
    }

    bool parseFrame(const uint8_t* frame, GasData& gas) {
      if (frame[0] != 0xFF || frame[1] != 0x86) {
        return false;
      }
      if (calcChecksum(frame, kFrameLen) != frame[10]) {
        return false;
      }

      const uint16_t coRaw = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
      const uint16_t h2sRaw = (static_cast<uint16_t>(frame[4]) << 8) | frame[5];
      const uint16_t o2Raw = (static_cast<uint16_t>(frame[6]) << 8) | frame[7];
      const uint16_t ch4Raw = (static_cast<uint16_t>(frame[8]) << 8) | frame[9];

      gas.co = coRaw * kCOResolution;
      gas.h2s = h2sRaw * kH2SResolution;
      gas.o2 = o2Raw * kO2Resolution;
      gas.ch4 = ch4Raw * kCH4Resolution;
      return true;
    }
  };

  class DHT21SensorImpl {
  public:
    struct Data {
      float temperature;
      float humidity;
    };

    explicit DHT21SensorImpl(uint8_t pin) : dht_(pin, DHT21) {}

    bool begin() {
      dht_.begin();
      delay(1000);   // DHT21 上电后建议等待 1 s
      failCount_ = 0;
      return true;
    }

    bool readData(Data& data) {
      if (!canReadNow() && hasCache_) {
        data = lastData_;
        return true;
      }

      float humidity = dht_.readHumidity(false);
      float temperature = dht_.readTemperature(false, false);

      // 第一次失败后，强制重读一次
      if (isnan(humidity) || isnan(temperature)) {
        delay(50);
        humidity = dht_.readHumidity(true);
        temperature = dht_.readTemperature(false, true);
      }

      if (isnan(humidity) || isnan(temperature)) {
        failCount_++;

        // 可选：短时失败时直接返回上次有效值
        if (hasCache_) {
          data = lastData_;
        }

        // 连续失败过多，重新初始化
        if (failCount_ >= 3) {
          dht_.begin();
          delay(1000);
          failCount_ = 0;
        }
        return false;
      }

      failCount_ = 0;
      lastReadMs_ = millis();
      lastData_ = { temperature, humidity };
      hasCache_ = true;
      data = lastData_;
      return true;
    }

  private:
    static constexpr uint32_t kMinReadIntervalMs = 2000;

    DHT dht_;
    unsigned long lastReadMs_ = 0;
    bool hasCache_ = false;
    uint8_t failCount_ = 0;
    Data lastData_{ NAN, NAN };

    bool canReadNow() const {
      return (millis() - lastReadMs_) >= kMinReadIntervalMs;
    }
  };

  MHZ16Sensor* g_mhz16 = nullptr;
  ZCE04BSensor* g_zce04b = nullptr;
  DHT21SensorImpl* g_dht21 = nullptr;

  int g_exhaustPin = -1;
  int g_aerationPin = -1;

  void destroySensors() {
    delete g_mhz16;
    delete g_zce04b;
    delete g_dht21;

    g_mhz16 = nullptr;
    g_zce04b = nullptr;
    g_dht21 = nullptr;
  }

}  // namespace

bool initSensorAndPump(
  int exhaustPin,
  int aerationPin,
  HardwareSerial& mhzSerial,
  int mhzRxPin,
  int mhzTxPin,
  HardwareSerial& zceSerial,
  int zceRxPin,
  int zceTxPin,
  uint8_t dhtPin,
  unsigned long timeoutMs) {
  unsigned long startMs = millis();

  destroySensors();

  g_exhaustPin = exhaustPin;
  g_aerationPin = aerationPin;

  pinMode(g_exhaustPin, OUTPUT);
  digitalWrite(g_exhaustPin, HIGH);

  pinMode(g_aerationPin, OUTPUT);
  digitalWrite(g_aerationPin, LOW);

  g_mhz16 = new MHZ16Sensor(mhzSerial, mhzRxPin, mhzTxPin);
  g_zce04b = new ZCE04BSensor(zceSerial, zceRxPin, zceTxPin);
  g_dht21 = new DHT21SensorImpl(dhtPin);

  if (!g_mhz16 || !g_zce04b || !g_dht21) {
    Serial.println("[Sensor] Failed to allocate sensor drivers");
    destroySensors();
    return false;
  }

  if (!g_mhz16->begin()) {
    Serial.println("[Sensor] MH-Z16 init failed");
    destroySensors();
    return false;
  }

  if (!g_zce04b->begin()) {
    Serial.println("[Sensor] ZCE04B init failed");
    destroySensors();
    return false;
  }

  if (!g_dht21->begin()) {
    Serial.println("[Sensor] DHT21 init failed");
    destroySensors();
    return false;
  }

  if (millis() - startMs > timeoutMs) {
    Serial.println("[Sensor] Init timeout");
    destroySensors();
    return false;
  }

  Serial.println("[Sensor] MH-Z16 initialized");
  Serial.println("[Sensor] ZCE04B initialized");
  Serial.println("[Sensor] DHT21 initialized");
  return true;
}

void exhaustPumpOn() {
  if (g_exhaustPin >= 0) {
    digitalWrite(g_exhaustPin, LOW);
  }
}

void exhaustPumpOff() {
  if (g_exhaustPin >= 0) {
    digitalWrite(g_exhaustPin, HIGH);
  }
}

void aerationOn() {
  if (g_aerationPin >= 0) {
    digitalWrite(g_aerationPin, HIGH);
  }
}

void aerationOff() {
  if (g_aerationPin >= 0) {
    digitalWrite(g_aerationPin, LOW);
  }
}

int readMHZ16() {
  if (!g_mhz16) {
    return -1;
  }

  uint16_t co2ppm = 0;
  if (!g_mhz16->readCO2(co2ppm)) {
    return -1;
  }

  return static_cast<int>(co2ppm);
}

bool readZCE04B(ZCE04BGasData& data) {
  if (!g_zce04b) {
    return false;
  }

  ZCE04BSensor::GasData gas{};
  if (!g_zce04b->readGasData(gas)) {
    return false;
  }

  data.co = gas.co;
  data.h2s = gas.h2s;
  data.o2 = gas.o2;
  data.ch4 = gas.ch4;
  return true;
}

float readEOxygen() {
  ZCE04BGasData data{};
  if (!readZCE04B(data)) {
    return -1.0f;
  }

  if (data.o2 < 0.0f || data.o2 > 100.0f) {
    return -1.0f;
  }

  return data.o2;
}

bool readDHT21(DHT21Data& data) {
  if (!g_dht21) {
    return false;
  }

  DHT21SensorImpl::Data raw{};
  if (!g_dht21->readData(raw)) {
    return false;
  }

  data.temperature = raw.temperature;
  data.humidity = raw.humidity;
  return true;
}

float readDHT21Temp() {
  DHT21Data data{};
  if (!readDHT21(data)) {
    return -127.0f;
  }

  return data.temperature;
}

float readDHT21Hum() {
  DHT21Data data{};
  if (!readDHT21(data)) {
    return -1.0f;
  }

  return data.humidity;
}
