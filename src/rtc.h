/*
 * ============================================================
 *  PCF85063 - l'orologio che non si ferma mai
 * ============================================================
 *
 *  L'ESP32 sa contare i secondi, ma quando lo stacchi dimentica
 *  tutto: riparte dal 1 gennaio 1970. Il PCF85063 e' un chip
 *  minuscolo che fa una cosa sola, contare il tempo, e continua a
 *  farlo a spese della batteria anche a scheda spenta.
 *
 *  Cosi' l'orologio sa che ora e' gia' al primo istante dopo
 *  l'accensione, senza aspettare il wifi.
 *
 *  I registri sono in BCD: un byte contiene due cifre decimali,
 *  una per ogni gruppo di 4 bit. Il numero 47 non e' scritto come
 *  0x2F (47 in binario) ma come 0x47. Sembra una stranezza, ma
 *  nasce dai display a sette segmenti, dove serviva leggere le
 *  cifre separate senza fare divisioni.
 * ============================================================
 */

#ifndef RTC_H
#define RTC_H

#include <Arduino.h>
#include <Wire.h>
#include <time.h>

#define PCF_ADDR 0x51
#define PCF_REG_SECONDS 0x04

static inline uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static inline uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

// Legge l'ora dal chip e la scrive in "out".
// Torna false se il chip non risponde o se il suo oscillatore si e'
// fermato (batteria scarica o primo avvio): in quel caso il
// contenuto e' spazzatura e va ignorato.
static bool rtcLeggi(struct tm &out)
{
    Wire.beginTransmission(PCF_ADDR);
    Wire.write(PCF_REG_SECONDS);
    if (Wire.endTransmission(false) != 0)
        return false;
    if (Wire.requestFrom((uint8_t)PCF_ADDR, (uint8_t)7) != 7)
        return false;

    uint8_t sec = Wire.read();
    uint8_t min = Wire.read();
    uint8_t ore = Wire.read();
    uint8_t giorno = Wire.read();
    Wire.read();                  // giorno della settimana: lo ricalcoliamo
    uint8_t mese = Wire.read();
    uint8_t anno = Wire.read();

    // Il bit 7 dei secondi e' la spia "l'oscillatore si e' fermato".
    if (sec & 0x80)
        return false;

    out.tm_sec = bcd2dec(sec & 0x7F);
    out.tm_min = bcd2dec(min & 0x7F);
    out.tm_hour = bcd2dec(ore & 0x3F);
    out.tm_mday = bcd2dec(giorno & 0x3F);
    out.tm_mon = bcd2dec(mese & 0x1F) - 1;      // tm conta i mesi da 0
    out.tm_year = bcd2dec(anno) + 100;          // tm conta gli anni dal 1900
    out.tm_isdst = -1;

    if (out.tm_mon < 0 || out.tm_mon > 11 || out.tm_mday < 1 || out.tm_mday > 31)
        return false;

    return true;
}

static bool rtcScrivi(const struct tm &t)
{
    Wire.beginTransmission(PCF_ADDR);
    Wire.write(PCF_REG_SECONDS);
    Wire.write(dec2bcd(t.tm_sec) & 0x7F);   // bit 7 a zero: oscillatore buono
    Wire.write(dec2bcd(t.tm_min));
    Wire.write(dec2bcd(t.tm_hour));
    Wire.write(dec2bcd(t.tm_mday));
    Wire.write(t.tm_wday & 0x07);
    Wire.write(dec2bcd(t.tm_mon + 1));
    Wire.write(dec2bcd(t.tm_year % 100));
    return Wire.endTransmission() == 0;
}

#endif // RTC_H
