#include "entrance.h"
#include "arcade.h"
#include "relay.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static _Atomic bool online, ready, maintenance, station_enabled;
static bool sntp_started;
static _Atomic int disconnect_reason;
static SemaphoreHandle_t network_mutex;
static portMUX_TYPE ip_mutex = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t reconnect_timer;
static unsigned failures;
static char station_ip[20] = "offline";

static void reconnect(void *arg)
{
    (void)arg;
    if (station_enabled && !maintenance)
        esp_wifi_connect();
}

static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START)
        ready = true;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP)
        ready = false;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        disconnect_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        online = false;
        portENTER_CRITICAL(&ip_mutex);
        strlcpy(station_ip, "offline", sizeof(station_ip));
        portEXIT_CRITICAL(&ip_mutex);
        if (station_enabled && !maintenance) {
            esp_timer_stop(reconnect_timer);
            unsigned delay = 1u << (failures < 6 ? failures++ : 6);
            esp_timer_start_once(reconnect_timer, (uint64_t)delay * 1000000);
        }
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (!station_enabled || maintenance)
            return;
        ip_event_got_ip_t *got = data;
        char address[20];
        snprintf(address, sizeof(address), IPSTR, IP2STR(&got->ip_info.ip));
        portENTER_CRITICAL(&ip_mutex);
        strlcpy(station_ip, address, sizeof(station_ip));
        portEXIT_CRITICAL(&ip_mutex);
        online = true;
        disconnect_reason = 0;
        failures = 0;
        if (!sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_started = esp_netif_sntp_init(&cfg) == ESP_OK;
        }
        diag_event("Wi-Fi uplink connected");
    }
}

esp_err_t net_apply(void)
{
    if (!network_mutex)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    settings_t snapshot = settings_snapshot();
    station_enabled = snapshot.enabled && (!arcade_guest_network() || relay_enabled());
    esp_timer_stop(reconnect_timer);
    online = false;
    esp_wifi_disconnect();
    if (!station_enabled || maintenance) {
        xSemaphoreGive(network_mutex);
        return ESP_OK;
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, snapshot.ssid, strlen(snapshot.ssid));
    strlcpy((char *)config.sta.password, snapshot.password, sizeof(config.sta.password));
    config.sta.threshold.authmode = *snapshot.password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Campus SSIDs have several BSSIDs. Scan all channels and try alternatives
       instead of repeatedly selecting the first access point that answers. */
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.failure_retry_cnt = 2;
    config.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK)
        err = esp_wifi_connect();
    memset(&config, 0, sizeof(config));
    memset(&snapshot, 0, sizeof(snapshot));
    xSemaphoreGive(network_mutex);
    return err;
}

esp_err_t net_init(void)
{
    network_mutex = xSemaphoreCreateMutex();
    if (!network_mutex)
        return ESP_ERR_NO_MEM;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK)
        return err;
    if ((err = esp_event_loop_create_default()) != ESP_OK)
        return err;
    if (!esp_netif_create_default_wifi_ap() || !esp_netif_create_default_wifi_sta())
        return ESP_FAIL;
    esp_timer_create_args_t timer = {.callback = reconnect, .name = "uplink_retry"};
    if ((err = esp_timer_create(&timer, &reconnect_timer)) != ESP_OK)
        return err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Settings failures must not prevent the identity-backed maintenance AP. */
    cfg.nvs_enable = 0;
    if ((err = esp_wifi_init(&cfg)) != ESP_OK)
        return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK)
        return err;
    if ((err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL)) != ESP_OK)
        return err;
    if ((err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, NULL)) != ESP_OK)
        return err;
    wifi_config_t ap = {0};
    const char *ssid = arcade_guest_network() ? ARCADE_GUEST_SSID : identity.ssid;
    const char *password = arcade_guest_network() ? ARCADE_GUEST_PASSWORD : identity.ap_password;
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    strlcpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.capable = true;
    if ((err = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK)
        return err;
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &ap)) != ESP_OK)
        return err;
    if ((err = esp_wifi_start()) != ESP_OK)
        return err;
    if ((err = esp_wifi_set_max_tx_power(CONFIG_ENTRANCE_WIFI_TX_POWER)) != ESP_OK)
        return err;
    wifi_config_t actual = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &actual) == ESP_OK)
        ESP_LOGI("network", "Hotspot %.*s, channel %u, hidden %u", actual.ap.ssid_len,
                 actual.ap.ssid, actual.ap.channel, actual.ap.ssid_hidden);
    esp_wifi_set_ps(WIFI_PS_NONE);
    maintenance = !settings_healthy;
    /* An uplink error must never prevent the independent local API starting. */
    if (net_apply() != ESP_OK)
        diag_event("Uplink startup failed; local hotspot remains available");
    return ESP_OK;
}

bool net_online(void)
{
    return online;
}
bool net_ready(void)
{
    return ready;
}
esp_err_t net_refresh_ap(void)
{
    if (!network_mutex)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(network_mutex, portMAX_DELAY);
    wifi_config_t ap = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK) {
        const char *ssid = arcade_guest_network() ? ARCADE_GUEST_SSID : identity.ssid;
        const char *password =
            arcade_guest_network() ? ARCADE_GUEST_PASSWORD : identity.ap_password;
        strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = strlen(ssid);
        strlcpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    xSemaphoreGive(network_mutex);
    return err == ESP_OK ? net_apply() : err;
}
void net_maintenance(void)
{
    maintenance = true;
    arcade_maintenance();
    net_refresh_ap();
    diag_event("Maintenance mode: uplink disabled until reboot; hotspot retained");
}
cJSON *net_status(void)
{
    settings_t snapshot = settings_snapshot();
    char address[20];
    portENTER_CRITICAL(&ip_mutex);
    strlcpy(address, station_ip, sizeof(address));
    portEXIT_CRITICAL(&ip_mutex);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ap_ssid",
                            arcade_guest_network() ? ARCADE_GUEST_SSID : identity.ssid);
    cJSON_AddStringToObject(j, "ap_ip", "192.168.4.1");
    cJSON_AddStringToObject(j, "station_ip", address);
    cJSON_AddStringToObject(j, "station_ssid", snapshot.ssid);
    cJSON_AddBoolToObject(j, "enabled", snapshot.enabled);
    cJSON_AddBoolToObject(j, "paused_for_arcade", arcade_guest_network() && !relay_enabled());
    cJSON_AddNumberToObject(j, "settings_generation", snapshot.generation);
    cJSON_AddNumberToObject(j, "settings_schema", snapshot.schema);
    cJSON_AddBoolToObject(j, "online", online);
    cJSON_AddBoolToObject(j, "maintenance", maintenance);
    cJSON_AddNumberToObject(j, "disconnect_reason", disconnect_reason);
    cJSON_AddNumberToObject(j, "unix_time", time(NULL));
    cJSON_AddBoolToObject(j, "time_valid", time(NULL) >= 1700000000);
    const char *status = !snapshot.enabled ? "Uplink disabled"
                         : (arcade_guest_network() && !relay_enabled())
                             ? "Uplink paused for local arcade play"
                         : maintenance ? "Disabled in maintenance mode"
                         : online      ? "Connected"
                                       : "Connecting; local hotspot remains available";
    if (snapshot.enabled && !online && !maintenance &&
        (!arcade_guest_network() || relay_enabled())) {
        if (disconnect_reason == WIFI_REASON_NO_AP_FOUND)
            status = "Network not found; check its name and 2.4 GHz band";
        if (disconnect_reason == WIFI_REASON_AUTH_FAIL ||
            disconnect_reason == WIFI_REASON_HANDSHAKE_TIMEOUT)
            status = "Authentication failed; verify the uplink password";
        if (disconnect_reason == WIFI_REASON_AUTH_EXPIRE)
            status = "Access point did not complete Wi-Fi authentication; trying alternatives";
        if (disconnect_reason == WIFI_REASON_BEACON_TIMEOUT)
            status = "Uplink signal lost; reconnecting";
    }
    cJSON_AddStringToObject(j, "station_detail", status);
    wifi_ap_record_t ap;
    if (online && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        cJSON_AddNumberToObject(j, "rssi", ap.rssi);
    return j;
}
