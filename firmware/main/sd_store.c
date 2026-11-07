#include "sd_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";
static bool s_ready;

bool sd_store_start(void)
{
    esp_err_t e = bsp_sdcard_mount();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(e));
        return false;
    }
    s_ready = true;
    sdmmc_card_print_info(stdout, bsp_sdcard);
    mkdir(SD_DIR, 0775);
    uint32_t tot, fr;
    sd_store_space(&tot, &fr);
    ESP_LOGI(TAG, "mounted at %s: %lu MB, %lu MB free", BSP_SD_MOUNT_POINT, (unsigned long)tot, (unsigned long)fr);
    return true;
}

bool sd_store_ready(void) { return s_ready; }

void sd_store_space(uint32_t *total_mb, uint32_t *free_mb)
{
    *total_mb = *free_mb = 0;
    if (!s_ready) return;
    uint64_t tot = 0, fr = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &tot, &fr) != ESP_OK) return;
    *total_mb = (uint32_t)(tot / (1024 * 1024));
    *free_mb = (uint32_t)(fr / (1024 * 1024));
}

void sd_store_selftest(void)
{
    if (!s_ready) { ESP_LOGW(TAG, "selftest: not mounted"); return; }
    uint32_t tot, fr;
    sd_store_space(&tot, &fr);
    ESP_LOGI(TAG, "card %s, %lu MB, %lu MB free", bsp_sdcard->cid.name, (unsigned long)tot, (unsigned long)fr);
    DIR *d = opendir(BSP_SD_MOUNT_POINT);
    if (d) {
        struct dirent *de;
        int n = 0;
        while ((de = readdir(d)) && n < 20) { ESP_LOGI(TAG, "  /%s%s", de->d_name, de->d_type == DT_DIR ? "/" : ""); n++; }
        closedir(d);
    }
    const char *p = SD_DIR "/probe.txt";
    FILE *f = fopen(p, "w");
    if (!f) { ESP_LOGE(TAG, "write %s: errno %d", p, errno); return; }
    fprintf(f, "blt-display probe\n");
    fclose(f);
    char line[40] = "";
    f = fopen(p, "r");
    if (f) { fgets(line, sizeof line, f); fclose(f); }
    remove(p);
    ESP_LOGI(TAG, "probe write/read/delete: %s", strncmp(line, "blt-display probe", 17) == 0 ? "OK" : "FAILED");
}
