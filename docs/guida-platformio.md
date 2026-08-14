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

Collega la board al Mac. Due avvertenze che fanno perdere pomeriggi interi:

- **Usa un cavo USB dati, non uno da sola ricarica.** Sono identici a
  vedersi. Se hai dubbi, prova con quello di un telefono che sai fare anche
  trasferimento file.
- **Se la board ha due prese USB-C, usa quella marcata `COM` o `UART`.**
  Questo progetto è già configurato per quella. L'altra (USB nativa)
  richiede due righe in più in `platformio.ini` — sono scritte lì dentro,
  già pronte da scommentare.

Per verificare che il Mac la veda, apri un terminale dentro VS Code
(`Terminale → Nuovo terminale`) e scrivi:

```bash
pio device list
```

Devi vedere una riga tipo `/dev/cu.usbserial-0001` o
`/dev/cu.wchusbserial14330`. **Se non compare niente**, il Mac non ha il
driver del convertitore seriale: cerca "CH34x driver macOS" (chip WCH) o
"CP210x VCP driver" (chip Silicon Labs) in base a quale dei due c'è sulla
tua board, installalo e **riavvia il Mac** — su macOS le estensioni di
sistema non si caricano prima del riavvio.

Non serve scrivere la porta in `platformio.ini`: PlatformIO la trova da solo.

---

## Passo 4 — Caricare il programma

Clicca la **→** (freccia destra) in basso: compila *e* carica sulla board.

Anche qui aspetti `SUCCESS`.

Se invece leggi **`Failed to connect to ESP32-S3: No serial data received`**,
la board non è entrata in modalità caricamento. Si forza a mano, con i due
pulsantini sulla board:

1. tieni premuto **BOOT**
2. premi e rilascia **RESET** (a volte marcato **EN**)
3. rilascia **BOOT**
4. riclicca la freccia

Su molte board economiche questo va fatto **ogni volta**: non è un guasto,
è che manca il circuitino di auto-reset che hanno le board ufficiali.

---

## Passo 5 — Parlare con la board

Clicca l'icona **🔌** (spina) in basso: apre il monitor seriale.

Premi **RESET** sulla board. Dovresti vedere comparire il rapporto completo
del chip, e poi un menu. Digita una lettera (`i`, `r`, `s`, `w`…) per lanciare
i vari test.

Per chiudere il monitor: `Ctrl+C` nel terminale. **Chiudilo sempre prima di
ricaricare**, altrimenti la porta risulta occupata e l'upload fallisce con
`Could not open port`.

---

## Cosa fanno i comandi del menu

| Tasto | Cosa fa |
|---|---|
| `i` | Rapporto completo: chip, flash, PSRAM, MAC, motivo dell'ultimo riavvio |
| `p` | Scrive e rilegge 64 KB di PSRAM per verificare che sia configurata bene |
| `r` | **Cerca il LED RGB**: accende a turno i GPIO più probabili. Guarda la board e segnati il numero annunciato quando vedi accendersi qualcosa |
| `l` | Come sopra ma per un LED normale, singolo colore |
| `s` | Elenca i sensori collegati al bus I2C, con un tentativo di identificarli |
| `w` | Scansiona le reti WiFi: serve a verificare che radio e antenna funzionino |
| `b` | Riavvia la board |

---

## Se qualcosa va storto

**Il monitor seriale non stampa niente**

1. Hai premuto RESET sulla board con il monitor già aperto?
2. Stai usando la porta USB nativa invece della COM/UART? Allora servono le
   righe della "MODIFICA 1" in `platformio.ini`.

**La board si riavvia in continuazione, o stampa testo incomprensibile**

Quasi sempre è la PSRAM configurata male. Rimetti `platformio.ini` come
te l'ho dato (tutto commentato) e ricarica.

**`Could not open port` durante l'upload**

Monitor seriale ancora aperto. `Ctrl+C`.

**La porta sparisce dopo il caricamento**

Succede solo con la porta USB nativa: il chip si riavvia e ricrea
l'interfaccia USB. Riapri il monitor dopo un paio di secondi.

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

Quando il comando `r` (o `l`) ti ha detto su quale GPIO sta il LED, il blink
vero è tre righe. Sostituisci il contenuto di `src/main.cpp` con:

```cpp
#include <Arduino.h>

#define LED_PIN 48   // <- il numero che ti ha trovato il comando 'r'

void setup() {}

void loop() {
  rgbLedWrite(LED_PIN, 40, 0, 0);   // rosso tenue
  delay(500);
  rgbLedWrite(LED_PIN, 0, 0, 0);    // spento
  delay(500);
}
```

Se invece è un LED normale:

```cpp
#include <Arduino.h>

#define LED_PIN 2

void setup() { pinMode(LED_PIN, OUTPUT); }

void loop() {
  digitalWrite(LED_PIN, HIGH); delay(500);
  digitalWrite(LED_PIN, LOW);  delay(500);
}
```

Consiglio: non sovrascrivere questo progetto. Duplica la cartella e lavora
sulla copia — `BoardDetective` ti tornerà utile ogni volta che compri una
board nuova.
