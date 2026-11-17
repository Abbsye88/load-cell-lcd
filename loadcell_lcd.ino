#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include "HX711.h"

// ====== PIN ======
const int DT_PIN   = 16;
const int SCK_PIN  = 4;
const int TARE_PIN = 18;   // tombol tare: satu kaki ke GPIO 18, kaki lain ke GND

// ====== PENGATURAN ======
const float MAX_GRAM   = 5000.0; // kapasitas maksimum 5 kg
const int   SAMPLES    = 9;      // jumlah sampel per pembacaan
const float EMA_ALPHA  = 0.35;   // 0.1 = halus tapi lambat, 0.6 = cepat
const float ZERO_BAND  = 2.0;    // di bawah nilai ini (gram) ditampilkan 0
const float JUMP_RESET = 30.0;   // selisih besar (gram) -> langsung ikuti

LiquidCrystal_I2C lcd(0x27, 16, 2); // ganti 0x3F jika alamat berbeda
HX711 scale;
Preferences prefs;

float calFactor  = 0;   // raw per gram (0 = belum kalibrasi)
long  baseOffset = 0;   // nol sebenarnya (timbangan kosong saat dinyalakan)
long  tareOffset = 0;   // nol sementara (hasil tare, boleh saat ada beban)
float smooth = 0;
bool  firstReading = true;

// ---------- Baca raw dengan filter ----------
bool readRawFiltered(long &result, int n = SAMPLES) {
  long v[24];
  int count = 0;
  if (n > 24) n = 24;

  for (int i = 0; i < n; i++) {
    unsigned long t0 = millis();
    while (!scale.is_ready()) {
      if (millis() - t0 > 1000) break;
      delay(1);
    }
    if (!scale.is_ready()) return false;
    v[count++] = scale.read();
  }
  if (count < 5) return false;

  for (int i = 1; i < count; i++) {
    long key = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; }
    v[j + 1] = key;
  }

  int trim = 2;
  long long sum = 0;
  int used = 0;
  for (int i = trim; i < count - trim; i++) { sum += v[i]; used++; }
  result = (long)(sum / used);
  return true;
}

// ---------- Tare (boleh dengan beban di atas timbangan) ----------
bool doTare() {
  long raw;
  if (!readRawFiltered(raw, 20)) return false;
  tareOffset = raw;
  firstReading = true;
  return true;
}

// ---------- Kalibrasi ----------
// Tare dulu (kosong), taruh beban diketahui, lalu kirim k<gram>
bool doCalibrate(float knownGram) {
  if (knownGram <= 0) return false;
  long raw;
  if (!readRawFiltered(raw, 20)) return false;
  float f = (float)(raw - tareOffset) / knownGram;
  if (fabs(f) < 1.0) return false;
  calFactor = f;
  prefs.putFloat("cal", calFactor);
  firstReading = true;
  return true;
}

// ---------- Tombol tare ----------
void handleButton() {
  static bool lastState = HIGH;
  static unsigned long lastChange = 0;
  bool state = digitalRead(TARE_PIN);

  if (state != lastState && millis() - lastChange > 50) {
    lastChange = millis();
    lastState = state;
    if (state == LOW) { // tombol ditekan
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Tare...");
      bool ok = doTare();
      lcd.setCursor(0, 1);
      lcd.print(ok ? "Selesai: 0 g" : "Gagal!");
      Serial.println(ok ? "Tare OK" : "Tare GAGAL");
      delay(600);
      lcd.clear();
    }
  }
}

// ---------- Perintah Serial ----------
void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "t") {
    Serial.println(doTare() ? "Tare OK" : "Tare GAGAL");
  }
  else if (cmd.startsWith("k")) {
    float g = cmd.substring(1).toFloat();
    if (doCalibrate(g)) {
      Serial.print("Kalibrasi OK, faktor = ");
      Serial.println(calFactor, 4);
    } else {
      Serial.println("Kalibrasi GAGAL. Pastikan beban terpasang & wiring benar.");
    }
  }
  else if (cmd == "z") {
    long raw;
    if (readRawFiltered(raw, 20)) {
      baseOffset = raw;
      tareOffset = raw;
      firstReading = true;
      Serial.println("Nol sebenarnya diset (timbangan harus kosong)");
    }
  }
  else if (cmd == "r") {
    long raw;
    if (readRawFiltered(raw)) {
      Serial.print("Raw: "); Serial.print(raw);
      Serial.print(" | Tare: "); Serial.println(tareOffset);
    }
  }
  else {
    Serial.println("Perintah:");
    Serial.println("  t      -> tare (nolkan, boleh ada beban)");
    Serial.println("  k500   -> kalibrasi, beban 500 gram (tare dulu saat kosong)");
    Serial.println("  z      -> set nol sebenarnya (timbangan kosong)");
    Serial.println("  r      -> lihat nilai raw");
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  pinMode(TARE_PIN, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Timbangan Digital");
  lcd.setCursor(0, 1);
  lcd.print("Pemanasan 5 dtk");

  prefs.begin("timbangan", false);
  calFactor = prefs.getFloat("cal", 0);

  scale.begin(DT_PIN, SCK_PIN);
  delay(5000); // JANGAN taruh beban saat pemanasan

  long raw;
  if (!readRawFiltered(raw, 20)) {
    lcd.clear();
    lcd.print("HX711 ERROR!");
    lcd.setCursor(0, 1);
    lcd.print("Cek kabel DT/SCK");
    Serial.println("HX711 tidak terdeteksi. Cek DT, SCK, VCC, GND.");
    while (true) delay(1000);
  }
  baseOffset = raw;
  tareOffset = raw;

  lcd.clear();
  Serial.println("Siap. Kirim huruf apa saja untuk daftar perintah.");
  if (calFactor == 0) {
    Serial.println("BELUM KALIBRASI. Taruh beban diketahui, lalu kirim: k<gram>, contoh k500");
  }
}

void loop() {
  handleButton();
  handleSerial();

  long raw;
  if (!readRawFiltered(raw)) {
    lcd.setCursor(0, 0);
    lcd.print("HX711 tidak ada ");
    lcd.setCursor(0, 1);
    lcd.print("Cek kabel       ");
    return;
  }

  // Belum kalibrasi
  if (calFactor == 0) {
    lcd.setCursor(0, 0);
    lcd.print("Belum kalibrasi ");
    lcd.setCursor(0, 1);
    lcd.print("Raw:            ");
    lcd.setCursor(4, 1);
    lcd.print(raw - tareOffset);
    Serial.print("Raw (selisih dari nol): ");
    Serial.println(raw - tareOffset);
    return;
  }

  float net   = (float)(raw - tareOffset) / calFactor; // berat setelah tare
  float gross = (float)(raw - baseOffset) / calFactor;  // berat total sebenarnya

  // Proteksi kelebihan beban
  if (gross > MAX_GRAM + 50) {
    lcd.setCursor(0, 0);
    lcd.print("OVERLOAD!       ");
    lcd.setCursor(0, 1);
    lcd.print("Maks 5 kg       ");
    Serial.println("OVERLOAD! Kurangi beban");
    return;
  }

  // Penghalusan
  if (firstReading || fabs(net - smooth) > JUMP_RESET) {
    smooth = net;
    firstReading = false;
  } else {
    smooth = EMA_ALPHA * net + (1.0 - EMA_ALPHA) * smooth;
  }

  float shown = smooth;
  if (fabs(shown) < ZERO_BAND) shown = 0;

  Serial.print("Berat: ");
  Serial.print(shown, 1);
  Serial.println(" g");

  lcd.setCursor(0, 0);
  lcd.print("Berat (maks 5kg)");
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  if (fabs(shown) >= 1000) {
    lcd.print(shown / 1000.0, 2);
    lcd.print(" kg");
  } else {
    lcd.print(shown, 0);
    lcd.print(" g");
  }
}