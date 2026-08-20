/*
 * ============================================================
 *  QMI8658 - l'accelerometro
 * ============================================================
 *
 *  Non misura l'inclinazione: misura l'accelerazione. Ma un
 *  oggetto fermo su un tavolo ha una sola accelerazione addosso,
 *  la gravita', che punta sempre verso il basso. Da come si
 *  distribuisce sui tre assi si capisce come e' girato l'oggetto.
 *
 *  IL PASSO CHE MANCA OVUNQUE
 *  Questo sensore vuole un reset esplicito all'accensione. Senza,
 *  risponde all'appello, dichiara il proprio nome, accetta la
 *  configurazione - e non produce un solo campione: tutte le
 *  letture tornano zero. Non e' scritto in evidenza da nessuna
 *  parte e manca in quasi tutti gli esempi in circolazione.
 * ============================================================
 */

#ifndef IMU_H
#define IMU_H

#include <Arduino.h>
#include <Wire.h>

#define QMI_ADDR 0x6B
#define QMI_WHO_AM_I 0x00
#define QMI_CTRL1 0x02
#define QMI_CTRL2 0x03
#define QMI_CTRL7 0x08
#define QMI_CTRL8 0x09
#define QMI_AX_L 0x35     // da qui sei byte: X, Y, Z a 16 bit
#define QMI_RST_RESULT 0x4D
#define QMI_RESET 0x60

static const float QMI_SCALA = 4.0f / 32768.0f;   // fondoscala 4g

static bool qmiWrite(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool qmiRead(uint8_t reg, uint8_t *buf, size_t len)
{
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)QMI_ADDR, (uint8_t)len) != len) return false;
    for (size_t i = 0; i < len; ++i) buf[i] = Wire.read();
    return true;
}

static bool imuBegin()
{
    qmiWrite(QMI_RESET, 0xB0);

    bool resetOk = false;
    uint32_t t0 = millis();
    while (millis() - t0 < 500)
    {
        delay(10);
        uint8_t r = 0;
        if (qmiRead(QMI_RST_RESULT, &r, 1) && r == 0x80) { resetOk = true; break; }
    }

    qmiWrite(QMI_CTRL1, 0x40);   // lettura dei tre assi in un colpo solo
    delay(10);

    uint8_t id = 0;
    if (!qmiRead(QMI_WHO_AM_I, &id, 1) || id != 0x05)
    {
        Serial.printf("[imu] QMI8658 assente o sconosciuto (0x%02X)\n", id);
        return false;
    }

    qmiWrite(QMI_CTRL8, 0x80);   // richiesto dal costruttore
    delay(10);
    qmiWrite(QMI_CTRL2, 0x16);   // 4g, 125 campioni al secondo
    delay(10);
    qmiWrite(QMI_CTRL7, 0x01);   // accende l'accelerometro
    delay(60);

    Serial.printf("[imu] QMI8658 pronto (reset %s)\n", resetOk ? "confermato" : "non confermato");
    return true;
}

static bool imuLeggi(float *ax, float *ay, float *az)
{
    uint8_t b[6];
    if (!qmiRead(QMI_AX_L, b, 6)) return false;
    *ax = (int16_t)(b[0] | (b[1] << 8)) * QMI_SCALA;
    *ay = (int16_t)(b[2] | (b[3] << 8)) * QMI_SCALA;
    *az = (int16_t)(b[4] | (b[5] << 8)) * QMI_SCALA;
    return true;
}

#endif // IMU_H
