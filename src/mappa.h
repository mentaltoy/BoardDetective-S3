/*
 * ============================================================
 *  LA MAPPA - le strade intorno a dove sei
 * ============================================================
 *
 *  Una mappa disegnata a punti, come tutto il resto. Non e' una
 *  foto di una mappa: sono le strade vere, prese una per una da
 *  OpenStreetMap come sequenze di coordinate, e ridisegnate sulla
 *  griglia del pannello.
 *
 *  PERCHE' VETTORI E NON IMMAGINI
 *  Scaricare le tessere gia' disegnate sarebbe piu' facile, ma poi
 *  bisognerebbe ridurle a punti accesi e spenti, e una mappa a
 *  colori ridotta a bianco e nero e' rumore. Avendo le strade come
 *  linee, invece, si decide tutto noi: quanto spessa e' una via
 *  rispetto a un viale, di che colore e' il fiume, cosa sparisce
 *  quando ti allontani. E' l'unico modo di farla sembrare disegnata
 *  per questo schermo invece che schiacciata dentro.
 *
 *  DA DOVE ARRIVANO
 *  Dall'Overpass API, un servizio pubblico che risponde a domande
 *  del tipo "tutte le strade in questo rettangolo". La risposta e'
 *  un JSON che puo' pesare due megabyte: non lo si tiene in memoria,
 *  lo si legge mentre arriva, un carattere alla volta, e si tengono
 *  solo i numeri che servono. Ogni coordinata diventa una coppia di
 *  metri dal centro, che stanno in quattro byte.
 *
 *  DOVE SIAMO
 *  La scheda non ha il GPS. La posizione la si chiede a un servizio
 *  che la ricava dall'indirizzo IP - che pero' dice dov'e' il
 *  fornitore di rete, non dove sei tu: da una linea Telecom a Roma
 *  risponde Milano. Per questo si possono fissare le coordinate a
 *  mano in secrets.h, e allora la rete non viene nemmeno chiesta.
 *
 *  I LIVELLI
 *  Cinque ingrandimenti, da un quarto di chilometro a quattro. Piu'
 *  ti allontani, meno strade si chiedono: a quattro chilometri le
 *  vie residenziali sarebbero un grigio uniforme, e a scaricarle
 *  tutte ci vorrebbero venti megabyte. Ogni livello si scarica la
 *  prima volta che lo guardi e da li' resta in memoria.
 * ============================================================
 */

#ifndef MAPPA_H
#define MAPPA_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ------------------------------------------------------------
//  COSA C'E' SULLA MAPPA
// ------------------------------------------------------------
//  Le classi sono poche di proposito: su una griglia di cento punti
//  di lato si distinguono tre spessori di strada, non dieci.

enum MappaClasse : uint8_t
{
    MAPPA_NIENTE = 0,
    MAPPA_ACQUA,     // fiumi e canali
    MAPPA_FERRO,     // ferrovie
    MAPPA_MINORE,    // vie residenziali, pedonali
    MAPPA_MEDIA,     // terziarie e secondarie
    MAPPA_GRANDE     // primarie, superstrade, autostrade
};

// Quanti punti e quante linee puo' tenere un livello. Il livello
// piu' largo misurato su Roma - quattro chilometri di arterie - fa
// ventimila punti. Oltre il limite si smette di aggiungere: meglio
// una mappa con qualche strada in meno che una che non arriva.
#define MAPPA_MAX_PUNTI 24000
#define MAPPA_MAX_LINEE 4000

// Una strada lunga puo' avere moltissimi vertici. Oltre questi si
// scartano: e' una linea sola, e la si vede lo stesso.
#define MAPPA_MAX_PUNTI_LINEA 2048

#define MAPPA_LIVELLI 5

// Mezza altezza della mappa, in metri, per ogni livello.
static const uint16_t MAPPA_RAGGIO[MAPPA_LIVELLI] = {250, 500, 1000, 2000, 4000};

struct MappaLivello
{
    int16_t *px = nullptr;       // metri verso est dal centro
    int16_t *py = nullptr;       // metri verso nord dal centro
    uint16_t *inizio = nullptr;  // indice del primo punto di ogni linea; inizio[n] e' la fine
    uint8_t *classe = nullptr;
    int nPunti = 0;
    int nLinee = 0;
    bool troncato = false;       // ha smesso di aggiungere per mancanza di spazio

    // Il centro a cui i metri si riferiscono. Se la posizione si
    // sposta di poco - Google oscilla di qualche metro fra un avvio e
    // l'altro - non si riscarica niente: si trasla al momento di
    // disegnare.
    double lat = 0, lon = 0;

    volatile bool pronto = false;
    volatile bool fallito = false;
    uint32_t riprovaA = 0;
};

// ------------------------------------------------------------
//  LEGGERE IL JSON MENTRE ARRIVA
// ------------------------------------------------------------
//  Non e' un parser JSON: e' una macchina a stati che conosce la
//  forma esatta di quello che manda Overpass e ne estrae tre cose -
//  le coordinate di ogni "geometry", il valore di highway, waterway
//  o railway, e la fine di ogni elemento. Tutto il resto scorre via.
//
//  La forma di un elemento e' questa, e l'ordine conta: le
//  coordinate arrivano PRIMA dei tag. Quindi si accumulano i punti
//  senza sapere ancora che strada sono, e si decide alla chiusura.
//
//    { "type": "way", "id": 1, "bounds": {...},
//      "geometry": [ {"lat": 41.9, "lon": 12.5}, ... ],
//      "tags": { "highway": "residential", ... } }
//
//  Le parentesi graffe si contano: la radice e' 1, ogni elemento
//  e' 2, quello che sta dentro un elemento e' 3. Un punto della
//  geometria si chiude tornando da 3 a 2; l'elemento si chiude
//  tornando da 2 a 1.

class MappaScanner
{
public:
    MappaScanner(MappaLivello &livello, double lat0, double lon0)
        : liv(livello), latCentro(lat0), lonCentro(lon0)
    {
        // Un grado di longitudine vale meno man mano che ci si
        // allontana dall'equatore: a Roma e' tre quarti di uno di
        // latitudine. Senza questo la mappa uscirebbe schiacciata.
        metriPerGradoLon = 111320.0 * cos(lat0 * M_PI / 180.0);
        azzera();
    }

    void alimenta(const uint8_t *dati, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            carattere((char)dati[i]);
        bytes += n;
    }

    uint32_t byteLetti() const { return bytes; }
    int lineeScartate() const { return scartate; }

private:
    MappaLivello &liv;
    double latCentro, lonCentro, metriPerGradoLon;

    uint32_t bytes = 0;
    int scartate = 0;

    int profondita = 0;
    bool inStringa = false, escape = false;
    char stringa[40];
    int lunStringa = 0;
    bool stringaChiusa = false;   // appena finita: sara' chiave o valore?

    char chiave[40];
    bool aspettaValore = false;

    char numero[24];
    int lunNumero = 0;

    bool inGeometria = false;
    double latTmp = 0, lonTmp = 0;
    bool haLat = false, haLon = false;

    uint8_t classeCorrente = MAPPA_NIENTE;

    // I punti dell'elemento in corso, in attesa di sapere che cos'e'.
    int16_t tx[MAPPA_MAX_PUNTI_LINEA], ty[MAPPA_MAX_PUNTI_LINEA];
    int nTmp = 0;

    void azzera()
    {
        nTmp = 0;
        classeCorrente = MAPPA_NIENTE;
        haLat = haLon = false;
    }

    static bool cifraDiNumero(char c)
    {
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
    }

    void carattere(char c)
    {
        // Dentro una stringa si accumula e basta. Le sequenze con la
        // barra si saltano: un nome di via con le virgolette dentro
        // non deve chiudere la stringa a meta'.
        if (inStringa)
        {
            if (escape) { escape = false; return; }
            if (c == '\\') { escape = true; return; }
            if (c == '"')
            {
                inStringa = false;
                stringa[lunStringa] = 0;
                stringaChiusa = true;
                return;
            }
            if (lunStringa < (int)sizeof(stringa) - 1) stringa[lunStringa++] = c;
            return;
        }

        // Una stringa appena chiusa e' una chiave se dopo viene un
        // due punti, altrimenti e' un valore.
        if (stringaChiusa)
        {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return;
            stringaChiusa = false;
            if (c == ':')
            {
                strcpy(chiave, stringa);
                aspettaValore = true;
                return;
            }
            if (aspettaValore)
            {
                valoreTesto(chiave, stringa);
                aspettaValore = false;
            }
            // e il carattere corrente va trattato normalmente
        }

        if (cifraDiNumero(c))
        {
            if (lunNumero < (int)sizeof(numero) - 1) numero[lunNumero++] = c;
            return;
        }
        if (lunNumero > 0)
        {
            numero[lunNumero] = 0;
            if (aspettaValore) valoreNumero(chiave, atof(numero));
            aspettaValore = false;
            lunNumero = 0;
        }

        switch (c)
        {
        case '"':
            inStringa = true;
            lunStringa = 0;
            break;

        case '{':
            ++profondita;
            if (profondita == 2) azzera();
            break;

        case '}':
            if (profondita == 3 && inGeometria && haLat && haLon)
                aggiungiPunto();
            if (profondita == 2) chiudiElemento();
            --profondita;
            if (profondita < 0) profondita = 0;
            break;

        case '[':
            if (profondita == 2 && strcmp(chiave, "geometry") == 0)
                inGeometria = true;
            aspettaValore = false;
            break;

        case ']':
            if (profondita == 2) inGeometria = false;
            break;

        default:
            // Virgole e spazi: un valore che era atteso e non e'
            // arrivato come stringa o numero - true, null - si lascia
            // cadere.
            if (c == ',') aspettaValore = false;
            break;
        }
    }

    void valoreNumero(const char *k, double v)
    {
        if (!inGeometria) return;
        if (strcmp(k, "lat") == 0) { latTmp = v; haLat = true; }
        else if (strcmp(k, "lon") == 0) { lonTmp = v; haLon = true; }
    }

    void valoreTesto(const char *k, const char *v)
    {
        if (strcmp(k, "highway") == 0)
        {
            if (!strcmp(v, "motorway") || !strcmp(v, "trunk") || !strcmp(v, "primary") ||
                !strcmp(v, "motorway_link") || !strcmp(v, "trunk_link") || !strcmp(v, "primary_link"))
                classeCorrente = MAPPA_GRANDE;
            else if (!strcmp(v, "secondary") || !strcmp(v, "tertiary") ||
                     !strcmp(v, "secondary_link") || !strcmp(v, "tertiary_link"))
                classeCorrente = MAPPA_MEDIA;
            else
                classeCorrente = MAPPA_MINORE;
        }
        else if (strcmp(k, "waterway") == 0)
            classeCorrente = MAPPA_ACQUA;
        else if (strcmp(k, "railway") == 0)
            classeCorrente = MAPPA_FERRO;
    }

    void aggiungiPunto()
    {
        haLat = haLon = false;
        if (nTmp >= MAPPA_MAX_PUNTI_LINEA) return;

        double e = (lonTmp - lonCentro) * metriPerGradoLon;
        double n = (latTmp - latCentro) * 110574.0;

        if (e > 32000) e = 32000; if (e < -32000) e = -32000;
        if (n > 32000) n = 32000; if (n < -32000) n = -32000;

        tx[nTmp] = (int16_t)lround(e);
        ty[nTmp] = (int16_t)lround(n);
        ++nTmp;
    }

    void chiudiElemento()
    {
        if (classeCorrente == MAPPA_NIENTE || nTmp < 2) { azzera(); return; }

        if (liv.nLinee >= MAPPA_MAX_LINEE || liv.nPunti + nTmp > MAPPA_MAX_PUNTI)
        {
            liv.troncato = true;
            ++scartate;
            azzera();
            return;
        }

        liv.inizio[liv.nLinee] = (uint16_t)liv.nPunti;
        liv.classe[liv.nLinee] = classeCorrente;
        memcpy(liv.px + liv.nPunti, tx, nTmp * sizeof(int16_t));
        memcpy(liv.py + liv.nPunti, ty, nTmp * sizeof(int16_t));
        liv.nPunti += nTmp;
        ++liv.nLinee;
        liv.inizio[liv.nLinee] = (uint16_t)liv.nPunti;

        azzera();
    }
};

// ------------------------------------------------------------
//  DALLE LINEE ALLE CELLE
// ------------------------------------------------------------
//  Lo schermo della mappa e' una griglia di celle, e ogni cella o
//  e' vuota o e' di una classe. Ogni segmento di ogni linea si
//  percorre a passi di una cella e accende quelle che attraversa.
//  Dove due cose si sovrappongono vince la piu' importante: una
//  strada sopra un fiume e' un ponte, e si vede la strada.

static const uint8_t MAPPA_PESO[6] = {0, 1, 2, 3, 4, 5};

static inline void mappaAccendi(uint8_t *celle, int cols, int rows,
                                int x, int y, uint8_t classe)
{
    if (x < 0 || y < 0 || x >= cols || y >= rows) return;
    uint8_t &c = celle[y * cols + x];
    if (MAPPA_PESO[classe] >= MAPPA_PESO[c]) c = classe;
}

// Bresenham fra due celle. I segmenti che stanno tutti fuori dalla
// griglia dallo stesso lato si saltano prima di cominciare: una
// strada a tre chilometri non deve far fare tremila passi a vuoto.
static void mappaSegmento(uint8_t *celle, int cols, int rows,
                          int x0, int y0, int x1, int y1, uint8_t classe)
{
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) ||
        (x0 >= cols && x1 >= cols) || (y0 >= rows && y1 >= rows))
        return;

    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;)
    {
        mappaAccendi(celle, cols, rows, x0, y0, classe);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Come un segmento ma a squadra: prima in diagonale a 45 gradi,
// poi dritto. E' il modo in cui si disegnano le mappe della metro,
// dove le linee hanno otto direzioni sole: una strada che va a
// trenta gradi diventa un tratto in diagonale e uno orizzontale, e
// tutte le linee della mappa parlano la stessa lingua.
static void mappaSegmentoSquadrato(uint8_t *celle, int cols, int rows,
                                   int x0, int y0, int x1, int y1, uint8_t classe)
{
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) ||
        (x0 >= cols && x1 >= cols) || (y0 >= rows && y1 >= rows))
        return;

    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    int ax = abs(dx), ay = abs(dy);
    int diag = ax < ay ? ax : ay;

    int x = x0, y = y0;
    mappaAccendi(celle, cols, rows, x, y, classe);
    for (int i = 0; i < diag; ++i)
    {
        x += sx; y += sy;
        mappaAccendi(celle, cols, rows, x, y, classe);
    }
    while (x != x1) { x += sx; mappaAccendi(celle, cols, rows, x, y, classe); }
    while (y != y1) { y += sy; mappaAccendi(celle, cols, rows, x, y, classe); }
}

// Semplifica una linea gia' in celle: toglie i vertici che stanno a
// meno di "tolleranza" celle dalla retta fra i loro vicini. E' il
// Douglas-Peucker, scritto senza ricorsione perche' qui lo stack e'
// piccolo: le coppie da esaminare stanno in un vettore, e se il
// vettore si riempie quel pezzo resta com'e' - meglio un tratto non
// semplificato che un programma che cade.
//
// A che serve: una strada di OpenStreetMap ha un vertice ogni pochi
// metri, e su una griglia da venti metri per cella quei vertici sono
// rumore - fanno fare al tratto un gradino di qua e uno di la'. Tolti
// quelli, restano segmenti lunghi, e un segmento lungo e' una linea
// pulita.
static int mappaSemplifica(int *xs, int *ys, int n, float tolleranza)
{
    if (n < 3) return n;

    static uint8_t tieni[MAPPA_MAX_PUNTI_LINEA];
    memset(tieni, 0, n);
    tieni[0] = tieni[n - 1] = 1;

    struct Coppia { int16_t a, b; };
    Coppia pila[48];
    int cima = 0;
    pila[cima++] = {0, (int16_t)(n - 1)};

    const float tol2 = tolleranza * tolleranza;

    while (cima > 0)
    {
        Coppia c = pila[--cima];
        int a = c.a, b = c.b;
        if (b - a < 2) continue;

        float ax = xs[a], ay = ys[a], bx = xs[b], by = ys[b];
        float vx = bx - ax, vy = by - ay;
        float lung2 = vx * vx + vy * vy;

        float peggio = -1;
        int quale = -1;
        for (int i = a + 1; i < b; ++i)
        {
            float px = xs[i] - ax, py = ys[i] - ay;
            float d2;
            if (lung2 < 0.0001f)
                d2 = px * px + py * py;
            else
            {
                float t = (px * vx + py * vy) / lung2;
                if (t < 0) t = 0; else if (t > 1) t = 1;
                float ex = px - t * vx, ey = py - t * vy;
                d2 = ex * ex + ey * ey;
            }
            if (d2 > peggio) { peggio = d2; quale = i; }
        }

        if (peggio > tol2)
        {
            tieni[quale] = 1;
            if (cima + 2 <= 48)
            {
                pila[cima++] = {(int16_t)a, (int16_t)quale};
                pila[cima++] = {(int16_t)quale, (int16_t)b};
            }
            else
                for (int i = a + 1; i < b; ++i) tieni[i] = 1;   // niente spazio: si tiene tutto
        }
    }

    int m = 0;
    for (int i = 0; i < n; ++i)
        if (tieni[i]) { xs[m] = xs[i]; ys[m] = ys[i]; ++m; }
    return m;
}

// Riempie la griglia con un livello. "metriPerCella" e' la scala:
// il centro della griglia e' il centro della mappa, l'est va a
// destra e il nord in alto - quindi la y dello schermo va al
// contrario di quella dei metri.
//
// "tolleranza" e' quanto si semplifica, in celle (zero = niente);
// "squadrato" disegna a otto direzioni invece che libero. "spostaE" e
// "spostaN" sono di quanti metri il centro dei dati sta a est e a nord
// del centro della mappa: servono quando la posizione si e' mossa di
// poco da quando i dati sono stati presi.
static void mappaRasterizza(const MappaLivello &liv, uint8_t *celle,
                            int cols, int rows, float metriPerCella,
                            float tolleranza = 0.0f, bool squadrato = false,
                            float spostaE = 0.0f, float spostaN = 0.0f)
{
    memset(celle, MAPPA_NIENTE, cols * rows);
    if (!liv.pronto) return;

    const float cx = cols * 0.5f + spostaE / metriPerCella;
    const float cy = rows * 0.5f - spostaN / metriPerCella;
    const float k = 1.0f / metriPerCella;

    static int xs[MAPPA_MAX_PUNTI_LINEA], ys[MAPPA_MAX_PUNTI_LINEA];

    // Prima acqua e ferro, poi le strade dalla piu' piccola alla piu'
    // grande: cosi' quel che conta di piu' viene disegnato per ultimo
    // e sta sopra, senza dover ragionare cella per cella.
    for (uint8_t classe = MAPPA_ACQUA; classe <= MAPPA_GRANDE; ++classe)
        for (int l = 0; l < liv.nLinee; ++l)
        {
            if (liv.classe[l] != classe) continue;
            int da = liv.inizio[l], a = liv.inizio[l + 1];

            // In celle, senza i doppioni consecutivi: due vertici
            // nella stessa cella sono un vertice.
            int n = 0;
            for (int i = da; i < a; ++i)
            {
                int x = (int)floorf(cx + liv.px[i] * k);
                int y = (int)floorf(cy - liv.py[i] * k);
                if (n > 0 && xs[n - 1] == x && ys[n - 1] == y) continue;
                xs[n] = x; ys[n] = y; ++n;
            }
            if (n < 2)
            {
                if (n == 1) mappaAccendi(celle, cols, rows, xs[0], ys[0], classe);
                continue;
            }

            if (tolleranza > 0.0f) n = mappaSemplifica(xs, ys, n, tolleranza);

            for (int i = 0; i + 1 < n; ++i)
            {
                if (squadrato)
                    mappaSegmentoSquadrato(celle, cols, rows, xs[i], ys[i], xs[i + 1], ys[i + 1], classe);
                else
                    mappaSegmento(celle, cols, rows, xs[i], ys[i], xs[i + 1], ys[i + 1], classe);
            }
        }
}

// Cosa chiedere a ogni livello. Le classi sparse via non e' che non
// si disegnino: proprio non si scaricano, che e' dove sta il vero
// risparmio.
static const char *mappaFiltroStrade(int livello)
{
    switch (livello)
    {
    case 0:
    case 1:  return "motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street|pedestrian";
    case 2:  return "motorway|trunk|primary|secondary|tertiary";
    case 3:  return "motorway|trunk|primary|secondary";
    default: return "motorway|trunk|primary";
    }
}

// Il testo della domanda per Overpass. Il rettangolo e' un po' piu'
// largo della mappa - un quarto per lato - perche' la griglia non e'
// un quadrato di metri esatto e le strade devono arrivare fino al
// bordo, non fermarsi un dito prima.
static void mappaComponiDomanda(char *out, size_t max, int livello,
                                double lat, double lon)
{
    double mezzo = MAPPA_RAGGIO[livello] * 1.25;
    double dLat = mezzo / 110574.0;
    double dLon = mezzo / (111320.0 * cos(lat * M_PI / 180.0));

    char bbox[96];
    snprintf(bbox, sizeof(bbox), "(%.5f,%.5f,%.5f,%.5f)",
             lat - dLat, lon - dLon, lat + dLat, lon + dLon);

    snprintf(out, max,
             "[out:json][timeout:40];"
             "(way[\"highway\"~\"^(%s)$\"]%s;"
             "way[\"waterway\"~\"^(river|canal)$\"]%s;"
             "way[\"railway\"=\"rail\"]%s;);"
             "out tags geom qt;",
             mappaFiltroStrade(livello), bbox, bbox, bbox);
}

// ============================================================
//  DA QUI IN GIU' SERVE LA BOARD
// ============================================================
//  Sopra c'e' tutto quello che si puo' provare al computer, con un
//  file scaricato a mano. Sotto c'e' la rete vera, la memoria della
//  board e il compito che gira su un core suo.

#ifndef MAPPA_SENZA_ARDUINO

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

static MappaLivello mappaLivelli[MAPPA_LIVELLI];

// Dove siamo. "pronta" quando la posizione e' decisa, da qualunque
// fonte sia arrivata.
static double mappaLat = 0, mappaLon = 0;
static volatile bool mappaPosizionePronta = false;
static volatile bool mappaPosizioneDaRete = false;   // ancora da chiedere
static char mappaCitta[24] = "";

// Da dove viene la posizione, e quanto e' buona. Si scrive sulla
// scheda: una mappa centrata "piu' o meno qui" deve dirlo.
enum MappaFonte : uint8_t
{
    MAPPA_FONTE_FISSA = 0,   // scritta a mano
    MAPPA_FONTE_IP,          // dall'indirizzo del fornitore: la citta', forse
    MAPPA_FONTE_WIFI         // dalle reti intorno: decine di metri
};
static volatile uint8_t mappaFonte = MAPPA_FONTE_FISSA;
static volatile int mappaPrecisione = 0;   // metri, se la fonte lo sa
static const char *mappaChiaveGoogle = "";
static bool mappaRipiegoPreciso = false;   // le coordinate di partenza sono di casa, non della citta'

// Quello che il disegno vuole sapere: che livello e' stato chiesto,
// se ne sta scaricando uno e quanto e' arrivato finora.
static volatile int mappaLivelloRichiesto = 1;
static volatile int mappaScaricando = -1;
static volatile uint32_t mappaByteArrivati = 0;

// Cresce a ogni cambiamento di stato: chi disegna guarda solo
// questo numero, e ridisegna quando cambia.
static volatile uint32_t mappaVersione = 0;

// Quando un server dice di no - troppe richieste, o non ce la fa -
// si aspetta, e ogni volta di piu': mezzo minuto, uno, due, fino a
// cinque. Continuare a chiedere a un server che ha appena detto
// "basta" e' il modo piu' sicuro di restare in castigo.
static uint32_t mappaPausaFino = 0;
static uint32_t mappaPausaDurata = 30000UL;
static bool mappaCacheOk = false;

// Di quanti metri il centro dei dati di un livello sta a est e a
// nord del centro attuale della mappa.
static void mappaScostamento(const MappaLivello &liv, float &est, float &nord)
{
    est = (float)((liv.lon - mappaLon) * 111320.0 * cos(mappaLat * M_PI / 180.0));
    nord = (float)((liv.lat - mappaLat) * 110574.0);
}

// Un livello vale ancora se il suo centro e' vicino a quello di
// adesso: entro un quarto del raggio i bordi vuoti non si notano.
// Piu' in la' i dati sono di un altro posto, e si riscaricano.
static bool mappaLivelloAncoraBuono(int l)
{
    float e, n;
    mappaScostamento(mappaLivelli[l], e, n);
    float limite = MAPPA_RAGGIO[l] * 0.25f;
    return (e * e + n * n) <= limite * limite;
}

// ------------------------------------------------------------
//  LA CACHE NELLA FLASH
// ------------------------------------------------------------
//  Un livello scaricato si scrive in un file: punti, linee, classi e
//  il centro a cui si riferiscono. Al riavvio si rilegge, e la mappa
//  c'e' subito - anche senza rete, anche se Overpass e' in castigo.
//  Sono cento KB per livello in una partizione da tre megabyte che
//  nessuno usava.
//
//  Il numero di versione nel file serve a buttare via i vecchi
//  quando cambia la forma dei dati: meglio riscaricare che leggere
//  numeri con un altro significato.

#define MAPPA_CACHE_MAGIA 0x4D415050UL   // "MAPP"
#define MAPPA_CACHE_VERSIONE 1

struct MappaCacheTesta
{
    uint32_t magia;
    uint16_t versione;
    uint16_t raggio;
    double lat, lon;
    int32_t nLinee, nPunti;
};

static void mappaCacheNome(int l, char *out, size_t max)
{
    snprintf(out, max, "/mappa%d.bin", l);
}

static void mappaCacheScrivi(int l)
{
    if (!mappaCacheOk) return;
    const MappaLivello &liv = mappaLivelli[l];

    char nome[24];
    mappaCacheNome(l, nome, sizeof(nome));
    File f = LittleFS.open(nome, "w");
    if (!f)
    {
        Serial.printf("[mappa] non riesco a scrivere %s\n", nome);
        return;
    }

    MappaCacheTesta t = {MAPPA_CACHE_MAGIA, MAPPA_CACHE_VERSIONE, MAPPA_RAGGIO[l],
                         liv.lat, liv.lon, liv.nLinee, liv.nPunti};
    f.write((const uint8_t *)&t, sizeof(t));
    f.write((const uint8_t *)liv.inizio, (liv.nLinee + 1) * sizeof(uint16_t));
    f.write((const uint8_t *)liv.classe, liv.nLinee);
    f.write((const uint8_t *)liv.px, liv.nPunti * sizeof(int16_t));
    f.write((const uint8_t *)liv.py, liv.nPunti * sizeof(int16_t));
    f.close();
    Serial.printf("[mappa] livello %d salvato nella flash\n", l);
}

static bool mappaAllocaLivello(MappaLivello &liv);

static bool mappaCacheLeggi(int l)
{
    if (!mappaCacheOk) return false;
    MappaLivello &liv = mappaLivelli[l];

    char nome[24];
    mappaCacheNome(l, nome, sizeof(nome));
    // Prima si guarda se c'e': aprire un file che non esiste fa
    // stampare alla libreria un errore rosso che non e' un errore.
    if (!LittleFS.exists(nome)) return false;
    File f = LittleFS.open(nome, "r");
    if (!f) return false;

    MappaCacheTesta t;
    bool ok = f.read((uint8_t *)&t, sizeof(t)) == sizeof(t) &&
              t.magia == MAPPA_CACHE_MAGIA && t.versione == MAPPA_CACHE_VERSIONE &&
              t.raggio == MAPPA_RAGGIO[l] &&
              t.nLinee > 0 && t.nLinee <= MAPPA_MAX_LINEE &&
              t.nPunti > 1 && t.nPunti <= MAPPA_MAX_PUNTI;

    if (ok && mappaAllocaLivello(liv))
    {
        ok = f.read((uint8_t *)liv.inizio, (t.nLinee + 1) * sizeof(uint16_t)) == (size_t)((t.nLinee + 1) * sizeof(uint16_t)) &&
             f.read((uint8_t *)liv.classe, t.nLinee) == (size_t)t.nLinee &&
             f.read((uint8_t *)liv.px, t.nPunti * sizeof(int16_t)) == (size_t)(t.nPunti * sizeof(int16_t)) &&
             f.read((uint8_t *)liv.py, t.nPunti * sizeof(int16_t)) == (size_t)(t.nPunti * sizeof(int16_t));
    }
    else
        ok = false;
    f.close();

    if (!ok)
    {
        Serial.printf("[mappa] %s non torna: lo butto\n", nome);
        LittleFS.remove(nome);
        return false;
    }

    liv.nLinee = t.nLinee;
    liv.nPunti = t.nPunti;
    liv.lat = t.lat;
    liv.lon = t.lon;
    liv.troncato = false;
    liv.fallito = false;
    liv.pronto = true;
    Serial.printf("[mappa] livello %d dalla flash: %d linee, %d punti\n", l, liv.nLinee, liv.nPunti);
    return true;
}

static bool mappaAllocaLivello(MappaLivello &liv)
{
    if (liv.px) return true;

    liv.px = (int16_t *)ps_malloc(MAPPA_MAX_PUNTI * sizeof(int16_t));
    liv.py = (int16_t *)ps_malloc(MAPPA_MAX_PUNTI * sizeof(int16_t));
    liv.inizio = (uint16_t *)ps_malloc((MAPPA_MAX_LINEE + 1) * sizeof(uint16_t));
    liv.classe = (uint8_t *)ps_malloc(MAPPA_MAX_LINEE);

    if (!liv.px || !liv.py || !liv.inizio || !liv.classe)
    {
        Serial.println("[mappa] memoria insufficiente per un livello");
        return false;
    }
    return true;
}

// Il ponte fra HTTPClient e lo scanner. writeToStream() sa togliere
// di mezzo i pezzi della codifica "chunked" e ci consegna il corpo
// pulito: noi non facciamo altro che passarlo avanti.
class MappaFlusso : public Stream
{
public:
    explicit MappaFlusso(MappaScanner &s) : scanner(s) {}

    size_t write(uint8_t c) override { scanner.alimenta(&c, 1); mappaByteArrivati = scanner.byteLetti(); return 1; }
    size_t write(const uint8_t *b, size_t n) override
    {
        scanner.alimenta(b, n);
        mappaByteArrivati = scanner.byteLetti();
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

private:
    MappaScanner &scanner;
};

// Percent-encoding per il corpo della richiesta: la domanda ha
// parentesi, virgolette e accenti circonflessi che nel corpo di un
// modulo web vanno scritti come %XX.
static void mappaCodifica(const char *in, String &out)
{
    static const char *hex = "0123456789ABCDEF";
    for (const char *p = in; *p; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
}

// A chi chiedere. Il primo e' il server principale di Overpass; il
// secondo e' un mirror indipendente, con i suoi limiti e i suoi
// blocchi. Quando uno dice di no si passa al successivo, e la pausa
// scatta solo dopo che hanno detto di no tutti. Chi risponde resta
// il preferito finche' non sbaglia.
struct MappaServer
{
    const char *url;
    bool sicuro;   // https: il certificato non si verifica, vedi Google
};

static const MappaServer MAPPA_SERVER[] = {
    {"http://overpass-api.de/api/interpreter", false},
    {"https://maps.mail.ru/osm/tools/overpass/api/interpreter", true},
};
#define MAPPA_N_SERVER ((int)(sizeof(MAPPA_SERVER) / sizeof(MAPPA_SERVER[0])))

static int mappaServerCorrente = 0;
static int mappaServerFallitiDiFila = 0;

static void mappaServerHaFallito()
{
    mappaServerCorrente = (mappaServerCorrente + 1) % MAPPA_N_SERVER;
    if (++mappaServerFallitiDiFila < MAPPA_N_SERVER)
    {
        Serial.printf("[mappa] provo con %s\n", MAPPA_SERVER[mappaServerCorrente].url);
        return;
    }

    // Nessuno risponde: da qui in poi si aspetta prima di
    // ricominciare, e ogni volta di piu'. Riprovare ogni cinque
    // secondi contro server che ci hanno messi alla porta e' il modo
    // di restarci.
    mappaServerFallitiDiFila = 0;
    mappaPausaFino = millis() + mappaPausaDurata;
    Serial.printf("[mappa] nessun server risponde: mi fermo %lu s\n",
                  (unsigned long)(mappaPausaDurata / 1000));
    if (mappaPausaDurata < 300000UL) mappaPausaDurata *= 2;
}

static bool mappaScaricaLivello(int livello)
{
    MappaLivello &liv = mappaLivelli[livello];
    if (!mappaAllocaLivello(liv)) return false;

    liv.nPunti = 0;
    liv.nLinee = 0;
    liv.troncato = false;
    liv.inizio[0] = 0;
    liv.lat = mappaLat;
    liv.lon = mappaLon;

    char domanda[512];
    mappaComponiDomanda(domanda, sizeof(domanda), livello, mappaLat, mappaLon);

    String corpo = "data=";
    mappaCodifica(domanda, corpo);

    const MappaServer &server = MAPPA_SERVER[mappaServerCorrente];

    // Il canale: in chiaro o cifrato a seconda del server. Tutti e
    // due sono client nel senso della libreria, e HTTPClient non fa
    // differenza.
    WiFiClient chiaro;
    WiFiClientSecure cifrato;
    if (server.sicuro) cifrato.setInsecure();
    WiFiClient &client = server.sicuro ? (WiFiClient &)cifrato : chiaro;

    HTTPClient http;
    // Lungo: Overpass puo' metterci dieci secondi a cominciare a
    // rispondere, perche' la domanda la esegue davvero - e il mirror
    // anche di piu'.
    http.setConnectTimeout(8000);
    http.setTimeout(60000);
    http.useHTTP10(false);

    if (!http.begin(client, server.url))
        return false;

    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    // Dire chi si e': e' la buona educazione che Overpass chiede a chi
    // lo usa, e serve a loro per capire chi bussa quando bussa troppo.
    http.setUserAgent("DotClock-ESP32 (BoardDetective-S3)");

    Serial.printf("[mappa] livello %d, raggio %d m: chiedo a %s...\n",
                  livello, MAPPA_RAGGIO[livello], server.url);
    uint32_t t0 = millis();

    int codice = http.POST(corpo);
    if (codice != 200)
    {
        // 429 e 504 sono il server che dice basta; -1 e' una
        // connessione rifiutata, che e' il server che dice basta piu'
        // forte. Si passa al prossimo.
        Serial.printf("[mappa] risposta http %d\n", codice);
        http.end();
        mappaServerHaFallito();
        return false;
    }
    mappaServerFallitiDiFila = 0;
    mappaPausaDurata = 30000UL;   // ha risposto: la prossima attesa riparte da capo

    // Lo scanner tiene due vettori da duemila punti: e' grosso per lo
    // stack di un compito, quindi vive nell'heap per il tempo che serve.
    MappaScanner *scanner = new MappaScanner(liv, mappaLat, mappaLon);
    MappaFlusso flusso(*scanner);
    int scritti = http.writeToStream(&flusso);
    http.end();

    uint32_t letti = scanner->byteLetti();
    int scartate = scanner->lineeScartate();
    delete scanner;

    if (scritti < 0 || liv.nLinee == 0)
    {
        // Anche una risposta che si spezza a meta' e' un no: si passa
        // al prossimo server, come per un rifiuto.
        Serial.printf("[mappa] scarico interrotto (%d), %lu byte, %d linee\n",
                      scritti, (unsigned long)letti, liv.nLinee);
        mappaServerHaFallito();
        return false;
    }

    Serial.printf("[mappa] livello %d pronto: %lu KB, %d linee, %d punti%s, in %lu s\n",
                  livello, (unsigned long)(letti / 1024), liv.nLinee, liv.nPunti,
                  liv.troncato ? " (troncato)" : "",
                  (unsigned long)((millis() - t0) / 1000));
    if (scartate > 0)
        Serial.printf("[mappa] %d linee non sono entrate\n", scartate);
    return true;
}

// ------------------------------------------------------------
//  LA POSIZIONE DALLE RETI INTORNO
// ------------------------------------------------------------
//  E' il trucco dei telefoni in casa, dove il GPS non prende: si
//  guardano i router che si vedono - il loro indirizzo fisico e
//  quanto forte arrivano - e si chiede a chi ha una mappa di tutti i
//  router del mondo dove sta quel gruppo. Google ce l'ha, e in citta'
//  risponde con qualche decina di metri di errore. Vuole una chiave
//  e passa per HTTPS.
//
//  Il certificato del server non si verifica: la board non ha un
//  elenco di autorita' fidate, e portarselo dietro costa piu' di
//  quel che protegge. Quello che passa e' un elenco di router
//  vicini e una posizione che finisce su uno schermo. La chiave
//  viaggia nell'indirizzo, quindi conviene che in Google sia
//  limitata a questa sola API.

static bool mappaChiediPosizioneWifi()
{
    if (!mappaChiaveGoogle || !mappaChiaveGoogle[0]) return false;

    Serial.println("[mappa] guardo le reti intorno...");
    int n = WiFi.scanNetworks(false, true);
    if (n <= 1)
    {
        Serial.printf("[mappa] viste %d reti: troppo poche per una posizione\n", n);
        WiFi.scanDelete();
        return false;
    }

    // Al massimo venti, le piu' forti prima: sono quelle che contano
    // per la posizione, e la risposta non migliora con le altre.
    // "considerIp" a false: se le reti non bastano vogliamo un no
    // secco, non la stessa risposta dell'IP con un altro nome.
    DynamicJsonDocument richiesta(4096);
    richiesta["considerIp"] = false;
    JsonArray reti = richiesta.createNestedArray("wifiAccessPoints");

    int messe = 0;
    for (int passata = 0; passata < 2 && messe < 20; ++passata)
        for (int i = 0; i < n && messe < 20; ++i)
        {
            bool forte = WiFi.RSSI(i) > -75;
            if ((passata == 0) != forte) continue;
            JsonObject r = reti.createNestedObject();
            r["macAddress"] = WiFi.BSSIDstr(i);
            r["signalStrength"] = WiFi.RSSI(i);
            r["channel"] = WiFi.channel(i);
            ++messe;
        }
    WiFi.scanDelete();

    String corpo;
    serializeJson(richiesta, corpo);
    Serial.printf("[mappa] %d reti viste, %d mandate a Google\n", n, messe);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(10000);

    String url = "https://www.googleapis.com/geolocation/v1/geolocate?key=";
    url += mappaChiaveGoogle;
    if (!http.begin(client, url)) return false;
    http.addHeader("Content-Type", "application/json");

    int codice = http.POST(corpo);
    String risposta = http.getString();
    http.end();

    if (codice != 200)
    {
        // 404 e' "non so dove sono queste reti"; 400 e 403 sono la
        // chiave sbagliata o l'API non abilitata. Si stampa tutto,
        // che e' l'unico modo di capire quale dei due.
        Serial.printf("[mappa] Google risponde %d: %s\n", codice,
                      risposta.substring(0, 160).c_str());
        return false;
    }

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, risposta) || doc["location"].isNull())
    {
        Serial.println("[mappa] Google: risposta illeggibile");
        return false;
    }

    mappaLat = doc["location"]["lat"] | 0.0;
    mappaLon = doc["location"]["lng"] | 0.0;
    mappaPrecisione = (int)(doc["accuracy"] | 0.0);
    mappaFonte = MAPPA_FONTE_WIFI;

    Serial.printf("[mappa] posizione dalle reti: %.5f, %.5f, entro %d m\n",
                  mappaLat, mappaLon, mappaPrecisione);
    return true;
}

// La posizione dall'indirizzo IP. E' quella del fornitore, non la
// tua: da casa puo' sbagliare di centinaia di chilometri. Ma e'
// gratis, non chiede chiavi e da' anche il nome della citta'.
static bool mappaChiediPosizioneIp()
{
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    if (!http.begin("http://ip-api.com/json/?fields=status,lat,lon,city"))
        return false;

    int codice = http.GET();
    if (codice != 200)
    {
        Serial.printf("[mappa] posizione: http %d\n", codice);
        http.end();
        return false;
    }

    String corpo = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, corpo) || strcmp(doc["status"] | "", "success") != 0)
    {
        Serial.println("[mappa] posizione: risposta illeggibile");
        return false;
    }

    mappaLat = doc["lat"] | 0.0;
    mappaLon = doc["lon"] | 0.0;
    mappaFonte = MAPPA_FONTE_IP;
    mappaPrecisione = 0;

    // Il nome in maiuscolo e solo ASCII: e' quello che il font sa
    // scrivere. Una lettera accentata diventa uno spazio.
    const char *citta = doc["city"] | "";
    int n = 0;
    for (const char *p = citta; *p && n < (int)sizeof(mappaCitta) - 1; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c > 126) continue;
        mappaCitta[n++] = (char)c;
    }
    mappaCitta[n] = 0;

    Serial.printf("[mappa] posizione dalla rete: %.4f, %.4f (%s)\n",
                  mappaLat, mappaLon, mappaCitta);
    return true;
}

static bool mappaLivelloDaScaricare(int l)
{
    const MappaLivello &liv = mappaLivelli[l];
    if (liv.pronto) return false;
    return !liv.fallito || (int32_t)(millis() - liv.riprovaA) >= 0;
}

// Quale livello scaricare adesso. Prima quello che si sta guardando;
// poi gli altri, dal piu' vicino al piu' lontano da quello, cosi'
// il prossimo tocco su piu' o meno trova gia' la mappa pronta.
// Scaricare tutto subito costa quattro megabyte e cinque domande al
// server: si fa una volta, all'accensione, e da li' lo zoom e'
// istantaneo. Un livello alla volta pero', perche' una richiesta
// in corso non si puo' interrompere e non vale la pena di averne
// due appese.
static int mappaProssimoDaScaricare()
{
    int centro = mappaLivelloRichiesto;
    if (centro < 0 || centro >= MAPPA_LIVELLI) centro = 1;

    if (mappaLivelloDaScaricare(centro)) return centro;

    for (int d = 1; d < MAPPA_LIVELLI; ++d)
    {
        // Prima il piu' largo: e' quello che, riscalato, copre tutto
        // lo schermo mentre si aspetta quello giusto.
        if (centro + d < MAPPA_LIVELLI && mappaLivelloDaScaricare(centro + d)) return centro + d;
        if (centro - d >= 0 && mappaLivelloDaScaricare(centro - d)) return centro - d;
    }
    return -1;
}

// Il livello pronto piu' vicino a quello chiesto, da mostrare
// riscalato finche' quello vero non arriva. A parita' di distanza si
// preferisce il piu' largo: riempie tutto lo schermo con meno
// dettaglio, invece di lasciare il bordo vuoto con piu' dettaglio.
// Torna -1 se non c'e' ancora niente.
static int mappaLivelloSostituto(int voluto)
{
    for (int d = 1; d < MAPPA_LIVELLI; ++d)
    {
        if (voluto + d < MAPPA_LIVELLI && mappaLivelli[voluto + d].pronto) return voluto + d;
        if (voluto - d >= 0 && mappaLivelli[voluto - d].pronto) return voluto - d;
    }
    return -1;
}

// Il compito. Aspetta la rete, decide la posizione, poi scarica i
// livelli uno alla volta, cominciando da quello in vista. Un livello
// che non arriva si riprova dopo mezzo minuto, non subito: un server
// in affanno non va martellato. Fra un livello e il successivo si
// respira un attimo, per lo stesso motivo.
static void mappaTask(void *)
{
    // Prima di tutto quello che c'e' gia' nella flash: la mappa
    // compare subito, e la rete serve solo per quello che manca.
    for (int l = 0; l < MAPPA_LIVELLI; ++l)
        mappaCacheLeggi(l);
    ++mappaVersione;

    bool posizioneVerificata = false;

    for (;;)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Appena la posizione e' quella definitiva, i livelli letti
        // dalla flash si controllano: quelli presi altrove si
        // riscaricano, quelli presi qui vicino si tengono e si
        // traslano al momento di disegnare.
        if (mappaPosizionePronta && !posizioneVerificata)
        {
            posizioneVerificata = true;
            for (int l = 0; l < MAPPA_LIVELLI; ++l)
                if (mappaLivelli[l].pronto && !mappaLivelloAncoraBuono(l))
                {
                    Serial.printf("[mappa] livello %d era di un altro posto: lo riscarico\n", l);
                    mappaLivelli[l].pronto = false;
                }
            ++mappaVersione;
        }

        // Prima le reti intorno, che sono precise; se non si puo' -
        // niente chiave, o Google non conosce questa zona - l'IP, che
        // almeno la citta' la azzecca quasi sempre. Se nemmeno quello,
        // resta la posizione di ripiego con cui si e' partiti, e si
        // riprova fra venti secondi.
        //
        // Ma l'IP si chiede solo se il ripiego e' vago: se le
        // coordinate di casa sono scritte a mano, sono meglio di
        // qualunque cosa possa dire l'indirizzo del fornitore, e se il
        // WiFi non risponde si tengono quelle.
        if (!mappaPosizionePronta)
        {
            if (mappaChiediPosizioneWifi() ||
                (!mappaRipiegoPreciso && mappaChiediPosizioneIp()))
            {
                mappaPosizionePronta = true;
                ++mappaVersione;
            }
            else if (mappaRipiegoPreciso)
            {
                Serial.println("[mappa] la rete non sa dove siamo: valgono le coordinate scritte a mano");
                mappaPosizionePronta = true;
                ++mappaVersione;
            }
            else
                vTaskDelay(pdMS_TO_TICKS(20000));
            continue;
        }

        // In castigo non si chiede niente.
        if ((int32_t)(millis() - mappaPausaFino) < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int l = mappaProssimoDaScaricare();
        if (l >= 0)
        {
            MappaLivello &liv = mappaLivelli[l];

            mappaScaricando = l;
            mappaByteArrivati = 0;
            ++mappaVersione;

            bool ok = mappaScaricaLivello(l);

            // Se non e' arrivato, il livello resta il primo della fila:
            // la colpa e' del server, e ad aspettare ci pensa la pausa
            // dei server. Questi cinque secondi servono solo a non
            // ripetere all'istante la stessa domanda allo stesso posto.
            liv.pronto = ok;
            liv.fallito = !ok;
            liv.riprovaA = millis() + 5000UL;
            mappaScaricando = -1;
            ++mappaVersione;

            if (ok) mappaCacheScrivi(l);

            vTaskDelay(pdMS_TO_TICKS(ok ? 2000 : 5000));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// Parte con la posizione gia' decisa - se la conosci - o con la
// promessa di chiederla alla rete.
static void mappaBegin(double lat, double lon, const char *citta, bool chiediAllaRete,
                       const char *chiaveGoogle = "", bool ripiegoPreciso = false)
{
    mappaLat = lat;
    mappaLon = lon;
    strncpy(mappaCitta, citta ? citta : "", sizeof(mappaCitta) - 1);
    mappaChiaveGoogle = chiaveGoogle ? chiaveGoogle : "";
    mappaRipiegoPreciso = ripiegoPreciso;
    mappaFonte = MAPPA_FONTE_FISSA;

    mappaPosizionePronta = !chiediAllaRete;
    mappaPosizioneDaRete = chiediAllaRete;

    // La partizione dati. La prima volta e' vuota e va formattata: ci
    // mette qualche secondo, una volta sola.
    mappaCacheOk = LittleFS.begin(true);
    if (mappaCacheOk)
        Serial.printf("[mappa] flash: %lu KB usati su %lu\n",
                      (unsigned long)(LittleFS.usedBytes() / 1024),
                      (unsigned long)(LittleFS.totalBytes() / 1024));
    else
        Serial.println("[mappa] partizione dati non disponibile: niente cache");

    // Lo scanner alloca i suoi vettori nell'heap, ma il compito parla
    // con HTTPClient, String e - per Google - con TLS, che nella
    // stretta di mano vuole parecchio stack. Sedici KB.
    xTaskCreatePinnedToCore(mappaTask, "mappa", 16384, nullptr, 1, nullptr, 0);
}

static void mappaChiediLivello(int livello)
{
    if (livello < 0) livello = 0;
    if (livello >= MAPPA_LIVELLI) livello = MAPPA_LIVELLI - 1;
    mappaLivelloRichiesto = livello;
    ++mappaVersione;
}

#endif // MAPPA_SENZA_ARDUINO

#endif // MAPPA_H
