#pragma once
#include <stdbool.h>
#include "auth.h"

#define SESSION_TOKEN_LEN   33     // 32 hex char + terminatore
#define SESSION_MAX         4      // admin + operatore + margine per re-login/tab in transizione
#define SESSION_TIMEOUT_MS  ((5 * 60 + 30) * 1000)   // 5 min + 30s grazia; il timer locale UI comanda a 5 min

// Crea una sessione per l'utente/ruolo dato. Scrive il token (SESSION_TOKEN_LEN)
// in out_token. Ritorna false se non ci sono slot liberi.
bool session_create(const char *username, auth_role_t role, char *out_token);

// Valida un token. Se valido e non scaduto: aggiorna il "last activity",
// scrive ruolo/username (se i puntatori non sono NULL) e ritorna true.
bool session_validate(const char *token, auth_role_t *out_role, char *out_user);

// Invalida (logout) la sessione con quel token.
void session_destroy(const char *token);

// Invalida tutte le sessioni (es. dopo cambio password o reset).
void session_destroy_all(void);