/*
 * ============================================================
 *  DOT CLOCK - Waveshare ESP32-S3-Touch-AMOLED-1.8
 * ============================================================
 *
 *  Un orologio da tavolo a schede. Scorri con il dito a destra e
 *  a sinistra per passare da una all'altra:
 *
 *    1. ORA       l'ora di Roma, la data, i secondi che scorrono
 *    2. SOLE      quanto manca al tramonto, alba e tramonto
 *    3. DOMANI    che tempo fara' domani
 *
 *  Tutto e' disegnato a matrice di punti, come un pannello a led.
 *
 *  COME FA A SCORRERE COSI'
 *  Il trucco non e' disegnare veloce: e' disegnare in anticipo.
 *  Ogni scheda vive gia' pronta in un suo foglio di memoria, in
 *  PSRAM. Durante lo scorrimento non ridisegniamo niente: copiamo
 *  due fogli affiancati dentro un terzo, spostati di quanto si e'
 *  mosso il dito, e mandiamo quello allo schermo. Copiare 300 KB
 *  di memoria costa molto meno che ricalcolare qualche migliaio
 *  di pallini, e il risultato e' che il contenuto sta incollato
 *  al dito invece di inseguirlo.
 *
 *  Al rilascio il movimento non si ferma di colpo: se hai dato una
 *  spinta decisa la scheda prosegue da sola, altrimenti torna
 *  indietro. E' la stessa fisica di uno smartphone, ed e' quello
 *  che rende lo scorrimento "giusto" invece che meccanico.
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

#include "dotmatrix.h"
#include "touch.h"
#include "rtc.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
// Nessun secrets.h: il programma gira lo stesso, ma senza rete.
// Copia secrets.h.example in secrets.h per avere ora esatta e meteo.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

// ------------------------------------------------------------
//  DOVE SIAMO
// ------------------------------------------------------------
//  Questa scheda non ha il GPS: non c'e' nel suo pinout, non e'
//  una dimenticanza. Per un orologio da tavolo va benissimo cosi',
//  la posizione la sappiamo gia' e non cambia.

#define LAT 41.9028   // Roma
#define LON 12.4964
#define CITTA "ROMA"

// La regola dell'ora legale europea, scritta nel formato che usa
// il sistema: un'ora avanti dall'ultima domenica di marzo
// all'ultima di ottobre. La gestisce da solo, non serve toccarla.
#define FUSO "CET-1CEST,M3.5.0,M10.5.0/3"

// ------------------------------------------------------------
//  PIN
// ------------------------------------------------------------

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_W 368
#define LCD_H 448

#define I2C_SDA 15
#define I2C_SCL 14

#define BTN_BOOT 0
#define AXP_ADDR 0x34

#define LUMINOSITA 190   // 0-255

// ------------------------------------------------------------
//  COLORI
// ------------------------------------------------------------
//  Il display e' AMOLED: il nero non e' un colore scuro, e' un
//  pixel spento. Per questo lo sfondo resta nero pieno, e i punti
//  spenti dei quadranti sono grigio ferro invece che invisibili.

#define COL_SFONDO 0x0000
#define COL_ACCESO 0xFFFF   // bianco
#define COL_SPENTO 0x39C7   // grigio ferro, il punto che non brilla
#define COL_ETICHETTA 0x630C
#define COL_SECONDARIO 0x9CD3
#define COL_ROSSO 0xE923    // l'unico accento
#define COL_CORNICE 0x2124

// ------------------------------------------------------------
//  DISPLAY
// ------------------------------------------------------------

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

static Arduino_SH8601 *panel = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED, 0, LCD_W, LCD_H);

// Il foglio che finisce davvero a schermo.
static Arduino_Canvas *comp = new Arduino_Canvas(LCD_W, LCD_H, panel);

// ------------------------------------------------------------
//  LE SCHEDE
// ------------------------------------------------------------

#define N_SCHEDE 3
#define SCHEDA_ORA 0
#define SCHEDA_SOLE 1
#define SCHEDA_METEO 2

// Ogni scheda ha il suo foglio di memoria, sempre pronto.
// Sono 329 KB l'uno: con 8 MB di PSRAM ce ne stanno una ventina,
// quindi aggiungere schede in futuro non e' un problema.
// L'output e' nullptr perche' questi fogli non vanno mai a
// schermo da soli: passano sempre da "comp".
static Arduino_Canvas *scheda[N_SCHEDE] = {
    new Arduino_Canvas(LCD_W, LCD_H, nullptr),
    new Arduino_Canvas(LCD_W, LCD_H, nullptr),
    new Arduino_Canvas(LCD_W, LCD_H, nullptr),
};
static bool daRidisegnare[N_SCHEDE] = {true, true, true};

// ------------------------------------------------------------
//  STATO
// ------------------------------------------------------------

static Touch touch;
static bool touchOk = false;

static int schedaCorrente = 0;

// Quanto siamo spostati rispetto alla scheda corrente, in pixel.
// Positivo = stiamo scivolando verso la scheda successiva.
static float scorrimento = 0;

static bool ditoGiu = false;
static int16_t ditoPartenzaX = 0;
static float scorrimentoPartenza = 0;
static int16_t ditoUltimaX = 0;
static uint32_t ditoUltimoT = 0;
static float velocita = 0;      // pixel al millisecondo
static bool eraTap = true;      // il dito si e' mosso poco: e' un tocco, non uno scorrimento

static bool animazioneAttiva = false;
static float animDa = 0, animA = 0;
static uint32_t animInizio = 0;
static uint16_t animDurata = 0;
static int animDirezione = 0;

static bool retePresente = false;
static bool oraSincronizzata = false;
static int batteria = -1;
static bool alimentato = false;

static int ultimoSecondo = -1;
static int ultimoMinuto = -1;
static uint32_t prossimoMeteo = 0;

// ------------------------------------------------------------
//  METEO
// ------------------------------------------------------------

struct Meteo
{
    bool valido = false;
    int codice = 0;
    float tmax = 0, tmin = 0;
    int pioggia = 0;   // probabilita' in percentuale
};
static Meteo domani;

// ------------------------------------------------------------
//  BATTERIA (AXP2101)
// ------------------------------------------------------------

static uint8_t axpLeggi(uint8_t reg)
{
    Wire.beginTransmission(AXP_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((uint8_t)AXP_ADDR, (uint8_t)1) != 1) return 0;
    return Wire.read();
}

// Chi c'e' sul bus. Serve solo a capire, quando qualcosa non va,
// se il chip che cerchiamo e' presente o proprio assente.
static void scanI2C()
{
    Serial.print("[i2c] chip presenti:");
    for (uint8_t a = 1; a < 127; ++a)
    {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0)
            Serial.printf(" 0x%02X", a);
    }
    Serial.println();
}

static void aggiornaBatteria()
{
    // Il chip la percentuale la calcola gia' lui e la tiene nel
    // registro 0xA4. Stimarla dalla tensione darebbe un numero
    // peggiore: la curva di scarica di una litio e' quasi piatta
    // per gran parte del ciclo.
    uint8_t p = axpLeggi(0xA4);
    if (p <= 100) batteria = p;
    alimentato = (axpLeggi(0x00) & 0x20) != 0;   // bit 5: il cavo c'e'
}

// ------------------------------------------------------------
//  ALBA E TRAMONTO
// ------------------------------------------------------------
//  Non serve chiederlo a internet: dipende solo da dove sei e da
//  che giorno e'. E' la stessa formula degli almanacchi nautici,
//  con la Terra ridotta a un'orbita circolare corretta da due
//  termini. Sbaglia di meno di un minuto, che per sapere quando
//  accendere la luce e' piu' che sufficiente.
//
//  Restituisce l'ora locale in formato decimale: 6.5 = le 06:30.

static float gradiSin(float g) { return sinf(g * (float)M_PI / 180.0f); }
static float gradiCos(float g) { return cosf(g * (float)M_PI / 180.0f); }

static bool calcolaSole(int giornoDellAnno, bool alba, float fusoOre, float &risultato)
{
    const float zenit = 90.833f;   // il Sole "tocca" l'orizzonte gia' un po' sotto, per la rifrazione

    float lngHour = LON / 15.0f;
    float t = giornoDellAnno + (((alba ? 6.0f : 18.0f) - lngHour) / 24.0f);

    float M = (0.9856f * t) - 3.289f;                    // anomalia media del Sole
    float L = M + (1.916f * gradiSin(M)) + (0.020f * gradiSin(2 * M)) + 282.634f;
    while (L < 0) L += 360.0f;
    while (L >= 360.0f) L -= 360.0f;

    float RA = atanf(0.91764f * tanf(L * (float)M_PI / 180.0f)) * 180.0f / (float)M_PI;
    while (RA < 0) RA += 360.0f;
    while (RA >= 360.0f) RA -= 360.0f;

    // L'arcotangente perde il quadrante: va rimesso a mano guardando L.
    float Lquad = floorf(L / 90.0f) * 90.0f;
    float RAquad = floorf(RA / 90.0f) * 90.0f;
    RA = (RA + (Lquad - RAquad)) / 15.0f;

    float sinDec = 0.39782f * gradiSin(L);
    float cosDec = cosf(asinf(sinDec));

    float cosH = (gradiCos(zenit) - (sinDec * gradiSin(LAT))) / (cosDec * gradiCos(LAT));
    // Oltre il circolo polare puo' non esserci ne' alba ne' tramonto.
    if (cosH > 1.0f || cosH < -1.0f) return false;

    float H = acosf(cosH) * 180.0f / (float)M_PI;
    if (alba) H = 360.0f - H;
    H /= 15.0f;

    float T = H + RA - (0.06571f * t) - 6.622f;
    float UT = T - lngHour;
    while (UT < 0) UT += 24.0f;
    while (UT >= 24.0f) UT -= 24.0f;

    float locale = UT + fusoOre;
    while (locale < 0) locale += 24.0f;
    while (locale >= 24.0f) locale -= 24.0f;

    risultato = locale;
    return true;
}

// ------------------------------------------------------------
//  RETE
// ------------------------------------------------------------

static void connettiWifi()
{
    if (strlen(WIFI_SSID) == 0)
    {
        Serial.println("[wifi] nessuna credenziale: salto (vedi secrets.h.example)");
        return;
    }

    Serial.printf("[wifi] connessione a \"%s\"", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t limite = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < limite)
    {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    retePresente = (WiFi.status() == WL_CONNECTED);
    if (retePresente)
        Serial.printf("[wifi] ok, ip %s\n", WiFi.localIP().toString().c_str());
    else
        Serial.println("[wifi] non riuscita: si va avanti con l'ora dell'RTC");
}

static void sincronizzaOra()
{
    if (!retePresente) return;

    // configTzTime fa due cose insieme: dice a che server chiedere
    // l'ora e in che fuso siamo. Da quel momento localtime() tiene
    // conto dell'ora legale da solo.
    configTzTime(FUSO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

    Serial.print("[ora] sincronizzazione");
    uint32_t limite = millis() + 10000;
    while (time(nullptr) < 1700000000 && millis() < limite)
    {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    oraSincronizzata = (time(nullptr) >= 1700000000);
    if (oraSincronizzata)
    {
        time_t adesso = time(nullptr);
        struct tm t;
        localtime_r(&adesso, &t);
        rtcScrivi(t);   // l'ora buona resta anche a scheda staccata
        Serial.printf("[ora] %02d:%02d:%02d del %02d/%02d/%04d\n",
                      t.tm_hour, t.tm_min, t.tm_sec,
                      t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    }
    else
    {
        Serial.println("[ora] nessuna risposta dal server");
    }
}

static bool scaricaMeteo()
{
    if (!retePresente) return false;

    String url = "http://api.open-meteo.com/v1/forecast?latitude=";
    url += String(LAT, 4);
    url += "&longitude=" + String(LON, 4);
    url += "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max";
    url += "&timezone=Europe%2FRome&forecast_days=2";

    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(url))
    {
        Serial.println("[meteo] url non valido");
        return false;
    }

    int codice = http.GET();
    if (codice != 200)
    {
        Serial.printf("[meteo] risposta http %d\n", codice);
        http.end();
        return false;
    }

    // Leggiamo il corpo in una stringa invece che dal flusso grezzo.
    // Il server manda la risposta a pezzi, con la lunghezza di ogni
    // pezzo scritta in mezzo ai dati: chi legge dal flusso si trova
    // quei numeri infilati dentro il JSON e non capisce piu' niente.
    // getString() li toglie. Sono seicento byte, ci stiamo comodi.
    String corpo = http.getString();
    http.end();

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, corpo);

    if (err)
    {
        Serial.printf("[meteo] json illeggibile: %s\n", err.c_str());
        Serial.printf("[meteo] arrivati %d byte, iniziano con: %s\n",
                      corpo.length(), corpo.substring(0, 80).c_str());
        return false;
    }

    // L'indice 0 e' oggi, l'1 e' domani: sono i due giorni chiesti.
    JsonObject d = doc["daily"];
    if (d.isNull() || d["time"].size() < 2)
    {
        Serial.println("[meteo] risposta senza il giorno di domani");
        return false;
    }

    domani.codice = d["weather_code"][1] | 0;
    domani.tmax = d["temperature_2m_max"][1] | 0.0f;
    domani.tmin = d["temperature_2m_min"][1] | 0.0f;
    domani.pioggia = d["precipitation_probability_max"][1] | 0;
    domani.valido = true;
    daRidisegnare[SCHEDA_METEO] = true;

    Serial.printf("[meteo] domani: codice %d, %.0f/%.0f gradi, pioggia %d%%\n",
                  domani.codice, domani.tmin, domani.tmax, domani.pioggia);
    return true;
}

// ------------------------------------------------------------
//  ICONE METEO
// ------------------------------------------------------------
//  Disegnate come griglie di caratteri: un cancelletto e' un punto
//  acceso. Si leggono a occhio nel codice, che e' il modo piu'
//  rapido per ritoccarle.

#define ICONA_LATO 15

static const char *ICO_SOLE[ICONA_LATO] = {
    ".......#.......",
    ".......#.......",
    "..#.........#..",
    ".....#####.....",
    "....#######....",
    "...#########...",
    "...#########...",
    "##.#########.##",
    "...#########...",
    "...#########...",
    "....#######....",
    ".....#####.....",
    "..#.........#..",
    ".......#.......",
    ".......#.......",
};

static const char *ICO_NUVOLA[ICONA_LATO] = {
    "...............",
    "...............",
    ".....#####.....",
    "....#######....",
    "...#########...",
    "..##########...",
    ".###########...",
    "###############",
    "###############",
    "###############",
    ".#############.",
    "...............",
    "...............",
    "...............",
    "...............",
};

static const char *ICO_PIOGGIA[ICONA_LATO] = {
    "...............",
    ".....#####.....",
    "....#######....",
    "...#########...",
    "..##########...",
    ".###########...",
    "###############",
    "###############",
    ".#############.",
    "...............",
    "..#...#...#....",
    "..#...#...#....",
    "...............",
    "....#...#...#..",
    "....#...#...#..",
};

static const char *ICO_NEVE[ICONA_LATO] = {
    "...............",
    ".....#####.....",
    "....#######....",
    "...#########...",
    "..##########...",
    ".###########...",
    "###############",
    "###############",
    ".#############.",
    "...............",
    "..#.#...#.#....",
    "...#.....#.....",
    "..#.#...#.#....",
    "...............",
    "...............",
};

static const char *ICO_TEMPORALE[ICONA_LATO] = {
    "...............",
    ".....#####.....",
    "....#######....",
    "...#########...",
    "..##########...",
    ".###########...",
    "###############",
    "###############",
    ".#############.",
    "...............",
    "......####.....",
    ".....####......",
    "....########...",
    "......####.....",
    ".....###.......",
};

static const char *ICO_NEBBIA[ICONA_LATO] = {
    "...............",
    "...............",
    "...............",
    ".###########...",
    "...............",
    "..###########..",
    "...............",
    ".###########...",
    "...............",
    "..###########..",
    "...............",
    ".###########...",
    "...............",
    "...............",
    "...............",
};

static void disegnaIcona(Arduino_GFX *g, int16_t cx, int16_t cy,
                         const char **griglia, int16_t passo, int16_t diam, uint16_t colore)
{
    int16_t x0 = cx - (ICONA_LATO * passo) / 2 + passo / 2;
    int16_t y0 = cy - (ICONA_LATO * passo) / 2 + passo / 2;
    for (int r = 0; r < ICONA_LATO; ++r)
        for (int c = 0; c < ICONA_LATO; ++c)
            if (griglia[r][c] == '#')
                dmDot(g, x0 + c * passo, y0 + r * passo, diam, colore);
}

// I codici sono lo standard meteorologico WMO: 0 e' cielo sereno,
// e piu' il numero sale piu' il tempo peggiora.
static const char **iconaPerCodice(int c)
{
    if (c == 0) return ICO_SOLE;
    if (c <= 3) return ICO_NUVOLA;
    if (c <= 48) return ICO_NEBBIA;
    if (c <= 67) return ICO_PIOGGIA;
    if (c <= 77) return ICO_NEVE;
    if (c <= 82) return ICO_PIOGGIA;
    if (c <= 86) return ICO_NEVE;
    return ICO_TEMPORALE;
}

static const char *descrizionePerCodice(int c)
{
    if (c == 0) return "SERENO";
    if (c == 1) return "POCO NUVOLOSO";
    if (c == 2) return "NUVOLOSO";
    if (c == 3) return "COPERTO";
    if (c <= 48) return "NEBBIA";
    if (c <= 57) return "PIOVIGGINE";
    if (c <= 67) return "PIOGGIA";
    if (c <= 77) return "NEVE";
    if (c <= 82) return "ROVESCI";
    if (c <= 86) return "NEVE";
    return "TEMPORALE";
}

// ------------------------------------------------------------
//  PEZZI COMUNI A TUTTE LE SCHEDE
// ------------------------------------------------------------

static const char *GIORNI[7] = {"DOM", "LUN", "MAR", "MER", "GIO", "VEN", "SAB"};
static const char *MESI[12] = {"GEN", "FEB", "MAR", "APR", "MAG", "GIU",
                               "LUG", "AGO", "SET", "OTT", "NOV", "DIC"};

static void telaio(Arduino_GFX *g, const char *etichetta)
{
    g->fillScreen(COL_SFONDO);
    g->drawRoundRect(6, 6, LCD_W - 12, LCD_H - 12, 26, COL_CORNICE);

    // L'etichetta in alto a sinistra, piccola e molto spaziata.
    dmText(g, 26, 26, etichetta, 3, 2, COL_ETICHETTA, 2);
}

static void indicatoreBatteria(Arduino_GFX *g)
{
    if (batteria < 0) return;

    const int16_t x = LCD_W - 78;
    const int16_t y = 28;
    const int16_t larghezza = 40;
    const int16_t altezza = 14;

    uint16_t colore = alimentato ? COL_ROSSO
                    : (batteria <= 15 ? COL_ROSSO : COL_SPENTO);

    g->drawRect(x, y, larghezza, altezza, colore);
    g->fillRect(x + larghezza, y + 4, 3, altezza - 8, colore);

    int riempimento = (larghezza - 6) * batteria / 100;
    if (riempimento > 0)
        g->fillRect(x + 3, y + 3, riempimento, altezza - 6,
                    alimentato ? COL_ROSSO : COL_SECONDARIO);
}

// ------------------------------------------------------------
//  SCHEDA 1: L'ORA
// ------------------------------------------------------------

static void disegnaOra(Arduino_GFX *g, const struct tm &t)
{
    telaio(g, CITTA);
    indicatoreBatteria(g);

    char buf[16];

    // L'ora, grande al centro.
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    dmTextCentered(g, LCD_W / 2, 120, buf, 10, 7, COL_ACCESO);

    // I secondi come una riga di sessanta punti che si riempie.
    // Un quadrante, non un numero: si legge con la coda dell'occhio.
    const int16_t passoSec = 5;
    const int16_t larghezza = 60 * passoSec;
    const int16_t x0 = (LCD_W - larghezza) / 2 + passoSec / 2;
    for (int i = 0; i < 60; ++i)
    {
        uint16_t c = (i < t.tm_sec) ? COL_SECONDARIO : COL_SPENTO;
        if (i == t.tm_sec) c = COL_ROSSO;
        dmDot(g, x0 + i * passoSec, 232, 3, c);
    }

    // La data.
    snprintf(buf, sizeof(buf), "%s %02d %s",
             GIORNI[t.tm_wday % 7], t.tm_mday, MESI[t.tm_mon % 12]);
    dmTextCentered(g, LCD_W / 2, 280, buf, 5, 4, COL_ACCESO);

    // In fondo, da dove viene l'ora che stai leggendo.
    const char *fonte = oraSincronizzata ? "SINCRONIZZATO" : "OROLOGIO INTERNO";
    dmTextCentered(g, LCD_W / 2, 370, fonte, 3, 2, COL_ETICHETTA, 2);
}

// ------------------------------------------------------------
//  SCHEDA 2: IL SOLE
// ------------------------------------------------------------

static void disegnaSole(Arduino_GFX *g, const struct tm &t)
{
    telaio(g, "SOLE");
    indicatoreBatteria(g);

    float fuso = 1.0f + (t.tm_isdst > 0 ? 1.0f : 0.0f);
    int giorno = t.tm_yday + 1;

    float alba = 0, tramonto = 0;
    bool okAlba = calcolaSole(giorno, true, fuso, alba);
    bool okTram = calcolaSole(giorno, false, fuso, tramonto);

    if (!okAlba || !okTram)
    {
        dmTextCentered(g, LCD_W / 2, 200, "NON CALCOLABILE", 4, 3, COL_SPENTO);
        return;
    }

    float adesso = t.tm_hour + t.tm_min / 60.0f + t.tm_sec / 3600.0f;
    bool giorno_chiaro = (adesso >= alba && adesso <= tramonto);

    // L'arco: il percorso del Sole da est a ovest. I punti accesi
    // sono la parte di giornata gia' passata, la testa rossa e'
    // dove sta il Sole adesso.
    const int16_t cx = LCD_W / 2;
    const int16_t cy = 268;
    const int16_t raggio = 132;
    const int totale = 33;

    float avanzamento = (adesso - alba) / (tramonto - alba);
    if (avanzamento < 0) avanzamento = 0;
    if (avanzamento > 1) avanzamento = 1;
    int accesi = giorno_chiaro ? (int)lroundf(avanzamento * totale) : (adesso < alba ? 0 : totale);

    dmRing(g, cx, cy, raggio, totale, -90.0f, 180.0f, accesi, 5,
           COL_SECONDARIO, COL_SPENTO, COL_ROSSO, giorno_chiaro);

    // Il conto alla rovescia verso la prossima transizione.
    float mancano;
    const char *verso;
    if (adesso < alba)
    {
        mancano = alba - adesso;
        verso = "ALL'ALBA";
    }
    else if (adesso < tramonto)
    {
        mancano = tramonto - adesso;
        verso = "AL TRAMONTO";
    }
    else
    {
        mancano = (24.0f - adesso) + alba;
        verso = "ALL'ALBA";
    }

    char buf[20];
    int ore = (int)mancano;
    int minuti = (int)((mancano - ore) * 60.0f);
    snprintf(buf, sizeof(buf), "%dH %02dM", ore, minuti);
    dmTextCentered(g, cx, 150, buf, 8, 6, COL_ACCESO);
    dmTextCentered(g, cx, 220, verso, 3, 2, COL_ETICHETTA, 2);

    // Gli orari veri, ai due capi dell'arco.
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)alba, (int)((alba - (int)alba) * 60));
    dmText(g, 40, 300, buf, 4, 3, COL_ACCESO);
    dmText(g, 40, 340, "ALBA", 3, 2, COL_ETICHETTA, 2);

    snprintf(buf, sizeof(buf), "%02d:%02d", (int)tramonto, (int)((tramonto - (int)tramonto) * 60));
    int16_t w = dmTextWidth(buf, 4, 1);
    dmText(g, LCD_W - 40 - w, 300, buf, 4, 3, COL_ACCESO);
    int16_t w2 = dmTextWidth("TRAMONTO", 3, 2);
    dmText(g, LCD_W - 40 - w2, 340, "TRAMONTO", 3, 2, COL_ETICHETTA, 2);
}

// ------------------------------------------------------------
//  SCHEDA 3: DOMANI
// ------------------------------------------------------------

static void disegnaMeteo(Arduino_GFX *g)
{
    telaio(g, "DOMANI");
    indicatoreBatteria(g);

    if (!domani.valido)
    {
        dmTextCentered(g, LCD_W / 2, 190, "NO LINK", 8, 6, COL_SPENTO);
        const char *motivo = retePresente ? "SERVER NON RAGGIUNGIBILE" : "NESSUNA RETE WIFI";
        dmTextCentered(g, LCD_W / 2, 260, motivo, 3, 2, COL_ETICHETTA, 2);
        return;
    }

    disegnaIcona(g, LCD_W / 2, 130, iconaPerCodice(domani.codice), 6, 4, COL_ACCESO);

    char buf[16];

    // La massima, grande. Il segno di grado e' il carattere ^ del
    // font: a matrice di punti un cerchietto e' un quadratino 3x3.
    snprintf(buf, sizeof(buf), "%d^", (int)lroundf(domani.tmax));
    dmTextCentered(g, LCD_W / 2, 218, buf, 11, 8, COL_ACCESO);

    snprintf(buf, sizeof(buf), "MIN %d^", (int)lroundf(domani.tmin));
    dmTextCentered(g, LCD_W / 2, 300, buf, 4, 3, COL_SECONDARIO);

    dmTextCentered(g, LCD_W / 2, 344, descrizionePerCodice(domani.codice), 3, 2, COL_ETICHETTA, 2);

    // La probabilita' di pioggia come barra di punti.
    const int totale = 20;
    const int16_t passo = 12;
    const int16_t x0 = (LCD_W - totale * passo) / 2 + passo / 2;
    int accesi = (domani.pioggia * totale + 50) / 100;
    for (int i = 0; i < totale; ++i)
        dmDot(g, x0 + i * passo, 386, 5, i < accesi ? COL_ROSSO : COL_SPENTO);

    snprintf(buf, sizeof(buf), "%d%% PIOGGIA", domani.pioggia);
    dmTextCentered(g, LCD_W / 2, 402, buf, 2, 2, COL_ETICHETTA, 2);
}

// ------------------------------------------------------------
//  RIDISEGNO
// ------------------------------------------------------------

static void oraCorrente(struct tm &t)
{
    time_t adesso = time(nullptr);
    localtime_r(&adesso, &t);
}

static void ridisegna(int i)
{
    struct tm t;
    oraCorrente(t);

    switch (i)
    {
    case SCHEDA_ORA:   disegnaOra(scheda[i], t);  break;
    case SCHEDA_SOLE:  disegnaSole(scheda[i], t); break;
    case SCHEDA_METEO: disegnaMeteo(scheda[i]);   break;
    }
    daRidisegnare[i] = false;
}

// ------------------------------------------------------------
//  COMPOSIZIONE E SCORRIMENTO
// ------------------------------------------------------------

// I pallini in basso. Quello rosso non salta da una posizione
// all'altra: scivola insieme al contenuto, cosi' durante il
// trascinamento sai sempre a che punto sei del passaggio.
static void disegnaPallini(Arduino_GFX *g, float posizione)
{
    const int16_t passo = 26;
    const int16_t y = LCD_H - 44;
    const int16_t x0 = LCD_W / 2 - (N_SCHEDE - 1) * passo / 2;

    for (int i = 0; i < N_SCHEDE; ++i)
        dmDot(g, x0 + i * passo, y, 6, COL_SPENTO);

    float px = x0 + posizione * passo;
    dmDot(g, (int16_t)lroundf(px), y, 10, COL_ROSSO);
}

static void componi()
{
    int off = (int)lroundf(scorrimento);

    // Prima la scheda corrente, spostata all'indietro di quanto si
    // e' mosso il dito. Poi la vicina, incollata al suo fianco.
    // Le coordinate negative non sono un problema: la libreria
    // ritaglia da sola la parte che esce dallo schermo.
    if (off > 0)
    {
        if (schedaCorrente + 1 < N_SCHEDE)
            comp->draw16bitRGBBitmap(LCD_W - off, 0, scheda[schedaCorrente + 1]->getFramebuffer(), LCD_W, LCD_H);
        else
            comp->fillRect(LCD_W - off, 0, off, LCD_H, COL_SFONDO);
    }
    else if (off < 0)
    {
        if (schedaCorrente - 1 >= 0)
            comp->draw16bitRGBBitmap(-LCD_W - off, 0, scheda[schedaCorrente - 1]->getFramebuffer(), LCD_W, LCD_H);
        else
            comp->fillRect(0, 0, -off, LCD_H, COL_SFONDO);
    }

    comp->draw16bitRGBBitmap(-off, 0, scheda[schedaCorrente]->getFramebuffer(), LCD_W, LCD_H);

    disegnaPallini(comp, schedaCorrente + scorrimento / (float)LCD_W);
    comp->flush();
}

static void avviaAnimazione(int direzione)
{
    animDirezione = direzione;
    animDa = scorrimento;
    animA = direzione * (float)LCD_W;
    animInizio = millis();

    // Piu' strada resta da fare, piu' tempo si prende: cosi' un
    // ritorno di pochi pixel non ha la stessa lentezza di un
    // cambio di scheda intero.
    float distanza = fabsf(animA - animDa);
    animDurata = (uint16_t)constrain(140 + distanza * 0.55f, 140.0f, 340.0f);
    animazioneAttiva = true;
}

static void aggiornaAnimazione()
{
    uint32_t passato = millis() - animInizio;
    if (passato >= animDurata)
    {
        schedaCorrente += animDirezione;
        scorrimento = 0;
        animazioneAttiva = false;
        animDirezione = 0;
        componi();
        return;
    }

    // Frenata dolce: parte veloce e si posa, invece di arrivare
    // di schianto. E' il terzo grado di una curva, niente di piu'.
    float p = (float)passato / (float)animDurata;
    float e = 1.0f - powf(1.0f - p, 3.0f);
    scorrimento = animDa + (animA - animDa) * e;
    componi();
}

static void gestisciTocco()
{
    TouchPoint tp = touch.leggi();
    uint32_t adesso = millis();

    if (tp.premuto && !ditoGiu)
    {
        // Dito appoggiato: da qui in poi comanda lui, e qualunque
        // animazione in corso si interrompe sul posto.
        ditoGiu = true;
        eraTap = true;
        animazioneAttiva = false;
        animDirezione = 0;
        ditoPartenzaX = tp.x;
        scorrimentoPartenza = scorrimento;
        ditoUltimaX = tp.x;
        ditoUltimoT = adesso;
        velocita = 0;

        // Utile la prima volta: se lo scorrimento andasse storto,
        // qui si vede subito se le coordinate arrivano ruotate.
        Serial.printf("[touch] x=%d y=%d\n", tp.x, tp.y);

        // Le schede vicine potrebbero avere dati vecchi: meglio
        // rinfrescarle adesso, prima che comincino a scorrere.
        for (int d = -1; d <= 1; d += 2)
        {
            int i = schedaCorrente + d;
            if (i >= 0 && i < N_SCHEDE && daRidisegnare[i])
                ridisegna(i);
        }
    }
    else if (tp.premuto && ditoGiu)
    {
        float nuovo = scorrimentoPartenza + (ditoPartenzaX - tp.x);

        // Ai due capi non c'e' niente da mostrare: il contenuto
        // segue il dito solo in parte, e si sente che e' finita.
        if ((schedaCorrente == 0 && nuovo < 0) ||
            (schedaCorrente == N_SCHEDE - 1 && nuovo > 0))
            nuovo *= 0.32f;

        if (abs(tp.x - ditoPartenzaX) > 8)
            eraTap = false;

        uint32_t dt = adesso - ditoUltimoT;
        if (dt >= 12)
        {
            // La velocita' istantanea sarebbe troppo nervosa: la
            // mescoliamo con quella di prima, cosi' un singolo
            // campione storto non manda all'aria il gesto.
            float istantanea = (float)(ditoUltimaX - tp.x) / (float)dt;
            velocita = velocita * 0.6f + istantanea * 0.4f;
            ditoUltimaX = tp.x;
            ditoUltimoT = adesso;
        }

        scorrimento = nuovo;
        componi();
    }
    else if (!tp.premuto && ditoGiu)
    {
        ditoGiu = false;

        if (eraTap)
        {
            // Un tocco secco non cambia scheda: rimette solo a
            // posto eventuali pixel di scarto.
            scorrimento = 0;
            componi();
            return;
        }

        // Se il dito e' rimasto fermo un attimo prima di staccarsi,
        // l'ultima velocita' calcolata non vale piu' niente.
        if (adesso - ditoUltimoT > 90)
            velocita = 0;

        int direzione = 0;
        const float sogliaSpinta = 0.30f;          // pixel per millisecondo
        const float sogliaStrada = LCD_W * 0.26f;

        if (velocita > sogliaSpinta && scorrimento > 12) direzione = 1;
        else if (velocita < -sogliaSpinta && scorrimento < -12) direzione = -1;
        else if (scorrimento > sogliaStrada) direzione = 1;
        else if (scorrimento < -sogliaStrada) direzione = -1;

        if (schedaCorrente + direzione < 0 || schedaCorrente + direzione >= N_SCHEDE)
            direzione = 0;

        avviaAnimazione(direzione);
    }
}

// ------------------------------------------------------------
//  SETUP
// ------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("============================================");
    Serial.println("  DOT CLOCK - ESP32-S3-Touch-AMOLED-1.8");
    Serial.println("============================================");

    pinMode(BTN_BOOT, INPUT_PULLUP);

    Wire.begin(I2C_SDA, I2C_SCL, 400000);

    if (!comp->begin())
    {
        Serial.println("[display] non parte: memoria insufficiente?");
        while (true) delay(1000);
    }
    panel->setBrightness(LUMINOSITA);

    // I fogli delle schede: nessuno di questi tocca il bus, sono
    // solo memoria. GFX_SKIP_OUTPUT_BEGIN glielo ricorda.
    for (int i = 0; i < N_SCHEDE; ++i)
    {
        if (!scheda[i]->begin(GFX_SKIP_OUTPUT_BEGIN))
        {
            Serial.printf("[psram] foglio %d non allocato\n", i);
            while (true) delay(1000);
        }
    }
    Serial.printf("[psram] %u KB liberi dopo l'allocazione dei fogli\n",
                  (unsigned)(ESP.getFreePsram() / 1024));

    scanI2C();

    // Il touch puo' essere ancora addormentato: insistiamo un po'.
    for (int tentativo = 0; tentativo < 3 && !touchOk; ++tentativo)
        touchOk = touch.begin();
    Serial.printf("[touch] FT3168 %s\n", touchOk ? "sveglio" : "non risponde, riprovero'");

    aggiornaBatteria();
    Serial.printf("[batteria] %d%%%s\n", batteria, alimentato ? " (cavo collegato)" : "");

    // Schermata di attesa: la rete puo' prendersi qualche secondo
    // e uno schermo nero sembrerebbe un blocco.
    comp->fillScreen(COL_SFONDO);
    dmTextCentered(comp, LCD_W / 2, 200, "AVVIO", 8, 6, COL_ACCESO);
    dmTextCentered(comp, LCD_W / 2, 270, "CONNESSIONE", 3, 2, COL_ETICHETTA, 2);
    comp->flush();

    // L'ora dell'RTC vale subito: se poi arriva quella di rete,
    // e' piu' precisa e prende il suo posto.
    setenv("TZ", FUSO, 1);
    tzset();
    struct tm salvata;
    if (rtcLeggi(salvata))
    {
        time_t quando = mktime(&salvata);
        struct timeval tv = {.tv_sec = quando, .tv_usec = 0};
        settimeofday(&tv, nullptr);
        Serial.printf("[rtc] ora recuperata: %02d:%02d del %02d/%02d\n",
                      salvata.tm_hour, salvata.tm_min, salvata.tm_mday, salvata.tm_mon + 1);
    }
    else
    {
        Serial.println("[rtc] nessuna ora valida salvata");
    }

    connettiWifi();
    sincronizzaOra();
    prossimoMeteo = millis() + (scaricaMeteo() ? 30UL * 60UL * 1000UL : 2UL * 60UL * 1000UL);

    for (int i = 0; i < N_SCHEDE; ++i)
        ridisegna(i);
    componi();

    Serial.println("[pronto] scorri con il dito per cambiare scheda");
}

// ------------------------------------------------------------
//  LOOP
// ------------------------------------------------------------

void loop()
{
    if (animazioneAttiva)
    {
        aggiornaAnimazione();
        return;   // mentre si muove, niente altro: la fluidita' viene prima
    }

    gestisciTocco();

    if (ditoGiu)
        return;

    // Se il chip non ha ancora risposto, ogni tanto gli ridiamo un
    // colpetto: puo' essersi riaddormentato mentre nessuno toccava.
    if (!touchOk)
    {
        static uint32_t prossimoTentativo = 0;
        if ((int32_t)(millis() - prossimoTentativo) >= 0)
        {
            prossimoTentativo = millis() + 2000;
            touchOk = touch.begin();
            if (touchOk) Serial.println("[touch] risvegliato");
        }
    }

    // Da qui in giu' si entra solo a schermo fermo.

    struct tm t;
    oraCorrente(t);

    if (t.tm_sec != ultimoSecondo)
    {
        ultimoSecondo = t.tm_sec;
        daRidisegnare[SCHEDA_ORA] = true;
        if (t.tm_min != ultimoMinuto)
        {
            ultimoMinuto = t.tm_min;
            daRidisegnare[SCHEDA_SOLE] = true;
            aggiornaBatteria();
            daRidisegnare[SCHEDA_METEO] = true;   // per l'indicatore di carica
        }
    }

    if (retePresente && (int32_t)(millis() - prossimoMeteo) >= 0)
    {
        // Andata bene si riguarda fra mezz'ora, andata male fra due
        // minuti: un buco di rete non deve lasciare la scheda vuota
        // per tutto quel tempo.
        prossimoMeteo = millis() + (scaricaMeteo() ? 30UL * 60UL * 1000UL : 2UL * 60UL * 1000UL);
    }

    // Si ridisegna solo quella che stai guardando: le altre
    // aspettano il momento in cui cominci a scorrere.
    if (daRidisegnare[schedaCorrente])
    {
        ridisegna(schedaCorrente);
        componi();
    }

    // Il pulsante BOOT come riserva, nel caso il touch faccia i
    // capricci: un colpo e si passa alla scheda successiva.
    static bool bottonePremuto = false;
    bool giu = (digitalRead(BTN_BOOT) == LOW);
    if (giu && !bottonePremuto)
    {
        bottonePremuto = true;
        int prossima = (schedaCorrente + 1) % N_SCHEDE;
        if (daRidisegnare[prossima]) ridisegna(prossima);
        if (prossima > schedaCorrente) avviaAnimazione(1);
        else { schedaCorrente = 0; scorrimento = 0; componi(); }
    }
    else if (!giu)
    {
        bottonePremuto = false;
    }

    delay(5);
}
