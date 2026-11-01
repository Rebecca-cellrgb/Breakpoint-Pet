#ifndef BSP_WS2812_H
#define BSP_WS2812_H

#ifdef __cplusplus
extern "C" {
#endif

#include "qm.h"

/*
 * WS2812，8 颗，数据线 GPIO5（主板 P5）。
 * 灯用 5V 供电，地和板子共地。不要从 ESP32 的 3.3V 给灯供电。
 * 数据线靠近第一颗灯串 330 欧，5V 对地并 100uF 以上电容。
 */
#define BSP_WS2812_GPIO     5
#define BSP_WS2812_LED_NUM  8

typedef enum {
    BSP_WS2812_STATE_IDLE = 0,
    BSP_WS2812_STATE_CONFIRM_SAVE,
    BSP_WS2812_STATE_CAPTURE_CONTENT,
    BSP_WS2812_STATE_AWAY,
} bsp_ws2812_state_t;

qm_err_t bsp_ws2812_init(void);
qm_err_t bsp_ws2812_set_pixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b);
qm_err_t bsp_ws2812_fill(uint8_t r, uint8_t g, uint8_t b);
qm_err_t bsp_ws2812_refresh(void);
qm_err_t bsp_ws2812_clear(void);
qm_err_t bsp_ws2812_start(void);
void bsp_ws2812_set_state(bsp_ws2812_state_t state);

/* 上电示例：8 颗各一色。用法写在函数上面。 */
void bsp_ws2812_example(void);

#ifdef __cplusplus
}
#endif

#endif
