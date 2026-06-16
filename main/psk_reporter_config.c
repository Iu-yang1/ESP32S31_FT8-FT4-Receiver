#include "psk_reporter_config.h"

#include <string.h>

#include "nvs.h"

#define NVS_NAMESPACE "pskreporter"

static psk_reporter_config_t s_config;

static void set_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));
    strlcpy(s_config.server, "report.pskreporter.info", sizeof(s_config.server));
    s_config.port = 4739;
}

void psk_reporter_config_init(void)
{
    set_defaults();

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t size = sizeof(s_config);
    if (nvs_get_blob(handle, "config", &s_config, &size) != ESP_OK || size != sizeof(s_config)) {
        set_defaults();
    }
    nvs_close(handle);
}

const psk_reporter_config_t *psk_reporter_config_get(void)
{
    return &s_config;
}

bool psk_reporter_config_save(const psk_reporter_config_t *config)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t result = nvs_set_blob(handle, "config", config, sizeof(*config));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result == ESP_OK) {
        s_config = *config;
        return true;
    }
    return false;
}
