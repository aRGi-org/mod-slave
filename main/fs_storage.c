// =============================================================================
//  fs_storage.c - Filesystem LittleFS su partizione "storage"
// =============================================================================
#include "fs_storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "fs";

static bool s_mounted = false;

esp_err_t fs_storage_init(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_littlefs_conf_t conf = {
        .base_path              = FS_BASE_PATH,
        .partition_label        = FS_PARTITION,
        .format_if_mount_failed = true,   // se non formattata (primo boot), formatta
        .dont_mount             = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(FS_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

// Costruisce il path completo /littlefs/<path> in buf.
static void full_path(const char *path, char *buf, size_t buf_len)
{
    // salta eventuale '/' iniziale in path per non avere doppio slash
    if (path[0] == '/') path++;
    snprintf(buf, buf_len, "%s/%s", FS_BASE_PATH, path);
}

int fs_read_text(const char *path, char *out, size_t out_len)
{
    if (!s_mounted || !out || out_len == 0) return -1;

    char fp[128];
    full_path(path, fp, sizeof(fp));

    FILE *f = fopen(fp, "r");
    if (!f) return -1;

    size_t n = fread(out, 1, out_len - 1, f);
    fclose(f);
    out[n] = '\0';
    return (int)n;
}

esp_err_t fs_write_text(const char *path, const char *data, size_t len)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    char fp[128];
    full_path(path, fp, sizeof(fp));

    FILE *f = fopen(fp, "w");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for write", fp);
        return ESP_FAIL;
    }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len) {
        ESP_LOGE(TAG, "short write on %s (%u/%u)", fp, (unsigned)w, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool fs_exists(const char *path)
{
    if (!s_mounted) return false;
    char fp[128];
    full_path(path, fp, sizeof(fp));
    struct stat st;
    return stat(fp, &st) == 0;
}

esp_err_t fs_info(size_t *total, size_t *used)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    return esp_littlefs_info(FS_PARTITION, total, used);
}

// Cancella un file. Ritorna true se rimosso o gia' inesistente (idempotente).
bool fs_delete(const char *path)
{
    if (!s_mounted) return false;
    char fp[128];
    full_path(path, fp, sizeof(fp));
    if (remove(fp) == 0) return true;
    return !fs_exists(path);
}
