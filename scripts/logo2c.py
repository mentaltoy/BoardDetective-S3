#!/usr/bin/env python3
"""
Converte un'immagine in una matrice di punti per il display.

    python3 scripts/logo2c.py assets/logo.png
    python3 scripts/logo2c.py assets/logo.png --punti 44 --copertura 0.3
    python3 scripts/logo2c.py assets/logo.png --cerchio

L'OPZIONE --cerchio
Se il logo e' racchiuso in un cerchio, conviene toglierlo qui e
lasciare che sia il programma a ridisegnarlo. Una circonferenza
ricavata da una griglia ha sempre i gradini, perche' i punti finiscono
dove capita la griglia; calcolata sull'angolo, invece, i punti sono
equidistanti e la curva viene morbida. Il resto del logo resta com'e'.

Genera src/logo.h, che il programma include da solo. Se il file non
c'e', il programma compila lo stesso: la scheda del logo semplicemente
non esiste.

COME DECIDE COS'E' DISEGNO E COS'E' SFONDO
Non guarda se un pixel e' chiaro o scuro: guarda quanto si discosta
dal colore dei bordi dell'immagine. Cosi' funziona sia con un logo
bianco su nero sia con uno rosso su bianco, senza doverglielo dire.

COME DECIDE SE ACCENDERE UN PUNTO
Non fa la media dei pixel, conta quanti appartengono al tratto. Con
la media una linea sottile sbiadisce fino a sparire; contando, basta
che il tratto occupi una fetta della casella perche' il punto si
accenda. E' la differenza fra un contorno continuo e uno spezzettato.

PERCHE' PASSA DA sips
Leggere un PNG significa decomprimerlo, e in Python serve una libreria
esterna. sips e' gia' dentro macOS e converte qualunque cosa tu abbia
salvato - png, jpg, heic - in BMP, che invece e' un formato cosi'
elementare da leggere in venti righe. Meno pezzi da installare, meno
cose che si rompono.
"""

import os
import struct
import subprocess
import sys
import tempfile

# Quanti punti largo sara' il logo. Piu' alto = piu' dettaglio, ma
# punti piu' piccoli: oltre una certa soglia smette di sembrare fatto
# di led, che e' proprio l'effetto che cerchiamo.
PUNTI_LARGHEZZA = 38

# Quanta parte di una casella deve essere coperta dal tratto perche'
# il punto si accenda. Abbassandola le linee si ingrossano e si
# chiudono i buchi; alzandola si assottigliano fino a spezzarsi.
# A 0.60 un contorno sottile viene spesso un punto solo, che e' il
# motivo per cui e' il valore scelto qui.
COPERTURA = 0.60

# Quanto un pixel deve essere diverso dal colore di sfondo per
# contare come disegno. Serve a ignorare le sfumature dei bordi, che
# in un jpg non sono mai nette.
DIFFERENZA_MINIMA = 70

# Risoluzione intermedia. Piu' alta = conteggi piu' precisi, ma il
# calcolo e' in Python puro e il tempo cresce col quadrato.
LARGHEZZA_LAVORO = 900


def leggi_bmp(percorso):
    """Restituisce (larghezza, altezza, righe di terne rgb)."""
    with open(percorso, "rb") as f:
        dati = f.read()

    if dati[:2] != b"BM":
        raise ValueError("non e' un BMP")

    inizio_pixel = struct.unpack_from("<I", dati, 10)[0]
    larghezza = struct.unpack_from("<i", dati, 18)[0]
    altezza = struct.unpack_from("<i", dati, 22)[0]
    bit = struct.unpack_from("<H", dati, 28)[0]

    if bit not in (24, 32):
        raise ValueError("mi aspettavo 24 o 32 bit per pixel, trovati %d" % bit)

    byte_per_pixel = bit // 8
    # Ogni riga e' allineata a multipli di 4 byte.
    passo = ((larghezza * byte_per_pixel + 3) // 4) * 4

    # Con altezza positiva le righe sono memorizzate dal basso in alto.
    dal_basso = altezza > 0
    altezza = abs(altezza)

    righe = []
    for y in range(altezza):
        y_reale = (altezza - 1 - y) if dal_basso else y
        base = inizio_pixel + y_reale * passo
        riga = []
        for x in range(larghezza):
            p = base + x * byte_per_pixel
            riga.append((dati[p + 2], dati[p + 1], dati[p]))   # bgr -> rgb
        righe.append(riga)
    return larghezza, altezza, righe


def colore_di_sfondo(righe, larghezza, altezza):
    """Il colore dei quattro bordi: quello e' lo sfondo, per definizione."""
    campioni = []
    for x in range(0, larghezza, max(1, larghezza // 60)):
        campioni.append(righe[0][x])
        campioni.append(righe[altezza - 1][x])
    for y in range(0, altezza, max(1, altezza // 60)):
        campioni.append(righe[y][0])
        campioni.append(righe[y][larghezza - 1])
    n = len(campioni)
    return (sum(c[0] for c in campioni) // n,
            sum(c[1] for c in campioni) // n,
            sum(c[2] for c in campioni) // n)


def mappa_del_tratto(righe, larghezza, altezza, sfondo, differenza_minima):
    """True dove c'e' disegno, False dove c'e' sfondo."""
    sr, sg, sb = sfondo
    soglia = differenza_minima * differenza_minima
    mappa = []
    for y in range(altezza):
        riga_sorgente = righe[y]
        riga = bytearray(larghezza)
        for x in range(larghezza):
            r, g, b = riga_sorgente[x]
            dr, dg, db = r - sr, g - sg, b - sb
            if dr * dr + dg * dg + db * db >= soglia:
                riga[x] = 1
        mappa.append(riga)
    return mappa


def riquadro_del_contenuto(mappa, larghezza, altezza):
    """I bordi del disegno, per buttare via lo sfondo intorno."""
    su, giu, sx, dx = altezza, -1, larghezza, -1
    for y in range(altezza):
        riga = mappa[y]
        if 1 not in riga:
            continue
        su = min(su, y)
        giu = max(giu, y)
        sx = min(sx, riga.index(1))
        dx = max(dx, larghezza - 1 - riga[::-1].index(1))
    if giu < 0:
        raise ValueError("non trovo nessun disegno: prova ad abbassare DIFFERENZA_MINIMA")
    return sx, su, dx, giu


def main():
    argomenti = sys.argv[1:]
    if not argomenti:
        print(__doc__)
        sys.exit(1)

    sorgente = argomenti[0]
    punti_x = PUNTI_LARGHEZZA
    copertura = COPERTURA
    differenza = DIFFERENZA_MINIMA

    togli_cerchio = "--cerchio" in argomenti

    i = 1
    while i < len(argomenti) - 1:
        if argomenti[i] == "--punti":
            punti_x = int(argomenti[i + 1])
        elif argomenti[i] == "--copertura":
            copertura = float(argomenti[i + 1])
        elif argomenti[i] == "--differenza":
            differenza = int(argomenti[i + 1])
        i += 2

    if not os.path.exists(sorgente):
        print("non trovo %s" % sorgente)
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmp:
        bmp = os.path.join(tmp, "lavoro.bmp")
        subprocess.run(
            ["sips", "-s", "format", "bmp",
             "--resampleWidth", str(LARGHEZZA_LAVORO),
             sorgente, "--out", bmp],
            check=True, capture_output=True)
        larghezza, altezza, righe = leggi_bmp(bmp)

    sfondo = colore_di_sfondo(righe, larghezza, altezza)
    mappa = mappa_del_tratto(righe, larghezza, altezza, sfondo, differenza)
    sx, su, dx, giu = riquadro_del_contenuto(mappa, larghezza, altezza)

    larghezza_utile = dx - sx + 1
    altezza_utile = giu - su + 1
    punti_y = max(1, round(altezza_utile * punti_x / larghezza_utile))

    griglia = []
    for gy in range(punti_y):
        riga = ""
        y0 = su + gy * altezza_utile // punti_y
        y1 = max(y0 + 1, su + (gy + 1) * altezza_utile // punti_y)
        for gx in range(punti_x):
            x0 = sx + gx * larghezza_utile // punti_x
            x1 = max(x0 + 1, sx + (gx + 1) * larghezza_utile // punti_x)

            tratto = totale = 0
            for y in range(y0, min(y1, altezza)):
                riga_mappa = mappa[y]
                for x in range(x0, min(x1, larghezza)):
                    tratto += riga_mappa[x]
                    totale += 1
            riga += "#" if totale and (tratto / totale) >= copertura else "."
        griglia.append(riga)

    # Il cerchio esterno se ne va: lo rifara' il programma, tondo.
    # Si spegne tutto quello che sta oltre l'85% del raggio, distanza
    # a cui il monogramma non arriva mai.
    if togli_cerchio:
        cx = (punti_x - 1) / 2.0
        cy = (punti_y - 1) / 2.0
        limite = 0.85 * min(cx, cy)
        for gy in range(punti_y):
            riga = list(griglia[gy])
            for gx in range(punti_x):
                if riga[gx] == "#":
                    dx_ = gx - cx
                    dy_ = gy - cy
                    if (dx_ * dx_ + dy_ * dy_) ** 0.5 > limite:
                        riga[gx] = "."
            griglia[gy] = "".join(riga)

    accesi = sum(r.count("#") for r in griglia)

    with open("src/logo.h", "w") as f:
        f.write("// Generato da scripts/logo2c.py - non modificare a mano.\n")
        f.write("// Sorgente: %s\n" % os.path.basename(sorgente))
        f.write("// Griglia %d x %d, copertura %.2f\n" % (punti_x, punti_y, copertura))
        f.write("//\n")
        f.write("// Un cancelletto e' un punto acceso. Si legge a occhio:\n")
        f.write("// se qualche tratto e' venuto male, si aggiusta qui.\n\n")
        f.write("#ifndef LOGO_H\n#define LOGO_H\n\n")
        f.write("#define LOGO_LARGHEZZA %d\n" % punti_x)
        f.write("#define LOGO_ALTEZZA   %d\n" % punti_y)
        f.write("#define LOGO_CERCHIO   %d\n\n" % (1 if togli_cerchio else 0))
        f.write("static const char *LOGO[LOGO_ALTEZZA] = {\n")
        for riga in griglia:
            f.write('    "%s",\n' % riga)
        f.write("};\n\n#endif // LOGO_H\n")

    print("sfondo rilevato: rgb%s" % (sfondo,))
    print("src/logo.h scritto: %d x %d punti, %d accesi" % (punti_x, punti_y, accesi))
    print()
    for riga in griglia:
        print("  " + riga.replace("#", "█").replace(".", "·"))


if __name__ == "__main__":
    main()
