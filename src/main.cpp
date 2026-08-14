/*
 * ============================================================
 *  BoardDetective - Diagnostica per ESP32-S3 (board generiche)
 * ============================================================
 *
 *  A cosa serve:
 *  Le board ESP32-S3 cinesi non hanno documentazione affidabile.
 *  Questo sketch scopre da solo cosa hai tra le mani:
 *    - modello di chip, revisione, core, frequenza
 *    - dimensione flash e PSRAM (e se la PSRAM funziona davvero)
 *    - indirizzo MAC / ID univoco
 *    - su QUALE GPIO si trova il LED (RGB o normale)
 *    - quali dispositivi I2C sono collegati
 *    - se il WiFi funziona
 *
 *  Come si usa:
 *  1. Carica lo sketch, apri il Monitor Seriale a 115200 baud
 *  2. Premi INVIO se non vedi nulla
 *  3. Digita una lettera dal menu e premi INVIO
 *
 *  Core richiesto: Arduino-ESP32 v3.x (testato su 3.3.x)
 *  Licenza: fai quello che vuoi.
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_system.h>

// ------------------------------------------------------------
//  CONFIGURAZIONE
// ------------------------------------------------------------

// GPIO dove cercare un LED RGB indirizzabile (WS2812 / NeoPixel).
// Sono i pin usati piu' spesso dai cloni ESP32-S3.
static const uint8_t RGB_CANDIDATES[]   = { 48, 38, 47, 21, 39, 40, 8 };

// GPIO dove cercare un LED normale (singolo colore).
static const uint8_t PLAIN_CANDIDATES[] = { 2, 1, 13, 15, 16, 17, 18 };

// Pin I2C di default piu' comuni sulle board S3.
#define I2C_SDA_DEFAULT 8
#define I2C_SCL_DEFAULT 9

// ATTENZIONE: i GPIO 26..37 sono cablati alla flash SPI e alla PSRAM
// octal. Toccarli fa crashare il chip. Non compaiono in nessuna lista.

// ------------------------------------------------------------
//  UTILITA'
// ------------------------------------------------------------

// NB: niente argomenti di default. L'IDE Arduino genera automaticamente
// i prototipi delle funzioni e con i default va in errore di compilazione.
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

// Scrive su un LED RGB indirizzabile usando il driver integrato nel core.
static void rgbWrite(uint8_t pin, uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(pin, r, g, b);   // core 3.x
#else
  neopixelWrite(pin, r, g, b); // core 2.x (nome vecchio della stessa funzione)
#endif
}

// Libera il canale RMT usato dal LED RGB (ce ne sono solo 4 sull'S3).
static void rgbRelease(uint8_t pin) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rmtDeinit(pin);
#else
  (void)pin;
#endif
}

// Svuota il buffer seriale in ingresso.
static void flushInput() {
  while (Serial.available()) Serial.read();
}

// ------------------------------------------------------------
//  1) INFORMAZIONI SUL CHIP
// ------------------------------------------------------------

static void showChipInfo() {
  title("INFORMAZIONI CHIP");

  Serial.printf("Modello .............. %s\n", ESP.getChipModel());
  // Da ESP-IDF 5.x la revisione e' codificata come major*100 + minor:
  // il valore grezzo 100 significa "v1.0", non "revisione 100".
  uint16_t rev = ESP.getChipRevision();
  Serial.printf("Revisione silicio .... v%u.%u\n",
                (unsigned)(rev / 100), (unsigned)(rev % 100));
  Serial.printf("Core ................. %d\n", ESP.getChipCores());
  Serial.printf("Frequenza CPU ........ %lu MHz\n", (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("Core Arduino ......... %d.%d.%d\n",
                ESP_ARDUINO_VERSION_MAJOR,
                ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("ESP-IDF .............. %s\n", ESP.getSdkVersion());

  line();

  uint32_t flashSize = ESP.getFlashChipSize();
  Serial.printf("Flash (dichiarata) ... %lu byte  = %lu MB\n",
                (unsigned long)flashSize,
                (unsigned long)(flashSize / (1024UL * 1024UL)));
  Serial.printf("Flash velocita' ...... %lu Hz\n", (unsigned long)ESP.getFlashChipSpeed());
  Serial.printf("Sketch occupa ........ %lu byte su %lu disponibili\n",
                (unsigned long)ESP.getSketchSize(),
                (unsigned long)(ESP.getSketchSize() + ESP.getFreeSketchSpace()));

  Serial.println();
  Serial.println("NOTA: se la flash risulta 4 MB ma hai comprato una N16,");
  Serial.println("      hai selezionato la Flash Size sbagliata nell'IDE.");

  line();

  size_t psram = ESP.getPsramSize();
  if (psram == 0) {
    Serial.println("PSRAM ................ NON rilevata");
    Serial.println();
    Serial.println("Puo' significare due cose:");
    Serial.println("  a) la tua board non ha PSRAM (es. modulo N8 senza R)");
    Serial.println("  b) ce l'ha, ma nell'IDE PSRAM e' su 'Disabled'");
    Serial.println("     -> prova 'OPI PSRAM' (moduli R8) o 'QSPI PSRAM' (R2)");
  } else {
    Serial.printf("PSRAM ................ %lu byte = %lu MB\n",
                  (unsigned long)psram,
                  (unsigned long)(psram / (1024UL * 1024UL)));
    Serial.printf("PSRAM libera ......... %lu byte\n", (unsigned long)ESP.getFreePsram());
  }

  Serial.printf("RAM interna libera ... %lu byte\n", (unsigned long)ESP.getFreeHeap());
  Serial.printf("Blocco max allocabile  %lu byte\n", (unsigned long)ESP.getMaxAllocHeap());

  line();

  uint64_t mac = ESP.getEfuseMac();
  uint8_t m[6];
  for (int i = 0; i < 6; i++) m[i] = (uint8_t)((mac >> (8 * i)) & 0xFF);
  Serial.printf("MAC base ............. %02X:%02X:%02X:%02X:%02X:%02X\n",
                m[0], m[1], m[2], m[3], m[4], m[5]);
  Serial.printf("ID univoco ........... %02X%02X%02X%02X%02X%02X\n",
                m[0], m[1], m[2], m[3], m[4], m[5]);

  Serial.println();
  Serial.println("Motivo dell'ultimo riavvio:");
  Serial.print("  ");
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  Serial.println("accensione (normale)"); break;
    case ESP_RST_EXT:      Serial.println("pulsante RESET"); break;
    case ESP_RST_SW:       Serial.println("riavvio software"); break;
    case ESP_RST_PANIC:    Serial.println("CRASH! (exception / panic)"); break;
    case ESP_RST_INT_WDT:  Serial.println("watchdog interrupt"); break;
    case ESP_RST_TASK_WDT: Serial.println("watchdog task"); break;
    case ESP_RST_BROWNOUT: Serial.println("BROWNOUT - alimentazione insufficiente!"); break;
    case ESP_RST_DEEPSLEEP:Serial.println("risveglio da deep sleep"); break;
    default:               Serial.println("sconosciuto"); break;
  }
}

// ------------------------------------------------------------
//  2) TEST PSRAM
// ------------------------------------------------------------

static void testPsram() {
  title("TEST PSRAM");

  if (ESP.getPsramSize() == 0) {
    Serial.println("Nessuna PSRAM rilevata: niente da testare.");
    Serial.println("Se sei sicuro di averla, controlla le impostazioni della board.");
    return;
  }

  const size_t N = 64 * 1024;  // 64 KB
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
    Serial.println("La PSRAM e' configurata e funzionante.");
  } else {
    Serial.printf("%u ERRORI su %u byte!\n", (unsigned)errors, (unsigned)N);
    Serial.println("Probabile modalita' sbagliata: se hai un modulo R8 serve");
    Serial.println("'OPI PSRAM', non 'QSPI PSRAM' (o viceversa per gli R2).");
  }

  free(buf);
}

// ------------------------------------------------------------
//  3) CACCIA AL LED RGB
// ------------------------------------------------------------

static void huntRgbLed() {
  title("CACCIA AL LED RGB");

  Serial.println("Provo un LED WS2812 su ogni GPIO candidato.");
  Serial.println("GUARDA LA BOARD: quando vedi accendersi qualcosa,");
  Serial.println("segnati il numero di GPIO che sto annunciando.");
  Serial.println();
  Serial.println("Ogni pin fa: ROSSO -> VERDE -> BLU -> spento (3 secondi)");
  line();
  delay(1500);

  const size_t n = sizeof(RGB_CANDIDATES) / sizeof(RGB_CANDIDATES[0]);
  for (size_t i = 0; i < n; i++) {
    uint8_t pin = RGB_CANDIDATES[i];
    Serial.printf(">>> GPIO %2u  ", pin);

    Serial.print("[ROSSO] ");
    rgbWrite(pin, 60, 0, 0);
    delay(800);

    Serial.print("[VERDE] ");
    rgbWrite(pin, 0, 60, 0);
    delay(800);

    Serial.print("[BLU] ");
    rgbWrite(pin, 0, 0, 60);
    delay(800);

    rgbWrite(pin, 0, 0, 0);
    Serial.println("[spento]");

    // IMPORTANTE: rgbLedWrite() occupa un canale RMT e non lo rilascia.
    // L'ESP32-S3 ne ha solo 4 in trasmissione: senza questa riga gli ultimi
    // pin della lista verrebbero saltati in silenzio.
    rgbRelease(pin);

    delay(400);
  }

  line();
  Serial.println("Caccia finita.");
  Serial.println("Se non si e' acceso niente: la board potrebbe avere un LED");
  Serial.println("normale invece che RGB. Prova l'opzione 'l' dal menu.");
  Serial.println();
  Serial.println("Se hai trovato il pin, nel tuo codice userai:");
  Serial.println("   rgbLedWrite(<PIN>, r, g, b);   // valori 0-255");
}

// ------------------------------------------------------------
//  4) CACCIA AL LED NORMALE
// ------------------------------------------------------------

static void huntPlainLed() {
  title("CACCIA AL LED NORMALE");

  Serial.println("Provo un LED singolo su ogni GPIO candidato.");
  Serial.println("Ogni pin lampeggia 6 volte veloce.");
  Serial.println();
  Serial.println("ATTENZIONE: molti LED integrati sono a logica INVERTITA");
  Serial.println("(LOW = acceso). Faccio entrambe le polarita'.");
  line();
  delay(1500);

  const size_t n = sizeof(PLAIN_CANDIDATES) / sizeof(PLAIN_CANDIDATES[0]);
  for (size_t i = 0; i < n; i++) {
    uint8_t pin = PLAIN_CANDIDATES[i];
    Serial.printf(">>> GPIO %2u lampeggia...\n", pin);

    pinMode(pin, OUTPUT);
    for (int k = 0; k < 6; k++) {
      digitalWrite(pin, HIGH);
      delay(150);
      digitalWrite(pin, LOW);
      delay(150);
    }
    // Lascio il pin in alta impedenza per non disturbare altro hardware.
    pinMode(pin, INPUT);
    delay(500);
  }

  line();
  Serial.println("Caccia finita.");
  Serial.println("Se hai trovato il pin, nel tuo codice userai:");
  Serial.println("   pinMode(<PIN>, OUTPUT);");
  Serial.println("   digitalWrite(<PIN>, HIGH);");
}

// ------------------------------------------------------------
//  5) SCANSIONE I2C
// ------------------------------------------------------------

static void describeI2cDevice(uint8_t addr) {
  switch (addr) {
    case 0x3C: case 0x3D: Serial.print("  <- probabile display OLED SSD1306/SH1106"); break;
    case 0x27: case 0x3F: Serial.print("  <- probabile LCD 16x2 con backpack PCF8574"); break;
    case 0x68: Serial.print("  <- probabile MPU6050 / DS3231 / DS1307"); break;
    case 0x76: case 0x77: Serial.print("  <- probabile BMP280 / BME280"); break;
    case 0x40: Serial.print("  <- probabile SHT21 / INA219 / PCA9685"); break;
    case 0x48: Serial.print("  <- probabile ADS1115 / LM75"); break;
    case 0x23: Serial.print("  <- probabile BH1750 (luce)"); break;
    case 0x5A: Serial.print("  <- probabile MLX90614 / CCS811"); break;
    case 0x57: Serial.print("  <- probabile EEPROM AT24C32 / MAX30102"); break;
    default: break;
  }
}

static void scanI2C(uint8_t sda, uint8_t scl) {
  title("SCANSIONE BUS I2C");

  Serial.printf("Uso SDA = GPIO %u, SCL = GPIO %u\n", sda, scl);
  Serial.println("(se hai cablato pin diversi, cambiali in cima allo sketch)");
  line();

  static bool i2cStarted = false;
  if (i2cStarted) Wire.end();
  i2cStarted = true;

  if (!Wire.begin(sda, scl)) {
    Serial.println("Impossibile inizializzare il bus I2C su questi pin.");
    return;
  }
  Wire.setClock(100000);

  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("Trovato dispositivo a 0x%02X (decimale %u)", addr, addr);
      describeI2cDevice(addr);
      Serial.println();
      found++;
      delay(5);
    }
  }

  line();
  if (found == 0) {
    Serial.println("Nessun dispositivo trovato.");
    Serial.println();
    Serial.println("Se ti aspettavi qualcosa, controlla nell'ordine:");
    Serial.println("  1. alimentazione del sensore (3V3, NON 5V sull'S3)");
    Serial.println("  2. SDA e SCL non invertiti");
    Serial.println("  3. GND in comune tra board e sensore");
    Serial.println("  4. resistenze di pull-up (molti moduli le hanno gia')");
  } else {
    Serial.printf("Totale: %u dispositivo/i.\n", found);
  }
}

// ------------------------------------------------------------
//  6) SCANSIONE WIFI
// ------------------------------------------------------------

static void scanWiFi() {
  title("SCANSIONE RETI WIFI");

  Serial.println("Serve a verificare che la radio e l'antenna funzionino.");
  Serial.println("Attendi qualche secondo...");
  Serial.println();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();

  if (n <= 0) {
    Serial.println("Nessuna rete trovata.");
    Serial.println("Se sei sicuro che ci siano reti intorno, l'antenna");
    Serial.println("potrebbe essere staccata o mal saldata.");
  } else {
    Serial.printf("Trovate %d reti:\n\n", n);
    Serial.println("  #  RSSI  Ch  Sicurezza      SSID");
    line();
    for (int i = 0; i < n; i++) {
      const char *sec;
      switch (WiFi.encryptionType(i)) {
        case WIFI_AUTH_OPEN:         sec = "APERTA"; break;
        case WIFI_AUTH_WEP:          sec = "WEP"; break;
        case WIFI_AUTH_WPA_PSK:      sec = "WPA"; break;
        case WIFI_AUTH_WPA2_PSK:     sec = "WPA2"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: sec = "WPA/WPA2"; break;
        case WIFI_AUTH_WPA3_PSK:     sec = "WPA3"; break;
        default:                     sec = "altro"; break;
      }
      Serial.printf("%3d  %4d  %2d  %-13s  %s\n",
                    i + 1, WiFi.RSSI(i), WiFi.channel(i), sec, WiFi.SSID(i).c_str());
    }
    line();
    Serial.println("RSSI: -30 ottimo, -67 buono, -80 debole, -90 inutilizzabile");
  }

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
}

// ------------------------------------------------------------
//  MENU
// ------------------------------------------------------------

static void showMenu() {
  Serial.println();
  lineC('*');
  Serial.println("  MENU - digita una lettera e premi INVIO");
  lineC('*');
  Serial.println("  i  Informazioni sul chip (flash, PSRAM, MAC, ...)");
  Serial.println("  p  Test read/write della PSRAM");
  Serial.println("  r  Caccia al LED RGB (WS2812)");
  Serial.println("  l  Caccia al LED normale");
  Serial.println("  s  Scansione bus I2C");
  Serial.println("  w  Scansione reti WiFi");
  Serial.println("  b  Riavvia la board");
  Serial.println("  h  Rimostra questo menu");
  lineC('*');
  Serial.print("> ");
}

// ------------------------------------------------------------
//  SETUP / LOOP
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Con USB CDC nativo la porta impiega un attimo a presentarsi.
  // Aspetto al massimo 3 secondi, poi vado avanti comunque:
  // cosi' lo sketch funziona sia su porta USB nativa che su UART.
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) {
    delay(50);
  }
  delay(300);

  Serial.println();
  Serial.println();
  lineC('#');
  Serial.println("   BoardDetective - diagnostica ESP32-S3");
  lineC('#');
  Serial.println();
  Serial.println("Se stai leggendo questo, la seriale funziona. Ottimo inizio.");

  showChipInfo();
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
    case 'r': case 'R': huntRgbLed(); break;
    case 'l': case 'L': huntPlainLed(); break;
    case 's': case 'S': scanI2C(I2C_SDA_DEFAULT, I2C_SCL_DEFAULT); break;
    case 'w': case 'W': scanWiFi(); break;
    case 'h': case 'H': break;
    case 'b': case 'B':
      Serial.println("Riavvio tra 1 secondo...");
      delay(1000);
      ESP.restart();
      break;
    default:
      Serial.printf("Comando '%c' sconosciuto.\n", c);
      break;
  }

  showMenu();
}
