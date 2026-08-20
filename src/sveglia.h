/*
 * ============================================================
 *  LE SVEGLIE
 * ============================================================
 *
 *  Un elenco di orari che sopravvive allo spegnimento.
 *
 *  DOVE FINISCONO
 *  Non in un file: in NVS, una zona della flash che l'ESP32
 *  riserva alle impostazioni. Funziona a coppie nome-valore, come
 *  un cassetto di etichette, e sopravvive anche al ricaricamento
 *  del programma - il che significa che ricompilare non ti
 *  cancella le sveglie.
 *
 *  Le scriviamo tutte insieme come un unico blocco di byte invece
 *  che una per una. La flash non si riscrive a byte singoli ma a
 *  settori interi: sei scritture separate consumerebbero il
 *  supporto sei volte tanto per salvare gli stessi dati.
 * ============================================================
 */

#ifndef SVEGLIA_H
#define SVEGLIA_H

#include <Arduino.h>
#include <Preferences.h>

#define MAX_SVEGLIE 8

struct Sveglia
{
    uint8_t ore;
    uint8_t minuti;
    bool attiva;
};

static Sveglia sveglie[MAX_SVEGLIE];
static int nSveglie = 0;

static Preferences prefsSveglie;

static void sveglieCarica()
{
    nSveglie = 0;

    // true = sola lettura. La primissima volta lo spazio non esiste
    // ancora e l'apertura fallisce: lo si crea subito, altrimenti a
    // ogni avvio la libreria stampa un errore che sembra un guasto e
    // non lo e'.
    if (!prefsSveglie.begin("dotclock", true))
    {
        if (prefsSveglie.begin("dotclock", false))
            prefsSveglie.end();
        return;
    }

    size_t byte = prefsSveglie.getBytesLength("sveglie");
    if (byte > 0 && byte <= sizeof(sveglie))
    {
        prefsSveglie.getBytes("sveglie", sveglie, byte);
        nSveglie = byte / sizeof(Sveglia);
    }
    prefsSveglie.end();

    // Difesa contro un salvataggio di una versione precedente del
    // programma, con una struttura diversa: meglio nessuna sveglia
    // che orari inventati.
    for (int i = 0; i < nSveglie; ++i)
        if (sveglie[i].ore > 23 || sveglie[i].minuti > 59)
        {
            nSveglie = 0;
            break;
        }
}

static void sveglieSalva()
{
    if (!prefsSveglie.begin("dotclock", false))
        return;
    prefsSveglie.putBytes("sveglie", sveglie, nSveglie * sizeof(Sveglia));
    prefsSveglie.end();
}

static int sveglieAttive()
{
    int n = 0;
    for (int i = 0; i < nSveglie; ++i)
        if (sveglie[i].attiva) ++n;
    return n;
}

static bool sveglieAggiungi(uint8_t ore, uint8_t minuti)
{
    if (nSveglie >= MAX_SVEGLIE) return false;
    sveglie[nSveglie].ore = ore;
    sveglie[nSveglie].minuti = minuti;
    sveglie[nSveglie].attiva = true;
    ++nSveglie;
    sveglieSalva();
    return true;
}

static void sveglieRimuovi(int indice)
{
    if (indice < 0 || indice >= nSveglie) return;
    for (int i = indice; i < nSveglie - 1; ++i)
        sveglie[i] = sveglie[i + 1];
    --nSveglie;
    sveglieSalva();
}

#endif // SVEGLIA_H
