/**
 * Copy from gatt-service.c
 */
#include "tuya_gatt.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gdbus/gdbus.h"
#include "tuya_bluez_compat.h"

#define ERROR_INTERFACE "org.bluez.Error"

#define DEVICE_INFACE "org.bluez.Device1"
#define GATT_MGR_IFACE "org.bluez.GattManager1"
#define GATT_SERVICE_IFACE "org.bluez.GattService1"
#define GATT_CHR_IFACE "org.bluez.GattCharacteristic1"
#define GATT_DESCRIPTOR_IFACE "org.bluez.GattDescriptor1"

#define PATH_PREFIX "/com/tuya"
#define GATT_REGISTER_RETRY_SEC 1
#define GATT_REGISTER_RETRY_MAX 10

struct characteristic {
    char *service;
    char *uuid;
    char *path;
    uint8_t *value;
    int vlen;
    uint8_t props;
    gboolean notifying;
};

struct descriptor {
    struct characteristic *chr;
    char *uuid;
    char *path;
    uint8_t *value;
    int vlen;
    uint8_t props;
};

static DBusConnection *connection = NULL;
static GDBusClient *client        = NULL;
static GSList *chr_list;
static GDBusProxy *g_gatt_manager = NULL;
static gboolean g_services_ready = FALSE;
static gboolean g_app_registered = FALSE;
static gboolean g_register_pending = FALSE;
static guint g_register_retry_id = 0;
static int g_register_retry_cnt = 0;
/* D-Bus path of the Device1 we currently consider connected ("" when idle). */
static char g_conn_dev_path[128] = {0};

static void (*__gatt_connect_event)(int status)                                      = NULL;
static void (*__gatt_write_request_event)(uint16_t uuid, uint8_t *data, uint16_t len) = NULL;

static void __try_register_app(void);
static gboolean __register_app_retry_cb(gpointer user_data);

/**
 * @brief Normalize characteristic UUID for BlueZ property + compact path id
 * @param[in] in short hex ("0001") or full dashed UUID
 * @param[out] uuid_out full UUID for GattCharacteristic1.UUID
 * @param[in] uuid_out_len uuid_out size
 * @param[out] path_id_out short path segment (unique per char)
 * @param[in] path_id_len path_id_out size
 * @return none
 * @note Tuya BLE chars use custom base 0000000N-0000-1001-8001-00805f9b07d0.
 *       Expanding short 0001/02/03 with Bluetooth SIG base breaks App discovery.
 */
static void __normalize_chr_uuid(const char *in, char *uuid_out, size_t uuid_out_len, char *path_id_out,
                                 size_t path_id_len)
{
    unsigned int v = 0;
    char tmp[9] = {0};

    if ((uuid_out == NULL) || (uuid_out_len < 37) || (path_id_out == NULL) || (path_id_len < 5)) {
        return;
    }
    uuid_out[0] = '\0';
    path_id_out[0] = '\0';
    if ((in == NULL) || (in[0] == '\0')) {
        return;
    }

    /* Already a full dashed UUID — keep as-is */
    if (strchr(in, '-') != NULL) {
        g_snprintf(uuid_out, uuid_out_len, "%s", in);
        /* time_low low 16 bits: chars [4..7] of "00000001-...." */
        if ((strncmp(in, "0000", 4) == 0) && (strlen(in) >= 8)) {
            memcpy(tmp, in + 4, 4);
            v = (unsigned int)strtoul(tmp, NULL, 16);
            g_snprintf(path_id_out, path_id_len, "%04x", v & 0xffffu);
        } else {
            g_snprintf(path_id_out, path_id_len, "%.8s", in);
        }
        return;
    }

    /* 32 hex digits without dashes — re-insert UUID dashes */
    if (strlen(in) == 32) {
        g_snprintf(uuid_out, uuid_out_len, "%.8s-%.4s-%.4s-%.4s-%.12s", in, in + 8, in + 12, in + 16, in + 20);
        memcpy(tmp, in + 4, 4);
        v = (unsigned int)strtoul(tmp, NULL, 16);
        g_snprintf(path_id_out, path_id_len, "%04x", v & 0xffffu);
        return;
    }

    v = (unsigned int)strtoul(in, NULL, 16) & 0xffffu;
    g_snprintf(path_id_out, path_id_len, "%04x", v);
    /* Tuya write/notify/read short forms */
    if ((v == 0x0001u) || (v == 0x0002u) || (v == 0x0003u)) {
        g_snprintf(uuid_out, uuid_out_len, "%08x-0000-1001-8001-00805f9b07d0", v);
        return;
    }
    /* Service-style / other 16-bit → Bluetooth SIG base */
    g_snprintf(uuid_out, uuid_out_len, "0000%04x-0000-1000-8000-00805f9b34fb", v);
}

/**
 * @brief Parse characteristic handle used by TKL write/notify matching
 * @param[in] uuid UUID string (short or full)
 * @return 16-bit handle
 */
static uint16_t __uuid_to_handle(const char *uuid)
{
    char tmp[5] = {0};

    if ((uuid == NULL) || (uuid[0] == '\0')) {
        return 0;
    }
    /* Tuya / SIG full UUID: "00000001-...." or "0000fd50-...." → low 16 of time_low */
    if ((strncmp(uuid, "0000", 4) == 0) && (strlen(uuid) >= 8)) {
        memcpy(tmp, uuid + 4, 4);
        return (uint16_t)strtoul(tmp, NULL, 16);
    }
    return (uint16_t)strtoul(uuid, NULL, 16);
}

/**
 * @brief Check whether a characteristic object path is already used.
 * @param[in] path object path to test
 * @return TRUE when duplicated, otherwise FALSE
 */
static gboolean __chr_path_exists(const char *path)
{
    GSList *c = chr_list;
    struct characteristic *chr = NULL;

    while (c != NULL) {
        chr = (struct characteristic *)c->data;
        if ((chr != NULL) && (chr->path != NULL) && (strcmp(chr->path, path) == 0)) {
            return TRUE;
        }
        c = c->next;
    }
    return FALSE;
}

static gboolean desc_get_uuid(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    struct descriptor *desc = user_data;

    dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &desc->uuid);

    return TRUE;
}

static gboolean desc_get_characteristic(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    struct descriptor *desc = user_data;

    dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &desc->chr->path);

    return TRUE;
}

static bool desc_read(struct descriptor *desc, DBusMessageIter *iter)
{
    DBusMessageIter array;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &array);

    if (desc->vlen && desc->value)
        dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE, &desc->value, desc->vlen);

    dbus_message_iter_close_container(iter, &array);

    return true;
}

static gboolean desc_get_value(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    struct descriptor *desc = user_data;

    PR_DEBUG("Descriptor(%s): Get('Value')", desc->uuid);

    return desc_read(desc, iter);
}

static void desc_write(struct descriptor *desc, const uint8_t *value, int len)
{
    g_free(desc->value);
    desc->value = g_memdup(value, len);
    desc->vlen  = len;

    g_dbus_emit_property_changed(connection, desc->path, GATT_DESCRIPTOR_IFACE, "Value");
}

static int parse_value(DBusMessageIter *iter, const uint8_t **value, int *len)
{
    DBusMessageIter array;

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
        return -EINVAL;

    dbus_message_iter_recurse(iter, &array);
    dbus_message_iter_get_fixed_array(&array, value, len);

    return 0;
}

static void desc_set_value(const GDBusPropertyTable *property, DBusMessageIter *iter, GDBusPendingPropertySet id, void *user_data)
{
    struct descriptor *desc = user_data;
    const uint8_t *value;
    int len;

    PR_DEBUG("Descriptor(%s): Set('Value', ...)", desc->uuid);

    if (parse_value(iter, &value, &len)) {
        PR_ERR("Invalid value for Set('Value'...)");
        g_dbus_pending_property_error(id, ERROR_INTERFACE ".InvalidArguments", "Invalid arguments in method call");
        return;
    }

    desc_write(desc, value, len);

    g_dbus_pending_property_success(id);
}

static gboolean desc_get_props(const GDBusPropertyTable *property, DBusMessageIter *iter, void *data)
{
    struct descriptor *desc = data;
    DBusMessageIter array;
    char *prop = NULL;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &array);

    if (desc->props & LE_GATT_CHR_PROP_WRITE_NO_RSP) {
        prop = "write-without-response";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }
    if (desc->props & LE_GATT_CHR_PROP_WRITE) {
        prop = "write";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }
    if (desc->props & LE_GATT_CHR_PROP_NOTIFY) {
        prop = "notify";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }
    if (desc->props & LE_GATT_CHR_PROP_INDICATE) {
        prop = "indicate";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }
    if (desc->props & LE_GATT_CHR_PROP_READ) {
        prop = "read";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }

    dbus_message_iter_close_container(iter, &array);

    return TRUE;
}

static const GDBusPropertyTable desc_properties[] = {{"UUID", "s", desc_get_uuid}, {"Characteristic", "o", desc_get_characteristic}, {"Value", "ay", desc_get_value, desc_set_value, NULL}, {"Flags", "as", desc_get_props, NULL, NULL}, {}};

static gboolean chr_get_uuid(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    struct characteristic *chr = user_data;

    dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &chr->uuid);

    return TRUE;
}

static gboolean chr_get_service(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    struct characteristic *chr = user_data;

    dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &chr->service);

    return TRUE;
}

static bool chr_read(struct characteristic *chr, DBusMessageIter *iter)
{
    DBusMessageIter array;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &array);

    /* Guard like desc_read(): value is NULL until the first write, and an App
     * may ReadValue before that. */
    if (chr->vlen && chr->value)
        dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE, &chr->value, chr->vlen);

    dbus_message_iter_close_container(iter, &array);

    return true;
}

static gboolean chr_get_value(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    struct characteristic *chr = user_data;

    PR_DEBUG("Characteristic(%s): Get('Value')", chr->uuid);

    return chr_read(chr, iter);
}

static gboolean chr_get_props(const GDBusPropertyTable *property, DBusMessageIter *iter, void *data)
{
    struct characteristic *chr = data;
    DBusMessageIter array;
    char *prop = NULL;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &array);

    if (chr->props & LE_GATT_CHR_PROP_WRITE_NO_RSP) {
        prop = "write-without-response";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }
    if (chr->props & LE_GATT_CHR_PROP_WRITE) {
        prop = "write";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }
    if (chr->props & LE_GATT_CHR_PROP_NOTIFY) {
        prop = "notify";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }
    if (chr->props & LE_GATT_CHR_PROP_INDICATE) {
        prop = "indicate";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }
    if (chr->props & LE_GATT_CHR_PROP_READ) {
        prop = "read";
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &prop);
    }

    dbus_message_iter_close_container(iter, &array);

    return TRUE;
}

static void chr_write(struct characteristic *chr, const uint8_t *value, int len)
{
    PR_DEBUG("chr_write uuid=%s len=%d notifying=%d", chr->uuid, len, chr->notifying);
    g_free(chr->value);
    chr->value = g_memdup(value, len);
    chr->vlen = len;

    /*
     * FLUSH, not the plain g_dbus_emit_property_changed(). This is a
     * notification path, and the plain variant COALESCES.
     *
     * gdbus/gdbus.h says it outright: "when multiple properties for a given
     * object path are changed in the same mainloop iteration, they will be
     * grouped with the last property changed". Concretely, gdbus/object.c:1811
     * early-returns when the property is already queued, and the queued signal
     * is only assembled later from a g_idle_add() callback, which re-reads the
     * value through the property getter at flush time.
     *
     * There is one value slot per characteristic (the g_free/g_memdup above),
     * so two notifications emitted inside a single main-loop iteration end as
     * ONE signal carrying the SECOND payload -- the first is freed before it is
     * ever serialised, and the caller still sees success.
     *
     * modules/tuya-ble does exactly that, in exactly one place: handle_pair_req()
     * sends the pair response (cmd 0x0001) and then the net-status frame
     * (cmd 0x001E) back to back with no yield, both from inside the WriteValue
     * dispatch. Without FLUSH the phone only ever receives the net-status frame,
     * waits forever for the pair response, and reports a provisioning failure --
     * after a log that shows both frames sent. Every other exchange in the
     * protocol is a single send, which is why only this step failed.
     */
    g_dbus_emit_property_changed_full(connection, chr->path, GATT_CHR_IFACE,
                                      "Value",
                                      G_DBUS_PROPERTY_CHANGED_FLAG_FLUSH);
}

static void chr_set_value(const GDBusPropertyTable *property, DBusMessageIter *iter, GDBusPendingPropertySet id, void *user_data)
{
    struct characteristic *chr = user_data;
    const uint8_t *value;
    int len;

    (void)property;
    PR_DEBUG("Characteristic(%s): Set('Value', ...)", chr->uuid);

    if (parse_value(iter, &value, &len)) {
        PR_ERR("Invalid value for Set('Value'...)");
        g_dbus_pending_property_error(id, ERROR_INTERFACE ".InvalidArguments", "Invalid arguments in method call");
        return;
    }

    chr_write(chr, value, len);
    g_dbus_pending_property_success(id);
}

/**
 * @brief Get Notifying property for BlueZ GATT characteristic
 * @param[in] property unused
 * @param[out] iter D-Bus iterator
 * @param[in] user_data characteristic
 * @return TRUE on success
 * @note Required for App CCCD/StartNotify path in Tuya BLE netcfg (Notify UUID).
 */
static gboolean chr_get_notifying(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    struct characteristic *chr = user_data;
    dbus_bool_t notifying;

    (void)property;
    notifying = chr->notifying ? TRUE : FALSE;
    dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &notifying);
    return TRUE;
}

static const GDBusPropertyTable chr_properties[] = {
    {"UUID", "s", chr_get_uuid},
    {"Service", "o", chr_get_service},
    {"Value", "ay", chr_get_value, chr_set_value, NULL},
    {"Flags", "as", chr_get_props, NULL, NULL},
    {"Notifying", "b", chr_get_notifying, NULL, NULL},
    {}};

static gboolean service_get_primary(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    dbus_bool_t primary = TRUE;

    dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &primary);

    return TRUE;
}

static gboolean service_get_uuid(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    const char *uuid = user_data;

    dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &uuid);

    return TRUE;
}

/* "Includes" is advertised in the property table but never exported: the
 * exist-callback below always returns FALSE, so BlueZ skips it entirely. */
static gboolean service_get_includes(const GDBusPropertyTable *property, DBusMessageIter *iter, void *user_data)
{
    (void)property;
    (void)iter;
    (void)user_data;
    return TRUE;
}

static gboolean service_exist_includes(const GDBusPropertyTable *property, void *user_data)
{
    (void)property;
    (void)user_data;
    return FALSE;
}

static const GDBusPropertyTable service_properties[] = {{"Primary", "b", service_get_primary}, {"UUID", "s", service_get_uuid}, {"Includes", "ao", service_get_includes, NULL, service_exist_includes}, {}};

static void chr_iface_destroy(gpointer user_data)
{
    struct characteristic *chr = user_data;

    g_free(chr->uuid);
    g_free(chr->service);
    g_free(chr->value);
    g_free(chr->path);
    g_free(chr);
}

static void desc_iface_destroy(gpointer user_data)
{
    struct descriptor *desc = user_data;

    g_free(desc->uuid);
    g_free(desc->value);
    g_free(desc->path);
    g_free(desc);
}

static int parse_options(DBusMessageIter *iter, const char **device)
{
    DBusMessageIter dict;

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
        return -EINVAL;

    dbus_message_iter_recurse(iter, &dict);

    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        const char *key;
        DBusMessageIter value, entry;
        int var;

        dbus_message_iter_recurse(&dict, &entry);
        dbus_message_iter_get_basic(&entry, &key);

        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &value);

        var = dbus_message_iter_get_arg_type(&value);
        if (strcasecmp(key, "device") == 0) {
            if (var != DBUS_TYPE_OBJECT_PATH)
                return -EINVAL;
            dbus_message_iter_get_basic(&value, device);
            PR_DEBUG("Device: %s", *device);
        }

        dbus_message_iter_next(&dict);
    }

    return 0;
}

static DBusMessage *chr_read_value(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    struct characteristic *chr = user_data;
    DBusMessage *reply;
    DBusMessageIter iter;
    const char *device;

    if (!dbus_message_iter_init(msg, &iter))
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");

    if (parse_options(&iter, &device))
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");

    reply = dbus_message_new_method_return(msg);
    if (!reply)
        return g_dbus_create_error(msg, DBUS_ERROR_NO_MEMORY, "No Memory");

    dbus_message_iter_init_append(reply, &iter);

    chr_read(chr, &iter);

    return reply;
}

static DBusMessage *chr_write_value(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    struct characteristic *chr = user_data;
    DBusMessageIter iter;
    const uint8_t *value;
    int len;
    const char *device;

    (void)conn;
    dbus_message_iter_init(msg, &iter);

    if (parse_value(&iter, &value, &len)) {
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");
    }
    if (parse_options(&iter, &device)) {
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");
    }

    PR_INFO("WriteValue uuid=%s len=%d", chr->uuid, len);
    chr_write(chr, value, len);

    if (__gatt_write_request_event) {
        /*
         * Do NOT cast len to uint8_t: after ATT MTU exchange, App may write
         * up to ~512 bytes. Truncation causes ble_data_unpack / decrypt errors.
         */
        __gatt_write_request_event(__uuid_to_handle(chr->uuid), (uint8_t *)value, (uint16_t)len);
    }

    return dbus_message_new_method_return(msg);
}

/**
 * @brief Enable notifications (BlueZ StartNotify)
 * @note Official sample returns Not Supported; that breaks Tuya Notify characteristic
 *       (0x2B10 / product notify UUID). Must succeed for App to receive Value Notification.
 */
static DBusMessage *chr_start_notify(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    struct characteristic *chr = user_data;

    (void)conn;
    if (!(chr->props & LE_GATT_CHR_PROP_NOTIFY) && !(chr->props & LE_GATT_CHR_PROP_INDICATE)) {
        PR_ERR("StartNotify REJECTED for %s: props=0x%02x lacks NOTIFY/INDICATE", chr->uuid,
               chr->props);
        return g_dbus_create_error(msg, ERROR_INTERFACE ".NotPermitted", "Not permitted");
    }

    if (!chr->notifying) {
        chr->notifying = TRUE;
        g_dbus_emit_property_changed(connection, chr->path, GATT_CHR_IFACE, "Notifying");
    }
    PR_INFO("StartNotify uuid=%s (App subscribed, notify path open)", chr->uuid);
    return dbus_message_new_method_return(msg);
}

/**
 * @brief Disable notifications (BlueZ StopNotify)
 */
static DBusMessage *chr_stop_notify(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    struct characteristic *chr = user_data;

    (void)conn;
    if (chr->notifying) {
        chr->notifying = FALSE;
        g_dbus_emit_property_changed(connection, chr->path, GATT_CHR_IFACE, "Notifying");
    }
    PR_INFO("StopNotify uuid=%s (App unsubscribed)", chr->uuid);
    return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable chr_methods[] = {{GDBUS_ASYNC_METHOD("ReadValue", GDBUS_ARGS({"options", "a{sv}"}), GDBUS_ARGS({"value", "ay"}), chr_read_value)},
                                               {GDBUS_ASYNC_METHOD("WriteValue", GDBUS_ARGS({"value", "ay"}, {"options", "a{sv}"}), NULL, chr_write_value)},
                                               {GDBUS_ASYNC_METHOD("StartNotify", NULL, NULL, chr_start_notify)},
                                               {GDBUS_METHOD("StopNotify", NULL, NULL, chr_stop_notify)},
                                               {}};

static DBusMessage *desc_read_value(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    struct descriptor *desc = user_data;
    DBusMessage *reply;
    DBusMessageIter iter;
    const char *device;

    if (!dbus_message_iter_init(msg, &iter))
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");

    if (parse_options(&iter, &device))
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");

    reply = dbus_message_new_method_return(msg);
    if (!reply)
        return g_dbus_create_error(msg, DBUS_ERROR_NO_MEMORY, "No Memory");

    dbus_message_iter_init_append(reply, &iter);

    desc_read(desc, &iter);

    return reply;
}

static DBusMessage *desc_write_value(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    struct descriptor *desc = user_data;
    DBusMessageIter iter;
    const char *device;
    const uint8_t *value;
    int len;

    if (!dbus_message_iter_init(msg, &iter))
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");

    if (parse_value(&iter, &value, &len))
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");

    if (parse_options(&iter, &device))
        return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS, "Invalid arguments");

    desc_write(desc, value, len);

    return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable desc_methods[] = {
    {GDBUS_ASYNC_METHOD("ReadValue", GDBUS_ARGS({"options", "a{sv}"}), GDBUS_ARGS({"value", "ay"}), desc_read_value)}, {GDBUS_ASYNC_METHOD("WriteValue", GDBUS_ARGS({"value", "ay"}, {"options", "a{sv}"}), NULL, desc_write_value)}, {}};

/**
 * @brief Schedule a delayed RegisterApplication retry
 * @return none
 */
static void __schedule_register_retry(void)
{
    if (g_register_retry_id != 0) {
        return;
    }
    if (g_register_retry_cnt >= GATT_REGISTER_RETRY_MAX) {
        PR_ERR("RegisterApplication retry exhausted (%d) — GATT server will NOT be "
               "visible to the App, BLE provisioning cannot proceed", g_register_retry_cnt);
        return;
    }
    g_register_retry_id = g_timeout_add_seconds(GATT_REGISTER_RETRY_SEC, __register_app_retry_cb, NULL);
}

/**
 * @brief Handle RegisterApplication D-Bus reply
 * @param[in] reply D-Bus reply message
 * @param[in] user_data unused
 * @return none
 */
static void register_app_reply(DBusMessage *reply, void *user_data)
{
    DBusError derr;

    (void)user_data;
    g_register_pending = FALSE;

    dbus_error_init(&derr);
    dbus_set_error_from_message(&derr, reply);

    if (dbus_error_is_set(&derr)) {
        g_app_registered = FALSE;
        PR_ERR("RegisterApplication: %s", derr.message);
        __schedule_register_retry();
    } else {
        g_app_registered = TRUE;
        g_register_retry_cnt = 0;
        if (g_register_retry_id != 0) {
            g_source_remove(g_register_retry_id);
            g_register_retry_id = 0;
        }
        PR_INFO("RegisterApplication: OK (%u characteristic(s) exported)",
                g_slist_length(chr_list));
    }

    dbus_error_free(&derr);
}

/**
 * @brief Fill RegisterApplication method arguments
 * @param[in,out] iter D-Bus message iterator
 * @param[in] user_data unused
 * @return none
 */
static void register_app_setup(DBusMessageIter *iter, void *user_data)
{
    const char *path = "/";
    DBusMessageIter dict;

    (void)user_data;
    dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(iter, &dict);
}

/**
 * @brief Issue RegisterApplication when GattManager and local services are ready
 * @return none
 * @note Must not run before GATT services are exported; empty ObjectManager
 *       causes BlueZ "No object received".
 */
static void __try_register_app(void)
{
    if (g_app_registered || g_register_pending) {
        return;
    }
    if (!g_services_ready) {
        PR_DEBUG("RegisterApplication deferred: services not ready");
        return;
    }
    if (g_gatt_manager == NULL) {
        PR_DEBUG("RegisterApplication deferred: GattManager not ready");
        return;
    }

    if (!g_dbus_proxy_method_call(g_gatt_manager, "RegisterApplication", register_app_setup, register_app_reply, NULL,
                                  NULL)) {
        PR_ERR("Unable to call RegisterApplication");
        __schedule_register_retry();
        return;
    }

    g_register_pending = TRUE;
    PR_INFO("RegisterApplication requested");
}

/**
 * @brief GLib timeout callback to retry RegisterApplication
 * @param[in] user_data unused
 * @return G_SOURCE_REMOVE always
 */
static gboolean __register_app_retry_cb(gpointer user_data)
{
    (void)user_data;
    g_register_retry_id = 0;
    g_register_retry_cnt++;
    PR_WARN("RegisterApplication retry %d/%d", g_register_retry_cnt, GATT_REGISTER_RETRY_MAX);
    __try_register_app();
    return G_SOURCE_REMOVE;
}

/**
 * @brief Report a link state change upwards, remembering which device it was.
 * @param[in] path Device1 object path (may be NULL when disconnecting)
 * @param[in] connected TRUE on link up
 * @note The TKL layer de-duplicates repeated edges; this only has to make sure
 *       every real edge is reported exactly once, including the "BlueZ dropped
 *       the device object" case handled in proxy_removed_cb().
 */
static void __report_link_state(const char *path, gboolean connected)
{
    if (connected) {
        g_strlcpy(g_conn_dev_path, path ? path : "", sizeof(g_conn_dev_path));
    } else {
        g_conn_dev_path[0] = '\0';
    }
    if (__gatt_connect_event) {
        __gatt_connect_event(connected ? 1 : 0);
    }
}

/**
 * @brief Cache GattManager proxy; register only after services are ready.
 *        Also catch a Device1 that is already connected when we attach.
 * @param[in] proxy new D-Bus proxy
 * @param[in] user_data unused
 * @return none
 */
static void proxy_added_cb(GDBusProxy *proxy, void *user_data)
{
    const char *iface;

    (void)user_data;
    iface = g_dbus_proxy_get_interface(proxy);

    if (g_strcmp0(iface, GATT_MGR_IFACE) == 0) {
        g_gatt_manager = proxy;
        PR_INFO("GattManager1 ready");
        __try_register_app();
        return;
    }

    /*
     * A phone that connects before our D-Bus client is attached shows up as an
     * already-Connected Device1 with no property-change to follow, so read the
     * current value instead of waiting for an edge that already happened.
     */
    if (g_strcmp0(iface, DEVICE_INFACE) == 0) {
        DBusMessageIter iter;
        dbus_bool_t connected = FALSE;

        if (g_dbus_proxy_get_property(proxy, "Connected", &iter)) {
            dbus_message_iter_get_basic(&iter, &connected);
            if (connected) {
                PR_INFO("device added already connected path=%s", g_dbus_proxy_get_path(proxy));
                __report_link_state(g_dbus_proxy_get_path(proxy), TRUE);
            }
        }
    }
}

/**
 * @brief Clear GattManager proxy on removal; treat losing the connected
 *        Device1 object as a disconnect.
 * @param[in] proxy removed D-Bus proxy
 * @param[in] user_data unused
 * @return none
 * @note BlueZ drops the Device1 object for a transient (non-paired) LE peer,
 *       which is exactly what a provisioning phone is. That removal often
 *       arrives INSTEAD of Connected=false, so without this the link state
 *       would stay stuck "connected" and the next connection would be ignored.
 */
static void proxy_removed_cb(GDBusProxy *proxy, void *user_data)
{
    const char *path;

    (void)user_data;

    if (proxy == g_gatt_manager) {
        PR_WARN("GattManager1 removed");
        g_gatt_manager = NULL;
        g_app_registered = FALSE;
        g_register_pending = FALSE;
        return;
    }

    if (g_conn_dev_path[0] == '\0') {
        return;
    }
    if (g_strcmp0(g_dbus_proxy_get_interface(proxy), DEVICE_INFACE) != 0) {
        return;
    }
    path = g_dbus_proxy_get_path(proxy);
    if (g_strcmp0(path, g_conn_dev_path) == 0) {
        PR_INFO("connected device object removed (path=%s) -> treat as disconnect", path);
        __report_link_state(NULL, FALSE);
    }
}

/**
 * @brief Handle BlueZ property changes (connection via ServicesResolved)
 * @param[in] proxy D-Bus proxy
 * @param[in] name property name
 * @param[in] iter property value
 * @param[in] user_data unused
 * @return none
 */
static void property_changed_cb(GDBusProxy *proxy, const char *name, DBusMessageIter *iter, void *user_data)
{
    dbus_bool_t conn_status = FALSE;
    const char *interface = g_dbus_proxy_get_interface(proxy);
    const char *path = g_dbus_proxy_get_path(proxy);

    (void)user_data;
    PR_DEBUG("property_changed, path: %s, iface: %s, name: %s", path, interface, name);

    if (!g_strcmp0(interface, DEVICE_INFACE)) {
        /*
         * Peripheral path: phone is GATT central. "Connected" tracks the LE link.
         * Keep ServicesResolved as a fallback for stacks that only flip that flag.
         */
        if (!g_strcmp0(name, "Connected") || !g_strcmp0(name, "ServicesResolved")) {
            dbus_message_iter_get_basic(iter, &conn_status);
            PR_INFO("device %s=%d path=%s", name, conn_status ? 1 : 0, path);
            /*
             * Only let a "false" from the device we actually track tear the
             * link down — a stale second device object flipping ServicesResolved
             * must not cancel a live provisioning session.
             */
            if (!conn_status && g_conn_dev_path[0] != '\0' &&
                g_strcmp0(path, g_conn_dev_path) != 0) {
                PR_DEBUG("ignore %s=0 from other device %s", name, path);
                return;
            }
            __report_link_state(path, conn_status ? TRUE : FALSE);
        }
    }
}

/**
 * @brief Initialize GATT D-Bus client and ObjectManager
 * @return LE_SUCCESS on success, LE_COM_ERROR on failure
 * @note Does not call RegisterApplication; call tuya_gatt_register_application()
 *       after local services/characteristics are exported.
 */
int tuya_gatt_init(void)
{
    connection = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, NULL);
    if (connection == NULL) {
        PR_ERR("g_dbus_setup_bus failed (is dbus/bluetoothd up?)");
        return LE_COM_ERROR;
    }

    if (!g_dbus_attach_object_manager(connection)) {
        PR_ERR("g_dbus_attach_object_manager error");
        return LE_COM_ERROR;
    }

    client = g_dbus_client_new(connection, "org.bluez", "/");
    g_dbus_client_set_proxy_handlers(client, proxy_added_cb, proxy_removed_cb, property_changed_cb, NULL);

    g_services_ready = FALSE;
    g_app_registered = FALSE;
    g_register_pending = FALSE;
    g_register_retry_cnt = 0;
    g_gatt_manager = NULL;

    return LE_SUCCESS;
}

/**
 * @brief Get system D-Bus connection used by GATT ObjectManager
 * @return connection pointer, or NULL if not initialized
 */
DBusConnection *tuya_gatt_get_connection(void)
{
    return connection;
}

/**
 * @brief Mark local GATT objects ready and register with BlueZ GattManager
 * @return LE_SUCCESS (async result reported in logs / retries)
 * @note Call after tuya_gatt_register_service/characteristic succeed.
 */
int tuya_gatt_register_application(void)
{
    g_services_ready = TRUE;
    g_register_retry_cnt = 0;
    if (g_register_retry_id != 0) {
        g_source_remove(g_register_retry_id);
        g_register_retry_id = 0;
    }
    __try_register_app();
    return LE_SUCCESS;
}

int tuya_gatt_register_service(uint16_t uuid)
{
    if (!connection) {
        PR_WARN("Connection not initialized");
        return LE_COM_ERROR;
    }

    char path[64]  = {0};
    char *uuid_str = NULL;
    char path_id[16] = {0};

    g_snprintf(path_id, sizeof(path_id), "%04x", uuid);
    uuid_str = g_strdup_printf("0000%04x-0000-1000-8000-00805f9b34fb", uuid);
    g_snprintf(path, sizeof(path), "%s/service%s", PATH_PREFIX, path_id);
    if (!g_dbus_register_interface(connection, path, GATT_SERVICE_IFACE, NULL, NULL, service_properties, uuid_str, g_free)) {
        PR_ERR("register service 0x%04x FAILED (path=%s)", uuid, path);
        g_free(uuid_str);
        return LE_COM_ERROR;
    }

    PR_INFO("service 0x%04x registered: %s", uuid, uuid_str);
    return LE_SUCCESS;
}

int tuya_gatt_register_characteristic(uint16_t svc_uuid, const uint8_t *chr_uuid, uint8_t props, uint16_t desc_uuid, uint8_t desc_props)
{
    if (!connection) {
        PR_ERR("Connection not initialized");
        return LE_COM_ERROR;
    }

    struct characteristic *chr = NULL;
    struct descriptor *desc    = NULL;

    char chr_path[128]  = {0};
    char svc_path[128]  = {0};
    char desc_path[128] = {0};

    char svc_uuid_str[32]  = {0};
    char chr_uuid_str[48]  = {0};
    char chr_path_id[16]   = {0};
    char desc_uuid_str[32] = {0};

    g_snprintf(svc_uuid_str, sizeof(svc_uuid_str), "%04x", svc_uuid);

    __normalize_chr_uuid((const char *)chr_uuid, chr_uuid_str, sizeof(chr_uuid_str), chr_path_id,
                         sizeof(chr_path_id));
    g_snprintf(svc_path, sizeof(svc_path), "%s/service%s", PATH_PREFIX, svc_uuid_str);
    g_snprintf(chr_path, sizeof(chr_path), "%s/service%s/characteristic%s", PATH_PREFIX, svc_uuid_str, chr_path_id);
    if (__chr_path_exists(chr_path)) {
        char base_path[128] = {0};
        uint32_t dup_idx = 1;
        g_snprintf(base_path, sizeof(base_path), "%s", chr_path);
        do {
            g_snprintf(chr_path, sizeof(chr_path), "%s_%u", base_path, dup_idx);
            dup_idx++;
        } while (__chr_path_exists(chr_path));
    }

    chr = g_new0(struct characteristic, 1);
    chr->uuid = g_strdup(chr_uuid_str);
    chr->props = props;
    chr->service = g_strdup(svc_path);
    chr->path = g_strdup(chr_path);
    chr->notifying = FALSE;

    PR_INFO("chr->uuid %s path=%s props=0x%02x", chr->uuid, chr->path, chr->props);
    if (!g_dbus_register_interface(connection, chr->path, GATT_CHR_IFACE, chr_methods, NULL, chr_properties, chr, chr_iface_destroy)) {
        PR_ERR("Couldn't register characteristic interface");
        chr_iface_destroy(chr);
        return LE_COM_ERROR;
    }

    chr_list = g_slist_append(chr_list, chr);

    if (!desc_uuid)
        return LE_SUCCESS;
   
    g_snprintf(desc_uuid_str, sizeof(desc_uuid_str), "%04x", desc_uuid);
    g_snprintf(desc_path, sizeof(desc_path), "%s/descriptor%s", chr->path, desc_uuid_str);

    desc        = g_new0(struct descriptor, 1);
    desc->uuid  = g_strdup_printf("0000%04x-0000-1000-8000-00805f9b34fb", desc_uuid);
    desc->chr   = chr;
    desc->props = desc_props;
    desc->path  = g_strdup(desc_path);

    if (!g_dbus_register_interface(connection, desc->path, GATT_DESCRIPTOR_IFACE, desc_methods, NULL, desc_properties, desc, desc_iface_destroy)) {
        PR_ERR("Couldn't register descriptor interface for %s", chr->uuid);
        /*
         * Drop chr from chr_list BEFORE unregistering: the unregister runs
         * chr_iface_destroy, which frees chr. Leaving it in the list would
         * leave a dangling pointer that the notify lookup walks later.
         */
        chr_list = g_slist_remove(chr_list, chr);
        g_dbus_unregister_interface(connection, chr->path, GATT_CHR_IFACE);
        desc_iface_destroy(desc);
        return LE_COM_ERROR;
    }

    return LE_SUCCESS;
}

int tuya_gatt_server_send_characteristic_notification(uint16_t uuid, uint8_t *data, uint16_t len)
{
    struct characteristic *chr = NULL;
    struct characteristic *notify_chr = NULL;
    struct characteristic *fallback_chr = NULL;
    GSList *c                  = chr_list;

    if ((data == NULL) || (len == 0)) {
        PR_ERR("notify uuid=0x%04x with no payload", uuid);
        return LE_INVALID_PARAM;
    }

    while (c != NULL) {
        chr = (struct characteristic *)c->data;
        if (uuid == __uuid_to_handle(chr->uuid)) {
            if (fallback_chr == NULL) {
                fallback_chr = chr;
            }
            if ((chr->props & LE_GATT_CHR_PROP_NOTIFY) || (chr->props & LE_GATT_CHR_PROP_INDICATE)) {
                notify_chr = chr;
                break;
            }
        }
        c = c->next;
    }

    chr = (notify_chr != NULL) ? notify_chr : fallback_chr;
    if (chr == NULL) {
        /*
         * Used to return success silently. If the SDK notifies a handle we
         * never registered, provisioning stalls with no trace at all — this is
         * the first place to look when the App connects but never advances.
         */
        PR_ERR("notify: no characteristic for handle 0x%04x (%u registered)", uuid,
               g_slist_length(chr_list));
        return LE_COM_ERROR;
    }
    if (notify_chr == NULL) {
        PR_WARN("notify: handle 0x%04x (%s) has no NOTIFY/INDICATE property", uuid, chr->uuid);
    }
    if (!chr->notifying) {
        /* App never issued StartNotify — BlueZ will not put this on the air. */
        PR_WARN("notify: %s not subscribed yet, App may miss len=%u", chr->uuid, len);
    }

    chr_write(chr, data, len);
    return LE_SUCCESS;
}

void tuya_gatt_register_connect_event(void (*cb)(int status))
{
    __gatt_connect_event = cb;
}

void tuya_gatt_register_write_req_event(void (*cb)(uint16_t uuid, uint8_t *data, uint16_t len))
{
    __gatt_write_request_event = cb;
}

