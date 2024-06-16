/*
 * ble_time_sync.h
 *
 *  Created on: May 15, 2024
 *      Author: hdavid03
 */

#ifndef BLE_TIME_SYNC_H_
#define BLE_TIME_SYNC_H_
#include "sl_bluetooth.h"

#define INVALID_NODE_CHAR_HANDLE          ((uint16_t)0)
#define INVALID_NODE_SERV_HANDLE          ((uint32_t)0)
#define INVALID_TABLE_INDEX               255
#define PAWR_SYNC_SKIP                    0x00U
#define PAWR_SYNC_MAX_TIMEOUT             0x2000U
#define PAWR_SUBEVENT_LENGTH              1
#define PAWR_CLOCK_DRIFT_MULTIPLIER       100
#define PAWR_MIN_SYNC_TIMEOUT             0x0a
#define PAWR_MAX_SYNC_TIMEOUT             0x4000
#define INVALID_AP_ADDRESS                ("\0\0\0\0\0\0")
#define PAWR_CLOCK_DRIFT_DIVISOR          1000
#define PAWR_MAX_SYNC_LOST                3
#define PAWR_INTERVAL_RESOLUTION_MS       1.25f
#define PAWR_INTEGER_INTERVAL             (uint32_t)(PAWR_CLOCK_DRIFT_MULTIPLIER * PAWR_INTERVAL_RESOLUTION_MS)
#define INVALID_NODE_ID                   255

typedef enum {
  inactive,
  scanning,
  discover_service,
  set_peripheral_node_id,
  set_subevent_id,
  set_wall_clock_time,
  set_clock_correction,
  sync_process_finished,
  sensor_network_full
} bt_connection_state_enum;

typedef struct peripheral_node_t {
  uint8_t        id;
  uint16_t       device_address;
  uint8_t        connection_handle;
  uint32_t       pawr_configuration_service_handle;
  uint16_t       subevent_id_characteristic_handle;
  uint16_t       wall_clock_time_characteristic_handle;
  uint16_t       clock_correction_characteristic_handle;
  uint16_t       peripheral_node_id_characteristic_handle;
  bool           is_synchronized;
} peripheral_node_t;

typedef struct time_sync_handle_t {
  uint8_t   id;
  uint8_t   connection_handle;
  uint8_t   subevent_id;
  int32_t   clock_offset;
  uint32_t  pawr_interval_ticks;
  uint16_t  sync_handle;
} time_sync_handle_t;

void gateway_node_on_bt_event(sl_bt_msg_t *bt_evt);
peripheral_node_t get_current_peripheral_node(uint8_t connection_handle);
typedef void(*sync_opened_cb)(uint8_t connection_handle);
void ble_time_sync_init(sync_opened_cb callback);
void peripheral_node_on_bt_event(sl_bt_msg_t* evt);
uint32_t get_timestamp();

#endif /* BLE_TIME_SYNC_H_ */
