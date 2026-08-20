/*
 * ============================================================
 *  DOTMATRIX - testo e grafica a matrice di punti
 * ============================================================
 *
 *  Tutto quello che vedi a schermo e' fatto di pallini.
 *  Non e' un filtro applicato dopo: e' proprio il modo in cui
 *  disegniamo. Una lettera non e' un glifo con le curve, e' una
 *  griglia 5x7 di caselle accese o spente, e ogni casella accesa
 *  diventa un cerchietto pieno.
 *
 *  Due numeri governano l'aspetto:
 *    - "passo": la distanza fra i centri di due punti vicini
 *    - "diam" : quanto e' grosso ogni punto
 *  Il rapporto fra i due decide se il testo sembra compatto o
 *  arioso. Circa 0.7 e' il punto in cui si legge bene restando
 *  chiaramente una matrice di led.
 * ============================================================
 */

#ifndef DOTMATRIX_H
#define DOTMATRIX_H

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

// ------------------------------------------------------------
//  IL FONT 5x7
// ------------------------------------------------------------
//  Ogni carattere e' descritto da 5 byte, uno per colonna.
//  Dentro il byte, il bit 0 e' la riga in alto e il bit 6 quella
//  in basso. Cosi' 0x7F (bit 0-6 tutti a uno) e' una colonna
//  piena da cima a fondo: la gamba sinistra della "H".
//
//  Copre solo maiuscole, cifre e pochi simboli. Non e' una
//  mancanza: il carattere di un display a led e' questo.

static const uint8_t DM_FONT_FIRST = 32;   // spazio
static const uint8_t DM_FONT_LAST  = 96;   // fino a '`'

static const uint8_t DM_FONT[][5] PROGMEM = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // 32 spazio
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // 33 !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // 34 "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // 35 #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // 36 $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // 37 %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // 38 &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // 39 '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // 40 (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // 41 )
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // 42 *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // 43 +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // 44 ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // 45 -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // 46 .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // 47 /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 48 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 49 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 50 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 51 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 52 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 53 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 54 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 55 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 56 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 57 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // 58 :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // 59 ;
    {0x08, 0x14, 0x22, 0x41, 0x00}, // 60 <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // 61 =
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, // 62 >  usato come freccia ->
    {0x02, 0x01, 0x51, 0x09, 0x06}, // 63 ?
    {0x00, 0x06, 0x09, 0x09, 0x06}, // 64 @  usato come grado
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 65 A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 66 B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 67 C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 68 D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 69 E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 70 F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 71 G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 72 H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 73 I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 74 J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 75 K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 76 L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 77 M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 78 N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 79 O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 80 P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 81 Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 82 R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 83 S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 84 T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 85 U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 86 V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 87 W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 88 X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 89 Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 90 Z
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // 91 [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // 92 backslash
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // 93 ]
    {0x00, 0x07, 0x05, 0x07, 0x00}, // 94 ^  usato come grado piccolo
    {0x40, 0x40, 0x40, 0x40, 0x40}, // 95 _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // 96 `
};

// ------------------------------------------------------------
//  DISSOLVENZA
// ------------------------------------------------------------
//  Il display non sa disegnare "mezzo trasparente": ogni pixel ha
//  un colore e basta. Ma lo sfondo qui e' sempre nero, e allora
//  sbiadire verso il nero da' lo stesso risultato: si scalano le
//  tre componenti e il colore si spegne.
//
//  Il formato tiene 5 bit per il rosso, 6 per il verde e 5 per il
//  blu dentro sedici bit. Al verde ne tocca uno in piu' perche'
//  l'occhio distingue molte piu' gradazioni di verde che di blu.

static inline uint16_t dmSfuma(uint16_t colore, float quanto)
{
    if (quanto >= 1.0f) return colore;
    if (quanto <= 0.0f) return 0;
    uint16_t r = (colore >> 11) & 0x1F;
    uint16_t g = (colore >> 5) & 0x3F;
    uint16_t b = colore & 0x1F;
    r = (uint16_t)(r * quanto);
    g = (uint16_t)(g * quanto);
    b = (uint16_t)(b * quanto);
    return (r << 11) | (g << 5) | b;
}

// ------------------------------------------------------------
//  IL PUNTO
// ------------------------------------------------------------
//  Sotto i 3 pixel di diametro un cerchio non e' piu' un cerchio:
//  l'algoritmo di Bresenham degenera in una crocetta storta. Sotto
//  quella soglia disegniamo un quadratino, che a quelle dimensioni
//  l'occhio legge comunque come un punto.

static inline void dmDot(Arduino_GFX *g, int16_t cx, int16_t cy, int16_t diam, uint16_t color)
{
    if (diam <= 1)
    {
        g->drawPixel(cx, cy, color);
    }
    else if (diam <= 3)
    {
        g->fillRect(cx - (diam >> 1), cy - (diam >> 1), diam, diam, color);
    }
    else
    {
        g->fillCircle(cx, cy, diam >> 1, color);
    }
}

// ------------------------------------------------------------
//  IL MARCATORE
// ------------------------------------------------------------
//  Il punto che dice "sei qui" sui quadranti: l'ora nei secondi,
//  l'adesso sulla curva del barometro.
//
//  Ingrandire e basta il punto non funzionava: diventava l'unica
//  cosa a schermo a non essere fatta di punti, e in mezzo a una
//  matrice di led un disco liscio si legge come un corpo estraneo.
//  Qui invece e' un punto centrale con altri sei intorno, che a
//  distanza si legge come una macchia tonda ma da vicino resta
//  chiaramente della stessa materia del resto.
//
//  Sei e non otto: sei punti equidistanti dal centro sono anche
//  equidistanti fra loro, e il contorno viene regolare senza
//  addensarsi negli angoli.

// I due limiti servono a farlo entrare e uscire da dietro un bordo
// invece di comparire e sparire di colpo: i punti oltre la finestra
// semplicemente non si disegnano.
static void dmMarcatore(Arduino_GFX *g, int16_t cx, int16_t cy,
                        int16_t raggio, int16_t diam, uint16_t colore,
                        int16_t finestraAlto = -32000,
                        int16_t finestraBasso = 32000)
{
    if (cy >= finestraAlto && cy <= finestraBasso)
        dmDot(g, cx, cy, diam, colore);

    for (int k = 0; k < 6; ++k)
    {
        float a = (float)k * (float)M_PI / 3.0f;
        int16_t py = cy + (int16_t)lroundf(sinf(a) * raggio);
        if (py < finestraAlto || py > finestraBasso) continue;
        dmDot(g, cx + (int16_t)lroundf(cosf(a) * raggio), py, diam, colore);
    }
}

// ------------------------------------------------------------
//  TESTO
// ------------------------------------------------------------
//  "gap" e' quante colonne vuote lasciare fra una lettera e la
//  successiva. Con 1 il testo e' compatto; alzandolo si ottiene
//  quella spaziatura larga da etichetta tecnica.

static inline int16_t dmTextWidth(const char *s, int16_t passo, int16_t gap = 1)
{
    int n = strlen(s);
    if (n == 0) return 0;
    return (n * (5 + gap) - gap) * passo;
}

static inline int16_t dmTextHeight(int16_t passo)
{
    return 7 * passo;
}

// Disegna un solo carattere, saltando i punti che escono da una
// finestra verticale. Il ritaglio serve al rullo: la cifra che
// scorre deve sparire dietro il bordo, non uscire a invadere la
// riga sopra. Un punto o e' dentro o e' fuori, non si dissolve:
// e' una matrice di led, non una sfumatura.
static void dmCharClip(Arduino_GFX *g, int16_t xSinistra, int16_t yAlto, char c,
                       int16_t passo, int16_t diam, uint16_t colore,
                       int16_t finestraAlto, int16_t finestraBasso)
{
    uint8_t k = (uint8_t)c;
    if (k >= 'a' && k <= 'z') k -= 32;
    if (k < DM_FONT_FIRST || k > DM_FONT_LAST) k = 32;

    const uint8_t *glifo = DM_FONT[k - DM_FONT_FIRST];
    const int16_t px0 = xSinistra + (passo >> 1);
    const int16_t py0 = yAlto + (passo >> 1);

    for (int col = 0; col < 5; ++col)
    {
        uint8_t colonna = pgm_read_byte(&glifo[col]);
        if (!colonna) continue;
        for (int riga = 0; riga < 7; ++riga)
        {
            if (!(colonna & (1 << riga))) continue;
            int16_t py = py0 + riga * passo;
            if (py < finestraAlto || py > finestraBasso) continue;
            dmDot(g, px0 + col * passo, py, diam, colore);
        }
    }
}

// ------------------------------------------------------------
//  IL RULLO
// ------------------------------------------------------------
//  Come i contatori meccanici: le cifre stanno su una ruota che
//  gira dietro una finestrella. Quando il numero cambia, la cifra
//  vecchia scorre via verso l'alto e la nuova sale da sotto.
//
//  Solo le cifre che cambiano davvero si muovono. In un orologio,
//  quando scatta il minuto, le ore restano ferme: se ruotasse
//  tutto sembrerebbe un tabellone che si ricarica, non un
//  meccanismo.

struct DmRullo
{
    char testo[20];
    char precedente[20];
    uint32_t inizio;
    uint8_t cambiate;   // quante cifre si stanno muovendo in questo giro
    bool attivo;
};

// Aggiorna il rullo con il nuovo testo e lo disegna allo stato in
// cui si trova adesso. Torna true finche' si sta muovendo: a quel
// punto chi chiama sa che deve ridisegnare ancora.
static bool dmRullo(Arduino_GFX *g, DmRullo &r, const char *nuovo,
                    int16_t x, int16_t y, int16_t passo, int16_t diam,
                    uint16_t colore, int16_t gap = 1, uint16_t durata = 800,
                    uint16_t ritardo = 80, bool animare = true)
{
    if (strncmp(r.testo, nuovo, sizeof(r.testo) - 1) != 0)
    {
        // Alla primissima volta non c'e' niente da cui scorrere.
        bool primo = (r.testo[0] == '\0');
        strncpy(r.precedente, r.testo, sizeof(r.precedente) - 1);
        r.precedente[sizeof(r.precedente) - 1] = '\0';
        strncpy(r.testo, nuovo, sizeof(r.testo) - 1);
        r.testo[sizeof(r.testo) - 1] = '\0';

        int vecchie = strlen(r.precedente);
        r.cambiate = 0;
        for (int i = 0; r.testo[i]; ++i)
        {
            char v = (i < vecchie) ? r.precedente[i] : ' ';
            if (r.testo[i] != v) ++r.cambiate;
        }

        r.inizio = millis();
        r.attivo = animare && !primo && r.cambiate > 0;
    }

    const int16_t altezza = 7 * passo;
    const int16_t corsa = altezza + passo;   // il passo extra e' lo stacco fra due cifre sulla ruota
    const int16_t finestraAlto = y + (passo >> 1);
    const int16_t finestraBasso = y + altezza - (passo >> 1);

    uint32_t passato = 0;
    if (r.attivo)
    {
        passato = millis() - r.inizio;
        // Finisce quando ha finito l'ultima, non la prima.
        uint32_t totale = durata + (uint32_t)(r.cambiate > 0 ? r.cambiate - 1 : 0) * ritardo;
        if (passato >= totale)
            r.attivo = false;
    }

    int16_t penna = x;
    int ordine = 0;   // la quantesima cifra che cambia, da sinistra
    int vecchie = strlen(r.precedente);

    for (int i = 0; r.testo[i]; ++i)
    {
        char nuovoC = r.testo[i];
        char vecchioC = (i < vecchie) ? r.precedente[i] : ' ';

        if (!r.attivo || nuovoC == vecchioC)
        {
            dmCharClip(g, penna, y, nuovoC, passo, diam, colore,
                       finestraAlto, finestraBasso);
        }
        else
        {
            // Ogni cifra parte un po' dopo la precedente. In un
            // contatore vero le ruote non scattano all'unisono: e'
            // quel minimo sfasamento a farlo sembrare un meccanismo
            // invece di un tabellone che si aggiorna.
            uint32_t mio = (uint32_t)ordine * ritardo;
            ++ordine;

            float p = 0.0f;
            if (passato > mio)
            {
                float t = (float)(passato - mio) / (float)durata;
                if (t >= 1.0f)
                {
                    p = 1.0f;
                }
                else
                {
                    // Piano all'inizio, veloce in mezzo, piano alla
                    // fine. Una ruota vera e' ferma e deve vincere la
                    // propria inerzia: partire subito a tutta
                    // velocita' e frenare da' uno strappo, perche'
                    // nessun oggetto con una massa si comporta cosi'.
                    p = (t < 0.5f) ? (4.0f * t * t * t)
                                   : (1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f);
                }
            }

            int16_t scorso = (int16_t)lroundf(p * corsa);
            dmCharClip(g, penna, y - scorso, vecchioC, passo, diam, colore,
                       finestraAlto, finestraBasso);
            dmCharClip(g, penna, y + corsa - scorso, nuovoC, passo, diam, colore,
                       finestraAlto, finestraBasso);
        }
        penna += (5 + gap) * passo;
    }

    return r.attivo;
}

// Disegna a partire dall'angolo in alto a sinistra della griglia.
// I punti "spenti" si disegnano solo se ne passi il colore: serve
// per l'effetto pannello led dove la matrice resta sempre visibile.
static void dmText(Arduino_GFX *g, int16_t x, int16_t y, const char *s,
                   int16_t passo, int16_t diam, uint16_t color,
                   int16_t gap = 1, uint16_t colorOff = 0, bool drawOff = false)
{
    int16_t penna = x + (passo >> 1);
    const int16_t top = y + (passo >> 1);

    for (const char *p = s; *p; ++p)
    {
        uint8_t c = (uint8_t)*p;
        if (c >= 'a' && c <= 'z') c -= 32;          // solo maiuscole
        if (c < DM_FONT_FIRST || c > DM_FONT_LAST)  // fuori tabella: spazio
            c = 32;

        const uint8_t *glifo = DM_FONT[c - DM_FONT_FIRST];

        for (int col = 0; col < 5; ++col)
        {
            uint8_t colonna = pgm_read_byte(&glifo[col]);
            for (int riga = 0; riga < 7; ++riga)
            {
                bool acceso = colonna & (1 << riga);
                if (acceso)
                    dmDot(g, penna + col * passo, top + riga * passo, diam, color);
                else if (drawOff)
                    dmDot(g, penna + col * passo, top + riga * passo, diam, colorOff);
            }
        }
        penna += (5 + gap) * passo;
    }
}

static inline void dmTextCentered(Arduino_GFX *g, int16_t cx, int16_t y, const char *s,
                                  int16_t passo, int16_t diam, uint16_t color,
                                  int16_t gap = 1)
{
    dmText(g, cx - (dmTextWidth(s, passo, gap) >> 1), y, s, passo, diam, color, gap);
}

// ------------------------------------------------------------
//  ANELLI DI PUNTI
// ------------------------------------------------------------
//  Il quadrante circolare dei riferimenti: N punti distribuiti su
//  un arco, i primi "accesi" e i restanti in grigio, piu' un punto
//  di testa in rosso che segna la posizione corrente.
//
//  Gli angoli sono in gradi, 0 = ore 12, e crescono in senso
//  orario: e' come si legge un orologio, non come si fa in
//  trigonometria. La conversione avviene qui dentro una volta
//  sola, cosi' chi chiama ragiona in modo naturale.

static void dmRing(Arduino_GFX *g, int16_t cx, int16_t cy, int16_t raggio,
                   int count, float gradiInizio, float gradiArco,
                   int accesi, int16_t diam,
                   uint16_t colorOn, uint16_t colorOff,
                   uint16_t colorTesta = 0, bool testa = false)
{
    if (count < 2) return;

    for (int i = 0; i < count; ++i)
    {
        float t = (float)i / (float)(count - 1);
        float a = (gradiInizio + gradiArco * t - 90.0f) * (float)M_PI / 180.0f;
        int16_t px = cx + (int16_t)lroundf(cosf(a) * raggio);
        int16_t py = cy + (int16_t)lroundf(sinf(a) * raggio);

        uint16_t c = (i < accesi) ? colorOn : colorOff;
        if (testa && i == accesi - 1 && accesi > 0)
            c = colorTesta;

        dmDot(g, px, py, diam, c);
    }
}

// ------------------------------------------------------------
//  ONDA
// ------------------------------------------------------------
//  Le barrette verticali di punti tipiche del registratore. Ogni
//  colonna e' alta quanto dice l'ampiezza, centrata sulla linea.

static void dmWave(Arduino_GFX *g, int16_t x, int16_t cy, int barre,
                   int16_t passoX, int16_t passoY, int16_t diam,
                   const uint8_t *ampiezze, int riempite,
                   uint16_t colorOn, uint16_t colorOff)
{
    for (int i = 0; i < barre; ++i)
    {
        int h = ampiezze[i];
        uint16_t c = (i < riempite) ? colorOn : colorOff;
        for (int j = -h; j <= h; ++j)
            dmDot(g, x + i * passoX, cy + j * passoY, diam, c);
    }
}

#endif // DOTMATRIX_H
