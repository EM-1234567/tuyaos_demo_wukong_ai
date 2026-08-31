/**
 * @file tuya_bluez_api.c
 * @brief Bridge between the TKL BLE layer and the two stacks this board uses.
 *
 * Split of responsibilities (do not "unify" these — see BLE_NETCFG_NOTES.md):
 *   - Advertising: raw legacy HCI (tuya_hci.c). BlueZ LEAdvertisingManager1
 *     registers fine on RTL8733BU but the controller never airs the packets,
 *     and registering it makes mgmt claim advertising, after which raw HCI
 *     LE Set Adv is rejected with 0x0C Command Disallowed. So we deliberately
 *     never register a BlueZ advertisement, which keeps mgmt adv-free.
 *   - GATT: BlueZ D-Bus GattManager1 (tuya_gatt.c).
 */
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tuya_bluez_compat.h"

#include "tuya_bluez_api.h"
#include "tuya_hci.h"
#include "tuya_gatt.h"

/*
 * Field diagnostics. Off by default: the dump forks 4-5 short-lived processes
 * (pidof / hciconfig / dbus-send / ps) every time it runs, which is pure cost
 * on the provisioning path. Rebuild with -DTUYA_BLE_DEBUG=1 to get it back.
 */
#ifndef TUYA_BLE_DEBUG
#define TUYA_BLE_DEBUG 0
#endif

static int g_bluez_inited = FALSE;
static GMainLoop *main_loop;

#if TUYA_BLE_DEBUG
/**
 * @brief Dump host BLE/D-Bus environment (dbus, bluetoothd, hci0, BlueZ ifaces)
 * @param[in] tag caller context string
 */
static void __ble_env_dump(const char *tag)
{
    static const char *const probes[] = {
        "pidof dbus-daemon 2>/dev/null",
        "pidof bluetoothd 2>/dev/null",
        "hciconfig hci0 2>/dev/null | head -6",
        "dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0 "
        "org.freedesktop.DBus.Introspectable.Introspect 2>/dev/null "
        "| tr '<>' '\\n' | grep -E 'LEAdvertising|Adapter1|GattManager' | head -12",
    };
    char line[256];
    unsigned int i;

    PR_INFO("==== BLE ENV DUMP begin (%s) ====", tag ? tag : "-");
    PR_INFO("dbus socket: %s",
            (access("/run/dbus/system_bus_socket", F_OK) == 0) ? "EXISTS" : "MISSING");
    PR_INFO("machine-id: %s",
            (access("/var/lib/dbus/machine-id", R_OK) == 0) ? "EXISTS" : "MISSING");
    PR_INFO("hci0 sysfs: %s",
            (access("/sys/class/bluetooth/hci0", F_OK) == 0) ? "EXISTS" : "MISSING");

    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        FILE *fp = popen(probes[i], "r");
        int n = 0;

        if (fp == NULL) {
            continue;
        }
        while ((fgets(line, sizeof(line), fp) != NULL) && (n < 12)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] != '\0') {
                PR_INFO("  %s", line);
            }
            n++;
        }
        if (n == 0) {
            PR_WARN("  (no output) %.40s", probes[i]);
        }
        pclose(fp);
    }
    PR_INFO("==== BLE ENV DUMP end (%s) ====", tag ? tag : "-");
}
#else
#define __ble_env_dump(tag) do { (void)(tag); } while (0)
#endif

static void *__loop_run(void *arg)
{
    g_main_loop_run((GMainLoop *)arg);

    return NULL;
}

int tuya_bluez_init(void)
{
    int ret = 0;
    pthread_t tid;

    if (g_bluez_inited) {
        PR_INFO("tuya bluez already initialized");
        return 0;
    }

    PR_INFO("tuya_bluez_init start");
    __ble_env_dump("pre-init");

    main_loop = g_main_loop_new(NULL, FALSE);

    ret = tuya_gatt_init();
    if (ret != 0) {
        PR_ERR("tuya_gatt_init error %d", ret);
        g_main_loop_unref(main_loop);
        main_loop = NULL;
        return ret;
    }

    if (pthread_create(&tid, NULL, __loop_run, main_loop) != 0) {
        PR_ERR("g_main_loop thread create failed");
        g_main_loop_unref(main_loop);
        main_loop = NULL;
        return LE_COM_ERROR;
    }
    pthread_detach(tid);

    g_bluez_inited = TRUE;
    PR_INFO("tuya_bluez_init OK (GATT via BlueZ, ADV via raw HCI)");
    __ble_env_dump("post-init");

    return 0;
}

int tuya_bluez_deinit(void)
{
    return 0;
}

int tuya_bluez_le_set_adv_params(le_set_adv_params_t *params)
{
    int ret = 0;

    if (params == NULL) {
        return LE_INVALID_PARAM;
    }

    /*
     * Stop advertising before reprogramming parameters: the controller rejects
     * LE Set Adv Params while advertising is enabled. If mgmt still holds the
     * adv slot the command comes back non-zero — that is not fatal, we fall
     * back to bluetoothd's defaults and let the caller carry on to Set Data +
     * Enable, so return success either way.
     *
     * The 50ms gap between Disable and Set Params is deliberate and was added
     * to settle a gdbus race crash; it is cheap, so keep it.
     */
    tuya_hci_le_set_adv_enable(false);
    usleep(50 * 1000);
    ret = tuya_hci_le_set_adv_params(params->min_interval, params->max_interval, params->advtype);
    if (ret != LE_SUCCESS) {
        PR_WARN("adv: Set Adv Params failed (%d), using controller defaults", ret);
    }
    return LE_SUCCESS;
}

int tuya_bluez_le_set_adv_enable(bool enable)
{
    return tuya_hci_le_set_adv_enable(enable);
}

int tuya_bluez_le_set_adv_data(uint8_t *data, uint8_t len)
{
    if ((data == NULL) || (len == 0)) {
        return LE_INVALID_PARAM;
    }
    return tuya_hci_le_set_adv_data(data, len);
}

int tuya_bluez_le_set_scan_rsp_data(uint8_t *data, uint8_t len)
{
    if ((data == NULL) || (len == 0)) {
        return LE_INVALID_PARAM;
    }
    return tuya_hci_le_set_scan_rsp_data(data, len);
}

int tuya_bluez_le_add_gatt_service(le_gatt_service_t *service, uint8_t service_num)
{
    int i = 0, j = 0;
    uint16_t svc_uuid = 0x0000;
    le_gatt_characteristic_t *p_chr = NULL;
    int registered = 0;

    if ((service == NULL) || (service_num == 0)) {
        return LE_INVALID_PARAM;
    }

    /*
     * TAL calls gatts_service_add BEFORE tkl_ble_stack_init, so BlueZ/D-Bus
     * must be brought up here. Otherwise every register fails and
     * tal_ble_bt_init maps that to OPRT_OS_ADAPTER_BLE_INIT_FAILED — the stack
     * never inits and advertising never starts.
     */
    if (tuya_bluez_init() != 0) {
        PR_ERR("tuya_bluez_init failed before gatt register");
        return LE_COM_ERROR;
    }

    PR_INFO("register gatt service num: %u", service_num);

    for (i = 0; i < service_num; i++) {
        svc_uuid = service[i].uuid;
        if (tuya_gatt_register_service(svc_uuid) != 0) {
            PR_ERR("register service 0x%04x failed", svc_uuid);
            continue;
        }
        p_chr = service[i].chr;
        for (j = 0; j < service[i].chr_num; j++) {
            uint16_t desc_uuid = 0;
            uint8_t desc_props = 0;

            if (p_chr[j].property & (LE_GATT_CHR_PROP_NOTIFY | LE_GATT_CHR_PROP_INDICATE)) {
                /* CCCD: many phone stacks require it before StartNotify works. */
                desc_uuid = 0x2902;
                desc_props = LE_GATT_CHR_PROP_READ | LE_GATT_CHR_PROP_WRITE;
            }
            if (tuya_gatt_register_characteristic(svc_uuid, p_chr[j].uuid, p_chr[j].property,
                                                  desc_uuid, desc_props) != 0) {
                PR_ERR("register chr %s failed", (const char *)p_chr[j].uuid);
                break;
            }
        }
        registered++;
    }

    /* Register with BlueZ only after local objects exist (else "No object received"). */
    if (registered > 0) {
        tuya_gatt_register_application();
        return LE_SUCCESS;
    }
    return LE_COM_ERROR;
}

int tuya_bluez_le_gatts_value_notify(uint16_t uuid, uint8_t *value, uint16_t len)
{
    return tuya_gatt_server_send_characteristic_notification(uuid, value, len);
}

void tuya_bluez_le_register_connect_event(void (*cb)(int status))
{
    tuya_gatt_register_connect_event(cb);
}

void tuya_bluez_le_register_write_req_event(void (*cb)(uint16_t uuid, uint8_t *data, uint16_t len))
{
    tuya_gatt_register_write_req_event(cb);
}
