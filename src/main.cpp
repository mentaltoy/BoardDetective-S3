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
#include <ArduinoOTA.h>
#include <time.h>
#include <esp_sntp.h>
#include <math.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#include "dotmatrix.h"
#include "touch.h"
#include "rtc.h"
#include "sveglia.h"
#include "audio.h"
#include "imu.h"
#include "web.h"
#include "clima.h"

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

// Lo specchio nel browser: metti 1 per accenderlo, 0 per spegnerlo.
// Misurato, non rallenta il disegno - vive sull'altro core - ma
// tiene il WiFi sveglio, e su un oggetto a batteria quello si paga.
// Quando e' spento il codice resta tutto al suo posto: cambia solo
// questo numero.
#define SPECCHIO 0

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
// Il centro di un'icona sta a mezza icona dal margine, non sul
// margine: e' il suo bordo che deve rispettare il padding, non il
// suo centro. Nove punti a passo quattro fanno 36 pixel, quindi 18.
#define MEZZA_ICONA 18
#define BARRA_SX (PADDING + MEZZA_ICONA)
#define BARRA_DX (LCD_W - PADDING - MEZZA_ICONA)
#define BARRA_VENT 232

// Quanto largo e' il bersaglio di un comando. Un dito copre quasi
// mezzo centimetro di vetro e non vede cosa sta coprendo: la zona
// che risponde deve essere molto piu' grande del simbolo disegnato.
#define BERSAGLIO 46

// ------------------------------------------------------------
//  IL PASSO DEI QUADRANTI
// ------------------------------------------------------------
//  Lo detta la scheda dell'ora, e tutte le altre si adeguano.
//  Trenta tacche perche' e' il massimo che sta nel margine senza
//  infittire i punti al punto da non sembrare piu' una matrice di
//  led: sessanta vorrebbero cinque pixel di passo, e a quella
//  distanza i punti si fondono in una riga continua.
//
//  Il marcatore pero' avanza ogni secondo lo stesso, muovendosi di
//  mezza tacca alla volta: a secondi pari sta sopra un punto, a
//  secondi dispari fra due. Un orologio deve battere il secondo,
//  e questo e' il modo di farlo senza tradire la griglia.
#define TACCHE_MINUTO 30
#define PASSO_QUADRANTE ((LCD_W - 2 * PADDING) / (TACCHE_MINUTO - 1))

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
#define COL_BOLLA 0xED07   // ambra
#define COL_TRAMA 0x18E3   // quasi nero: si sente, non si vede

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
#define N_SCHEDE 9
#else
#define N_SCHEDE 8
#endif

#define SCHEDA_ORA 0
#define SCHEDA_CRONO 1
#define SCHEDA_SOLE 2
#define SCHEDA_METEO 3
#define SCHEDA_BARO 4
#define SCHEDA_ARIA 5
#define SCHEDA_CLIMA 6     // il salone
#define SCHEDA_CLIMA2 7    // la camera
#define SCHEDA_LOGO 8

// Da quale scheda si comanda quale macchina.
#define CLIMA_DI(scheda) (climi[(scheda) == SCHEDA_CLIMA ? 0 : 1])

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
    VISTA_ALLARME,    // sta suonando
    VISTA_GIRI        // l'elenco dei giri del cronografo
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

// Le bollicine dell'aria: all'ingresso salgono dal fondo, poi
// continuano a muoversi finche' resti a guardarle.
// L'icona del meteo si condensa un punto alla volta.
#define ICONA_DURATA 1200
static uint32_t iconaInizio = 0;
static bool iconaAttiva = false;
static bool iconaArmata = true;

#define BOLLE_DURATA 1100
static uint32_t bolleInizio = 0;
static bool bolleAttive = false;
static bool bolleArmate = true;

static DmRullo rulloOra;
static DmRullo rulloSole;
static DmRullo rulloBaro;
static DmRullo rulloAria;
static DmRullo rulloClima[N_CLIMI];
static DmRullo rulloModo[N_CLIMI];
static DmRullo rulloCrono;

// ------------------------------------------------------------
//  IL CRONOMETRO
// ------------------------------------------------------------
//  Il tempo non si accumula sommando pezzetti a ogni giro: si
//  guarda l'orologio all'avvio e si sottrae. Sommare significa
//  raccogliere tutti gli errori di arrotondamento uno dopo l'altro,
//  e un cronometro che perde mezzo secondo ogni dieci minuti non e'
//  un cronometro.

static bool cronoAttivo = false;
static uint32_t cronoPartenza = 0;    // quando e' stato fatto partire
static uint32_t cronoAccumulato = 0;  // quanto aveva gia' fatto prima dell'ultima pausa
static uint32_t cronoGiroDa = 0;      // da dove si conta il giro in corso
static uint32_t cronoGiroUltimo = 0;  // quanto e' durato l'ultimo giro chiuso

// I giri si accumulano: uno solo non serve a niente, perche' il senso
// di un giro e' il confronto con quelli prima.
#define MAX_GIRI 24
static uint32_t cronoGiri[MAX_GIRI];
static int cronoNGiri = 0;

static uint32_t cronoTempo()
{
    return cronoAccumulato + (cronoAttivo ? (millis() - cronoPartenza) : 0);
}

static void cronoAvviaFerma()
{
    if (cronoAttivo)
    {
        cronoAccumulato += millis() - cronoPartenza;
        cronoAttivo = false;
    }
    else
    {
        cronoPartenza = millis();
        cronoAttivo = true;
    }
}

// Azzerare non e' istantaneo: la lancetta torna indietro fino allo
// zero. Su un cronografo meccanico quel ritorno esiste davvero, ed e'
// anche il modo di far vedere che il comando e' stato ricevuto -
// altrimenti premi e non succede niente di visibile, perche' il
// quadrante era gia' quasi fermo.
#define AZZERA_DURATA 420
static bool azzeraInCorso = false;
static uint32_t azzeraInizio = 0;
static float azzeraDa = 0;

static void cronoAzzera()
{
    azzeraDa = (cronoTempo() % 60000UL) / 60000.0f;
    if (azzeraDa > 0.001f)
    {
        azzeraInizio = millis();
        azzeraInCorso = true;
    }

    cronoAttivo = false;
    cronoAccumulato = 0;
    cronoGiroDa = 0;
    cronoGiroUltimo = 0;
    cronoNGiri = 0;
}

// Il giro: quanto e' passato dall'ultima volta che hai premuto. Su un
// cronografo serve a misurare i pezzi di una cosa lunga senza
// fermare il conto totale.
static void cronoGiro()
{
    uint32_t adesso = cronoTempo();
    cronoGiroUltimo = adesso - cronoGiroDa;
    cronoGiroDa = adesso;

    if (cronoNGiri < MAX_GIRI)
        cronoGiri[cronoNGiri++] = cronoGiroUltimo;
}

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
static int ultimoSecondoInvolucro = -1;
static int ultimoMinuto = -1;
static uint32_t prossimoMeteo = 0;
static uint32_t prossimoClima = 0;

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
//  QUALITA' DELL'ARIA
// ------------------------------------------------------------
//  L'indice europeo mette insieme cinque inquinanti e ne restituisce
//  uno solo, tarato in modo che il numero sia confrontabile fra
//  paesi diversi. Zero e' aria pulita, cento e' aria da restare in
//  casa.

struct Aria
{
    bool valido = false;
    int indice = 0;
    float pm25 = 0;
    float pm10 = 0;
};
static Aria aria;

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

static void axpScrivi(uint8_t reg, uint8_t valore)
{
    Wire.beginTransmission(AXP_ADDR);
    Wire.write(reg);
    Wire.write(valore);
    Wire.endTransmission();
}

// Quali regolatori secondari sono accesi all'avvio: e' lo stato a cui
// si torna ogni volta che ci si risveglia.
static uint8_t ldoNormali = 0;
static bool imuPronto = false;

// Quali alimentazioni si spengono in standby: tutte quelle
// secondarie. Provate una per una a schermo acceso, nessuna di
// queste alimenta il pannello - lui sta su un convertitore
// principale, che non si tocca. Ed e' anche il motivo per cui il
// risveglio faceva crashare: rimettevo in piedi un display che non
// era mai caduto, e la sua funzione di avvio reinizializza il bus.
#define LDO_DA_SPEGNERE 0xFF

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
    WiFi.setAutoReconnect(true);   // se cade, ci riprova da solo
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

// Il server chiama questa quando l'ora e' arrivata davvero. E'
// l'unico segnale affidabile: chiedere "lo stato della
// sincronizzazione" su questo core non riporta mai il
// completamento, anche quando l'ora viene corretta eccome.
static volatile bool ntpArrivato = false;

static void ntpNotifica(struct timeval *)
{
    ntpArrivato = true;
}

static void sincronizzaOra()
{
    if (!retePresente) return;

    // configTzTime fa due cose insieme: dice a che server chiedere
    // l'ora e in che fuso siamo. Da quel momento localtime() tiene
    // conto dell'ora legale da solo.
    // La notifica va messa prima di far partire il client, o il
    // primo aggiornamento passerebbe inosservato.
    ntpArrivato = false;
    sntp_set_time_sync_notification_cb(ntpNotifica);

    configTzTime(FUSO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

    // Aspettare che "l'ora sia plausibile" non basterebbe: una
    // plausibile ce l'ha gia' data l'orologio interno, il controllo
    // passerebbe subito e la risposta del server arriverebbe dopo,
    // spostando l'ora di sorpresa. Qui si aspetta che sia arrivata.
    Serial.print("[ora] sincronizzazione");
    uint32_t limite = millis() + 12000;
    while (!ntpArrivato && millis() < limite)
    {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    oraSincronizzata = ntpArrivato && (time(nullptr) >= 1700000000);
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
    // Corto: questa chiamata blocca il ciclo, e un server che non
    // risponde non deve congelare lo schermo per otto secondi.
    http.setTimeout(4000);
    http.setConnectTimeout(3000);
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

static bool scaricaAria()
{
    if (!retePresente) return false;

    String url = "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=";
    url += String(LAT, 4);
    url += "&longitude=" + String(LON, 4);
    url += "&current=european_aqi,pm2_5,pm10&timezone=Europe%2FRome";

    HTTPClient http;
    http.setTimeout(4000);
    http.setConnectTimeout(3000);
    if (!http.begin(url)) return false;

    int codice = http.GET();
    if (codice != 200)
    {
        Serial.printf("[aria] risposta http %d\n", codice);
        http.end();
        return false;
    }

    String corpo = http.getString();
    http.end();

    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, corpo))
    {
        Serial.println("[aria] json illeggibile");
        return false;
    }

    JsonObject c = doc["current"];
    if (c.isNull()) return false;

    aria.indice = c["european_aqi"] | 0;
    aria.pm25 = c["pm2_5"] | 0.0f;
    aria.pm10 = c["pm10"] | 0.0f;
    aria.valido = true;
    daRidisegnare[SCHEDA_ARIA] = true;

    Serial.printf("[aria] indice %d, pm2.5 %.1f, pm10 %.1f\n",
                  aria.indice, aria.pm25, aria.pm10);
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

// Numeri sparsi ma sempre gli stessi. Moltiplicare l'indice e
// prendere il resto - che e' quello che facevo prima - non basta:
// resta una progressione, e all'occhio le bolle risultavano
// allineate. Qui i bit vengono rimescolati fra loro finche' due
// indici vicini non hanno piu' niente in comune.
static uint32_t mescola(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

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

static const char *ICO_AVVIA[ICONA_COMANDO] = {
    "..#......",
    "..##.....",
    "..###....",
    "..####...",
    "..#####..",
    "..####...",
    "..###....",
    "..##.....",
    "..#......",
};

static const char *ICO_FERMA[ICONA_COMANDO] = {
    ".##...##.",
    ".##...##.",
    ".##...##.",
    ".##...##.",
    ".##...##.",
    ".##...##.",
    ".##...##.",
    ".##...##.",
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

// La stessa griglia, ma che si accende un punto alla volta in ordine
// sparso. Ogni punto ha il suo momento, ricavato dalla sua posizione,
// e non si accende di scatto: sale di luminosita' in una frazione di
// secondo. Accenderli tutti insieme sarebbe stato un lampo, in fila
// una scansione: sparsi sembra che l'immagine si condensi.
static void disegnaGrigliaComparsa(Arduino_GFX *g, int16_t cx, int16_t cy,
                                   const char **griglia, int lato,
                                   int16_t passo, int16_t diam, uint16_t colore,
                                   float progresso)
{
    int16_t x0 = cx - (lato * passo) / 2 + passo / 2;
    int16_t y0 = cy - (lato * passo) / 2 + passo / 2;

    for (int r = 0; r < lato; ++r)
        for (int c = 0; c < lato; ++c)
        {
            if (griglia[r][c] != '#') continue;

            uint32_t seme = mescola((uint32_t)(r * 131 + c * 17 + 7));
            float mio = (float)(seme % 1000) / 1000.0f * 0.62f;

            float quanto = (progresso - mio) / 0.38f;
            if (quanto <= 0.0f) continue;
            if (quanto > 1.0f) quanto = 1.0f;

            // Non si accende sul posto: arriva da sopra mentre si
            // accende, e si posa dove deve stare. Il movimento e'
            // breve, tre quarti di passo della griglia: quel tanto
            // che basta perche' l'occhio veda una discesa invece di
            // un'apparizione.
            float scesa = (1.0f - quanto) * (float)passo * 3.5f;

            dmDot(g, x0 + c * passo, y0 + r * passo - (int16_t)scesa, diam,
                  dmSfuma(colore, quanto));
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
                        int16_t raggio, int16_t orizzonte,
                        int16_t alto = -32000)
{
    for (int16_t dy = -raggio; dy <= raggio; ++dy)
    {
        int16_t y = cy + dy;
        if (y > orizzonte) break;
        if (y < alto) continue;
        int16_t dx = (int16_t)lroundf(sqrtf((float)(raggio * raggio - dy * dy)));
        g->fillRect(cx - dx, y, 2 * dx + 1, 1, COL_SFONDO);
    }
}

// Il marcatore con il suo vuoto intorno. Sono sempre andati insieme:
// senza il disco di nero sotto, i punti del quadrante spuntano da
// dentro il marcatore e si vede un pallino bianco affiorare da sotto
// quello rosso. Tenerli in una funzione sola vuol dire che nessun
// quadrante puo' dimenticarsi il secondo pezzo.
#define ALONE 14

static void marcatoreConAlone(Arduino_GFX *g, int16_t cx, int16_t cy,
                              uint16_t colore,
                              int16_t alto = -32000, int16_t basso = 32000)
{
    spazioTondo(g, cx, cy, ALONE, basso, alto);
    dmMarcatore(g, cx, cy, 8, 5, colore, alto, basso);
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

// Centra un testo garantendo il margine. Se non ci sta, rimpicciolisce
// finche' non entra, invece di sbordare.
//
// E' una regola del progetto: niente tocca i bordi piu' di PADDING.
// Applicarla qui, una volta, invece che controllare a occhio ogni
// scritta, significa che vale anche per quelle che aggiungeremo
// domani - e che nessuna potra' dimenticarsela.
static void testoCentrato(Arduino_GFX *g, int16_t y, const char *s,
                          int16_t passo, int16_t diam, uint16_t colore,
                          int16_t gap = 1)
{
    const int16_t utile = LCD_W - 2 * PADDING;
    while (passo > 1 && dmTextWidth(s, passo, gap) > utile)
    {
        --passo;
        diam = (passo * 7) / 10;
        if (diam < 2) diam = 2;
    }
    dmTextCentered(g, LCD_W / 2, y, s, passo, diam, colore, gap);
}

// Come testoCentrato, ma dentro una colonna e allineato a sinistra.
static void testoInColonna(Arduino_GFX *g, int16_t x, int16_t y, const char *s,
                           int16_t passo, int16_t diam, uint16_t colore,
                           int16_t gap, int16_t larghezza)
{
    while (passo > 1 && dmTextWidth(s, passo, gap) > larghezza)
    {
        --passo;
        diam = (passo * 7) / 10;
        if (diam < 2) diam = 2;
    }
    dmText(g, x, y, s, passo, diam, colore, gap);
}

// ------------------------------------------------------------
//  IL BATTITO DELLA RETE
// ------------------------------------------------------------
//  Un pallino che si accende e si spegne una volta al secondo.
//  Nessuna dissolvenza: sfumare vorrebbe dire rifare lo schermo
//  piu' volte al secondo, e ogni fotogramma intero costa 36
//  millisecondi. Cosi' invece non costa niente - l'orologio si
//  ridisegna comunque a ogni scatto di secondo, e il pallino
//  cambia insieme a lui.
//
//  Non si spegne fino a sparire: passa al grigio dei punti spenti,
//  lo stesso di tutti gli altri quadranti. Un pallino che sparisce
//  del tutto sembrerebbe la rete che cade.

#define BATT_CX (LCD_W - PADDING - 11 * 4 - 26)
#define BATT_CY (PADDING + 10)

static void indicatoreRete(Arduino_GFX *g, int16_t cx, int16_t cy)
{
    if (!retePresente)
    {
        // Senza rete non pulsa e resta sbarrato. Un pallino
        // semplicemente spento potrebbe essere il battito colto nel
        // momento di riposo; una sbarra no.
        dmMarcatore(g, cx, cy, 6, 3, COL_SPENTO);
        for (int k = -3; k <= 3; ++k)
            dmDot(g, cx + k * 3, cy - k * 3, 3, COL_ACCESO);
        return;
    }

    bool acceso = (time(nullptr) % 2) == 0;
    dmMarcatore(g, cx, cy, 6, 3, acceso ? COL_ACCESO : COL_SPENTO);
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

// Fa passare il tempo per la pala: la velocita' insegue quella
// richiesta e l'angolo avanza di conseguenza.
static void avanzaVentola(Clima &clima)
{
    uint32_t adesso = millis();
    float dt = (adesso - clima.ultimoGiro) / 1000.0f;
    clima.ultimoGiro = adesso;
    if (dt > 0.2f) dt = 0.2f;

    float bersaglio = clima.acceso
                          ? (2.0f * (float)M_PI) / (climaGiroMs(clima) / 1000.0f)
                          : 0.0f;

    clima.velocita += (bersaglio - clima.velocita) * fminf(1.0f, dt * 1.6f);
    clima.angolo += clima.velocita * dt;
    if (clima.angolo > 2.0f * (float)M_PI) clima.angolo -= 2.0f * (float)M_PI;
}

static void disegnaVentola(Arduino_GFX *g, int16_t cx, int16_t cy,
                           int16_t raggio, float angolo, uint16_t colore)
{
    // Sotto una certa taglia le pale diventano due punti e l'anello
    // esterno si impasta con loro: la versione piccola e' piu' rada.
    bool minuta = (raggio < 30);
    int16_t passoPala = minuta ? 5 : 7;
    int16_t grossezza = minuta ? 4 : 5;

    // Tre pale, curve. Ogni pala e' una fila di punti che si allarga
    // dal centro ruotando: dritta sembrerebbe un'elica di carta,
    // piegata sembra una ventola.
    for (int pala = 0; pala < 3; ++pala)
    {
        float base = angolo + pala * (2.0f * (float)M_PI / 3.0f);
        for (int16_t r = minuta ? 6 : 10; r <= raggio - (minuta ? 2 : 12); r += passoPala)
        {
            float a = base + (float)r * (minuta ? 0.030f : 0.016f);
            dmDot(g, cx + (int16_t)lroundf(cosf(a) * r),
                     cy + (int16_t)lroundf(sinf(a) * r), grossezza, colore);
        }
    }

    // Il mozzo, e la griglia intorno solo quando c'e' spazio.
    if (minuta)
    {
        // Senza anello: alla taglia dell'iconcina le tre pale da sole
        // si leggono meglio di quanto si leggano dentro un cerchio,
        // che a quelle dimensioni finisce per chiuderle in una
        // macchia invece di incorniciarle.
        dmDot(g, cx, cy, 4, colore);
    }
    else
    {
        dmMarcatore(g, cx, cy, 6, 4, colore);

        int quanti = (int)(2.0f * (float)M_PI * raggio / PASSO_QUADRANTE);
        dmRing(g, cx, cy, raggio, quanti, 0.0f, 360.0f * (quanti - 1) / quanti,
               quanti, 4, COL_SPENTO, COL_SPENTO);
    }
}

// ------------------------------------------------------------
//  SCHEDA 2: IL CRONOMETRO
// ------------------------------------------------------------
//  Minuti e secondi scorrono come sull'orologio; i decimi no.
//  A dieci cambi al secondo un'animazione non farebbe in tempo a
//  finire che gia' ne parte un'altra, e si vedrebbero cifre
//  interrotte a meta' corsa invece di un numero che corre. Le cose
//  veloci devono essere nette.

#define CRONO_CX (LCD_W / 2)
#define CRONO_CY 216
#define CRONO_RAGGIO 136

// Sopra il quadrante c'e' l'etichetta, sotto le cifre: la stessa
// distanza da tutte e due, o l'occhio vede il disco scivolare da una
// parte.
#define CRONO_CIFRE_Y 372
#define CRONO_PULSANTI_Y 386
#define CRONO_BOLLO 70

// Una tacca: punti in fila lungo il raggio. Sui quadranti veri le
// tacche sono trattini, non pallini - e la differenza fra il minuto
// e il quinto di minuto si legge dalla loro lunghezza prima ancora
// che dal loro spessore.
static void cronoTacca(Arduino_GFX *g, float angolo, int16_t rDa, int16_t rA,
                       int16_t diam, uint16_t colore)
{
    float co = cosf(angolo), si = sinf(angolo);

    // Due passate, come per la lancetta: prima il vuoto, poi il pieno.
    // Cosi' la tacca si stacca dalla trama invece di doverla coprire
    // punto per punto - e il fondo puo' restare fitto quanto vuole
    // senza mai spuntare da sotto.
    for (int passata = 0; passata < 2; ++passata)
    {
        uint16_t tinta = (passata == 0) ? COL_SFONDO : colore;
        int16_t d = (passata == 0) ? diam + 5 : diam;

        for (int16_t r = rDa; r <= rA; r += 4)
            dmDot(g, CRONO_CX + (int16_t)lroundf(co * r),
                     CRONO_CY + (int16_t)lroundf(si * r), d, tinta);

        // Il punto sul bordo esterno si disegna comunque: con il passo
        // fisso l'ultimo cadeva dove capitava, e le tacche corte
        // finivano due pixel piu' dentro di quelle lunghe - si vedeva
        // che il cerchio esterno non era uno solo.
        dmDot(g, CRONO_CX + (int16_t)lroundf(co * rA),
                 CRONO_CY + (int16_t)lroundf(si * rA), d, tinta);
    }
}

// Il fondo del quadrante: una griglia regolare di punti quasi neri.
// Non deve vedersi, deve sentirsi. Serve a dare una superficie a cui
// la lancetta si stacchi sopra, e a far capire che il disco e' un
// fondo e non un buco. E' l'idea del Tapisserie degli orologi buoni
// ridotta a quello che questo schermo sa fare: se la si nota, e' gia'
// troppo forte.
static void cronoTrama(Arduino_GFX *g)
{
    const int16_t passo = 10;
    // Fin quasi alle tacche: prima si fermava trenta pixel prima e
    // fra la trama e il bordo restava un anello vuoto.
    // Fin quasi alle tacche, ma non addosso: a quattordici pixel i
    // punti della trama arrivavano a mescolarsi con quelli delle
    // tacche corte, e le tacche perdevano il loro stacco. Diciotto
    // lascia il respiro che serve a leggerle come cose diverse.
    const int16_t limite = CRONO_RAGGIO - 18;
    const int32_t limite2 = (int32_t)limite * limite;

    for (int16_t dy = -limite; dy <= limite; dy += passo)
        for (int16_t dx = -limite; dx <= limite; dx += passo)
            if ((int32_t)dx * dx + (int32_t)dy * dy <= limite2)
                dmDot(g, CRONO_CX + dx, CRONO_CY + dy, 3, COL_TRAMA);
}

// Le sessanta tacche: ogni cinque piu' lunga e piu' grossa. La
// differenza si legge dalla lunghezza prima ancora che dallo
// spessore, ed e' quello che permette di leggere un quadrante senza
// contare le tacche una per una.
static void cronoTacche(Arduino_GFX *g)
{
    for (int i = 0; i < 60; ++i)
    {
        float a = (i * 6.0f - 90.0f) * (float)M_PI / 180.0f;

        if ((i % 5) == 0)
            cronoTacca(g, a, CRONO_RAGGIO - 24, CRONO_RAGGIO, 5, COL_SECONDARIO);
        else
            cronoTacca(g, a, CRONO_RAGGIO - 10, CRONO_RAGGIO, 3, COL_ETICHETTA);
    }
}

// Il triangolo sopra lo zero: dice dov'e' il sessanta e si trova
// senza cercarlo anche mentre la lancetta corre. La punta guarda in
// fuori, verso la tacca; la base sta verso il centro.
static void cronoTriangolo(Arduino_GFX *g)
{
    const float a = -90.0f * (float)M_PI / 180.0f;
    const float co = cosf(a), si = sinf(a);

    const int16_t righe[3] = {CRONO_RAGGIO - 36, CRONO_RAGGIO - 44, CRONO_RAGGIO - 52};
    const int lati[3] = {0, 1, 2};

    for (int riga = 0; riga < 3; ++riga)
        for (int k = -lati[riga]; k <= lati[riga]; ++k)
        {
            int16_t px = CRONO_CX + (int16_t)lroundf(co * righe[riga] - si * k * 7);
            int16_t py = CRONO_CY + (int16_t)lroundf(si * righe[riga] + co * k * 7);
            dmDot(g, px, py, 5, COL_ACCESO);
        }
}

// La lancetta scorre, non scatta: siccome i decimi si contano
// davvero, una lancetta che salta direbbe una cosa falsa su cio' che
// si sta misurando. Ed e' una freccia - larga alla base, affilata in
// punta - perche' un bastoncino di spessore costante punta ovunque.
//
// I punti laterali non spariscono di colpo: si avvicinano all'asse
// fino a fondersi con quello centrale. Passando da tre punti a uno da
// un raggio all'altro si vedeva uno scalino, e una lancetta con uno
// scalino non e' affilata, e' rotta.
static void cronoLancetta(Arduino_GFX *g, uint32_t t)
{
    float giro;
    if (azzeraInCorso)
    {
        uint32_t passato = millis() - azzeraInizio;
        if (passato >= AZZERA_DURATA)
        {
            azzeraInCorso = false;
            giro = 0.0f;
        }
        else
        {
            // Parte deciso e si posa sullo zero, come una lancetta
            // che viene richiamata da una molla.
            float p = (float)passato / (float)AZZERA_DURATA;
            giro = azzeraDa * powf(1.0f - p, 2.2f);
            numeriInMovimento = true;
        }
    }
    else
    {
        giro = (t % 60000UL) / 60000.0f;
    }

    float a = (giro * 360.0f - 90.0f) * (float)M_PI / 180.0f;
    float co = cosf(a), si = sinf(a);
    uint16_t colore = cronoAttivo ? COL_ROSSO : COL_ACCESO;

    // La forma e' geometrica, non approssimata: un semicerchio dietro
    // il centro, due lati paralleli larghi quanto il suo diametro, e
    // in punta un triangolo equilatero con la base larga uguale.
    // Tre pezzi che si toccano dove finiscono, senza raccordi
    // inventati - e' cosi' che sono fatte le lancette vere.
    //
    // Per disegnarla non si segue il contorno: per ogni distanza dal
    // centro si calcola quanto e' larga li', e si riempie. Il
    // semicerchio da' Pitagora, il corpo una costante, la punta una
    // retta che scende a zero.
    const float RP = 8.0f;                       // raggio del perno
    const int16_t rPunta = CRONO_RAGGIO - 2;
    const float hPunta = 2.0f * RP * 0.866f;     // altezza di un equilatero di lato 2*RP
    const float rSpalla = rPunta - hPunta;

    // Due passate. Prima tutto il vuoto, poi tutto il pieno: facendo
    // vuoto e pieno insieme punto per punto, il vuoto di ognuno
    // cancellerebbe il pieno del precedente.
    for (int passata = 0; passata < 2; ++passata)
    {
        uint16_t tinta = (passata == 0) ? COL_SFONDO : colore;
        int16_t grossezza = (passata == 0) ? 11 : 5;

        // Il passo si infittisce nel semicerchio dietro il centro: li'
        // la larghezza cambia in fretta - a un capo e' zero, a meta'
        // e' quasi piena - e con il passo del corpo la curva veniva a
        // gradini, con l'ultimo punto che sporgeva da solo.
        // A punti staccati, non a massa piena. La forma resta quella -
        // semicerchio, paralleli, triangolo - ma invece di riempirla
        // fitta si mette un punto ogni sette pixel, in righe e
        // colonne: cosi' la lancetta e' fatta della stessa materia di
        // tutto il resto dello schermo, invece di essere l'unica cosa
        // solida in mezzo a una matrice di led.
        //
        // Il passo resta fitto solo nel semicerchio dietro il centro,
        // dove la larghezza cambia troppo in fretta perche' pochi
        // punti sappiano raccontare la curva.
        const float PASSO = 7.0f;

        for (float r = -RP; r <= rPunta; r += (r < 0.0f ? 3.0f : PASSO))
        {
            float semi;
            if (r < 0.0f)
                semi = sqrtf(RP * RP - r * r);
            else if (r <= rSpalla)
                semi = RP;
            else
                semi = RP * (1.0f - (r - rSpalla) / hPunta);

            if (semi < 0.0f) semi = 0.0f;

            // Il centro della fila c'e' sempre; gli altri si allineano
            // da li' verso i lati a passo fisso, cosi' le colonne
            // restano dritte per tutta la lunghezza.
            dmDot(g, CRONO_CX + (int16_t)lroundf(co * r),
                     CRONO_CY + (int16_t)lroundf(si * r), grossezza, tinta);

            for (float k = PASSO; k <= semi + 1.0f; k += PASSO)
                for (int lato = -1; lato <= 1; lato += 2)
                {
                    int16_t px = CRONO_CX + (int16_t)lroundf(co * r - si * k * lato);
                    int16_t py = CRONO_CY + (int16_t)lroundf(si * r + co * k * lato);
                    dmDot(g, px, py, grossezza, tinta);
                }
        }
    }

    // Il foro al centro: sugli orologi veri e' il buco in cui si
    // infila l'asse, e senza, il perno sembra una goccia appoggiata
    // invece di un pezzo montato su qualcosa.
    dmDot(g, CRONO_CX, CRONO_CY, 13, COL_SFONDO);
}

static void disegnaCrono(Arduino_GFX *g)
{
    telaio(g, "CRONOMETRO");

    uint32_t t = cronoTempo();

    cronoTrama(g);
    cronoTacche(g);

    cronoTriangolo(g);

    // Quanti giri hai segnato, a ore sei: sui quadranti veri li' ci
    // sta il nome della marca, ed e' l'unico posto dentro il disco
    // dove qualcosa puo' stare senza dare fastidio. E' anche il
    // pulsante per aprire l'elenco - un bersaglio tondo in mezzo allo
    // schermo si prende con il dito senza guardare.
    if (cronoNGiri > 0)
        disegnaBollo(g, CRONO_CX, CRONO_CY + CRONO_BOLLO, '0' + (cronoNGiri % 10),
                     4, 3, COL_ACCESO);

    cronoLancetta(g, t);
    // ---- il tempo in cifre ----
    uint32_t minuti = t / 60000;
    uint32_t secondi = (t / 1000) % 60;
    uint32_t decimi = (t / 100) % 10;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)minuti, (unsigned long)secondi);

    // Punti piu' radi rispetto al passo: la stessa taglia ma meno
    // pieni, cosi' il quadrante resta lo strumento e il numero il
    // riscontro.
    const int16_t passo = 4;
    const int16_t largoGrande = dmTextWidth(buf, passo, 1);
    // Il decimo e' grande come il resto: e' un numero della stessa
    // riga, e farlo piu' piccolo lo faceva sembrare una nota a
    // margine invece di una cifra del tempo.
    const int16_t largoDecimo = dmTextWidth("0", passo, 1);
    const int16_t STACCO = 12;
    const int16_t x0 = (LCD_W - (largoGrande + STACCO + largoDecimo)) / 2;
    const int16_t yCifre = CRONO_CIFRE_Y;

    if (dmRullo(g, rulloCrono, buf, x0, yCifre, passo, 3, COL_ACCESO, 1, 240, 30))
        numeriInMovimento = true;

    dmDot(g, x0 + largoGrande + 4, yCifre + 7 * passo - 3, 3, COL_ACCESO);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)decimi);
    dmText(g, x0 + largoGrande + STACCO, yCifre, buf, passo, 3,
           cronoAttivo ? COL_ROSSO : COL_SECONDARIO);

    // I due comandi in fondo, come le anse di un cronografo: azzerare
    // a sinistra, segnare il giro a destra. Spenti quando non
    // servirebbero a niente.
    disegnaGriglia(g, PADDING + MEZZA_ICONA, CRONO_PULSANTI_Y, ICO_CHIUDI,
                   ICONA_COMANDO, 4, 3,
                   (!cronoAttivo && t > 0) ? COL_SECONDARIO : COL_SPENTO);

    disegnaGriglia(g, LCD_W - PADDING - MEZZA_ICONA, CRONO_PULSANTI_Y, ICO_PIU,
                   ICONA_COMANDO, 4, 3,
                   cronoAttivo ? COL_SECONDARIO : COL_SPENTO);



    // Finche' corre - o finche' torna verso lo zero - la lancetta
    // chiede il fotogramma successivo.
    if ((cronoAttivo || azzeraInCorso) && schedaCorrente == SCHEDA_CRONO)
        numeriInMovimento = true;
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

    // I secondi come un quadrante che si riempie, non come un numero:
    // si legge con la coda dell'occhio.
    //
    // Il passo e' quello di tutti gli altri quadranti - l'arco del
    // Sole, la curva del barometro - perche' e' una regola del
    // progetto. Ne viene una conseguenza: sessanta punti a questa
    // distanza sarebbero settecento pixel, il doppio dello schermo.
    // Quindi un punto ogni due secondi. Meglio trenta punti che si
    // leggono come parte della stessa famiglia che sessanta punti
    // fitti che sembrano un'altra cosa.
    const int16_t x0 = PADDING;
    const int16_t larghezza = (TACCHE_MINUTO - 1) * PASSO_QUADRANTE;

    // Il marcatore scorre sulla linea con sessanta posizioni, una per
    // secondo: cade sopra un punto o esattamente in mezzo a due.
    const int16_t xMarcatore = x0 + (int16_t)((int32_t)t.tm_sec * larghezza / 59);

    for (int i = 0; i < TACCHE_MINUTO; ++i)
    {
        int16_t px = x0 + i * PASSO_QUADRANTE;
        dmDot(g, px, 238, 5, px <= xMarcatore ? COL_SECONDARIO : COL_SPENTO);
    }

    marcatoreConAlone(g, xMarcatore, 238, COL_ROSSO);

    // La data.
    snprintf(buf, sizeof(buf), "%s %02d %s",
             GIORNI[t.tm_wday % 7], t.tm_mday, MESI[t.tm_mon % 12]);
    testoCentrato(g, 278, buf, 5, 4, COL_ACCESO);

    // Da dove viene l'ora che stai leggendo. Sta sopra, appoggiata
    // all'orario: e' una nota a margine di quel numero, non un dato
    // per conto suo.
    const char *fonte = oraSincronizzata ? "SINCRONIZZATO" : "OROLOGIO INTERNO";
    testoCentrato(g, 84, fonte, 3, 2, COL_ETICHETTA, 2);

    // La barra dei comandi: a sinistra le sveglie, a destra ne
    // aggiungi una. Le zone toccabili sono piu' grandi dei simboli -
    // un dito non e' un puntatore, e centrare nove punti sarebbe un
    // esercizio di mira.
    disegnaGriglia(g, BARRA_SX, BARRA_Y, ICO_CAMPANA, ICONA_COMANDO, 4, 3,
                   sveglieAttive() > 0 ? COL_ACCESO : COL_SPENTO);

    if (sveglieAttive() > 0)
        disegnaBollo(g, BARRA_SX + 50, BARRA_Y, '0' + sveglieAttive(), 4, 3, COL_ACCESO);

    disegnaGriglia(g, BARRA_DX, BARRA_Y, ICO_PIU, ICONA_COMANDO, 4, 3, COL_SECONDARIO);

    // Una ventola piccola: dice se in casa c'e' un condizionatore
    // acceso, senza dover andare a guardare le sue schede.
    //
    // Non gira: scatta di un passo al secondo, come il battito della
    // rete e il marcatore dei secondi. Farla girare davvero
    // vorrebbe dire ridisegnare l'orologio venti volte al secondo -
    // trentasei millesimi ogni volta - e il processore non
    // tornerebbe mai a dormire. In un oggetto dove tutto scandisce
    // il secondo, uno scatto al secondo non e' un ripiego.
    {
        int accesi = 0;
        for (int i = 0; i < N_CLIMI; ++i)
            if (climi[i].valido && climi[i].acceso) ++accesi;

        float scatto = (time(nullptr) % 4) * (float)M_PI / 6.0f;   // trenta gradi per volta
        disegnaVentola(g, BARRA_VENT, BARRA_Y, 17, scatto,
                       accesi > 0 ? COL_ACCESO : COL_SPENTO);
    }
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
        testoCentrato(g, 200, "NON CALCOLABILE", 4, 3, COL_SPENTO);
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
    // Tanti punti quanti ne servono per avere lo stesso passo degli
    // altri quadranti lungo la curva.
    const int totale = (int)((float)M_PI * raggio / PASSO_QUADRANTE) + 1;

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

    testoCentrato(g, 318, verso, 3, 2, COL_ETICHETTA, 2);

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
        testoCentrato(g, 190, "NO LINK", 8, 6, COL_SPENTO);
        const char *motivo = retePresente ? "SERVER NON RAGGIUNGIBILE" : "NESSUNA RETE WIFI";
        testoCentrato(g, 260, motivo, 3, 2, COL_ETICHETTA, 2);
        return;
    }

    float quanto = 1.0f;
    if (iconaArmata)
    {
        quanto = 0.0f;
    }
    else if (iconaAttiva)
    {
        uint32_t passato = millis() - iconaInizio;
        if (passato >= ICONA_DURATA)
            iconaAttiva = false;
        else
        {
            quanto = (float)passato / (float)ICONA_DURATA;
            numeriInMovimento = true;
        }
    }

    disegnaGrigliaComparsa(g, LCD_W / 2, 130, iconaPerCodice(domani.codice),
                           ICONA_LATO, 6, 4, COL_ACCESO, quanto);

    char buf[16];

    // La massima, grande. Il segno di grado e' il carattere ^ del
    // font: a matrice di punti un cerchietto e' un quadratino 3x3.
    snprintf(buf, sizeof(buf), "%d^", (int)lroundf(domani.tmax));
    testoCentrato(g, 218, buf, 11, 8, COL_ACCESO);

    snprintf(buf, sizeof(buf), "MIN %d^", (int)lroundf(domani.tmin));
    testoCentrato(g, 300, buf, 4, 3, COL_SECONDARIO);

    testoCentrato(g, 344, descrizionePerCodice(domani.codice), 3, 2, COL_ETICHETTA, 2);

    // La probabilita' di pioggia come barra di punti.
    const int totale = 20;
    const int16_t passo = 12;
    const int16_t x0 = (LCD_W - totale * passo) / 2 + passo / 2;
    int accesi = (domani.pioggia * totale + 50) / 100;
    for (int i = 0; i < totale; ++i)
        dmDot(g, x0 + i * passo, 386, 5, i < accesi ? COL_ROSSO : COL_SPENTO);

    snprintf(buf, sizeof(buf), "%d%% PIOGGIA", domani.pioggia);
    testoCentrato(g, 402, buf, 2, 2, COL_ETICHETTA, 2);
}

// ------------------------------------------------------------
//  SCHEDA 4: IL LOGO
// ------------------------------------------------------------
//  Il logo non e' un'immagine incollata sullo schermo: e' passato
//  per la stessa griglia di tutto il resto. Il passo dei punti si
//  calcola qui invece che fissarlo nel file generato, cosi' se un
//  giorno rigeneri il logo piu' fitto o piu' rado si adatta da solo
//  allo spazio che ha.

// ------------------------------------------------------------
//  IL LOGO CHE SI SFALDA
// ------------------------------------------------------------
//  Tocca il logo e i suoi punti smettono di essere un disegno:
//  diventano oggetti con una massa, che cadono da dove si trovano
//  e si accumulano sul fondo. Inclinando la board scivolano, e
//  siccome sono tanti e si spingono a vicenda si comportano come
//  un liquido invece che come palline separate.
//
//  Non c'e' niente di simulato in senso stretto: ogni punto ha
//  una posizione e una velocita', la gravita' cambia la velocita'
//  e la velocita' cambia la posizione. Il comportamento da liquido
//  non e' programmato da nessuna parte - esce da solo dal fatto che
//  due punti non possono stare nello stesso posto.

#if HA_LOGO

#define MAX_PARTICELLE 340
// Il raggio con cui i punti si fanno spazio a vicenda: uguale a
// quello con cui sono disegnati. Farlo piu' largo alzava il mucchio,
// ma faceva sembrare che i punti crescessero nel momento in cui li
// tocchi - e un pezzo di logo che cade deve restare lo stesso
// pezzo di logo.
#define PART_RAGGIO 4.0f
#define GRAVITA 2000.0f      // pixel al secondo quadrato, con la board in verticale
#define ATTRITO 0.992f
#define RIMBALZO 0.35f
#define RIPOSO 7.0f          // sotto questa velocita' un punto e' fermo

// Come si mappa il sensore sullo schermo. Girare un segno specchia
// quella direzione.
#define SEGNO_ORIZZONTALE (-1.0f)
#define SEGNO_VERTICALE (+1.0f)

// I bordi della vasca: dentro la cornice, non contro il vetro.
#define VASCA_SX 26
#define VASCA_DX (LCD_W - 26)
#define VASCA_ALTO 26
#define VASCA_BASSO (LCD_H - 26)

struct Particella
{
    float x, y, vx, vy;
};

// La griglia che evita i confronti inutili. Le caselle sono larghe
// quanto il raggio d'azione di un punto: piu' piccole e servirebbe
// guardare piu' lontano, piu' grandi e ogni casella ne conterrebbe
// troppi.
#define CELLA 16
#define GRIGLIA_W ((VASCA_DX - VASCA_SX) / CELLA + 2)
#define GRIGLIA_H ((VASCA_BASSO - VASCA_ALTO) / CELLA + 2)
#define PER_CELLA 12

static uint8_t cellaQuante[GRIGLIA_W * GRIGLIA_H];
static uint16_t cellaChi[GRIGLIA_W * GRIGLIA_H][PER_CELLA];

static Particella particelle[MAX_PARTICELLE];
static int nParticelle = 0;
static bool fisicaAttiva = false;
static uint32_t fisicaUltimo = 0;

// Dove stanno i punti del logo quando e' fermo. Serve sia a
// disegnarlo sia a sapere da dove far partire i pezzi.
static void logoGeometria(int16_t &passo, int16_t &diam, int16_t &x0, int16_t &y0)
{
    const int16_t spazioX = LCD_W - 80;
    const int16_t spazioY = 300;

    passo = spazioX / LOGO_LARGHEZZA;
    int16_t passoY = spazioY / LOGO_ALTEZZA;
    if (passoY < passo) passo = passoY;
    if (passo < 2) passo = 2;

    diam = (passo * 8) / 10;
    if (diam < 2) diam = 2;

    x0 = (LCD_W - LOGO_LARGHEZZA * passo) / 2 + passo / 2;
    y0 = 96 + (spazioY - LOGO_ALTEZZA * passo) / 2;
}

static void fisicaAvvia()
{
    // Si sveglia adesso, che e' l'unico momento in cui serve.
    if (!imuPronto)
        imuPronto = imuBegin();

    int16_t passo, diam, x0, y0;
    logoGeometria(passo, diam, x0, y0);

    nParticelle = 0;

    for (int r = 0; r < LOGO_ALTEZZA && nParticelle < MAX_PARTICELLE; ++r)
        for (int c = 0; c < LOGO_LARGHEZZA && nParticelle < MAX_PARTICELLE; ++c)
            if (LOGO[r][c] == '#')
            {
                particelle[nParticelle].x = x0 + c * passo;
                particelle[nParticelle].y = y0 + r * passo;
                particelle[nParticelle].vx = 0;
                particelle[nParticelle].vy = 0;
                ++nParticelle;
            }

#if LOGO_CERCHIO
    const int16_t cx = x0 + ((LOGO_LARGHEZZA - 1) * passo) / 2;
    const int16_t cy = y0 + ((LOGO_ALTEZZA - 1) * passo) / 2;
    const int16_t raggio = ((LOGO_LARGHEZZA - 1) * passo) / 2;
    int quanti = (int)(2.0f * (float)M_PI * raggio / passo);

    for (int i = 0; i < quanti && nParticelle < MAX_PARTICELLE; ++i)
    {
        float a = 2.0f * (float)M_PI * i / quanti;
        particelle[nParticelle].x = cx + cosf(a) * raggio;
        particelle[nParticelle].y = cy + sinf(a) * raggio;
        particelle[nParticelle].vx = 0;
        particelle[nParticelle].vy = 0;
        ++nParticelle;
    }
#endif

    fisicaAttiva = true;
    fisicaUltimo = millis();
    Serial.printf("[fisica] %d punti in caduta\n", nParticelle);
}

static void fisicaPasso()
{
    uint32_t adesso = millis();
    float dt = (adesso - fisicaUltimo) / 1000.0f;
    fisicaUltimo = adesso;

    // Un fotogramma lungo - per esempio dopo una pausa - farebbe
    // attraversare i muri a tutto quanto. Meglio rallentare il tempo
    // che far esplodere la scena.
    if (dt > 0.033f) dt = 0.033f;
    if (dt <= 0) return;

    float ax = 0, ay = 0, az = 0;
    imuLeggi(&ax, &ay, &az);

    // Il sensore e' montato girato di un quarto rispetto allo
    // schermo: quello che per lui e' l'asse X, per chi guarda e'
    // l'alto-basso. La livella non lo faceva vedere perche' si usa
    // appoggiata in piano, dove nessuno dei due assi domina.
    //
    // Se un giorno la direzione risultasse specchiata, sono questi
    // due segni da girare e nient'altro.
    float gx = SEGNO_ORIZZONTALE * ay * GRAVITA;
    float gy = SEGNO_VERTICALE * ax * GRAVITA;

    // Un occhio ai numeri grezzi, una volta al secondo: se la
    // direzione non torna, e' da qui che si capisce quale asse sta
    // facendo cosa.
    static uint32_t ultimoLog = 0;
    static int fotogrammi = 0;
    ++fotogrammi;
    if (adesso - ultimoLog > 1000)
    {
        Serial.printf("[fisica] %d punti, %d al secondo, ax %+.2f ay %+.2f az %+.2f\n",
                      nParticelle, fotogrammi, ax, ay, az);
        ultimoLog = adesso;
        fotogrammi = 0;
    }

    for (int i = 0; i < nParticelle; ++i)
    {
        Particella &p = particelle[i];
        p.vx = (p.vx + gx * dt) * ATTRITO;
        p.vy = (p.vy + gy * dt) * ATTRITO;
        p.x += p.vx * dt;
        p.y += p.vy * dt;

        if (p.x < VASCA_SX + PART_RAGGIO) { p.x = VASCA_SX + PART_RAGGIO; p.vx = -p.vx * RIMBALZO; }
        if (p.x > VASCA_DX - PART_RAGGIO) { p.x = VASCA_DX - PART_RAGGIO; p.vx = -p.vx * RIMBALZO; }
        if (p.y < VASCA_ALTO + PART_RAGGIO) { p.y = VASCA_ALTO + PART_RAGGIO; p.vy = -p.vy * RIMBALZO; }
        if (p.y > VASCA_BASSO - PART_RAGGIO) { p.y = VASCA_BASSO - PART_RAGGIO; p.vy = -p.vy * RIMBALZO; }

        // Sotto una certa lentezza un punto e' fermo, e insistere a
        // muoverlo di frazioni di pixel produce solo tremolio. Un
        // oggetto vero, appoggiato, sta appoggiato.
        if (fabsf(p.vx) < RIPOSO && fabsf(p.vy) < RIPOSO)
        {
            p.vx = 0;
            p.vy = 0;
        }
    }

    // Due punti non possono stare nello stesso posto: quando si
    // sovrappongono si spingono via a meta' strada l'uno dell'altro.
    // E' tutta la "fisica del liquido" che c'e' qui dentro - il resto
    // e' una conseguenza.
    //
    // Confrontare ogni punto con ogni altro sarebbero cinquantamila
    // coppie a fotogramma, e la stragrande maggioranza fra punti che
    // stanno ai due capi opposti dello schermo. Invece si divide
    // l'area in caselle grandi quanto il raggio d'azione: due punti
    // possono toccarsi solo se stanno nella stessa casella o in una
    // delle otto accanto, e tutte le altre coppie non vengono
    // nemmeno guardate.
    const float minimo = PART_RAGGIO * 2.0f;
    const float minimo2 = minimo * minimo;

    memset(cellaQuante, 0, sizeof(cellaQuante));

    for (int i = 0; i < nParticelle; ++i)
    {
        int cx = (int)((particelle[i].x - VASCA_SX) / CELLA);
        int cy = (int)((particelle[i].y - VASCA_ALTO) / CELLA);
        if (cx < 0) cx = 0; else if (cx >= GRIGLIA_W) cx = GRIGLIA_W - 1;
        if (cy < 0) cy = 0; else if (cy >= GRIGLIA_H) cy = GRIGLIA_H - 1;

        int k = cy * GRIGLIA_W + cx;
        if (cellaQuante[k] < PER_CELLA)
            cellaChi[k][cellaQuante[k]++] = i;
    }

    for (int cy = 0; cy < GRIGLIA_H; ++cy)
        for (int cx = 0; cx < GRIGLIA_W; ++cx)
        {
            int k = cy * GRIGLIA_W + cx;
            for (int a = 0; a < cellaQuante[k]; ++a)
            {
                int i = cellaChi[k][a];

                for (int dy = 0; dy <= 1; ++dy)
                    for (int dx = (dy == 0 ? 0 : -1); dx <= 1; ++dx)
                    {
                        int nx2 = cx + dx, ny2 = cy + dy;
                        if (nx2 < 0 || nx2 >= GRIGLIA_W || ny2 >= GRIGLIA_H) continue;

                        int k2 = ny2 * GRIGLIA_W + nx2;
                        for (int b = (k2 == k ? a + 1 : 0); b < cellaQuante[k2]; ++b)
                        {
                            int j = cellaChi[k2][b];

                            float ddx = particelle[j].x - particelle[i].x;
                            float ddy = particelle[j].y - particelle[i].y;
                            float d2 = ddx * ddx + ddy * ddy;
                            if (d2 >= minimo2 || d2 < 0.0001f) continue;

                            float d = sqrtf(d2);
                            float spinta = (minimo - d) * 0.5f;
                            float nnx = ddx / d, nny = ddy / d;

                            particelle[i].x -= nnx * spinta;
                            particelle[i].y -= nny * spinta;
                            particelle[j].x += nnx * spinta;
                            particelle[j].y += nny * spinta;

                            // Separare le posizioni non basta: se la
                            // velocita' resta quella di prima, al
                            // fotogramma dopo i due si rientrano
                            // dentro e vengono rispinti fuori, e cosi'
                            // via - ed e' quel rimbalzo continuo che
                            // si vede come un friggere. Qui si toglie
                            // la parte di velocita' con cui si stanno
                            // andando addosso, lasciando intatta
                            // quella con cui scivolano di fianco.
                            float vrel = (particelle[j].vx - particelle[i].vx) * nnx +
                                         (particelle[j].vy - particelle[i].vy) * nny;
                            if (vrel < 0)
                            {
                                float togli = vrel * 0.5f;
                                particelle[i].vx += nnx * togli;
                                particelle[i].vy += nny * togli;
                                particelle[j].vx -= nnx * togli;
                                particelle[j].vy -= nny * togli;
                            }
                        }
                    }
            }
        }
}

#endif // HA_LOGO

#if HA_LOGO
static void disegnaLogo(Arduino_GFX *g)
{
    telaio(g, LOGO_ETICHETTA);

    if (fisicaAttiva)
    {
        // Stesso diametro dei punti del logo fermo, preso dalla
        // stessa funzione che lo disegna. Disegnarli anche solo due
        // pixel piu' grossi faceva sembrare che si accendessero nel
        // momento del tocco.
        int16_t passoL, diamL, xL, yL;
        logoGeometria(passoL, diamL, xL, yL);

        for (int i = 0; i < nParticelle; ++i)
            dmDot(g, (int16_t)particelle[i].x, (int16_t)particelle[i].y, diamL, LOGO_COLORE);

        testoCentrato(g, LCD_H - 62, "INCLINA LA BOARD", 2, 2, COL_ETICHETTA, 2);
        if (schedaCorrente == SCHEDA_LOGO)
            numeriInMovimento = true;
        return;
    }

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
        testoCentrato(g, 402, LOGO_SOTTOTITOLO, 3, 2, COL_ETICHETTA, 2);
    else
        testoCentrato(g, 402, "TOCCA", 2, 2, COL_ETICHETTA, 2);
}
#endif

// ------------------------------------------------------------
//  SCHEDA 4: IL BAROMETRO
// ------------------------------------------------------------

#define BARO_X PADDING
#define BARO_LARGO (LCD_W - 2 * PADDING)
#define BARO_ALTO 104
#define BARO_BASSO 214
// Un punto per ogni ora, con lo stesso passo e la stessa grandezza
// dei punti dell'arco del Sole. E' una regola del progetto: i
// quadranti parlano tutti la stessa lingua, altrimenti ogni scheda
// sembra disegnata da una persona diversa.
#define BARO_PUNTI TACCHE_MINUTO

static void disegnaBaro(Arduino_GFX *g)
{
    telaio(g, "BAROMETRO");

    if (!baro.valido)
    {
        testoCentrato(g, 190, "NO LINK", 8, 6, COL_SPENTO);
        const char *motivo = retePresente ? "SERVER NON RAGGIUNGIBILE" : "NESSUNA RETE WIFI";
        testoCentrato(g, 260, motivo, 3, 2, COL_ETICHETTA, 2);
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

    const int16_t passo = PASSO_QUADRANTE;
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
            marcatoreConAlone(g, px, py, COL_ROSSO);
        else
            dmDot(g, px, py, 5, COL_SECONDARIO);
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

    testoCentrato(g, 314, tendenza, 4, 3,
                   delta < -1.0f ? COL_ROSSO : COL_ACCESO);
    testoCentrato(g, 348, significa, 2, 2, COL_ETICHETTA, 2);

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
//  SCHEDA 5: LA QUALITA' DELL'ARIA
// ------------------------------------------------------------
//  Un contenitore con dentro delle bollicine: quante sono dipende
//  da quanto e' sporca l'aria. Un numero da solo non dice se 23 sia
//  tanto o poco, mentre un bicchiere quasi vuoto o pieno di
//  particelle si capisce senza sapere cosa sia un microgrammo.

// Due colonne di larghezza uguale con un corridoio in mezzo: il
// bicchiere a sinistra, i numeri a destra allineati fra loro.
#define ARIA_CORRIDOIO 24
#define ARIA_COLONNA ((LCD_W - 2 * PADDING - ARIA_CORRIDOIO) / 2)
#define ARIA_SX PADDING
#define ARIA_DX (PADDING + ARIA_COLONNA)
#define ARIA_TESTO (ARIA_DX + ARIA_CORRIDOIO)
#define ARIA_ALTO 96
#define ARIA_BASSO 382

static const char *qualitaAria(int i)
{
    if (i <= 20) return "BUONA";
    if (i <= 40) return "DISCRETA";
    if (i <= 60) return "MEDIA";
    if (i <= 80) return "SCARSA";
    if (i <= 100) return "CATTIVA";
    return "PESSIMA";
}

static void disegnaAria(Arduino_GFX *g)
{
    telaio(g, "ARIA");

    if (!aria.valido)
    {
        testoCentrato(g, 190, "NO LINK", 8, 6, COL_SPENTO);
        const char *motivo = retePresente ? "SERVER NON RAGGIUNGIBILE" : "NESSUNA RETE WIFI";
        testoCentrato(g, 260, motivo, 3, 2, COL_ETICHETTA, 2);
        return;
    }

    // Il contenitore: aperto in alto, come un bicchiere.
    const int16_t passo = 8;
    for (int16_t y = ARIA_ALTO; y <= ARIA_BASSO; y += passo)
    {
        dmDot(g, ARIA_SX, y, 4, COL_ACCESO);
        dmDot(g, ARIA_DX, y, 4, COL_ACCESO);
    }
    for (int16_t x = ARIA_SX; x <= ARIA_DX; x += passo)
        dmDot(g, x, ARIA_BASSO, 4, COL_ACCESO);

    // Quante bollicine. Aria pulita: quattro che salgono piano. Aria
    // pessima: il contenitore ne e' pieno.
    int quante = 4 + (aria.indice * 26) / 100;
    if (quante > 30) quante = 30;

    float quanto = 1.0f;
    if (bolleArmate)
    {
        quanto = 0.0f;
    }
    else if (bolleAttive)
    {
        uint32_t passato = millis() - bolleInizio;
        if (passato >= BOLLE_DURATA)
            bolleAttive = false;
        else
            quanto = 1.0f - powf(1.0f - (float)passato / (float)BOLLE_DURATA, 3.0f);
    }

    // Finche' sei su questa scheda le bolle si muovono: e' l'unica
    // del gruppo che non sta mai ferma.
    if (schedaCorrente == SCHEDA_ARIA)
        numeriInMovimento = true;

    int fino = (quanto > 0.0f) ? quante : 0;

    // La corsa comincia sotto il fondo e finisce sopra il bordo: le
    // bolle non nascono ne' muoiono a mezz'aria, entrano ed escono
    // dal contenitore.
    const float corsa = (ARIA_BASSO + 24) - (ARIA_ALTO - 24);

    // Il tempo si conta dall'ingresso nella scheda, non
    // dall'accensione: cosi' rientrando l'animazione riparte davvero
    // da capo invece di riprendere dove sarebbe stata.
    float t = (millis() - bolleInizio) / 1000.0f;

    for (int i = 0; i < fino; ++i)
    {
        // I parametri di ogni bolla nascono da un rimescolamento dei
        // bit del suo numero d'ordine. Moltiplicare l'indice e
        // prendere il resto - che facevo prima - lascia una
        // progressione, e le bolle risultavano allineate.
        uint32_t seme = mescola(i * 2654435761U + 12345U);

        // La velocita' varia poco fra una bolla e l'altra. Con
        // intervalli ampi si desincronizzano in fretta e finiscono
        // per ammucchiarsi tutte nello stesso tratto: piu' casuale
        // sulla carta, peggiore a vedersi.
        float velocita = 24.0f + (float)(seme % 22);
        float ondaAmp = 4.0f + (float)((seme >> 18) % 8);
        float ondaVel = 1.2f + (float)((seme >> 24) % 20) / 10.0f;

        // Le bolle non partono da altezze diverse: partono tutte dal
        // fondo, ma in momenti diversi. Sfasare la posizione le
        // teneva staccate, pero' entrando nella scheda ne trovavi
        // gia' meta' a mezz'aria - e una bolla che era gia' li'
        // prima che tu arrivassi non e' una bolla.
        //
        // Lo sfasamento e' su un paio di secondi: piu' lungo e
        // l'ultima entrerebbe dopo dieci, piu' corto e uscirebbero
        // tutte in blocco. A distanziarle da li' in poi ci pensano
        // le velocita' diverse.
        float scarto = (float)((seme >> 8) % 100) / 100.0f;
        float ritardo = ((float)i + 0.6f * scarto) / (float)quante;
        float partenza = ritardo * 2.2f;

        if (t < partenza) continue;

        float percorso = (t - partenza) * velocita;
        float y = (ARIA_BASSO + 24) - fmodf(percorso, corsa);

        // A ogni risalita riparte da una colonna diversa: con la
        // colonna fissa per sempre ripassava sempre nello stesso
        // punto, e la scia si leggeva come una riga verticale.
        uint32_t giro = (uint32_t)(percorso / corsa);
        uint32_t semeGiro = mescola(seme + giro * 7919U);

        // Le bolle vere non salgono dritte: la scia che si lasciano
        // dietro le fa oscillare da un lato all'altro.
        float onda = sinf(t * ondaVel + ritardo * 6.28f) * ondaAmp;

        const int16_t utile = ARIA_DX - ARIA_SX - 40;
        int16_t x = ARIA_SX + 20 + (int16_t)(semeGiro % utile) + (int16_t)onda;

        // Un disco di nero pieno dietro ogni bolla, largo abbastanza
        // da coprirla tutta e un po' oltre. Disegnandolo bolla per
        // bolla, quella che passa dopo ritaglia un morso in quella
        // gia' disegnata - ed e' il morso a dire quale delle due sta
        // davanti, invece di lasciarle fondere in una macchia sola.
        // Stessa cosa dei marcatori degli altri quadranti, tagliata
        // ai due bordi del contenitore.
        marcatoreConAlone(g, x, (int16_t)y, COL_BOLLA,
                          ARIA_ALTO + 2, ARIA_BASSO - 2);
    }

    char buf[16];

    // La colonna di destra: tutto incolonnato sullo stesso bordo, che
    // e' quello che rende leggibile un elenco di dati diversi.
    snprintf(buf, sizeof(buf), "%d", aria.indice);
    if (dmRullo(g, rulloAria, buf, ARIA_TESTO, 100, 10, 7, COL_ACCESO))
        numeriInMovimento = true;

    testoInColonna(g, ARIA_TESTO, 186, "INDICE", 2, 2, COL_ETICHETTA, 2, ARIA_COLONNA);
    testoInColonna(g, ARIA_TESTO, 204, "EUROPEO", 2, 2, COL_ETICHETTA, 2, ARIA_COLONNA);

    testoInColonna(g, ARIA_TESTO, 244, qualitaAria(aria.indice), 4, 3,
                   aria.indice > 60 ? COL_ROSSO : COL_ACCESO, 1, ARIA_COLONNA);

    // Le due polveri sottili. Il numero grande le riassume insieme ad
    // altri tre inquinanti, ma sono queste che trovi sui bollettini.
    dmText(g, ARIA_TESTO, 300, "PM2.5", 2, 2, COL_ETICHETTA, 2);
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(aria.pm25));
    dmText(g, ARIA_TESTO, 318, buf, 3, 3, COL_SECONDARIO);

    dmText(g, ARIA_TESTO, 348, "PM10", 2, 2, COL_ETICHETTA, 2);
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(aria.pm10));
    dmText(g, ARIA_TESTO, 366, buf, 3, 3, COL_SECONDARIO);
}

// ------------------------------------------------------------
//  SCHEDA 6: IL CONDIZIONATORE
// ------------------------------------------------------------
//  La prima scheda che non guarda soltanto: da qui si comanda.
//
//  La ventola gira davvero quando la macchina e' accesa, e sta
//  ferma e grigia quando e' spenta. Non e' decorazione: e' il modo
//  piu' immediato di sapere se sta funzionando senza leggere una
//  parola.

#define CLIMA_CX (LCD_W / 2)
#define CLIMA_CY 150
#define CLIMA_RAGGIO 64

static void disegnaClima(Arduino_GFX *g, Clima &clima, int quale)
{
    telaio(g, clima.nome);

    if (!clima.valido)
    {
        testoCentrato(g, 190, "NO LINK", 8, 6, COL_SPENTO);
        const char *motivo = retePresente ? "CONDIZIONATORE NON TROVATO" : "NESSUNA RETE WIFI";
        testoCentrato(g, 260, motivo, 3, 2, COL_ETICHETTA, 2);
        return;
    }

    // Gira solo se e' acceso, e piano: una ventola che sfreccia
    // sembrerebbe un caricamento in corso.
    // L'angolo si accumula invece di essere ricavato dall'orologio:
    // calcolandolo dal tempo assoluto, cambiare la durata del giro
    // faceva saltare la pala in un'altra posizione. E la velocita'
    // insegue quella richiesta invece di adottarla di colpo - una
    // ventola ha una massa, prende giri e li perde.
    avanzaVentola(clima);
    float angolo = clima.angolo;

    // Finche' gira - anche mentre sta rallentando fino a fermarsi -
    // la scheda va ridisegnata.
    if ((clima.acceso || clima.velocita > 0.02f) &&
        (schedaCorrente == SCHEDA_CLIMA || schedaCorrente == SCHEDA_CLIMA2))
        numeriInMovimento = true;

    disegnaVentola(g, CLIMA_CX, CLIMA_CY, CLIMA_RAGGIO, angolo,
                   clima.acceso ? COL_ACCESO : COL_SPENTO);

    // I comandi della ventola, ai suoi lati.
    disegnaGriglia(g, 52, CLIMA_CY, ICO_MENO, ICONA_COMANDO, 5, 4,
                   clima.acceso ? COL_SECONDARIO : COL_SPENTO);
    disegnaGriglia(g, LCD_W - 52, CLIMA_CY, ICO_PIU, ICONA_COMANDO, 5, 4,
                   clima.acceso ? COL_SECONDARIO : COL_SPENTO);

    testoCentrato(g, 222, climaNomeVelocita(clima.ventola), 2, 2,
                  clima.acceso ? COL_ETICHETTA : COL_SPENTO, 2);

    // La temperatura che hai chiesto.
    char buf[16];
    snprintf(buf, sizeof(buf), "%d^", (int)lroundf(clima.impostata));
    int16_t largo = dmTextWidth(buf, 9, 1);
    if (dmRullo(g, rulloClima[quale], buf, (LCD_W - largo) / 2, 248, 9, 7,
                clima.acceso ? COL_ACCESO : COL_SPENTO))
        numeriInMovimento = true;

    // La modalita' e' anche un comando: toccandola si passa alla
    // successiva. Il rosso la faceva gia' sembrare la cosa viva della
    // scheda, tanto vale che lo sia. E cambia scorrendo, come i
    // numeri dell'orologio.
    //
    // Il nome viene messo dentro un campo di larghezza fissa, con gli
    // spazi ai lati: le parole sono lunghe diverse, e centrando ogni
    // volta il testo salterebbe di lato mentre scorre. Cosi' invece la
    // riga resta ferma e si muovono solo le lettere.
    {
        const char *nome = clima.acceso ? climaModo(clima.modo) : "SPENTO";
        char campo[20];
        int len = strlen(nome);
        int vuoto = 12 - len;
        if (vuoto < 0) vuoto = 0;
        int sinistra = vuoto / 2;
        snprintf(campo, sizeof(campo), "%*s%s%*s", sinistra, "", nome,
                 vuoto - sinistra, "");

        int16_t largoModo = dmTextWidth(campo, 3, 2);
        // Tutte le lettere insieme e in fretta. Lo sfasamento serve a
        // un contatore, dove ogni cifra e' una ruota per conto suo;
        // una parola invece e' una cosa sola, e vederla arrivare a
        // pezzi la fa sembrare lenta.
        if (dmRullo(g, rulloModo[quale], campo, (LCD_W - largoModo) / 2, 322, 3, 3,
                    clima.acceso ? COL_ROSSO : COL_SPENTO, 2, 280, 0))
            numeriInMovimento = true;
    }

    // Quella che c'e' davvero, dentro e fuori.
    dmText(g, PADDING, 346, "IN CASA", 2, 2, COL_ETICHETTA, 2);
    snprintf(buf, sizeof(buf), "%d^", (int)lroundf(clima.interna));
    dmText(g, PADDING, 372, buf, 3, 3, COL_SECONDARIO);

    int16_t w = dmTextWidth("FUORI", 2, 2);
    dmText(g, LCD_W - PADDING - w, 346, "FUORI", 2, 2, COL_ETICHETTA, 2);
    snprintf(buf, sizeof(buf), "%d^", (int)lroundf(clima.esterna));
    w = dmTextWidth(buf, 3, 1);
    dmText(g, LCD_W - PADDING - w, 372, buf, 3, 3, COL_SECONDARIO);

    // I comandi, ai lati della temperatura.
    disegnaGriglia(g, 52, 274, ICO_MENO, ICONA_COMANDO, 5, 4, COL_SECONDARIO);
    disegnaGriglia(g, LCD_W - 52, 274, ICO_PIU, ICONA_COMANDO, 5, 4, COL_SECONDARIO);
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
    case SCHEDA_CRONO: disegnaCrono(scheda[i]);   break;
    case SCHEDA_SOLE:  disegnaSole(scheda[i], t); break;
    case SCHEDA_METEO: disegnaMeteo(scheda[i]);   break;
    case SCHEDA_BARO:  disegnaBaro(scheda[i]);   break;
    case SCHEDA_ARIA:  disegnaAria(scheda[i]);   break;
    case SCHEDA_CLIMA:  disegnaClima(scheda[i], climi[0], 0); break;
    case SCHEDA_CLIMA2: disegnaClima(scheda[i], climi[1], 1); break;
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
#define LISTA_BASSO (LCD_H - PADDING - 56)
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

// L'elenco dei giri. Uno solo non direbbe niente: il senso di un giro
// e' il confronto con quelli prima, e per confrontare bisogna vederli
// insieme.
static void disegnaGiri(Arduino_GFX *g)
{
    // La riga in cima e' sempre la stessa in tutto l'oggetto: a
    // sinistra il nome di dove sei, a destra la carica - o la croce
    // per uscire, quando ci si trova dentro una sezione. E' l'unico
    // punto fermo mentre tutto il resto cambia, e non si tocca.
    telaio(g, "GIRI");

    disegnaGriglia(g, LCD_W - PADDING - MEZZA_ICONA, PADDING + 8, ICO_CHIUDI,
                   ICONA_COMANDO, 4, 3, COL_SECONDARIO);

    // Il tempo corrente comincia sotto. Il bollo dei giri sta in mezzo
    // al quadrante e si preme anche senza volerlo: entrando per
    // sbaglio, almeno si vede l'ora del cronografo invece di un
    // elenco che non si cercava.
    {
        uint32_t t = cronoTempo();
        char buf[16];
        snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(t / 60000),
                 (unsigned long)((t / 1000) % 60));

        const int16_t passo = 6;
        const int16_t largo = dmTextWidth(buf, passo, 1);
        const int16_t x0 = (LCD_W - (largo + 14 + dmTextWidth("0", 4, 1))) / 2;
        const int16_t y0 = 84;

        dmText(g, x0, y0, buf, passo, 5,
               cronoAttivo ? COL_ROSSO : COL_ACCESO);
        dmDot(g, x0 + largo + 5, y0 + 7 * passo - 4, 4, COL_SECONDARIO);

        snprintf(buf, sizeof(buf), "%lu", (unsigned long)((t / 100) % 10));
        dmText(g, x0 + largo + 14, y0 + 7 * passo - 28, buf, 4, 3, COL_SECONDARIO);
    }

    if (cronoNGiri == 0)
    {
        testoCentrato(g, 230, "NESSUN GIRO", 4, 3, COL_SPENTO);
        return;
    }

    // Il migliore si segna: e' la cosa che si cerca guardando un
    // elenco di tempi.
    uint32_t migliore = cronoGiri[0];
    for (int i = 1; i < cronoNGiri; ++i)
        if (cronoGiri[i] < migliore) migliore = cronoGiri[i];

    const int16_t ALTO = 156;
    const int16_t RIGA = 46;
    char buf[20];

    for (int i = 0; i < cronoNGiri; ++i)
    {
        int16_t y = ALTO + i * RIGA - (int16_t)lroundf(listaScorrimento);
        if (y < ALTO - 30 || y > LCD_H - PADDING - 20) continue;

        bool top = (cronoGiri[i] == migliore) && (cronoNGiri > 1);

        snprintf(buf, sizeof(buf), "%d", i + 1);
        dmText(g, PADDING, y + 4, buf, 3, 2, COL_ETICHETTA, 2);

        snprintf(buf, sizeof(buf), "%02lu:%02lu.%lu",
                 (unsigned long)(cronoGiri[i] / 60000),
                 (unsigned long)((cronoGiri[i] / 1000) % 60),
                 (unsigned long)((cronoGiri[i] / 100) % 10));
        int16_t w = dmTextWidth(buf, 4, 1);
        dmText(g, LCD_W - PADDING - w, y, buf, 4, 3,
               top ? COL_ROSSO : COL_ACCESO);
    }
}

static void disegnaLista(Arduino_GFX *g)
{
    telaio(g, "SVEGLIE");

    disegnaGriglia(g, LCD_W - PADDING - MEZZA_ICONA, PADDING + 8, ICO_CHIUDI,
                   ICONA_COMANDO, 4, 3, COL_SECONDARIO);

    if (nSveglie == 0)
    {
        testoCentrato(g, 200, "NESSUNA SVEGLIA", 4, 3, COL_SPENTO);
        testoCentrato(g, 240, "TOCCA IL PIU PER AGGIUNGERE", 2, 2,
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
        disegnaGriglia(g, LCD_W / 2, LCD_H - PADDING - 24, ICO_PIU, ICONA_COMANDO, 5, 4, COL_SECONDARIO);
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

    disegnaGriglia(g, LCD_W - PADDING - MEZZA_ICONA, PADDING + 8, ICO_CHIUDI,
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

    testoCentrato(g, 236, editorCampo == 0 ? "ORE" : "MINUTI",
                   2, 2, COL_ETICHETTA, 2);

    // Meno e piu'.
    disegnaGriglia(g, 90, 290, ICO_MENO, ICONA_COMANDO, 6, 5, COL_ACCESO);
    disegnaGriglia(g, LCD_W - 90, 290, ICO_PIU, ICONA_COMANDO, 6, 5, COL_ACCESO);

    // Salva, e se stai modificando anche elimina.
    disegnaGriglia(g, LCD_W / 2, LCD_H - PADDING - 28, ICO_SPUNTA, ICONA_COMANDO, 6, 5, COL_ROSSO);
    if (editorIndice >= 0)
        disegnaGriglia(g, PADDING + MEZZA_ICONA, LCD_H - PADDING - 28, ICO_CESTINO,
                       ICONA_COMANDO, 4, 3, COL_SPENTO);
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
    testoCentrato(g, 200, buf, 10, 7, acceso ? COL_ACCESO : COL_SPENTO);

    testoCentrato(g, 300, "SVEGLIA", 4, 3, COL_ROSSO);
    testoCentrato(g, 370, "TOCCA PER SPEGNERE", 2, 2, COL_ETICHETTA, 2);
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
    else if (i == SCHEDA_METEO)
    {
        iconaAttiva = false;
        iconaArmata = true;
        daRidisegnare[SCHEDA_METEO] = true;
    }
    else if (i == SCHEDA_ARIA)
    {
        bolleAttive = false;
        bolleArmate = true;
        daRidisegnare[SCHEDA_ARIA] = true;
    }
#if HA_LOGO
    else if (i == SCHEDA_LOGO && fisicaAttiva)
    {
        // Il logo si ricompone da solo appena esci di scena. Ritrovare
        // il mucchio di punti dove l'avevi lasciato sarebbe stato
        // divertente una volta e fastidioso tutte le altre: quella
        // scheda deve essere prima di tutto il logo.
        fisicaAttiva = false;
        daRidisegnare[SCHEDA_LOGO] = true;
    }
#endif
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
    else if (schedaCorrente == SCHEDA_METEO && iconaArmata)
    {
        iconaArmata = false;
        iconaAttiva = true;
        iconaInizio = millis();
        ridisegna(SCHEDA_METEO);
    }
    else if (schedaCorrente == SCHEDA_ARIA && bolleArmate)
    {
        bolleArmate = false;
        bolleAttive = true;
        bolleInizio = millis();
        ridisegna(SCHEDA_ARIA);
    }
    else if (schedaCorrente == SCHEDA_CRONO)
    {
        // Come le schede del condizionatore: niente animazione
        // d'ingresso, ma una lancetta che gira sempre. Se nessuno
        // chiede il primo disegno, lei resta ferma per sempre -
        // perche' e' disegnandosi che chiede il fotogramma dopo.
        daRidisegnare[SCHEDA_CRONO] = true;
    }
    else if (schedaCorrente == SCHEDA_CLIMA || schedaCorrente == SCHEDA_CLIMA2)
    {
        // Le schede del condizionatore non hanno un'animazione
        // d'ingresso: hanno una ventola che gira sempre. Ma il
        // disegno riparte solo se qualcuno dice che c'e' qualcosa da
        // muovere, e arrivando qui non lo diceva nessuno - la pala
        // restava ferma finche' non arrivava una lettura dalla rete,
        // fino a venti secondi dopo. Basta chiedere un disegno: da
        // quello in poi e' la ventola stessa a chiedere il seguente.
        daRidisegnare[schedaCorrente] = true;
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

// Durante lo scorrimento le schede non vengono ridisegnate: si
// affiancano quelle gia' pronte. Ma quelle che si muovono da sole -
// la ventola, l'orologio - si fermerebbero proprio mentre le stai
// guardando passare.
//
// Le due hanno costi diversi e si trattano in modo diverso. La pala
// gira di continuo, quindi si ripulisce e si rifa' solo il suo
// riquadro: 140 pixel per 140 contro 368 per 448. L'orologio invece
// cambia una volta al secondo, quindi tanto vale rifarlo tutto -
// capita al massimo una volta per scorrimento.
// Il cronografo mentre scorri: si ripulisce solo il disco interno -
// le tacche non si muovono - e si rifanno triangolo, lancetta e
// perno. Un terzo dell'area invece di tutta la scheda.
static void rinfrescaCrono()
{
    if ((!cronoAttivo && !azzeraInCorso) || !schedaVisibile(SCHEDA_CRONO)) return;

    Arduino_GFX *g = scheda[SCHEDA_CRONO];

    // Un cerchio, non un quadrato: gli angoli di un quadrato inscritto
    // sporgono di meta' diagonale oltre i suoi lati, arrivavano fin
    // dove stanno le tacche e se le mangiavano quattro per volta.
    g->fillCircle(CRONO_CX, CRONO_CY, CRONO_RAGGIO - 28, COL_SFONDO);
    cronoTrama(g);
    cronoTacche(g);
    cronoTriangolo(g);

    if (cronoNGiri > 0)
        disegnaBollo(g, CRONO_CX, CRONO_CY + CRONO_BOLLO, '0' + (cronoNGiri % 10),
                     4, 3, COL_ACCESO);

    cronoLancetta(g, cronoTempo());
}

static void rinfrescaVisibili()
{
    rinfrescaCrono();

    if (schedaVisibile(SCHEDA_ORA))
    {
        struct tm t;
        oraCorrente(t);

        static int ultimoScatto = -1;
        if (t.tm_sec != ultimoScatto)
        {
            ultimoScatto = t.tm_sec;
            ridisegna(SCHEDA_ORA);
        }
    }

#if N_CLIMI > 0
    for (int i = 0; i < N_CLIMI; ++i)
    {
        int sc = SCHEDA_CLIMA + i;
        if (sc >= N_SCHEDE) break;
        if (!climi[i].valido) continue;
        if (!climi[i].acceso && climi[i].velocita <= 0.02f) continue;
        if (!schedaVisibile(sc)) continue;

        avanzaVentola(climi[i]);

        const int16_t lato = 140;
        scheda[sc]->fillRect(CLIMA_CX - lato / 2, CLIMA_CY - lato / 2,
                             lato, lato, COL_SFONDO);
        disegnaVentola(scheda[sc], CLIMA_CX, CLIMA_CY, CLIMA_RAGGIO,
                       climi[i].angolo,
                       climi[i].acceso ? COL_ACCESO : COL_SPENTO);
    }
#endif
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
    indicatoreRete(comp, BATT_CX, BATT_CY);

    float opacita = palliniOpacita();
    disegnaPallini(comp, schedaCorrente + scorrimento / (float)LCD_W, opacita);
    palliniOpacitaDisegnata = opacita;
    comp->flush();
    ++webVersione;


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
    rinfrescaVisibili();
    componi();
}

// ------------------------------------------------------------
//  DOVE HAI TOCCATO
// ------------------------------------------------------------

static bool dentro(int16_t x, int16_t y, int16_t cx, int16_t cy, int16_t raggio)
{
    return (abs(x - cx) <= raggio) && (abs(y - cy) <= raggio);
}

// Bersaglio rettangolare, per le scritte: sono larghe e basse, e un
// quadrato o non le copre o invade quello che sta sopra e sotto.
static bool dentroRett(int16_t x, int16_t y, int16_t cx, int16_t cy,
                       int16_t mezzaLarghezza, int16_t mezzaAltezza)
{
    return (abs(x - cx) <= mezzaLarghezza) && (abs(y - cy) <= mezzaAltezza);
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

// Salta alla scheda del primo condizionatore acceso. Se sono tutti
// fermi va comunque al primo: chi tocca la ventola vuole vedere i
// condizionatori, non ricevere un rifiuto.
static void vaiAlCondizionatore()
{
    int bersaglio = SCHEDA_CLIMA;
    for (int i = 0; i < N_CLIMI; ++i)
        if (climi[i].valido && climi[i].acceso)
        {
            bersaglio = SCHEDA_CLIMA + i;
            break;
        }

    if (bersaglio >= N_SCHEDE) return;

    schedaCorrente = bersaglio;
    scorrimento = 0;
    palliniFino = millis() + PALLINI_ATTESA;

    // Le schede lasciate indietro riarmano le loro animazioni, e
    // quella su cui si atterra fa partire la sua: e' lo stesso
    // passaggio di uno scorrimento, solo senza il viaggio in mezzo.
    aggiornaVisibilita();
    avviaAnimazioniIngresso();

    if (daRidisegnare[schedaCorrente])
        ridisegna(schedaCorrente);
    componi();
}

// Il tocco secco sulla barra in fondo alla scheda dell'ora.
static void tapNelleSchede()
{
#if HA_LOGO
    if (schedaCorrente == SCHEDA_LOGO && !fisicaAttiva)
    {
        fisicaAvvia();
        daRidisegnare[SCHEDA_LOGO] = true;
        return;
    }
#endif

    if ((schedaCorrente == SCHEDA_CLIMA || schedaCorrente == SCHEDA_CLIMA2) &&
        CLIMA_DI(schedaCorrente).valido)
    {
        // Ogni scheda comanda la sua macchina. Meno e piu' ai lati per
        // ventola e gradi, la scritta rossa per la modalita', e tutto
        // il resto accende o spegne: il bersaglio piu' grande per il
        // gesto piu' comune.
        Clima &clima = CLIMA_DI(schedaCorrente);
        int v = climaIndiceVelocita(clima);

        // I bersagli non sono i simboli: sono le fasce che li
        // contengono. La scheda e' divisa in due colonne laterali e
        // una centrale, e in tre fasce in altezza - ventola, gradi,
        // modalita'. Ogni punto dello schermo appartiene a qualcosa,
        // senza zone morte fra un comando e l'altro: un dito copre
        // mezzo centimetro di vetro e non vede cosa sta coprendo.
        //
        // La grafica resta identica: quello che cambia e' solo dove
        // lo schermo ascolta.
        const int16_t COLONNA = 58;   // meta' larghezza delle fasce laterali
        const int16_t SINISTRA = 52;
        const int16_t DESTRA = LCD_W - 52;

        if (dentroRett(tapX, tapY, SINISTRA, 152, COLONNA, 60))
        {
            if (v > 0) climaComanda(clima, clima.acceso, clima.impostata, CLIMA_VELOCITA[v - 1], clima.modo);
        }
        else if (dentroRett(tapX, tapY, DESTRA, 152, COLONNA, 60))
        {
            if (v < CLIMA_N_VELOCITA - 1) climaComanda(clima, clima.acceso, clima.impostata, CLIMA_VELOCITA[v + 1], clima.modo);
        }
        else if (dentroRett(tapX, tapY, SINISTRA, 258, COLONNA, 42))
            climaComanda(clima, clima.acceso, clima.impostata - 1, clima.ventola, clima.modo);
        else if (dentroRett(tapX, tapY, DESTRA, 258, COLONNA, 42))
            climaComanda(clima, clima.acceso, clima.impostata + 1, clima.ventola, clima.modo);
        else if (clima.acceso && dentroRett(tapX, tapY, CLIMA_CX, 334, 90, 30))
            climaComanda(clima, true, clima.impostata, clima.ventola, climaProssimoModo(clima));
        else if (dentro(tapX, tapY, CLIMA_CX, CLIMA_CY, CLIMA_RAGGIO))
            climaComanda(clima, !clima.acceso, clima.impostata, clima.ventola, clima.modo);
        else
            return;

        daRidisegnare[schedaCorrente] = true;
        return;
    }

    if (schedaCorrente == SCHEDA_CRONO)
    {
        // I due angoli in fondo hanno i loro comandi; tutto il resto
        // dello schermo avvia e ferma. Di corsa si guarda il tempo,
        // non il dito, e il bersaglio piu' grande va al gesto piu'
        // comune.
        if (!cronoAttivo && cronoTempo() > 0 &&
            dentroRett(tapX, tapY, PADDING + MEZZA_ICONA, CRONO_PULSANTI_Y, 56, 34))
            cronoAzzera();
        else if (cronoAttivo &&
                 dentroRett(tapX, tapY, LCD_W - PADDING - MEZZA_ICONA,
                            CRONO_PULSANTI_Y, 56, 34))
            cronoGiro();
        else if (cronoNGiri > 0 &&
                 dentro(tapX, tapY, CRONO_CX, CRONO_CY + CRONO_BOLLO, 40))
        {
            vista = VISTA_GIRI;
            listaScorrimento = 0;
            vistaDaRidisegnare = true;
        }
        else
            cronoAvviaFerma();

        daRidisegnare[SCHEDA_CRONO] = true;
        return;
    }

    if (schedaCorrente != SCHEDA_ORA) return;

    // Tre zone affiancate, ciascuna larga quanto le serve senza
    // invadere la vicina. Le sveglie si prendono anche il contatore
    // accanto alla campanella, che di suo non sarebbe toccabile.
    if (dentroRett(tapX, tapY, 82, BARRA_Y, 72, BERSAGLIO))
        apriLista();
    else if (dentroRett(tapX, tapY, BARRA_VENT, BARRA_Y, 44, BERSAGLIO))
        vaiAlCondizionatore();
    else if (dentroRett(tapX, tapY, 320, BARRA_Y, 40, BERSAGLIO))
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
    if (dentro(tapX, tapY, LCD_W - PADDING - MEZZA_ICONA, PADDING + 8, BERSAGLIO))
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

    if (dentro(tapX, tapY, LCD_W / 2, LCD_H - PADDING - 28, BERSAGLIO))
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

    if (editorIndice >= 0 && dentro(tapX, tapY, PADDING + MEZZA_ICONA, LCD_H - PADDING - 28, BERSAGLIO))
    {
        sveglieRimuovi(editorIndice);
        daRidisegnare[SCHEDA_ORA] = true;
        apriLista();
    }
}

static void tapNellaLista()
{
    if (dentro(tapX, tapY, LCD_W - PADDING - MEZZA_ICONA, PADDING + 8, BERSAGLIO))
    {
        vista = VISTA_SCHEDE;
        daRidisegnare[SCHEDA_ORA] = true;
        componi();
        return;
    }

    if (nSveglie < MAX_SVEGLIE && dentro(tapX, tapY, LCD_W / 2, LCD_H - PADDING - 24, BERSAGLIO))
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

        if ((vista == VISTA_LISTA || vista == VISTA_GIRI) && mosso)
        {
            float massimo = (vista == VISTA_GIRI)
                                ? (float)(cronoNGiri * 46) - 240.0f
                                : (float)(nSveglie * LISTA_RIGA) - (LISTA_BASSO - LISTA_ALTO);
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
            audioFerma();
            vista = VISTA_SCHEDE;
            daRidisegnare[SCHEDA_ORA] = true;
            componi();
            break;
        case VISTA_GIRI:
            if (dentro(tapX, tapY, LCD_W - PADDING - MEZZA_ICONA, PADDING + 8, BERSAGLIO))
            {
                vista = VISTA_SCHEDE;
                daRidisegnare[SCHEDA_CRONO] = true;
                componi();
            }
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
    case VISTA_GIRI:    disegnaGiri(comp);  break;
    case VISTA_EDITOR:  disegnaEditor(comp); break;
    case VISTA_ALLARME: disegnaAllarme(comp, t); break;
    default: return;
    }
    comp->flush();
    ++webVersione;
}

// ------------------------------------------------------------
//  ENTRARE E USCIRE DALLO STANDBY
// ------------------------------------------------------------
//  A schermo spento restano vivi solo l'orologio e le sveglie: e'
//  tutto quello che serve di notte, ed e' tutto quello che deve
//  consumare.
//
//  La voce cara e' la radio. Un ESP32 con il wifi associato beve
//  quasi cento milliampere anche mentre non trasmette nulla, contro
//  i pochi di tutto il resto messo insieme: su una batteria di
//  questa taglia sono poche ore. E di notte non serve a niente -
//  l'ora la tiene l'orologio interno, che va avanti per conto suo, e
//  la sveglia non ha bisogno di internet per suonare.
//
//  Il processore poi non ha motivo di correre a 240 MHz per
//  guardare un pulsante: a 80 fa la stessa cosa consumando meno di
//  un terzo. Torna veloce appena riaccendi, e comunque prima che
//  suoni una sveglia - l'audio ha bisogno di tutta la velocita'.

static void entraInStandby()
{
    schermoAcceso = false;
    panel->displayOff();
    audioRiposo();

    // Spegnere l'immagine non spegne il circuito che alimenta il
    // pannello: un AMOLED vuole tensioni alte generate da un
    // convertitore che continua a lavorare a vuoto. Lo stesso vale
    // per touch e audio. Qui si tolgono proprio le alimentazioni.
    //
    // Il processore, la flash e la memoria stanno su un'alimentazione
    // diversa che non si tocca - se toccarla fosse possibile, la
    // board si spegnerebbe e basta.
    if (ldoNormali & LDO_DA_SPEGNERE)
        axpScrivi(0x90, ldoNormali & ~LDO_DA_SPEGNERE);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    retePresente = false;

    // L'accelerometro serve solo al gioco delle particelle.
    qmiWrite(QMI_CTRL7, 0x00);

    setCpuFrequencyMhz(80);

    Serial.println("[standby] schermo spento, radio spenta, processore al minimo");
}

// ------------------------------------------------------------
//  DORMIRE FRA UN SECONDO E L'ALTRO
// ------------------------------------------------------------
//  A schermo spento il programma ha una cosa sola da fare: guardare
//  che ora e' e se e' il momento di suonare. Ci mette qualche
//  millesimo di secondo, e per i restanti 995 millesimi non c'e'
//  nessuna ragione di restare accesi.
//
//  Nel sonno leggero il chip stacca quasi tutto ma tiene la
//  memoria: al risveglio il programma riprende esattamente da dove
//  era, con le schede gia' disegnate e niente da ricostruire. Si
//  passa da una ventina di milliampere a due o tre.
//
//  Due cose lo svegliano: il tempo, dopo un secondo, e il pulsante,
//  subito. Cosi' l'orologio resta immediato al tocco pur avendo
//  dormito fino a un attimo prima.
//
//  Con il cavo attaccato non dorme: non serve risparmiare, e il
//  sonno interromperebbe il collegamento seriale rendendo cieco
//  chiunque stia guardando i messaggi.

static void dormiFinoAlProssimoSecondo()
{
    static uint32_t prossimoControlloAlimentazione = 0;
    if ((int32_t)(millis() - prossimoControlloAlimentazione) >= 0)
    {
        prossimoControlloAlimentazione = millis() + 30000UL;
        aggiornaBatteria();
    }

    // Con il cavo attaccato non dorme: non serve risparmiare, e nel
    // sonno la board non risponde nemmeno al caricamento.
    // Con il cavo attaccato non dorme: non serve risparmiare, e nel
    // sonno la board non risponde nemmeno al caricamento.
    if (alimentato)
    {
        delay(50);
        return;
    }

    // La memoria esterna deve restare alimentata: senza, al risveglio
    // il programma troverebbe spazzatura al posto delle schede.
    esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON);

    esp_sleep_enable_timer_wakeup(1000000ULL);
    gpio_wakeup_enable((gpio_num_t)BTN_BOOT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    esp_light_sleep_start();

    // Dopo il sonno il piedino puo' restare isolato, e letto come
    // premuto: la board entrerebbe e uscirebbe dallo standby da sola.
    pinMode(BTN_BOOT, INPUT_PULLUP);
}

static void esciDaStandby()
{
    // Prima la velocita': tutto il resto la vuole gia' piena.
    setCpuFrequencyMhz(240);

    // Poi le alimentazioni, e il tempo perche' si stabilizzino: i
    // chip che le ricevono hanno bisogno di trovare la tensione
    // pronta prima di sentirsi parlare.
    if (ldoNormali & LDO_DA_SPEGNERE)
    {
        axpScrivi(0x90, ldoNormali);

        // Il tempo perche' le tensioni salgano. Quaranta millesimi
        // bastano: sono regolatori piccoli, e ogni decimo speso qui
        // e' un decimo che aspetti guardando lo schermo nero.
        delay(40);

        // Restati senza corrente, hanno dimenticato tutto: vanno
        // riconfigurati. Ma solo i chip, non i bus con cui ci si
        // parla: quelli stanno nel processore e non si sono mai
        // spenti, e reinstallarli darebbe errore o peggio.
        audioRiconfigura();

        // L'accelerometro non si tocca qui: il suo risveglio vuole
        // mezzo secondo di attese, e serve solo al gioco del logo.
        // Lo si sveglia quando lo si usa, non quando si accende lo
        // schermo - meta' del tempo di risveglio se ne andava li'.
        imuPronto = false;

        // Un tentativo solo: se non risponde subito ci pensa il
        // ritentativo del ciclo, ogni tre decimi. Insistere qui
        // significherebbe far aspettare chi guarda lo schermo per un
        // pezzo che gli servira' fra un secondo.
        touchOk = touch.begin();
    }

    audioRisveglio();

    qmiWrite(QMI_CTRL7, 0x01);

    panel->displayOn();
    panel->setBrightness(LUMINOSITA);
    schermoAcceso = true;

    // La riconnessione non blocca: parte e va per conto suo, e
    // l'indicatore si accorgera' da solo quando la rete e' tornata.
    if (strlen(WIFI_SSID) > 0)
    {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    for (int i = 0; i < N_SCHEDE; ++i)
        daRidisegnare[i] = true;

    Serial.println("[standby] sveglio");
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
        // servirebbe a niente. E il processore deve tornare veloce
        // prima di provare a generare un suono.
        if (!schermoAcceso)
            esciDaStandby();

        audioAvvia();
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
        rinfrescaVisibili();
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

// ------------------------------------------------------------
//  IL SECONDO PULSANTE
// ------------------------------------------------------------
//  Non e' collegato al processore come quello dello standby: passa
//  dall'espansore, il chip che aggiunge otto linee sul bus condiviso.
//  Il suo stato si legge chiedendoglielo, e a riposo la linea sta
//  alta - premendo va a zero.
//
//  Serve anche all'elettronica di accensione, e tenendolo premuto sei
//  secondi la board si spegne: quella parte e' cablata e il programma
//  non la puo' impedire. Per questo qui si guarda solo il colpo
//  secco.
//
//  Fa una cosa sola: avvia e ferma il cronografo. Altrove non fa
//  niente, che e' meglio di un comando che cambia significato a
//  seconda di dove ti trovi.

#define EXP_ADDR 0x20
#define EXP_PWR 0x10   // bit 4, trovato guardando cosa cambia premendo

static bool pwrPremuto()
{
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)EXP_ADDR, (uint8_t)1) != 1) return false;
    return (Wire.read() & EXP_PWR) == 0;
}

static void gestisciSecondoPulsante()
{
    static bool precedente = false;
    static uint32_t ultimoCambio = 0;
    static uint32_t prossimaLettura = 0;

    // Ogni quaranta millesimi: un dito non fa una pressione piu' corta
    // di cosi', e ogni lettura e' una chiacchierata sul bus condiviso.
    if ((int32_t)(millis() - prossimaLettura) < 0) return;
    prossimaLettura = millis() + 40;

    bool ora = pwrPremuto();

    if (ora && !precedente && (millis() - ultimoCambio) > 250)
    {
        ultimoCambio = millis();

        if (schermoAcceso && vista == VISTA_SCHEDE && schedaCorrente == SCHEDA_CRONO)
        {
            cronoAvviaFerma();
            daRidisegnare[SCHEDA_CRONO] = true;
        }
    }

    precedente = ora;
}

static void gestisciPulsante()
{
    static bool precedente = HIGH;
    static uint32_t ultimoCambio = 0;

    bool ora = digitalRead(BTN_BOOT);

    // I contatti di un pulsante, nel momento in cui si chiudono,
    // rimbalzano per qualche millisecondo: senza questa attesa un
    // colpo solo verrebbe letto come venti, e lo schermo
    // lampeggerebbe invece di cambiare stato.
    // Il pulsante fa una cosa sola, sempre, da qualunque scheda:
    // accende e spegne lo schermo. Avevo provato a dargli anche
    // l'avvio del cronografo, ma un comando che cambia significato a
    // seconda di dove ti trovi non e' un comando: e' una cosa da
    // ricordare.
    if (precedente == HIGH && ora == LOW && (millis() - ultimoCambio) > 300)
    {
        ultimoCambio = millis();

        if (schermoAcceso)
        {
            entraInStandby();
        }
        else
        {
            esciDaStandby();
            componi();
        }
    }

    precedente = ora;
}

// ------------------------------------------------------------
//  AGGIORNAMENTO VIA WIFI
// ------------------------------------------------------------
//  Il programma nuovo arriva dalla rete e si scrive nella meta'
//  libera della memoria: al riavvio si parte da quella. Se il
//  trasferimento si interrompe a meta', la vecchia e' ancora intatta
//  e la board riparte come prima - e' per questo che le meta' sono
//  due.
//
//  Funziona solo a schermo acceso, perche' in standby la radio e'
//  spenta. Un colpo al pulsante e si puo' aggiornare.

static volatile bool otaInCorso = false;
static volatile int otaPercento = 0;

static void otaPrepara()
{
    ArduinoOTA.setHostname("dotclock");
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif

    ArduinoOTA.onStart([]() {
        otaInCorso = true;
        otaPercento = 0;
        Serial.println("[ota] aggiornamento in arrivo");
    });

    ArduinoOTA.onProgress([](unsigned int fatto, unsigned int totale) {
        otaPercento = totale ? (int)((fatto * 100ULL) / totale) : 0;
    });

    ArduinoOTA.onEnd([]() {
        otaPercento = 100;
        Serial.println("[ota] arrivato, riavvio");
    });

    ArduinoOTA.onError([](ota_error_t errore) {
        otaInCorso = false;
        Serial.printf("[ota] errore %u\n", errore);
    });

    ArduinoOTA.begin();
    Serial.println("[ota] pronto a ricevere aggiornamenti dalla rete");
}

// La schermata che si vede mentre arriva: una barra fatta di punti,
// come tutto il resto.
static void disegnaOta()
{
    comp->fillScreen(COL_SFONDO);
    testoCentrato(comp, 120, "AGGIORNAMENTO", 3, 2, COL_ETICHETTA, 2);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", otaPercento);
    testoCentrato(comp, 170, buf, 9, 7, COL_ACCESO);

    const int TACCHE = 30;
    const int16_t passo = (LCD_W - 2 * PADDING) / (TACCHE - 1);
    int accese = (otaPercento * TACCHE + 50) / 100;

    for (int i = 0; i < TACCHE; ++i)
        dmDot(comp, PADDING + i * passo, 280, 5,
              i < accese ? COL_ROSSO : COL_SPENTO);

    testoCentrato(comp, 340, "ATTENDERE", 2, 2, COL_ETICHETTA, 2);
    comp->flush();
}

// ------------------------------------------------------------
//  CHI PARLA CON LA RETE
// ------------------------------------------------------------
//  Tutte le chiamate stanno qui, su un core loro. Prima erano nel
//  ciclo principale, e ogni volta che il programma si fermava ad
//  aspettare una risposta - decimi di secondo per il condizionatore
//  in casa, secondi interi per un server lontano - lo schermo
//  restava fermo. Si vedeva bene sulla ventola, che e' l'unica cosa
//  che gira di continuo: ogni tanto si bloccava di colpo.
//
//  Aspettare non e' lavorare: mentre questo core sta fermo in
//  attesa di una risposta, l'altro continua a disegnare.

static void taskRete(void *)
{
    uint32_t prossimoMeteoTask = 0;
    uint32_t prossimaLettura = 0;

    for (;;)
    {
        // Prima i comandi in attesa: chi ha appena toccato vede gia'
        // il risultato a schermo, ma la macchina deve saperlo subito.
        if (retePresente)
            ArduinoOTA.handle();

        if (retePresente)
            for (int i = 0; i < N_CLIMI; ++i)
                if (climi[i].daInviare)
                {
                    climi[i].daInviare = false;
                    climaSpedisci(climi[i]);
                }

        if (retePresente && (int32_t)(millis() - prossimaLettura) >= 0)
        {
            prossimaLettura = millis() + 20000UL;

            for (int i = 0; i < N_CLIMI; ++i)
            {
                if (climi[i].daInviare) continue;   // ha la precedenza il comando

                bool eraAcceso = climi[i].acceso;
                float eranoGradi = climi[i].impostata;
                bool eraValido = climi[i].valido;

                climaLeggi(climi[i]);

                if (climi[i].valido != eraValido ||
                    climi[i].acceso != eraAcceso ||
                    climi[i].impostata != eranoGradi)
                    daRidisegnare[SCHEDA_CLIMA + i] = true;
            }

            if ((int32_t)(millis() - prossimoMeteoTask) >= 0)
            {
                bool ok = scaricaMeteo();
                scaricaAria();
                prossimoMeteoTask = millis() + (ok ? 30UL * 60UL * 1000UL
                                                   : 2UL * 60UL * 1000UL);
            }
        }

        // Corto, per accorgersi in fretta di un comando appena dato.
        vTaskDelay(pdMS_TO_TICKS(80));
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

    // A quanti milliampere carica: serve a ricavare la capacita' vera
    // della batteria guardando quanto sale la percentuale al minuto.
    {
        uint8_t icc = axpLeggi(0x62) & 0x1F;
        int mA = (icc <= 8) ? (icc * 25) : (200 + (icc - 8) * 100);
        Serial.printf("[axp] corrente di carica impostata: %d mA\n", mA);
    }

    ldoNormali = axpLeggi(0x90);
    Serial.printf("[axp] regolatori secondari: 0x%02X\n", ldoNormali);

    sveglieCarica();
    Serial.printf("[sveglie] %d salvate, %d attive\n", nSveglie, sveglieAttive());

    audioBegin();
    imuPronto = imuBegin();

    // Schermata di attesa: la rete puo' prendersi qualche secondo
    // e uno schermo nero sembrerebbe un blocco.
    comp->fillScreen(COL_SFONDO);
    testoCentrato(comp, 118, "ESP32", 3, 2, COL_ETICHETTA, 2);
    testoCentrato(comp, 168, "AVVIO", 8, 6, COL_ACCESO);
    testoCentrato(comp, 248, "MENTALTOY", 4, 3, COL_SECONDARIO, 2);
    testoCentrato(comp, 300, "CONNESSIONE", 3, 2, COL_ETICHETTA, 2);
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
    scaricaAria();

    for (int i = 0; i < N_CLIMI; ++i)
    {
        if (climaLeggi(climi[i]))
            Serial.printf("[clima] %s: %s, %s, chiesti %.0f, in casa %.0f, fuori %.0f\n",
                          climi[i].nome, climi[i].acceso ? "acceso" : "spento",
                          climaModo(climi[i].modo), climi[i].impostata,
                          climi[i].interna, climi[i].esterna);
        else
            Serial.printf("[clima] %s non raggiungibile\n", climi[i].nome);
    }

    for (int i = 0; i < N_SCHEDE; ++i)
        ridisegna(i);
    componi();

#if SPECCHIO
    // Lo specchio guarda lo stesso foglio che finisce a schermo.
    webBegin(comp->getFramebuffer(), LCD_W, LCD_H);
#else
    Serial.println("[web] specchio spento (SPECCHIO 0 in main.cpp)");
#endif

    if (retePresente)
        otaPrepara();

    // Da qui in poi la rete se la vede un core per conto suo.
    xTaskCreatePinnedToCore(taskRete, "rete", 6144, nullptr, 1, nullptr, 0);

    schedaCorrente = SCHEDA_CRONO;
    cronoAccumulato = 41 * 1000UL + 400;
    ridisegna(SCHEDA_CRONO);
    componi();

    Serial.println("[pronto] scorri con il dito per cambiare scheda");
}

// ------------------------------------------------------------
//  LOOP
// ------------------------------------------------------------

void loop()
{
    // Mentre arriva un aggiornamento non si fa altro: si guarda la
    // barra e si aspetta.
    if (otaInCorso)
    {
        static int ultimo = -1;
        if (otaPercento != ultimo)
        {
            ultimo = otaPercento;
            disegnaOta();
        }
        delay(20);
        return;
    }

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
        dormiFinoAlProssimoSecondo();
        return;
    }

    // Nelle schermate della sveglia il carosello e' sospeso.
    if (vista != VISTA_SCHEDE)
    {
        // Il suono viene prima del disegno: se il bus resta a secco
        // si sente un buco, mentre un fotogramma in ritardo non lo
        // nota nessuno.
        if (vista == VISTA_ALLARME)
            audioAggiorna();

        gestisciToccoModale();

        // Il campo scelto e la campana dell'allarme lampeggiano: si
        // ridisegna quando cambia lo stato, non a ritmo fisso.
        static bool ultimoLampeggio = false;
        bool lampeggia = (vista == VISTA_EDITOR || vista == VISTA_ALLARME ||
                          (vista == VISTA_GIRI && cronoAttivo));
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
            prossimoTentativo = millis() + 300;
            touchOk = touch.begin();
            if (touchOk) Serial.println("[touch] risvegliato");
        }
    }

    // Da qui in giu' si entra solo a schermo fermo.

    const struct tm &t = adesso;

    // Un rigo al minuto sulla carica: e' l'unico modo di misurare la
    // batteria senza uno strumento esterno - la pendenza di questa
    // riga, insieme alla corrente di carica, da' la capacita'.
    {
        static uint32_t prossimaNota = 0;
        if ((int32_t)(millis() - prossimaNota) >= 0)
        {
            prossimaNota = millis() + 60000UL;
            aggiornaBatteria();
            Serial.printf("[batteria] %d%% %s\n", batteria,
                          alimentato ? "(in carica)" : "(a batteria)");
        }
    }

    if (t.tm_sec != ultimoSecondo)
    {
        ultimoSecondo = t.tm_sec;
        daRidisegnare[SCHEDA_ORA] = true;

        // Se la rete c'e' ancora si controlla di continuo, non una
        // volta sola all'avvio. Crederla presente quando non c'e'
        // significa continuare a bussare a un server irraggiungibile
        // e restare appesi ogni volta.
        bool adessoCe = (WiFi.status() == WL_CONNECTED);
        if (adessoCe != retePresente)
        {
            retePresente = adessoCe;
            Serial.printf("[wifi] rete %s\n", adessoCe ? "tornata" : "persa");

            // Cambia l'indicatore, e le schede dei dati passano a
            // "NO LINK": vanno rifatte tutte.
            for (int i = 0; i < N_SCHEDE; ++i)
                daRidisegnare[i] = true;

            // Appena torna, i dati si riprendono subito invece di
            // aspettare il prossimo giro di mezz'ora.
            if (adessoCe)
                prossimoMeteo = millis();
        }
        if (t.tm_min != ultimoMinuto)
        {
            ultimoMinuto = t.tm_min;
            daRidisegnare[SCHEDA_SOLE] = true;
            aggiornaBatteria();
            daRidisegnare[SCHEDA_METEO] = true;   // per l'indicatore di carica
        }
    }

    // Si ridisegna solo quella che stai guardando: le altre
    // aspettano il momento in cui cominci a scorrere.
#if HA_LOGO
    if (fisicaAttiva && schedaCorrente == SCHEDA_LOGO)
        fisicaPasso();
#endif

    if (daRidisegnare[schedaCorrente] || numeriInMovimento)
    {
        numeriInMovimento = false;
        ridisegna(schedaCorrente);   // se un numero scorre ancora, lo rialza
        componi();
    }
    else if (t.tm_sec != ultimoSecondoInvolucro)
    {
        // Il battito della rete e la carica vivono nell'involucro, che
        // si rifa' solo quando si ricompone. Su una scheda ferma non
        // si ricomponeva mai e il pallino restava immobile: da fuori
        // sembrava rete caduta. Un fotogramma al secondo costa niente
        // e lo tiene vivo dappertutto.
        ultimoSecondoInvolucro = t.tm_sec;
        componi();
    }
    if (fabsf(palliniOpacita() - palliniOpacitaDisegnata) > 0.02f)
    {
        // I pallini si stanno spegnendo: basta ricomporre, le schede
        // sono gia' pronte e non serve ridisegnare niente.
        componi();
    }

    gestisciPulsante();
    gestisciSecondoPulsante();

    delay(5);
}
