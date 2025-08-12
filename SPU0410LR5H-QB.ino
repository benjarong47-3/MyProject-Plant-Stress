#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

// =================================================================
// --- การตั้งค่าพื้นฐาน (สามารถปรับเปลี่ยนได้) ---
// =================================================================

// -- ขาที่เชื่อมต่อ --
const int AUDIO_PIN = 32; // ขา ADC ที่ต่อกับสัญญาณเสียงจาก Amplifier
const int SD_CS_PIN = 5;  // ขา Chip Select (CS) สำหรับ SD Card Module

// -- การตั้งค่าการบันทึก --
const int RECORD_TIME_SECONDS = 30; // ระยะเวลาที่ต้องการบันทึก (วินาที)

// **สำคัญ:** ปรับค่านี้ตามเป้าหมาย
// - สำหรับเสียงทั่วไป (ต้นไม้ใหญ่, เสียงบรรยากาศ): 8000 - 16000
// - สำหรับเสียงอัลตราโซนิก (ต้นภูด่าง, พืชเครียด): 44100 - 100000
const int SAMPLING_RATE = 44100;

// =================================================================
// --- ตัวแปรและค่าคงที่ของระบบ (ไม่ต้องแก้ไข) ---
// =================================================================

const int BUFFER_SIZE = 1024;   // ขนาดของบัฟเฟอร์
const int WAV_HEADER_SIZE = 44; // ขนาดของ WAV Header

// --- ตัวแปรสำหรับ Double Buffering ---
byte buffer1[BUFFER_SIZE];
byte buffer2[BUFFER_SIZE];

volatile byte *current_buffer = buffer1;
volatile bool buffer_ready = false;
volatile int buffer_idx = 0;

// --- ตัวแปรสำหรับ Timer และสถานะการทำงาน ---
hw_timer_t *timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
File wavFile;
uint32_t totalSamplesWritten = 0;

// =================================================================
// --- ฟังก์ชันสร้าง WAV Header ---
// =================================================================
void writeWavHeader(File file, uint32_t totalAudioDataBytes)
{
    byte header[WAV_HEADER_SIZE];

    uint32_t fileSize = totalAudioDataBytes + WAV_HEADER_SIZE - 8;
    uint32_t dataSize = totalAudioDataBytes;

    uint16_t numChannels = 1;   // Mono
    uint16_t bitsPerSample = 8; // 8-bit
    uint32_t byteRate = SAMPLING_RATE * numChannels * (bitsPerSample / 8);

    // RIFF Chunk
    header[0] = 'R';
    header[1] = 'I';
    header[2] = 'F';
    header[3] = 'F';
    header[4] = (byte)(fileSize & 0xFF);
    header[5] = (byte)((fileSize >> 8) & 0xFF);
    header[6] = (byte)((fileSize >> 16) & 0xFF);
    header[7] = (byte)((fileSize >> 24) & 0xFF);

    // WAVE Format
    header[8] = 'W';
    header[9] = 'A';
    header[10] = 'V';
    header[11] = 'E';

    // "fmt " sub-chunk
    header[12] = 'f';
    header[13] = 'm';
    header[14] = 't';
    header[15] = ' ';
    header[16] = 16;
    header[17] = 0;
    header[18] = 0;
    header[19] = 0; // Sub-chunk size (16 for PCM)
    header[20] = 1;
    header[21] = 0; // Audio Format (1 for PCM)
    header[22] = numChannels;
    header[23] = 0; // Number of Channels
    header[24] = (byte)(SAMPLING_RATE & 0xFF);
    header[25] = (byte)((SAMPLING_RATE >> 8) & 0xFF);
    header[26] = (byte)((SAMPLING_RATE >> 16) & 0xFF);
    header[27] = (byte)((SAMPLING_RATE >> 24) & 0xFF);
    header[28] = (byte)(byteRate & 0xFF);
    header[29] = (byte)((byteRate >> 8) & 0xFF);
    header[30] = (byte)((byteRate >> 16) & 0xFF);
    header[31] = (byte)((byteRate >> 24) & 0xFF);
    header[32] = numChannels * (bitsPerSample / 8);
    header[33] = 0; // Block Align
    header[34] = bitsPerSample;
    header[35] = 0; // Bits per Sample

    // "data" sub-chunk
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    header[40] = (byte)(dataSize & 0xFF);
    header[41] = (byte)((dataSize >> 8) & 0xFF);
    header[42] = (byte)((dataSize >> 16) & 0xFF);
    header[43] = (byte)((dataSize >> 24) & 0xFF);

    file.seek(0);
    file.write(header, WAV_HEADER_SIZE);
}

// =================================================================
// --- Interrupt Service Routine (ISR) - หัวใจของการเก็บเสียง ---
// =================================================================
void IRAM_ATTR onTimer()
{
    portENTER_CRITICAL_ISR(&timerMux);

    if (buffer_idx < BUFFER_SIZE)
    {
        uint16_t adc_value = analogRead(AUDIO_PIN);
        current_buffer[buffer_idx] = adc_value >> 4; // แปลงจาก 12-bit (0-4095) เป็น 8-bit (0-255)
        buffer_idx++;
    }
    else
    {
        buffer_ready = true;
        // สลับ Buffer เพื่อให้ loop หลักนำไปเขียนลง SD Card
        if (current_buffer == buffer1)
        {
            current_buffer = buffer2;
        }
        else
        {
            current_buffer = buffer1;
        }
        buffer_idx = 0;
    }

    portEXIT_CRITICAL_ISR(&timerMux);
}

// =================================================================
// --- SETUP: ทำงานครั้งเดียวเมื่อเปิดเครื่อง ---
// =================================================================
void setup()
{
    Serial.begin(115200);
    Serial.println("\n--- Audio Recorder Initializing ---");

    // --- ตั้งค่า SD Card ---
    SPI.begin();
    if (!SD.begin(SD_CS_PIN))
    {
        Serial.println("Error: SD Card initialization failed!");
        while (1)
            ;
    }
    Serial.println("OK: SD Card initialized.");

    // --- สร้างไฟล์ WAV ---
    String filename = "/recording.wav";
    if (SD.exists(filename))
    {
        SD.remove(filename);
    }
    wavFile = SD.open(filename, FILE_WRITE);
    if (!wavFile)
    {
        Serial.println("Error: Failed to open file for writing.");
        while (1)
            ;
    }
    Serial.printf("OK: Recording to file: %s\n", filename.c_str());

    // เขียน Header ปลอมไปก่อน 44 bytes เพื่อจองพื้นที่
    byte dummyHeader[WAV_HEADER_SIZE];
    wavFile.write(dummyHeader, WAV_HEADER_SIZE);

    // --- ตั้งค่า Timer ---
    // The ESP32 Arduino Core timer API has changed. We now specify the desired interrupt frequency directly.
    timer = timerBegin(SAMPLING_RATE);     // Set timer to trigger at the sampling rate frequency.
    timerAttachInterrupt(timer, &onTimer); // Attach the ISR. The timer starts automatically.

    Serial.printf("OK: Timer started. Recording for %d seconds at %d Hz.\n", RECORD_TIME_SECONDS, SAMPLING_RATE);
    Serial.print("Recording in progress ");
}

// =================================================================
// --- LOOP: ทำงานวนซ้ำไปเรื่อยๆ ---
// =================================================================
void loop()
{
    uint32_t totalSamplesToRecord = SAMPLING_RATE * RECORD_TIME_SECONDS;

    // --- ส่วนหลักในการเขียนข้อมูล ---
    if (totalSamplesWritten < totalSamplesToRecord)
    {
        if (buffer_ready)
        {
            portENTER_CRITICAL(&timerMux);
            buffer_ready = false;
            portEXIT_CRITICAL(&timerMux);

            // เขียน Buffer ที่เต็มแล้วลง SD Card
            if (current_buffer == buffer1)
            {
                wavFile.write(buffer2, BUFFER_SIZE);
            }
            else
            {
                wavFile.write(buffer1, BUFFER_SIZE);
            }
            totalSamplesWritten += BUFFER_SIZE;
            Serial.print(".");
        }
    }
    // --- เมื่อบันทึกเสร็จ ---
    else
    {
        Serial.println("\n--- Recording Finished ---");

        // หยุด Timer
        // 'timerAlarmDisable' is deprecated. Use 'timerStop' to stop the timer counter.
        timerStop(timer);
        timerEnd(timer);

        // เขียนข้อมูลที่เหลือใน Buffer สุดท้ายที่ยังไม่เต็ม
        wavFile.write((const uint8_t *)current_buffer, buffer_idx);
        totalSamplesWritten += buffer_idx;

        // อัปเดต WAV Header ด้วยขนาดไฟล์ที่ถูกต้อง
        Serial.println("Updating WAV header...");
        writeWavHeader(wavFile, totalSamplesWritten);

        // ปิดไฟล์
        wavFile.close();
        Serial.printf("OK: File saved. Total data size: %d bytes.\n", totalSamplesWritten);
        Serial.println("Device halted. Please reset to record again.");

        // หยุดการทำงานทั้งหมด
        while (1)
            ;
    }
}