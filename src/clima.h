/*
 * ============================================================
 *  IL CONDIZIONATORE
 * ============================================================
 *
 *  L'adattatore wifi Daikin risponde in rete locale, senza
 *  password e senza passare da nessun servizio esterno: si chiede
 *  un indirizzo e risponde con una riga di testo.
 *
 *      ret=OK,pow=1,mode=3,stemp=25.0,...
 *
 *  Niente JSON, niente chiavi: e' piu' semplice del meteo.
 *
 *  UNA TRAPPOLA
 *  Per cambiare qualcosa non si manda solo quello che cambia: si
 *  rimandano tutti i parametri insieme, compresi quelli che
 *  restano uguali. Mandare la sola temperatura azzererebbe
 *  modalita' e ventola, perche' il condizionatore prende quello
 *  che riceve come lo stato completo e nuovo. Per questo prima si
 *  legge, poi si modifica il pezzo che interessa, poi si rimanda
 *  tutto.
 * ============================================================
 */

#ifndef CLIMA_H
#define CLIMA_H

#include <Arduino.h>
#include <HTTPClient.h>

#define CLIMA_IP "192.168.1.2"
#define CLIMA_NOME "SALONE"

struct Clima
{
    bool valido = false;
    bool acceso = false;
    int modo = 3;          // 3 = raffredda, 4 = riscalda, 2 = deumidifica, 6 = ventola
    float impostata = 25;  // quella che chiedi
    float interna = 0;     // quella che c'e' in casa
    float esterna = 0;
    String ventola = "A";
    String direzione = "0";
    uint32_t ultimoTentativo = 0;
};

static Clima clima;

// Estrae un campo dalla riga di risposta.
static String climaCampo(const String &riga, const char *nome)
{
    String chiave = String(nome) + "=";
    int i = riga.indexOf("," + chiave);
    if (i >= 0) i += 1;
    else if (riga.startsWith(chiave)) i = 0;
    else return String();

    int inizio = i + chiave.length();
    int fine = riga.indexOf(',', inizio);
    if (fine < 0) fine = riga.length();
    return riga.substring(inizio, fine);
}

static bool climaChiama(const char *percorso, String &risposta)
{
    HTTPClient http;
    http.setTimeout(3000);
    http.setConnectTimeout(2000);

    String url = String("http://") + CLIMA_IP + percorso;
    if (!http.begin(url)) return false;

    int codice = http.GET();
    if (codice != 200)
    {
        http.end();
        return false;
    }
    risposta = http.getString();
    http.end();
    return risposta.startsWith("ret=OK");
}

static bool climaLeggi()
{
    String r;
    if (!climaChiama("/aircon/get_control_info", r))
    {
        clima.valido = false;
        return false;
    }

    clima.acceso = (climaCampo(r, "pow") == "1");
    clima.modo = climaCampo(r, "mode").toInt();
    clima.impostata = climaCampo(r, "stemp").toFloat();
    clima.ventola = climaCampo(r, "f_rate");
    clima.direzione = climaCampo(r, "f_dir");

    if (climaChiama("/aircon/get_sensor_info", r))
    {
        clima.interna = climaCampo(r, "htemp").toFloat();
        clima.esterna = climaCampo(r, "otemp").toFloat();
    }

    clima.valido = true;
    return true;
}

// Le velocita' che la macchina accetta, dalla piu' piana alla piu'
// forte. "A" e' automatica e sta in fondo perche' non e' un livello:
// e' la macchina che sceglie da sola.
static const char *CLIMA_VELOCITA[] = {"B", "3", "4", "5", "6", "7", "A"};
#define CLIMA_N_VELOCITA 7

static int climaIndiceVelocita()
{
    for (int i = 0; i < CLIMA_N_VELOCITA; ++i)
        if (clima.ventola == CLIMA_VELOCITA[i]) return i;
    return CLIMA_N_VELOCITA - 1;
}

static const char *climaNomeVelocita(const String &f)
{
    if (f == "A") return "AUTOMATICA";
    if (f == "B") return "SILENZIOSA";
    static char b[16];
    snprintf(b, sizeof(b), "VELOCITA %d", f.toInt() - 2);   // 3..7 diventa 1..5
    return b;
}

// Quanti millesimi di secondo per un giro di pala. Piu' forte va, piu'
// in fretta gira: e' l'unica cosa che risponde subito all'occhio, e
// vederla accelerare e' la prova che il comando e' arrivato.
static uint32_t climaGiroMs()
{
    if (clima.ventola == "B") return 11000;
    if (clima.ventola == "A") return 6500;
    switch (clima.ventola.toInt())
    {
    case 3: return 9000;
    case 4: return 7000;
    case 5: return 5200;
    case 6: return 3800;
    case 7: return 2600;
    }
    return 6500;
}

// Rimanda tutto lo stato con dentro le modifiche richieste.
static bool climaComanda(bool acceso, float gradi, const String &ventola, int modo)
{
    if (gradi < 16) gradi = 16;
    if (gradi > 32) gradi = 32;

    char url[160];
    snprintf(url, sizeof(url),
             "/aircon/set_control_info?pow=%d&mode=%d&stemp=%.1f&shum=0&f_rate=%s&f_dir=%s",
             acceso ? 1 : 0, modo, gradi,
             ventola.c_str(), clima.direzione.c_str());

    String r;
    if (!climaChiama(url, r)) return false;

    // Si aggiorna subito quello che si vede, senza aspettare la
    // rilettura: il comando e' andato a buon fine, e vedere il numero
    // cambiare nell'istante in cui tocchi e' meta' della sensazione
    // che il comando funzioni.
    clima.acceso = acceso;
    clima.impostata = gradi;
    clima.ventola = ventola;
    clima.modo = modo;
    return true;
}

// Le modalita' in un giro: freddo, caldo, deumidifica, ventola,
// automatico. Toccando la scritta si passa alla successiva.
static const int CLIMA_MODI[] = {3, 4, 2, 6, 1};
#define CLIMA_N_MODI 5

static int climaProssimoModo()
{
    for (int i = 0; i < CLIMA_N_MODI; ++i)
        if (CLIMA_MODI[i] == clima.modo)
            return CLIMA_MODI[(i + 1) % CLIMA_N_MODI];
    return CLIMA_MODI[0];
}

static const char *climaModo(int m)
{
    switch (m)
    {
    case 2: return "DEUMIDIFICA";
    case 3: return "RAFFREDDA";
    case 4: return "RISCALDA";
    case 6: return "VENTOLA";
    default: return "AUTOMATICO";
    }
}

#endif // CLIMA_H
