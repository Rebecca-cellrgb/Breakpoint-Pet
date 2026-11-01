#ifndef BSP_SERVO_H
#define BSP_SERVO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "qm.h"

/*
 * 实测脉宽 500us = 0°，2400us = 180°。零位取中点 1450us。
 * 信号 GPIO2，电源 4.8V，地共地。
 */
#define BSP_SERVO_PWM_PIN           2
#define BSP_SERVO_PWM_HZ            50
#define BSP_SERVO_PWM_RES_BIT       14

#define BSP_SERVO_PULSE_MIN_US      500
#define BSP_SERVO_PULSE_MAX_US      2400

#define BSP_SERVO_PULSE_ANGLE_MIN   0
#define BSP_SERVO_PULSE_ANGLE_MAX   180

/* 500 和 2400 的中点。改这个值就是改上电停的位置。 */
#define BSP_SERVO_ZERO_PULSE_US     1450

/* Breakpoint-pet poses; tune these after checking the mounted servo direction. */
#define BSP_SERVO_POSE_CENTER_DEG       90
#define BSP_SERVO_POSE_LOOK_AT_USER_DEG 120
#define BSP_SERVO_POSE_LISTEN_DEG       105
#define BSP_SERVO_POSE_SLEEP_DEG        75

#define BSP_SERVO_MOVE_STEP_DEG         5
#define BSP_SERVO_MOVE_STEP_MS          40

qm_err_t bsp_servo_init(void);
void bsp_servo_deinit(void);

void bsp_servo_set_pulse_us(uint16_t pulse_us);
uint16_t bsp_servo_get_pulse_us(void);

void bsp_servo_set_angle(int16_t angle);
int16_t bsp_servo_get_angle(void);

#ifdef __cplusplus
}
#endif

#endif
