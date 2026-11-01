#include "bsp_ws2812.h"

#include "led_strip.h"
#include "led_strip_rmt.h"
#include "qm_kernel.h"
#include "qm_log.h"

#define LOG_TAG "bsp_ws2812"
#define WS2812_TASK_STACK 2048
#define WS2812_TASK_PRIORITY 4
#define WS2812_MAX_BRIGHTNESS 32
#define WS2812_TICK_MS 80

static led_strip_handle_t s_strip;
static uint8_t s_inited;
static uint8_t s_running;
static volatile bsp_ws2812_state_t s_state = BSP_WS2812_STATE_IDLE;
static qm_task_t s_animation_task;

static void ws2812_show(uint8_t r, uint8_t g, uint8_t b)
{
    (void)bsp_ws2812_fill(r, g, b);
}

static void ws2812_animation_task(void *arg)
{
    static const uint8_t test_colors[][3] = {
        {0, 24, 0}, {0, 0, 24}, {0, 0, 32}, {24, 14, 0},
    };
    uint8_t brightness = 2;
    int8_t direction = 1;
    uint32_t tick = 0;

    (void)arg;
    for (size_t i = 0; i < sizeof(test_colors) / sizeof(test_colors[0]); i++) {
        ws2812_show(test_colors[i][0], test_colors[i][1], test_colors[i][2]);
        qm_msleep(350);
    }

    while (s_running) {
        bsp_ws2812_state_t state = s_state;

        switch (state) {
        case BSP_WS2812_STATE_IDLE:
            ws2812_show(0, brightness, 0);
            break;
        case BSP_WS2812_STATE_CONFIRM_SAVE:
            ws2812_show(0, 0, brightness);
            break;
        case BSP_WS2812_STATE_CAPTURE_CONTENT:
            ws2812_show(0, 0, (tick & 3U) < 2U ? WS2812_MAX_BRIGHTNESS : 0);
            break;
        case BSP_WS2812_STATE_AWAY:
            ws2812_show(brightness, (uint8_t)(brightness * 3U / 5U), 0);
            break;
        default:
            s_state = BSP_WS2812_STATE_IDLE;
            continue;
        }

        if (state != BSP_WS2812_STATE_CAPTURE_CONTENT) {
            if (direction > 0) {
                if (brightness >= WS2812_MAX_BRIGHTNESS) {
                    direction = -1;
                } else {
                    brightness = (uint8_t)(brightness + 2U);
                }
            } else if (brightness <= 2U) {
                direction = 1;
            } else {
                brightness = (uint8_t)(brightness - 2U);
            }
        }
        tick++;
        qm_msleep(WS2812_TICK_MS);
    }
    qm_task_exit(&s_animation_task);
}

qm_err_t bsp_ws2812_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = BSP_WS2812_GPIO,
        .max_leds = BSP_WS2812_LED_NUM,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    /*
     * 8 颗 * 24bit = 192 个 symbol，正好占满 S3 的 4 块 RMT 内存（每块 48）。
     * 不开 DMA：PSRAM 上的通道对象过不了 GDMA 的内部 RAM 检查。
     */
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = BSP_WS2812_LED_NUM * 24,
        .flags.with_dma = false,
    };

    if (s_inited) {
        return QM_EOK;
    }

    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip) != ESP_OK) {
        QM_LOGE(LOG_TAG, "rmt init fail gpio%d", BSP_WS2812_GPIO);
        return -QM_EIO;
    }
    if (led_strip_clear(s_strip) != ESP_OK) {
        QM_LOGE(LOG_TAG, "clear fail");
        return -QM_EIO;
    }

    s_inited = 1;
    QM_LOGI(LOG_TAG, "init ok gpio%d leds %d", BSP_WS2812_GPIO, BSP_WS2812_LED_NUM);
    return QM_EOK;
}

qm_err_t bsp_ws2812_set_pixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_inited) {
        QM_LOGW(LOG_TAG, "set ignored, not inited");
        return -QM_EIO;
    }
    if (index >= BSP_WS2812_LED_NUM) {
        return -QM_EINVAL;
    }
    if (led_strip_set_pixel(s_strip, index, r, g, b) != ESP_OK) {
        return -QM_EIO;
    }
    return QM_EOK;
}

qm_err_t bsp_ws2812_refresh(void)
{
    if (!s_inited) {
        QM_LOGW(LOG_TAG, "refresh ignored, not inited");
        return -QM_EIO;
    }
    if (led_strip_refresh(s_strip) != ESP_OK) {
        QM_LOGE(LOG_TAG, "refresh fail");
        return -QM_EIO;
    }
    return QM_EOK;
}

qm_err_t bsp_ws2812_fill(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t i;

    if (!s_inited) {
        QM_LOGW(LOG_TAG, "fill ignored, not inited");
        return -QM_EIO;
    }

    for (i = 0; i < BSP_WS2812_LED_NUM; i++) {
        if (led_strip_set_pixel(s_strip, i, r, g, b) != ESP_OK) {
            return -QM_EIO;
        }
    }
    return bsp_ws2812_refresh();
}

qm_err_t bsp_ws2812_clear(void)
{
    if (!s_inited) {
        QM_LOGW(LOG_TAG, "clear ignored, not inited");
        return -QM_EIO;
    }
    if (led_strip_clear(s_strip) != ESP_OK) {
        QM_LOGE(LOG_TAG, "clear fail");
        return -QM_EIO;
    }
    return QM_EOK;
}

qm_err_t bsp_ws2812_start(void)
{
    if (s_running) {
        return QM_EOK;
    }
    if (bsp_ws2812_init() != QM_EOK) {
        return -QM_EIO;
    }
    s_state = BSP_WS2812_STATE_IDLE;
    s_running = 1;
    if (qm_task_new(&s_animation_task, "led_pet", ws2812_animation_task, NULL,
                    WS2812_TASK_STACK, WS2812_TASK_PRIORITY) != 0) {
        s_running = 0;
        QM_LOGE(LOG_TAG, "animation task create failed");
        return -QM_EIO;
    }
    QM_LOGI(LOG_TAG, "pet status lighting started");
    return QM_EOK;
}

void bsp_ws2812_set_state(bsp_ws2812_state_t state)
{
    if (state < BSP_WS2812_STATE_IDLE || state > BSP_WS2812_STATE_AWAY) {
        QM_LOGW(LOG_TAG, "invalid pet state %d", (int)state);
        return;
    }
    s_state = state;
}

/*
 * 先 init。set_pixel 只改缓冲，要 bsp_ws2812_refresh() 才发出去。
 * fill 和 clear 自己会发出去。参数顺序是 R、G、B，0 灭，255 最亮。
 * 下面的 48 大约是满亮度的五分之一。8 颗全白时每颗大约十几毫安。
 *
 *   bsp_ws2812_init();
 *   bsp_ws2812_set_pixel(0, 48, 0, 0);   // 第 0 颗红
 *   bsp_ws2812_set_pixel(1, 0, 48, 0);   // 第 1 颗绿
 *   bsp_ws2812_refresh();
 *
 *   bsp_ws2812_fill(0, 0, 48);           // 8 颗一起蓝
 *   bsp_ws2812_clear();                  // 全灭
 */
void bsp_ws2812_example(void)
{
    static const uint8_t colors[BSP_WS2812_LED_NUM][3] = {
        {48, 0,  0},  /* 红 */
        {0,  48, 0},  /* 绿 */
        {0,  0,  48}, /* 蓝 */
        {48, 48, 0},  /* 黄 */
        {0,  48, 48}, /* 青 */
        {48, 0,  48}, /* 紫 */
        {48, 24, 0},  /* 橙，绿通道用一半 */
        {48, 48, 48}, /* 白 */
    };
    uint32_t i;

    if (bsp_ws2812_init() != QM_EOK) {
        return;
    }
    for (i = 0; i < BSP_WS2812_LED_NUM; i++) {
        (void)bsp_ws2812_set_pixel(i, colors[i][0], colors[i][1], colors[i][2]);
    }
    (void)bsp_ws2812_refresh();
}
