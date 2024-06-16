/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "em_common.h"
#include "app_assert.h"
#include "sl_bluetooth.h"
#include "app_log.h"
#include "sl_sleeptimer.h"
#include "app.h"
#include "em_cmu.h"
#include "ble_time_sync/ble_time_sync.h"
#include "ble_time_sync_config.h"


static uint8_t find_index_by_connection_handle(uint8_t connection);
static void sensor_node_ready(uint8_t connection);

static sensor_node_handle_t sensor_node_handles[MAX_NUM_PERIPHERAL_NODES];
static uint8_t connected_devices_ctr = 0U;
// Peripheral node "PAwR Configuration" service UUID
static uint8_t audio_stream_service_uuid[2] = { 0xCBU, 0x95U };
// Peripheral node "Local Timestamp" characteristic UUID
static uint8_t audio_data_characteristic_uuid[2] = { 0x6BU, 0x97U };


void init_sensor_node_handles()
{
    for (int i = 0; i < MAX_NUM_PERIPHERAL_NODES; i++) {
        sensor_node_handles[i].audio_data_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
        sensor_node_handles[i].audio_stream_service_handle = INVALID_NODE_SERV_HANDLE;
        sensor_node_handles[i].connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
        sensor_node_handles[i].audio_data_characteristic_discovered = false;
        sensor_node_handles[i].audio_stream_indication_enabled = false;
    }
}
/**************************************************************************//**
 * Application Init.
 *****************************************************************************/
SL_WEAK void app_init(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application init code here!                         //
  // This is called once during start-up.                                    //
  /////////////////////////////////////////////////////////////////////////////
  ble_time_sync_init(sensor_node_ready);
}

/**************************************************************************//**
 * Application Process Action.
 *****************************************************************************/
SL_WEAK void app_process_action(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application code here!                              //
  // This is called infinitely.                                              //
  // Do not call blocking functions from here!                               //
  /////////////////////////////////////////////////////////////////////////////
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the dummy weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;
  peripheral_node_t current_sensor_node;

  gateway_node_on_bt_event(evt);

  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_connection_opened_id:
      sensor_node_handles[connected_devices_ctr].connection_handle = evt->data.evt_connection_opened.connection;
      connected_devices_ctr++;
    break;
    case sl_bt_evt_connection_closed_id:
      uint8_t i;
      uint8_t table_index = find_index_by_connection_handle(evt->data.evt_connection_closed.connection);

      if (connected_devices_ctr > 0) {
          connected_devices_ctr--;
      }
      // Shift entries after the removed connection toward 0 index
      for (i = table_index; i < connected_devices_ctr; i++) {
          sensor_node_handles[i] = sensor_node_handles[i + 1];
      }
      // Clear the slots we've just removed so no junk values appear
      for (i = connected_devices_ctr; i < MAX_NUM_PERIPHERAL_NODES; i++) {
          sensor_node_handles[i].audio_data_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
          sensor_node_handles[i].audio_stream_service_handle = INVALID_NODE_SERV_HANDLE;
          sensor_node_handles[i].connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
          sensor_node_handles[i].audio_data_characteristic_discovered = false;
          sensor_node_handles[i].audio_stream_indication_enabled = false;
      }
    break;

    case sl_bt_evt_gatt_characteristic_id:
      table_index = find_index_by_connection_handle(evt->data.evt_gatt_characteristic.connection);
      current_sensor_node = get_current_peripheral_node(evt->data.evt_gatt_characteristic.connection);
      if (table_index != INVALID_TABLE_INDEX && current_sensor_node.is_synchronized) {
          if (memcmp(evt->data.evt_gatt_characteristic.uuid.data, audio_data_characteristic_uuid,
                   sizeof(audio_data_characteristic_uuid)) == 0) {
            // Save characteristic handle for future reference
            sensor_node_handles[table_index].audio_data_characteristic_handle = evt->data.evt_gatt_characteristic.characteristic;
            sensor_node_handles[table_index].audio_data_characteristic_discovered = true;
            app_log_info("Audio data characteristic discovered! %d" APP_LOG_NL, evt->data.evt_gatt_characteristic.characteristic);
          }
      }
    break;

    case sl_bt_evt_gatt_service_id:
      table_index = find_index_by_connection_handle(evt->data.evt_gatt_service.connection);
      current_sensor_node = get_current_peripheral_node(evt->data.evt_gatt_service.connection);
      if (table_index != INVALID_TABLE_INDEX && current_sensor_node.is_synchronized) {
        if (sensor_node_handles[table_index].audio_stream_service_handle == INVALID_NODE_SERV_HANDLE) {
          // Save service handle for future reference
          sensor_node_handles[table_index].audio_stream_service_handle = evt->data.evt_gatt_service.service;
          app_log_info("Audio streaming service discovered!" APP_LOG_NL);
        }
      }
    break;
    // -------------------------------
    // This event is generated for various procedure completions, e.g. when a
    // write procedure is completed, or service discovery is completed
    case sl_bt_evt_gatt_procedure_completed_id:
      // If service discovery finished
      table_index = find_index_by_connection_handle(evt->data.evt_gatt_procedure_completed.connection);
      current_sensor_node = get_current_peripheral_node(evt->data.evt_gatt_procedure_completed.connection);
      if (table_index != INVALID_TABLE_INDEX && current_sensor_node.is_synchronized) {
        if (sensor_node_handles[table_index].audio_stream_service_handle != INVALID_NODE_SERV_HANDLE
            && !sensor_node_handles[table_index].audio_data_characteristic_discovered) {
          // Discover PAwR config characteristics
          sc = sl_bt_gatt_discover_characteristics(evt->data.evt_gatt_procedure_completed.connection,
                                                   sensor_node_handles[table_index].audio_stream_service_handle);
          app_assert_status(sc);
          break;
        }
        if (sensor_node_handles[table_index].audio_data_characteristic_handle != INVALID_NODE_CHAR_HANDLE
            && !sensor_node_handles[table_index].audio_stream_indication_enabled) {
          sc = evt->data.evt_gatt_procedure_completed.result;
          app_assert_status_f(sc, "GATT write is failed to complete" APP_LOG_NL);
          sc = sl_bt_gatt_set_characteristic_notification(evt->data.evt_gatt_procedure_completed.connection,
                                                          sensor_node_handles[table_index].audio_data_characteristic_handle,
                                                          sl_bt_gatt_notification);
          app_assert_status_f(sc, "GATT notification is failed to enable" APP_LOG_NL);
          sensor_node_handles[table_index].audio_stream_indication_enabled = true;
          app_log_info("Notification enabled" APP_LOG_NL);
          if (table_index)
            app_log("START" APP_LOG_NL);
        }
      }
    break;
    case sl_bt_evt_gatt_characteristic_value_id:
      current_sensor_node = get_current_peripheral_node(evt->data.evt_gatt_characteristic_value.connection);
      int32_t data_length = evt->data.evt_gatt_characteristic_value.value.len;
      uint32_t ts = *(uint32_t*)&(evt->data.evt_gatt_characteristic_value.value.data[0]);
      //sl_iostream_write(SL_IOSTREAM_STDOUT, (const uint8_t*)(&current_sensor_node.id), sizeof(current_sensor_node.id));
      //sl_iostream_write(SL_IOSTREAM_STDOUT, (const uint8_t*)(&evt->data.evt_gatt_characteristic_value.value.data), data_length);
      app_log("id_%d_t:%ld" APP_LOG_NL, current_sensor_node.id, ts);
      app_log("id_%d:", current_sensor_node.id);
      for (int i = 4; i < data_length; i += 2) {
        int16_t data = *(int16_t*)&(evt->data.evt_gatt_characteristic_value.value.data[i]) * 2;
        app_log("%d,", data);
      }
      app_log(APP_LOG_NL);
    break;
    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////

    // -------------------------------
    // Default event handler.
    default:
    break;
  }

}

static uint8_t find_index_by_connection_handle(uint8_t connection)
{
    for (uint8_t i = 0; i < connected_devices_ctr; i++) {
      if (sensor_node_handles[i].connection_handle == connection) {
        return i;
      }
    }
    return INVALID_TABLE_INDEX;
}


static void sensor_node_ready(uint8_t connection)
{
  sl_status_t sc;
  sc = sl_bt_gatt_discover_primary_services_by_uuid(connection,
                                                    sizeof(audio_stream_service_uuid),
                                                    (const uint8_t*)audio_stream_service_uuid);
  app_assert_status_f(sc, "Failed to start discover audio stream service" APP_LOG_NL);
}

