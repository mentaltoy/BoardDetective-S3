/*
 * ============================================================
 *  IL NASTRO - registrare una nota e risentirla, anche a mano
 * ============================================================
 *
 *  Un registratore a nastro, o almeno la sua idea: si preme il
 *  punto rosso, si parla, si preme ancora. Poi si fa girare e si
 *  riascolta. E si puo' prendere la bobina con il dito e farla
 *  girare a mano, avanti e indietro, come si fa con un disco sul
 *  piatto: il suono segue il dito, lento e grave se lo muovi
 *  piano, al contrario se torni indietro.
 *
 *  DA DOVE ENTRA LA VOCE
 *  L'ES8311 non e' solo l'altoparlante: ha anche un ingresso, e
 *  sulla board c'e' un microfono MEMS analogico attaccato a
 *  quell'ingresso. I campioni tornano all'ESP32 sullo stesso bus I2S
 *  che porta il suono in uscita, su un filo che finora non usavamo -
 *  il GPIO 10. Il microfono e la parte analogica del codec vivono sul
 *  rail A3V3, che e' un regolatore dell'AXP2101 spento di serie:
 *  main.cpp lo accende prima del codec. Senza, il convertitore
 *  d'ingresso gira e consegna un silenzio digitale perfetto.
 *
 *  DOVE STA
 *  Sedici mila campioni al secondo, due byte l'uno: un minuto fa
 *  poco meno di due megabyte, e sta nella PSRAM. Per non perderlo
 *  allo spegnimento si scrive nella partizione dati, la stessa
 *  dove la mappa tiene le sue strade. Scriverlo costa qualche
 *  secondo, e lo fa il compito audio per conto suo dopo che hai
 *  smesso di registrare: lo schermo non si ferma.
 *
 *  LO SCRATCH
 *  Riascoltare vuol dire leggere il nastro un campione dopo
 *  l'altro. Farlo girare a mano vuol dire leggerlo alla velocita'
 *  del dito: la posizione nel nastro e' legata all'angolo della
 *  bobina, e ogni blocco di suono si legge da dov'era a dove il
 *  dito vuole che sia. Fra un campione e l'altro si interpola, o a
 *  velocita' bassa si sentirebbe il gradino. Al contrario si legge
 *  al contrario. E' tutto qui: e' quello che fa la puntina.
 *
 *  UN CORE SUO
 *  L'audio vuole essere alimentato a ritmo costante, e lo schermo
 *  vuole i suoi trentasei millisecondi per fotogramma. Sullo
 *  stesso core uno dei due aspetterebbe l'altro. Il nastro gira
 *  sul core zero, come la mappa e lo specchio; lo schermo gli dice
 *  solo cosa vuole - registra, suona, ferma, vai qui - e lui
 *  esegue al blocco successivo.
 * ============================================================
 */

#ifndef NASTRO_H
#define NASTRO_H

#include <Arduino.h>
#include <driver/i2s.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <math.h>

#define NASTRO_SECONDI 60
#define NASTRO_MAX ((uint32_t)NASTRO_SECONDI * AUDIO_RATE)   // campioni
#define NASTRO_FILE "/nota.pcm"

// Quanto nastro, in pixel di schermo, c'e' in tutto l'anello: lo
// misura il disegno la prima volta che lo traccia, e da qui in poi
// tutte le velocita' - bobine, motore, ruota - discendono da quel
// numero e dalla posizione, come su una macchina vera in cui il
// nastro tira tutto. Il dito sulla ruota sposta il nastro di quanto
// la ruota ha girato, e le bobine seguono.
static volatile float nastroAnelloPx = 950.0f;

// Quale dei due canali I2S porta il microfono. Il codec e' mono e
// mette il campione su uno solo dei due: l'altro e' zero.
#define NASTRO_CANALE 0

enum NastroStato : uint8_t
{
    NASTRO_FERMO = 0,
    NASTRO_REGISTRA,
    NASTRO_SUONA,
    NASTRO_SCRATCH   // la bobina e' in mano al dito
};

static int16_t *nastro = nullptr;
static volatile uint32_t nastroLunghezza = 0;   // campioni registrati
static volatile float nastroPosizione = 0;       // dove sta la testina, in campioni
static volatile float nastroBersaglio = 0;       // dove la vuole il dito
static volatile uint8_t nastroStato = NASTRO_FERMO;
static volatile uint8_t nastroChiesto = NASTRO_FERMO;   // cosa vuole lo schermo
static volatile uint32_t nastroChiestoA = 0;             // quando l'ha chiesto
static volatile float nastroLivello = 0;   // quanto e' forte il suono adesso, 0..1
static volatile float nastroLivelloAltro = 0;   // lo stesso sull'altro canale: serve a scoprire su quale sta il microfono
static volatile uint32_t nastroByteLetti = 0;    // diagnostica: quanti byte ha consegnato il ricevitore
static volatile uint32_t nastroErroriLettura = 0;
static volatile uint32_t nastroGiroMassimo = 0;   // diagnostica: il giro piu' lungo del compito mentre il nastro gira
static volatile int16_t nastroMin0 = 0, nastroMax0 = 0, nastroMin1 = 0, nastroMax1 = 0;   // grezzi, ultimo blocco
static volatile uint32_t nastroVersione = 0;   // cresce a ogni cambio di stato
static volatile bool nastroDaSalvare = false;
static volatile bool nastroSalvando = false;
static volatile bool nastroDaCancellare = false;
static bool nastroRiprendi = false;   // dopo lo scratch si torna a suonare?

// Il volume dell'ascolto, a tacche: si applica ai campioni prima di
// mandarli fuori, cosi' la sveglia non ne sa niente. Resta in memoria.
#define NASTRO_VOLUME_PASSI 10
static volatile int nastroVolume = NASTRO_VOLUME_PASSI / 2;
static Preferences prefsNastro;

static void nastroVolumeCarica()
{
    if (!prefsNastro.begin("nastro", true))
    {
        if (prefsNastro.begin("nastro", false)) prefsNastro.end();
        return;
    }
    nastroVolume = prefsNastro.getInt("vol2", NASTRO_VOLUME_PASSI / 2);
    prefsNastro.end();
    if (nastroVolume < 0) nastroVolume = 0;
    if (nastroVolume > NASTRO_VOLUME_PASSI) nastroVolume = NASTRO_VOLUME_PASSI;
}

static void nastroVolumeImposta(int passi)
{
    if (passi < 0) passi = 0;
    if (passi > NASTRO_VOLUME_PASSI) passi = NASTRO_VOLUME_PASSI;
    if (passi == nastroVolume) return;
    nastroVolume = passi;
    if (prefsNastro.begin("nastro", false))
    {
        prefsNastro.putInt("vol2", passi);
        prefsNastro.end();
    }
}

// Butta via la nota: la memoria, e il file. Lo fa il compito, da
// fermo, per non tirare via il nastro da sotto la testina.
static void nastroCancella()
{
    nastroDaCancellare = true;
}

static void nastroSalva()
{
    nastroSalvando = true;
    File f = LittleFS.open(NASTRO_FILE, "w");
    if (f)
    {
        const uint8_t *p = (const uint8_t *)nastro;
        size_t resto = nastroLunghezza * sizeof(int16_t);
        while (resto > 0)
        {
            size_t pezzo = resto > 8192 ? 8192 : resto;
            if (f.write(p, pezzo) != pezzo) break;
            p += pezzo;
            resto -= pezzo;
            vTaskDelay(1);   // un respiro alla mappa, che sta sullo stesso core
        }
        f.close();
        Serial.printf("[nastro] salvato: %lu s\n", (unsigned long)(nastroLunghezza / AUDIO_RATE));
    }
    else
        Serial.println("[nastro] non riesco a scrivere il file");
    nastroSalvando = false;
}

static void nastroCarica()
{
    if (!LittleFS.exists(NASTRO_FILE)) return;
    File f = LittleFS.open(NASTRO_FILE, "r");
    if (!f) return;

    size_t byte = f.size();
    if (byte > NASTRO_MAX * sizeof(int16_t)) byte = NASTRO_MAX * sizeof(int16_t);
    size_t letti = f.read((uint8_t *)nastro, byte);
    f.close();

    nastroLunghezza = letti / sizeof(int16_t);
    nastroPosizione = 0;
    ++nastroVersione;
    Serial.printf("[nastro] dalla flash: %lu s\n", (unsigned long)(nastroLunghezza / AUDIO_RATE));
}

// Appena finita una registrazione la si porta a un livello pieno: si
// cerca il picco e si alza tutto finche' il picco non sta al
// novantacinque per cento del fondo scala. Al massimo otto volte - una nota di solo
// fruscio non deve diventare un ruggito di fruscio. E' quello che fa
// un tecnico del suono prima di consegnare, e qui costa un
// centesimo di secondo.
static void nastroNormalizza()
{
    if (nastroLunghezza == 0) return;

    int32_t picco = 0;
    for (uint32_t i = 0; i < nastroLunghezza; ++i)
    {
        int32_t v = nastro[i] < 0 ? -(int32_t)nastro[i] : (int32_t)nastro[i];
        if (v > picco) picco = v;
    }
    if (picco == 0) return;

    float guadagno = 0.95f * 32767.0f / (float)picco;
    if (guadagno > 8.0f) guadagno = 8.0f;
    if (guadagno < 1.0f) guadagno = 1.0f;

    // La porta antirumore: si misura il nastro a blocchi di sedici
    // millesimi, e i blocchi che stanno sotto un quarantesimo del
    // picco sono fruscio fra una parola e l'altra. Non si azzerano -
    // un silenzio assoluto suona come un buco - si abbassano a un
    // quarto.
    //
    // La porta si apre subito e si chiude piano: la chiusura scende
    // di un quinto a blocco, un centinaio di millesimi per arrivare
    // in fondo, e per riaprirsi vuole un livello piu' alto di quello
    // a cui si e' chiusa. Senza queste due cose, sui suoni deboli -
    // una consonante, un respiro - apriva e chiudeva a ogni blocco, e
    // sotto la voce si sentiva un ricchettio.
    const uint32_t blocco = AUDIO_BLOCCO;
    const uint32_t nBlocchi = (nastroLunghezza + blocco - 1) / blocco;
    const float sogliaApre = (float)picco / 25.0f;
    const float sogliaChiude = (float)picco / 50.0f;
    const float chiusa = 0.25f;
    uint32_t chiusi = 0;

    bool aperta = true;
    float fattore = 1.0f;
    for (uint32_t b = 0; b < nBlocchi; ++b)
    {
        uint32_t da = b * blocco;
        uint32_t a = da + blocco;
        if (a > nastroLunghezza) a = nastroLunghezza;

        float energia = 0;
        for (uint32_t i = da; i < a; ++i) energia += (float)nastro[i] * (float)nastro[i];
        float rms = sqrtf(energia / (float)(a - da));
        if (aperta && rms < sogliaChiude) aperta = false;
        else if (!aperta && rms > sogliaApre) aperta = true;

        float bersaglio = aperta ? 1.0f : chiusa;
        float nuovo = (bersaglio > fattore) ? bersaglio : fattore + (bersaglio - fattore) * 0.2f;
        if (!aperta) ++chiusi;

        for (uint32_t i = da; i < a; ++i)
        {
            float f = fattore + (nuovo - fattore) * (float)(i - da) / (float)blocco;
            nastro[i] = (int16_t)((float)nastro[i] * guadagno * f);
        }
        fattore = nuovo;
    }

    Serial.printf("[nastro] normalizzato: picco %ld, guadagno x%.1f, porta chiusa su %lu blocchi di %lu\n",
                  (long)picco, guadagno, (unsigned long)chiusi, (unsigned long)nBlocchi);
}

// Il passaggio da uno stato all'altro. Lo fa solo il compito, fra un
// blocco e l'altro: cosi' nessuno cambia le carte mentre un blocco
// sta ancora uscendo.
static void nastroApplica(uint8_t nuovo)
{
    uint8_t vecchio = nastroStato;

    if ((nuovo == NASTRO_SUONA || nuovo == NASTRO_SCRATCH) && nastroLunghezza == 0)
        nuovo = NASTRO_FERMO;

    switch (nuovo)
    {
    case NASTRO_REGISTRA:
        digitalWrite(AUDIO_PA, LOW);   // l'altoparlante tace: si ascolta
        nastroLunghezza = 0;
        nastroPosizione = 0;
        break;

    case NASTRO_SUONA:
        // Si riparte da dove sta la testina, anche se e' sul nastro
        // vuoto: il giro la riporta alla nota da solo. Riportarla
        // all'inizio sarebbe comodo ma non naturale, e dopo aver
        // girato la ruota a mano il nastro deve restare dove l'hai
        // lasciato.
        i2s_zero_dma_buffer(I2S_NUM_0);
        digitalWrite(AUDIO_PA, HIGH);
        break;

    case NASTRO_SCRATCH:
        // Il bersaglio l'ha gia' messo il dito, in nastroScratchInizia:
        // qui non si tocca, o si perderebbe il primo pezzo di gesto.
        digitalWrite(AUDIO_PA, HIGH);
        break;

    default:   // fermo
        digitalWrite(AUDIO_PA, LOW);
        i2s_zero_dma_buffer(I2S_NUM_0);
        nastroLivello = 0;
        if (vecchio == NASTRO_REGISTRA)
        {
            nastroNormalizza();
            nastroDaSalvare = true;
        }
        break;
    }

    nastroStato = nuovo;
    nastroChiesto = nuovo;
    ++nastroVersione;
}

static void nastroTask(void *)
{
    nastroCarica();

    static int16_t blocco[AUDIO_BLOCCO * 2];

    for (;;)
    {
        // Diagnostica: un giro del compito che dura troppo si vede sul
        // seriale, con lo stato in cui era.
        static uint32_t giroPrecedente = 0;
        uint32_t adesso = millis();
        uint32_t giro = giroPrecedente ? adesso - giroPrecedente : 0;
        if (giro > 200)
            Serial.printf("[nastro] giro lento: %lu ms nello stato %u\n",
                          (unsigned long)giro, (unsigned)nastroStato);
        // Mentre il nastro gira, un giro piu' lungo del buffer del DMA
        // vuol dire che l'uscita e' rimasta a secco: si sente un buco,
        // o un colpo.
        if (nastroStato != NASTRO_FERMO && giro > nastroGiroMassimo) nastroGiroMassimo = giro;
        giroPrecedente = adesso;

        if (nastroChiesto != nastroStato)
        {
            Serial.printf("[nastro] stato %u -> %u dopo %lu ms\n", (unsigned)nastroStato,
                          (unsigned)nastroChiesto, (unsigned long)(millis() - nastroChiestoA));
            nastroApplica(nastroChiesto);
        }

        switch (nastroStato)
        {
        case NASTRO_REGISTRA:
        {
            size_t letti = 0;
            esp_err_t esito = i2s_read(I2S_NUM_0, blocco, sizeof(blocco), &letti, pdMS_TO_TICKS(100));
            size_t n = letti / (2 * sizeof(int16_t));
            nastroByteLetti += letti;
            if (esito != ESP_OK) nastroErroriLettura++;

            float energia = 0, energiaAltro = 0;
            for (size_t i = 0; i < n && nastroLunghezza < NASTRO_MAX; ++i)
            {
                int16_t c = blocco[i * 2 + NASTRO_CANALE];
                int16_t altro = blocco[i * 2 + 1 - NASTRO_CANALE];
                nastro[nastroLunghezza] = c;
                nastroLunghezza = nastroLunghezza + 1;
                energia += (float)c * (float)c;
                energiaAltro += (float)altro * (float)altro;
            }
            if (n > 0)
            {
                nastroLivello = sqrtf(energia / (float)n) / 32768.0f;
                nastroLivelloAltro = sqrtf(energiaAltro / (float)n) / 32768.0f;
            }
            nastroPosizione = (float)nastroLunghezza;

            if (nastroLunghezza >= NASTRO_MAX) nastroChiesto = NASTRO_FERMO;   // nastro finito
            break;
        }

        case NASTRO_SUONA:
        case NASTRO_SCRATCH:
        {
            // Il nastro e' un anello lungo quanto tutto lo spazio, e la
            // nota ne occupa una parte. Suonando, la testina va sempre
            // alla stessa velocita': legge la nota, poi il nastro
            // vuoto in silenzio, e ritrova l'inizio. Il giro continua
            // finche' non lo fermi.
            float velocita = 1.0f;
            if (nastroStato == NASTRO_SCRATCH)
            {
                // Da dov'e' a dove vuole il dito, spalmato sul blocco.
                // Non piu' di quattro volte la velocita' normale: oltre
                // non si sente piu' niente di riconoscibile, e il dito
                // non e' cosi' preciso da volerlo davvero. L'anello e'
                // chiuso: la strada e' la piu' corta delle due.
                float scarto = nastroBersaglio - nastroPosizione;
                if (scarto > (float)NASTRO_MAX / 2) scarto -= (float)NASTRO_MAX;
                if (scarto < -(float)NASTRO_MAX / 2) scarto += (float)NASTRO_MAX;

                // Il nastro non resta mai indietro rispetto al dito piu'
                // di un blocco a tutta velocita': se il dito e' andato
                // piu' in la', il pezzo in mezzo si salta. Prima
                // l'anticipo si accumulava, e quando il dito tornava
                // indietro il nastro finiva di inseguire il vecchio
                // bersaglio prima di girarsi: mezzo secondo di ritardo
                // al cambio di verso.
                const float massimo = 6.0f;
                const float limite = massimo * (float)AUDIO_BLOCCO;
                if (scarto > limite)
                {
                    nastroPosizione = nastroBersaglio - limite;
                    scarto = limite;
                }
                else if (scarto < -limite)
                {
                    nastroPosizione = nastroBersaglio + limite;
                    scarto = -limite;
                }
                velocita = scarto / (float)AUDIO_BLOCCO;
                if (fabsf(velocita) < 0.02f) velocita = 0.0f;
            }

            float pos = nastroPosizione;
            const float fine = (float)nastroLunghezza - 1.0f;
            float energia = 0;
            // A meta' scala la nota esce com'e', a fondo scala; sopra si
            // spinge fino al doppio, e un limitatore morbido piega le
            // punte invece di tagliarle: piu' forte, non piu' sporco.
            const float volume = (float)nastroVolume / (float)(NASTRO_VOLUME_PASSI / 2);

            for (int i = 0; i < AUDIO_BLOCCO; ++i)
            {
                int16_t c = 0;
                if (velocita != 0.0f && pos >= 0.0f && pos < fine)
                {
                    uint32_t i0 = (uint32_t)pos;
                    float f = pos - (float)i0;
                    float v = ((float)nastro[i0] * (1.0f - f) + (float)nastro[i0 + 1] * f) * volume / 32767.0f;
                    float m = fabsf(v);
                    if (m > 0.7f) m = 0.7f + 0.3f * tanhf((m - 0.7f) / 0.3f);
                    c = (int16_t)((v < 0 ? -m : m) * 32767.0f);
                }
                blocco[i * 2] = c;
                blocco[i * 2 + 1] = c;
                energia += (float)c * (float)c;

                pos += velocita;
                // L'anello si chiude, in tutti e due i versi.
                if (pos >= (float)NASTRO_MAX) pos -= (float)NASTRO_MAX;
                if (pos < 0.0f) pos += (float)NASTRO_MAX;
            }
            nastroPosizione = pos;
            nastroLivello = sqrtf(energia / (float)AUDIO_BLOCCO) / 32768.0f;

            size_t scritti = 0;
            i2s_write(I2S_NUM_0, blocco, sizeof(blocco), &scritti, portMAX_DELAY);

            // Il ricevitore intanto continua a riempirsi: si svuota
            // senza aspettarlo, o alla prossima registrazione i primi
            // blocchi sarebbero di adesso invece che di allora.
            {
                static int16_t scarto[AUDIO_BLOCCO * 2];
                size_t letti = 0;
                i2s_read(I2S_NUM_0, scarto, sizeof(scarto), &letti, 0);
            }

            break;
        }

        default:
            if (nastroDaCancellare)
            {
                nastroDaCancellare = false;
                nastroDaSalvare = false;
                nastroLunghezza = 0;
                nastroPosizione = 0;
                LittleFS.remove(NASTRO_FILE);
                ++nastroVersione;
                Serial.println("[nastro] cancellato");
            }
            if (nastroDaSalvare)
            {
                nastroDaSalvare = false;
                nastroSalva();
            }
            // Anche da fermo si continua a leggere il ricevitore e a
            // buttare via: cosi' la coda del DMA non si intasa mai, e
            // quando si registra i primi campioni sono di adesso.
            // Costa niente - la lettura aspetta il DMA, non gira a
            // vuoto - e in cambio il livello del microfono e' vivo
            // sempre, che serve a chi vuole vedere se sente.
            {
                size_t letti = 0;
                if (i2s_read(I2S_NUM_0, blocco, sizeof(blocco), &letti, pdMS_TO_TICKS(100)) == ESP_OK &&
                    letti > 0)
                {
                    size_t n = letti / (2 * sizeof(int16_t));
                    float energia = 0, energiaAltro = 0;
                    int16_t mn0 = 32767, mx0 = -32768, mn1 = 32767, mx1 = -32768;
                    for (size_t i = 0; i < n; ++i)
                    {
                        int16_t c0 = blocco[i * 2], c1 = blocco[i * 2 + 1];
                        if (c0 < mn0) mn0 = c0; if (c0 > mx0) mx0 = c0;
                        if (c1 < mn1) mn1 = c1; if (c1 > mx1) mx1 = c1;
                        float c = blocco[i * 2 + NASTRO_CANALE];
                        float altro = blocco[i * 2 + 1 - NASTRO_CANALE];
                        energia += c * c;
                        energiaAltro += altro * altro;
                    }
                    nastroMin0 = mn0; nastroMax0 = mx0; nastroMin1 = mn1; nastroMax1 = mx1;
                    nastroLivello = sqrtf(energia / (float)n) / 32768.0f;
                    nastroLivelloAltro = sqrtf(energiaAltro / (float)n) / 32768.0f;
                    nastroByteLetti += letti;
                }
                else
                    vTaskDelay(pdMS_TO_TICKS(20));
            }
            break;
        }
    }
}

// ------------------------------------------------------------
//  QUELLO CHE LO SCHERMO PUO' CHIEDERE
// ------------------------------------------------------------

static void nastroChiedi(uint8_t stato)
{
    nastroChiestoA = millis();
    nastroChiesto = stato;
}

static bool nastroInMoto()
{
    return nastroStato != NASTRO_FERMO || nastroChiesto != NASTRO_FERMO;
}

// Il dito ha preso la bobina.
static void nastroScratchInizia()
{
    nastroRiprendi = (nastroStato == NASTRO_SUONA);
    nastroBersaglio = nastroPosizione;
    nastroChiedi(NASTRO_SCRATCH);
}

// Il dito ha spostato il nastro di tanti pixel di schermo (con
// segno): e' la ruota che ha girato, per il suo raggio. Il nastro e'
// un anello, e tutto l'anello e' a portata di dito, vuoto compreso.
static void nastroScratchMuoviPixel(float pixel)
{
    float b = nastroBersaglio + pixel * (float)NASTRO_MAX / nastroAnelloPx;
    while (b >= (float)NASTRO_MAX) b -= (float)NASTRO_MAX;
    while (b < 0.0f) b += (float)NASTRO_MAX;
    nastroBersaglio = b;
}

// Il dito ha lasciato: si riprende da dove si e' arrivati, o si
// resta fermi li' - e il prossimo avvio parte da quel punto.
static void nastroScratchFine()
{
    nastroChiedi(nastroRiprendi ? NASTRO_SUONA : NASTRO_FERMO);
}

// Quanto nastro e' passato, in pixel di schermo: da qui il disegno
// ricava di quanto ha girato ogni cosa che il nastro tocca.
static float nastroSpostamentoPx()
{
    return nastroPosizione * nastroAnelloPx / (float)NASTRO_MAX;
}

static void nastroBegin()
{
    if (!audioPronto) return;

    nastro = (int16_t *)ps_malloc(NASTRO_MAX * sizeof(int16_t));
    if (!nastro)
    {
        Serial.println("[nastro] niente PSRAM per il nastro");
        return;
    }

    // La partizione dati si monta qui, nel setup, prima che il
    // compito parta: cosi' non se la contende con la mappa, che la
    // monta anche lei e la trova gia' pronta.
    LittleFS.begin(true);
    nastroVolumeCarica();

    xTaskCreatePinnedToCore(nastroTask, "nastro", 8192, nullptr, 2, nullptr, 0);
    Serial.printf("[nastro] pronto: fino a %d secondi\n", NASTRO_SECONDI);
}

#endif
