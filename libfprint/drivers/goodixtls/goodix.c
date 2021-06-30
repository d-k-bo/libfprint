// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#define FP_COMPONENT "goodixtls"

#include <gio/gio.h>
#include <glib.h>
#include <gusb.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodixtls.h"

#define gx_dbg(format, data, data_len, ...)      \
  gchar *data_str = data_to_str(data, data_len); \
  fp_dbg(format, __VA_ARGS__, data_str);         \
  g_free(data_str)

typedef struct {
  pthread_t tls_server_thread;
  gint tls_server_sock;
  SSL_CTX *tls_server_ctx;

  guint8 current_cmd;

  gpointer current_data;
  guint16 current_data_len;
} FpiDeviceGoodixTlsPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(FpiDeviceGoodixTls, fpi_device_goodixtls,
                                    FP_TYPE_IMAGE_DEVICE);

static gchar *data_to_str(gpointer data, gsize data_len) {
  gchar *data_str = g_malloc(2 * data_len + 1);

  for (gsize i = 0; i < data_len; i++)
    sprintf((gchar *)data_str + 2 * i, "%02x", *((guint8 *)data + i));

  return data_str;
}

// ---- GOODIX SECTION START ----

void goodix_receive_data(FpiSsm *ssm, FpDevice *dev, gboolean timeout) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);

  G_DEBUG_HERE();

  transfer->ssm = ssm;
  transfer->short_is_error = FALSE;

  fpi_usb_transfer_fill_bulk(transfer, class->ep_in, GOODIX_EP_IN_MAX_BUF_SIZE);

  fpi_usb_transfer_submit(transfer, timeout ? GOODIX_TIMEOUT : 0, NULL,
                          goodix_receive_data_cb, NULL);
}

void goodix_cmd_done(FpiSsm *ssm, FpDevice *dev, guint8 cmd) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  G_DEBUG_HERE();

  if (priv->current_cmd != cmd) {
    // Received a command that is not running. This should not happen.
    fp_warn("Command 0x%02x is done but the runnning command is 0x%02x", cmd,
            priv->current_cmd);
    return;
  }

  fpi_ssm_next_state(ssm);
}

void goodix_ack_handle(FpiSsm *ssm, FpDevice *dev, gpointer data,
                       gsize data_len, GDestroyNotify data_destroy) {
  guint8 cmd;
  gboolean has_no_config;
  GError *error = NULL;

  has_no_config = goodix_decode_ack(&cmd, data, data_len, data_destroy, &error);

  if (error) goto failed;

  if (has_no_config) fp_warn("MCU has no config");

  switch (cmd) {
    case GOODIX_CMD_NOP:
      fp_warn(
          "Received nop ack, device might be in application production mode");
    case GOODIX_CMD_MCU_GET_IMAGE:
      goto read;
    case GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN:
      goodix_receive_data(ssm, dev, FALSE);
      return;
    case GOODIX_CMD_MCU_SWITCH_TO_FDT_UP:
      goodix_receive_data(ssm, dev, FALSE);
      return;
    case GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE:
      goto read;
    case GOODIX_CMD_NAV_0:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_NAV_0);
      goto read;
    case GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE);
      goto read;
    case GOODIX_CMD_WRITE_SENSOR_REGISTER:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_WRITE_SENSOR_REGISTER);
    case GOODIX_CMD_READ_SENSOR_REGISTER:
    case GOODIX_CMD_UPLOAD_CONFIG_MCU:
    case GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY:
      goto read;
    case GOODIX_CMD_ENABLE_CHIP:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_ENABLE_CHIP);
    case GOODIX_CMD_RESET:
      goto read;
    case GOODIX_CMD_MCU_ERASE_APP:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_MCU_ERASE_APP);
    case GOODIX_CMD_READ_OTP:
    case GOODIX_CMD_FIRMWARE_VERSION:
    case GOODIX_CMD_QUERY_MCU_STATE:
    case GOODIX_CMD_ACK:
    case GOODIX_CMD_REQUEST_TLS_CONNECTION:
      goto read;
    case GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED:
      goodix_cmd_done(ssm, dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED);
    case GOODIX_CMD_PRESET_PSK_WRITE_R:
    case GOODIX_CMD_PRESET_PSK_READ_R:
      goto read;
    default:
      // Unknown command. Raising an error.
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Invalid ack command: 0x%02x", cmd);
      goto failed;
  }

read:
  goodix_receive_data(ssm, dev, TRUE);
  return;

failed:
  fpi_ssm_mark_failed(ssm, error);
  return;
}

void goodix_protocol_handle(FpiSsm *ssm, FpDevice *dev, gpointer data,
                            gsize data_len, GDestroyNotify data_destroy) {
  guint8 cmd;
  gpointer payload;
  guint16 payload_len, payload_ptr_len;
  GError *error = NULL;

  payload_ptr_len = goodix_decode_protocol(
      &cmd, &payload, &payload_len, TRUE, data, data_len, data_destroy, &error);

  if (error) goto failed;

  if (payload_ptr_len < payload_len) {
    // Command is not full but packet is since we checked that before.
    // This means that something when wrong. This should never happen.
    // Raising an error.
    // TODO implement reassembling for messages protocol beacause some device
    // doesn't use essages packets.
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid message protocol length: %d bytes / %d bytes",
                payload_ptr_len, payload_len);
    goto failed;
  }

  gx_dbg("Received command: cmd=0x%02x, payload=%s", payload, payload_len, cmd);

  switch (cmd) {
    case GOODIX_CMD_ACK:
      // Ack reply. Decoding it.
      goodix_ack_handle(ssm, dev, payload, payload_ptr_len, NULL);
      goto free;

    default:
      // Unknown command. Raising an error.
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Invalid message protocol command: 0x%02x", cmd);
      goto failed;
  }

failed:
  fpi_ssm_mark_failed(ssm, error);

free:
  g_free(payload);
}

void goodix_pack_handle(FpiSsm *ssm, FpDevice *dev, gpointer data,
                        gsize data_len, GDestroyNotify data_destroy) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  guint8 flags;
  gpointer payload;
  guint16 payload_len, payload_ptr_len;
  GError *error = NULL;

  priv->current_data =
      g_realloc(priv->current_data, priv->current_data_len + data_len);
  memcpy((guint8 *)priv->current_data + priv->current_data_len, data, data_len);
  if (data_destroy) data_destroy(data);
  priv->current_data_len += data_len;

  payload_ptr_len =
      goodix_decode_pack(&flags, &payload, &payload_len, priv->current_data,
                         priv->current_data_len, NULL, &error);

  if (error) goto failed;

  if (payload_ptr_len < payload_len)
    // Packet is not full, we still need data. Starting to read again.
    goto read;

  gx_dbg("Received pack: flags=0x%02x, payload=%s", payload, payload_len,
         flags);

  switch (flags) {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      // Message protocol. Decoding it.
      goodix_protocol_handle(ssm, dev, payload, payload_ptr_len, NULL);
      goto clear;

    case GOODIX_FLAGS_TLS:
      // TLS message sending it to TLS server.
      // TODO
      goto read;

    default:
      // Unknown flags. Raising an error.
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Invalid message pack flags: 0x%02x", flags);
      goto failed;
  }

read:
  goodix_receive_data(ssm, dev, TRUE);
  goto free;

failed:
  fpi_ssm_mark_failed(ssm, error);

clear:
  priv->current_data_len = 0;
  g_clear_pointer(&priv->current_data, g_free);

free:
  g_free(payload);
}

void goodix_receive_data_cb(FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer user_data, GError *error) {
  G_DEBUG_HERE();

  if (error) goto failed;

  goodix_pack_handle(transfer->ssm, dev, transfer->buffer,
                     transfer->actual_length, NULL);

  return;

failed:
  fpi_ssm_mark_failed(transfer->ssm, error);
}

void goodix_send_pack(FpiSsm *ssm, FpDevice *dev, guint8 flags,
                      gpointer payload, guint16 payload_len,
                      GDestroyNotify payload_destroy) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new(dev);
  GError *error = NULL;
  gpointer data;
  gsize data_len;

  gx_dbg("Sending pack: flags=0x%02x, payload=%s", payload, payload_len, flags);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  data_len = goodix_encode_pack(&data, TRUE, flags, payload, payload_len,
                                payload_destroy);

  for (gsize i = 0; i < data_len; i += GOODIX_EP_OUT_MAX_BUF_SIZE) {
    fpi_usb_transfer_fill_bulk_full(transfer, class->ep_out, (guint8 *)data + i,
                                    GOODIX_EP_OUT_MAX_BUF_SIZE, NULL);

    if (!fpi_usb_transfer_submit_sync(transfer, GOODIX_TIMEOUT, &error))
      goto failed;
  }

  goto free;

failed:
  fpi_ssm_mark_failed(ssm, error);

free:
  fpi_usb_transfer_unref(transfer);
  g_free(data);
}

void goodix_send_protocol(FpiSsm *ssm, FpDevice *dev, guint8 cmd,
                          gboolean calc_checksum, gpointer payload,
                          guint16 payload_len, GDestroyNotify payload_destroy) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);
  gpointer data;
  gsize data_len;

  gx_dbg("Sending command: cmd=0x%02x, calc_checksum=%d, payload=%s", payload,
         payload_len, cmd, calc_checksum);

  priv->current_cmd = cmd;

  data_len = goodix_encode_protocol(&data, FALSE, cmd, calc_checksum, payload,
                                    payload_len, payload_destroy);

  goodix_send_pack(ssm, dev, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len, g_free);
}

void goodix_cmd_nop(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint32 unknown;
  } __attribute__((__packed__)) payload = {.unknown = 0};

  fp_dbg("Goodix nop");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_NOP, FALSE, &payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_mcu_get_image(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint8 unused_flags;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {.unused_flags = 0x01};

  fp_dbg("Goodix mcu get image");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_GET_IMAGE, TRUE, &payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_mcu_switch_to_fdt_down(FpiSsm *ssm, FpDevice *dev,
                                       gpointer mode, guint16 mode_len,
                                       GDestroyNotify mode_destroy) {
  fp_dbg("Goodix mcu switch to fdt down");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, TRUE, mode,
                       mode_len, mode_destroy);
}

void goodix_cmd_mcu_switch_to_fdt_up(FpiSsm *ssm, FpDevice *dev, gpointer mode,
                                     guint16 mode_len,
                                     GDestroyNotify mode_destroy) {
  fp_dbg("Goodix mcu switch to fdt up");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, TRUE, mode,
                       mode_len, mode_destroy);
}

void goodix_cmd_mcu_switch_to_fdt_mode(FpiSsm *ssm, FpDevice *dev,
                                       gpointer mode, guint16 mode_len,
                                       GDestroyNotify mode_destroy) {
  fp_dbg("Goodix mcu switch to fdt mode");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, TRUE, mode,
                       mode_len, mode_destroy);
}

void goodix_cmd_nav_0(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint8 unused_flags;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {.unused_flags = 0x01};

  fp_dbg("Goodix nav 0");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_NAV_0, TRUE, &payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_mcu_switch_to_idle_mode(FpiSsm *ssm, FpDevice *dev,
                                        guint8 sleep_time) {
  struct _payload {
    guint8 sleep_time;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {.sleep_time = sleep_time};

  fp_dbg("Goodix mcu switch to idle mode");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE, TRUE,
                       &payload, sizeof(payload), NULL);
}

void goodix_cmd_write_sensor_register(FpiSsm *ssm, FpDevice *dev,
                                      guint16 address, guint16 value) {
  // Only support one address and one value

  struct _payload {
    guint8 multiples;
    guint16 address;
    guint16 value;
  } __attribute__((__packed__)) payload = {.multiples = FALSE,
                                           .address = GUINT16_TO_LE(address),
                                           .value = GUINT16_TO_LE(value)};

  fp_dbg("Goodix write sensor register");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_WRITE_SENSOR_REGISTER, TRUE,
                       &payload, sizeof(payload), NULL);
}

void goodix_cmd_read_sensor_register(FpiSsm *ssm, FpDevice *dev,
                                     guint16 address, guint8 length) {
  // Only support one address

  struct _payload {
    guint8 multiples;
    guint16 address;
    guint8 length;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {
      .multiples = FALSE, .address = GUINT16_TO_LE(address), .length = length};

  fp_dbg("Goodix read sensor register");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_READ_SENSOR_REGISTER, TRUE,
                       &payload, sizeof(payload), NULL);
}

void goodix_cmd_upload_config_mcu(FpiSsm *ssm, FpDevice *dev, gpointer config,
                                  guint16 config_len,
                                  GDestroyNotify config_destroy) {
  fp_dbg("Goodix upload config mcu");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, TRUE, config,
                       config_len, config_destroy);
}

void goodix_cmd_set_powerdown_scan_frequency(FpiSsm *ssm, FpDevice *dev,
                                             guint16 powerdown_scan_frequency) {
  struct _payload {
    guint16 powerdown_scan_frequency;
  } __attribute__((__packed__)) payload = {
      .powerdown_scan_frequency = GUINT16_TO_LE(powerdown_scan_frequency)};

  fp_dbg("Goodix set powerdown scan frequency");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY, TRUE,
                       &payload, sizeof(payload), NULL);
}

void goodix_cmd_enable_chip(FpiSsm *ssm, FpDevice *dev, gboolean enable) {
  struct _payload {
    guint8 enable;
    guint8 : 8;
  } __attribute__((__packed__)) payload = {.enable = enable ? TRUE : FALSE};

  fp_dbg("Goodix enable chip");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_ENABLE_CHIP, TRUE, &payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_reset(FpiSsm *ssm, FpDevice *dev, gboolean reset_sensor,
                      gboolean soft_reset_mcu, guint8 sleep_time) {
  struct _payload {
    guint8 reset_sensor : 1;
    guint8 soft_reset_mcu : 1;
    guint8 : 6;
    guint8 sleep_time;
  } __attribute__((__packed__))
  payload = {.soft_reset_mcu = soft_reset_mcu ? TRUE : FALSE,
             .reset_sensor = reset_sensor ? TRUE : FALSE,
             .sleep_time = sleep_time};

  fp_dbg("Goodix reset");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_RESET, TRUE, &payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_firmware_version(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint16 : 16;
  } __attribute__((__packed__)) payload = {};

  fp_dbg("Goodix firmware version");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_FIRMWARE_VERSION, TRUE, &payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_query_mcu_state(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint8 unused_flags;
  } __attribute__((__packed__)) payload = {.unused_flags = 0x55};

  fp_dbg("Goodix query mcu state");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_QUERY_MCU_STATE, TRUE, &payload,
                       sizeof(payload), NULL);
}

void goodix_cmd_request_tls_connection(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint16 : 16;
  } __attribute__((__packed__)) payload = {};

  fp_dbg("Goodix request tls connection");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_REQUEST_TLS_CONNECTION, TRUE,
                       &payload, sizeof(payload), NULL);
}

void goodix_cmd_tls_successfully_established(FpiSsm *ssm, FpDevice *dev) {
  struct _payload {
    guint16 : 16;
  } __attribute__((__packed__)) payload = {};

  fp_dbg("Goodix tls successfully established");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED, TRUE,
                       &payload, sizeof(payload), NULL);
}

void goodix_cmd_preset_psk_write_r(FpiSsm *ssm, FpDevice *dev, guint32 address,
                                   gpointer psk, guint32 psk_len,
                                   GDestroyNotify psk_destroy) {
  // Only support one address, one payload and one length

  struct _payload {
    guint32 address;
    guint32 length;
  } __attribute__((__packed__)) payload = {.address = GUINT32_TO_LE(address),
                                           .length = GUINT32_TO_LE(psk_len)};
  gpointer payload_ptr = g_malloc(sizeof(payload) + psk_len);

  fp_dbg("Goodix preset psk write r");

  memcpy(payload_ptr, &payload, sizeof(payload));
  memcpy((guint8 *)payload_ptr + sizeof(payload), psk, psk_len);
  if (psk_destroy) psk_destroy(psk);

  goodix_send_protocol(ssm, dev, GOODIX_CMD_PRESET_PSK_WRITE_R, TRUE,
                       payload_ptr, sizeof(payload) + psk_len, g_free);
}

void goodix_cmd_preset_psk_read_r(FpiSsm *ssm, FpDevice *dev, guint32 address,
                                  guint32 length) {
  struct _payload {
    guint32 address;
    guint32 length;
  } __attribute__((__packed__)) payload = {.address = GUINT32_TO_LE(address),
                                           .length = GUINT32_TO_LE(length)};

  fp_dbg("Goodix preset psk read r");

  goodix_send_protocol(ssm, dev, GOODIX_CMD_PRESET_PSK_READ_R, TRUE, &payload,
                       sizeof(payload), NULL);
}

// ---- GOODIX SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean goodix_dev_init(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  G_DEBUG_HERE();

  priv->current_data_len = 0;
  priv->current_data = NULL;

  return g_usb_device_claim_interface(fpi_device_get_usb_device(dev),
                                      class->interface, 0, error);
}

gboolean goodix_dev_deinit(FpDevice *dev, GError **error) {
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS(dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS(self);
  FpiDeviceGoodixTlsPrivate *priv =
      fpi_device_goodixtls_get_instance_private(self);

  G_DEBUG_HERE();

  priv->current_data_len = 0;
  g_free(priv->current_data);

  return g_usb_device_release_interface(fpi_device_get_usb_device(dev),
                                        class->interface, 0, error);
}

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

enum tls_states {
  TLS_SERVER_INIT,
  TLS_SERVER_HANDSHAKE_INIT,
  TLS_NUM_STATES,
};

void goodix_tls_run_state(FpiSsm *ssm, FpDevice *dev) {
  G_DEBUG_HERE();

  switch (fpi_ssm_get_cur_state(ssm)) {
    case TLS_SERVER_INIT:
      tls_server_init(ssm);
      break;

    case TLS_SERVER_HANDSHAKE_INIT:
      tls_server_handshake_init();
      break;
  }
}

void goodix_tls_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  G_DEBUG_HERE();

  fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
}

void goodix_tls(FpDevice *dev) {
  G_DEBUG_HERE();

  fpi_ssm_start(fpi_ssm_new(dev, goodix_tls_run_state, TLS_NUM_STATES),
                goodix_tls_complete);
}

// ---- TLS SECTION END ----

static void fpi_device_goodixtls_init(FpiDeviceGoodixTls *self) {}

static void fpi_device_goodixtls_class_init(FpiDeviceGoodixTlsClass *class) {}