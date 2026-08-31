#ifndef __TUYA_GATT_H__
#define __TUYA_GATT_H__

#include <dbus/dbus.h>

#include "tuya_bluez_def.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief init gatt
 *
 * @return 0: success, other: fail
 */
int tuya_gatt_init(void);

/**
 * @brief Get system D-Bus connection used by GATT ObjectManager
 * @return connection pointer, or NULL if not initialized
 * @note Used by LE advertisement to share ObjectManager with GATT.
 */
DBusConnection *tuya_gatt_get_connection(void);

/**
 * @brief Register local GATT application with BlueZ after services are exported
 * @return 0: request accepted (async), other: fail
 * @note Call only after tuya_gatt_register_service/characteristic.
 */
int tuya_gatt_register_application(void);

/**
 * @brief register gatt service
 *
 * @param uuid service uuid
 *
 * @return 0: success, other: fail
 */
int tuya_gatt_register_service(uint16_t uuid);

/**
 * @brief register gatt characteristic
 *
 * @param svc_uuid   service uuid
 * @param chr_uuid   characteristic uuid
 * @param props      characteristic properties
 * @param value      characteristic value
 * @param vlen       characteristic value length
 * @param desc_uuid  descriptor uuid
 * @param desc_props descriptor properties
 *
 * @return 0: success, other: fail
 */
int tuya_gatt_register_characteristic(uint16_t svc_uuid, const uint8_t *chr_uuid, uint8_t props, uint16_t desc_uuid, uint8_t desc_props);

/**
 * @brief notify an attribute value
 *
 * @param uuid       characteristic uuid
 * @param data       data
 * @param len        data length
 *
 * @return 0: success, other: fail
 */
int tuya_gatt_server_send_characteristic_notification(uint16_t uuid, uint8_t *data, uint16_t len);

/**
 * @brief connect event
 */
void tuya_gatt_register_connect_event(void (*cb)(int status));

/**
 * @brief gatt write request event
 * @note len must be uint16_t: ATT MTU may exceed 255 after Exchange MTU.
 */
void tuya_gatt_register_write_req_event(void (*cb)(uint16_t uuid, uint8_t *data, uint16_t len));

#ifdef __cplusplus
}
#endif

#endif