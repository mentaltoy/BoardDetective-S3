/*
 * ============================================================
 *  ES8311 - far suonare la sveglia
 * ============================================================
 *
 *  Sulla board ci sono due chip distinti che devono mettersi
 *  d'accordo, e capire la divisione dei compiti e' meta' del
 *  lavoro:
 *
 *  L'ESP32 produce i numeri. Un suono digitale e' una sequenza di
 *  valori: l'altezza dell'onda, misurata sedicimila volte al
 *  secondo. Quei numeri viaggiano su un bus dedicato, l'I2S, che
 *  serve solo a questo e li consegna a ritmo costante - se
 *  arrivassero a scatti si sentirebbero i buchi.
 *
 *  L'ES8311 li trasforma in tensione. E' un convertitore
 *  digitale-analogico con un amplificatore attaccato: prende i
 *  numeri e muove il cono dell'altoparlante di conseguenza.
 *
 *  Ma prima va configurato, e la configurazione passa da un bus
 *  completamente diverso, l'I2C, quello dove stanno gia' touch,
 *  batteria e orologio. E' la stessa divisione che c'e' in uno
 *  studio di registrazione fra i cavi del segnale e le manopole
 *  del mixer: due cose separate, e le manopole vanno girate prima.
 *
 *  I NUMERI DELLA CONFIGURAZIONE
 *  Il codec deve sapere a che velocita' arrivano i campioni. Non
 *  glielo diciamo in hertz: gli diciamo in che rapporto stanno i
 *  segnali di sincronismo che gli manderemo. Da qui i divisori nei
 *  registri 0x02-0x08, che messi insieme dicono "ogni giro di
 *  clock principale vale un duecentocinquantaseiesimo di campione".
 * ============================================================
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <math.h>

#define ES8311_ADDR 0x18

#define AUDIO_MCLK 16
#define AUDIO_BCLK 9
#define AUDIO_WS 45
#define AUDIO_DOUT 8    // dall'ESP32 al codec
#define AUDIO_DIN 10    // dal codec all'ESP32: il microfono
#define AUDIO_PA 46     // accende l'amplificatore di potenza

#define AUDIO_RATE 16000
#define AUDIO_BLOCCO 256   // campioni per volta

static bool audioPronto = false;

static void esScrivi(uint8_t reg, uint8_t valore)
{
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(valore);
    Wire.endTransmission();
}

static uint8_t esLeggi(uint8_t reg)
{
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) != 1) return 0;
    return Wire.read();
}

// Solo i registri del codec, senza toccare il bus dei campioni. Serve
// al risveglio: il codec resta senza corrente e dimentica tutto, ma
// il bus e' del processore e non si e' mosso - reinstallarlo darebbe
// errore.
// L'ingresso: il microfono analogico sul primo ingresso, con il
// preamplificatore a trenta decibel - un microfono MEMS da' pochi
// millivolt - piu' diciotto di scala del convertitore, e il filtro
// che toglie il soffio sotto i cento hertz. Sono i valori dell'esempio
// del costruttore per questa board. Si scrivono sia all'avvio sia
// quando il codec viene riconfigurato da capo.
//
// Tutto questo non serve a niente se il rail analogico A3V3 e' spento:
// e' l'uscita ALDO1 del gestore di alimentazione, e lo accende main.cpp
// prima di chiamare audioBegin.
//
// Niente controllo automatico di livello: provato, alza il fondo di
// rumore di quaranta volte per tenere il silenzio al livello del
// parlato. Il livello lo si aggiusta dopo, sulla registrazione
// intera, dove si sa qual e' il picco e cosa e' rumore (nastro.h).
static void audioMicrofono()
{
    esScrivi(0x14, 0x1A);   // ingresso MIC1, preamplificatore a +30 dB
    esScrivi(0x16, 0x04);   // scala del convertitore: +24 dB
    esScrivi(0x17, 0xC8);   // volume digitale dell'ingresso: +4,5 dB
    esScrivi(0x18, 0x00);   // controllo automatico spento
    esScrivi(0x1B, 0x0A);   // filtro passa-alto
    esScrivi(0x1C, 0x6A);
}

static void audioRiconfigura()
{
    esScrivi(0x00, 0x1F);
    delay(20);
    esScrivi(0x00, 0x00);
    esScrivi(0x00, 0x80);
    esScrivi(0x01, 0x3F);
    esScrivi(0x02, 0x00);
    esScrivi(0x03, 0x10);
    esScrivi(0x04, 0x10);
    esScrivi(0x05, 0x00);
    esScrivi(0x06, 0x03);
    esScrivi(0x07, 0x00);
    esScrivi(0x08, 0xFF);
    esScrivi(0x09, 0x0C);
    esScrivi(0x0A, 0x0C);
    esScrivi(0x0D, 0x01);
    esScrivi(0x0E, 0x02);
    esScrivi(0x12, 0x00);
    esScrivi(0x13, 0x10);
    esScrivi(0x32, 0xC0);
    esScrivi(0x37, 0x08);
    audioMicrofono();
}

static bool audioBegin()
{
    // Il chip si presenta con due registri di sola lettura che
    // contengono il suo stesso nome scritto in esadecimale: 0x83 e
    // 0x11 sono "8311". Se non risponde cosi', tutto il resto della
    // configurazione andrebbe a vuoto senza dirci perche'.
    uint8_t id1 = esLeggi(0xFD);
    uint8_t id2 = esLeggi(0xFE);
    if (id1 != 0x83 || id2 != 0x11)
    {
        Serial.printf("[audio] ES8311 non riconosciuto (0x%02X 0x%02X)\n", id1, id2);
        return false;
    }

    // Spegnimento e riaccensione: il codec puo' essere rimasto in
    // uno stato qualunque da prima del riavvio.
    esScrivi(0x00, 0x1F);
    delay(20);
    esScrivi(0x00, 0x00);
    esScrivi(0x00, 0x80);   // fuori dal reset, il clock lo comanda l'ESP32

    esScrivi(0x01, 0x3F);   // accende i clock interni

    // I divisori per 16000 campioni al secondo con il clock
    // principale a 256 volte tanto.
    esScrivi(0x02, 0x00);
    esScrivi(0x03, 0x10);
    esScrivi(0x04, 0x10);
    esScrivi(0x05, 0x00);
    esScrivi(0x06, 0x03);
    esScrivi(0x07, 0x00);
    esScrivi(0x08, 0xFF);

    // Formato dei dati: I2S standard, campioni da 16 bit.
    esScrivi(0x09, 0x0C);
    esScrivi(0x0A, 0x0C);

    esScrivi(0x0D, 0x01);   // alimentazione della parte digitale
    esScrivi(0x0E, 0x02);   // alimentazione della parte analogica
    esScrivi(0x12, 0x00);   // accende il convertitore
    esScrivi(0x13, 0x10);   // manda l'uscita all'amplificatore
    esScrivi(0x32, 0xC0);   // volume del convertitore
    esScrivi(0x37, 0x08);   // quanto e' morbida la rampa del volume
    audioMicrofono();

    i2s_config_t cfg = {};
    // In tutti e due i versi: il suono esce e la voce entra sullo
    // stesso bus, con lo stesso orologio.
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    cfg.sample_rate = AUDIO_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 6;
    cfg.dma_buf_len = AUDIO_BLOCCO;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK)
    {
        Serial.println("[audio] driver I2S non installato");
        return false;
    }

    i2s_pin_config_t pin = {};
    pin.mck_io_num = AUDIO_MCLK;
    pin.bck_io_num = AUDIO_BCLK;
    pin.ws_io_num = AUDIO_WS;
    pin.data_out_num = AUDIO_DOUT;
    pin.data_in_num = AUDIO_DIN;

    if (i2s_set_pin(I2S_NUM_0, &pin) != ESP_OK)
    {
        Serial.println("[audio] piedini I2S rifiutati");
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);

    pinMode(AUDIO_PA, OUTPUT);
    digitalWrite(AUDIO_PA, LOW);   // amplificatore spento finche' non serve

    audioPronto = true;
    Serial.println("[audio] ES8311 pronto");
    return true;
}

// Il convertitore acceso a vuoto consuma qualche milliampere: di
// notte, quando non deve suonare niente, tanto vale spegnerlo.
static void audioRiposo()
{
    if (!audioPronto) return;
    digitalWrite(AUDIO_PA, LOW);
    esScrivi(0x12, 0x02);   // convertitore giu'
    esScrivi(0x0E, 0x00);   // parte analogica giu'
}

static void audioRisveglio()
{
    if (!audioPronto) return;
    esScrivi(0x0E, 0x02);
    esScrivi(0x12, 0x00);
}

// ------------------------------------------------------------
//  IL TONO DELLA SVEGLIA
// ------------------------------------------------------------
//  Tre bip corti e una pausa, che si ripetono. Il motivo per cui
//  quasi tutte le sveglie fanno cosi' non e' pigrizia: un suono
//  continuo il cervello lo impara in pochi secondi e lo mette
//  sotto la soglia dell'attenzione, mentre un ritmo che riparte
//  costringe a riaccorgersene ogni volta.
//
//  L'onda e' una sinusoide, non un'onda quadra. La quadra e' piu'
//  facile da generare ma contiene tutte le armoniche dispari
//  insieme, ed e' per questo che i cicalini di una volta erano
//  cosi' sgradevoli. Ogni bip sale e scende di volume ai bordi:
//  senza quella smussatura si sentirebbe un "toc" a ogni attacco,
//  perche' l'altoparlante verrebbe strappato dalla sua posizione
//  di riposo.

#define BIP_DURATA 130      // millisecondi di suono
#define BIP_PAUSA 90        // silenzio fra un bip e l'altro
#define BIP_QUANTI 3        // bip per gruppo
#define BIP_RIPOSO 900      // silenzio fra un gruppo e il successivo
#define BIP_FREQUENZA 880.0f
#define BIP_RAMPA 6         // millisecondi di salita e discesa

static float audioFase = 0;
static uint32_t audioPosizione = 0;   // a che punto siamo del ritmo, in campioni

static void audioAvvia()
{
    if (!audioPronto) return;
    audioFase = 0;
    audioPosizione = 0;
    i2s_zero_dma_buffer(I2S_NUM_0);
    digitalWrite(AUDIO_PA, HIGH);
}

static void audioFerma()
{
    if (!audioPronto) return;
    digitalWrite(AUDIO_PA, LOW);
}

// Da chiamare spesso mentre suona: prepara il pezzo successivo e lo
// consegna al bus. Non aspetta mai: se il buffer e' ancora pieno
// torna subito, perche' bloccarsi qui congelerebbe l'animazione.
static void audioAggiorna()
{
    if (!audioPronto) return;

    const uint32_t ciclo = ((BIP_DURATA + BIP_PAUSA) * BIP_QUANTI + BIP_RIPOSO)
                           * AUDIO_RATE / 1000;
    const uint32_t durataBip = BIP_DURATA * AUDIO_RATE / 1000;
    const uint32_t passoBip = (BIP_DURATA + BIP_PAUSA) * AUDIO_RATE / 1000;
    const uint32_t rampa = BIP_RAMPA * AUDIO_RATE / 1000;

    static int16_t blocco[AUDIO_BLOCCO * 2];

    for (int i = 0; i < AUDIO_BLOCCO; ++i)
    {
        uint32_t dentro = audioPosizione % ciclo;
        int quale = dentro / passoBip;
        uint32_t da = dentro - quale * passoBip;

        float ampiezza = 0;
        if (quale < BIP_QUANTI && da < durataBip)
        {
            ampiezza = 1.0f;
            if (da < rampa)
                ampiezza = (float)da / (float)rampa;
            else if (da > durataBip - rampa)
                ampiezza = (float)(durataBip - da) / (float)rampa;
        }

        int16_t campione = (int16_t)(sinf(audioFase) * 9000.0f * ampiezza);
        blocco[i * 2] = campione;
        blocco[i * 2 + 1] = campione;

        audioFase += 2.0f * (float)M_PI * BIP_FREQUENZA / (float)AUDIO_RATE;
        if (audioFase > 2.0f * (float)M_PI) audioFase -= 2.0f * (float)M_PI;
        ++audioPosizione;
    }

    size_t scritti = 0;
    i2s_write(I2S_NUM_0, blocco, sizeof(blocco), &scritti, 0);

    // Se il bus non ha accettato tutto il blocco, i campioni avanzati
    // non sono stati suonati: si riporta indietro il conto, altrimenti
    // il ritmo scivolerebbe in avanti a ogni giro.
    size_t campioniScritti = scritti / (2 * sizeof(int16_t));
    uint32_t rifiutati = AUDIO_BLOCCO - campioniScritti;
    audioPosizione -= rifiutati;

    // Anche la fase dell'onda va riportata indietro, non solo il
    // conto. Altrimenti al giro successivo la sinusoide ripartirebbe
    // da un punto diverso da dove si era interrotta, e ogni salto di
    // fase si sente come un click.
    audioFase -= rifiutati * 2.0f * (float)M_PI * BIP_FREQUENZA / (float)AUDIO_RATE;
    while (audioFase < 0) audioFase += 2.0f * (float)M_PI;
}

#endif // AUDIO_H
