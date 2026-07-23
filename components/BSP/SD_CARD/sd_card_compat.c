#include "spi_sd.h"

#include "ff.h"

esp_err_t sd_spi_init(void)
{
    return sd_card_mount();
}

bool sd_spi_is_mounted(void)
{
    return sd_card_is_mounted();
}

void sd_get_fatfs_usage(size_t *out_total_kb, size_t *out_free_kb)
{
    FATFS *filesystem;
    DWORD free_clusters;
    FRESULT result = f_getfree("0:", &free_clusters, &filesystem);
    if (result != FR_OK)
    {
        if (out_total_kb)
            *out_total_kb = 0;
        if (out_free_kb)
            *out_free_kb = 0;
        return;
    }

    size_t total_sectors = (filesystem->n_fatent - 2) * filesystem->csize;
    size_t free_sectors = free_clusters * filesystem->csize;
    if (out_total_kb)
        *out_total_kb = total_sectors * filesystem->ssize / 1024;
    if (out_free_kb)
        *out_free_kb = free_sectors * filesystem->ssize / 1024;
}
