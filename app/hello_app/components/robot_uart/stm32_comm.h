#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define STM32_UART_NUM       1
#define STM32_UART_TX_PIN    7
#define STM32_UART_RX_PIN    8
#define STM32_UART_BAUD      115200
#define PROTO_SOF_0          0xAA
#define PROTO_SOF_1          0x55
#define PROTO_VER            0x01
#define PROTO_OVERHEAD       7
#define PROTO_MAX_PAYLOAD    64
#define PROTO_MAX_FRAME      (PROTO_OVERHEAD + PROTO_MAX_PAYLOAD)
#define CMD_MOTOR            0x01
#define CMD_SERVO            0x02
#define CMD_STOP             0x03
#define CMD_LED              0x04
#define CMD_SENSOR_READ      0x05
#define CMD_STATUS           0x06
#define CMD_PAN_TILT         0x07
#define CMD_HEARTBEAT        0x10
#define CMD_ACTION_NOD       0x20
#define CMD_ACTION_THINK     0x21
#define CMD_ACTION_CENTER    0x22
#define CMD_ACK              0x80
#define CMD_ERROR            0x81
esp_err_t stm32_comm_init(void);
void      stm32_comm_deinit(void);
esp_err_t stm32_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len);
esp_err_t stm32_recv_frame(uint8_t *cmd, uint8_t *payload, uint8_t *len, uint32_t timeout_ms);
esp_err_t stm32_send_heartbeat(void);
esp_err_t stm32_send_stop(void);
esp_err_t stm32_send_motor(int16_t speed);
/* servo_id 0 = PB5/MG3115, servo_id 1 = PB4/MG995; angle is 0..180. */
esp_err_t stm32_send_servo(uint8_t servo_id, uint8_t angle);
esp_err_t stm32_send_pan_tilt(uint8_t pan, uint8_t tilt);
esp_err_t stm32_send_action(uint8_t action_cmd);
uint16_t  crc16_ccitt(const uint8_t *data, size_t len);
#ifdef __cplusplus
}
#endif
