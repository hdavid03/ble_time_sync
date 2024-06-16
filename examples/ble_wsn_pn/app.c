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
#include <stdbool.h>
#include <stdio.h>
#include "em_common.h"
#include "app_assert.h"
#include "sl_bluetooth.h"
#include "gatt_db.h"
#include "app.h"
#include "sl_mic.h"
#include "sl_power_manager.h"
#include "sl_board_control.h"
#include "ble_time_sync/ble_time_sync.h"
#include "voice.h"

#define INVALID_CONNECTION_HANDLE         255
#define SAMPLE_RATE_HZ                    6400
#define MIC_BUFFER_SIZE                   51200
#define MTU                               250U

// Connection handle for configuring PAwR.
static uint8_t connection_handle = INVALID_CONNECTION_HANDLE;

static volatile bool mic_started = false;
/**************************************************************************//**
 * Application Init.
 *****************************************************************************/
SL_WEAK void app_init(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application init code here!                         //
  // This is called once during start-up.                                    //
  /////////////////////////////////////////////////////////////////////////////
  sl_status_t sc;
  voice_init();
  uint16_t set_mtu;
  sc = sl_bt_gatt_server_set_max_mtu (MTU, &set_mtu);
  app_assert_status(sc);
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
  voice_process_action();
}

void voice_transmit(uint8_t *buffer, uint32_t size)
{
  if (connection_handle != INVALID_CONNECTION_HANDLE) {
    // Write data to characteristic
    (void)sl_bt_gatt_server_send_notification(
      connection_handle,
      gattdb_audio_data,
      size,
      buffer);
  }
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the dummy weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  peripheral_node_on_bt_event(evt);

  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_connection_opened_id:
        connection_handle = evt->data.evt_connection_opened.connection;
    break;
    case sl_bt_evt_pawr_sync_subevent_report_id:
      // skip any incomplete data
       if (evt->data.evt_pawr_sync_subevent_report.data_status == 0) {

         if (!mic_started) {
             voice_start();
             mic_started = true;
         }
       }
    break;
  }
}


