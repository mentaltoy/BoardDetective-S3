# Come si usa questo progetto (PlatformIO su VS Code)

Guida per la primissima volta. Nessuna conoscenza data per scontata.

Se hai clonato il repository con `git clone`, la cartella che ti serve è già
sul disco e puoi saltare direttamente al **Passo 1**.

---

## Come è fatto un progetto PlatformIO

PlatformIO non ragiona a "file singolo" come l'IDE Arduino: ragiona a
**cartelle**. Una cartella con dentro un file `platformio.ini` *è* un
progetto. Questa:

```
BoardDetective-S3/          <- la cartella del progetto
├── platformio.ini          <- la "scheda tecnica": che board, che velocità
└── src/
    └── main.cpp            <- il programma vero e proprio
```

Due file, due ruoli distinti:

- **`platformio.ini`** non è codice. Dice a PlatformIO *quale chip* stai
  usando e come parlarci. È il posto dove nell'IDE Arduino avresti usato il
  menu "Strumenti".
- **`src/main.cpp`** è il programma. Se hai già visto degli sketch Arduino,
  è la stessa identica cosa: c'è un `setup()` che gira una volta all'avvio e
  un `loop()` che gira all'infinito. Cambia solo l'estensione (`.cpp` invece
  di `.ino`), perché PlatformIO usa direttamente il C++ senza le scorciatoie
  dell'IDE Arduino.

Non spostare `main.cpp` fuori da `src/`: PlatformIO compila **solo** quello
che trova lì dentro.

---

## Passo 1 — Aprire il progetto

In VS Code: `File → Apri cartella…` e scegli la cartella **`BoardDetective-S3`**
(la cartella intera, non il file `main.cpp`).

Se hai scelto quella giusta, in basso a sinistra compare una barra con delle
icone: ✓, →, 🔌, 🗑. Sono i pulsanti di PlatformIO. Se non le vedi, hai
aperto la cartella sbagliata: PlatformIO si attiva solo quando trova
`platformio.ini` nella radice.

---

## Passo 2 — La prima compilazione (mettiti comodo)

Clicca il **✓** in basso a sinistra (è "Build", compila senza caricare).

La prima volta PlatformIO scarica il compilatore per ESP32 e tutte le
librerie: **1–2 GB**, dai 5 ai 20 minuti a seconda della connessione. Nel
terminale scorrerà un fiume di testo, è normale. Succede una sola volta: dal
secondo progetto in poi è già tutto lì.

Alla fine devi leggere `SUCCESS` in verde.

Se leggi `SUCCESS`, il codice è a posto. La board non l'hai ancora toccata.

---

## Passo 3 — Collegare la board e trovarla

Collega la board al Mac con un **cavo USB dati**, non uno da sola ricarica.
Sono identici a vedersi: se hai dubbi, usa quello di un telefono che sai fare
anche trasferimento file.

Questa board ha una sola presa USB-C, cablata all'USB nativo del chip. Per
verificare che il Mac la veda, apri un terminale in VS Code
(`Terminale → Nuovo terminale`) e scrivi:

```bash
pio device list
```

Deve comparire una riga tipo `/dev/cu.usbmodem14201`. Il prefisso `usbmodem`
conferma che stai parlando con l'USB nativo (un convertitore seriale esterno
si chiamerebbe `usbserial` o `wchusbserial`). Non serve alcun driver.

Non scrivere la porta in `platformio.ini`: PlatformIO la trova da sola.

---

## Passo 4 — Caricare il programma

Clicca la **→** (freccia destra) in basso: compila *e* carica sulla board.

Anche qui aspetti `SUCCESS`. Alla fine la board si riavvia da sola e il
display si accende con il riepilogo.

Se leggi **`Failed to connect to ESP32-S3`**, la board non è entrata in
modalità caricamento. Questa board **non ha un pulsante RESET**: ha solo BOOT
(in alto) e PWR (in basso). Quindi:

1. tieni premuto **BOOT**
2. stacca e riattacca il cavo USB
3. rilascia **BOOT**
4. riclicca la freccia

---

## Passo 5 — Parlare con la board

Clicca l'icona **🔌** (spina) in basso: apre il monitor seriale.

Il display si è già acceso da solo, ma il monitor ti dà il rapporto completo.
Digita una lettera del menu (`i`, `s`, `p`, `w`) e premi invio.

Se il monitor si apre vuoto, è normale: la board si era già riavviata e ha
stampato l'intestazione prima che tu aprissi la finestra. Digita `i` e te la
ristampa.

Per chiudere il monitor: `Ctrl+C` nel terminale. **Chiudilo sempre prima di
ricaricare**, altrimenti la porta risulta occupata e l'upload fallisce con
`Could not open port`.

---

## Cosa fanno i comandi del menu

| Tasto | Cosa fa |
|---|---|
| `i` | Rapporto completo: chip, flash, PSRAM, MAC, motivo dell'ultimo riavvio |
| `p` | Scrive e rilegge 64 KB di PSRAM per verificare che sia configurata bene |
| `s` | Elenca i chip collegati al bus I2C e li identifica per nome |
| `w` | Scansiona le reti WiFi: verifica che radio e antenna funzionino |
| `d` | Ridisegna il riepilogo sul display AMOLED |
| `b` | Riavvia la board |

## Se qualcosa va storto

**Il monitor seriale non stampa niente**

Servono le due righe `ARDUINO_USB_CDC_ON_BOOT=1` e `ARDUINO_USB_MODE=1` nei
`build_flags`. Senza, il programma gira ma manda `Serial` sui pin fisici
della UART invece che sull'USB. È il problema numero uno di questa board.

**Errore di compilazione su `Arduino_ESP32RGBPanel.cpp`**

Lancia la build una seconda volta: alla prima passata lo script di patch gira
prima che la libreria sia stata scaricata. Vedi il README per il dettaglio.

**La board si riavvia in continuazione, o il display resta nero**

Controlla `board_build.arduino.memory_type = qio_opi` e `-D BOARD_HAS_PSRAM`.
Senza PSRAM il buffer del display non ci sta in memoria.

**`Could not open port` durante l'upload**

Monitor seriale ancora aperto. `Ctrl+C`.

**La porta sparisce dopo il caricamento**

Normale con l'USB nativo: il chip si riavvia e ricrea l'interfaccia USB.
Riapri il monitor dopo un paio di secondi.

---

## Prima di collegare qualsiasi cosa ai pin

- **GPIO 26–37: mai usarli.** Sono cablati alla memoria flash e alla PSRAM.
  Se ci colleghi qualcosa il chip va in crash immediato.
- **GPIO 0, 3, 45, 46** decidono come si avvia il chip. Puoi usarli, ma se
  qualcosa li tiene alti o bassi all'accensione la board non parte.
- **GPIO 19 e 20** sono i dati dell'USB nativo.
- **Tutto funziona a 3.3 V.** Nessun pin sopporta i 5 V. Un sensore
  alimentato a 5 V che risponde con 5 V sul pin dati può bruciare il chip:
  alimentalo a 3V3, o usa un convertitore di livello.
- Per il resto sull'ESP32 i pin sono rimappabili: puoi decidere tu quali usare
  per I2C, SPI o seriale. Non sei vincolato come su un Arduino Uno.

---

## Il passo successivo

Il display funziona, quindi da qui puoi andare in qualsiasi direzione. Il
disegno passa tutto per l'oggetto `gfx`:

```cpp
gfx->fillScreen(RGB565_BLACK);
gfx->setTextSize(3);
gfx->setTextColor(RGB565_CYAN);
gfx->setCursor(20, 100);
gfx->println("Ciao!");

gfx->fillCircle(184, 300, 60, RGB565_RED);
gfx->drawLine(0, 0, 368, 448, RGB565_WHITE);
gfx->setBrightness(120);   // 0-255
```

Le direzioni naturali da esplorare, in ordine di difficoltà:

1. **Touch** (FT3168 sul bus I2C) — leggere dove tocchi lo schermo
2. **IMU** (QMI8658) — inclinazione e movimento, per ruotare l'immagine
3. **RTC** (PCF85063) — un orologio che sopravvive allo spegnimento
4. **Batteria** (AXP2101) — leggere carica e consumo
5. **Audio** (ES8311) — microfono e altoparlante
6. **LVGL** — l'interfaccia grafica vera, con widget e animazioni

Gli esempi ufficiali per ognuna di queste sono nel
[repository Waveshare](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8),
sotto `examples/arduino/examples/`.

Consiglio: non sovrascrivere questo progetto. Duplica la cartella e lavora
sulla copia — `BoardDetective` ti tornerà utile ogni volta che compri una
board nuova.
