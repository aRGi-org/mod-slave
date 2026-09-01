#pragma once
#include <stdbool.h>

// Avvia il web server HTTPS del simulatore (porta 443). Registra la SPA e gli
// endpoint REST (config seriale + modalita' fisica, generatore dati, wifi, ota,
// ntp, dashboard, event log, config import/export, gestione utenti).
// Parte in HTTP (default) oppure HTTPS se g_cfg.web_https e' true e i certificati
// (cert.pem/key.pem sull'immagine LittleFS) sono presenti; in caso contrario fa
// fallback automatico su HTTP.
void web_server_start(void);

// True se il server e' attivo in HTTPS (puo' differire dal richiesto in config
// se e' scattato il fallback su HTTP).
bool web_server_is_https(void);
