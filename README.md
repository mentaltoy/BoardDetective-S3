# BoardDetective S3

[![build](https://github.com/mentaltoy/BoardDetective-S3/actions/workflows/build.yml/badge.svg)](https://github.com/mentaltoy/BoardDetective-S3/actions)

**Capire cosa hai davvero comprato, prima di scrivere una riga di codice utile.**

Le board ESP32-S3 generiche (AliExpress, Amazon, cloni vari) arrivano senza
documentazione affidabile: non sai su quale GPIO sia il LED, non sai se la
PSRAM c'è davvero, e la metà dei "non funziona" nasce da un'impostazione
sbagliata nell'IDE, non dal codice.

Questo sketch te lo dice lui, interrogando il chip.

```
============================================================
  INFORMAZIONI CHIP
============================================================
Modello .............. ESP32-S3
Revisione silicio .... v0.2
Core ................. 2
Frequenza CPU ........ 240 MHz
Core Arduino ......... 3.3.11
------------------------------------------------------------
Flash (dichiarata) ... 16777216 byte  = 16 MB
PSRAM ................ 8388608 byte = 8 MB
------------------------------------------------------------
MAC base ............. 34:85:18:9A:BC:DE
Motivo dell'ultimo riavvio:
  accensione (normale)
```

## Cosa fa

| Tasto | Test |
|:---:|---|
| `i` | Chip, revisione, flash, PSRAM, MAC, motivo dell'ultimo riavvio |
| `p` | Scrive e rilegge 64 KB di PSRAM — verifica che sia configurata davvero bene |
| `r` | **Caccia al LED RGB**: accende a turno i GPIO più probabili, annunciandoli sulla seriale |
| `l` | Caccia al LED normale, con entrambe le polarità (molti sono a logica invertita) |
| `s` | Scansione bus I2C, con tentativo di identificare il sensore dall'indirizzo |
| `w` | Scansione reti WiFi — verifica radio e antenna |
| `b` | Riavvio |

La "caccia al LED" è la parte che risolve il problema più fastidioso: sui
cloni il LED può stare sul GPIO 48, 38, 47, 21, 39, 40 o 8 e nessuno te lo
dice. Lo sketch li prova tutti, tu guardi la board e leggi il numero.

## Uso rapido

Serve [PlatformIO](https://platformio.org/install/ide?install=vscode) su VS Code.

```bash
git clone https://github.com/<utente>/BoardDetective-S3.git
cd BoardDetective-S3
pio run --target upload
pio device monitor
```

Poi premi RESET sulla board e digita una lettera del menu.

Se è la prima volta che usi PlatformIO, parti da
**[docs/guida-platformio.md](docs/guida-platformio.md)**: spiega tutto dal
primo click, inclusi gli intoppi che capitano a chiunque.

## Configurazione

Di serie è impostato per la **porta USB marcata COM / UART**, che è la scelta
giusta per iniziare: è più tollerante e non sparisce a ogni riavvio del chip.

Dentro [`platformio.ini`](platformio.ini) ci sono due blocchi commentati,
ognuno con scritto quando serve:

- **porta USB nativa** → richiede `ARDUINO_USB_CDC_ON_BOOT=1`, altrimenti il
  monitor seriale resta muto (è il problema n°1 su queste board)
- **PSRAM** → dipende dalla sigla stampata sul modulo: `R8` vuole
  `qio_opi`, `R2` vuole il default, nessuna `R` vuole tutto commentato

Leggi la sigla sul modulo metallico, tipo `ESP32-S3-WROOM-1-N16R8`:
`N16` = 16 MB di flash, `R8` = 8 MB di PSRAM octal.

## Note hardware (leggi prima di collegare qualcosa)

- **GPIO 26–37: mai usarli.** Sono cablati alla flash SPI e alla PSRAM octal.
  Usarli manda il chip in crash immediato.
- **GPIO 0, 3, 45, 46** sono pin di strapping: decidono come si avvia il chip.
  Se qualcosa li tiene alti o bassi all'accensione, la board non parte.
- **GPIO 19 e 20** sono D− e D+ dell'USB nativo.
- **Tutto è a 3.3 V.** Nessun pin tollera i 5 V.

## Un dettaglio non ovvio

`rgbLedWrite()` occupa un canale RMT e non lo rilascia mai. L'ESP32-S3 ne ha
**solo 4 in trasmissione**: senza una `rmtDeinit()` esplicita dopo ogni pin,
la caccia al LED salterebbe in silenzio gli ultimi tre candidati della lista,
facendoti credere che lì non ci sia nulla — GPIO 8 compreso, che è comune.

È il tipo di bug che non dà errori: fa solo dare la risposta sbagliata.

## Licenza

MIT — vedi [LICENSE](LICENSE).
