// ---------------- ADC DMA 驱动层实现 ----------------
// ESP32-S3: 仅 ADC1 支持 DMA 连续模式, GPIO1 = ADC1_CH0, 12bit + 12dB 衰减
#include <Arduino.h>
#include "driver/adc.h"
#include "config.h"
#include "adc_dma.h"

static uint32_t dmaBuf[FFT_SIZE * BYTES_PER_SAMPLE / 4];   // DMA 读取缓冲 (16KB)

// 诊断计数
static uint32_t overflowCnt = 0;                           // DMA 溢出计数
static uint32_t dbgBlocks = 0;                             // 成功读取的块数
static uint32_t dbgTimeout = 0;                            // 读取超时次数
static uint32_t lastDbgMs = 0;                             // 上次诊断输出时间
static bool chunkLogged = false;                           // 首片大小是否已打印
static bool rawLogged = false;                             // 首块样本是否已打印

bool adcDmaBegin(void)
{
    adc_digi_init_config_t initCfg = {};
    initCfg.max_store_buf_size = DMA_BUF_BYTES * 2;   // 驱动内部环形缓冲
    initCfg.conv_num_each_intr = 4096;                // 每次中断 4096 字节
    initCfg.adc1_chan_mask = (1 << ADC_CHANNEL);
    initCfg.adc2_chan_mask = 0;

    esp_err_t err = adc_digi_initialize(&initCfg);
    if (err != ESP_OK)
    {
        Serial.print("adc_digi_initialize failed: 0x");
        Serial.println(err, HEX);
        return false;
    }

    adc_digi_pattern_config_t pattern = {};
    pattern.atten     = ADC_ATTEN_DB_12;   // 12dB 衰减 (即旧称 11dB), 量程约 0~3.1V
    pattern.channel   = ADC_CHANNEL;
    pattern.unit      = 0;   // DMA 模式 unit 为 0 基索引: 0=ADC1, 1=ADC2 (S3 仅支持 ADC1 DMA)
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;   // DMA 模式填实际位宽 12 (非枚举值)

    adc_digi_configuration_t ctrlCfg = {};
    ctrlCfg.conv_limit_en  = true;
    ctrlCfg.conv_limit_num = 255;
    ctrlCfg.pattern_num    = 1;
    ctrlCfg.adc_pattern    = &pattern;
    ctrlCfg.sample_freq_hz = SAMPLE_RATE_HZ;
    ctrlCfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
    ctrlCfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

    err = adc_digi_controller_configure(&ctrlCfg);
    if (err != ESP_OK)
    {
        Serial.print("adc_digi_controller_configure failed: 0x");
        Serial.println(err, HEX);
        return false;
    }

    err = adc_digi_start();
    if (err != ESP_OK)
    {
        Serial.print("adc_digi_start failed: 0x");
        Serial.println(err, HEX);
        return false;
    }

    Serial.println("ADC DMA started.");
    return true;
}

bool adcDmaReadBlock(float *out)
{
    // 周期诊断输出 (每 2 s)
    if (millis() - lastDbgMs >= 2000)
    {
        lastDbgMs = millis();
        Serial.print("DBG blocks=");
        Serial.print(dbgBlocks);
        Serial.print(" timeout=");
        Serial.print(dbgTimeout);
        Serial.print(" ovr=");
        Serial.println(overflowCnt);
    }

    // 累积读取 4096 样本 (16384 字节)
    // 兼容按中断块分片返回数据的驱动, 短读不整块丢弃
    uint32_t got = 0;
    while (got < DMA_BUF_BYTES)
    {
        uint32_t outLen = 0;
        esp_err_t err = adc_digi_read_bytes((uint8_t *)dmaBuf + got,
                                            DMA_BUF_BYTES - got, &outLen, 200);

        if (err == ESP_ERR_INVALID_STATE)
        {
            overflowCnt++;        // DMA 溢出: 数据丢失, 本块作废
            return false;
        }
        if (err != ESP_OK || outLen == 0)
        {
            dbgTimeout++;
            return false;
        }
        if (!chunkLogged)
        {
            chunkLogged = true;
            Serial.print("DBG first chunk = ");
            Serial.print(outLen);
            Serial.println(" bytes");
        }
        got += outLen;
    }

    // TYPE2 帧解析: 每样本 4 字节, 12bit 有效数据
    const adc_digi_output_data_t *frames = (const adc_digi_output_data_t *)dmaBuf;

    if (!rawLogged)
    {
        rawLogged = true;
        Serial.print("DBG raw[0..3] = ");
        for (int i = 0; i < 4; i++)
        {
            Serial.print(frames[i].type2.data);
            Serial.print(' ');
        }
        Serial.println();
    }

    for (uint32_t i = 0; i < FFT_SIZE; i++)
    {
        out[i] = (float)frames[i].type2.data;
    }
    dbgBlocks++;
    return true;
}

uint32_t adcDmaTakeOverflow(void)
{
    uint32_t cnt = overflowCnt;
    overflowCnt = 0;
    return cnt;
}
