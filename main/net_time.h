#pragma once
// =============================================================================
//  net_time - Ora di sistema via NTP/SNTP + timestamp UTC per i log
// =============================================================================
//  Gestisce l'ora del gateway:
//   - Sincronizza via SNTP con un server configurabile (hostname o IP), cosi'
//     copre sia NTP pubblico (gateway connesso) sia NTP locale (impianto isolato
//     con time-server interno).
//   - Sync allo startup, poi ogni ora. Se una sync fallisce, ritenta dopo 1 ora.
//   - Timezone come stringa POSIX TZ (setenv TZ + tzset). Il DST e' gestito dalla
//     stringa POSIX ma serve SOLO per la visualizzazione dell'ora locale (nella
//     UI). I LOG restano SEMPRE in UTC.
//   - Override di esp_log_system_timestamp() (weak linkage) per stampare i log
//     con timestamp ISO 8601 UTC "2026-08-20T14:30:15Z". Finche' l'ora non e'
//     sincronizzata, il timestamp ripiega sull'uptime (ms da boot).
//
//  Da avviare quando la STA ha ottenuto l'IP (callback on_sta_up), come mDNS.
//  L'avvio e' idempotente e riflette la config corrente (server/tz/enabled).
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Lunghezze massime per i campi di configurazione (usate anche dalla config).
#define NTP_SERVER_MAXLEN   64    // hostname o IP del server NTP
#define NTP_TZ_MAXLEN       48    // stringa POSIX TZ (es. CET-1CEST,M3.5.0,M10.5.0/3)

// Applica la timezone (stringa POSIX). Chiamabile in qualsiasi momento: aggiorna
// solo la conversione dell'ora locale (i log restano UTC). Se tz e' vuota, usa UTC.
void net_time_set_timezone(const char *posix_tz);

// (Ri)avvia la sincronizzazione NTP secondo la configurazione fornita.
//   enabled : se false, NON sincronizza (resta su uptime). Ferma un eventuale
//             SNTP gia' attivo.
//   server  : hostname o IP del server NTP (ignorato se enabled=false).
//   posix_tz: timezone POSIX per la visualizzazione locale (puo' essere NULL/"").
// Idempotente: sicura da chiamare a ogni (ri)connessione STA e dopo un cambio di
// configurazione. Avvia internamente il task di risincronizzazione periodica.
void net_time_start(bool enabled, const char *server, const char *posix_tz);

// Ferma la sincronizzazione NTP (se attiva). L'ora gia' acquisita resta valida
// (mantenuta dall'orologio interno del SoC).
void net_time_stop(void);

// true se e' stata effettuata almeno una sincronizzazione riuscita dall'avvio.
// Utile per la UI (stato "sincronizzato / non sincronizzato").
bool net_time_is_synced(void);

// Scrive in 'out' l'ora UTC corrente in ISO 8601 "2026-08-20T14:30:15Z".
// Se l'ora non e' ancora sincronizzata, scrive una stringa vuota e ritorna false.
// 'out' deve essere >= 21 byte. Pensata per l'endpoint che alimenta il footer UI.
bool net_time_utc_iso8601(char *out, size_t out_len);

// Scrive in 'out' l'ora LOCALE corrente come "YYYY-MM-DD HH:MM" (senza secondi),
// applicando la timezone impostata (localtime). Il gateway la calcola perche' ha
// gia' tzset() con la stringa POSIX; cosi' il browser deve solo formattarla, non
// convertire il fuso. Se non sincronizzata, stringa vuota e ritorna false.
// 'out' deve essere >= 17 byte.
bool net_time_local_hhmm(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
