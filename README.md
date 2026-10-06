# esp-emu repro: Bluedroid (BTDM) and soft-AP provisioning on ESP32-S31

Minimal ESP-IDF app for [espressif/esp-emulator#17](https://github.com/espressif/esp-emulator/issues/17),
laid out like `test_apps/`. It configures Bluetooth and Wi-Fi the way a
firmware that needs Classic BT (A2DP) and provisions over its own soft AP does:

1. `WIFI_AP_STA` soft AP `repro-setup` (192.168.4.1) with an `esp_http_server` on port 80.
2. Bluedroid on the BTDM controller (`CONFIG_BTDM_CTRL_MODE_BTDM`, BLE + Classic,
   A2DP enabled), one GATT service (0x00FF / 0xFF01) and legacy advertising.

Each step logs a `REPRO_*` marker; a separate task prints `REPRO_SUMMARY` after 30 s.

## Build

ESP-IDF v6.1 (`release/v6.1`, tested at `fff9895c82d744c7237be8847347bdd1b07c6643`):

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py --preview merge-bin -o merged.bin
```

The app needs a 3 MB factory partition (`partitions.csv`, 4 MB flash): Bluedroid
with Classic BT does not fit the 1.5 MB single-app layout.

## Run (esp-emu 0.46.0)

```sh
esp-emu --chip esp32s31 --firmware build/merged.bin --elf build/s31_bluedroid_softap.elf \
  --net "user,hostfwd=tcp:127.0.0.1:18080-:80" --timeout 50s
# in another terminal, while it runs:
curl -m 8 http://127.0.0.1:18080/
```

## Observed

```
[WARN  periph::ble_hci] BLE: BTDM controller without the NimBLE transport (hci_driver_host_*) is not modelled; BLE left unpatched
I (266) repro: REPRO_SOFTAP_UP ssid=repro-setup ip=192.168.4.1
I (266) repro: REPRO_HTTPD_UP port=80
I (266) repro: REPRO_BT_CONTROLLER_INIT ...
I (366) BTDM_INIT: BTDM controller init OK
I (366) repro: REPRO_BT_CONTROLLER_ENABLE (BTDM) ...
I (366) repro: REPRO_BT_CONTROLLER_ENABLED
E (5206) task_wdt: Task watchdog got triggered. ...
E (5206) task_wdt: CPU 0: btdm
E (10206) task_wdt: CPU 0: btdm
...
```

- **BTDM:** `esp_bt_controller_enable(ESP_BT_MODE_BTDM)` returns, then the `btdm`
  task spins on CPU 0 inside the controller library (MEPC in
  `r_sym_bt_KrvfcwDw4eZoaTPVdFj5`, RA in `r_sym_ble_I7hG9nRpY0HKGB9EK0hX`).
  `esp_bluedroid_init()` never completes (`REPRO_BLUEDROID_ENABLED` and
  `REPRO_BLE_ADVERTISING` never appear), and with CPU 0 starved not even
  `REPRO_SUMMARY` is printed.
- **Soft AP:** the AP and the HTTP server are up, but `curl http://127.0.0.1:18080/`
  gets no answer (`HTTP 000`): `hostfwd` reaches the station side, which is not
  associated during provisioning, not the firmware's own soft-AP network.

## Expected

- BLE advertising `repro` and the GATT service, ideally forwardable to Bumble via
  `--ble-hci` like the NimBLE transport.
- A way to reach the firmware's soft-AP network from the host (e.g. `hostfwd`
  into the soft-AP netif, or a virtual station associated with it), so
  `curl` returns `hello from the soft AP`.
