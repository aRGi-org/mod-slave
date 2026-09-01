#pragma once
#include <stdbool.h>
#include <stdint.h>

// Due ruoli.
typedef enum {
    ROLE_NONE  = 0,   // non autenticato
    ROLE_USER  = 1,   // operatore: stato + device
    ROLE_ADMIN = 2    // pieno controllo
} auth_role_t;

#define AUTH_USERNAME_LEN   16
#define AUTH_SALT_LEN       16    // salt binario
#define AUTH_HASH_LEN       32    // SHA-256 = 32 byte
#define AUTH_PBKDF2_ITERS   10000   // iterazioni PBKDF2: sicurezza vs latenza login

// Un utente: username, hash della password, salt, flag cambio obbligatorio.
typedef struct {
    char        username[AUTH_USERNAME_LEN];
    uint8_t     salt[AUTH_SALT_LEN];
    uint8_t     hash[AUTH_HASH_LEN];
    auth_role_t role;
    bool        must_change;   // true finche' non cambia la password iniziale
} auth_user_t;

// Inizializza: carica utenti da NVS o applica i default (admin/user iniziali).
void auth_init(void);

// Verifica le credenziali. Ritorna il ruolo se ok, ROLE_NONE se fallisce.
// Se out_must_change != NULL, vi scrive il flag must_change dell'utente.
auth_role_t auth_check(const char *username, const char *password, bool *out_must_change);

// Cambia la password di un utente (dopo verifica della vecchia).
// Azzera il flag must_change. Ritorna false se la vecchia password e' errata.
bool auth_change_password(const char *username, const char *old_pw, const char *new_pw);

// Admin reset: imposta la password di un utente senza la vecchia (solo admin).
bool auth_set_password(const char *username, const char *new_pw);

// Ripristina gli utenti ai default di fabbrica (admin/user iniziali, must_change=true).
void auth_reset_defaults(void);

// Salva/carica lo stato utenti in NVS (namespace "auth").
bool auth_save(void);
bool auth_load(void);

// Cancella gli utenti da NVS e ripristina i default (per il factory-reset).
bool auth_erase(void);