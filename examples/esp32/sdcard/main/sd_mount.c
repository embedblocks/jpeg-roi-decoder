#include "sd_mount.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdkconfig.h"

static sdmmc_card_t *card;

bool sd_mount_init(void)
{
    esp_err_t ret;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = CONFIG_SD_SPI_HOST;
    host.max_freq_khz = 5000;   // do this for bring-up, can be increased later

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_SD_SPI_MOSI,
        .miso_io_num = CONFIG_SD_SPI_MISO,
        .sclk_io_num = CONFIG_SD_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_SD_SPI_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ret = esp_vfs_fat_sdspi_mount(
        CONFIG_SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &card
    );

    if (ret != ESP_OK) {
        spi_bus_free(host.slot);
        return false;
    }

    return true;
}

void sd_mount_deinit(void)
{
    esp_vfs_fat_sdcard_unmount(CONFIG_SD_MOUNT_POINT, card);
    spi_bus_free(CONFIG_SD_SPI_HOST);
}