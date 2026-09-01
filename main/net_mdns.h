#pragma once
// =============================================================================
//  net_mdns - Annuncio mDNS (nome <host>.local sulla rete locale)
// =============================================================================
//  Rende il simulatore raggiungibile come "rgmbss.local" indipendentemente dall IP
//  assegnato dal DHCP. Serve per accedere alla UI HTTPS con un nome STABILE che
//  corrisponde al SAN del certificato (DNS:rgmbss.local), evitando i warning di
//  mismatch quando l'IP cambia.
//
//  Da avviare quando la STA ha ottenuto l'IP (callback on_sta_up).
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Hostname mDNS (senza suffisso .local). Tutto minuscolo, solo lettere/numeri/-.
#define MDNS_HOSTNAME   "rgmbss"

// Inizializza mDNS e imposta l'hostname. Pubblicizza anche il servizio HTTPS
// sulla porta 443. Idempotente: se gia' avviato, non fa nulla. Sicura da
// chiamare a ogni (ri)connessione STA.
void net_mdns_start(void);

#ifdef __cplusplus
}
#endif
