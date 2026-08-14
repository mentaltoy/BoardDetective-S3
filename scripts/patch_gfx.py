# Aggira un bug di Arduino_GFX 1.6.1 sul core Arduino 2.x.
#
# Il file Arduino_ESP32RGBPanel.cpp e' un driver per pannelli RGB
# paralleli. Contiene un ramo di codice legacy che referenzia
# "esp_rgb_panel_t", un tipo interno di ESP-IDF che nella 4.4 non e'
# accessibile: su core 2.x quel file non compila e blocca tutta la build.
#
# Su questa board il display e' un AMOLED SH8601 su bus QSPI: quel
# driver non viene mai usato, viene compilato solo perche' PlatformIO
# compila l'intera libreria. Qui lo svuotiamo.
#
# Si potra' buttare il giorno in cui passeremo al core Arduino 3.x.

import os

Import("env")

MARKER = "// svuotato da scripts/patch_gfx.py"

target = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "GFX Library for Arduino",
    "src", "databus", "Arduino_ESP32RGBPanel.cpp",
)

if not os.path.isfile(target):
    print("patch_gfx: libreria non ancora presente, ricompila una seconda volta.")
else:
    with open(target, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()
    if content.startswith(MARKER):
        print("patch_gfx: gia' applicata.")
    else:
        with open(target, "w", encoding="utf-8") as f:
            f.write(MARKER + "\n")
        print("patch_gfx: neutralizzato Arduino_ESP32RGBPanel.cpp")