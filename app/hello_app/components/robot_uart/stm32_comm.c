/*
 * stm32_comm.c — ESP32-P4 <-> STM32 UART communication
 * Protocol V1.0: SOF(2) + VER(1) + LEN(1) + CMD(1) + PAYLOAD(N) + CRC16(2)
 */
#include "stm32_comm.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "stm32_comm";

uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

esp_err_t stm32_comm_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = STM32_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(STM32_UART_NUM, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(STM32_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(STM32_UART_NUM, STM32_UART_TX_PIN, STM32_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d init: TX=GPIO%d RX=GPIO%d baud=%d",
             STM32_UART_NUM, STM32_UART_TX_PIN, STM32_UART_RX_PIN, STM32_UART_BAUD);
    return ESP_OK;
}

void stm32_comm_deinit(void)
{
    uart_driver_delete(STM32_UART_NUM);
}

esp_err_t stm32_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    if (len > PROTO_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    uint8_t frame[PROTO_MAX_FRAME];
    uint8_t idx = 0;
    frame[idx++] = PROTO_SOF_0;
    frame[idx++] = PROTO_SOF_1;
    frame[idx++] = PROTO_VER;
    frame[idx++] = len;
    frame[idx++] = cmd;
    if (payload && len > 0) { memcpy(&frame[idx], payload, len); idx += len; }
    uint16_t crc = crc16_ccitt(frame, idx);
    frame[idx++] = (crc >> 8) & 0xFF;
    frame[idx++] = crc & 0xFF;
    int w = uart_write_bytes(STM32_UART_NUM, frame, idx);
    ESP_LOGI(TAG, "TX cmd=0x%02X len=%u", cmd, len);
    return (w == idx) ? ESP_OK : ESP_FAIL;
}

esp_err_t stm32_recv_frame(uint8_t *cmd, uint8_t *payload, uint8_t *len, uint32_t timeout_ms)
{
    uint8_t sof0 = 0;
    int64_t deadline = (int64_t)timeout_ms * 1000 + esp_timer_get_time();
    while (esp_timer_get_time() < deadline) {
        if (uart_read_bytes(STM32_UART_NUM, &sof0, 1, pdMS_TO_TICKS(100)) == 1 && sof0 == PROTO_SOF_0) break;
    }
    if (sof0 != PROTO_SOF_0) return ESP_ERR_TIMEOUT;
    uint8_t sof1 = 0;
    if (uart_read_bytes(STM32_UART_NUM, &sof1, 1, pdMS_TO_TICKS(100)) != 1 || sof1 != PROTO_SOF_1) return ESP_ERR_INVALID_RESPONSE;
    uint8_t hdr[3];
    if (uart_read_bytes(STM32_UART_NUM, hdr, 3, pdMS_TO_TICKS(200)) != 3) return ESP_ERR_INVALID_RESPONSE;
    *cmd = hdr[2];
    uint8_t plen = hdr[1];
    if (plen > PROTO_MAX_PAYLOAD) return ESP_ERR_INVALID_RESPONSE;
    if (plen > 0 && uart_read_bytes(STM32_UART_NUM, payload, plen, pdMS_TO_TICKS(500)) != plen) return ESP_ERR_INVALID_RESPONSE;
    *len = plen;
    uint8_t crc_b[2];
    if (uart_read_bytes(STM32_UART_NUM, crc_b, 2, pdMS_TO_TICKS(100)) != 2) return ESP_ERR_INVALID_RESPONSE;
    uint8_t buf[PROTO_MAX_FRAME];
    uint8_t i = 0;
    buf[i++] = PROTO_SOF_0; buf[i++] = PROTO_SOF_1; buf[i++] = hdr[0]; buf[i++] = plen; buf[i++] = *cmd;
    if (plen > 0) { memcpy(&buf[i], payload, plen); i += plen; }
    uint16_t exp = crc16_ccitt(buf, i);
    uint16_t got = ((uint16_t)crc_b[0] << 8) | crc_b[1];
    if (exp != got) { ESP_LOGW(TAG, "CRC mismatch: exp 0x%04X got 0x%04X", exp, got); return ESP_ERR_INVALID_CRC; }
    ESP_LOGI(TAG, "RX cmd=0x%02X len=%u", *cmd, plen);
    return ESP_OK;
}

esp_err_t stm32_send_heartbeat(void) { return stm32_send_frame(CMD_HEARTBEAT, NULL, 0); }
esp_err_t stm32_send_stop(void) { return stm32_send_frame(CMD_STOP, NULL, 0); }
esp_err_t stm32_send_motor(int16_t speed)
{
    if (speed < -1000) speed = -1000;
    if (speed > 1000) speed = 1000;
    uint8_t p[2] = { (speed >> 8) & 0xFF, speed & 0xFF };
    return stm32_send_frame(CMD_MOTOR, p, 2);
}

esp_err_t stm32_send_servo(uint8_t servo_id, uint8_t angle)
{
    if (servo_id > 1) return ESP_ERR_INVALID_ARG;
    if (angle > 180) angle = 180;
    uint8_t p[2] = {servo_id, angle};
    return stm32_send_frame(CMD_SERVO, p, sizeof(p));
}

esp_err_t stm32_send_pan_tilt(uint8_t pan, uint8_t tilt)
{
    if (pan > 180) pan = 180;
    if (tilt > 180) tilt = 180;
    uint8_t p[2] = {pan, tilt};
    return stm32_send_frame(CMD_PAN_TILT, p, sizeof(p));
}

esp_err_t stm32_send_action(uint8_t action_cmd)
{
    if (action_cmd != CMD_ACTION_NOD &&
        action_cmd != CMD_ACTION_THINK &&
        action_cmd != CMD_ACTION_CENTER) {
        return ESP_ERR_INVALID_ARG;
    }
    return stm32_send_frame(action_cmd, NULL, 0);
}
