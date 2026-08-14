/*
 * ============================================================
 *  LIVELLA A BOLLA - Waveshare ESP32-S3-Touch-AMOLED-1.8
 * ============================================================
 *
 *  Una bolla che scivola sullo schermo seguendo l'inclinazione
 *  reale della board, letta dall'accelerometro QMI8658.
 *
 *  Appoggia la board su un tavolo: la bolla va al centro e
 *  diventa verde. Inclinala: scappa in salita, come in una
 *  livella vera.
 *
 *  Premi il pulsante BOOT per spegnere e riaccendere lo schermo.
 *
 *  COME FUNZIONA
 *  L'accelerometro non misura l'inclinazione: misura
 *  l'accelerazione. Ma da fermo l'unica accelerazione presente
 *  e' la gravita', che punta sempre verso il basso: da come si
 *  distribuisce sui tre assi si ricava di quanto e' inclinato
 *  l'oggetto. E' lo stesso trucco con cui il telefono capisce
 *  se lo tieni in verticale.
 *
 *  Nessuna libreria oltre a quelle gia' nel progetto: il sensore
 *  si pilota scrivendo direttamente nei suoi registri via I2C.
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

// ------------------------------------------------------------
//  PIN (dal repository ufficiale Waveshare)
// ------------------------------------------------------------

#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK   11
#define LCD_CS     12
#define LCD_W     368
#define LCD_H     448

#define I2C_SDA    15
#define I2C_SCL    14

// Il pulsante BOOT e' il GPIO 0. Serve al chip solo durante l'avvio:
// una volta partito lo sketch si puo' leggere come un normale pulsante.
#define BTN_BOOT    0

// ------------------------------------------------------------
//  ACCELEROMETRO QMI8658
// ------------------------------------------------------------

#define QMI_ADDR       0x6B  // indirizzo sul bus I2C
#define QMI_WHO_AM_I   0x00  // deve rispondere 0x05
#define QMI_CTRL1      0x02
#define QMI_CTRL2      0x03
#define QMI_CTRL7      0x08
#define QMI_CTRL8      0x09
#define QMI_AX_L       0x35  // da qui 6 byte: X, Y, Z (interi a 16 bit)
#define QMI_RST_RESULT 0x4D  // dopo il reset deve valere 0x80
#define QMI_RESET      0x60

// Fondoscala 4g -> quanti g vale un singolo passo del sensore
static const float ACC_SCALE = 4.0f / 32768.0f;

static void qmiWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static bool qmiRead(uint8_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)QMI_ADDR, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static bool qmiBegin() {
  // 1. SOFT RESET.
  // Indispensabile: senza, il sensore risponde all'appello ma resta in
  // uno stato in cui non genera campioni, e tutte le letture tornano
  // zero. E' il passo che manca in quasi tutti gli esempi in giro.
  qmiWrite(QMI_RESET, 0xB0);

  bool resetOk = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 500) {
    delay(10);
    uint8_t r = 0;
    if (qmiRead(QMI_RST_RESULT, &r, 1) && r == 0x80) {
      resetOk = true;
      break;
    }
  }
  Serial.printf("QMI8658: reset %s\n", resetOk ? "confermato" : "NON confermato");

  // 2. Indirizzamento automatico: permette di leggere i 6 byte dei tre
  //    assi in un colpo solo invece che uno per volta.
  qmiWrite(QMI_CTRL1, 0x40);
  delay(10);

  // 3. Verifica dell'identita' del chip
  uint8_t id = 0;
  if (!qmiRead(QMI_WHO_AM_I, &id, 1)) {
    Serial.println("QMI8658: nessuna risposta sul bus I2C.");
    return false;
  }
  Serial.printf("QMI8658: WHO_AM_I = 0x%02X (atteso 0x05)\n", id);
  if (id != 0x05) return false;

  // 4. Handshake interno richiesto dal costruttore
  qmiWrite(QMI_CTRL8, 0x80);
  delay(10);

  // 5. Fondoscala 4g, 125 campioni al secondo
  qmiWrite(QMI_CTRL2, 0x16);
  delay(10);

  // 6. Accende l'accelerometro
  qmiWrite(QMI_CTRL7, 0x01);
  delay(100);

  return true;
}

// Restituisce l'accelerazione sui tre assi, in g.
static bool qmiReadAccel(float *ax, float *ay, float *az) {
  uint8_t b[6];
  if (!qmiRead(QMI_AX_L, b, 6)) return false;
  *ax = (int16_t)(b[0] | (b[1] << 8)) * ACC_SCALE;
  *ay = (int16_t)(b[2] | (b[3] << 8)) * ACC_SCALE;
  *az = (int16_t)(b[4] | (b[5] << 8)) * ACC_SCALE;
  return true;
}

// ------------------------------------------------------------
//  PMIC AXP2101 - stato della batteria
// ------------------------------------------------------------
//  E' il chip che gestisce l'alimentazione della board. Sta sullo
//  stesso bus I2C dell'accelerometro, a un indirizzo diverso, e ha
//  un misuratore di carica integrato: non stimiamo noi la
//  percentuale a partire dalla tensione, la calcola lui.
// ------------------------------------------------------------

#define AXP_ADDR        0x34
#define AXP_STATUS1     0x00  // bit 3: batteria collegata
#define AXP_STATUS2     0x01  // bit 7-5: stato di carica
#define AXP_GAUGE_CTRL  0x18  // bit 3: accende il misuratore di carica
#define AXP_ADC_CTRL    0x30  // bit 0: accende la misura di tensione
#define AXP_VBAT_H      0x34  // 5 bit alti della tensione
#define AXP_VBAT_L      0x35  // 8 bit bassi
#define AXP_BAT_DET     0x68  // bit 0: accende il rilevamento batteria
#define AXP_BAT_PERCENT 0xA4  // percentuale di carica, gia' calcolata

static void axpWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(AXP_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static int axpRead(uint8_t reg) {
  Wire.beginTransmission(AXP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)AXP_ADDR, 1) != 1) return -1;
  return Wire.read();
}

// Accende un singolo bit lasciando gli altri come stanno.
static void axpSetBit(uint8_t reg, uint8_t bit) {
  int v = axpRead(reg);
  if (v < 0) return;
  axpWrite(reg, (uint8_t)(v | (1 << bit)));
}

static bool axpBegin() {
  if (axpRead(AXP_STATUS1) < 0) {
    Serial.println("AXP2101: nessuna risposta sul bus I2C.");
    return false;
  }
  axpSetBit(AXP_BAT_DET, 0);     // rileva se c'e' una batteria
  axpSetBit(AXP_ADC_CTRL, 0);    // misura la tensione di batteria
  axpSetBit(AXP_GAUGE_CTRL, 3);  // accende il misuratore di carica
  delay(50);
  Serial.println("AXP2101: pronto.");
  return true;
}

// Stadi del caricabatterie, letti dai bit 2-0 di STATUS2
#define CHG_TRICKLE  0
#define CHG_PRE      1
#define CHG_CC       2   // corrente costante: la fase principale
#define CHG_CV       3   // tensione costante: fase finale
#define CHG_DONE     4
#define CHG_STOP     5

struct StatoBatteria {
  bool presente;    // c'e' una batteria collegata?
  bool usb;         // il cavo USB sta alimentando?
  bool inCarica;    // sta effettivamente entrando corrente?
  int  stadio;      // vedi CHG_* qui sopra
  int  percento;    // -1 se non disponibile
  int  milliVolt;   // 0 se non disponibile
};

static StatoBatteria axpLeggiBatteria() {
  StatoBatteria b = {false, false, false, CHG_STOP, -1, 0};

  int s1 = axpRead(AXP_STATUS1);
  if (s1 < 0) return b;
  b.presente = (s1 >> 3) & 0x01;   // bit 3: batteria collegata
  b.usb      = (s1 >> 5) & 0x01;   // bit 5: alimentazione USB presente

  int s2 = axpRead(AXP_STATUS2);
  if (s2 >= 0) {
    // I bit 6-5 dicono in che verso scorre la corrente nella batteria:
    //   0 = ferma, 1 = in ingresso (carica), 2 = in uscita (scarica)
    b.inCarica = (((s2 >> 5) & 0x03) == 0x01);
    // I bit 2-0 dicono a che punto e' il ciclo di carica
    b.stadio = s2 & 0x07;
  }

  if (b.presente) {
    int p = axpRead(AXP_BAT_PERCENT);
    if (p >= 0 && p <= 100) b.percento = p;

    int h = axpRead(AXP_VBAT_H);
    int l = axpRead(AXP_VBAT_L);
    if (h >= 0 && l >= 0) b.milliVolt = ((h & 0x1F) << 8) | l;
  }
  return b;
}

// ------------------------------------------------------------
//  DISPLAY
// ------------------------------------------------------------

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

static Arduino_SH8601 *panel = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED, 0, LCD_W, LCD_H);

// Disegniamo su un foglio in memoria e lo riversiamo a schermo tutto
// insieme: cosi' l'animazione non sfarfalla. Sono 368*448*2 = 329 KB,
// che stanno solo in PSRAM. Ecco a cosa servivano quegli 8 MB.
static Arduino_Canvas *gfx = new Arduino_Canvas(LCD_W, LCD_H, panel);

#define LUMINOSITA 200   // 0-255

static bool schermoAcceso = true;

// ------------------------------------------------------------
//  GEOMETRIA
// ------------------------------------------------------------

#define CX        184   // centro della livella
#define CY        210
#define R_VIAL    150   // raggio della "fiala"
#define R_TARGET   28   // cerchio centrale: dentro qui sei in bolla
#define R_BUBBLE   22

// Quanti pixel di spostamento per ogni g di inclinazione
#define SENSIBILITA 340.0f

// Se la bolla va dalla parte sbagliata, cambia il segno di quello
// storto. Dipende da come tieni la board rispetto allo schermo.
#define SEGNO_X   (-1.0f)
#define SEGNO_Y   (+1.0f)

// Valori filtrati, per evitare che la bolla tremi
static float fx = 0, fy = 0;

// Stato batteria: si aggiorna ogni due secondi, non a ogni fotogramma.
// Le letture I2C costano, e la carica non cambia 50 volte al secondo.
static StatoBatteria batteria = {false, false, false, CHG_STOP, -1, 0};

// Indicatore di batteria, in alto a destra.
#define BAT_X   300
#define BAT_Y    12
#define BAT_W    45
#define BAT_H    17
#define BAT_DX  360   // bordo destro per l'allineamento dei testi

// Scrive una riga allineata a destra rispetto a BAT_DX. Il font base
// e' largo 6 pixel per carattere, moltiplicato per la dimensione.
static void printRight(const char *txt, int y, uint8_t size, uint16_t col) {
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(BAT_DX - (int)strlen(txt) * 6 * size, y);
  gfx->print(txt);
}

// Un fulmine, disegnato con due triangoli. Rende lo stato "in carica"
// riconoscibile a colpo d'occhio, senza dover leggere.
static void drawFulmine(int cx, int cy, uint16_t col) {
  gfx->fillTriangle(cx + 2, cy - 6, cx - 3, cy + 1, cx + 1, cy + 1, col);
  gfx->fillTriangle(cx - 2, cy + 6, cx + 3, cy - 1, cx - 1, cy - 1, col);
}

// Marchio in alto a sinistra.
#define LOGO_X   16
#define LOGO_Y   12

static void drawLogo() {
  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(LOGO_X, LOGO_Y);
  gfx->print("MTY");
}

static void drawBatteria(const StatoBatteria &b) {
  // Contorno e polo positivo
  gfx->drawRect(BAT_X, BAT_Y, BAT_W, BAT_H, RGB565_WHITE);
  gfx->fillRect(BAT_X + BAT_W, BAT_Y + 5, 3, 7, RGB565_WHITE);

  if (!b.presente) {
    // Nessuna batteria collegata: la board vive di USB
    printRight("USB", BAT_Y + 5, 1, RGB565_DARKGREY);
    printRight("nessuna batteria", BAT_Y + BAT_H + 6, 1, RGB565_DARKGREY);
    return;
  }

  int p = (b.percento >= 0) ? b.percento : 0;

  uint16_t col;
  if (b.inCarica)   col = RGB565_CYAN;
  else if (p <= 20) col = RGB565_RED;
  else if (p <= 50) col = RGB565_YELLOW;
  else              col = RGB565_GREEN;

  // Riempimento proporzionale alla carica
  int pieno = ((BAT_W - 4) * p) / 100;
  if (pieno > 0) {
    gfx->fillRect(BAT_X + 2, BAT_Y + 2, pieno, BAT_H - 4, col);
  }

  // Fulmine al centro quando sta caricando
  if (b.inCarica) {
    drawFulmine(BAT_X + BAT_W / 2, BAT_Y + BAT_H / 2, RGB565_BLACK);
  }

  char riga[24];

  // Percentuale
  if (b.percento >= 0) snprintf(riga, sizeof(riga), "%d%%", b.percento);
  else                 snprintf(riga, sizeof(riga), "--%%");
  printRight(riga, BAT_Y + BAT_H + 6, 1, RGB565_WHITE);

  // Riga di stato: e' la risposta a "sta caricando o no?"
  const char *stato;
  uint16_t colStato;
  if (b.inCarica) {
    stato = (b.stadio == CHG_CV) ? "in carica (fine)" : "IN CARICA";
    colStato = RGB565_CYAN;
  } else if (b.usb && b.stadio == CHG_DONE) {
    stato = "carica completa";
    colStato = RGB565_GREEN;
  } else if (b.usb) {
    stato = "collegata a USB";
    colStato = RGB565_WHITE;
  } else {
    stato = "a batteria";
    colStato = RGB565_DARKGREY;
  }
  printRight(stato, BAT_Y + BAT_H + 18, 1, colStato);

  if (b.milliVolt > 0) {
    snprintf(riga, sizeof(riga), "%d.%02d V",
             b.milliVolt / 1000, (b.milliVolt % 1000) / 10);
    printRight(riga, BAT_Y + BAT_H + 30, 1, RGB565_DARKGREY);
  }
}

// ------------------------------------------------------------
//  DISEGNO
// ------------------------------------------------------------

static void drawScene(float ax, float ay, float az) {
  // Filtro passa-basso: ogni nuova lettura pesa il 20%. Senza, il
  // rumore del sensore farebbe vibrare la bolla di continuo.
  fx = fx * 0.8f + ax * 0.2f;
  fy = fy * 0.8f + ay * 0.2f;

  float dx = SEGNO_X * fx * SENSIBILITA;
  float dy = SEGNO_Y * fy * SENSIBILITA;

  // La bolla non puo' uscire dalla fiala
  float dist = sqrtf(dx * dx + dy * dy);
  float limite = R_VIAL - R_BUBBLE - 4;
  if (dist > limite) {
    dx = dx * limite / dist;
    dy = dy * limite / dist;
  }

  // Inclinazione in gradi, ricavata dalla gravita'
  float roll  = atan2f(ax, az) * 180.0f / PI;
  float pitch = atan2f(ay, az) * 180.0f / PI;

  bool inBolla = (sqrtf(fx * fx + fy * fy) * SENSIBILITA) < (R_TARGET - 6);

  gfx->fillScreen(RGB565_BLACK);

  // Fiala: cerchi concentrici come riferimento visivo
  gfx->drawCircle(CX, CY, R_VIAL, RGB565_DARKGREY);
  gfx->drawCircle(CX, CY, R_VIAL - 1, RGB565_DARKGREY);
  gfx->drawCircle(CX, CY, (R_VIAL * 2) / 3, RGB565_DARKGREY);

  // Croce centrale
  gfx->drawFastHLine(CX - R_VIAL, CY, R_VIAL * 2, RGB565_DARKGREY);
  gfx->drawFastVLine(CX, CY - R_VIAL, R_VIAL * 2, RGB565_DARKGREY);

  // Bersaglio: verde quando sei in bolla
  uint16_t colTarget = inBolla ? RGB565_GREEN : RGB565_DARKGREY;
  gfx->drawCircle(CX, CY, R_TARGET, colTarget);
  gfx->drawCircle(CX, CY, R_TARGET - 1, colTarget);

  // La bolla
  int bx = CX + (int)dx;
  int by = CY + (int)dy;
  uint16_t colBubble = inBolla ? RGB565_GREEN : RGB565_CYAN;
  gfx->fillCircle(bx, by, R_BUBBLE, colBubble);
  gfx->fillCircle(bx - 6, by - 6, 6, RGB565_WHITE);  // riflesso

  // Valori grezzi del sensore, utili per capire se sta leggendo davvero
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setCursor(20, 372);
  gfx->printf("grezzi  ax %+.3f  ay %+.3f  az %+.3f g", ax, ay, az);

  // Inclinazione in gradi
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(20, 386);
  gfx->printf("X %+6.1f", roll);
  gfx->setCursor(200, 386);
  gfx->printf("Y %+6.1f", pitch);

  gfx->setTextSize(3);
  gfx->setCursor(20, 414);
  if (inBolla) {
    gfx->setTextColor(RGB565_GREEN);
    gfx->print("IN BOLLA");
  } else {
    gfx->setTextColor(RGB565_DARKGREY);
    gfx->print("inclinata");
  }

  drawLogo();
  drawBatteria(batteria);

  gfx->flush();
}

// ------------------------------------------------------------
//  PULSANTE BOOT: accende e spegne lo schermo
// ------------------------------------------------------------

static void gestisciPulsante() {
  static int precedente = HIGH;
  static uint32_t ultimoCambio = 0;

  int ora = digitalRead(BTN_BOOT);

  // Il filtro sui 300 ms non e' decorativo: i contatti meccanici
  // "rimbalzano" per qualche millisecondo alla pressione, generando
  // decine di transizioni. Senza, un tocco solo verrebbe letto come
  // venti e lo schermo lampeggerebbe invece di cambiare stato.
  if (precedente == HIGH && ora == LOW && (millis() - ultimoCambio) > 300) {
    ultimoCambio = millis();
    schermoAcceso = !schermoAcceso;

    if (schermoAcceso) {
      panel->displayOn();
      panel->setBrightness(LUMINOSITA);
      Serial.println("Schermo acceso");
    } else {
      panel->displayOff();
      Serial.println("Schermo spento (premi BOOT per riaccendere)");
    }
  }

  precedente = ora;
}

// ------------------------------------------------------------
//  SETUP / LOOP
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 2000)) delay(50);

  Serial.println("\nLivella a bolla - QMI8658 + AMOLED");
  Serial.println("Premi BOOT per spegnere/riaccendere lo schermo.");

  pinMode(BTN_BOOT, INPUT_PULLUP);

  if (!gfx->begin()) {
    Serial.println("Display: init fallita.");
    Serial.println("Quasi certamente manca la PSRAM: il foglio di disegno");
    Serial.println("occupa 329 KB e in RAM interna non ci starebbe mai.");
    return;
  }
  panel->setBrightness(LUMINOSITA);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  axpBegin();
  batteria = axpLeggiBatteria();

  if (!qmiBegin()) {
    Serial.println("Accelerometro non inizializzato.");
    gfx->fillScreen(RGB565_BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_RED);
    gfx->setCursor(20, 200);
    gfx->println("QMI8658 non risponde");
    gfx->flush();
    return;
  }

  Serial.println("Accelerometro pronto. Inclina la board.");
}

void loop() {
  gestisciPulsante();

  // Schermo spento: niente da disegnare. Saltando il ridisegno il chip
  // resta in idle invece di ricalcolare 50 volte al secondo
  // un'immagine che nessuno vede.
  if (!schermoAcceso) {
    delay(50);
    return;
  }

  // Aggiorna lo stato della batteria ogni due secondi
  static uint32_t ultimaBatteria = 0;
  if (millis() - ultimaBatteria > 2000) {
    ultimaBatteria = millis();
    batteria = axpLeggiBatteria();
  }

  float ax, ay, az;
  if (qmiReadAccel(&ax, &ay, &az)) {
    drawScene(ax, ay, az);

    // Eco sulla seriale due volte al secondo, per diagnosi.
    // Con la board ferma e in piano ci si aspetta az vicino a 1.000
    // (tutta la gravita' sull'asse perpendicolare allo schermo) e gli
    // altri due vicini a zero. Se sono tutti e tre zero, il sensore
    // non sta generando campioni.
    static uint32_t ultimoLog = 0;
    if (millis() - ultimoLog > 500) {
      ultimoLog = millis();
      Serial.printf("ax %+.3f  ay %+.3f  az %+.3f  (g)\n", ax, ay, az);
    }
  }

  delay(20);  // circa 50 fotogrammi al secondo
}