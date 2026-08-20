/*
 * ============================================================
 *  FT3168 - il touch capacitivo del pannello
 * ============================================================
 *
 *  Nessuna libreria: il chip si legge scrivendo e leggendo
 *  registri sul bus I2C, esattamente come l'accelerometro.
 *
 *  I FocalTech hanno tutti la stessa mappa di registri. Quello
 *  che serve sta in cinque byte consecutivi a partire da 0x02:
 *
 *    0x02  quante dita stanno toccando (solo i 4 bit bassi)
 *    0x03  bit 7-6 = tipo di evento, bit 3-0 = X parte alta
 *    0x04  X parte bassa
 *    0x05  bit 7-4 = numero del dito, bit 3-0 = Y parte alta
 *    0x06  Y parte bassa
 *
 *  Le coordinate sono a 12 bit spezzate in due byte perche' 8 bit
 *  arrivano solo a 255 e lo schermo e' piu' largo di cosi'.
 *
 *  Leggiamo tutti e cinque i byte in un colpo solo: sono contigui,
 *  e una singola transazione I2C evita di beccare il chip mentre
 *  aggiorna i registri fra una lettura e l'altra.
 * ============================================================
 */

#ifndef TOUCH_H
#define TOUCH_H

#include <Arduino.h>
#include <Wire.h>

#define FT3168_ADDR 0x38
#define FT_REG_STATUS 0x02   // primo dei cinque byte che ci interessano
#define TP_INT 21            // piedino di interrupt, serve anche per svegliarlo

// Se un giorno il touch dovesse risultare ruotato o specchiato
// rispetto a quello che vedi, si aggiusta qui senza toccare altro.
#define TOUCH_SWAP_XY 0
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0

struct TouchPoint
{
    bool premuto;
    int16_t x;
    int16_t y;
};

class Touch
{
public:
    bool begin()
    {
        sveglia();

        // Un colpetto sul registro 0x00: modalita' normale, niente
        // riconoscimento gesti a bordo. I gesti li calcoliamo noi,
        // cosi' abbiamo in mano la fisica dello scorrimento.
        Wire.beginTransmission(FT3168_ADDR);
        Wire.write(0x00);
        Wire.write(0x00);
        bool ok = (Wire.endTransmission() == 0);

        // 0xA4 = come si comporta il piedino di interrupt.
        // 0x00 significa "tienilo basso finche' c'e' un dito":
        // a noi non serve, leggiamo a polling, ma lasciarlo in uno
        // stato noto evita sorprese.
        Wire.beginTransmission(FT3168_ADDR);
        Wire.write(0xA4);
        Wire.write(0x00);
        Wire.endTransmission();

        return ok;
    }

    // I FocalTech, se nessuno li tocca, si addormentano sul serio:
    // smettono di rispondere anche solo alla chiamata sul bus, e
    // sembrano guasti. Si svegliano con un impulso sul piedino di
    // interrupt, che per un istante usiamo al contrario: invece di
    // ascoltarlo, ci parliamo.
    void sveglia()
    {
        pinMode(TP_INT, OUTPUT);
        digitalWrite(TP_INT, HIGH);
        delay(1);
        digitalWrite(TP_INT, LOW);
        delay(5);
        digitalWrite(TP_INT, HIGH);
        delay(5);
        pinMode(TP_INT, INPUT_PULLUP);
        delay(50);
    }

    // Il chip risponde sul bus?
    bool presente()
    {
        Wire.beginTransmission(FT3168_ADDR);
        return Wire.endTransmission() == 0;
    }

    // Restituisce la posizione del primo dito. Se non c'e' nessun
    // dito appoggiato, "premuto" e' false e le coordinate non
    // vanno guardate.
    TouchPoint leggi()
    {
        TouchPoint tp = {false, 0, 0};

        // Il chip tiene questo piedino basso per tutto il tempo in
        // cui un dito e' appoggiato. Se e' alto non c'e' niente da
        // leggere, e chiedere lo stesso significa svegliarlo per
        // nulla duecento volte al secondo: rumore sul bus e qualche
        // risposta persa. Guardare un piedino costa un'istruzione.
        if (digitalRead(TP_INT) == HIGH)
            return tp;

        Wire.beginTransmission(FT3168_ADDR);
        Wire.write(FT_REG_STATUS);
        if (Wire.endTransmission(false) != 0)
            return tp;

        if (Wire.requestFrom((uint8_t)FT3168_ADDR, (uint8_t)5) != 5)
            return tp;

        uint8_t dita = Wire.read() & 0x0F;
        uint8_t xh = Wire.read();
        uint8_t xl = Wire.read();
        uint8_t yh = Wire.read();
        uint8_t yl = Wire.read();

        // Alcuni esemplari, quando il dito si stacca, lasciano il
        // contatore a un valore assurdo invece che a zero.
        if (dita == 0 || dita > 5)
            return tp;

        // I bit 7-6 di xh dicono che tipo di evento e': 2 significa
        // "dito ancora giu'", 0 "appena appoggiato", 1 "appena
        // sollevato". Solo i primi due contano come tocco attivo.
        uint8_t evento = xh >> 6;
        if (evento == 1)
            return tp;

        int16_t x = ((xh & 0x0F) << 8) | xl;
        int16_t y = ((yh & 0x0F) << 8) | yl;

#if TOUCH_SWAP_XY
        int16_t t = x; x = y; y = t;
#endif
#if TOUCH_FLIP_X
        x = 367 - x;
#endif
#if TOUCH_FLIP_Y
        y = 447 - y;
#endif

        tp.premuto = true;
        tp.x = x;
        tp.y = y;
        return tp;
    }
};

#endif // TOUCH_H
