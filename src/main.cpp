/*
 * ============================================================
 *  BoardDetective - Waveshare ESP32-S3-Touch-AMOLED-1.8
 * ============================================================
 *
 *  Diagnostica per questa board specifica. Stampa tutto sulla
 *  seriale E sul display AMOLED.
 *
 *  Comandi (digita la lettera nel monitor seriale):
 *    i  informazioni sul chip
 *    p  test read/write della PSRAM
 *    s  scansione del bus I2C con identificazione dei chip
 *    w  scansione reti WiFi
 *    d  ridisegna il riepilogo sul display
 *    b  riavvia
 *
 *  ATTENZIONE - niente caccia al LED su questa board:
 *  non esiste un LED utente, e i GPIO che una board generica
 *  lascerebbe liberi qui sono tutti occupati (vedi tabella).
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_system.h>
#include <Arduino_GFX_Library.h>

// ------------------------------------------------------------
//  PINOUT REALE (da waveshareteam/ESP32-S3-Touch-AMOLED-1.8,
//  file examples/arduino/libraries/Mylibrary/pin_config.h)
// ------------------------------------------------------------

// Display SH8601 su bus QSPI
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK   11
#define LCD_CS     12
#define LCD_W     368
#define LCD_H     448

// Bus I2C condiviso: touch FT3168, PMIC AXP2101, IMU QMI8658,
// RTC PCF85063, IO expander TCA9554, codec audio ES8311
#define I2C_SDA    15
#define I2C_SCL    14
#define TP_INT     21

// Audio I2S verso il codec ES8311
#define I2S_MCLK   16
#define I2S_BCLK    9
#define I2S_WS     45
#define I2S_DOUT    8
#define I2S_DIN    10
#define I2S_PA_EN  46

// Slot microSD (modalita' SDMMC a 1 bit)
#define SD_CLK      2
#define SD_CMD      1
#define SD_DATA     3

// GPIO 26..37 sono flash SPI e PSRAM octal: intoccabili.
// GPIO 0 e' il pulsante BOOT.
// Il pulsante PWR non e' un GPIO: passa dall'expander (EXIO4).

// ------------------------------------------------------------
//  DISPLAY
// ------------------------------------------------------------

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

static Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED /* nessun pin di reset */, 0 /* rotazione */,
    LCD_W, LCD_H);

static bool displayOk = false;

// ------------------------------------------------------------
//  UTILITA'
// ------------------------------------------------------------

static void lineC(char c) {
  for (uint8_t i = 0; i < 60; i++) Serial.print(c);
  Serial.println();
}

static void line() {
  lineC('-');
}

static void title(const char *t) {
  Serial.println();
  lineC('=');
  Serial.print("  ");
  Serial.println(t);
  lineC('=');
}

// Stampa una dimensione in byte con l'equivalente in MB arrotondato a un
// decimale. Con la divisione intera secca 8386231 byte diventerebbero
// "7 MB", che e' fuorviante: sono 8.0 MB tondi.
static void printSize(const char *label, uint64_t bytes) {
  uint32_t tenths = (uint32_t)((bytes * 10 + 524288) / 1048576);
  Serial.printf("%s %llu byte = %lu.%lu MB\n",
                label,
                (unsigned long long)bytes,
                (unsigned long)(tenths / 10),
                (unsigned long)(tenths % 10));
}

static void flushInput() {
  while (Serial.available()) Serial.read();
}

// ------------------------------------------------------------
//  INFORMAZIONI SUL CHIP
// ------------------------------------------------------------

static void showChipInfo() {
  title("INFORMAZIONI CHIP");

  Serial.printf("Modello .............. %s\n", ESP.getChipModel());

  // La codifica cambia tra le versioni di ESP-IDF:
  //   IDF 5.x (core Arduino 3.x) -> major*100 + minor, quindi 102 = v1.2
  //   IDF 4.x (core Arduino 2.x) -> numero semplice, senza la major
  uint16_t rev = ESP.getChipRevision();
  if (rev >= 100) {
    Serial.printf("Revisione silicio .... v%u.%u\n",
                  (unsigned)(rev / 100), (unsigned)(rev % 100));
  } else {
    Serial.printf("Revisione silicio .... %u (parziale; vedi esptool)\n",
                  (unsigned)rev);
  }

  Serial.printf("Core ................. %d\n", ESP.getChipCores());
  Serial.printf("Frequenza CPU ........ %lu MHz\n", (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("Core Arduino ......... %d.%d.%d\n",
                ESP_ARDUINO_VERSION_MAJOR,
                ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("ESP-IDF .............. %s\n", ESP.getSdkVersion());

  line();

  printSize("Flash ................", ESP.getFlashChipSize());
  Serial.printf("Sketch occupa ........ %lu byte su %lu disponibili\n",
                (unsigned long)ESP.getSketchSize(),
                (unsigned long)(ESP.getSketchSize() + ESP.getFreeSketchSpace()));

  line();

  size_t psram = ESP.getPsramSize();
  if (psram == 0) {
    Serial.println("PSRAM ................ NON rilevata");
    Serial.println();
    Serial.println("Su questa board la PSRAM NON e' opzionale: il buffer del");
    Serial.println("display e' 368*448*2 = 329 KB, piu' della RAM interna.");
    Serial.println("Servono in platformio.ini:");
    Serial.println("  board_build.arduino.memory_type = qio_opi");
    Serial.println("  -D BOARD_HAS_PSRAM");
  } else {
    printSize("PSRAM ...............", psram);
    printSize("PSRAM libera ........", ESP.getFreePsram());
  }

  Serial.printf("RAM interna libera ... %lu byte\n", (unsigned long)ESP.getFreeHeap());

  line();

  uint64_t mac = ESP.getEfuseMac();
  uint8_t m[6];
  for (int i = 0; i < 6; i++) m[i] = (uint8_t)((mac >> (8 * i)) & 0xFF);
  Serial.printf("MAC base ............. %02X:%02X:%02X:%02X:%02X:%02X\n",
                m[0], m[1], m[2], m[3], m[4], m[5]);

  Serial.print("Ultimo riavvio ....... ");
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  Serial.println("accensione"); break;
    case ESP_RST_EXT:      Serial.println("reset esterno"); break;
    case ESP_RST_SW:       Serial.println("riavvio software"); break;
    case ESP_RST_PANIC:    Serial.println("CRASH (exception / panic)"); break;
    case ESP_RST_INT_WDT:  Serial.println("watchdog interrupt"); break;
    case ESP_RST_TASK_WDT: Serial.println("watchdog task"); break;
    case ESP_RST_BROWNOUT: Serial.println("BROWNOUT - alimentazione insufficiente"); break;
    case ESP_RST_DEEPSLEEP:Serial.println("risveglio da deep sleep"); break;
    case ESP_RST_SDIO:     Serial.println("reset via SDIO"); break;
    default:
      Serial.println("sconosciuto (normale subito dopo un caricamento:");
      Serial.println("                       il reset arriva da esptool)");
      break;
  }
}

// ------------------------------------------------------------
//  TEST PSRAM
// ------------------------------------------------------------

static void testPsram() {
  title("TEST PSRAM");

  if (ESP.getPsramSize() == 0) {
    Serial.println("Nessuna PSRAM rilevata: niente da testare.");
    return;
  }

  const size_t N = 64 * 1024;
  Serial.printf("Alloco %u byte in PSRAM...\n", (unsigned)N);

  uint8_t *buf = (uint8_t *)ps_malloc(N);
  if (!buf) {
    Serial.println("FALLITO: ps_malloc ha restituito NULL.");
    return;
  }
  Serial.printf("OK, indirizzo 0x%08lX\n", (unsigned long)(uintptr_t)buf);

  Serial.print("Scrittura pattern... ");
  for (size_t i = 0; i < N; i++) buf[i] = (uint8_t)(i * 31 + 7);
  Serial.println("fatto");

  Serial.print("Rilettura e confronto... ");
  size_t errors = 0;
  for (size_t i = 0; i < N; i++) {
    if (buf[i] != (uint8_t)(i * 31 + 7)) errors++;
  }

  if (errors == 0) {
    Serial.println("PERFETTO, 0 errori.");
  } else {
    Serial.printf("%u ERRORI su %u byte!\n", (unsigned)errors, (unsigned)N);
    Serial.println("Modalita' PSRAM probabilmente sbagliata (serve qio_opi).");
  }

  free(buf);
}

// ------------------------------------------------------------
//  SCANSIONE I2C
// ------------------------------------------------------------

// Chip che sappiamo esserci su questa board, per indirizzo.
static const char *knownI2cDevice(uint8_t addr) {
  switch (addr) {
    case 0x18: return "ES8311  - codec audio (mic + altoparlante)";
    case 0x20: return "TCA9554 - IO expander (pulsante PWR, CS della SD)";
    case 0x34: return "AXP2101 - PMIC, gestione alimentazione e batteria";
    case 0x38: return "FT3168  - controller touch dello schermo";
    case 0x51: return "PCF85063- orologio RTC con batteria tampone";
    case 0x6A: return "QMI8658 - IMU 6 assi (accelerometro + giroscopio)";
    case 0x6B: return "QMI8658 - IMU 6 assi (accelerometro + giroscopio)";
    default:   return NULL;
  }
}

static uint8_t scanI2C() {
  title("SCANSIONE BUS I2C");

  Serial.printf("Bus su SDA = GPIO %u, SCL = GPIO %u\n", I2C_SDA, I2C_SCL);
  Serial.println("(pin presi dal repository ufficiale Waveshare)");
  line();

  static bool started = false;
  if (started) Wire.end();
  started = true;

  if (!Wire.begin(I2C_SDA, I2C_SCL)) {
    Serial.println("Impossibile inizializzare il bus I2C.");
    return 0;
  }
  Wire.setClock(100000);

  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      const char *name = knownI2cDevice(addr);
      Serial.printf("0x%02X  %s\n", addr, name ? name : "sconosciuto");
      found++;
      delay(5);
    }
  }

  line();
  Serial.printf("Totale: %u dispositivi.\n", found);
  if (found < 5) {
    Serial.println("Ne mancano all'appello: alcuni chip rispondono solo");
    Serial.println("dopo essere stati svegliati dal PMIC.");
  }
  return found;
}

// ------------------------------------------------------------
//  SCANSIONE WIFI
// ------------------------------------------------------------

static void scanWiFi() {
  title("SCANSIONE RETI WIFI");
  Serial.println("Attendi qualche secondo...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("Nessuna rete trovata.");
  } else {
    Serial.printf("Trovate %d reti:\n\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf("%3d  %4d dBm  ch%2d  %s\n",
                    i + 1, WiFi.RSSI(i), WiFi.channel(i), WiFi.SSID(i).c_str());
    }
    Serial.println();
    Serial.println("RSSI: -30 ottimo, -67 buono, -80 debole");
  }

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
}

// ------------------------------------------------------------
//  RIEPILOGO SUL DISPLAY
// ------------------------------------------------------------

static void drawSummary(uint8_t i2cCount) {
  if (!displayOk) return;

  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextSize(2);

  gfx->setCursor(12, 16);
  gfx->setTextColor(RGB565_CYAN);
  gfx->println("BoardDetective");

  gfx->setTextSize(1);
  gfx->setCursor(12, 44);
  gfx->setTextColor(RGB565_WHITE);
  gfx->println("Waveshare ESP32-S3-Touch-AMOLED-1.8");

  gfx->setTextSize(2);
  int y = 78;

  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(12, y); gfx->printf("Chip   %s", ESP.getChipModel());       y += 26;
  gfx->setCursor(12, y); gfx->printf("CPU    %lu MHz",
                                     (unsigned long)ESP.getCpuFreqMHz());    y += 26;
  gfx->setCursor(12, y); gfx->printf("Flash  %lu MB",
                                     (unsigned long)(ESP.getFlashChipSize() / 1048576UL)); y += 26;

  size_t psram = ESP.getPsramSize();
  gfx->setTextColor(psram ? RGB565_GREEN : RGB565_RED);
  gfx->setCursor(12, y);
  if (psram) {
    gfx->printf("PSRAM  %lu MB", (unsigned long)((psram + 524288UL) / 1048576UL));
  } else {
    gfx->print("PSRAM  assente!");
  }
  y += 26;

  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(12, y); gfx->printf("I2C    %u chip", i2cCount);            y += 34;

  uint64_t mac = ESP.getEfuseMac();
  uint8_t m[6];
  for (int i = 0; i < 6; i++) m[i] = (uint8_t)((mac >> (8 * i)) & 0xFF);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(12, y);
  gfx->printf("MAC %02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  y += 18;
  gfx->setCursor(12, y);
  gfx->printf("Core Arduino %d.%d.%d",
              ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
              ESP_ARDUINO_VERSION_PATCH);

  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(12, LCD_H - 40);
  gfx->print("Se leggi questo,");
  gfx->setCursor(12, LCD_H - 22);
  gfx->print("funziona tutto.");
}

// ------------------------------------------------------------
//  MENU
// ------------------------------------------------------------

static void showMenu() {
  Serial.println();
  lineC('*');
  Serial.println("  i  informazioni chip        s  scansione I2C");
  Serial.println("  p  test PSRAM               w  scansione WiFi");
  Serial.println("  d  ridisegna il display     b  riavvia");
  lineC('*');
  Serial.print("> ");
}

// ------------------------------------------------------------
//  SETUP / LOOP
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(50);
  delay(300);

  Serial.println();
  lineC('#');
  Serial.println("   BoardDetective - Waveshare ESP32-S3-Touch-AMOLED-1.8");
  lineC('#');

  Serial.print("Inizializzo il display... ");
  displayOk = gfx->begin();
  if (displayOk) {
    gfx->setBrightness(200);
    gfx->fillScreen(RGB565_BLACK);
    Serial.println("OK");
  } else {
    Serial.println("FALLITO");
    Serial.println("Controlla che la libreria Arduino_GFX sia installata");
    Serial.println("e che la PSRAM sia abilitata in platformio.ini.");
  }

  showChipInfo();
  uint8_t n = scanI2C();
  drawSummary(n);
  showMenu();
}

void loop() {
  if (!Serial.available()) {
    delay(20);
    return;
  }

  char c = Serial.read();
  if (c == '\n' || c == '\r' || c == ' ') return;

  Serial.println(c);
  flushInput();

  switch (c) {
    case 'i': case 'I': showChipInfo(); break;
    case 'p': case 'P': testPsram(); break;
    case 's': case 'S': drawSummary(scanI2C()); break;
    case 'w': case 'W': scanWiFi(); break;
    case 'd': case 'D': drawSummary(scanI2C()); break;
    case 'b': case 'B':
      Serial.println("Riavvio...");
      delay(500);
      ESP.restart();
      break;
    default:
      Serial.printf("Comando '%c' sconosciuto.\n", c);
      break;
  }

  showMenu();
}
