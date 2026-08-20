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
#include "sveglia.h"

// Il logo compare come quarta scheda solo se src/logo.h esiste.
// Lo genera scripts/logo2c.py da un'immagine qualsiasi; senza,
// il programma resta a tre schede e compila lo stesso.
#if __has_include("logo.h")
#include "logo.h"
#define HA_LOGO 1
#else
#define HA_LOGO 0
#endif

// Cosa scrivere sopra e sotto il logo.
#define LOGO_ETICHETTA "MENTALTOY"
#define LOGO_SOTTOTITOLO ""
#define LOGO_COLORE COL_ACCESO

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

// Il vetro ha gli angoli molto arrotondati, piu' di quanto sembri a
// schermo spento: una cornice troppo squadrata ci finisce sotto e si
// vede tagliata. Questi due numeri la tengono al riparo.
#define BORDO_MARGINE 10
#define BORDO_RAGGIO 54

// Quanto stanno larghe dal bordo l'etichetta e la batteria. Con
// angoli cosi' tondi la curva entra parecchio verso il centro: un
// testo appoggiato all'angolo sembra schiacciato contro il vetro.
#define PADDING 38

// La barra dei comandi in fondo alla scheda dell'ora.
#define BARRA_Y 366
#define BARRA_SX (PADDING + 14)
#define BARRA_DX (LCD_W - PADDING - 14)

// Quanto largo e' il bersaglio di un comando. Un dito copre quasi
// mezzo centimetro di vetro e non vede cosa sta coprendo: la zona
// che risponde deve essere molto piu' grande del simbolo disegnato.
#define BERSAGLIO 46

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
#define COL_SOLE 0xFE80    // giallo caldo
#define COL_LUNA 0x7ADF    // viola-blu

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

#if HA_LOGO
#define N_SCHEDE 5
#else
#define N_SCHEDE 4
#endif

#define SCHEDA_ORA 0
#define SCHEDA_SOLE 1
#define SCHEDA_METEO 2
#define SCHEDA_BARO 3
#define SCHEDA_LOGO 4

// Ogni scheda ha il suo foglio di memoria, sempre pronto.
// Sono 329 KB l'uno: con 8 MB di PSRAM ce ne stanno una ventina,
// quindi aggiungere schede in futuro non e' un problema.
// L'output e' nullptr perche' questi fogli non vanno mai a
// schermo da soli: passano sempre da "comp".
static Arduino_Canvas *scheda[N_SCHEDE] = {
    new Arduino_Canvas(LCD_W, LCD_H, nullptr),
    new Arduino_Canvas(LCD_W, LCD_H, nullptr),
    new Arduino_Canvas(LCD_W, LCD_H, nullptr),
    new Arduino_Canvas(LCD_W, LCD_H, nullptr),
#if HA_LOGO
    new Arduino_Canvas(LCD_W, LCD_H, nullptr),
#endif
};
static bool daRidisegnare[N_SCHEDE];

// ------------------------------------------------------------
//  STATO
// ------------------------------------------------------------

// ------------------------------------------------------------
//  DOVE SI TROVA L'UTENTE
// ------------------------------------------------------------
//  Le schede sono cose che si guardano e si scorrono via. La
//  sveglia e' una cosa in cui si entra e da cui si esce, quindi non
//  e' una quinta scheda: e' una schermata che sta sopra il
//  carosello e se lo tiene da parte finche' non hai finito.

enum Vista
{
    VISTA_SCHEDE = 0,
    VISTA_LISTA,      // l'elenco delle sveglie
    VISTA_EDITOR,     // imposta un orario
    VISTA_ALLARME     // sta suonando
};

static Vista vista = VISTA_SCHEDE;
static bool vistaDaRidisegnare = true;

static float listaScorrimento = 0;   // di quanto e' scorsa la lista, in pixel
static int editorIndice = -1;        // quale sveglia si sta modificando, -1 = nuova
static uint8_t editorOre = 7;
static uint8_t editorMinuti = 0;
static int editorCampo = 0;          // 0 = ore, 1 = minuti

static int allarmeIndice = -1;
static int allarmeUltimoMinuto = -1;

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
static bool eraTap = true;
static int16_t tapX = 0, tapY = 0;      // il dito si e' mosso poco: e' un tocco, non uno scorrimento

// I pallini non stanno sempre accesi: compaiono quando li tocchi e
// se ne vanno da soli. Una schermata di orologio deve essere ferma;
// i comandi servono nel momento in cui li usi, non prima.
#define PALLINI_ATTESA 1000   // quanto restano dopo l'ultimo tocco
#define PALLINI_FADE 450      // quanto ci mettono a spegnersi
static uint32_t palliniFino = 0;
static float palliniOpacitaDisegnata = -1.0f;

static bool animazioneAttiva = false;
static float animDa = 0, animA = 0;
static uint32_t animInizio = 0;
static uint16_t animDurata = 0;
static int animDirezione = 0;

static bool schermoAcceso = true;

static bool retePresente = false;
static bool oraSincronizzata = false;
static int batteria = -1;
static bool alimentato = false;

// I numeri che scorrono: uno per l'ora, uno per il conto alla
// rovescia del Sole. Ognuno si ricorda da solo che numero mostrava
// prima, che e' tutto quello che serve per animare il cambio.
// Quando entri nella scheda del Sole, l'astro non e' gia' li' dove
// deve stare: parte dal capo dell'arco e ci arriva, riempiendo il
// quadrante man mano. Vedere il percorso dice quanta giornata e'
// passata molto meglio di un punto fermo.
#define SORGERE_DURATA 1100

// Da dove parte l'astro, in frazione di arco. Negativo vuol dire
// sotto l'orizzonte: da li' non si vede, e comincia a spuntare solo
// mentre sale. Un corpo celeste non compare, sorge.
#define SORGERE_PARTENZA -0.09f
static uint32_t sorgereInizio = 0;
static bool sorgereAttivo = false;
static bool sorgereArmato = true;   // pronto a partire alla prossima entrata

// Il grafico del barometro si traccia da sinistra a destra, con la
// stessa regola: si riarma solo quando la scheda esce del tutto.
#define TRACCIA_DURATA 1100
static uint32_t tracciaInizio = 0;
static bool tracciaAttiva = false;
static bool tracciaArmata = true;

static DmRullo rulloOra;
static DmRullo rulloSole;
static DmRullo rulloBaro;

// Anche i numeri della sveglia scorrono. Mentre tieni premuto pero'
// l'animazione si spegne: a quel ritmo non farebbe in tempo a
// finire, e si vedrebbero cifre che partono e vengono interrotte.
static DmRullo rulloEditorOre;
static DmRullo rulloEditorMin;
static bool editorInRipetizione = false;

// Lo alzano le funzioni di disegno quando un numero si sta ancora
// muovendo: e' il segnale al ciclo principale che non ha finito.
static bool numeriInMovimento = false;

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
//  BAROMETRO
// ------------------------------------------------------------
//  Questa scheda non ha un sensore dietro: il QMI8658 misura
//  accelerazione e rotazione, non pressione. I valori arrivano
//  dallo stesso servizio del meteo, che le ore passate le tiene.
//
//  Serve la storia, non il numero: una pressione di 1013 non dice
//  niente da sola, mentre 1013 dopo che era 1020 dice che sta per
//  arrivare qualcosa. E' la ragione per cui gli orologi da
//  escursione hanno sempre avuto questo grafico.

#define ORE_BARO 24

struct Barometro
{
    bool valido = false;
    float valori[ORE_BARO];
    float minimo = 0, massimo = 0;
};
static Barometro baro;

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
    url += "&hourly=pressure_msl&past_days=1&forecast_days=2";
    // Le ore come numeri invece che come date scritte: la risposta
    // dimezza, e a noi servono solo per capire dove siamo adesso.
    url += "&timeformat=unixtime&timezone=Europe%2FRome";

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

    DynamicJsonDocument doc(8192);
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

    // La pressione delle ultime ventiquattro ore. Nell'elenco c'e'
    // anche il futuro: si cerca l'ora in cui siamo e si guarda
    // indietro.
    JsonArray quando = doc["hourly"]["time"];
    JsonArray valori = doc["hourly"]["pressure_msl"];

    if (!quando.isNull() && !valori.isNull() && quando.size() == valori.size())
    {
        time_t adesso = time(nullptr);
        int corrente = -1;
        for (size_t i = 0; i < quando.size(); ++i)
            if ((time_t)(quando[i].as<long>()) <= adesso)
                corrente = (int)i;

        if (corrente >= ORE_BARO - 1)
        {
            baro.minimo = 9999;
            baro.massimo = -9999;
            for (int k = 0; k < ORE_BARO; ++k)
            {
                float v = valori[corrente - (ORE_BARO - 1) + k] | 0.0f;
                baro.valori[k] = v;
                if (v < baro.minimo) baro.minimo = v;
                if (v > baro.massimo) baro.massimo = v;
            }
            baro.valido = true;
            daRidisegnare[SCHEDA_BARO] = true;
            Serial.printf("[baro] %.0f hPa, in 24h da %.0f a %.0f\n",
                          baro.valori[ORE_BARO - 1], baro.minimo, baro.massimo);
        }
    }

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

// Il Sole che scorre sull'arco. Piccolo, con i raggi, cosi' si
// distingue a colpo d'occhio dai punti del quadrante.
#define SOLE_LATO 9
static const char *ICO_SOLE_PICCOLO[SOLE_LATO] = {
    "....#....",
    "..#...#..",
    "...###...",
    "..#####..",
    "#.#####.#",
    "..#####..",
    "...###...",
    "..#...#..",
    "....#....",
};

// La Luna: un disco con un morso, che e' il modo piu' corto per
// dire "luna" con una manciata di punti.
static const char *ICO_LUNA[SOLE_LATO] = {
    "....##...",
    "..####...",
    ".####....",
    ".###.....",
    ".###.....",
    ".###.....",
    ".####....",
    "..####...",
    "....##...",
};

// ------------------------------------------------------------
//  ICONE DEI COMANDI
// ------------------------------------------------------------
//  Nove per nove: la misura piu' piccola in cui un simbolo resta
//  riconoscibile avendo un centro esatto, che serve per la croce e
//  per la campana.

#define ICONA_COMANDO 9

static const char *ICO_CAMPANA[ICONA_COMANDO] = {
    "....#....",
    "...#.#...",
    "..#...#..",
    "..#...#..",
    ".#.....#.",
    ".#.....#.",
    "#########",
    ".........",
    "...###...",
};

static const char *ICO_PIU[ICONA_COMANDO] = {
    ".........",
    "....#....",
    "....#....",
    "....#....",
    ".#######.",
    "....#....",
    "....#....",
    "....#....",
    ".........",
};

static const char *ICO_MENO[ICONA_COMANDO] = {
    ".........",
    ".........",
    ".........",
    ".........",
    ".#######.",
    ".........",
    ".........",
    ".........",
    ".........",
};

static const char *ICO_CHIUDI[ICONA_COMANDO] = {
    ".........",
    ".#.....#.",
    "..#...#..",
    "...#.#...",
    "....#....",
    "...#.#...",
    "..#...#..",
    ".#.....#.",
    ".........",
};

static const char *ICO_SPUNTA[ICONA_COMANDO] = {
    ".........",
    ".......##",
    "......##.",
    ".....##..",
    "##..##...",
    ".####....",
    "..##.....",
    ".........",
    ".........",
};

static const char *ICO_CESTINO[ICONA_COMANDO] = {
    "..#####..",
    ".#######.",
    ".........",
    "#########",
    ".#.#.#.#.",
    ".#.#.#.#.",
    ".#.#.#.#.",
    ".#######.",
    ".........",
};

// Il disco pieno su cui si scava la cifra del contatore.
static const char *ICO_DISCO[ICONA_COMANDO] = {
    "..#####..",
    ".#######.",
    "#########",
    "#########",
    "#########",
    "#########",
    "#########",
    ".#######.",
    "..#####..",
};

// Un numero dentro un bollo, tutto fatto di punti come il resto.
// La cifra non si disegna sopra: si tolgono i punti del disco dove
// il carattere passa, e quello che si vede e' lo sfondo attraverso
// il buco. Con i punti staccati e' l'unico modo perche' la cifra
// resti netta invece di sembrare appoggiata sopra.
static void disegnaBollo(Arduino_GFX *g, int16_t cx, int16_t cy, char cifra,
                         int16_t passo, int16_t diam, uint16_t colore)
{
    uint8_t k = (uint8_t)cifra;
    if (k < DM_FONT_FIRST || k > DM_FONT_LAST) k = 32;
    const uint8_t *glifo = DM_FONT[k - DM_FONT_FIRST];

    const int16_t x0 = cx - (ICONA_COMANDO * passo) / 2 + passo / 2;
    const int16_t y0 = cy - (ICONA_COMANDO * passo) / 2 + passo / 2;

    for (int r = 0; r < ICONA_COMANDO; ++r)
        for (int c = 0; c < ICONA_COMANDO; ++c)
        {
            if (ICO_DISCO[r][c] != '#') continue;

            // Il carattere sta al centro: cinque colonne per sette
            // righe dentro una griglia di nove per nove.
            int gc = c - 2;
            int gr = r - 1;
            if (gc >= 0 && gc < 5 && gr >= 0 && gr < 7 &&
                (pgm_read_byte(&glifo[gc]) & (1 << gr)))
                continue;   // qui passa la cifra: si lascia vuoto

            dmDot(g, x0 + c * passo, y0 + r * passo, diam, colore);
        }
}

static void disegnaGriglia(Arduino_GFX *g, int16_t cx, int16_t cy,
                           const char **griglia, int lato,
                           int16_t passo, int16_t diam, uint16_t colore,
                           int16_t orizzonte = 32767)
{
    int16_t x0 = cx - (lato * passo) / 2 + passo / 2;
    int16_t y0 = cy - (lato * passo) / 2 + passo / 2;
    for (int r = 0; r < lato; ++r)
        for (int c = 0; c < lato; ++c)
            if (griglia[r][c] == '#')
            {
                int16_t py = y0 + r * passo;
                if (py > orizzonte) continue;   // questa parte e' ancora sotto
                dmDot(g, x0 + c * passo, py, diam, colore);
            }
}

// Il tondo di nero che fa spazio all'astro, tagliato all'orizzonte.
// fillCircle non sa fermarsi a meta', quindi il cerchio si costruisce
// una riga alla volta e le righe sotto la linea non si disegnano.
static void spazioTondo(Arduino_GFX *g, int16_t cx, int16_t cy,
                        int16_t raggio, int16_t orizzonte)
{
    for (int16_t dy = -raggio; dy <= raggio; ++dy)
    {
        int16_t y = cy + dy;
        if (y > orizzonte) break;
        int16_t dx = (int16_t)lroundf(sqrtf((float)(raggio * raggio - dy * dy)));
        g->fillRect(cx - dx, y, 2 * dx + 1, 1, COL_SFONDO);
    }
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
    g->drawRoundRect(BORDO_MARGINE, BORDO_MARGINE,
                     LCD_W - 2 * BORDO_MARGINE, LCD_H - 2 * BORDO_MARGINE,
                     BORDO_RAGGIO, COL_CORNICE);

    // L'etichetta in alto a sinistra, piccola e molto spaziata.
    dmText(g, PADDING, PADDING, etichetta, 3, 2, COL_ETICHETTA, 2);
}

static void indicatoreBatteria(Arduino_GFX *g)
{
    if (batteria < 0) return;

    // Anche la batteria e' una matrice: undici punti per cinque, il
    // contorno acceso e le tacche interne che salgono con la carica.
    // Disegnarla con dei rettangoli pieni avrebbe stonato - sarebbe
    // stato l'unico oggetto sullo schermo a non essere fatto di punti.
    const int16_t passo = 4;
    const int16_t diam = 3;
    const int16_t colonne = 11;
    const int16_t righe = 5;

    // Allineata in verticale al centro dell'etichetta di sinistra.
    const int16_t x0 = LCD_W - PADDING - colonne * passo;
    const int16_t y0 = PADDING + 2;

    uint16_t colore = (alimentato || batteria <= 15) ? COL_ROSSO : COL_SECONDARIO;

    // Il contorno.
    for (int c = 0; c < colonne; ++c)
    {
        dmDot(g, x0 + c * passo, y0, diam, colore);
        dmDot(g, x0 + c * passo, y0 + (righe - 1) * passo, diam, colore);
    }
    for (int r = 1; r < righe - 1; ++r)
    {
        dmDot(g, x0, y0 + r * passo, diam, colore);
        dmDot(g, x0 + (colonne - 1) * passo, y0 + r * passo, diam, colore);
    }

    // Il polo positivo, il dentino sul lato corto.
    dmDot(g, x0 + colonne * passo, y0 + 2 * passo, diam, colore);

    // Le tacche di carica: nove colonne interne.
    const int tacche = colonne - 2;
    int piene = (batteria * tacche + 50) / 100;
    for (int c = 0; c < tacche; ++c)
        for (int r = 1; r < righe - 1; ++r)
            dmDot(g, x0 + (c + 1) * passo, y0 + r * passo, diam,
                  c < piene ? colore : COL_SPENTO);
}

// ------------------------------------------------------------
//  SCHEDA 1: L'ORA
// ------------------------------------------------------------

static void disegnaOra(Arduino_GFX *g, const struct tm &t)
{
    telaio(g, CITTA);

    char buf[16];

    // L'ora, grande al centro.
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    int16_t largoOra = dmTextWidth(buf, 10, 1);
    if (dmRullo(g, rulloOra, buf, (LCD_W - largoOra) / 2, 128, 10, 7, COL_ACCESO))
        numeriInMovimento = true;

    // I secondi come una riga di sessanta punti che si riempie.
    // Un quadrante, non un numero: si legge con la coda dell'occhio.
    const int16_t passoSec = 5;
    const int16_t larghezza = 60 * passoSec;
    const int16_t x0 = (LCD_W - larghezza) / 2 + passoSec / 2;
    for (int i = 0; i < 60; ++i)
    {
        uint16_t c = (i < t.tm_sec) ? COL_SECONDARIO : COL_SPENTO;
        if (i == t.tm_sec) c = COL_ROSSO;
        dmDot(g, x0 + i * passoSec, 238, 3, c);
    }

    // La data.
    snprintf(buf, sizeof(buf), "%s %02d %s",
             GIORNI[t.tm_wday % 7], t.tm_mday, MESI[t.tm_mon % 12]);
    dmTextCentered(g, LCD_W / 2, 278, buf, 5, 4, COL_ACCESO);

    // Da dove viene l'ora che stai leggendo. Sta sopra, appoggiata
    // all'orario: e' una nota a margine di quel numero, non un dato
    // per conto suo.
    const char *fonte = oraSincronizzata ? "SINCRONIZZATO" : "OROLOGIO INTERNO";
    dmTextCentered(g, LCD_W / 2, 84, fonte, 3, 2, COL_ETICHETTA, 2);

    // La barra dei comandi: a sinistra le sveglie, a destra ne
    // aggiungi una. Le zone toccabili sono piu' grandi dei simboli -
    // un dito non e' un puntatore, e centrare nove punti sarebbe un
    // esercizio di mira.
    disegnaGriglia(g, BARRA_SX, BARRA_Y, ICO_CAMPANA, ICONA_COMANDO, 4, 3,
                   sveglieAttive() > 0 ? COL_ACCESO : COL_SPENTO);

    if (sveglieAttive() > 0)
        disegnaBollo(g, BARRA_SX + 50, BARRA_Y, '0' + sveglieAttive(), 4, 3, COL_ACCESO);

    disegnaGriglia(g, BARRA_DX, BARRA_Y, ICO_PIU, ICONA_COMANDO, 4, 3, COL_SECONDARIO);
}

// ------------------------------------------------------------
//  SCHEDA 2: IL SOLE
// ------------------------------------------------------------

static void disegnaSole(Arduino_GFX *g, const struct tm &t)
{
    telaio(g, "SOLE");

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
    bool giornoChiaro = (adesso >= alba && adesso <= tramonto);

    // L'arco e' la traversata del cielo, e vale tutte le ventiquattro
    // ore: di giorno il Sole da est a ovest, di notte la Luna dal
    // tramonto all'alba. Cosi' il quadrante racconta sempre qualcosa,
    // invece di restare spento per meta' del tempo.
    const int16_t cx = LCD_W / 2;
    const int16_t cy = 228;
    const int16_t raggio = 110;
    const int totale = 31;

    float avanzamento;
    if (giornoChiaro)
    {
        avanzamento = (adesso - alba) / (tramonto - alba);
    }
    else
    {
        float durataNotte = (24.0f - tramonto) + alba;
        float trascorso = (adesso > tramonto) ? (adesso - tramonto)
                                              : ((24.0f - tramonto) + adesso);
        avanzamento = trascorso / durataNotte;
    }
    if (avanzamento < 0) avanzamento = 0;
    if (avanzamento > 1) avanzamento = 1;

    // Dove sta l'astro in questo istante. Sono tre situazioni, e la
    // prima e' quella che rende il movimento credibile: finche' non
    // stai guardando questa scheda, l'astro aspetta al capo dell'arco.
    // Se invece restasse dove il calcolo lo vuole, scorrendo lo
    // vedresti gia' arrivato, e l'animazione sembrerebbe un
    // ripensamento - il pianeta che sparisce e riparte.
    float mostrato;
    if (sorgereArmato)
    {
        mostrato = SORGERE_PARTENZA;
    }
    else if (sorgereAttivo)
    {
        uint32_t passato = millis() - sorgereInizio;
        if (passato >= SORGERE_DURATA)
        {
            sorgereAttivo = false;
            mostrato = avanzamento;
        }
        else
        {
            // Parte deciso e si posa dove deve: e' un corpo che
            // scivola verso una posizione, non una ruota che scatta.
            float t = (float)passato / (float)SORGERE_DURATA;
            float e = 1.0f - powf(1.0f - t, 3.0f);
            mostrato = SORGERE_PARTENZA + (avanzamento - SORGERE_PARTENZA) * e;
            numeriInMovimento = true;
        }
    }
    else
    {
        mostrato = avanzamento;
    }

    dmRing(g, cx, cy, raggio, totale, -90.0f, 180.0f,
           (int)lroundf(mostrato * totale), 5,
           COL_SECONDARIO, COL_SPENTO);

    // L'astro dove sta adesso. Non salta di punto in punto: la sua
    // posizione si calcola sull'angolo, quindi scivola lungo la curva
    // anche fra un punto e l'altro del quadrante.
    {
        float angolo = (-90.0f + 180.0f * mostrato - 90.0f) * (float)M_PI / 180.0f;
        int16_t ax = cx + (int16_t)lroundf(cosf(angolo) * raggio);
        int16_t ay = cy + (int16_t)lroundf(sinf(angolo) * raggio);

        // La linea che unisce i due capi dell'arco e' l'orizzonte:
        // sotto quella non si disegna niente. E' cosi' che l'astro
        // esce dal nulla invece di comparire gia' intero.
        const int16_t orizzonte = cy;

        // Si fa spazio: senza, i punti del quadrante spunterebbero da
        // sotto e sembrerebbe sporco.
        spazioTondo(g, ax, ay, 17, orizzonte);
        disegnaGriglia(g, ax, ay,
                       giornoChiaro ? ICO_SOLE_PICCOLO : ICO_LUNA, SOLE_LATO,
                       3, 3, giornoChiaro ? COL_SOLE : COL_LUNA, orizzonte);
    }

    // Quanto manca alla prossima transizione.
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

    // Il numero sotto l'arco: cosi' l'astro ha il cielo tutto per se'
    // e si vede scorrere senza niente davanti.
    int16_t largo = dmTextWidth(buf, 8, 1);
    if (dmRullo(g, rulloSole, buf, (LCD_W - largo) / 2, 248, 8, 6, COL_ACCESO))
        numeriInMovimento = true;

    dmTextCentered(g, cx, 318, verso, 3, 2, COL_ETICHETTA, 2);

    // Gli orari veri, ai due capi, con l'etichetta sopra.
    dmText(g, PADDING, 344, "ALBA", 2, 2, COL_ETICHETTA, 2);
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)alba, (int)((alba - (int)alba) * 60));
    dmText(g, PADDING, 372, buf, 3, 3, COL_ACCESO);

    int16_t w = dmTextWidth("TRAMONTO", 2, 2);
    dmText(g, LCD_W - PADDING - w, 344, "TRAMONTO", 2, 2, COL_ETICHETTA, 2);
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)tramonto, (int)((tramonto - (int)tramonto) * 60));
    w = dmTextWidth(buf, 3, 1);
    dmText(g, LCD_W - PADDING - w, 372, buf, 3, 3, COL_ACCESO);
}

// ------------------------------------------------------------
//  SCHEDA 3: DOMANI
// ------------------------------------------------------------

static void disegnaMeteo(Arduino_GFX *g)
{
    telaio(g, "DOMANI");

    if (!domani.valido)
    {
        dmTextCentered(g, LCD_W / 2, 190, "NO LINK", 8, 6, COL_SPENTO);
        const char *motivo = retePresente ? "SERVER NON RAGGIUNGIBILE" : "NESSUNA RETE WIFI";
        dmTextCentered(g, LCD_W / 2, 260, motivo, 3, 2, COL_ETICHETTA, 2);
        return;
    }

    disegnaGriglia(g, LCD_W / 2, 130, iconaPerCodice(domani.codice), ICONA_LATO, 6, 4, COL_ACCESO);

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
//  SCHEDA 4: IL LOGO
// ------------------------------------------------------------
//  Il logo non e' un'immagine incollata sullo schermo: e' passato
//  per la stessa griglia di tutto il resto. Il passo dei punti si
//  calcola qui invece che fissarlo nel file generato, cosi' se un
//  giorno rigeneri il logo piu' fitto o piu' rado si adatta da solo
//  allo spazio che ha.

#if HA_LOGO
static void disegnaLogo(Arduino_GFX *g)
{
    telaio(g, LOGO_ETICHETTA);

    const int16_t spazioX = LCD_W - 80;
    const int16_t spazioY = 300;

    int16_t passo = spazioX / LOGO_LARGHEZZA;
    int16_t passoY = spazioY / LOGO_ALTEZZA;
    if (passoY < passo) passo = passoY;
    if (passo < 2) passo = 2;

    // Punti grossi rispetto al passo: il logo deve leggersi come una
    // forma piena, non come una nuvola di puntini staccati.
    int16_t diam = (passo * 8) / 10;
    if (diam < 2) diam = 2;

    const int16_t x0 = (LCD_W - LOGO_LARGHEZZA * passo) / 2 + passo / 2;
    const int16_t y0 = 96 + (spazioY - LOGO_ALTEZZA * passo) / 2;

    for (int r = 0; r < LOGO_ALTEZZA; ++r)
        for (int c = 0; c < LOGO_LARGHEZZA; ++c)
            if (LOGO[r][c] == '#')
                dmDot(g, x0 + c * passo, y0 + r * passo, diam, LOGO_COLORE);

#if LOGO_CERCHIO
    // Il cerchio non viene dalla griglia: e' calcolato sull'angolo,
    // quindi i punti sono davvero equidistanti e la curva non ha
    // gradini. Quanti punti servono lo dice la circonferenza: uno
    // ogni passo, cosi' la spaziatura e' la stessa del resto.
    const int16_t cx = x0 + ((LOGO_LARGHEZZA - 1) * passo) / 2;
    const int16_t cy = y0 + ((LOGO_ALTEZZA - 1) * passo) / 2;
    const int16_t raggio = ((LOGO_LARGHEZZA - 1) * passo) / 2;

    int quanti = (int)(2.0f * (float)M_PI * raggio / passo);
    if (quanti < 12) quanti = 12;

    // L'arco si ferma poco prima del giro completo: altrimenti
    // l'ultimo punto finirebbe esattamente sopra il primo.
    float arco = 360.0f * (float)(quanti - 1) / (float)quanti;
    dmRing(g, cx, cy, raggio, quanti, 0.0f, arco, quanti, diam,
           LOGO_COLORE, LOGO_COLORE);
#endif

    if (strlen(LOGO_SOTTOTITOLO) > 0)
        dmTextCentered(g, LCD_W / 2, 402, LOGO_SOTTOTITOLO, 3, 2, COL_ETICHETTA, 2);
}
#endif

// ------------------------------------------------------------
//  SCHEDA 4: IL BAROMETRO
// ------------------------------------------------------------

#define BARO_X 46
#define BARO_LARGO 276
#define BARO_ALTO 104
#define BARO_BASSO 214
#define BARO_PUNTI 47

static void disegnaBaro(Arduino_GFX *g)
{
    telaio(g, "BAROMETRO");

    if (!baro.valido)
    {
        dmTextCentered(g, LCD_W / 2, 190, "NO LINK", 8, 6, COL_SPENTO);
        const char *motivo = retePresente ? "SERVER NON RAGGIUNGIBILE" : "NESSUNA RETE WIFI";
        dmTextCentered(g, LCD_W / 2, 260, motivo, 3, 2, COL_ETICHETTA, 2);
        return;
    }

    // La scala. Se in ventiquattro ore la pressione si e' mossa di
    // poco, allargare tutto al massimo trasformerebbe mezzo hPa di
    // oscillazione in una montagna: sotto una certa escursione la
    // finestra resta fissa, e il grafico si vede giustamente piatto.
    float minimo = baro.minimo, massimo = baro.massimo;
    const float minimaEscursione = 6.0f;
    if (massimo - minimo < minimaEscursione)
    {
        float mezzo = (massimo + minimo) / 2.0f;
        minimo = mezzo - minimaEscursione / 2.0f;
        massimo = mezzo + minimaEscursione / 2.0f;
    }

    float quanto = 1.0f;
    if (tracciaArmata)
    {
        quanto = 0.0f;
    }
    else if (tracciaAttiva)
    {
        uint32_t passato = millis() - tracciaInizio;
        if (passato >= TRACCIA_DURATA)
        {
            tracciaAttiva = false;
        }
        else
        {
            float t = (float)passato / (float)TRACCIA_DURATA;
            quanto = 1.0f - powf(1.0f - t, 3.0f);
            numeriInMovimento = true;
        }
    }

    const int16_t passo = BARO_LARGO / (BARO_PUNTI - 1);
    int fino = (int)lroundf(quanto * BARO_PUNTI);

    for (int i = 0; i < fino && i < BARO_PUNTI; ++i)
    {
        // I campioni sono ventiquattro, i punti molti di piu': fra
        // un'ora e l'altra il valore si interpola, altrimenti la
        // curva sarebbe una fila di gradini.
        float posizione = (float)i * (ORE_BARO - 1) / (float)(BARO_PUNTI - 1);
        int a = (int)posizione;
        int b = (a < ORE_BARO - 1) ? a + 1 : a;
        float frazione = posizione - a;
        float v = baro.valori[a] + (baro.valori[b] - baro.valori[a]) * frazione;

        float alto = (v - minimo) / (massimo - minimo);
        if (alto < 0) alto = 0;
        if (alto > 1) alto = 1;

        int16_t px = BARO_X + i * passo;
        int16_t py = BARO_BASSO - (int16_t)lroundf(alto * (BARO_BASSO - BARO_ALTO));

        if (i == BARO_PUNTI - 1)
        {
            // Adesso: un anello, non un punto piu' grosso. Ingrossare
            // il punto lo avrebbe fatto sembrare solo un valore piu'
            // importante; un cerchio intorno dice "sei qui", che e'
            // un'altra cosa.
            dmRing(g, px, py, 9, 11, 0.0f, 360.0f * 10.0f / 11.0f, 11, 3,
                   COL_ROSSO, COL_ROSSO);
            dmDot(g, px, py, 5, COL_ROSSO);
        }
        else
        {
            dmDot(g, px, py, 4, COL_SECONDARIO);
        }
    }

    // Il valore di adesso.
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(baro.valori[ORE_BARO - 1]));
    int16_t largo = dmTextWidth(buf, 8, 1);
    if (dmRullo(g, rulloBaro, buf, (LCD_W - largo) / 2 - 22, 248, 8, 6, COL_ACCESO))
        numeriInMovimento = true;
    dmText(g, (LCD_W + largo) / 2 - 14, 274, "HPA", 3, 2, COL_ETICHETTA, 2);

    // Dove sta andando. Il confronto e' con tre ore fa: piu' indietro
    // e' storia, piu' vicino e' rumore.
    float delta = baro.valori[ORE_BARO - 1] - baro.valori[ORE_BARO - 4];
    const char *tendenza;
    const char *significa;
    if (delta > 1.0f)       { tendenza = "IN SALITA";  significa = "TEMPO IN MIGLIORAMENTO"; }
    else if (delta < -1.0f) { tendenza = "IN CALO";    significa = "TEMPO IN PEGGIORAMENTO"; }
    else                    { tendenza = "STABILE";    significa = "NESSUN CAMBIAMENTO"; }

    dmTextCentered(g, LCD_W / 2, 314, tendenza, 4, 3,
                   delta < -1.0f ? COL_ROSSO : COL_ACCESO);
    dmTextCentered(g, LCD_W / 2, 348, significa, 2, 2, COL_ETICHETTA, 2);

    // I due estremi della giornata, ai lati.
    dmText(g, PADDING, 380, "MIN", 2, 2, COL_ETICHETTA, 2);
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(baro.minimo));
    dmText(g, PADDING + 34, 378, buf, 3, 3, COL_SECONDARIO);

    int16_t w = dmTextWidth("MAX", 2, 2);
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(baro.massimo));
    int16_t wv = dmTextWidth(buf, 3, 1);
    dmText(g, LCD_W - PADDING - wv - w - 10, 380, "MAX", 2, 2, COL_ETICHETTA, 2);
    dmText(g, LCD_W - PADDING - wv, 378, buf, 3, 3, COL_SECONDARIO);
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
    case SCHEDA_BARO:  disegnaBaro(scheda[i]);   break;
#if HA_LOGO
    case SCHEDA_LOGO:  disegnaLogo(scheda[i]);    break;
#endif
    }
    daRidisegnare[i] = false;
}

// ------------------------------------------------------------
//  COMPOSIZIONE E SCORRIMENTO
// ------------------------------------------------------------

// I pallini in basso. Quello rosso non salta da una posizione
// all'altra: scivola insieme al contenuto, cosi' durante il
// trascinamento sai sempre a che punto sei del passaggio.
// Quanto sono visibili adesso: 1 mentre ci stai interagendo, poi
// pieni per un secondo e infine in dissolvenza.
static float palliniOpacita()
{
    if (ditoGiu || animazioneAttiva)
        return 1.0f;

    int32_t passato = (int32_t)(millis() - palliniFino);
    if (passato < 0) return 1.0f;
    if (passato >= PALLINI_FADE) return 0.0f;
    return 1.0f - (float)passato / (float)PALLINI_FADE;
}

static void disegnaPallini(Arduino_GFX *g, float posizione, float opacita)
{
    if (opacita <= 0.0f) return;

    const int16_t passo = 26;
    const int16_t y = LCD_H - 44;
    const int16_t x0 = LCD_W / 2 - (N_SCHEDE - 1) * passo / 2;

    for (int i = 0; i < N_SCHEDE; ++i)
        dmDot(g, x0 + i * passo, y, 6, dmSfuma(COL_SPENTO, opacita));

    float px = x0 + posizione * passo;
    dmDot(g, (int16_t)lroundf(px), y, 10, dmSfuma(COL_ROSSO, opacita));
}

// ------------------------------------------------------------
//  SCHERMATA: ELENCO DELLE SVEGLIE
// ------------------------------------------------------------

#define LISTA_ALTO 110
#define LISTA_BASSO 372
#define LISTA_RIGA 66

// L'interruttore, disegnato a punti come tutto il resto: una
// pillola con il cursore a sinistra o a destra. Non ha bisogno di
// scritte, la posizione dice gia' tutto.
static void disegnaInterruttore(Arduino_GFX *g, int16_t x, int16_t cy, bool acceso)
{
    const int16_t passo = 5;
    const int16_t colonne = 8;
    const int16_t righe = 4;
    const int16_t y0 = cy - (righe - 1) * passo / 2;

    uint16_t colore = acceso ? COL_ROSSO : COL_SPENTO;

    for (int c = 0; c < colonne; ++c)
    {
        dmDot(g, x + c * passo, y0, 3, colore);
        dmDot(g, x + c * passo, y0 + (righe - 1) * passo, 3, colore);
    }
    for (int r = 1; r < righe - 1; ++r)
    {
        dmDot(g, x, y0 + r * passo, 3, colore);
        dmDot(g, x + (colonne - 1) * passo, y0 + r * passo, 3, colore);
    }

    // Il cursore: tre colonne piene, a destra se acceso.
    int16_t base = acceso ? (colonne - 4) : 1;
    for (int c = 0; c < 3; ++c)
        for (int r = 1; r < righe - 1; ++r)
            dmDot(g, x + (base + c) * passo, y0 + r * passo, 4, colore);
}

static void disegnaLista(Arduino_GFX *g)
{
    telaio(g, "SVEGLIE");

    disegnaGriglia(g, LCD_W - PADDING - 4, PADDING + 8, ICO_CHIUDI,
                   ICONA_COMANDO, 4, 3, COL_SECONDARIO);

    if (nSveglie == 0)
    {
        dmTextCentered(g, LCD_W / 2, 200, "NESSUNA SVEGLIA", 4, 3, COL_SPENTO);
        dmTextCentered(g, LCD_W / 2, 240, "TOCCA IL PIU PER AGGIUNGERE", 2, 2,
                       COL_ETICHETTA, 2);
    }

    char buf[8];
    for (int i = 0; i < nSveglie; ++i)
    {
        int16_t y = LISTA_ALTO + i * LISTA_RIGA - (int16_t)lroundf(listaScorrimento);
        int16_t cy = y + LISTA_RIGA / 2;

        // Fuori dalla finestra della lista non si disegna: le righe
        // devono sparire sotto il bordo, non sopra l'intestazione.
        if (cy < LISTA_ALTO - 10 || cy > LISTA_BASSO + 10)
            continue;

        snprintf(buf, sizeof(buf), "%02d:%02d", sveglie[i].ore, sveglie[i].minuti);
        dmText(g, PADDING, cy - 17, buf, 5, 4,
               sveglie[i].attiva ? COL_ACCESO : COL_SPENTO);

        disegnaInterruttore(g, LCD_W - PADDING - 40, cy, sveglie[i].attiva);
    }

    // Il piu', in fondo al centro.
    if (nSveglie < MAX_SVEGLIE)
        disegnaGriglia(g, LCD_W / 2, 404, ICO_PIU, ICONA_COMANDO, 5, 4, COL_SECONDARIO);
}

// ------------------------------------------------------------
//  SCHERMATA: IMPOSTA UN ORARIO
// ------------------------------------------------------------

#define EDITOR_ORE_X 40
#define EDITOR_MIN_X 220
#define EDITOR_Y 150
#define EDITOR_PASSO 10

static void disegnaEditor(Arduino_GFX *g)
{
    telaio(g, editorIndice < 0 ? "NUOVA SVEGLIA" : "MODIFICA");

    disegnaGriglia(g, LCD_W - PADDING - 4, PADDING + 8, ICO_CHIUDI,
                   ICONA_COMANDO, 4, 3, COL_SECONDARIO);

    // Il campo scelto lampeggia. Non e' decorazione: e' il modo in
    // cui l'oggetto dice "sto ascoltando questo", ed e' l'unico
    // segnale possibile quando non c'e' un cursore da mostrare.
    bool acceso = ((millis() / 420) % 2) == 0;

    char buf[4];
    bool anima = !editorInRipetizione;

    snprintf(buf, sizeof(buf), "%02d", editorOre);
    uint16_t colOre = (editorCampo == 0 && !acceso) ? COL_SPENTO : COL_ACCESO;
    if (dmRullo(g, rulloEditorOre, buf, EDITOR_ORE_X, EDITOR_Y, EDITOR_PASSO, 7,
                colOre, 1, 380, 70, anima))
        vistaDaRidisegnare = true;

    dmText(g, EDITOR_ORE_X + 2 * 6 * EDITOR_PASSO, EDITOR_Y, ":", EDITOR_PASSO, 7, COL_ACCESO);

    snprintf(buf, sizeof(buf), "%02d", editorMinuti);
    uint16_t colMin = (editorCampo == 1 && !acceso) ? COL_SPENTO : COL_ACCESO;
    if (dmRullo(g, rulloEditorMin, buf, EDITOR_MIN_X, EDITOR_Y, EDITOR_PASSO, 7,
                colMin, 1, 380, 70, anima))
        vistaDaRidisegnare = true;

    dmTextCentered(g, LCD_W / 2, 236, editorCampo == 0 ? "ORE" : "MINUTI",
                   2, 2, COL_ETICHETTA, 2);

    // Meno e piu'.
    disegnaGriglia(g, 90, 290, ICO_MENO, ICONA_COMANDO, 6, 5, COL_ACCESO);
    disegnaGriglia(g, LCD_W - 90, 290, ICO_PIU, ICONA_COMANDO, 6, 5, COL_ACCESO);

    // Salva, e se stai modificando anche elimina.
    disegnaGriglia(g, LCD_W / 2, 384, ICO_SPUNTA, ICONA_COMANDO, 6, 5, COL_ROSSO);
    if (editorIndice >= 0)
        disegnaGriglia(g, PADDING + 10, 384, ICO_CESTINO, ICONA_COMANDO, 4, 3, COL_SPENTO);
}

// ------------------------------------------------------------
//  SCHERMATA: STA SUONANDO
// ------------------------------------------------------------

static void disegnaAllarme(Arduino_GFX *g, const struct tm &t)
{
    bool acceso = ((millis() / 420) % 2) == 0;

    g->fillScreen(COL_SFONDO);

    disegnaGriglia(g, LCD_W / 2, 110, ICO_CAMPANA, ICONA_COMANDO, 7, 6,
                   acceso ? COL_ROSSO : COL_SPENTO);

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    dmTextCentered(g, LCD_W / 2, 200, buf, 10, 7, acceso ? COL_ACCESO : COL_SPENTO);

    dmTextCentered(g, LCD_W / 2, 300, "SVEGLIA", 4, 3, COL_ROSSO);
    dmTextCentered(g, LCD_W / 2, 370, "TOCCA PER SPEGNERE", 2, 2, COL_ETICHETTA, 2);
}

// ------------------------------------------------------------
//  QUANDO UNA SCHEDA ESCE DI SCENA
// ------------------------------------------------------------
//  Regola generale del progetto: le animazioni d'ingresso di una
//  scheda si riarmano solo quando di quella scheda non e' rimasto
//  visibile nemmeno un pixel.
//
//  Il motivo e' che un'animazione d'ingresso racconta un arrivo. Se
//  fai uno scorrimento a meta' e torni indietro non sei mai andato
//  da nessuna parte, e rivederla sarebbe come se l'oggetto si fosse
//  dimenticato di dove stava. Legare il riarmo alla visibilita'
//  invece che al cambio di scheda dice esattamente questo, e vale
//  per qualunque animazione aggiungeremo in futuro.

static bool schedaVisibile(int i)
{
    if (i == schedaCorrente) return true;

    int off = (int)lroundf(scorrimento);
    if (off > 0 && i == schedaCorrente + 1) return true;
    if (off < 0 && i == schedaCorrente - 1) return true;
    return false;
}

static void riarmaAnimazioni(int i)
{
    if (i == SCHEDA_SOLE)
    {
        sorgereAttivo = false;
        sorgereArmato = true;
        daRidisegnare[SCHEDA_SOLE] = true;
    }
    else if (i == SCHEDA_BARO)
    {
        tracciaAttiva = false;
        tracciaArmata = true;
        daRidisegnare[SCHEDA_BARO] = true;
    }
}

// Chiamata quando una scheda diventa quella in vista: se ha
// un'animazione d'ingresso pronta, parte adesso.
static void avviaAnimazioniIngresso()
{
    if (schedaCorrente == SCHEDA_SOLE && sorgereArmato)
    {
        sorgereArmato = false;
        sorgereAttivo = true;
        sorgereInizio = millis();
        ridisegna(SCHEDA_SOLE);
    }
    else if (schedaCorrente == SCHEDA_BARO && tracciaArmata)
    {
        tracciaArmata = false;
        tracciaAttiva = true;
        tracciaInizio = millis();
        ridisegna(SCHEDA_BARO);
    }
}

static void aggiornaVisibilita()
{
    static bool eraVisibile[N_SCHEDE] = {false};

    for (int i = 0; i < N_SCHEDE; ++i)
    {
        bool ora = schedaVisibile(i);
        if (eraVisibile[i] && !ora)
            riarmaAnimazioni(i);
        eraVisibile[i] = ora;
    }
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

    // La batteria e i pallini si disegnano qui, dopo che le schede
    // sono state affiancate: non appartengono a nessuna scheda in
    // particolare, sono l'involucro. Cosi' durante lo scorrimento
    // restano fermi mentre il contenuto scivola sotto - che e' anche
    // il modo in cui l'occhio capisce quali sono i comandi
    // dell'oggetto e quali il contenuto.
    indicatoreBatteria(comp);

    float opacita = palliniOpacita();
    disegnaPallini(comp, schedaCorrente + scorrimento / (float)LCD_W, opacita);
    palliniOpacitaDisegnata = opacita;
    comp->flush();

    aggiornaVisibilita();
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
        palliniFino = millis() + PALLINI_ATTESA;

        // Prima si prende atto di chi e' appena uscito di scena: e'
        // quello che riarma le animazioni. Solo dopo si guarda se la
        // scheda arrivata ne ha una pronta da far partire.
        aggiornaVisibilita();

        // Subito, o per un istante si vedrebbe gia' arrivata.
        avviaAnimazioniIngresso();

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

// ------------------------------------------------------------
//  DOVE HAI TOCCATO
// ------------------------------------------------------------

static bool dentro(int16_t x, int16_t y, int16_t cx, int16_t cy, int16_t raggio)
{
    return (abs(x - cx) <= raggio) && (abs(y - cy) <= raggio);
}

static void apriLista()
{
    vista = VISTA_LISTA;
    listaScorrimento = 0;
    vistaDaRidisegnare = true;
}

static void apriEditor(int indice)
{
    editorIndice = indice;
    if (indice >= 0)
    {
        editorOre = sveglie[indice].ore;
        editorMinuti = sveglie[indice].minuti;
    }
    else
    {
        // Una sveglia nuova parte dall'ora che e' adesso: e' quasi
        // sempre piu' vicina a quella che vuoi di un valore fisso.
        struct tm t;
        oraCorrente(t);
        editorOre = t.tm_hour;
        editorMinuti = t.tm_min;
    }
    editorCampo = 0;
    // Azzerati: aprendo la schermata i numeri sono gia' quelli
    // giusti, non devono arrivarci scorrendo.
    rulloEditorOre.testo[0] = '\0';
    rulloEditorMin.testo[0] = '\0';
    editorInRipetizione = false;
    vista = VISTA_EDITOR;
    vistaDaRidisegnare = true;
}

// Il tocco secco sulla barra in fondo alla scheda dell'ora.
static void tapNelleSchede()
{
    if (schedaCorrente != SCHEDA_ORA) return;

    if (dentro(tapX, tapY, BARRA_SX, BARRA_Y, BERSAGLIO))
        apriLista();
    else if (dentro(tapX, tapY, BARRA_DX, BARRA_Y, BERSAGLIO))
        apriEditor(-1);
}

// ------------------------------------------------------------
//  IL TOCCO NELLE SCHERMATE DELLA SVEGLIA
// ------------------------------------------------------------
//  Qui non c'e' il carosello: i gesti sono due, il tocco secco sui
//  comandi e lo scorrimento verticale dell'elenco.

static void modificaCampo(int verso)
{
    if (editorCampo == 0)
        editorOre = (editorOre + 24 + verso) % 24;
    else
        editorMinuti = (editorMinuti + 60 + verso) % 60;
    vistaDaRidisegnare = true;
}

static void tapNellEditor()
{
    if (dentro(tapX, tapY, LCD_W - PADDING - 4, PADDING + 8, BERSAGLIO))
    {
        apriLista();   // annulla e torna all'elenco
        return;
    }

    // I due campi dell'orario: toccare quello che vuoi cambiare e'
    // piu' diretto che tenere premuto per selezionarlo, che nelle
    // sveglie di una volta era l'unica strada perche' i tasti erano
    // due e fissi.
    if (tapY >= EDITOR_Y - 20 && tapY <= EDITOR_Y + 7 * EDITOR_PASSO + 20)
    {
        if (tapX < LCD_W / 2 - 10) { editorCampo = 0; vistaDaRidisegnare = true; return; }
        if (tapX > LCD_W / 2 + 10) { editorCampo = 1; vistaDaRidisegnare = true; return; }
    }

    if (dentro(tapX, tapY, 90, 290, BERSAGLIO)) { modificaCampo(-1); return; }
    if (dentro(tapX, tapY, LCD_W - 90, 290, BERSAGLIO)) { modificaCampo(+1); return; }

    if (dentro(tapX, tapY, LCD_W / 2, 384, BERSAGLIO))
    {
        if (editorIndice >= 0)
        {
            sveglie[editorIndice].ore = editorOre;
            sveglie[editorIndice].minuti = editorMinuti;
            sveglie[editorIndice].attiva = true;
            sveglieSalva();
        }
        else
        {
            sveglieAggiungi(editorOre, editorMinuti);
        }
        daRidisegnare[SCHEDA_ORA] = true;
        apriLista();
        return;
    }

    if (editorIndice >= 0 && dentro(tapX, tapY, PADDING + 10, 384, BERSAGLIO))
    {
        sveglieRimuovi(editorIndice);
        daRidisegnare[SCHEDA_ORA] = true;
        apriLista();
    }
}

static void tapNellaLista()
{
    if (dentro(tapX, tapY, LCD_W - PADDING - 4, PADDING + 8, BERSAGLIO))
    {
        vista = VISTA_SCHEDE;
        daRidisegnare[SCHEDA_ORA] = true;
        componi();
        return;
    }

    if (nSveglie < MAX_SVEGLIE && dentro(tapX, tapY, LCD_W / 2, 404, BERSAGLIO))
    {
        apriEditor(-1);
        return;
    }

    for (int i = 0; i < nSveglie; ++i)
    {
        int16_t cy = LISTA_ALTO + i * LISTA_RIGA - (int16_t)lroundf(listaScorrimento)
                     + LISTA_RIGA / 2;
        if (tapY < cy - LISTA_RIGA / 2 || tapY > cy + LISTA_RIGA / 2) continue;
        if (cy < LISTA_ALTO - 10 || cy > LISTA_BASSO + 10) continue;

        // Meta' destra: accendi o spegni. Meta' sinistra: apri.
        if (tapX > LCD_W - PADDING - 70)
        {
            sveglie[i].attiva = !sveglie[i].attiva;
            sveglieSalva();
            daRidisegnare[SCHEDA_ORA] = true;
            vistaDaRidisegnare = true;
        }
        else
        {
            apriEditor(i);
        }
        return;
    }
}

static void gestisciToccoModale()
{
    static bool giu = false;
    static int16_t partenzaY = 0;
    static float scorrimentoPartenzaY = 0;
    static bool mosso = false;
    static uint32_t ripetiDa = 0;
    static uint32_t ripetiUltima = 0;

    TouchPoint tp = touch.leggi();
    uint32_t adesso = millis();

    if (tp.premuto && !giu)
    {
        giu = true;
        mosso = false;
        tapX = tp.x;
        tapY = tp.y;
        partenzaY = tp.y;
        scorrimentoPartenzaY = listaScorrimento;

        if (vista == VISTA_EDITOR)
            ripetiDa = adesso + 450;   // da qui in poi tenere premuto ripete
    }
    else if (tp.premuto && giu)
    {
        if (abs(tp.y - partenzaY) > 8 || abs(tp.x - tapX) > 8)
            mosso = true;

        if (vista == VISTA_LISTA && mosso)
        {
            float massimo = (float)(nSveglie * LISTA_RIGA) - (LISTA_BASSO - LISTA_ALTO);
            if (massimo < 0) massimo = 0;
            listaScorrimento = scorrimentoPartenzaY + (partenzaY - tp.y);
            if (listaScorrimento < 0) listaScorrimento = 0;
            if (listaScorrimento > massimo) listaScorrimento = massimo;
            vistaDaRidisegnare = true;
        }

        // Tenere premuto su piu' o meno fa scorrere i numeri, come su
        // qualunque sveglia: toccare sessanta volte per mezz'ora non
        // e' un'interazione, e' una penitenza.
        if (vista == VISTA_EDITOR && !mosso && adesso > ripetiDa &&
            (adesso - ripetiUltima) > 110)
        {
            ripetiUltima = adesso;
            if (dentro(tapX, tapY, 90, 290, BERSAGLIO))
            {
                editorInRipetizione = true;
                modificaCampo(-1);
            }
            else if (dentro(tapX, tapY, LCD_W - 90, 290, BERSAGLIO))
            {
                editorInRipetizione = true;
                modificaCampo(+1);
            }
        }
    }
    else if (!tp.premuto && giu)
    {
        giu = false;
        editorInRipetizione = false;
        if (mosso) return;

        switch (vista)
        {
        case VISTA_ALLARME:
            vista = VISTA_SCHEDE;
            daRidisegnare[SCHEDA_ORA] = true;
            componi();
            break;
        case VISTA_LISTA:  tapNellaLista(); break;
        case VISTA_EDITOR: tapNellEditor(); break;
        default: break;
        }
    }
}

static void disegnaVista()
{
    struct tm t;
    oraCorrente(t);

    // Si azzera PRIMA di disegnare. E' durante il disegno che un
    // numero ancora in movimento rialza la richiesta per dire "non
    // ho finito": azzerarla dopo la cancellerebbe ogni volta, e
    // l'animazione avanzerebbe solo quando qualcos'altro obbliga a
    // ridisegnare.
    vistaDaRidisegnare = false;

    switch (vista)
    {
    case VISTA_LISTA:   disegnaLista(comp); break;
    case VISTA_EDITOR:  disegnaEditor(comp); break;
    case VISTA_ALLARME: disegnaAllarme(comp, t); break;
    default: return;
    }
    comp->flush();
}

// ------------------------------------------------------------
//  QUANDO SCATTA UNA SVEGLIA
// ------------------------------------------------------------

static void controllaSveglie(const struct tm &t)
{
    if (vista == VISTA_ALLARME) return;

    // Un minuto solo per ogni scatto: senza questo la sveglia
    // ripartirebbe di continuo per tutti i sessanta secondi, anche
    // subito dopo che l'hai spenta.
    int minuto = t.tm_hour * 60 + t.tm_min;
    if (minuto == allarmeUltimoMinuto) return;

    for (int i = 0; i < nSveglie; ++i)
    {
        if (!sveglie[i].attiva) continue;
        if (sveglie[i].ore != t.tm_hour || sveglie[i].minuti != t.tm_min) continue;

        allarmeUltimoMinuto = minuto;
        allarmeIndice = i;
        vista = VISTA_ALLARME;
        vistaDaRidisegnare = true;

        // Se lo schermo dormiva, una sveglia muta e nera non
        // servirebbe a niente.
        if (!schermoAcceso)
        {
            schermoAcceso = true;
            panel->displayOn();
            panel->setBrightness(LUMINOSITA);
        }

        Serial.printf("[sveglia] scattata: %02d:%02d\n", sveglie[i].ore, sveglie[i].minuti);
        return;
    }
}

static void gestisciTocco()
{
    TouchPoint tp = touch.leggi();
    uint32_t adesso = millis();

    if (tp.premuto)
        palliniFino = adesso + PALLINI_ATTESA;

    if (tp.premuto && !ditoGiu)
    {
        // Dito appoggiato: da qui in poi comanda lui, e qualunque
        // animazione in corso si interrompe sul posto.
        ditoGiu = true;
        eraTap = true;
        animazioneAttiva = false;
        animDirezione = 0;
        ditoPartenzaX = tp.x;
        tapX = tp.x;
        tapY = tp.y;
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
            // Un tocco secco non cambia scheda: rimette a posto
            // eventuali pixel di scarto e guarda se hai centrato
            // uno dei comandi.
            scorrimento = 0;
            componi();
            tapNelleSchede();
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
//  IL PULSANTE
// ------------------------------------------------------------
//  Su questa scheda non c'e' un tasto di accensione utilizzabile:
//  il PWR e' cablato al chip dell'alimentazione e una pressione
//  lunga spegne tutto. Il BOOT invece serve al chip solo nei primi
//  istanti dopo l'accensione: da li' in poi e' un pulsante libero,
//  e lo usiamo per mettere in standby lo schermo.
//
//  displayOff() non abbassa la luminosita': manda proprio il
//  pannello a dormire. Su un AMOLED significa che i pixel non
//  emettono piu' niente, che e' l'unico modo di spegnerlo davvero.

static void gestisciPulsante()
{
    static bool precedente = HIGH;
    static uint32_t ultimoCambio = 0;

    bool ora = digitalRead(BTN_BOOT);

    // I contatti di un pulsante, nel momento in cui si chiudono,
    // rimbalzano per qualche millisecondo: senza questa attesa un
    // colpo solo verrebbe letto come venti, e lo schermo
    // lampeggerebbe invece di cambiare stato.
    if (precedente == HIGH && ora == LOW && (millis() - ultimoCambio) > 300)
    {
        ultimoCambio = millis();
        schermoAcceso = !schermoAcceso;

        if (schermoAcceso)
        {
            panel->displayOn();
            panel->setBrightness(LUMINOSITA);
            componi();
            Serial.println("[schermo] acceso");
        }
        else
        {
            panel->displayOff();
            Serial.println("[schermo] in standby (premi BOOT per riaccendere)");
        }
    }

    precedente = ora;
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

    sveglieCarica();
    Serial.printf("[sveglie] %d salvate, %d attive\n", nSveglie, sveglieAttive());

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
    struct tm adesso;
    oraCorrente(adesso);

    // Prima di tutto il resto, e comunque: una sveglia che non
    // suona perche' l'utente stava guardando un'altra schermata
    // non e' una sveglia.
    controllaSveglie(adesso);

    // A schermo spento non c'e' niente da calcolare: si ascolta
    // solo il pulsante. Ridisegnare un'immagine che nessuno vede
    // terrebbe sveglio il chip per nulla.
    if (!schermoAcceso)
    {
        gestisciPulsante();
        delay(20);
        return;
    }

    // Nelle schermate della sveglia il carosello e' sospeso.
    if (vista != VISTA_SCHEDE)
    {
        gestisciToccoModale();

        // Il campo scelto e la campana dell'allarme lampeggiano: si
        // ridisegna quando cambia lo stato, non a ritmo fisso.
        static bool ultimoLampeggio = false;
        bool lampeggia = (vista == VISTA_EDITOR || vista == VISTA_ALLARME);
        bool ora = ((millis() / 420) % 2) == 0;

        if (vistaDaRidisegnare || (lampeggia && ora != ultimoLampeggio))
        {
            ultimoLampeggio = ora;
            disegnaVista();
        }

        gestisciPulsante();
        delay(5);
        return;
    }

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

    const struct tm &t = adesso;

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
    if (daRidisegnare[schedaCorrente] || numeriInMovimento)
    {
        numeriInMovimento = false;
        ridisegna(schedaCorrente);   // se un numero scorre ancora, lo rialza
        componi();
    }
    else if (fabsf(palliniOpacita() - palliniOpacitaDisegnata) > 0.02f)
    {
        // I pallini si stanno spegnendo: basta ricomporre, le schede
        // sono gia' pronte e non serve ridisegnare niente.
        componi();
    }

    gestisciPulsante();

    delay(5);
}
