# BoardDetective S3

**Diagnostica per la Waveshare ESP32-S3-Touch-AMOLED-1.8 — perché il profilo
di serie di PlatformIO su questa board dice cose false.**

Il progetto nasce da un problema concreto: PlatformIO non ha una definizione
per questa board, quindi si parte da quella generica `esp32-s3-devkitc-1` —
che dichiara **8 MB di flash e nessuna PSRAM**. Il modulo reale ne ha **16 MB
e 8 MB di PSRAM octal**. Compilando con il profilo di serie si butta via metà
della memoria di programma e la totalità della PSRAM, e il display non può
nemmeno funzionare.

Questo sketch interroga il chip e ti dice cosa c'è davvero, sulla seriale e
sullo schermo.

```
============================================================
  INFORMAZIONI CHIP
============================================================
Modello .............. ESP32-S3
Core ................. 2
Frequenza CPU ........ 240 MHz
------------------------------------------------------------
Flash ................ 16777216 byte = 16.0 MB
Sketch occupa ........ 702928 byte su 7256528 disponibili
------------------------------------------------------------
PSRAM ................ 8386231 byte = 8.0 MB
PSRAM libera ......... 8386051 byte = 8.0 MB
RAM interna libera ... 335840 byte
------------------------------------------------------------
MAC base ............. 30:ED:A0:AC:94:F8
```

## Cosa fa

| Tasto | Test |
|:---:|---|
| `i` | Chip, revisione, flash, PSRAM, MAC, motivo dell'ultimo riavvio |
| `p` | Scrive e rilegge 64 KB di PSRAM: verifica che sia configurata davvero |
| `s` | Scansione I2C con identificazione dei sei chip a bordo |
| `w` | Scansione reti WiFi, per verificare radio e antenna |
| `d` | Ridisegna il riepilogo sul display AMOLED |
| `b` | Riavvio |

Si comanda dal monitor seriale: digiti una lettera, premi invio.

## Uso rapido

Serve [PlatformIO](https://platformio.org/install/ide?install=vscode) su VS Code.

```bash
git clone https://github.com/mentaltoy/BoardDetective-S3.git
cd BoardDetective-S3
pio run --target upload
pio device monitor
```

Se è la prima volta che usi PlatformIO, parti da
**[docs/guida-platformio.md](docs/guida-platformio.md)**.

## L'hardware, verificato

Pinout preso dal
[repository ufficiale Waveshare](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8)
(`examples/arduino/libraries/Mylibrary/pin_config.h`), non dedotto.

| Sottosistema | Chip | GPIO |
|---|---|---|
| Display QSPI | SH8601, 368×448 | SDIO 4/5/6/7, SCLK 11, CS 12 |
| Bus I2C | *(condiviso)* | SDA **15**, SCL **14** |
| Touch | FT3168 | INT 21 |
| Alimentazione | AXP2101 (PMIC) | via I2C |
| Movimento | QMI8658 (IMU 6 assi) | via I2C |
| Orologio | PCF85063 (RTC) | via I2C |
| Espansione IO | TCA9554 | via I2C |
| Audio | ES8311 | MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10, PA 46 |
| microSD | *(SDMMC 1 bit)* | CLK 2, CMD 1, DATA 3 |

**Non esiste un LED utente.** Il display è l'unica uscita visiva. Ed è la
ragione per cui questo progetto non contiene la classica routine che cerca il
LED provando i GPIO: su questa board ognuno di quei pin è occupato da qualcosa
— il GPIO 15 è addirittura la linea SDA su cui parlano PMIC, touch e orologio.

**Non esiste un pulsante RESET.** Ci sono solo BOOT e PWR, e il PWR non è
nemmeno un GPIO: passa dall'expander (EXIO4), pressione lunga di 6 secondi.

**GPIO 26–37 intoccabili**: sono flash SPI e PSRAM octal.

## Le tre trappole di configurazione

Documentate qui perché ognuna costa un pomeriggio a chi non le conosce.

### 1. Il monitor seriale resta muto

Questa board ha una sola presa USB-C, cablata all'USB nativo del chip
(`USB-Serial/JTAG`). Il caricamento funziona lo stesso perché lo gestisce il
bootloader in ROM, ma appena parte lo sketch `Serial` finisce sui pin fisici
della UART e non vedi più niente. Servono:

```ini
-D ARDUINO_USB_CDC_ON_BOOT=1
-D ARDUINO_USB_MODE=1
```

### 2. La PSRAM non è opzionale

Il framebuffer è 368 × 448 × 2 = **329 KB**, più dei 320 KB di RAM interna del
chip. Senza PSRAM il display non può funzionare, punto. La PSRAM è integrata
nel package (variante S3R8) ed è di tipo octal:

```ini
board_build.arduino.memory_type = qio_opi
-D BOARD_HAS_PSRAM
```

Il comando `p` serve proprio a distinguere una PSRAM *dichiarata* da una PSRAM
*funzionante*: con la modalità sbagliata viene rilevata ma corrompe i dati.

### 3. La libreria grafica e il core spaccato in due

Il supporto al controller SH8601 compare in Arduino_GFX a partire dalla
**1.6.1**. Ma dalla **1.6.2** in poi la libreria richiede il core Arduino 3.x
(include `esp32-hal-periman.h`), e il PlatformIO ufficiale è fermo al core
**2.0.17** su tutte le versioni, 7.0.1 compresa.

Resta quindi una sola versione utilizzabile, la 1.6.1 — che però ha un difetto
suo: mantiene un ramo di codice legacy in `Arduino_ESP32RGBPanel.cpp` che
referenzia `esp_rgb_panel_t`, un tipo interno di ESP-IDF non accessibile nella
4.4. Quel file non compila e fa fallire l'intera build, pur essendo un driver
per pannelli RGB paralleli che questa board non ha.

[`scripts/patch_gfx.py`](scripts/patch_gfx.py) lo svuota subito dopo il
download della libreria. Alla primissima compilazione lo script gira prima che
la libreria sia stata scaricata: basta lanciare la build una seconda volta.

## Core 2.x o 3.x

Il progetto compila su entrambi: il codice che dipende dalla versione è
isolato dietro `ESP_ARDUINO_VERSION_MAJOR`.

Per passare al core 3.x serve la piattaforma community
[pioarduino](https://github.com/pioarduino/platform-espressif32), perché il
PlatformIO ufficiale non ci arriva. In quel caso si può alzare Arduino_GFX
alla 1.6.4 (la versione che Waveshare distribuisce e collauda) e lo script di
patch diventa inutile:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
lib_deps = moononournation/GFX Library for Arduino @ 1.6.4
; extra_scripts non serve piu'
```

Attenzione: pioarduino richiede la sua estensione VS Code dedicata
(`pioarduino IDE`), e con Python 3.14 la creazione dell'ambiente virtuale può
fallire. Se non ti serve, il core 2.x va benissimo.

## Due revisioni hardware

Esistono due versioni di questa board: l'originale con display **SH8601** e
touch **FT3168**, e una V2 con **CO5300** e **CST820**. Questo codice è per la
prima — controlla la sigla stampata sull'etichetta prima di riusarlo.

## Licenza

MIT — vedi [LICENSE](LICENSE).
