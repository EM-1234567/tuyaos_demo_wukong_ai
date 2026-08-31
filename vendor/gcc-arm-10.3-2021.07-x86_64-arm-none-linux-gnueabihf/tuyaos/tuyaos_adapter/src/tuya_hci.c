/**
 * @file tuya_hci.c
 * @brief Legacy HCI LE advertising helpers for the BlueZ adapter.
 *
 * Advertising deliberately bypasses BlueZ and talks to the controller over a
 * raw HCI socket — see tuya_bluez_api.c for why. All four commands share the
 * same open / send-with-retry / close shape, factored into __hci_cmd().
 *
 * @version 1.0
 * @date 2026-07-24
 * @copyright Copyright (c) Tuya Inc.
 */
#include "tuya_hci.h"
#include <stdio.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "tuya_bluez_compat.h"
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define HCI_LE_ADV_DATA_MAX_LEN 31
#define HCI_CMD_TIMEOUT_MS      1000

/*
 * Retry budget for a single HCI command.
 *
 * The controller answers 0x12 (Controller Busy) while bluetoothd is still
 * settling the adapter, so a few retries genuinely help. But the budget has to
 * stay bounded: this runs on the provisioning path, and the previous code slept
 * 10 x 300ms in EACH of two failure branches, i.e. up to ~6s per command and
 * ~24s for a full adv_start sequence.
 */
#define HCI_CMD_RETRY_MAX       8
#define HCI_CMD_RETRY_DELAY_US  (200 * 1000)

/* Dump adv/scan-rsp payload bytes. Off by default (this is per adv update). */
#ifndef TUYA_BLE_DEBUG
#define TUYA_BLE_DEBUG 0
#endif

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */
/**
 * @brief Map HCI Command Complete status byte to an adapter error code
 * @param[in] status HCI status byte
 * @return LE_SUCCESS or LE_HCI_STATUS_ERROR
 */
static int __hci_status_to_ret(uint8_t status)
{
    return (status == 0x00) ? LE_SUCCESS : LE_HCI_STATUS_ERROR;
}

/**
 * @brief Human-readable outcome for logs.
 * @param[in] ret value returned by __hci_cmd()
 * @param[in] status HCI status byte (only meaningful when the command was
 *            actually delivered)
 * @note Must key off ret first: when the send itself fails the status byte was
 *       never written by the controller and stays 0, which would otherwise be
 *       printed as a cheerful "OK" next to an error return.
 */
static const char *__hci_outcome_str(int ret, uint8_t status)
{
    if (ret == LE_SUCCESS) {
        return "OK";
    }
    if (ret == LE_OPEN_ERROR) {
        return "OPEN-FAIL";
    }
    if (ret == LE_READ_ERROR) {
        return "SEND-FAIL";
    }
    switch (status) {
    case 0x0C: return "DISALLOWED";
    case 0x12: return "BUSY";
    default:   return "STATUS-ERR";
    }
}

/**
 * @brief Map Tuya TKL adv_type encoding to the Bluetooth HCI Adv_Type field
 * @param[in] tkl_type TKL advertising type
 * @return HCI Adv_Type value
 * @note TKL and HCI use DIFFERENT encodings — passing TKL values straight
 *       through turns CONN_SCANNABLE_UNDIRECTED (TKL 0x01) into ADV_DIRECT_IND
 *       (HCI 0x01), which is directed at the zero address and therefore
 *       invisible to every scanner.
 */
static uint8_t __tkl_to_hci_adv_type(uint8_t tkl_type)
{
    switch (tkl_type) {
    case 0x01: return 0x00; /* CONN_SCANNABLE_UNDIRECTED  -> ADV_IND */
    case 0x02: return 0x01; /* CONN_NONSCANNABLE_DIR_HIGH -> ADV_DIRECT_IND */
    case 0x03: return 0x04; /* CONN_NONSCANNABLE_DIR_LOW  -> ADV_DIRECT_IND_LOW */
    case 0x04: return 0x02; /* NONCONN_SCANNABLE_UNDIR    -> ADV_SCAN_IND */
    case 0x05: return 0x03; /* NONCONN_NONSCANNABLE_UNDIR -> ADV_NONCONN_IND */
    default:   return 0x00; /* unknown -> ADV_IND (safest, scannable by all) */
    }
}

/**
 * @brief Open hci0, run one LE control command with retries, close.
 * @param[in] ocf OCF within OGF_LE_CTL
 * @param[in] cparam command parameter block
 * @param[in] clen command parameter length
 * @param[in] tag short name used in log lines
 * @param[out] p_status HCI status byte (may be NULL)
 * @return LE_SUCCESS, or LE_OPEN_ERROR / LE_READ_ERROR / LE_HCI_STATUS_ERROR
 * @note Both "send failed" and "non-zero status" are retried by the same loop.
 *       The old code re-sent the command one extra time after a retry had
 *       already succeeded, which duplicated every recovered command.
 */
static int __hci_cmd(uint16_t ocf, void *cparam, int clen, const char *tag, uint8_t *p_status)
{
    struct hci_request req;
    uint8_t status = 0;
    int device = -1;
    int ret = LE_SUCCESS;
    int attempt = 0;

    device = hci_open_dev(hci_get_route(NULL));
    if (device < 0) {
        PR_ERR("hci: %s open device FAILED", tag);
        return LE_OPEN_ERROR;
    }

    memset(&req, 0, sizeof(req));
    req.ogf = OGF_LE_CTL;
    req.ocf = ocf;
    req.cparam = cparam;
    req.clen = clen;
    req.rparam = &status;
    req.rlen = 1;

    for (attempt = 0; attempt <= HCI_CMD_RETRY_MAX; attempt++) {
        if (attempt > 0) {
            usleep(HCI_CMD_RETRY_DELAY_US);
        }

        status = 0;
        if (hci_send_req(device, &req, HCI_CMD_TIMEOUT_MS) != 0) {
            ret = LE_READ_ERROR;
            PR_WARN("hci: %s send failed (attempt %d/%d)", tag, attempt + 1, HCI_CMD_RETRY_MAX + 1);
            continue;
        }

        ret = __hci_status_to_ret(status);
        if (ret == LE_SUCCESS) {
            break;
        }
        PR_WARN("hci: %s status=0x%02x (%s) attempt %d/%d", tag, status,
                __hci_outcome_str(ret, status), attempt + 1, HCI_CMD_RETRY_MAX + 1);
    }

    hci_close_dev(device);

    if (p_status != NULL) {
        *p_status = status;
    }
    return ret;
}

#if TUYA_BLE_DEBUG
static void __hci_dump_payload(const char *tag, const uint8_t *data, uint8_t len)
{
    char hex[HCI_LE_ADV_DATA_MAX_LEN * 3 + 1] = {0};
    int i, n = 0;

    for (i = 0; i < len; i++) {
        n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02x ", data[i]);
    }
    PR_INFO("hci: %s payload [%u] %s", tag, len, hex);
}
#else
#define __hci_dump_payload(tag, data, len) do { (void)(tag); (void)(data); (void)(len); } while (0)
#endif

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
/**
 * @brief Set LE advertising parameters via legacy HCI
 * @param[in] min_interval minimum advertising interval (0.625ms units)
 * @param[in] max_interval maximum advertising interval (0.625ms units)
 * @param[in] advtype TKL advertising type (mapped to HCI encoding)
 * @return LE_SUCCESS on success, error code on failure
 */
int tuya_hci_le_set_adv_params(uint16_t min_interval, uint16_t max_interval, uint8_t advtype)
{
    le_set_advertising_parameters_cp cp;
    uint8_t status = 0;
    int ret;

    memset(&cp, 0, sizeof(cp));
    cp.advtype = __tkl_to_hci_adv_type(advtype);
    cp.min_interval = htobs(min_interval);
    cp.max_interval = htobs(max_interval);
    cp.chan_map = 7;

    ret = __hci_cmd(OCF_LE_SET_ADVERTISING_PARAMETERS, &cp, LE_SET_ADVERTISING_PARAMETERS_CP_SIZE,
                    "Set Adv Params", &status);
    PR_INFO("hci: LE Set Adv Params status=0x%02x (%s) ret=%d [type=%u->%u min=%u max=%u]", status,
            __hci_outcome_str(ret, status), ret, advtype, cp.advtype, min_interval, max_interval);
    return ret;
}

/**
 * @brief Enable or disable LE advertising via legacy HCI
 * @param[in] enable true to enable, false to disable
 * @return LE_SUCCESS on success, error code on failure
 */
int tuya_hci_le_set_adv_enable(bool enable)
{
    le_set_advertise_enable_cp cp;
    uint8_t status = 0;
    int ret;

    memset(&cp, 0, sizeof(cp));
    cp.enable = enable ? 1 : 0;

    ret = __hci_cmd(OCF_LE_SET_ADVERTISE_ENABLE, &cp, LE_SET_ADVERTISE_ENABLE_CP_SIZE,
                    enable ? "Set Adv Enable" : "Set Adv Disable", &status);
    PR_INFO("hci: LE Set Adv %s status=0x%02x (%s) ret=%d", enable ? "Enable" : "Disable", status,
            __hci_outcome_str(ret, status), ret);
    return ret;
}

/**
 * @brief Set LE advertising data via legacy HCI
 * @param[in] data advertising payload
 * @param[in] len payload length (1..31)
 * @return LE_SUCCESS on success, error code on failure
 */
int tuya_hci_le_set_adv_data(uint8_t *data, uint8_t len)
{
    le_set_advertising_data_cp cp;
    uint8_t status = 0;
    int ret;

    if ((data == NULL) || (len == 0) || (len > HCI_LE_ADV_DATA_MAX_LEN)) {
        return LE_INVALID_PARAM;
    }
    __hci_dump_payload("adv data", data, len);

    memset(&cp, 0, sizeof(cp));
    memcpy(cp.data, data, len);
    cp.length = len;

    ret = __hci_cmd(OCF_LE_SET_ADVERTISING_DATA, &cp, LE_SET_ADVERTISING_DATA_CP_SIZE,
                    "Set Adv Data", &status);
    PR_INFO("hci: LE Set Adv Data status=0x%02x (%s) ret=%d len=%u", status,
            __hci_outcome_str(ret, status), ret, len);
    return ret;
}

/**
 * @brief Set LE scan response data via legacy HCI
 * @param[in] data scan response payload
 * @param[in] len payload length (1..31)
 * @return LE_SUCCESS on success, error code on failure
 */
int tuya_hci_le_set_scan_rsp_data(uint8_t *data, uint8_t len)
{
    le_set_scan_response_data_cp cp;
    uint8_t status = 0;
    int ret;

    if ((data == NULL) || (len == 0) || (len > HCI_LE_ADV_DATA_MAX_LEN)) {
        return LE_INVALID_PARAM;
    }
    __hci_dump_payload("scan rsp", data, len);

    memset(&cp, 0, sizeof(cp));
    memcpy(cp.data, data, len);
    cp.length = len;

    ret = __hci_cmd(OCF_LE_SET_SCAN_RESPONSE_DATA, &cp, LE_SET_SCAN_RESPONSE_DATA_CP_SIZE,
                    "Set Scan Rsp", &status);
    PR_INFO("hci: LE Set Scan Rsp status=0x%02x (%s) ret=%d len=%u", status,
            __hci_outcome_str(ret, status), ret, len);
    return ret;
}
