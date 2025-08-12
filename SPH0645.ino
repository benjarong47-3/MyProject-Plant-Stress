#include <Arduino.h>
#include <driver/i2s.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "DHT.h"

// === CONFIGURATION ===
const char *ssid = "Flip";
const char *password = "@Bankl2q8";
const char *google_script_url = "https://script.google.com/macros/s/AKfycbxvIblPqcjhxN2S5bFcw4t6et1NhTy5MIqICjrO5QZxhpSXcP7OLLs8_BhAfKO0c8Kd/exec";

#define I2S_WS 25
#define I2S_SD 22
#define I2S_SCK 26
#define I2S_PORT I2S_NUM_0

#define SD_CS 5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18

#define STATUS_LED 13
#define RECORD_BTN 14

#define DHTPIN 15
#define DHTTYPE DHT22

#define SAMPLE_RATE 48000
#define BUFFER_SIZE 1024
#define FILE_RECORD_TIME_MS (30 * 1000UL)
#define TOTAL_RECORD_TIME_MS (5UL * 60UL * 60UL * 1000UL)
#define START_DELAY_MS (60 * 1000UL)

// --- Global Variables & Structs ---
File audioFile, csvFile;
String folderName = "";
uint32_t recordedSamples = 0, fileIndex = 0;
unsigned long fileStartTime = 0, totalRecordStartTime = 0;
bool waiting = false, recording = false;
DHT dht(DHTPIN, DHTTYPE);

struct WAVHeader
{
    char riff[4];
    uint32_t fileSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
};
WAVHeader wavHeader;

// --- Functions ---
void sendDataToGoogleSheet(String fileName, float temp, float hum);

void createWAVHeader(uint32_t totalSamples)
{
    memcpy(wavHeader.riff, "RIFF", 4);
    memcpy(wavHeader.wave, "WAVE", 4);
    memcpy(wavHeader.fmt, "fmt ", 4);
    memcpy(wavHeader.data, "data", 4);
    wavHeader.fmtSize = 16;
    wavHeader.audioFormat = 1;
    wavHeader.numChannels = 1;
    wavHeader.sampleRate = SAMPLE_RATE;
    wavHeader.bitsPerSample = 16;
    wavHeader.byteRate = SAMPLE_RATE * wavHeader.numChannels * wavHeader.bitsPerSample / 8;
    wavHeader.blockAlign = wavHeader.numChannels * wavHeader.bitsPerSample / 8;
    wavHeader.dataSize = totalSamples * wavHeader.blockAlign;
    wavHeader.fileSize = 36 + wavHeader.dataSize;
}

void setupI2S()
{
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), .sample_rate = SAMPLE_RATE, .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = I2S_COMM_FORMAT_I2S_MSB, .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8, .dma_buf_len = BUFFER_SIZE, .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0};
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK, .ws_io_num = I2S_WS, .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_SD};
    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
}

void forceSyncTimeFromNTP()
{
    Serial.println("🌐 Connecting to WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        digitalWrite(STATUS_LED, HIGH);
        delay(250);
        digitalWrite(STATUS_LED, LOW);
        delay(250);
    }
    Serial.println("\n✅ WiFi connected!");
    configTime(7 * 3600, 0, "pool.ntp.org");
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo))
    {
        Serial.print(".");
        delay(500);
    }
    Serial.println("\n✅ Time synchronized.");
}

String getCurrentTimeString()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
        return "TIME_ERR";
    char buffer[30];
    snprintf(buffer, sizeof(buffer), "%02d/%02d/%04d %02d:%02d:%02d",
             timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buffer);
}

String getTimestampFolder()
{
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    char buffer[30];
    snprintf(buffer, sizeof(buffer), "/%02d-%02d-%d_%02d-%02d",
             timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
             timeinfo.tm_hour, timeinfo.tm_min);
    return String(buffer);
}

void writeCSV(String filename)
{
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    if (isnan(temp) || isnan(hum))
    {
        Serial.println("⚠️ Failed to read DHT");
        temp = hum = 0.0;
    }
    if (!csvFile)
    {
        String csvPath = folderName + "/record.csv";
        csvFile = SD.open(csvPath, FILE_WRITE);
        if (csvFile)
            csvFile.println("filename,temp,humidity,timestamp");
    }
    csvFile.printf("%s,%.2f,%.2f,%s\n", filename.c_str(), temp, hum, getCurrentTimeString().c_str());
    csvFile.flush();
}

void startNewFile()
{
    String filename = folderName + "/REC" + String(fileIndex++) + ".WAV";
    audioFile = SD.open(filename, FILE_WRITE);
    if (!audioFile)
    {
        Serial.println("❌ Can't open file: " + filename);
        return;
    }
    uint32_t totalSamples = SAMPLE_RATE * (FILE_RECORD_TIME_MS / 1000);
    createWAVHeader(totalSamples);
    audioFile.write((uint8_t *)&wavHeader, sizeof(WAVHeader));
    recordedSamples = 0;
    fileStartTime = millis();
    Serial.println("🔴 Started file: " + filename);
    writeCSV(filename);
}

void closeCurrentFile()
{
    wavHeader.dataSize = recordedSamples * 2;
    wavHeader.fileSize = 36 + wavHeader.dataSize;
    audioFile.seek(0);
    audioFile.write((uint8_t *)&wavHeader, sizeof(WAVHeader));
    audioFile.close();
    Serial.println("⏹️ Closed file " + String(fileIndex - 1));
}

void setup()
{
    Serial.begin(115200);
    pinMode(STATUS_LED, OUTPUT);
    pinMode(RECORD_BTN, INPUT_PULLUP);

    dht.begin();
    forceSyncTimeFromNTP();
    setupI2S();

    Serial.println("🗂️ Initializing SD Card...");
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    while (!SD.begin(SD_CS))
    {
        Serial.println("❌ SD Card failed. Retrying...");
        digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
        delay(500);
    }

    Serial.println("✅ System ready. LED ON. Waiting for button press...");
    digitalWrite(STATUS_LED, HIGH);
    WiFi.disconnect();
}

void loop()
{
    if (!waiting && !recording && digitalRead(RECORD_BTN) == LOW)
    {
        delay(50);
        if (digitalRead(RECORD_BTN) == LOW)
        {
            waiting = true;
            Serial.println("🕐 Button pressed. Waiting 1 min (LED will blink)...");
            unsigned long start = millis();
            while (millis() - start < START_DELAY_MS)
            {
                digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
                delay(250);
            }

            waiting = false;
            recording = true;
            fileIndex = 0;
            totalRecordStartTime = millis();
            folderName = getTimestampFolder();
            SD.mkdir(folderName.c_str());
            startNewFile();

            digitalWrite(STATUS_LED, HIGH);
            Serial.println("🎙️ Recording started...");
        }
    }

    if (recording)
    {
        int32_t samples[BUFFER_SIZE];
        size_t bytesRead;
        i2s_read(I2S_PORT, (void *)samples, sizeof(samples), &bytesRead, portMAX_DELAY);
        int samplesRead = bytesRead / sizeof(int32_t);
        for (int i = 0; i < samplesRead; i++)
        {
            int16_t s = (int16_t)(samples[i] >> 11);
            audioFile.write((uint8_t *)&s, sizeof(int16_t));
            recordedSamples++;
        }

        if (millis() - fileStartTime >= FILE_RECORD_TIME_MS)
        {
            String lastFileName = folderName + "/REC" + String(fileIndex - 1) + ".WAV";
            float lastTemp = dht.readTemperature();
            float lastHum = dht.readHumidity();
            closeCurrentFile();

            // <--- CHANGE: ดับไฟเพื่อบอกว่าบันทึกไฟล์เสร็จแล้ว ---
            digitalWrite(STATUS_LED, LOW);
            Serial.println("💡 LED OFF - File saved, sending data...");

            sendDataToGoogleSheet(lastFileName, lastTemp, lastHum);

            if (millis() - totalRecordStartTime < TOTAL_RECORD_TIME_MS)
            {
                // <--- CHANGE: เปิดไฟอีกครั้งก่อนเริ่มไฟล์ใหม่ ---
                digitalWrite(STATUS_LED, HIGH);
                Serial.println("💡 LED ON - Starting next file.");
                startNewFile();
            }
            else
            {
                csvFile.close();
                recording = false;
                digitalWrite(STATUS_LED, LOW);
                Serial.println("✅ Finished recording.");
                Serial.println("💤 Entering Deep Sleep...");
                Serial.flush();
                esp_sleep_enable_ext0_wakeup(GPIO_NUM_14, 0);
                esp_deep_sleep_start();
            }
        }
    }
}

void sendDataToGoogleSheet(String fileName, float temp, float hum)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("📡 Reconnecting to WiFi to send data...");
        WiFi.begin(ssid, password);
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 20)
        {
            delay(500);
            Serial.print(".");
            retries++;
        }
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("\n❌ Could not connect to WiFi. Skipping data send.");
            return;
        }
        Serial.println("\n✅ WiFi reconnected.");
    }

    HTTPClient http;
    http.begin(google_script_url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<200> doc;
    doc["fileName"] = fileName;
    doc["temperature"] = temp;
    doc["humidity"] = hum;

    String json_payload;
    serializeJson(doc, json_payload);

    Serial.println("📤 Sending data to Google Sheet...");
    int httpCode = http.POST(json_payload);

    if (httpCode > 0)
    {
        String response = http.getString();
        Serial.print("HTTP Response code: ");
        Serial.println(httpCode);
        Serial.println(response);
    }
    else
    {
        Serial.print("Error on sending POST: ");
        Serial.println(httpCode);
    }

    http.end();
    WiFi.disconnect();
    Serial.println("📶 WiFi disconnected.");
}