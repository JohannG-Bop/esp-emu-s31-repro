// esp-emu repro for ESP32-S31:
//  1. WIFI_AP_STA soft AP (192.168.4.1) + esp_http_server on port 80
//     -> is the page reachable from the host?
//  2. Bluedroid on the BTDM controller (BLE + Classic), one GATT service,
//     legacy advertising -> does esp_bt_controller_enable() return?
// Each step logs a REPRO_* marker; REPRO_SUMMARY is printed by a watchdog
// task after 30 s whether or not the BT steps finished.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"

static const char *TAG = "repro";
static volatile int s_softap_ok, s_httpd_ok, s_bt_init_ok, s_bt_enable_ok, s_bluedroid_ok, s_adv_ok;

// ---- 1. soft AP + HTTP ------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "REPRO_HTTP_REQUEST %s", req->uri);
    return httpd_resp_sendstr(req, "hello from the soft AP\n");
}

static void start_softap_and_httpd(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap = {
        .ap = {
            .ssid = "repro-setup",
            .ssid_len = strlen("repro-setup"),
            .password = "repro-setup-pass",
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 1,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_softap_ok = 1;
    ESP_LOGI(TAG, "REPRO_SOFTAP_UP ssid=repro-setup ip=192.168.4.1");

    httpd_handle_t server = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = 80;
    ESP_ERROR_CHECK(httpd_start(&server, &hc));
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    s_httpd_ok = 1;
    ESP_LOGI(TAG, "REPRO_HTTPD_UP port=80");
}

// ---- 2. Bluedroid GATT server + advertising ---------------------------------

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x40,
    .adv_int_max = 0x80,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT) {
        esp_ble_gap_start_advertising(&s_adv_params);
    } else if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT) {
        s_adv_ok = param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
        ESP_LOGI(TAG, "REPRO_BLE_ADVERTISING status=%d", param->adv_start_cmpl.status);
    }
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        esp_gatt_srvc_id_t service = {
            .is_primary = true,
            .id = { .inst_id = 0, .uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = 0x00FF } } },
        };
        esp_ble_gatts_create_service(gatts_if, &service, 4);
    } else if (event == ESP_GATTS_CREATE_EVT) {
        uint16_t handle = param->create.service_handle;
        esp_ble_gatts_start_service(handle);
        esp_bt_uuid_t ch = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = 0xFF01 } };
        esp_ble_gatts_add_char(handle, &ch, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE, NULL, NULL);
    } else if (event == ESP_GATTS_START_EVT) {
        ESP_LOGI(TAG, "REPRO_GATT_SERVICE_STARTED");
        static uint8_t adv[] = { 2, 0x01, 0x06, 6, 0x09, 'r', 'e', 'p', 'r', 'o', 3, 0x03, 0xFF, 0x00 };
        esp_ble_gap_config_adv_data_raw(adv, sizeof(adv));
    }
}

static void bt_task(void *arg)
{
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "REPRO_BT_CONTROLLER_INIT ...");
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    s_bt_init_ok = 1;
    ESP_LOGI(TAG, "REPRO_BT_CONTROLLER_ENABLE (BTDM) ...");
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    s_bt_enable_ok = 1;
    ESP_LOGI(TAG, "REPRO_BT_CONTROLLER_ENABLED");
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    s_bluedroid_ok = 1;
    ESP_LOGI(TAG, "REPRO_BLUEDROID_ENABLED");
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));
    vTaskDelete(NULL);
}

static void summary_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(30000));
    ESP_LOGI(TAG, "REPRO_SUMMARY softap=%d httpd=%d bt_init=%d bt_enable=%d bluedroid=%d ble_adv=%d",
             s_softap_ok, s_httpd_ok, s_bt_init_ok, s_bt_enable_ok, s_bluedroid_ok, s_adv_ok);
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    xTaskCreate(summary_task, "summary", 3072, NULL, 5, NULL);
    start_softap_and_httpd();
    xTaskCreate(bt_task, "bt", 6144, NULL, 5, NULL);
}
