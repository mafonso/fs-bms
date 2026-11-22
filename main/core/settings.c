#include <string.h>
#include <ctype.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "settings";
static const char *NS = "app";

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    if (settings_get_wifi(ssid, pass) != ESP_OK && strlen(CONFIG_APP_WIFI_PASSWORD) > 0) {
        ESP_LOGI(TAG, "seeding Wi-Fi credentials for '%s' from build config", CONFIG_APP_WIFI_SSID);
        return settings_set_wifi(CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PASSWORD);
    }
    return ESP_OK;
}

esp_err_t settings_get_wifi(char ssid[SETTINGS_SSID_MAX], char pass[SETTINGS_PASS_MAX])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = SETTINGS_SSID_MAX;
    err = nvs_get_str(h, "ssid", ssid, &len);
    if (err == ESP_OK) {
        len = SETTINGS_PASS_MAX;
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_set_wifi(const char *ssid, const char *pass)
{
    if (strlen(ssid) >= SETTINGS_SSID_MAX || strlen(pass) >= SETTINGS_PASS_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_get_name(char name[SETTINGS_NAME_MAX])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = SETTINGS_NAME_MAX;
    err = nvs_get_str(h, "name", name, &len);
    nvs_close(h);
    return err;
}

esp_err_t settings_set_name(const char *name)
{
    if (strlen(name) >= SETTINGS_NAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    // An empty name clears the override and restores the MAC-based default.
    err = name[0] ? nvs_set_str(h, "name", name) : nvs_erase_key(h, "name");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool settings_valid_hostname(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len >= SETTINGS_NAME_MAX || n[0] == '-' || n[len - 1] == '-') {
        return false;
    }
    for (const char *c = n; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '-') {
            return false;
        }
    }
    return true;
}

esp_err_t settings_get_admin_hash(uint8_t salt[SETTINGS_SALT_LEN], uint8_t hash[SETTINGS_HASH_LEN])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = SETTINGS_SALT_LEN;
    err = nvs_get_blob(h, "admin_salt", salt, &len);
    if (err == ESP_OK && len != SETTINGS_SALT_LEN) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        len = SETTINGS_HASH_LEN;
        err = nvs_get_blob(h, "admin_hash", hash, &len);
        if (err == ESP_OK && len != SETTINGS_HASH_LEN) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_set_admin_hash(const uint8_t salt[SETTINGS_SALT_LEN], const uint8_t hash[SETTINGS_HASH_LEN])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (salt && hash) {
        err = nvs_set_blob(h, "admin_salt", salt, SETTINGS_SALT_LEN);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "admin_hash", hash, SETTINGS_HASH_LEN);
        }
    } else {
        // Hash first: without it the password is off even if the salt survives.
        err = nvs_erase_key(h, "admin_hash");
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_erase_key(h, "admin_salt");
        }
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool settings_get_bms_enabled(void)
{
    uint8_t v = 1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bms_on", &v);
        nvs_close(h);
    }
    return v != 0;
}

esp_err_t settings_set_bms_enabled(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "bms_on", on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint32_t settings_get_bms_interval(void)
{
    uint32_t v = SETTINGS_BMS_INTERVAL_DEFAULT;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "bms_interval", &v);
        nvs_close(h);
    }
    if (v < SETTINGS_BMS_INTERVAL_MIN || v > SETTINGS_BMS_INTERVAL_MAX) {
        v = SETTINGS_BMS_INTERVAL_DEFAULT;
    }
    return v;
}

esp_err_t settings_set_bms_interval(uint32_t seconds)
{
    if (seconds < SETTINGS_BMS_INTERVAL_MIN || seconds > SETTINGS_BMS_INTERVAL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, "bms_interval", seconds);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_get_bms_addr(char addr[SETTINGS_BMS_ADDR_MAX])
{
    addr[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    size_t len = SETTINGS_BMS_ADDR_MAX;
    err = nvs_get_str(h, "bms_addr", addr, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        addr[0] = '\0';
    }
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t settings_set_bms_addr(const char *addr)
{
    if (strlen(addr) >= SETTINGS_BMS_ADDR_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = addr[0] ? nvs_set_str(h, "bms_addr", addr) : nvs_erase_key(h, "bms_addr");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_get_bms_pin(uint32_t *pin)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u32(h, "bms_pin", pin);
    nvs_close(h);
    return err;
}

esp_err_t settings_set_bms_pin(const uint32_t *pin)
{
    if (pin && *pin > 999999) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = pin ? nvs_set_u32(h, "bms_pin", *pin) : nvs_erase_key(h, "bms_pin");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
