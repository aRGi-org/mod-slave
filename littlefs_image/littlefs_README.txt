Contenuto dell'immagine LittleFS (partizione 'storage'), flashata a ogni
'idf.py flash' e sopravvive agli OTA.

FILE:
- wifi.json   Credenziali STA di DEFAULT di fabbrica (ssid/pass). Lasciato VUOTO
              nel repo: compilalo con la tua rete se vuoi che il simulatore si
              colleghi da subito senza passare dal captive portal. Se resta
              vuoto, al primo boot parte l'AP di configurazione "argimbss".
              Le credenziali salvate dal portale (in NVS) hanno la precedenza.

- cert.pem    (OPZIONALI) Certificato + chiave self-signed per l'HTTPS. NON
- key.pem     necessari con l'avvio in HTTP di default: servono SOLO se attivi
              HTTPS dalla pagina Security della UI. Se attivi HTTPS senza questi
              file presenti, il server fa fallback automatico su HTTP.
              Per generarli (esempio):
                openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
                  -days 3650 -nodes -subj "/CN=argimbss.local" \
                  -addext "subjectAltName=DNS:argimbss.local"
