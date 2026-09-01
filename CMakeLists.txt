# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(argimbss)

# Immagine LittleFS: genera il filesystem dalla cartella 'littlefs_image/' del
# progetto e lo flasha nella partizione 'storage' a ogni 'idf.py flash'.
# Contiene i file di DEFAULT di fabbrica: wifi.json (credenziali STA opzionali)
# e, se si usa HTTPS, cert.pem/key.pem. Con l'avvio in HTTP di default i
# certificati NON sono necessari al boot: servono solo se si attiva HTTPS dalla
# pagina Security. Sopravvive agli OTA (che scrivono solo su ota_0/ota_1).
littlefs_create_partition_image(storage littlefs_image FLASH_IN_PROJECT)
