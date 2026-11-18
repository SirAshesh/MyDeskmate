#include <Arduino.h>
#include <Adafruit_GFX.h>

// ============================================================
// DEFINISI PIN PARALEL 8-BIT ESP32-S3
// ============================================================
const int PIN_RD  = 1;
const int PIN_WR  = 4;
const int PIN_RS  = 5; // D/C
const int PIN_CS  = 6;
const int PIN_RST = 7;

const int PINS_DATA[8] = {8, 9, 10, 11, 12, 13, 14, 15}; // D0..D7

// Warna Standar 16-bit 565
#define COLOR_BLACK       0x0000
#define COLOR_NAVY        0x000F
#define COLOR_DARKGREEN   0x03E0
#define COLOR_DARKCYAN    0x03EF
#define COLOR_MAROON      0x7800
#define COLOR_PURPLE      0x780F
#define COLOR_OLIVE       0x7BE0
#define COLOR_LIGHTGREY   0xC618
#define COLOR_DARKGREY    0x7BEF
#define COLOR_BLUE        0x001F
#define COLOR_GREEN       0x07E0
#define COLOR_CYAN        0x07FF
#define COLOR_RED         0xF800
#define COLOR_MAGENTA     0xF81F
#define COLOR_YELLOW      0xFFE0
#define COLOR_WHITE       0xFFFF
#define COLOR_GREENYELLOW 0xAFE5

// ============================================================
// DRIVER HX8357D KHUSUS ESP32-S3 (FAST GPIO & ADAFRUIT GFX)
// ============================================================
class TFT_HX8357D : public Adafruit_GFX {
public:
  TFT_HX8357D() : Adafruit_GFX(480, 320) {}

  inline void write8(uint8_t val) {
    uint32_t set_mask = (uint32_t)val << 8;
    uint32_t clr_mask = ((uint32_t)(~val & 0xFF)) << 8;
    GPIO.out_w1ts = set_mask;
    GPIO.out_w1tc = clr_mask;

    GPIO.out_w1tc = (1 << PIN_WR);
    __asm__ __volatile__("nop; nop; nop; nop; nop;");
    GPIO.out_w1ts = (1 << PIN_WR);
    __asm__ __volatile__("nop; nop;");
  }

  inline void writeCmd(uint8_t cmd) {
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_RS, LOW);
    write8(cmd);
    digitalWrite(PIN_CS, HIGH);
  }

  inline void writeData(uint8_t dat) {
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_RS, HIGH);
    write8(dat);
    digitalWrite(PIN_CS, HIGH);
  }

  inline void setAddrWindow(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    writeCmd(0x2A); // CASET
    writeData(x0 >> 8);
    writeData(x0 & 0xFF);
    writeData(x1 >> 8);
    writeData(x1 & 0xFF);

    writeCmd(0x2B); // PASET
    writeData(y0 >> 8);
    writeData(y0 & 0xFF);
    writeData(y1 >> 8);
    writeData(y1 & 0xFF);

    writeCmd(0x2C); // RAMWR
  }

  void begin() {
    pinMode(PIN_CS, OUTPUT);
    pinMode(PIN_RS, OUTPUT);
    pinMode(PIN_WR, OUTPUT);
    pinMode(PIN_RD, OUTPUT);
    pinMode(PIN_RST, OUTPUT);

    for (int i = 0; i < 8; i++) {
      pinMode(PINS_DATA[i], OUTPUT);
    }

    digitalWrite(PIN_CS, HIGH);
    digitalWrite(PIN_RS, HIGH);
    digitalWrite(PIN_WR, HIGH);
    digitalWrite(PIN_RD, HIGH);

    // Hardware Reset
    digitalWrite(PIN_RST, HIGH);
    delay(20);
    digitalWrite(PIN_RST, LOW);
    delay(100);
    digitalWrite(PIN_RST, HIGH);
    delay(150);

    // ==========================================
    // INISIALISASI ASLI HX8357D
    // ==========================================
    writeCmd(0x01); // Software Reset
    delay(20);

    // SETC: Password akses extended register HX8357D
    writeCmd(0xB9);
    writeData(0xFF);
    writeData(0x83);
    writeData(0x57);
    delay(100);

    // SETRGB: Interface mode & RGB control
    writeCmd(0xB3);
    writeData(0x80);
    writeData(0x00);
    writeData(0x06);
    writeData(0x06);

    // SETCOM: VCOM voltage
    writeCmd(0xB6);
    writeData(0x25);

    // SETOSC: Internal oscillator
    writeCmd(0xB0);
    writeData(0x68);

    // SETPANEL
    writeCmd(0xCC);
    writeData(0x08);

    // SETPWR1: Power Control
    writeCmd(0xB1);
    writeData(0x00);
    writeData(0x15);
    writeData(0x1C);
    writeData(0x1C);
    writeData(0x83);
    writeData(0xAA);

    // SETSTBA: Standby Mode
    writeCmd(0xC0);
    writeData(0x50);
    writeData(0x50);
    writeData(0x01);
    writeData(0x3C);
    writeData(0x1E);
    writeData(0x08);

    // SETCYC: Cycle / Timing control
    writeCmd(0xB4);
    writeData(0x02);
    writeData(0x40);
    writeData(0x00);
    writeData(0x2A);
    writeData(0x2A);
    writeData(0x0D);
    writeData(0x78);

    // SETGAMMA: Kurva Gamma HX8357D
    writeCmd(0xE0);
    const uint8_t gamma[] = {
      0x02, 0x0A, 0x11, 0x1D, 0x23, 0x35, 0x41, 0x4B,
      0x4B, 0x42, 0x3A, 0x27, 0x1B, 0x08, 0x09, 0x03,
      0x02, 0x0A, 0x11, 0x1D, 0x23, 0x35, 0x41, 0x4B,
      0x4B, 0x42, 0x3A, 0x27, 0x1B, 0x08, 0x09, 0x03,
      0x00, 0x01
    };
    for (size_t i = 0; i < sizeof(gamma); i++) {
      writeData(gamma[i]);
    }

    // Format Pixel (16-bit / 565)
    writeCmd(0x3A);
    writeData(0x55);

    // Orientasi Layar Default (Landscape 480x320)
    setRotation(1);

    // Keluar dari Sleep Mode
    writeCmd(0x11);
    delay(150);

    // Aktifkan Tampilan (Display ON)
    writeCmd(0x29);
    delay(100);
  }

  void setRotation(uint8_t r) override {
    rotation = r & 3;
    writeCmd(0x36);
    switch (rotation) {
      case 0:
        _width  = 320;
        _height = 480;
        writeData(0x48);
        break;
      case 1:
        _width  = 480;
        _height = 320;
        writeData(0x28);
        break;
      case 2:
        _width  = 320;
        _height = 480;
        writeData(0x88);
        break;
      case 3:
        _width  = 480;
        _height = 320;
        writeData(0xE8);
        break;
    }
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= _width || y < 0 || y >= _height) return;
    digitalWrite(PIN_CS, LOW);
    setAddrWindow(x, y, x, y);
    digitalWrite(PIN_RS, HIGH);
    write8(color >> 8);
    write8(color & 0xFF);
    digitalWrite(PIN_CS, HIGH);
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    if (x >= _width || y >= _height || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > _width)  w = _width - x;
    if (y + h > _height) h = _height - y;

    digitalWrite(PIN_CS, LOW);
    setAddrWindow(x, y, x + w - 1, y + h - 1);
    digitalWrite(PIN_RS, HIGH);
    uint32_t total = (uint32_t)w * h;
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    while (total--) {
      write8(hi);
      write8(lo);
    }
    digitalWrite(PIN_CS, HIGH);
  }

  void fillScreen(uint16_t color) override {
    fillRect(0, 0, _width, _height, color);
  }
};

TFT_HX8357D tft;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Inisialisasi Driver HX8357D Dimulai ---");

  tft.begin();
  tft.setRotation(1); // Landscape (480 x 320)

  // Latar belakang warna Navy Blue
  tft.fillScreen(COLOR_NAVY);

  // 1. Header Box
  tft.fillRoundRect(15, 15, 450, 50, 8, COLOR_DARKCYAN);
  tft.drawRoundRect(15, 15, 450, 50, 8, COLOR_WHITE);

  tft.setTextSize(3);
  tft.setTextColor(COLOR_WHITE);
  tft.setCursor(35, 28);
  tft.print("HX8357D 3.5\" DISPLAY");

  // 2. Info Box
  tft.fillRoundRect(15, 80, 450, 225, 8, COLOR_BLACK);
  tft.drawRoundRect(15, 80, 450, 225, 8, COLOR_CYAN);

  tft.setTextSize(2);
  tft.setTextColor(COLOR_GREENYELLOW);
  tft.setCursor(35, 100);
  tft.print("Status  : DRIVER HX8357D AKTIF!");

  tft.setTextColor(COLOR_WHITE);
  tft.setCursor(35, 135);
  tft.print("Board   : ESP32-S3 N16R8");

  tft.setCursor(35, 165);
  tft.print("Bus     : 8-Bit Parallel 8080");

  tft.setCursor(35, 195);
  tft.print("Resolusi: 480 x 320 Piksel");

  // 3. Palet Titik Warna
  uint16_t dots[] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW, COLOR_MAGENTA, COLOR_CYAN};
  for (int i = 0; i < 6; i++) {
    tft.fillCircle(55 + (i * 38), 260, 13, dots[i]);
    tft.drawCircle(55 + (i * 38), 260, 13, COLOR_WHITE);
  }

  Serial.println("HX8357D berhasil diinisialisasi & gambar dikirim!");
}

int detik = 0;
void loop() {
  // Update counter berjalan
  tft.fillRect(270, 245, 185, 30, COLOR_BLACK);
  tft.setTextColor(COLOR_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(280, 252);
  tft.printf("Uptime: %ds", detik++);

  // Indikator kedip bulat
  if (detik % 2 == 0) {
    tft.fillCircle(430, 107, 8, COLOR_GREEN);
  } else {
    tft.fillCircle(430, 107, 8, COLOR_DARKGREY);
  }

  Serial.printf("Detik ke: %d\n", detik);
  delay(1000);
}
