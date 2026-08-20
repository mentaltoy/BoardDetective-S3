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
