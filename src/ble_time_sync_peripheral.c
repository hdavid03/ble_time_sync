/*
 * ble_time_sync.c
 *
 *  Created on: May 16, 2024
 *      Author: hdavid03
 */

#include "app_assert.h"
#include "ble_time_sync.h"
#include "gatt_db.h"
#include "sl_status.h"

static time_sync_handle_t time_sync_handle = {
    .connection_handle = SL_BT_INVALID_CONNECTION_HANDLE,
    .sync_handle = SL_BT_INVALID_SYNC_HANDLE,
    .id = INVALID_NODE_ID,
    .subevent_id = INVALID_NODE_ID,
    .clock_offset = 0,
    .pawr_interval_ticks = 0U
};

static uint8_t  advertising_set_handle;
static uint32_t last_subevent_timestamp;
static int32_t  tick_error_max;
static int32_t  last_subevent_tick_error = 0;

static sl_status_t pawr_update_sync_parameters(uint32_t timeout, uint16_t skip);
static void peripheral_node_bt_boot();
static void peripheral_node_bt_connection_parameters(uint8_t connection);
static void peripheral_node_bt_write_request(sl_bt_msg_t* evt);
static void peripheral_node_bt_sync_transfer_received(sl_bt_msg_t* evt);
static void peripheral_node_bt_sync_subevent_report(sl_bt_msg_t* evt);
static void peripheral_node_bt_connection_closed();


uint32_t get_timestamp()
{
  return (sl_sleeptimer_get_tick_count() + time_sync_handle.clock_offset);
}

void peripheral_node_on_bt_event(sl_bt_msg_t* evt)
{
  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      // Create an advertising set.
      peripheral_node_bt_boot();
    break;

    case sl_bt_evt_connection_parameters_id:
      peripheral_node_bt_connection_parameters(evt->data.evt_connection_parameters.connection);
    break;
    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      time_sync_handle.connection_handle = evt->data.evt_connection_opened.connection;
    break;

    case sl_bt_evt_gatt_server_user_write_request_id:
      peripheral_node_bt_write_request(evt);
    break;

    case sl_bt_evt_pawr_sync_transfer_received_id:
      peripheral_node_bt_sync_transfer_received(evt);
    break;

    case sl_bt_evt_pawr_sync_subevent_report_id:
      peripheral_node_bt_sync_subevent_report(evt);
    break;
    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      peripheral_node_bt_connection_closed();
    break;

    case sl_bt_evt_sync_closed_id:
      time_sync_handle.sync_handle = SL_BT_INVALID_SYNC_HANDLE;
    break;
    // -------------------------------
    // Default event handler.
    default:
    break;
  }
}


static sl_status_t pawr_update_sync_parameters(uint32_t timeout, uint16_t skip)
{
  sl_status_t result = SL_STATUS_INVALID_STATE; // default response if unsynced
  uint32_t pawr_sync_timeout = timeout;
  if (!!timeout) {
    // calculate an appropriate sync timeout parameter value - but the latter
    // is expected in units of 10ms.
    timeout = ((PAWR_MAX_SYNC_LOST * timeout) + PAWR_MIN_SYNC_TIMEOUT) / 10;

    // finally, keep sync_timeout within its limits according to documentation
    if (timeout < PAWR_MIN_SYNC_TIMEOUT) {
      timeout = PAWR_MIN_SYNC_TIMEOUT;
    } else if (timeout > PAWR_MAX_SYNC_TIMEOUT) {
      timeout = PAWR_MAX_SYNC_TIMEOUT;
    }
    // store new timeout value if any
    pawr_sync_timeout = timeout;
  }

  // adjust actual timeout value to the current skip parameter
  timeout = pawr_sync_timeout * (skip + 1);
  timeout = timeout > PAWR_SYNC_MAX_TIMEOUT ? PAWR_SYNC_MAX_TIMEOUT : timeout;

  if (time_sync_handle.sync_handle != SL_BT_INVALID_SYNC_HANDLE) {
    // set an appropriate timeout for possible sync lost
    result = sl_bt_sync_update_sync_parameters(time_sync_handle.sync_handle,
                                               skip,
                                               timeout);
  }

  return result;
}


static void peripheral_node_bt_boot()
{
  sl_status_t sc;
  sc = sl_bt_advertiser_create_set(&advertising_set_handle);
  app_assert_status(sc);

  // Generate data for advertising
  sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                             sl_bt_advertiser_general_discoverable);
  app_assert_status(sc);

  // Set advertising interval to 100ms.
  sc = sl_bt_advertiser_set_timing(
    advertising_set_handle,
    160, // min. adv. interval (milliseconds * 1.6)
    160, // max. adv. interval (milliseconds * 1.6)
    0,   // adv. duration
    0);  // max. num. adv. events
  app_assert_status(sc);
  // Start advertising and enable connections.
  sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                     sl_bt_advertiser_connectable_scannable);
  app_assert_status(sc);
}


static void peripheral_node_bt_connection_parameters(uint8_t connection)
{
  sl_status_t sc;
  sc = sl_bt_past_receiver_set_sync_receive_parameters(connection,
                                                       sl_bt_past_receiver_mode_synchronize,
                                                       PAWR_SYNC_SKIP,
                                                       PAWR_SYNC_MAX_TIMEOUT,
                                                       sl_bt_sync_report_all);

  app_assert_status(sc);
}


static void peripheral_node_bt_write_request(sl_bt_msg_t* evt)
{
  sl_status_t sc;
  if (evt->data.evt_gatt_server_user_write_request.characteristic == gattdb_wall_clock_time) {
      CORE_ATOMIC_SECTION(
          uint32_t wall_clock_time = *(uint32_t*)evt->data.evt_gatt_server_attribute_value.value.data;
          time_sync_handle.clock_offset = (int32_t)wall_clock_time - sl_sleeptimer_get_tick_count();
      );
  }
  if (evt->data.evt_gatt_server_user_write_request.characteristic == gattdb_clock_correction) {
      CORE_ATOMIC_SECTION(
          uint32_t clock_correction = *(uint32_t*)evt->data.evt_gatt_server_attribute_value.value.data;
          time_sync_handle.clock_offset += clock_correction;
      );
  }
  if (evt->data.evt_gatt_server_user_write_request.characteristic == gattdb_peripheral_node_id) {
      time_sync_handle.id = evt->data.evt_gatt_server_attribute_value.value.data[0];
  }
  if (evt->data.evt_gatt_server_user_write_request.characteristic == gattdb_subevent_id) {
      time_sync_handle.subevent_id = evt->data.evt_gatt_server_attribute_value.value.data[0];
  }
  // send response only if required by the client
  if (evt->data.evt_gatt_server_user_write_request.att_opcode == sl_bt_gatt_write_request) {
      sc = sl_bt_gatt_server_send_user_write_response(evt->data.evt_gatt_server_user_write_request.connection,
                                                 evt->data.evt_gatt_server_user_write_request.characteristic,
                                                 SL_STATUS_OK);
      app_assert_status(sc);
  }
}


static void peripheral_node_bt_sync_transfer_received(sl_bt_msg_t* evt)
{
  sl_status_t sc;
  if (evt->data.evt_pawr_sync_transfer_received.status == SL_STATUS_OK) {
    last_subevent_timestamp = sl_sleeptimer_get_tick_count();
    // get the base for the timeout value from the adv_interval that arrives in unit of 1.25 ms
    uint32_t pawr_interval_ms = (10 * evt->data.evt_pawr_sync_transfer_received.adv_interval) / 8;
    // accept the sync transfer only from the bonded AP according to ESL spec.

    time_sync_handle.sync_handle = evt->data.evt_pawr_sync_transfer_received.sync;

    pawr_update_sync_parameters(pawr_interval_ms, PAWR_SYNC_SKIP);

    uint32_t pawr_interval_ticks;
      // due to the earlier multiplication trick, this will always give an
      // integer value equal to one hundred times the current interval!
    (void)sl_sleeptimer_ms32_to_tick(pawr_interval_ms, &pawr_interval_ticks);
    // correction with 36 ppm
    pawr_interval_ticks -= (36 * pawr_interval_ticks / 1000000U);
    // calculation of tick error (max. 20 ppm)
    tick_error_max = (20 * pawr_interval_ticks / 1000000U);
    time_sync_handle.pawr_interval_ticks = pawr_interval_ticks;
    sc = sl_bt_pawr_sync_set_sync_subevents(time_sync_handle.sync_handle,
                                             sizeof(time_sync_handle.subevent_id),
                                             &(time_sync_handle.subevent_id));
    app_assert_status(sc);
    }
  else {
    // clean up sync info
    // esl_sync_cleanup(evt->data.evt_pawr_sync_transfer_received.status);
  }
}


static void peripheral_node_bt_sync_subevent_report(sl_bt_msg_t* evt)
{
  // skip any incomplete data
   if (evt->data.evt_pawr_sync_subevent_report.data_status == 0) {
     uint32_t tick_now = sl_sleeptimer_get_tick_count();
     uint32_t ticks_elapsed;
     int32_t  tick_error;

     ticks_elapsed = (uint32_t)(tick_now - last_subevent_timestamp);

     if ((int32_t)ticks_elapsed < 0) {
         ticks_elapsed = -ticks_elapsed;
     }

     tick_error = (int32_t)(ticks_elapsed - time_sync_handle.pawr_interval_ticks);
     ticks_elapsed = (uint32_t)(tick_now - last_subevent_timestamp);

     if ((int32_t)ticks_elapsed < 0) {
         ticks_elapsed = -ticks_elapsed;
     }

     tick_error = (int32_t)(ticks_elapsed - time_sync_handle.pawr_interval_ticks);
     //================================================
     if (tick_error > tick_error_max || tick_error < -tick_error_max) {
        CORE_ATOMIC_SECTION(
            time_sync_handle.clock_offset -= last_subevent_tick_error;
        );
     } else {
        CORE_ATOMIC_SECTION(
            time_sync_handle.clock_offset -= tick_error;
        );
        last_subevent_tick_error = tick_error;
     }
//=================================================
     // save current tick count
     last_subevent_timestamp = tick_now;
   }
}


static void peripheral_node_bt_connection_closed()
{
  sl_status_t sc;
  // reset connection handle - before re-enabling advertising!
  time_sync_handle.connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
  // Generate data for advertising
  sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                             sl_bt_advertiser_general_discoverable);
  app_assert_status(sc);

  // Restart advertising after client has disconnected.
  sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                     sl_bt_advertiser_connectable_scannable);
  app_assert_status(sc);
}
