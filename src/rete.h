/*
 * ============================================================
 *  LA RETE - a quali WiFi ci si aggancia, e come se ne impara una
 * ============================================================
 *
 *  Un orologio che esce di casa trova altre reti. Fin qui ne
 *  conosceva una sola, scritta in secrets.h: fuori di li' era
 *  cieco. Adesso ne conosce quante ne vuoi - quelle scritte in
 *  secrets.h e quelle imparate sul posto - e si aggancia alla piu'
 *  forte fra quelle che vede.
 *
 *  LA PASSWORD NON SI SCRIVE SULLO SCHERMO
 *  Lo schermo e' largo tre centimetri: una tastiera ci starebbe con
 *  tasti da tre millimetri, e una password WiFi ha venti caratteri
 *  fra maiuscole, minuscole e simboli. Si scrive invece dal
 *  telefono, che una tastiera ce l'ha. L'orologio apre per un
 *  momento una rete sua, "OROLOGIO"; il telefono ci si collega e
 *  gli si apre da solo una pagina con l'elenco delle reti viste e
 *  un campo per la password. Premuto "salva", l'orologio chiude la
 *  sua rete e si aggancia a quella nuova. E' quello che fanno le
 *  lampadine e le stampanti, ed e' l'unico modo che non chiede al
 *  dito di fare il lavoro di una tastiera.
 *
 *  La rete dell'orologio ha una password anche lei, scritta sullo
 *  schermo: la password di casa viaggia dal telefono all'orologio
 *  via radio, e su una rete aperta la leggerebbe chiunque passi.
 *
 *  QUELLO CHE SI IMPARA RESTA
 *  Le reti aggiunte dal telefono finiscono nella memoria del chip,
 *  come le sveglie. Al prossimo giro fuori casa l'hotspot lo
 *  riconosce da solo.
 *
 *  A CHI SI AGGANCIA
 *  All'accensione e ogni volta che resta senza rete: guarda che
 *  reti ci sono, e fra quelle che conosce prende la prima della
 *  lista che sia in vista. Non le prova una per una - ogni prova
 *  costa quindici secondi - e non si ostina sulla rete di casa
 *  quando casa e' a venti chilometri. Ma a casa resta a casa, anche
 *  se l'hotspot del telefono e' li' accanto e arriva piu' forte.
 * ============================================================
 */

#ifndef RETE_H
#define RETE_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

#define RETE_MAX_NOTE 12    // quante reti si ricordano
#define RETE_MAX_VISTE 16   // quante se ne elencano dopo una scansione

// La rete che l'orologio apre per farsi dire una password. Il nome e
// la password vanno scritti sullo schermo, che non ha le minuscole:
// per questo la password e' di sole cifre.
#define RETE_AP_NOME "OROLOGIO"
#define RETE_AP_PASS "12345678"

struct ReteNota
{
    char ssid[33];
    char pass[65];
};

struct ReteVista
{
    char ssid[33];
    int rssi;
    bool protetta;
};

static ReteNota reteNote[RETE_MAX_NOTE];
static int nReteNote = 0;
// Le prime della lista sono quelle di secrets.h: compilate dentro,
// non si salvano e non si dimenticano.
static int nReteScritte = 0;

static ReteVista reteViste[RETE_MAX_VISTE];
static int nReteViste = 0;
static bool reteScansioneInCorso = false;
static bool reteAgganciaDopoScansione = false;

// Cresce a ogni novita' - una scansione finita, un aggancio riuscito
// o fallito. Chi disegna guarda solo questo numero.
static volatile uint32_t reteVersione = 0;

// A quale rete si sta provando ad agganciarsi, e fino a quando si
// aspetta. Vuoto quando nessuno sta provando.
static char reteVoluta[33] = "";
static uint32_t reteTentativoFino = 0;
static char reteFallita[33] = "";   // l'ultima a cui non si e' riusciti

// L'ultima rete a cui ci si e' agganciati davvero: al risveglio si
// riprova quella, senza scansione, perche' quasi sempre e' ancora li'.
static char reteUltima[33] = "";

static Preferences prefsReti;

// ------------------------------------------------------------
//  LE RETI CONOSCIUTE
// ------------------------------------------------------------

static int reteIndiceNota(const char *ssid)
{
    if (!ssid || !ssid[0]) return -1;
    for (int i = 0; i < nReteNote; ++i)
        if (strcmp(reteNote[i].ssid, ssid) == 0) return i;
    return -1;
}

static void reteSalva()
{
    if (!prefsReti.begin("reti", false)) return;
    prefsReti.clear();

    int n = nReteNote - nReteScritte;
    prefsReti.putInt("n", n);
    for (int i = 0; i < n; ++i)
    {
        char chiave[8];
        snprintf(chiave, sizeof(chiave), "s%d", i);
        prefsReti.putString(chiave, reteNote[nReteScritte + i].ssid);
        snprintf(chiave, sizeof(chiave), "p%d", i);
        prefsReti.putString(chiave, reteNote[nReteScritte + i].pass);
    }
    prefsReti.putString("ultima", reteUltima);
    prefsReti.end();
}

static void reteAggiungiInMemoria(const char *ssid, const char *pass)
{
    if (nReteNote >= RETE_MAX_NOTE) return;
    strncpy(reteNote[nReteNote].ssid, ssid, 32);
    reteNote[nReteNote].ssid[32] = '\0';
    strncpy(reteNote[nReteNote].pass, pass ? pass : "", 64);
    reteNote[nReteNote].pass[64] = '\0';
    ++nReteNote;
}

static void reteCarica()
{
    nReteNote = 0;

#ifdef WIFI_RETI
    static const ReteNota scritte[] = {WIFI_RETI};
    for (size_t i = 0; i < sizeof(scritte) / sizeof(scritte[0]); ++i)
        if (scritte[i].ssid[0]) reteAggiungiInMemoria(scritte[i].ssid, scritte[i].pass);
#else
    if (strlen(WIFI_SSID) > 0) reteAggiungiInMemoria(WIFI_SSID, WIFI_PASSWORD);
#endif
    nReteScritte = nReteNote;

    // Sola lettura. La prima volta lo spazio non esiste e l'apertura
    // fallisce: lo si crea subito, come per le sveglie.
    if (!prefsReti.begin("reti", true))
    {
        if (prefsReti.begin("reti", false)) prefsReti.end();
        return;
    }

    int n = prefsReti.getInt("n", 0);
    for (int i = 0; i < n && nReteNote < RETE_MAX_NOTE; ++i)
    {
        char chiave[8];
        snprintf(chiave, sizeof(chiave), "s%d", i);
        String ssid = prefsReti.getString(chiave, "");
        snprintf(chiave, sizeof(chiave), "p%d", i);
        String pass = prefsReti.getString(chiave, "");
        if (ssid.length() == 0 || reteIndiceNota(ssid.c_str()) >= 0) continue;
        reteAggiungiInMemoria(ssid.c_str(), pass.c_str());
    }

    String ultima = prefsReti.getString("ultima", "");
    strncpy(reteUltima, ultima.c_str(), 32);
    reteUltima[32] = '\0';
    prefsReti.end();

    Serial.printf("[rete] conosco %d reti, %d da secrets.h\n", nReteNote, nReteScritte);
}

// Impara una rete, o aggiorna la password di una che gia' conosce.
// Se la lista e' piena si dimentica la piu' vecchia fra quelle
// imparate: quelle di secrets.h non si toccano.
static void reteRicorda(const char *ssid, const char *pass)
{
    int i = reteIndiceNota(ssid);
    if (i >= 0)
    {
        strncpy(reteNote[i].pass, pass ? pass : "", 64);
        reteNote[i].pass[64] = '\0';
    }
    else
    {
        if (nReteNote >= RETE_MAX_NOTE)
        {
            for (int k = nReteScritte; k + 1 < nReteNote; ++k)
                reteNote[k] = reteNote[k + 1];
            --nReteNote;
        }
        reteAggiungiInMemoria(ssid, pass);
    }
    reteSalva();
    Serial.printf("[rete] imparata \"%s\"\n", ssid);
}

// ------------------------------------------------------------
//  GUARDARSI INTORNO
// ------------------------------------------------------------
//  La scansione e' asincrona: dura un paio di secondi, e in quei
//  secondi le lancette devono continuare a girare. Si chiede, e poi
//  si passa a guardare se e' finita a ogni giro del programma.

static void reteRaccogliScansione(int n)
{
    nReteViste = 0;
    for (int i = 0; i < n; ++i)
    {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;

        int rssi = WiFi.RSSI(i);
        bool protetta = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

        // Lo stesso nome piu' volte e' la stessa rete su piu'
        // antenne: si tiene la piu' forte.
        int trovata = -1;
        for (int k = 0; k < nReteViste; ++k)
            if (strcmp(reteViste[k].ssid, ssid.c_str()) == 0) { trovata = k; break; }

        if (trovata >= 0)
        {
            if (rssi > reteViste[trovata].rssi) reteViste[trovata].rssi = rssi;
            continue;
        }
        if (nReteViste >= RETE_MAX_VISTE) continue;

        strncpy(reteViste[nReteViste].ssid, ssid.c_str(), 32);
        reteViste[nReteViste].ssid[32] = '\0';
        reteViste[nReteViste].rssi = rssi;
        reteViste[nReteViste].protetta = protetta;
        ++nReteViste;
    }
    WiFi.scanDelete();

    // Le piu' forti prima: sono quelle a portata davvero.
    for (int i = 1; i < nReteViste; ++i)
    {
        ReteVista v = reteViste[i];
        int k = i - 1;
        while (k >= 0 && reteViste[k].rssi < v.rssi) { reteViste[k + 1] = reteViste[k]; --k; }
        reteViste[k + 1] = v;
    }
}

static bool reteScansiona()
{
    if (reteScansioneInCorso) return true;
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);

    // Puo' dire di no se qualcun altro sta gia' guardando - la mappa,
    // per esempio, che chiede la posizione alle reti intorno. Si
    // riprovera' al prossimo giro.
    if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) return false;

    reteScansioneInCorso = true;
    return true;
}

// Fra le reti viste, la prima conosciuta nell'ordine della lista - non
// la piu' forte. L'ordine in secrets.h e' una preferenza: casa viene
// prima dell'hotspot, e se ci sono tutte e due si sta a casa. Con la
// regola della piu' forte, un telefono sul tavolo con l'hotspot acceso
// si portava via la board, che finiva su una rete dove il computer non
// la trova e a consumare i giga del telefono. Le reti imparate dal
// telefono stanno in coda, dopo quelle scritte.
static const char *reteMigliore()
{
    for (int k = 0; k < nReteNote; ++k)
        for (int i = 0; i < nReteViste; ++i)
            if (strcmp(reteViste[i].ssid, reteNote[k].ssid) == 0) return reteViste[i].ssid;
    return nullptr;
}

// ------------------------------------------------------------
//  AGGANCIARSI
// ------------------------------------------------------------

static bool reteAggancia(const char *ssid)
{
    int i = reteIndiceNota(ssid);
    if (i < 0) return false;

    strncpy(reteVoluta, ssid, 32);
    reteVoluta[32] = '\0';
    reteFallita[0] = '\0';
    reteTentativoFino = millis() + 20000UL;

    Serial.printf("[rete] mi aggancio a \"%s\"\n", ssid);

    // Se il portale e' aperto la radio sta facendo anche da
    // antenna: si resta in tutti e due i modi, o il telefono
    // verrebbe buttato fuori prima della risposta.
    if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    // Prima staccare: una begin() su una connessione a meta' non
    // riparte, si accoda e basta.
    WiFi.disconnect();
    WiFi.begin(reteNote[i].ssid, reteNote[i].pass);
    return true;
}

// L'ultima rete buona, senza guardarsi intorno: e' la mossa del
// risveglio, quando quasi sempre si e' ancora nello stesso posto.
static bool reteAgganciaUltima()
{
    if (reteUltima[0] && reteAggancia(reteUltima)) return true;
    if (nReteNote > 0) return reteAggancia(reteNote[0].ssid);
    return false;
}

// Guardarsi intorno e poi agganciarsi alla prima della lista fra le
// conosciute in vista. E' la mossa di chi e' rimasto senza rete e non sa dove
// si trova.
static void reteRitenta()
{
    if (reteScansiona()) reteAgganciaDopoScansione = true;
}

// ------------------------------------------------------------
//  IL PORTALE
// ------------------------------------------------------------
//  Una rete tutta sua, un DNS che risponde "sono io" a qualunque
//  nome, e una pagina sola. Il DNS bugiardo e' quello che fa aprire
//  la pagina da sola sul telefono: appena collegato, il telefono
//  chiede a un suo indirizzo "c'e' internet?", si vede rispondere da
//  noi, e capisce che c'e' qualcosa da compilare prima.

static WebServer *retePortale = nullptr;
static DNSServer *reteDns = nullptr;
static char retePortaleSsid[33] = "";           // la rete proposta nella pagina
static char retePortaleRicevutoSsid[33] = "";   // quella scelta dal telefono
static uint32_t retePortaleRicevutoA = 0;       // quando, 0 = ancora niente

static bool retePortaleAperto()
{
    return retePortale != nullptr;
}

static void reteHtmlEscape(String &out, const char *s)
{
    for (; *s; ++s)
    {
        switch (*s)
        {
        case '&': out += F("&amp;"); break;
        case '<': out += F("&lt;"); break;
        case '>': out += F("&gt;"); break;
        case '"': out += F("&quot;"); break;
        default: out += *s;
        }
    }
}

static void retePortalePagina()
{
    String h;
    h.reserve(3000);
    h += F("<!doctype html><html lang=\"it\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Orologio</title><style>"
           "body{font-family:-apple-system,system-ui,sans-serif;background:#000;color:#eee;"
           "margin:0;padding:24px;max-width:420px}"
           "h1{font-weight:300;letter-spacing:.3em;font-size:18px;color:#999}"
           "label{display:block;margin:18px 0 6px;font-size:13px;color:#888;letter-spacing:.1em}"
           "select,input{width:100%;box-sizing:border-box;font-size:17px;padding:12px;"
           "border:1px solid #333;border-radius:8px;background:#111;color:#fff}"
           "button{margin-top:28px;width:100%;font-size:17px;padding:14px;border:0;"
           "border-radius:8px;background:#e33;color:#fff}"
           "p{color:#888;font-size:14px;line-height:1.5}"
           "</style></head><body><h1>OROLOGIO</h1>"
           "<p>A quale rete mi collego?</p>"
           "<form method=\"post\" action=\"/salva\">"
           "<label>Rete</label><select name=\"ssid\">");

    bool proposta = false;
    for (int i = 0; i < nReteViste; ++i)
    {
        bool scelta = strcmp(reteViste[i].ssid, retePortaleSsid) == 0;
        proposta |= scelta;
        h += F("<option value=\"");
        reteHtmlEscape(h, reteViste[i].ssid);
        h += scelta ? F("\" selected>") : F("\">");
        reteHtmlEscape(h, reteViste[i].ssid);
        h += F("</option>");
    }
    if (retePortaleSsid[0] && !proposta)
    {
        h += F("<option value=\"");
        reteHtmlEscape(h, retePortaleSsid);
        h += F("\" selected>");
        reteHtmlEscape(h, retePortaleSsid);
        h += F("</option>");
    }
    h += F("</select>"
           "<label>Oppure scrivi il nome, se non &egrave; in elenco</label>"
           "<input name=\"altra\" autocapitalize=\"none\" autocorrect=\"off\">"
           "<label>Password</label>"
           "<input name=\"pass\" type=\"password\" autocomplete=\"off\">"
           "<button>Salva e collega</button></form>"
           "<p>Dopo il salvataggio la rete OROLOGIO sparisce. Se stai dando "
           "l'hotspot del telefono, riaccendilo: l'orologio lo cerca da solo.</p>"
           "</body></html>");

    retePortale->send(200, "text/html", h);
}

static void retePortaleSalva()
{
    String ssid = retePortale->arg("altra");
    ssid.trim();
    if (ssid.length() == 0) ssid = retePortale->arg("ssid");
    String pass = retePortale->arg("pass");

    if (ssid.length() == 0 || ssid.length() > 32)
    {
        retePortale->sendHeader("Location", "/", true);
        retePortale->send(302, "text/plain", "");
        return;
    }

    reteRicorda(ssid.c_str(), pass.c_str());
    strncpy(retePortaleRicevutoSsid, ssid.c_str(), 32);
    retePortaleRicevutoSsid[32] = '\0';
    retePortaleRicevutoA = millis();

    String h;
    h.reserve(600);
    h += F("<!doctype html><html lang=\"it\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Orologio</title><style>body{font-family:-apple-system,system-ui,sans-serif;"
           "background:#000;color:#eee;margin:0;padding:24px;max-width:420px}"
           "h1{font-weight:300;letter-spacing:.3em;font-size:18px;color:#999}"
           "p{color:#bbb;font-size:16px;line-height:1.5}</style></head><body>"
           "<h1>OROLOGIO</h1><p>Salvata. Adesso mi collego a <b>");
    reteHtmlEscape(h, ssid.c_str());
    h += F("</b>.</p><p>Puoi tornare alla tua rete: sullo schermo vedi come va.</p>"
           "</body></html>");
    retePortale->send(200, "text/html", h);
}

// Qualunque altro indirizzo rimanda alla pagina. E' cosi' che i
// telefoni si accorgono del portale.
static void retePortaleRimanda()
{
    retePortale->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    retePortale->send(302, "text/plain", "");
}

static void retePortaleApri(const char *ssid)
{
    if (retePortale) return;

    strncpy(retePortaleSsid, ssid ? ssid : "", 32);
    retePortaleSsid[32] = '\0';
    retePortaleRicevutoA = 0;
    retePortaleRicevutoSsid[0] = '\0';

    // Antenna e ricevitore insieme: se si era gia' agganciati a una
    // rete non la si perde, e il telefono trova la nostra.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(RETE_AP_NOME, RETE_AP_PASS);

    reteDns = new DNSServer();
    reteDns->start(53, "*", WiFi.softAPIP());

    // Sulla porta 80, la stessa dello specchio: i due non possono
    // stare su insieme, e lo specchio e' spento (SPECCHIO 0).
    retePortale = new WebServer(80);
    retePortale->on("/", HTTP_GET, retePortalePagina);
    retePortale->on("/salva", HTTP_POST, retePortaleSalva);
    retePortale->onNotFound(retePortaleRimanda);
    retePortale->begin();

    Serial.printf("[rete] portale aperto: rete %s, %s\n", RETE_AP_NOME,
                  WiFi.softAPIP().toString().c_str());
}

static void retePortaleChiudi()
{
    if (!retePortale) return;

    retePortale->stop();
    delete retePortale;
    retePortale = nullptr;

    reteDns->stop();
    delete reteDns;
    reteDns = nullptr;

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    retePortaleRicevutoA = 0;

    Serial.println("[rete] portale chiuso");
}

// ------------------------------------------------------------
//  A OGNI GIRO
// ------------------------------------------------------------

static void reteAggiorna()
{
    // La scansione, se e' finita.
    if (reteScansioneInCorso)
    {
        int n = WiFi.scanComplete();
        if (n != WIFI_SCAN_RUNNING)
        {
            reteScansioneInCorso = false;
            if (n >= 0)
            {
                reteRaccogliScansione(n);
                Serial.printf("[rete] viste %d reti\n", nReteViste);
            }
            ++reteVersione;

            if (reteAgganciaDopoScansione)
            {
                reteAgganciaDopoScansione = false;
                const char *s = reteMigliore();
                if (s) reteAggancia(s);
                else Serial.println("[rete] nessuna rete conosciuta in vista");
            }
        }
    }

    // Il portale, se e' aperto.
    if (retePortale)
    {
        reteDns->processNextRequest();
        retePortale->handleClient();

        // La risposta al telefono deve fare in tempo a partire prima
        // che la rete gli sparisca sotto i piedi.
        if (retePortaleRicevutoA && (millis() - retePortaleRicevutoA) > 1500)
        {
            char ssid[33];
            strcpy(ssid, retePortaleRicevutoSsid);
            retePortaleChiudi();
            reteAggancia(ssid);
        }
    }

    // Il tentativo in corso, se c'e'.
    if (reteVoluta[0])
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.printf("[rete] agganciato a \"%s\", ip %s\n", WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str());
            // Si segna la rete a cui si e' davvero agganciati, che puo'
            // non essere quella voluta: la radio puo' aver ripescato
            // da sola la precedente nel frattempo.
            String ssid = WiFi.SSID();
            if (ssid != reteUltima && reteIndiceNota(ssid.c_str()) >= 0)
            {
                strncpy(reteUltima, ssid.c_str(), 32);
                reteUltima[32] = '\0';
                reteSalva();
            }
            reteVoluta[0] = '\0';
            ++reteVersione;
        }
        else if ((int32_t)(millis() - reteTentativoFino) >= 0)
        {
            Serial.printf("[rete] \"%s\" non risponde\n", reteVoluta);
            strcpy(reteFallita, reteVoluta);
            reteVoluta[0] = '\0';
            ++reteVersione;
        }
    }
}

#endif
