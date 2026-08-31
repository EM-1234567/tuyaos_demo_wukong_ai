/**
 * @file tuya_app_main.c
 * @author www.tuya.com
 * @brief Tuya AI toy (Wukong) application main entry and SOC device lifecycle.
 *
 * This file implements the application entry point and the SOC (System on Chip) device
 * lifecycle: event subscription (reset, link up/down, MQTT), authorization (UUID/AUTHKEY
 * or MF production test), IoT callback registration (upgrade, DP object/raw, DP query,
 * cloud status), WiFi/wired/cellular init, AI toy init, and optional Tuya UI. It also
 * provides MF (manufacturing) UART and product-test callbacks used when authorization
 * is burned via the Tuya Cloud module tool.
 *
 * Main flow:
 * - main() / tuya_app_main() creates the application thread tuya_app_thread.
 * - tuya_app_thread runs tuya_base_utilities_init(), LWIP (if enabled), then user_main().
 * - user_main() does reset_netconfig init, (optional) cellular boot, Tuya IoT params/DB
 *   init, log config, then __soc_device_init() which registers all IoT callbacks and
 *   inits AI toy and UI.
 *
 * @version 0.1
 * @date 2022-10-28
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tuya_cloud_types.h"
#include "tuya_svc_netmgr.h"
#if defined(ENABLE_TUYA_LAN) && (ENABLE_TUYA_LAN == 1)
#include "tuya_svc_lan.h"
#endif
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
#include "tuya_iot_wifi_api.h"
#include "tal_wifi.h"
#endif
#if defined(ENABLE_BT_SERVICE) && (ENABLE_BT_SERVICE == 1)
#include "tuya_bt.h"
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
#include "tuya_iot_base_api.h"
#endif
#if defined(ENABLE_CELLULAR_DONGLE) && (ENABLE_CELLULAR_DONGLE == 1)
#include "tuya_ai_cellular.h"
#include "tuya_ai_linkpolicy.h"
#endif
#if defined(ENABLE_CELLULAR) && (ENABLE_CELLULAR == 1)
#include "tuya_cellular_cloud.h"
#endif

#include "tuya_iot_com_api.h"
#include "tuya_ws_db.h"
#include "tal_system.h"
#include "tal_sw_timer.h"
#include "tal_sleep.h"
#include "tal_log.h"
#include "base_event.h"
#include "mf_test.h"
#include "mqc_app.h"
#include "log_seq.h"
#if defined(ENABLE_LWIP) && (ENABLE_LWIP == 1)
#include "lwip_init.h"
#endif

#include "tal_uart.h"
#include "tuya_device_cfg.h"
#include "tuya_ai_toy.h"
#include "tuya_ai_toy_key.h"
#if defined(ENABLE_BATTERY) && (ENABLE_BATTERY == 1)
#include "tuya_ai_battery.h"
#endif
#if (defined(ENABLE_PRODUCT_AUTOTEST) && (ENABLE_PRODUCT_AUTOTEST == 1))
#include "tuya_ai_toy_mf_test.h"
#endif
#include "tuya_iot_internal_api.h"
#include "tuya_device_board.h"
#if defined(ENABLE_TUYA_UI) && (ENABLE_TUYA_UI == 1)
// #include "tuya_app_gui_config.h"
#include "tuya_ai_display.h"
// #include "tuya_app_gui_gw_core0.h"
// #include "tuya_app_gui_fs_path.h"
// #include "tuya_list.h"
// #include "app_ipc_command.h"
// #include "smart_frame.h"
// #include "img_direct_flush.h"
// #ifdef TUYA_TFTP_SERVER
// #include "tuya_app_gui_tftp.h"
// #endif
#endif
#include "tuya_app_config.h"
#if defined(CODEC_BENCH_TEST) && (CODEC_BENCH_TEST == 1)
#include "codec_bench.h"
#endif

/* ---------------------------------------------------------------------------
 * Macro definitions
 * --------------------------------------------------------------------------- */

/**
 * Product ID from Tuya IoT platform. Used by tuya_iot_wf_soc_dev_init() when PID is
 * defined; when both UUID and AUTHKEY are defined, authorization is set in code
 * instead of via MF (production test) tool.
 */

 #define PID            "qwibzfanwyhzqpev"

 /* Cloud authorization embedded in code (software authorization).
  * With UUID+AUTHKEY defined, __soc_device_init() takes the set_gw_prod_info()
  * path instead of the MF production-test path. Burn via MF tool instead by
  * commenting these two lines out. */
 #define UUID           "uuida9fb6337c4540722"
 #define AUTHKEY        "YfnQSkn0HBX4A5qe1MNng037qS5ikBV9"


/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
extern void tuya_ble_enable_debug(bool enable);

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */

/** Handle of the application main thread (tuya_app_thread). Cleared when thread exits. */
STATIC THREAD_HANDLE ty_app_thread = NULL;

/* ---------------------------------------------------------------------------
 * QR code helpers (cellular / QR code active only)
 * --------------------------------------------------------------------------- */
#if (defined(ENABLE_QRCODE_ACTIVE) && (ENABLE_QRCODE_ACTIVE == 1))
extern INT_T qrcode_exec(INT_T argc, CHAR_T **argv);

/**
 * @brief Print a QR code for the given message using qrcode_exec.
 * @param[in] msg String to encode (e.g. short URL). Not modified.
 * @return Return value of qrcode_exec().
 */
STATIC INT_T __qrcode_printf(CHAR_T *msg)
{
    CHAR_T *qrcode_argv[] = {
        "qrcode_exec", "-m", "3", "-t", "ansiutf8", msg
    };

    return qrcode_exec((INT_T)(sizeof(qrcode_argv) / sizeof(qrcode_argv[0])), qrcode_argv);
}

/**
 * @brief Callback when TuyaOS provides the activation short URL (e.g. for cellular or QR code active).
 * Parses the short URL JSON, extracts the "shortUrl" field and prints its QR code via __qrcode_printf().
 * @param[in] shorturl JSON string containing short URL (e.g. {"shortUrl":"https://..."}). Can be NULL.
 */
STATIC VOID __qrcode_active_shorturl_cb(CONST CHAR_T *shorturl)
{
    if (NULL == shorturl) {
        return;
    }

    TAL_PR_DEBUG("shorturl : %s", shorturl);

#if OPERATING_SYSTEM == SYSTEM_LINUX
    ty_cJSON *item = ty_cJSON_Parse(shorturl);
    if (NULL == item) {
        return;
    }
    ty_cJSON *url_obj = ty_cJSON_GetObjectItem(item, "shortUrl");
    if (NULL != url_obj && NULL != url_obj->valuestring) {
        __qrcode_printf(url_obj->valuestring);
    }
    ty_cJSON_Delete(item);
#endif
}

#endif

/* ---------------------------------------------------------------------------
 * SOC device IoT callbacks (upgrade, status, DP, reset, network)
 * --------------------------------------------------------------------------- */

/**
 * @brief Callback when the device receives firmware upgrade information from the cloud.
 * Used for OTA; this implementation only logs the upgrade info and returns OPRT_OK.
 * @param[in] fw Firmware upgrade info (type, URL, HMAC, version, size).
 * @return OPRT_OK. See tuya_error_code.h for other codes.
 */
STATIC OPERATE_RET __soc_dev_rev_upgrade_info_cb(IN CONST FW_UG_S *fw)
{
    TAL_PR_DEBUG("SOC Rev Upgrade Info");
    TAL_PR_DEBUG("fw->tp:%d", fw->tp);
    TAL_PR_DEBUG("fw->fw_url:%s", fw->fw_url);
    TAL_PR_DEBUG("fw->fw_hmac:%s", fw->fw_hmac);
    TAL_PR_DEBUG("fw->sw_ver:%s", fw->sw_ver);
    TAL_PR_DEBUG("fw->file_size:%u", fw->file_size);

    return OPRT_OK;
}

/**
 * @brief Callback when the gateway (device) cloud connection state changes.
 * @param[in] status New cloud status (e.g. connected, disconnected).
 */
STATIC VOID_T __soc_dev_status_changed_cb(IN CONST GW_STATUS_E status)
{
    TAL_PR_DEBUG("SOC TUYA-Cloud Status:%d", status);
}

/**
 * @brief Callback when the cloud or app queries device DP (data point) state.
 * Used to report current DP values; this implementation only logs the query.
 * @param[in] dp_qry Query request: cnt and dpid[] indicate which DPs are queried; cnt==0 means all.
 */
STATIC VOID_T __soc_dev_dp_query_cb(IN CONST TY_DP_QUERY_S *dp_qry)
{
    UINT32_T index = 0;

    TAL_PR_DEBUG("SOC Rev DP Query Cmd");
    if (dp_qry->cid != NULL) {
        TAL_PR_ERR("soc not have cid.%s", dp_qry->cid);
    }

    if (dp_qry->cnt == 0) {
        TAL_PR_DEBUG("soc rev all dp query");
    } else {
        TAL_PR_DEBUG("soc rev dp query cnt:%d", dp_qry->cnt);
        for (index = 0; index < dp_qry->cnt; index++) {
            TAL_PR_DEBUG("rev dp query:%d", dp_qry->dpid[index]);
        }
    }
}

/**
 * @brief Callback when the device receives an object-format DP command from the cloud or app.
 * Forwards the command to the AI toy module for processing (e.g. volume, trigger mode).
 * @param[in] dp Received object DP packet (cmd_tp, dtt_tp, dps_cnt and DP list).
 */
STATIC VOID_T __soc_dev_obj_dp_cmd_cb(IN CONST TY_RECV_OBJ_DP_S *dp)
{
    TAL_PR_DEBUG("SOC Rev DP Obj Cmd t1:%d t2:%d CNT:%u", dp->cmd_tp, dp->dtt_tp, dp->dps_cnt);
    tuya_ai_toy_dp_process(dp);
}

/**
 * @brief Callback when the device receives a raw (transparent) DP command.
 * This implementation only logs the packet; extend to handle custom raw DP if needed.
 * @param[in] dp Raw DP packet (dpid, len, payload).
 */
STATIC VOID_T __soc_dev_raw_dp_cmd_cb(IN CONST TY_RECV_RAW_DP_S *dp)
{
    TAL_PR_DEBUG("SOC Rev DP Raw Cmd t1:%d t2:%d dpid:%d len:%u", dp->cmd_tp, dp->dtt_tp, dp->dpid, dp->len);
}

/* Delay before rebooting on unbind/factory-reset, in ms. The SDK has already flushed all
 * binding data to flash by the time gw_reset_cb runs, but other EVENT_RESET subscribers
 * (e.g. __on_ai_toy_reset) and the mqtt_thread teardown still need a moment to finish. */
#define APP_RESET_REBOOT_DELAY_MS 2000

STATIC TIMER_ID s_reset_reboot_timer = NULL;

/**
 * @brief One-shot timer callback: flush filesystem caches then reboot after an unbind or
 * factory reset. Rebooting is what makes the device re-enter netconfig: the SDK's reset
 * path only tears things down (clears gw_wsm/schema/timer/meta, stops BLE adv, drops MQTT)
 * and never restarts a netconfig channel by itself.
 */
STATIC VOID_T __on_reset_reboot_timer(TIMER_ID timer_id, VOID_T *arg)
{
    TAL_PR_NOTICE("factory reset done, rebooting to re-enter netconfig...");
    /* rootfs is squashfs but /userdata is UBIFS with write-back caching - flush before
     * resetting, otherwise the just-cleared kv data can be lost and UBIFS needs recovery
     * on the next boot. */
    sync();
    tal_system_reset();
}

/**
 * @brief Callback to inform the app that a device reset has been requested (e.g. from cloud).
 * @param[in] type Type of reset (e.g. factory reset, reboot).
 */
STATIC VOID_T __soc_dev_reset_inform_cb(GW_RESET_TYPE_E type)
{
    TAL_PR_DEBUG("reset type %d", type);

    /* Reboot only for the types that actually leave the device unbound. Deliberately NOT
     * handled here:
     *   GW_RESET_DATA_FACTORY / GW_REMOTE_RESET_DATA_FACTORY - raised while activating
     *     (cloud_reset=1, local_reset=0); rebooting there would loop the activation.
     *   GW_UNREGISTER_CLOUD_SERVICE - only asks cloud services to unregister, not an unbind. */
    switch (type) {
    case GW_LOCAL_RESET_FACTORY:
    case GW_REMOTE_UNACTIVE:
    case GW_LOCAL_UNACTIVE:
    case GW_REMOTE_RESET_FACTORY:
        break;
    default:
        return;
    }

    if (s_reset_reboot_timer == NULL) {
        if (OPRT_OK != tal_sw_timer_create(__on_reset_reboot_timer, NULL, &s_reset_reboot_timer)) {
            /* Fall back to an immediate reboot - staying up would leave the device unbound
             * with no netconfig channel, which is worse than a slightly early reset. */
            TAL_PR_ERR("create reset reboot timer failed, reboot now");
            sync();
            tal_system_reset();
            return;
        }
    }
    tal_sw_timer_start(s_reset_reboot_timer, APP_RESET_REBOOT_DELAY_MS, TAL_TIMER_ONCE);
}

/**
 * @brief Callback when network linkage or MQTT connection state changes (EVENT_LINK_UP,
 * EVENT_LINK_DOWN, EVENT_MQTT_CONNECTED). When linkage is up and MQTT is connected,
 * optionally uploads cellular ICCID if ENABLE_CELLULAR_DONGLE is set.
 * @param[in] data Event context (unused).
 * @return OPRT_OK.
 */
STATIC OPERATE_RET __soc_dev_net_status_cb(VOID *data)
{
    STATIC BOOL_T s_syn_all_status = FALSE;

    TAL_PR_DEBUG("network status changed!");
    if (tuya_svc_netmgr_linkage_is_up(LINKAGE_TYPE_DEFAULT)) {
        TAL_PR_DEBUG("linkage status changed, current status is up");
        if (get_mqc_conn_stat()) {
            TAL_PR_DEBUG("mqtt is connected!");
            if (FALSE == s_syn_all_status) {
                s_syn_all_status = TRUE;
            }
#if defined(ENABLE_CELLULAR_DONGLE) && (ENABLE_CELLULAR_DONGLE == 1)
            tuya_ai_cellular_upload_iccid();
#endif
        }
    } else {
        TAL_PR_DEBUG("linkage status changed, current status is down");
    }

    return OPRT_OK;
}

/**
 * @brief Event handler for EVENT_RESET. Not subscribed by default - the reboot on unbind is
 * driven by iot_cbs.gw_reset_cb (__soc_dev_reset_inform_cb) instead, which the SDK calls
 * after it has flushed all reset data to flash. Kept for setups that prefer the event path;
 * it delegates so both paths share the same delayed, sync()'d reboot.
 * @param[in] data Cast to GW_RESET_TYPE_E (passed as void* by event system).
 * @return OPRT_OK.
 */
STATIC OPERATE_RET __soc_dev_reset_cb(VOID *data)
{
    __soc_dev_reset_inform_cb((GW_RESET_TYPE_E)data);
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * MF (manufacturing) UART and product-test callbacks
 * Used when authorization is burned via Tuya Cloud module tool (UUID/AUTHKEY not defined).
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialize UART used for MF production test (TuYA_UART_NUM_0).
 * @param[in] baud  Baud rate requested by MF framework.
 * @param[in] bufsz Receive buffer size in bytes.
 */
VOID mf_uart_init_callback(UINT_T baud, UINT_T bufsz)
{
    TAL_UART_CFG_T cfg;
    memset(&cfg, 0, sizeof(TAL_UART_CFG_T));
    cfg.base_cfg.baudrate = baud;
    cfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    cfg.base_cfg.parity = TUYA_UART_PARITY_TYPE_NONE;
    cfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    cfg.rx_buffer_size = bufsz;

    tal_uart_init(TUYA_UART_NUM_0, &cfg);
}

/**
 * @brief De-initialize the MF UART (TUYA_UART_NUM_0). Called when leaving MF test mode.
 */
VOID mf_uart_free_callback(VOID)
{
    tal_uart_deinit(TUYA_UART_NUM_0);
}

/**
 * @brief Send data over the MF UART to the Tuya Cloud module or test tool.
 * @param[in] data Pointer to bytes to send. Must be valid for at least len bytes.
 * @param[in] len  Number of bytes to send.
 */
VOID mf_uart_send_callback(IN BYTE_T *data, IN CONST UINT_T len)
{
    tal_uart_write(TUYA_UART_NUM_0, data, len);
}

/**
 * @brief Read data from the MF UART (non-blocking behaviour depends on tal_uart_read).
 * @param[out] buf Buffer to store received bytes.
 * @param[in] len  Maximum number of bytes to read.
 * @return Number of bytes actually read (0 if none available).
 */
UINT_T mf_uart_recv_callback(OUT BYTE_T *buf, IN CONST UINT_T len)
{
    return tal_uart_read(TUYA_UART_NUM_0, buf, len);
}

/**
 * @brief User-defined product test handler invoked by MF framework (e.g. GPIO test).
 * Implement custom tests (e.g. LED, key, ADC) here; see tuyaos_demo_examples
 * src/examples/service_mf_test for reference.
 * @param[in] cmd      Test command ID from the tool.
 * @param[in] data     Command payload (optional).
 * @param[in] len      Payload length.
 * @param[out] ret_data Pointer to fill with response data (allocated by caller or static).
 * @param[out] ret_len  Length of response data.
 * @return OPRT_OK on success; see tuya_error_code.h for errors.
 */
OPERATE_RET mf_user_product_test_callback(USHORT_T cmd, UCHAR_T *data, UINT_T len, OUT UCHAR_T **ret_data, OUT USHORT_T *ret_len)
{
    (void)cmd;
    (void)data;
    (void)len;
    (void)ret_data;
    (void)ret_len;
    return OPRT_OK;
}

/**
 * @brief User hook called when MF framework writes configuration (e.g. after burning auth).
 * Extend to reload or apply config if needed.
 */
VOID mf_user_callback(VOID)
{
}

/**
 * @brief Called by MF framework just before entering production test mode.
 * Use to prepare hardware (e.g. GPIO, peripherals) for test.
 */
VOID mf_user_enter_mf_callback(VOID)
{
}

/* ---------------------------------------------------------------------------
 * SOC device initialization
 * --------------------------------------------------------------------------- */

/**
 * @brief Perform full SOC device initialization: subscribe to base events (reset, link,
 * MQTT), set authorization (UUID/AUTHKEY in code or MF init with UART/test callbacks),
 * optionally run AI toy MF test and product autotest, configure WiFi low-power and
 * P2P if enabled, register IoT callbacks and init WF/wired/soc IoT, then init board/UI
 * and AI toy (and optionally start display).
 * @return OPRT_OK on success; otherwise error from Tuya API (see tuya_error_code.h).
 */
OPERATE_RET __soc_device_init(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;

    /* Subscribe to system events so we react to reset, link and MQTT state changes. */
    // ty_subscribe_event(EVENT_RESET, "quickstart", __soc_dev_reset_cb, SUBSCRIBE_TYPE_EMERGENCY);
    ty_subscribe_event(EVENT_LINK_UP, "quickstart", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);
    ty_subscribe_event(EVENT_LINK_DOWN, "quickstart", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);
    ty_subscribe_event(EVENT_MQTT_CONNECTED, "quickstart", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);

#if (defined(UUID) && defined(AUTHKEY))
#ifndef ENABLE_KV_FILE
    ws_db_init_mf();
#endif
    /* Set authorization in software (Tuya IoT platform; see license guide in URL below).
     * Note that if you use the default authorization information of the code, there may be problems of multiple users and conflicts,
     * so try to use all the authorizations purchased from the tuya iot platform.
     * Buying guide: https://developer.tuya.com/cn/docs/iot/lisence-management?id=Kb4qlem97idl0.
     * You can also apply for two authorization codes for free in the five-step hardware development stage of the Tuya IoT platform.
     * Authorization information can also be written through the production testing tool.
     * When the production testing function is started and the authorization is burned with the Tuya Cloud module tool,
     * When using production test to burn authorization, comment out this block.
     */
#ifdef ENABLE_WIFI_SERVICE
    WF_GW_PROD_INFO_S prod_info = {UUID, AUTHKEY};
    TUYA_CALL_ERR_RETURN(tuya_iot_set_wf_gw_prod_info(&prod_info));
#else
    GW_PROD_INFO_S prod_info = {UUID, AUTHKEY};
    TUYA_CALL_ERR_RETURN(tuya_iot_set_gw_prod_info(&prod_info));
#endif

#else
#if defined(ENABLE_CELLULAR) && (ENABLE_CELLULAR == 1)
    TUYA_CALL_ERR_RETURN(tuya_cniot_set_mftest_fw_info(APP_BIN_NAME, USER_SW_VER));
    USER_MF_TEST_INTF_S user_mf_test_intf = {NULL, NULL, NULL, NULL};
    user_mf_test_intf.user_enter_mf_callback = mf_user_enter_mf_callback;
    user_mf_test_intf.user_pre_gpio_test = NULL;
    user_mf_test_intf.user_product_test = mf_user_product_test_callback;
    user_mf_test_intf.user_callback = mf_user_callback;
    TUYA_CALL_ERR_RETURN(tuya_cniot_user_mftest_register(&user_mf_test_intf));

    tuya_cniot_mftest_set_uart_port(TUYA_UART_NUM_0, 9600);

    // 涂鸦云设备持续连接超过15分钟，就会关闭进入涂鸦产测，在调试中可以关闭这个功能。如果正式产品建议取消对mf_test_ignore_close_flag的调用
    // mf_test_ignore_close_flag();
    // 启动涂鸦产测。
    tuya_cniot_start_mftest();
#else
    /* No UUID/AUTHKEY in code: use MF (production test) to burn authorization.
     * Register UART and product-test callbacks; see service_mf_test example for GPIO test. */
    MF_IMPORT_INTF_S intf = {0};
    intf.uart_init = mf_uart_init_callback;
    intf.uart_free = mf_uart_free_callback;
    intf.uart_send = mf_uart_send_callback;
    intf.uart_recv = mf_uart_recv_callback;
    intf.mf_user_product_test = mf_user_product_test_callback;
    intf.user_callback = mf_user_callback;
    intf.user_enter_mf_callback = mf_user_enter_mf_callback;
    TUYA_CALL_ERR_RETURN(mf_init(&intf, APP_BIN_NAME, USER_SW_VER, TRUE));
    TAL_PR_NOTICE("mf_init successfully");
#endif
#endif

    /* AI toy config from board defaults; on non-Linux, init AI toy MF test (e.g. key/LED test). */
    TY_AI_TOY_CFG_T ai_toy_cfg = TY_AI_TOY_CFG_DEFAULT;
#if (defined(ENABLE_PRODUCT_AUTOTEST) && (ENABLE_PRODUCT_AUTOTEST == 1))
#if defined(ENABLE_AI_MF_TEST) && (ENABLE_AI_MF_TEST == 1)
    ty_ai_toy_mf_test_init(&ai_toy_cfg);
    TAL_PR_NOTICE("ty_ai_toy_mf_test_init");
#endif
    /* If product autotest triggers (e.g. SSID scan), skip normal init and return. */
    if (prodtest_ssid_scan(500)) {
        TAL_PR_NOTICE("prodtest_ssid_scan");
        return OPRT_OK;
    }
    TAL_PR_NOTICE("prodtest_ssid_scan ignored");
#endif

#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
    /* Optional: configure WiFi low-power (DTIM 3); currently LP is disabled after config. */
    tal_cpu_set_lp_mode(TRUE);
    tal_wifi_set_lps_dtim(3);
    tal_cpu_lp_disable();
    tal_wifi_lp_disable();
#endif

#if defined(ENABLE_AI_MODE_P2P) && (ENABLE_AI_MODE_P2P == 1)
    tuya_p2p_sdk_subscribe_iot_init_event();
#endif

// #if defined(ENABLE_CELLULAR_DONGLE) && (ENABLE_CELLULAR_DONGLE == 1)
//     TAL_PR_NOTICE("init cellular");
//     TUYA_CALL_ERR_LOG(tuya_ai_cellular_init());
// #endif

    /* Register IoT callbacks: cloud status, upgrade, reset, DP object/raw, DP query, optional short URL. */
    TY_IOT_CBS_S iot_cbs = {0};
    iot_cbs.gw_status_cb    = __soc_dev_status_changed_cb;
    iot_cbs.gw_ug_cb        = __soc_dev_rev_upgrade_info_cb;
    iot_cbs.gw_reset_cb     = __soc_dev_reset_inform_cb;
    iot_cbs.dev_obj_dp_cb   = __soc_dev_obj_dp_cmd_cb;
    iot_cbs.dev_raw_dp_cb   = __soc_dev_raw_dp_cmd_cb;
    iot_cbs.dev_dp_query_cb = __soc_dev_dp_query_cb;
#if (defined(ENABLE_QRCODE_ACTIVE) && (ENABLE_QRCODE_ACTIVE == 1))
    iot_cbs.active_shorturl = __qrcode_active_shorturl_cb;
#endif
#ifdef ENABLE_WIFI_SERVICE
#ifdef PID
    /* SoftAP-only: sniffer/SmartCfg is not supported on RK3506 USB Wi-Fi TKL. */
    TUYA_CALL_ERR_RETURN(tuya_iot_wf_soc_dev_init(GWCM_OLD, WF_START_AP_ONLY, &iot_cbs, PID, USER_SW_VER));
#else
    tuya_iot_oem_set(TRUE);
    TUYA_CALL_ERR_RETURN(tuya_iot_wf_soc_dev_init_param(GWCM_OLD, WF_START_AP_ONLY, &iot_cbs, GFW_FIRMWARE_KEY, GFW_FIRMWARE_KEY, USER_SW_VER));
#endif
#ifdef ENABLE_WIRED
    TUYA_CALL_ERR_RETURN(tuya_svc_wired_init());
#endif
#elif defined(ENABLE_CELLULAR) && (ENABLE_CELLULAR == 1)
    TUYA_CALL_ERR_RETURN(tuya_cellular_cloud_start(&iot_cbs,PID,USER_SW_VER,NULL,MQTT_KEEPALIVE));
#else
    TUYA_CALL_ERR_RETURN(tuya_iot_soc_init(&iot_cbs, PID, USER_SW_VER));
#endif

    TUYA_CALL_ERR_RETURN(tuya_device_board_init());

#if defined(ENABLE_TUYA_UI) && ENABLE_TUYA_UI == 1
    TAL_PR_NOTICE("ai toy -> init ui");
    tuya_ai_display_init();
#endif

    /* Disable log sequence to reduce flash wear; then init AI toy and optionally start display. */
#if defined(ENABLE_BT_SERVICE) && (ENABLE_BT_SERVICE == 1)
    tuya_ble_enable_debug(FALSE);
#endif
    TUYA_CALL_ERR_RETURN(log_seq_set_enable(FALSE));

#if defined(CODEC_BENCH_TEST) && (CODEC_BENCH_TEST == 1)
    /* Codec bench: only key + encoder test, no AI toy / display. */
    TUYA_CALL_ERR_RETURN(codec_bench_start(&ai_toy_cfg));
    return rt;
#else
    TUYA_CALL_ERR_RETURN(tuya_ai_toy_init(&ai_toy_cfg));
#if defined(ENABLE_TUYA_UI) && ENABLE_TUYA_UI == 1
    tuya_ai_display_start(FALSE);
#endif
    return rt;
#endif
}

/**
 * @brief First-stage main logic: init netconfig, optional cellular boot, Tuya IoT params/DB,
 * then log level and device init (__soc_device_init). Long-running DB init is done here
 * so it does not block other startup paths.
 */
STATIC VOID_T user_main(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;
    tuya_reset_netconfig_init();

#if defined(ENABLE_BT_SERVICE) && (ENABLE_BT_SERVICE == 1)
    /* BLE only for provision */
    tuya_ble_set_startup_attr(TUYA_BLE_ABILITY_NETCFG);
#endif

#if defined(ENABLE_TUYA_LAN) && (ENABLE_TUYA_LAN == 1)
    /* Disable LAN */
    tuya_svc_lan_disable();
#endif


#if defined(ENABLE_CELLULAR) && (ENABLE_CELLULAR == 1)
    TUYA_CALL_ERR_RETURN(tuya_cellular_cloud_init());
#else
    /* Tuya IoT init: on Linux use a local DB directory; on RTOS use init_param (DB and platform). */
#if OPERATING_SYSTEM == SYSTEM_LINUX
    (void)system("mkdir -p ./tuya_db_files/");
    TUYA_CALL_ERR_LOG(tuya_iot_init_params("./tuya_db_files/", NULL));
#else
    TY_INIT_PARAMS_S init_param = {0};
    init_param.init_db = TRUE;
    strcpy(init_param.sys_env, TARGET_PLATFORM);
    TUYA_CALL_ERR_LOG(tuya_iot_init_params(NULL, &init_param));
#endif
#endif

#if defined(ENABLE_CELLULAR_DONGLE) && (ENABLE_CELLULAR_DONGLE == 1)
    tuya_ai_cellular_module_boot();
    TUYA_CALL_ERR_LOG(tuya_ai_cellular_init());
#endif


#if defined(ENABLE_CELLULAR_DONGLE) && (ENABLE_CELLULAR_DONGLE == 1) && defined (ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
    tuya_ai_linkpolicy_init();
#endif


    TAL_PR_NOTICE("sdk_info:%s", tuya_iot_get_sdk_info());
    TAL_PR_NOTICE("name:%s:%s", APP_BIN_NAME, USER_SW_VER);
    TAL_PR_NOTICE("firmware compiled at %s %s", __DATE__, __TIME__);
    TAL_PR_NOTICE("system reset reason:[%d]", tal_system_get_reset_reason(NULL));

    tal_log_set_manage_attr(TAL_LOG_LEVEL_DEBUG);
    tal_log_set_manage_ms_info(TRUE);

    TAL_PR_DEBUG("device_init in");
    TUYA_CALL_ERR_LOG(__soc_device_init());
}

/**
 * @brief Application main task: init base utilities and (if enabled) LWIP, then run user_main().
 * After user_main() returns, the thread deletes itself and clears ty_app_thread.
 * @param[in] arg Thread argument (unused).
 */
STATIC VOID_T tuya_app_thread(VOID_T *arg)
{
    (void)arg;
    tuya_base_utilities_init();

#if defined(ENABLE_LWIP) && (ENABLE_LWIP == 1)
    TUYA_LwIP_Init();
#endif

    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

/**
 * @brief Application entry point. On Linux builds this is main(); on RTOS it is tuya_app_main().
 * Optionally enables PSRAM usage (ENABLE_EXT_RAM), then creates and starts the app thread
 * with stack 4096 and priority THREAD_PRIO_2. On Linux, the main thread then sleeps forever.
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
INT_T main(INT_T argc, CHAR_T **argv)
#else
VOID_T tuya_app_main(VOID)
#endif
{
#if OPERATING_SYSTEM == SYSTEM_LINUX
    (void)argc;
    (void)argv;
#endif
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    extern VOID_T tkl_system_psram_malloc_force_set(BOOL_T enable);
    tkl_system_psram_malloc_force_set(TRUE);
#endif

    THREAD_CFG_T thrd_param = {4096, THREAD_PRIO_2, "tuya_app_main"};
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
#if OPERATING_SYSTEM == SYSTEM_LINUX
    while (1) {
        tal_system_sleep(1000);
    }
#endif
}
