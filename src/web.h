/*
 * ============================================================
 *  LO SPECCHIO - vedere lo schermo dal browser
 * ============================================================
 *
 *  La board mette su un piccolo sito. Apri l'indirizzo dal
 *  telefono o dal computer e vedi quello che sta sul display in
 *  quel momento, mentre succede.
 *
 *  COSA VIAGGIA SULLA RETE
 *  Non un'immagine: i pixel, contati. Lo schermo e' quasi tutto
 *  nero e usa sette colori in croce, quindi invece di spedire
 *  centosessantacinquemila valori si spedisce "questo colore, per
 *  questo numero di volte". Un fotogramma passa da 322 KB a circa
 *  20: quindici volte meno, e comprimere costa una sola passata
 *  sui dati.
 *
 *  Un vero compressore farebbe molto meglio - zlib scende a 3 KB -
 *  ma vorrebbe centinaia di millisecondi di calcolo a fotogramma,
 *  e su questo chip si perderebbe piu' tempo di quanto se ne
 *  risparmia sul cavo. A rimettere insieme i pixel ci pensa il
 *  browser, che di lavoro fa quello.
 *
 *  UN FLUSSO SOLO, NON UNA RICHIESTA PER FOTOGRAMMA
 *  Aprire un collegamento costa quanto spedire il fotogramma
 *  stesso: saluti, risposta, chiusura, e si ricomincia. Con i dati
 *  ridotti a 20 KB quell'attesa diventava la parte piu' lenta di
 *  tutte. Cosi' il collegamento si apre una volta e resta aperto, e
 *  i fotogrammi arrivano uno dietro l'altro con davanti la loro
 *  lunghezza, che e' come si separano due messaggi su un flusso che
 *  non finisce mai.
 *
 *  PERCHE' STA SU UN CORE SUO
 *  Spedire 322 KB su WiFi prende un decimo di secondo. Se il
 *  programma principale si fermasse ad aspettare, ogni volta che
 *  guardi la pagina le animazioni sull'oggetto vero andrebbero a
 *  scatti - e lo specchio avrebbe rotto proprio la cosa che
 *  doveva mostrare. L'ESP32-S3 ha due core e il programma ne usa
 *  uno solo: il sito vive sull'altro.
 *
 *  Che ogni tanto il fotogramma spedito sia mezzo vecchio e mezzo
 *  nuovo non e' un problema: e' uno specchio, non un backup.
 * ============================================================
 */

#ifndef WEB_H
#define WEB_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

static WebServer webServer(80);
static uint16_t *webFramebuffer = nullptr;
static int16_t webLarghezza = 0, webAltezza = 0;
static uint8_t *webBuffer = nullptr;
static size_t webBufferMax = 0;

// Cambia ogni volta che il programma manda qualcosa a schermo. Lo
// specchio se lo segna: se non e' cambiato, il fotogramma e' identico
// a quello di prima e rispedirlo sarebbe banda buttata via, oltre a
// togliere tempo a quello nuovo che intanto potrebbe arrivare.
volatile uint32_t webVersione = 0;

// Scrive "quante volte, quale colore" per ogni tratto di pixel
// uguali. Torna quanti byte ha prodotto, oppure zero se il risultato
// non ci sta nel buffer - cosa che succederebbe solo con
// un'immagine senza due pixel uguali di fila, che qui non esiste.
static size_t webComprimi(const uint16_t *fb, size_t quanti,
                          uint8_t *out, size_t massimo)
{
    size_t o = 0, i = 0;
    while (i < quanti)
    {
        uint16_t v = fb[i];
        size_t n = 1;
        while (i + n < quanti && fb[i + n] == v && n < 65535) ++n;

        if (o + 4 > massimo) return 0;
        out[o++] = (uint8_t)(n & 0xFF);
        out[o++] = (uint8_t)(n >> 8);
        out[o++] = (uint8_t)(v & 0xFF);
        out[o++] = (uint8_t)(v >> 8);
        i += n;
    }
    return o;
}

// Il flusso: resta aperto e spedisce un fotogramma dietro l'altro,
// ognuno preceduto dalla propria lunghezza.
static void webStream()
{
    WiFiClient client = webServer.client();
    client.print(F("HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Cache-Control: no-store\r\n"
                   "Connection: close\r\n\r\n"));

    const size_t pixel = (size_t)webLarghezza * webAltezza;

    uint32_t vistaVersione = 0xFFFFFFFF;

    while (client.connected())
    {
        // Si aspetta che ci sia qualcosa di nuovo da mostrare.
        if (webVersione == vistaVersione)
        {
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }
        vistaVersione = webVersione;

        size_t n = webComprimi(webFramebuffer, pixel, webBuffer, webBufferMax);
        if (n == 0) break;

        uint8_t testa[4] = {
            (uint8_t)(n & 0xFF), (uint8_t)((n >> 8) & 0xFF),
            (uint8_t)((n >> 16) & 0xFF), (uint8_t)((n >> 24) & 0xFF)
        };
        if (client.write(testa, 4) != 4) break;

        size_t rimasti = n;
        const uint8_t *p = webBuffer;
        while (rimasti > 0 && client.connected())
        {
            size_t pezzo = rimasti > 4096 ? 4096 : rimasti;
            size_t scritti = client.write(p, pezzo);
            if (scritti == 0) break;
            p += scritti;
            rimasti -= scritti;
        }
        if (rimasti > 0) break;

        vTaskDelay(1);   // un respiro al resto del sistema, non una pausa
    }
    client.stop();
}

static const char WEB_PAGINA[] PROGMEM = R"PAGINA(<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dot Clock</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body {
    margin: 0; min-height: 100vh; background: #0a0a0a; color: #6a6a6a;
    display: flex; flex-direction: column; align-items: center;
    justify-content: center; gap: 18px; padding: 24px;
    font: 12px ui-monospace, SFMono-Regular, Menlo, monospace;
    letter-spacing: .18em; text-transform: uppercase;
  }
  .guscio {
    padding: 14px; border-radius: 58px; background: #161616;
    box-shadow: 0 20px 60px rgba(0,0,0,.7), inset 0 1px 0 rgba(255,255,255,.06);
  }
  canvas {
    display: block; border-radius: 46px; background: #000;
    width: min(368px, 78vw); height: auto; image-rendering: pixelated;
  }
  .riga { display: flex; gap: 20px; align-items: center; }
  b { color: #d8d8d8; font-weight: 500; }
  .spia { width: 7px; height: 7px; border-radius: 50%; background: #E8261A; }
  .spia.giu { background: #3a3a3a; }
</style>
</head>
<body>
  <div class="guscio"><canvas id="schermo" width="368" height="448"></canvas></div>
  <div class="riga">
    <span class="spia" id="spia"></span>
    <span><b id="fps">--</b> al secondo</span>
    <span><b id="kb">--</b> kb/s</span>
    <span><b id="stato">collegamento</b></span>
  </div>
<script>
const cv = document.getElementById('schermo');
const cx = cv.getContext('2d');
const img = cx.createImageData(368, 448);

// Si scrive un pixel per volta come numero da quattro byte invece
// che come quattro byte separati: e' la stessa memoria vista in un
// altro modo, ma cosi' un intero tratto di pixel uguali si riempie
// con una chiamata sola invece che con un ciclo.
const pix = new Uint32Array(img.data.buffer);

// Da 565 a colori veri: cinque bit di rosso, sei di verde, cinque di
// blu, riportati su 0-255. Le tabelle evitano di rifare la stessa
// divisione a ogni pixel.
const R = new Uint8Array(32), G = new Uint8Array(64);
for (let i = 0; i < 32; i++) R[i] = Math.round(i * 255 / 31);
for (let i = 0; i < 64; i++) G[i] = Math.round(i * 255 / 63);

const eFps = document.getElementById('fps');
const eStato = document.getElementById('stato');
const eSpia = document.getElementById('spia');
const eKb = document.getElementById('kb');

let conta = 0, ultimo = performance.now(), byteConta = 0;

function disegna(d) {
  let o = 0;
  for (let i = 0; i + 3 < d.length; i += 4) {
    const n = d[i] | (d[i + 1] << 8);
    const v = d[i + 2] | (d[i + 3] << 8);
    // Il colore va messo insieme al contrario - blu, verde, rosso -
    // perche' e' cosi' che i quattro byte finiscono in memoria su
    // queste macchine.
    const c = 0xff000000 | (R[v & 31] << 16) | (G[(v >> 5) & 63] << 8) | R[(v >> 11) & 31];
    pix.fill(c, o, o + n);
    o += n;
  }
  cx.putImageData(img, 0, 0);

  conta++;
  byteConta += d.length;
  const adesso = performance.now();
  if (adesso - ultimo >= 1000) {
    eFps.textContent = conta;
    eKb.textContent = Math.round(byteConta / 1024);
    conta = 0; byteConta = 0; ultimo = adesso;
  }
}

async function collega() {
  const res = await fetch('/stream', { cache: 'no-store' });
  if (!res.ok) throw new Error(res.status);

  eStato.textContent = 'in diretta';
  eSpia.classList.remove('giu');

  const lettore = res.body.getReader();
  let coda = new Uint8Array(0);

  for (;;) {
    const { done, value } = await lettore.read();
    if (done) break;

    const unito = new Uint8Array(coda.length + value.length);
    unito.set(coda); unito.set(value, coda.length);
    coda = unito;

    // Ogni fotogramma ha davanti la propria lunghezza: si estrae solo
    // quando e' arrivato tutto, perche' i pezzi in cui la rete lo
    // spezza non hanno niente a che vedere con dove finisce.
    for (;;) {
      if (coda.length < 4) break;
      const len = coda[0] | (coda[1] << 8) | (coda[2] << 16) | (coda[3] << 24);
      if (coda.length < 4 + len) break;
      disegna(coda.subarray(4, 4 + len));
      coda = coda.slice(4 + len);
    }
  }
}

async function giro() {
  for (;;) {
    try {
      await collega();
    } catch (e) { /* caduta: si riprova */ }
    eStato.textContent = 'board non raggiungibile';
    eSpia.classList.add('giu');
    eFps.textContent = '--';
    await new Promise(f => setTimeout(f, 1200));
  }
}
giro();
</script>
</body>
</html>)PAGINA";

static void webRadice()
{
    webServer.send_P(200, "text/html", WEB_PAGINA);
}

static void webFrame()
{
    if (!webFramebuffer)
    {
        webServer.send(503, "text/plain", "schermo non pronto");
        return;
    }

    const size_t quanti = (size_t)webLarghezza * webAltezza * 2;

    webServer.setContentLength(quanti);
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/octet-stream", "");

    // A blocchi, non in un colpo solo: la scheda di rete ha un
    // buffer di pochi KB e scriverci dentro 322 KB tutti insieme
    // significherebbe soltanto aspettare piu' a lungo nello stesso
    // punto.
    WiFiClient client = webServer.client();
    const uint8_t *p = (const uint8_t *)webFramebuffer;
    size_t rimasti = quanti;
    while (rimasti > 0 && client.connected())
    {
        size_t pezzo = rimasti > 2048 ? 2048 : rimasti;
        size_t scritti = client.write(p, pezzo);
        if (scritti == 0) break;
        p += scritti;
        rimasti -= scritti;
    }
}

// Il sito gira qui dentro, per sempre, su un core suo.
static void webTask(void *)
{
    webServer.on("/", webRadice);
    webServer.on("/fb", webFrame);
    webServer.on("/stream", webStream);
    webServer.onNotFound([]() { webServer.send(404, "text/plain", "non c'e'"); });
    webServer.begin();

    for (;;)
    {
        webServer.handleClient();
        vTaskDelay(1);   // lascia respirare il resto del sistema
    }
}

static void webBegin(uint16_t *framebuffer, int16_t larghezza, int16_t altezza)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[web] niente rete: lo specchio resta spento");
        return;
    }

    webFramebuffer = framebuffer;
    webLarghezza = larghezza;
    webAltezza = altezza;

    // Sta largo: nel caso peggiore i tratti sono molti, e meglio
    // sprecare un po' di PSRAM che dover rinunciare a un fotogramma.
    webBufferMax = 160 * 1024;
    webBuffer = (uint8_t *)ps_malloc(webBufferMax);
    if (!webBuffer)
    {
        Serial.println("[web] memoria insufficiente per lo specchio");
        return;
    }

    // Core 0: il programma Arduino gira sull'1, e restano separati.
    xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);

    Serial.printf("[web] specchio acceso: http://%s/\n", WiFi.localIP().toString().c_str());
}

#endif // WEB_H
