#include "tkl_bluetooth.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "tuya_cloud_types.h"
#include "tuya_bluez_api.h"
#include "tuya_bluez_compat.h"

/* Doc sample uses a fixed connection handle; keep char handles as UUIDs. */
#define BLE_CONN_HANDLE 0x0001

/*
 * Writes that landed before the D-Bus "Connected" edge did. Each one carries
 * the time it was cached: a cached write only makes sense for the connection
 * it arrived on, and if that connection's CONNECT edge never shows up (BlueZ
 * can drop the Device1 object instead) the entry would otherwise sit in the
 * queue and get replayed into the NEXT phone's session, corrupting the Tuya
 * pairing handshake.
 */
#define BLE_CACHE_TTL_SEC 10

typedef struct {
    UINT16_T uuid;
    USHORT_T length;
    time_t   ts;
    UCHAR_T  data[0];
} BLE_CACHE_DATA_T;

STATIC TKL_BLE_GAP_EVT_FUNC_CB __gap_evt_cb   = NULL;
STATIC TKL_BLE_GATT_EVT_FUNC_CB __gatt_evt_cb = NULL;

STATIC BOOL_T g_connected          = FALSE;
STATIC BOOL_T g_stack_inited       = FALSE;
STATIC P_QUEUE_CLASS g_cache_queue = NULL;
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Hand a GATT write up to the SDK.
 * @note Must never take g_cache_mutex — it is called both directly and from
 *       the cache-flush path, and the mutex is not recursive.
 */
STATIC VOID __deliver_write_event(UINT16_T uuid, UINT8_T *data, UINT16_T len)
{
    TKL_BLE_GATT_PARAMS_EVT_T event;

    if (__gatt_evt_cb == NULL) {
        return;
    }

    memset(&event, 0, SIZEOF(TKL_BLE_GATT_PARAMS_EVT_T));
    event.result = 0;
    event.type = TKL_BLE_GATT_EVT_WRITE_REQ;
    event.conn_handle = BLE_CONN_HANDLE;
    event.gatt_event.write_report.char_handle = uuid;
    event.gatt_event.write_report.report.p_data = data;
    event.gatt_event.write_report.report.length = len;
    __gatt_evt_cb(&event);
}

/**
 * @brief Pop one cached write, or NULL when the queue is empty.
 */
STATIC BLE_CACHE_DATA_T *__cache_pop(VOID)
{
    BLE_CACHE_DATA_T *cache_data = NULL;

    pthread_mutex_lock(&g_cache_mutex);
    if ((g_cache_queue != NULL) && GetCurQueNum(g_cache_queue)) {
        if (!OutQueue(g_cache_queue, (unsigned char *)&cache_data, 1)) {
            cache_data = NULL;
        }
    }
    pthread_mutex_unlock(&g_cache_mutex);
    return cache_data;
}

/**
 * @brief Deliver every write that arrived before the connect event landed.
 * @note Entries are popped under the lock but dispatched outside it: the SDK
 *       callback can re-enter this module, and the previous version called the
 *       write handler while still holding g_cache_mutex, which self-deadlocked
 *       on the disconnect path (g_connected == FALSE made the handler try to
 *       re-take the same non-recursive mutex).
 */
STATIC VOID __cache_flush(VOID)
{
    BLE_CACHE_DATA_T *cache_data = NULL;
    time_t now = time(NULL);
    UINT_T sent = 0, stale = 0;

    while ((cache_data = __cache_pop()) != NULL) {
        if ((now - cache_data->ts) > BLE_CACHE_TTL_SEC) {
            /* Left over from a session whose CONNECT edge never arrived. */
            stale++;
        } else {
            PR_DEBUG("flush cached write: uuid=0x%04x, len=%u", cache_data->uuid,
                     (UINT_T)cache_data->length);
            __deliver_write_event(cache_data->uuid, cache_data->data, cache_data->length);
            sent++;
        }
        Free(cache_data);
    }
    if (sent || stale) {
        PR_INFO("cache flush on connect: delivered=%u dropped_stale=%u", sent, stale);
    }
}

/**
 * @brief Drop cached writes without delivering them (link went away).
 */
STATIC VOID __cache_drop(VOID)
{
    BLE_CACHE_DATA_T *cache_data = NULL;
    UINT_T dropped = 0;

    while ((cache_data = __cache_pop()) != NULL) {
        Free(cache_data);
        dropped++;
    }
    if (dropped > 0) {
        PR_WARN("dropped %u cached write(s) on disconnect", dropped);
    }
}

STATIC VOID __gatt_write_request_event_cb(UINT16_T uuid, UINT8_T *data, UINT16_T len)
{
    BLE_CACHE_DATA_T *cache_data = NULL;

    PR_INFO("recv write request, uuid: 0x%04x, len: %u", uuid, (UINT_T)len);

    /**
     * BlueZ uses D-BUS for inter-process communication. Data may arrive
     * before the connection event is detected. Cache data until connected.
     */
    if (!g_connected) {
        cache_data = (BLE_CACHE_DATA_T *)Malloc(SIZEOF(BLE_CACHE_DATA_T) + len);
        if (!cache_data) {
            PR_ERR("Malloc err");
            return;
        }
        cache_data->uuid   = uuid;
        cache_data->length = len;
        cache_data->ts     = time(NULL);
        memcpy(cache_data->data, data, len);

        pthread_mutex_lock(&g_cache_mutex);
        if ((g_cache_queue == NULL) || !InQueue(g_cache_queue, (unsigned char *)&cache_data, 1)) {
            pthread_mutex_unlock(&g_cache_mutex);
            PR_ERR("cache queue full, drop write uuid=0x%04x", uuid);
            Free(cache_data);
            return;
        }
        pthread_mutex_unlock(&g_cache_mutex);
        return;
    }

    __deliver_write_event(uuid, data, len);
}

STATIC VOID __gap_connect_event_cb(INT_T status)
{
    TKL_BLE_GAP_PARAMS_EVT_T event;
    BOOL_T connected = (status != 0) ? TRUE : FALSE;

    /*
     * BlueZ reports the link through two properties (Connected and
     * ServicesResolved), so the same transition arrives twice. Report only
     * real edges to the SDK.
     */
    if (connected == g_connected) {
        PR_DEBUG("ignore duplicate connect event, status: %d", status);
        return;
    }
    PR_INFO("recv connect event, status: %d", status);
    g_connected = connected;

    memset(&event, 0, SIZEOF(TKL_BLE_GAP_PARAMS_EVT_T));
    event.result = 0;
    event.type = connected ? TKL_BLE_GAP_EVT_CONNECT : TKL_BLE_GAP_EVT_DISCONNECT;
    event.conn_handle = BLE_CONN_HANDLE;
    event.gap_event.connect.role = TKL_BLE_ROLE_SERVER;

    if (__gap_evt_cb) {
        __gap_evt_cb(&event);
    }

    /* BlueZ D-BUS race: writes can land before the connect event. */
    if (connected) {
        __cache_flush();
    } else {
        __cache_drop();
    }
}

/**
 * @brief Notify the SDK that BLE stack init is done — without it the SDK never
 *        starts advertising.
 * @note The official gateway sample omits STACK_INIT; the Wukong Wi-Fi SDK
 *       requires it.
 */
STATIC VOID __gap_init_event_cb(VOID)
{
    TKL_BLE_GAP_PARAMS_EVT_T event;

    if (__gap_evt_cb == NULL) {
        return;
    }

    memset(&event, 0, SIZEOF(TKL_BLE_GAP_PARAMS_EVT_T));
    event.result = 0;
    event.type = TKL_BLE_EVT_STACK_INIT;
    event.conn_handle = BLE_CONN_HANDLE;
    event.gap_event.connect.role = TKL_BLE_ROLE_SERVER;
    __gap_evt_cb(&event);
}

OPERATE_RET tkl_ble_stack_init(UCHAR_T role)
{
    (void)role;
    PR_INFO("tkl_ble_stack_init");

    if (g_cache_queue == NULL) {
        /*
         * The queue holds POINTERS to heap-allocated BLE_CACHE_DATA_T, so the
         * unit size is the pointer size, not the struct size. The old code
         * passed SIZEOF(BLE_CACHE_DATA_T) and only survived because the struct
         * was then exactly 4 bytes, same as a pointer here — it is 12 now that
         * entries carry a timestamp, so that bug would corrupt the queue.
         */
        g_cache_queue = CreateQueueObj(32, SIZEOF(BLE_CACHE_DATA_T *));
        if (!g_cache_queue) {
            PR_ERR("CreateQueueObj error");
            return OPRT_COM_ERROR;
        }
    }

    tuya_bluez_init();
    tuya_bluez_le_register_connect_event(__gap_connect_event_cb);
    tuya_bluez_le_register_write_req_event(__gatt_write_request_event_cb);

    g_stack_inited = TRUE;
    /* If GAP callback already registered, deliver STACK_INIT now. */
    __gap_init_event_cb();

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_callback_register(CONST TKL_BLE_GAP_EVT_FUNC_CB gap_evt)
{
    PR_INFO("tkl_ble_gap_callback_register");
    __gap_evt_cb = gap_evt;
    /*
     * SDK may register callbacks after stack_init. Official gateway sample
     * does not use STACK_INIT; WiFi SDK does — deliver it when callback arrives.
     */
    if (g_stack_inited) {
        __gap_init_event_cb();
    }
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatt_callback_register(CONST TKL_BLE_GATT_EVT_FUNC_CB gatt_evt)
{
    PR_INFO("tkl_ble_gatt_callback_register");
    __gatt_evt_cb = gatt_evt;
    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_adv_start(TKL_BLE_GAP_ADV_PARAMS_T CONST *p_adv_params)
{
    INT_T ret = 0;
    le_set_adv_params_t adv_param;

    PR_INFO("tkl_ble_gap_adv_start");

    if (p_adv_params == NULL) {
        return OPRT_INVALID_PARM;
    }

    /*
     * BLE coexists with SoftAP on RTL8733BU USB combo: BLE is an independent
     * netcfg channel (tuya_enable_ble_netcfg), while SoftAP (SmartLife-xxxx)
     * remains available. App may use either path.
     */
    memset(&adv_param, 0, sizeof(adv_param));
    adv_param.advtype = p_adv_params->adv_type;
    adv_param.min_interval = p_adv_params->adv_interval_min;
    adv_param.max_interval = p_adv_params->adv_interval_max;

    ret = tuya_bluez_le_set_adv_params(&adv_param);
    if (ret != LE_SUCCESS) {
        PR_ERR("set adv params failed: %d", ret);
        return OPRT_COM_ERROR;
    }

    ret = tuya_bluez_le_set_adv_enable(1);
    if (ret != LE_SUCCESS) {
        PR_ERR("set adv enable failed: %d", ret);
        return OPRT_COM_ERROR;
    }

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_adv_stop(VOID)
{
    INT_T ret = 0;

    PR_INFO("tkl_ble_gap_adv_stop");
    ret = tuya_bluez_le_set_adv_enable(0);
    if (ret != LE_SUCCESS) {
        PR_ERR("set adv disable failed: %d", ret);
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}

/**
 * @brief Format TKL UUID to string for BlueZ GattCharacteristic1.UUID
 * @param[in] uuid TKL UUID (16/32/128, 128-bit is Little-Endian)
 * @param[out] out output buffer
 * @param[in] out_len buffer length
 * @return none
 * @note Tuya BLE netcfg chars must be published as
 *       00000001/02/03-0000-1001-8001-00805f9b07d0 (NOT Bluetooth SIG base).
 */
STATIC VOID __tkl_uuid_to_str(CONST TKL_BLE_UUID_T *uuid, CHAR_T *out, SIZE_T out_len)
{
    CONST UCHAR_T *u = NULL;

    if ((uuid == NULL) || (out == NULL) || (out_len == 0)) {
        return;
    }
    if (uuid->uuid_type == TKL_BLE_UUID_TYPE_16) {
        snprintf(out, out_len, "%04x", uuid->uuid.uuid16);
        return;
    }
    if (uuid->uuid_type == TKL_BLE_UUID_TYPE_32) {
        snprintf(out, out_len, "%08x", uuid->uuid.uuid32);
        return;
    }
    /* Little-Endian uuid128 -> dashed string (MSB first) */
    u = uuid->uuid.uuid128;
    snprintf(out, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8], u[7], u[6], u[5], u[4], u[3],
             u[2], u[1], u[0]);
}

/**
 * @brief Pick a 16-bit handle used by BlueZ write/notify callback matching
 * @param[in] uuid TKL UUID
 * @return 16-bit handle value
 * @note For Tuya custom 128-bit UUID (...00805f9b07d0), handle is time_low
 *       low 16 bits (0x0001 write / 0x0002 notify / 0x0003 read). Do NOT use
 *       uuid128[0..1] which is the company suffix 0x07d0 for all three chars.
 */
STATIC USHORT_T __tkl_uuid_to_handle(CONST TKL_BLE_UUID_T *uuid)
{
    CONST UCHAR_T *u = NULL;

    if (uuid == NULL) {
        return 0;
    }
    if (uuid->uuid_type == TKL_BLE_UUID_TYPE_16) {
        return uuid->uuid.uuid16;
    }
    if (uuid->uuid_type == TKL_BLE_UUID_TYPE_32) {
        return (USHORT_T)(uuid->uuid.uuid32 & 0xFFFF);
    }
    u = uuid->uuid.uuid128;
    /* Tuya base ends with 07d0 (LE: u[0]=0xd0, u[1]=0x07) */
    if ((u[0] == 0xd0) && (u[1] == 0x07) && (u[2] == 0x9b) && (u[3] == 0x5f)) {
        return (USHORT_T)(u[12] | ((USHORT_T)u[13] << 8));
    }
    /* Bluetooth SIG base / other 128-bit: 16-bit alias at LE start */
    return (USHORT_T)(u[0] | ((USHORT_T)u[1] << 8));
}

OPERATE_RET tkl_ble_gap_adv_rsp_data_set(TKL_BLE_DATA_T CONST *p_adv, TKL_BLE_DATA_T CONST *p_scan_rsp)
{
    INT_T ret = 0;

    PR_INFO("tkl_ble_gap_adv_rsp_data_set");
    /*
     * TKL contract (tkl_bluetooth.h / gateway doc): if p_data == NULL or
     * length == 0, do not update that field. Both pointers themselves may be
     * non-NULL with empty payload.
     */
    if ((p_adv != NULL) && (p_adv->p_data != NULL) && (p_adv->length > 0)) {
        ret = tuya_bluez_le_set_adv_data(p_adv->p_data, p_adv->length);
        if (ret != LE_SUCCESS) {
            PR_ERR("set adv data failed: %d", ret);
            return OPRT_COM_ERROR;
        }
    }

    if ((p_scan_rsp != NULL) && (p_scan_rsp->p_data != NULL) && (p_scan_rsp->length > 0)) {
        ret = tuya_bluez_le_set_scan_rsp_data(p_scan_rsp->p_data, p_scan_rsp->length);
        if (ret != LE_SUCCESS) {
            PR_ERR("set scan rsp data failed: %d", ret);
            return OPRT_COM_ERROR;
        }
    }

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_adv_rsp_data_update(TKL_BLE_DATA_T CONST *p_adv, TKL_BLE_DATA_T CONST *p_scan_rsp)
{
    PR_INFO("tkl_ble_gap_adv_rsp_data_update");
    return tkl_ble_gap_adv_rsp_data_set(p_adv, p_scan_rsp);
}

/**
 * @brief Release the temporary le_gatt_service_t array built for the BlueZ call
 */
STATIC VOID __gatt_svc_array_free(le_gatt_service_t *gatt_svc, UINT8_T svc_num)
{
    UINT8_T i;

    if (gatt_svc == NULL) {
        return;
    }
    for (i = 0; i < svc_num; i++) {
        if (gatt_svc[i].chr) {
            Free(gatt_svc[i].chr);
        }
    }
    Free(gatt_svc);
}

OPERATE_RET tkl_ble_gatts_service_add(TKL_BLE_GATTS_PARAMS_T *p_service)
{
    INT_T i = 0, j = 0;
    UINT8_T svc_num             = 0;
    le_gatt_service_t *gatt_svc = NULL;
    OPERATE_RET rt              = OPRT_OK;

    if ((p_service == NULL) || (p_service->p_service == NULL) || (p_service->svc_num == 0)) {
        return OPRT_INVALID_PARM;
    }

    svc_num = p_service->svc_num;
    PR_INFO("register gatt service, num: %u", svc_num);

    gatt_svc = (le_gatt_service_t *)Malloc(svc_num * SIZEOF(le_gatt_service_t));
    if (!gatt_svc) {
        return OPRT_MALLOC_FAILED;
    }
    memset(gatt_svc, 0, svc_num * SIZEOF(le_gatt_service_t));

    for (i = 0; i < svc_num; i++) {
        TKL_BLE_SERVICE_PARAMS_T *p_service_param = &p_service->p_service[i];
        UINT8_T chr_num = p_service_param->char_num;
        le_gatt_characteristic_t *gatt_chr = NULL;

        gatt_svc[i].uuid = p_service_param->svc_uuid.uuid.uuid16;
        /* Distinct enums, but TKL_BLE_UUID_SERVICE_* and LE_SERVICE_* are the
         * same GATT attribute-type values (0x2800/0x2801/0x2802/0x2803), so the
         * mapping is a straight cast. Cast explicitly to keep -Wenum-conversion
         * quiet and to flag the assumption if either enum is ever renumbered. */
        gatt_svc[i].type = (le_service_type_e)p_service_param->type;
        gatt_svc[i].chr_num = chr_num;
        /* Doc: Characteristic handle; service handle can mirror UUID. */
        p_service_param->handle = p_service_param->svc_uuid.uuid.uuid16;

        PR_DEBUG("service uuid: 0x%04x, char_num: %u", gatt_svc[i].uuid, chr_num);

        gatt_chr = (le_gatt_characteristic_t *)Malloc(chr_num * SIZEOF(le_gatt_characteristic_t));
        if (!gatt_chr) {
            /* Free the chr arrays already attached to gatt_svc[0..i-1] too. */
            __gatt_svc_array_free(gatt_svc, svc_num);
            return OPRT_MALLOC_FAILED;
        }
        memset(gatt_chr, 0, chr_num * SIZEOF(le_gatt_characteristic_t));
        gatt_svc[i].chr = gatt_chr;

        for (j = 0; j < chr_num; j++) {
            USHORT_T handle = __tkl_uuid_to_handle(&p_service_param->p_char[j].char_uuid);

            /*
             * Publish full Tuya UUID string to BlueZ (App discovers by UUID).
             * Handle stays 0x0001/0x0002/0x0003 for write/notify matching.
             */
            __tkl_uuid_to_str(&p_service_param->p_char[j].char_uuid, (CHAR_T *)gatt_chr[j].uuid,
                              sizeof(gatt_chr[j].uuid));
            gatt_chr[j].property = p_service_param->p_char[j].property;
            p_service_param->p_char[j].handle = handle;
            PR_INFO("chr[%d] uuid: %s, handle: 0x%04x, props: 0x%02x", j, gatt_chr[j].uuid, handle,
                    gatt_chr[j].property);
        }
    }

    if (tuya_bluez_le_add_gatt_service(gatt_svc, svc_num) != 0) {
        PR_ERR("tuya_bluez_le_add_gatt_service failed");
        rt = OPRT_COM_ERROR;
    }

    __gatt_svc_array_free(gatt_svc, svc_num);
    return rt;
}



OPERATE_RET tkl_ble_gatts_value_notify(USHORT_T conn_handle, USHORT_T char_handle, UCHAR_T *p_data, USHORT_T length)
{
    PR_DEBUG("tkl_ble_gatts_value_notify, chr_handle: 0x%04x", char_handle);
    tuya_bluez_le_gatts_value_notify(char_handle, p_data, length);
    return OPRT_OK;
}

OPERATE_RET tkl_ble_stack_deinit(UCHAR_T role)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_stack_gatt_link(USHORT_T *p_link)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_addr_set(TKL_BLE_GAP_ADDR_T CONST *p_peer_addr)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_address_get(TKL_BLE_GAP_ADDR_T *p_peer_addr)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_scan_start(TKL_BLE_GAP_SCAN_PARAMS_T CONST *p_scan_params)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_scan_stop(VOID)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_connect(TKL_BLE_GAP_ADDR_T CONST *p_peer_addr, TKL_BLE_GAP_SCAN_PARAMS_T CONST *p_scan_params, TKL_BLE_GAP_CONN_PARAMS_T CONST *p_conn_params)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_disconnect(USHORT_T conn_handle, UCHAR_T hci_reason)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_conn_param_update(USHORT_T conn_handle, TKL_BLE_GAP_CONN_PARAMS_T CONST *p_conn_params)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_tx_power_set(UCHAR_T role, INT_T tx_power)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_rssi_get(USHORT_T conn_handle)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gap_name_set(CHAR_T *p_name)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatts_value_set(USHORT_T conn_handle, USHORT_T char_handle, UCHAR_T *p_data, USHORT_T length)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatts_value_get(USHORT_T conn_handle, USHORT_T char_handle, UCHAR_T *p_data, USHORT_T length)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatts_value_indicate(USHORT_T conn_handle, USHORT_T char_handle, UCHAR_T *p_data, USHORT_T length)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gatts_exchange_mtu_reply(USHORT_T conn_handle, USHORT_T server_rx_mtu)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gattc_all_service_discovery(USHORT_T conn_handle)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gattc_all_char_discovery(USHORT_T conn_handle, USHORT_T start_handle, USHORT_T end_handle)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gattc_char_desc_discovery(USHORT_T conn_handle, USHORT_T start_handle, USHORT_T end_handle)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gattc_write_without_rsp(USHORT_T conn_handle, USHORT_T char_handle, UCHAR_T *p_data, USHORT_T length)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gattc_write(USHORT_T conn_handle, USHORT_T char_handle, UCHAR_T *p_data, USHORT_T length)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gattc_read(USHORT_T conn_handle, USHORT_T char_handle)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_gattc_exchange_mtu_request(USHORT_T conn_handle, USHORT_T client_rx_mtu)
{

    return OPRT_OK;
}

OPERATE_RET tkl_ble_vendor_command_control(USHORT_T opcode, VOID_T *user_data, USHORT_T data_len)
{

    return OPRT_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * Stubs required by public TKL header (peripheral provisioning path unused)
 * --------------------------------------------------------------------------- */
OPERATE_RET tkl_ble_gatts_service_change(USHORT_T conn_handle, USHORT_T start_handle, USHORT_T end_handle)
{
    (void)conn_handle;
    (void)start_handle;
    (void)end_handle;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gap_ext_adv_create(TKL_BLE_GAP_EXT_ADV_T *p_ext_adv)
{
    (void)p_ext_adv;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gap_ext_adv_config(TKL_BLE_GAP_EXT_ADV_T ext_adv, TKL_BLE_GAP_EXT_ADV_PARAMS_T CONST *p_adv_params,
                                       TKL_BLE_DATA_T CONST *p_adv_data, TKL_BLE_DATA_T CONST *p_scan_rsp)
{
    (void)ext_adv;
    (void)p_adv_params;
    (void)p_adv_data;
    (void)p_scan_rsp;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gap_ext_adv_start(TKL_BLE_GAP_EXT_ADV_T ext_adv)
{
    (void)ext_adv;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gap_ext_adv_stop(TKL_BLE_GAP_EXT_ADV_T ext_adv)
{
    (void)ext_adv;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gap_ext_adv_delete(TKL_BLE_GAP_EXT_ADV_T ext_adv)
{
    (void)ext_adv;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_ble_gap_ext_adv_clear(void)
{
    return OPRT_NOT_SUPPORTED;
}

uint16_t tkl_ble_gap_ext_adv_get_max_data_length(void)
{
    return 0;
}

uint8_t tkl_ble_gap_ext_adv_get_support_number(void)
{
    return 0;
}

OPERATE_RET tkl_ble_set_mode(CONST BOOL_T enable, CONST UCHAR_T mode)
{
    (void)enable;
    (void)mode;
    return OPRT_OK;
}
