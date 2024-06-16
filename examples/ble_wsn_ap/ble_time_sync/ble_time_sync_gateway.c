/*
 * ble_time_sync.c
 *
 *  Created on: May 15, 2024
 *      Author: hdavid03
 */
#include "app_log.h"
#include "app_assert.h"
#include "ble_time_sync.h"
#include "ble_time_sync_config.h"
#include <stdbool.h>


#define PAWR_NUM_SUBEVENTS              0x01U
#define PAWR_PACKET_SIZE                0x02U
#define PAWR_OPTION_FLAGS               0x00U
#define PAWR_SUBEVENT_INTERVAL          0xFFU
#define PAWR_RESPONSE_SLOT_DELAY        0x50U
#define PAWR_RESPONSE_SLOT_SPACING      0x10U
#define PAST_CONN_INTERVAL_MAX          0x0C80
#define PAST_CONN_INTERVAL_MIN          0x0006
#define PAST_CONN_DEFAULT_TIMEOUT       1000
#define PAST_CONN_MAX_TIMEOUT           0x0C80
#define PAST_CONN_MIN_TIMEOUT           0x000A
#define SUBEVENT_ID                     0U
#define SL_SLEEPTIMER_WALLCLOCK_CONFIG  0xFFU


// Peripheral node "PAwR Configuration" service UUID
static const uint8_t pawr_configuration_service_uuid[2]             = { 0xC7U, 0x98U };
// Peripheral node "Subevent ID" characteristic UUID
static const uint8_t pawr_subevent_id_characteristic_uuid[2]        = { 0xA5U, 0xB8U };
// Peripheral node "Peripheral Node ID" characteristic UUID
static const uint8_t pawr_peripheral_node_id_characteristic_uuid[2] = { 0x0BU, 0x69U };
// Peripheral node "Wall Clock Time" characteristic UUID
static const uint8_t pawr_wall_clock_time_characteristic_uuid[2]    = { 0x9AU, 0x50U };
// Peripheral node "Clock Correction" characteristic UUID
static const uint8_t pawr_clock_correction_characteristic_uuid[2]   = { 0xC6U, 0x9AU };
// Number of active connections
static uint8_t active_connections_num = 0U;
// Peripheral node counter
static uint8_t peripheral_node_id = 0U;
// Subevent ID
static uint8_t subevent_id = 0U;
// Wall clock time
static uint32_t wall_clock_time;
// Offset
static uint32_t offset;
// Connection state
static bt_connection_state_enum connection_state = inactive;
static bool ble_time_sync_initialized = false;
static uint8_t evt_counter = 0U;
// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xFFU;
static uint8_t connection_handle      = 0xFFU;

static peripheral_node_t peripheral_nodes[MAX_NUM_PERIPHERAL_NODES];

static uint8_t find_service_by_uuid(uint8_t *data, uint8_t len);
static void num_to_str(uint8_t* num_arr, uint8_t len, char* str);
static void add_connection(uint8_t connection, uint16_t address);
static void remove_connection(uint8_t connection);
static uint8_t find_index_by_connection_handle(uint8_t connection);
static void init_sensor_nodes();
static void gateway_node_bt_boot(sl_bt_msg_t *evt);
static void gateway_node_bt_legacy_advertisement_report(sl_bt_msg_t *evt);
static void gateway_node_bt_connection_opened(sl_bt_msg_t *evt);
static void gateway_node_bt_service(sl_bt_msg_t *evt);
static void gateway_node_bt_characteristic(sl_bt_msg_t *evt);
static void gateway_node_bt_discover_service(sl_bt_msg_t *evt);
static void gateway_node_bt_set_peripheral_node_id(sl_bt_msg_t *evt);
static void gateway_node_bt_set_subevent_id(sl_bt_msg_t *evt);
static void gateway_node_bt_set_wall_clock_time(sl_bt_msg_t *evt);
static void gateway_node_bt_set_clock_correction(sl_bt_msg_t *evt);
static void gateway_node_bt_sync_process_finished(sl_bt_msg_t *evt);
static void gateway_node_bt_advertiser_subevent_data_request();
static void gateway_node_bt_connection_closed(sl_bt_msg_t *evt);
static sync_opened_cb sync_ready_callback = NULL;


void ble_time_sync_init(sync_opened_cb callback)
{
  init_sensor_nodes();
  sync_ready_callback = callback;
  ble_time_sync_initialized = true;
}


static void num_to_str(uint8_t *num, uint8_t len, char* dest) {
    dest[0] = '0';
    dest[1] = 'x';
    int8_t j = len - 1;
    uint8_t i = 2;
    while (j >= 0) {
        uint8_t tmp = num[j] / 16;
        dest[i] = (tmp > 9) ? 0x37 + tmp : 0x30 + tmp;
        i++;
        tmp = num[j] - tmp * 16;
        dest[i] = (tmp > 9) ? 0x37 + tmp : 0x30 + tmp;
        i++;
        j--;
    }
    dest[i] = '\0';
}

// Parse advertisements looking for advertised service by its UUID
static uint8_t find_service_by_uuid(uint8_t *data, uint8_t len)
{
  uint8_t ad_field_length;
  uint8_t ad_field_type;
  uint8_t i = 0;
  // Parse advertisement packet
  while (i < len) {
    ad_field_length = data[i];
    ad_field_type = data[i + 1];
    // Partial ($02) or complete ($03) list of 16-bit UUIDs
    if (ad_field_type == 0x02 || ad_field_type == 0x03) {
      // compare UUID to Health Thermometer service UUID
      if (memcmp(&data[i + 2], pawr_configuration_service_uuid, 2) == 0) {
        return 1;
      }
    }
    // advance to the next AD struct
    i = i + ad_field_length + 1;
  }
  return 0;
}


static void init_sensor_nodes() {
  for (int i = 0; i < (int)MAX_NUM_PERIPHERAL_NODES; i++) {
      peripheral_nodes[i].pawr_configuration_service_handle = INVALID_NODE_SERV_HANDLE;
      peripheral_nodes[i].peripheral_node_id_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
      peripheral_nodes[i].subevent_id_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
      peripheral_nodes[i].wall_clock_time_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
      peripheral_nodes[i].clock_correction_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
      peripheral_nodes[i].connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
      peripheral_nodes[i].is_synchronized = false;
  }
  app_log("Peripheral nodes initialized!" APP_LOG_NL);
}

// Add a new connection to the connection_properties array
static void add_connection(uint8_t connection, uint16_t address)
{
  peripheral_nodes[active_connections_num].connection_handle = connection;
  peripheral_nodes[active_connections_num].device_address    = address;
  peripheral_nodes[active_connections_num].id = active_connections_num;
  active_connections_num++;
}

// Remove a connection from the connection_properties array
static void remove_connection(uint8_t connection)
{
  uint8_t i;
  uint8_t table_index = find_index_by_connection_handle(connection);

  if (active_connections_num > 0) {
    active_connections_num--;
  }
  app_log_info("Connection with id_%d removed" APP_LOG_NL, peripheral_nodes[table_index].id);
  // Shift entries after the removed connection toward 0 index
  for (i = table_index; i < active_connections_num; i++) {
    peripheral_nodes[i] = peripheral_nodes[i + 1];
  }
  // Clear the slots we've just removed so no junk values appear
  for (i = active_connections_num; i < MAX_NUM_PERIPHERAL_NODES; i++) {
      peripheral_nodes[i].pawr_configuration_service_handle = INVALID_NODE_SERV_HANDLE;
      peripheral_nodes[i].peripheral_node_id_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
      peripheral_nodes[i].subevent_id_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
      peripheral_nodes[i].wall_clock_time_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
      peripheral_nodes[i].clock_correction_characteristic_handle = INVALID_NODE_CHAR_HANDLE;
      peripheral_nodes[i].connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
      peripheral_nodes[i].is_synchronized = false;
  }
}

// Find the index of a given connection in the connection_properties array
static uint8_t find_index_by_connection_handle(uint8_t connection)
{
  for (uint8_t i = 0; i < active_connections_num; i++) {
    if (peripheral_nodes[i].connection_handle == connection) {
      return i;
    }
  }
  return INVALID_TABLE_INDEX;
}


void gateway_node_on_bt_event(sl_bt_msg_t *evt)
{
    app_assert(ble_time_sync_initialized == true, "BLE Time Sync is not initialized!" APP_LOG_NL);
    switch (SL_BT_MSG_ID(evt->header)) {
      // -------------------------------
      // This event indicates the device has started and the radio is ready.
      // Do not call any stack command before receiving this boot event!
      case sl_bt_evt_system_boot_id:
      // Create an advertising set.
          gateway_node_bt_boot(evt);
      break;
      // -------------------------------
      // This event is generated when an advertisement packet or a scan response
      // is received from a responder
      case sl_bt_evt_scanner_legacy_advertisement_report_id:
      // Parse advertisement packets
          gateway_node_bt_legacy_advertisement_report(evt);
      break;
      // -------------------------------
      // This event indicates that a new connection was opened.
      case sl_bt_evt_connection_opened_id:
          gateway_node_bt_connection_opened(evt);
      break;
      // -------------------------------
      // This event is generated when a new service is discovered
      case sl_bt_evt_gatt_service_id:
          gateway_node_bt_service(evt);
      break;
      // -------------------------------
      // This event is generated when a new characteristic is discovered
      case sl_bt_evt_gatt_characteristic_id:
          gateway_node_bt_characteristic(evt);
      break;
      // -------------------------------
      // This event is generated for various procedure completions, e.g. when a
      // write procedure is completed, or service discovery is completed
      case sl_bt_evt_gatt_procedure_completed_id:
        switch(connection_state) {
          case discover_service:
            gateway_node_bt_discover_service(evt);
          break;
          case set_peripheral_node_id:
            gateway_node_bt_set_peripheral_node_id(evt);
          break;
          case set_subevent_id:
            gateway_node_bt_set_subevent_id(evt);
          break;
          case set_wall_clock_time:
            gateway_node_bt_set_wall_clock_time(evt);
          break;
          case set_clock_correction:
            gateway_node_bt_set_clock_correction(evt);
          break;
          case sync_process_finished:
            gateway_node_bt_sync_process_finished(evt);
          break;
          default: break;
        }
      break;

      // subevent data request periodically
      case sl_bt_evt_pawr_advertiser_subevent_data_request_id:
          gateway_node_bt_advertiser_subevent_data_request();
      break;
      // -------------------------------
      // This event indicates that a connection was closed.
      case sl_bt_evt_connection_closed_id:
          // remove connection from active connections
          gateway_node_bt_connection_closed(evt);
      break;
      // -------------------------------
      // Default event handler.
      default:
        break;
    }
}


static void gateway_node_bt_boot(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    sc = sl_bt_advertiser_create_set(&advertising_set_handle);
    app_log_info("Bluetooth stack booted: v%d.%d.%d-b%d" APP_LOG_NL,
                 evt->data.evt_system_boot.major,
                 evt->data.evt_system_boot.minor,
                 evt->data.evt_system_boot.patch,
                 evt->data.evt_system_boot.build);
    app_assert_status(sc);

    // Enable PAwR functionality
    uint16_t pawr_interval = (uint16_t)(PAWR_INTERVAL * 1000 * 8 / 10);
    app_assert(pawr_interval > 0x06U, "Invalid PAwR interval:%d (range: 0.0075 - 81.92 s)" APP_LOG_NL, pawr_interval);
    sc = sl_bt_pawr_advertiser_start(advertising_set_handle, pawr_interval,
                                     pawr_interval, PAWR_OPTION_FLAGS,
                                     PAWR_NUM_SUBEVENTS, PAWR_SUBEVENT_INTERVAL,
                                     PAWR_RESPONSE_SLOT_DELAY, PAWR_RESPONSE_SLOT_SPACING,
                                     MAX_NUM_PERIPHERAL_NODES);
    app_assert_status_f(sc, "Failed to enable PAwR" APP_LOG_NL);
    app_log("PAwR started!" APP_LOG_NL);

    // Start scanning - looking for thermometer devices
    sc = sl_bt_scanner_start(sl_bt_scanner_scan_phy_1m, sl_bt_scanner_discover_generic);
    app_assert_status_f(sc, "Failed to start discovery" APP_LOG_NL);
    app_log("Start scanning" APP_LOG_NL);
    init_sensor_nodes();
    connection_state = scanning;
}


static void gateway_node_bt_legacy_advertisement_report(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    if (evt->data.evt_scanner_legacy_advertisement_report.event_flags
        == (SL_BT_SCANNER_EVENT_FLAG_CONNECTABLE | SL_BT_SCANNER_EVENT_FLAG_SCANNABLE)) {
        // If a peripheral node is found...
        if (find_service_by_uuid(&(evt->data.evt_scanner_legacy_advertisement_report.data.data[0]),
                                            evt->data.evt_scanner_legacy_advertisement_report.data.len) != 0) {
            char ad_addr[12 + 2 + 1];
            app_log("Scanning" APP_LOG_NL);
            num_to_str(evt->data.evt_scanner_legacy_advertisement_report.address.addr,
                       6, ad_addr);
            app_log_info("Device found: %s" APP_LOG_NL, ad_addr);
            // then stop scanning for a while
            sc = sl_bt_scanner_stop();
            app_assert_status(sc);
            // and connect to that device
            if (active_connections_num < SL_BT_CONFIG_MAX_CONNECTIONS) {
            sc = sl_bt_connection_open(evt->data.evt_scanner_legacy_advertisement_report.address,
                                       evt->data.evt_scanner_legacy_advertisement_report.address_type,
                                       sl_bt_gap_phy_1m, &connection_handle);
            app_assert_status(sc);
        }
      }
    }
}


static void gateway_node_bt_connection_opened(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    sc = sl_bt_gatt_discover_primary_services_by_uuid(evt->data.evt_connection_opened.connection,
                                                      sizeof(pawr_configuration_service_uuid),
                                                      (const uint8_t*)pawr_configuration_service_uuid);
    if (sc == SL_STATUS_INVALID_HANDLE) {
      // Failed to open connection, restart scanning
      app_log_warning("Primary service discovery failed with invalid handle, dropping client\n");
      sc = sl_bt_scanner_start(sl_bt_gap_phy_1m, sl_bt_scanner_discover_generic);
      app_assert_status(sc);
      connection_state = scanning;
    } else {
      app_assert_status(sc);
    }

    app_log_info("Connection opened!" APP_LOG_NL);
    // Get last two bytes of sender address
    uint16_t addr_value = (uint16_t)(evt->data.evt_connection_opened.address.addr[1] << 8) + evt->data.evt_connection_opened.address.addr[0];
    // Add connection to the connection_properties array
    add_connection(evt->data.evt_connection_opened.connection, addr_value);

    app_log_info("GATT database discovering started!" APP_LOG_NL);
    connection_state = discover_service;
}


static void gateway_node_bt_service(sl_bt_msg_t *evt)
{
    uint8_t table_index = find_index_by_connection_handle(evt->data.evt_gatt_service.connection);
    if (table_index != INVALID_TABLE_INDEX && connection_state == discover_service &&
        peripheral_nodes[table_index].pawr_configuration_service_handle == INVALID_NODE_SERV_HANDLE) {
      // Save service handle for future reference
        peripheral_nodes[table_index].pawr_configuration_service_handle = evt->data.evt_gatt_service.service;
        app_log_info("PAwR config service discovered!" APP_LOG_NL);
    }
}


static void gateway_node_bt_characteristic(sl_bt_msg_t *evt)
{
    uint8_t table_index = find_index_by_connection_handle(evt->data.evt_gatt_characteristic.connection);
    if (table_index != INVALID_TABLE_INDEX) {
      if (memcmp(evt->data.evt_gatt_characteristic.uuid.data, pawr_subevent_id_characteristic_uuid,
                 sizeof(pawr_subevent_id_characteristic_uuid)) == 0) {
        // Save characteristic handle for future reference
        peripheral_nodes[table_index].subevent_id_characteristic_handle = evt->data.evt_gatt_characteristic.characteristic;
        app_log_info("PAwR subevent ID characteristic discovered!" APP_LOG_NL);
      }
      if (memcmp(evt->data.evt_gatt_characteristic.uuid.data, pawr_peripheral_node_id_characteristic_uuid,
                 sizeof(pawr_peripheral_node_id_characteristic_uuid)) == 0) {
        // Save characteristic handle for future reference
        peripheral_nodes[table_index].peripheral_node_id_characteristic_handle = evt->data.evt_gatt_characteristic.characteristic;
        app_log_info("PAwR node ID characteristic discovered!" APP_LOG_NL);
      }
      if (memcmp(evt->data.evt_gatt_characteristic.uuid.data, pawr_wall_clock_time_characteristic_uuid,
                 sizeof(pawr_peripheral_node_id_characteristic_uuid)) == 0) {
        // Save characteristic handle for future reference
        peripheral_nodes[table_index].wall_clock_time_characteristic_handle = evt->data.evt_gatt_characteristic.characteristic;
        app_log_info("Wall clock characteristic discovered!" APP_LOG_NL);
      }
      if (memcmp(evt->data.evt_gatt_characteristic.uuid.data, pawr_clock_correction_characteristic_uuid,
                 sizeof(pawr_peripheral_node_id_characteristic_uuid)) == 0) {
        // Save characteristic handle for future reference
        peripheral_nodes[table_index].clock_correction_characteristic_handle = evt->data.evt_gatt_characteristic.characteristic;
        app_log_info("Clock correction characteristic discovered!" APP_LOG_NL);
      }
    }
}


static void gateway_node_bt_discover_service(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    uint8_t table_index = find_index_by_connection_handle(evt->data.evt_gatt_characteristic.connection);
    if (table_index != INVALID_TABLE_INDEX) {
      if (peripheral_nodes[table_index].pawr_configuration_service_handle != INVALID_NODE_SERV_HANDLE) {
        // Discover PAwR config characteristics
        sc = sl_bt_gatt_discover_characteristics(evt->data.evt_gatt_procedure_completed.connection,
                                                 peripheral_nodes[table_index].pawr_configuration_service_handle);
        app_assert_status(sc);
        connection_state = set_peripheral_node_id;
      }
    }
}


static void gateway_node_bt_set_peripheral_node_id(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    uint8_t table_index = find_index_by_connection_handle(evt->data.evt_gatt_characteristic.connection);
    if (table_index != INVALID_TABLE_INDEX) {
      // If characteristic discovery finished
      if (peripheral_nodes[table_index].peripheral_node_id_characteristic_handle != INVALID_NODE_CHAR_HANDLE) {
      // stop discovering
      sl_bt_scanner_stop();
      // set peripheral node id
      peripheral_node_id = active_connections_num - 1;
      sc = sl_bt_gatt_write_characteristic_value(evt->data.evt_gatt_procedure_completed.connection,
                                                 peripheral_nodes[table_index].peripheral_node_id_characteristic_handle,
                                                 sizeof(peripheral_node_id),
                                                 (const uint8_t*)&peripheral_node_id);
      app_assert_status(sc);
      app_log_info("Peripheral node ID sent to the peripheral node" APP_LOG_NL);
      connection_state = set_subevent_id;
      }
    }
}


static void gateway_node_bt_set_subevent_id(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    uint8_t table_index = find_index_by_connection_handle(evt->data.evt_gatt_characteristic.connection);
    if (table_index != INVALID_TABLE_INDEX) {

      // If characteristic discovery finished
      if (peripheral_nodes[table_index].subevent_id_characteristic_handle != INVALID_NODE_CHAR_HANDLE) {
        // enable indications
        sc = sl_bt_gatt_write_characteristic_value(evt->data.evt_gatt_procedure_completed.connection,
                                                   peripheral_nodes[table_index].subevent_id_characteristic_handle,
                                                   sizeof(subevent_id),
                                                   (const uint8_t*)&subevent_id);
        app_assert_status(sc);
        app_log_info("Subevent ID sent to the peripheral node" APP_LOG_NL);
        connection_state = set_wall_clock_time;
      }
    }
}


static void gateway_node_bt_set_wall_clock_time(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    uint8_t table_index = find_index_by_connection_handle(evt->data.evt_gatt_characteristic.connection);
    if (table_index != INVALID_TABLE_INDEX) {
      // If characteristic discovery finished
      if (peripheral_nodes[table_index].wall_clock_time_characteristic_handle != INVALID_NODE_CHAR_HANDLE) {
        CORE_ATOMIC_SECTION(
            wall_clock_time = sl_sleeptimer_get_tick_count();
        );
        sc = sl_bt_gatt_write_characteristic_value(evt->data.evt_gatt_procedure_completed.connection,
                                                   peripheral_nodes[table_index].wall_clock_time_characteristic_handle,
                                                   sizeof(wall_clock_time),
                                                   (const uint8_t*)&wall_clock_time);
        app_assert_status(sc);
        app_log_info("Wall clock time sent to the peripheral node: %ld" APP_LOG_NL, wall_clock_time);
        connection_state = set_clock_correction;
      }
    }
}


static void gateway_node_bt_set_clock_correction(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    uint8_t table_index = find_index_by_connection_handle(evt->data.evt_gatt_characteristic.connection);
    if (table_index != INVALID_TABLE_INDEX) {
      // If characteristic discovery finished
      if (peripheral_nodes[table_index].clock_correction_characteristic_handle != INVALID_NODE_CHAR_HANDLE) {
          CORE_ATOMIC_SECTION(
              offset = sl_sleeptimer_get_tick_count() - wall_clock_time;
          );
          offset = offset % 2 ? offset / 2 + 1 : offset / 2;
          sc = evt->data.evt_gatt_procedure_completed.result;
          app_assert_status_f(sc, "GATT write is failed to complete" APP_LOG_NL);
          sc = sl_bt_gatt_write_characteristic_value(evt->data.evt_gatt_procedure_completed.connection,
                                                     peripheral_nodes[table_index].clock_correction_characteristic_handle,
                                                     sizeof(offset),
                                                     (const uint8_t*)&offset);
          app_assert_status(sc);
          app_log_info("Clock correction sent to the peripheral node: %ld" APP_LOG_NL, offset);
          connection_state = sync_process_finished;
      }
    }
}


static void gateway_node_bt_sync_process_finished(sl_bt_msg_t *evt)
{
    sl_status_t sc;
    uint8_t table_index = find_index_by_connection_handle(evt->data.evt_gatt_characteristic.connection);
    if (table_index != INVALID_TABLE_INDEX) {
        sc = evt->data.evt_gatt_procedure_completed.result;
        app_assert_status_f(sc, "GATT write is failed to complete" APP_LOG_NL);
        sc = sl_bt_advertiser_past_transfer(evt->data.evt_gatt_characteristic.connection, 0, advertising_set_handle);
        app_assert_status_f(sc, "Failed to send PAST info!" APP_LOG_NL);
        app_log_info("PAST info sent!" APP_LOG_NL);
        peripheral_nodes[table_index].is_synchronized = true;
        if (sync_ready_callback) {
            sync_ready_callback(evt->data.evt_gatt_characteristic.connection);
        }
        if (active_connections_num < SL_BT_CONFIG_MAX_CONNECTIONS) {
          // start scanning again to find new devices
          sc = sl_bt_scanner_start(sl_bt_scanner_scan_phy_1m,
                                   sl_bt_scanner_discover_generic);
          app_assert_status_f(sc, "Failed to start discovery #2" APP_LOG_NL);
          connection_state = scanning;
        } else {
          connection_state = sensor_network_full;
        }
    }
}


static void gateway_node_bt_advertiser_subevent_data_request()
{
  sl_status_t sc;
  sc = sl_bt_pawr_advertiser_set_subevent_data(advertising_set_handle,
                                               SUBEVENT_ID, 0,
                                               MAX_NUM_PERIPHERAL_NODES,
                                               PAWR_PACKET_SIZE, (const uint8_t*)&evt_counter);
  app_assert_status_f(sc, "Failed to queue subevent data into PAwR train!" APP_LOG_NL);
}


static void gateway_node_bt_connection_closed(sl_bt_msg_t *evt)
{
  sl_status_t sc;
  // remove connection from active connections
  remove_connection(evt->data.evt_connection_closed.connection);
  if (connection_state != scanning) {
    // start scanning again to find new devices
    sc = sl_bt_scanner_start(sl_bt_scanner_scan_phy_1m,
                             sl_bt_scanner_discover_generic);
    app_assert_status_f(sc, "Failed to start discovery #3" APP_LOG_NL);
    connection_state = scanning;
  }
}


peripheral_node_t get_current_peripheral_node(uint8_t connection_handle)
{
    uint8_t node_id = find_index_by_connection_handle(connection_handle);
    return peripheral_nodes[node_id];
}

