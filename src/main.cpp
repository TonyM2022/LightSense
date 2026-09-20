/*
  ESP32-S3 Flicker Meter V1

  功能:
  1. ADC采样
  2. 串口输出波形
  3. Serial Plotter显示
  4. Max/Min统计
  5. Flicker%计算

  硬件:
  - Board: Wifiduino32S3 (ESP32-S3)
  - Light sensor 信号输出 -> GPIO1 (ADC1_CH0)
*/

#include <Arduino.h>

#define ADC_PIN 1      // GPIO1 (ADC1_CH0)

const uint32_t SAMPLE_INTERVAL_US = 1000; // 1kHz

uint16_t adcValue = 0;

uint16_t maxValue = 0;
uint16_t minValue = 4095;

uint32_t sampleCount = 0;

uint32_t lastSampleTime = 0;
uint32_t lastReportTime = 0;

void setup()
{
    Serial.begin(115200);

    // ESP32 ADC分辨率
    analogReadResolution(12);

    // ADC衰减
    // 允许测量接近3.3V
    analogSetAttenuation(ADC_11db);

    delay(1000);

    Serial.println();
    Serial.println("ESP32-S3 Flicker Meter Start");
}

void loop()
{
    uint32_t now = micros();

    // 定时采样
    if (now - lastSampleTime >= SAMPLE_INTERVAL_US)
    {
        lastSampleTime += SAMPLE_INTERVAL_US;

        adcValue = analogRead(ADC_PIN);

        if (adcValue > maxValue)
            maxValue = adcValue;

        if (adcValue < minValue)
            minValue = adcValue;

        sampleCount++;

        // 波形输出
        // 打开 Serial Plotter 观察
        Serial.println(adcValue);
    }

    // 每秒统计一次
    if (millis() - lastReportTime >= 1000)
    {
        lastReportTime = millis();

        float flickerPercent = 0.0f;

        if ((maxValue + minValue) > 0)
        {
            flickerPercent =
                100.0f *
                (float)(maxValue - minValue) /
                (float)(maxValue + minValue);
        }

        Serial.println();
        Serial.println("------------------");

        Serial.print("Samples : ");
        Serial.println(sampleCount);

        Serial.print("Max     : ");
        Serial.println(maxValue);

        Serial.print("Min     : ");
        Serial.println(minValue);

        Serial.print("Flicker : ");
        Serial.print(flickerPercent, 2);
        Serial.println("%");

        Serial.println("------------------");
        Serial.println();

        // 清零重新统计
        maxValue = 0;
        minValue = 4095;
        sampleCount = 0;
    }
}
