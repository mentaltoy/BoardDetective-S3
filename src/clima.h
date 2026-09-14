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
#include <WiFiUdp.h>

struct Clima
{
    Clima(const char *etichetta) : nome(etichetta) { ip[0] = '\0'; }

    // L'indirizzo lo si scopre in rete, per nome: non si scrive. Il
    // router li ridistribuisce quando gli pare, e un giorno il
    // condizionatore del salone era diventato il bridge delle
    // lampadine.
    char ip[16];
    const char *nome;

    bool valido = false;
    bool acceso = false;
    int modo = 3;          // 3 = raffredda, 4 = riscalda, 2 = deumidifica, 6 = ventola
    float impostata = 25;  // quella che chiedi
    float interna = 0;     // quella che c'e' in casa
    float esterna = 0;
    String ventola = "A";
    String direzione = "0";

    // Lo stato della pala, che vive fra un disegno e l'altro.
    float angolo = 0;
    float velocita = 0;
    uint32_t ultimoGiro = 0;

    // Alzata quando c'e' un comando da spedire. Chi tocca cambia lo
    // stato qui e alza questa; a spedirlo davvero ci pensa il core
    // della rete, quando puo'.
    volatile bool daInviare = false;
};

// Le macchine di casa, con il nome che hanno nell'app Daikin: si
// confrontano le iniziali, senza badare alle maiuscole, quindi
// "CAMERA" trova "Camera da letto". Aggiungerne una e' una riga.
#define N_CLIMI 2
static Clima climi[N_CLIMI] = {
    {"SALONE"},
    {"CAMERA"},
};

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

// I nomi arrivano con le lettere strane in %XX: "Salone" e' scritto
// %53%61%6c%6f%6e%65. Si riportano a lettere.
static String climaDecodifica(const String &s)
{
    String out;
    for (unsigned i = 0; i < s.length(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.length())
        {
            out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
            i += 2;
        }
        else out += s[i];
    }
    return out;
}

static bool climaNomeCorrisponde(const char *nostro, const String &loro)
{
    size_t n = strlen(nostro);
    if (loro.length() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (toupper((unsigned char)nostro[i]) != toupper((unsigned char)loro[i])) return false;
    return true;
}

// Chi c'e'? Si grida in broadcast sulla porta degli adattatori Daikin
// e ognuno risponde con la sua scheda, nome compreso, dal suo
// indirizzo. Un secondo e mezzo di ascolto basta a tutti. Torna
// quanti dei nostri ha trovato.
static int climaScopri()
{
    IPAddress mio = WiFi.localIP(), maschera = WiFi.subnetMask(), broadcast;
    for (int i = 0; i < 4; ++i) broadcast[i] = mio[i] | ~maschera[i];

    WiFiUDP udp;
    if (!udp.begin(30000)) return 0;

    const char *domanda = "DAIKIN_UDP/common/basic_info";
    udp.beginPacket(broadcast, 30050);
    udp.write((const uint8_t *)domanda, strlen(domanda));
    udp.endPacket();

    int trovati = 0;
    uint32_t fine = millis() + 1500;
    while ((int32_t)(millis() - fine) < 0)
    {
        int n = udp.parsePacket();
        if (n <= 0) { delay(20); continue; }

        char buf[512];
        int letti = udp.read(buf, sizeof(buf) - 1);
        buf[letti > 0 ? letti : 0] = '\0';
        String risposta(buf);
        if (!risposta.startsWith("ret=OK")) continue;

        String nome = climaDecodifica(climaCampo(risposta, "name"));
        String da = udp.remoteIP().toString();
        for (int i = 0; i < N_CLIMI; ++i)
            if (climaNomeCorrisponde(climi[i].nome, nome))
            {
                strncpy(climi[i].ip, da.c_str(), sizeof(climi[i].ip) - 1);
                climi[i].ip[sizeof(climi[i].ip) - 1] = '\0';
                ++trovati;
                Serial.printf("[clima] %s trovato a %s (\"%s\")\n", climi[i].nome, climi[i].ip, nome.c_str());
            }
    }
    udp.stop();
    if (trovati < N_CLIMI) Serial.printf("[clima] trovati %d condizionatori su %d\n", trovati, N_CLIMI);
    return trovati;
}

static bool climaChiama(const Clima &c, const char *percorso, String &risposta)
{
    if (!c.ip[0]) return false;   // non ancora trovato in rete

    HTTPClient http;
    http.setTimeout(3000);
    http.setConnectTimeout(2000);

    String url = String("http://") + c.ip + percorso;
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

static bool climaLeggi(Clima &clima)
{
    String r;
    if (!climaChiama(clima, "/aircon/get_control_info", r))
    {
        clima.valido = false;
        return false;
    }

    clima.acceso = (climaCampo(r, "pow") == "1");
    clima.modo = climaCampo(r, "mode").toInt();
    clima.impostata = climaCampo(r, "stemp").toFloat();
    clima.ventola = climaCampo(r, "f_rate");
    clima.direzione = climaCampo(r, "f_dir");

    if (climaChiama(clima, "/aircon/get_sensor_info", r))
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

static int climaIndiceVelocita(const Clima &clima)
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
static uint32_t climaGiroMs(const Clima &clima)
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

// Il tocco cambia subito quello che si vede e lascia detto che c'e'
// da spedire. Non aspetta la macchina.
//
// E' una scelta, non una scorciatoia: la risposta arriva in qualche
// decimo di secondo, e in quel tempo un'interfaccia che non reagisce
// sembra rotta - si finisce per premere due volte. Mostrare subito il
// risultato e mandarlo per davvero un istante dopo e' il modo in cui
// si comportano tutte le cose che sembrano immediate. Se la macchina
// non recepisse, la rilettura periodica rimetterebbe le cose a posto
// da sola entro venti secondi.
static void climaComanda(Clima &clima, bool acceso, float gradi,
                         const String &ventola, int modo)
{
    if (gradi < 16) gradi = 16;
    if (gradi > 32) gradi = 32;

    clima.acceso = acceso;
    clima.impostata = gradi;
    clima.ventola = ventola;
    clima.modo = modo;
    clima.daInviare = true;
}

// Spedisce davvero lo stato: la chiama il core della rete.
static bool climaSpedisci(Clima &clima)
{
    char url[160];
    snprintf(url, sizeof(url),
             "/aircon/set_control_info?pow=%d&mode=%d&stemp=%.1f&shum=0&f_rate=%s&f_dir=%s",
             clima.acceso ? 1 : 0, clima.modo, clima.impostata,
             clima.ventola.c_str(), clima.direzione.c_str());

    String r;
    return climaChiama(clima, url, r);
}

// Le modalita' in un giro: freddo, caldo, deumidifica, ventola,
// automatico. Toccando la scritta si passa alla successiva.
static const int CLIMA_MODI[] = {3, 4, 2, 6, 1};
#define CLIMA_N_MODI 5

static int climaProssimoModo(const Clima &clima)
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
