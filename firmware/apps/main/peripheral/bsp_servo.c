#include "bsp_servo.h"

#include "qm_log.h"
#include "qm_pwm.h"

#define LOG_TAG "bsp_servo"

#define BSP_SERVO_PERIOD_US     (1000000u / BSP_SERVO_PWM_HZ)
#define BSP_SERVO_DUTY_MAX      ((1u << BSP_SERVO_PWM_RES_BIT) - 1u)

#define BSP_SERVO_MOVE_STACK    2048
#define BSP_SERVO_MOVE_PRIO     5

static qm_pwm_dev_t s_pwm = {
    .port = 0,
    .pin.pwm_pin = BSP_SERVO_PWM_PIN,
    .config.freq = BSP_SERVO_PWM_HZ,
};

static uint8_t s_inited;
static uint8_t s_move_run;
static uint16_t s_pulse_us;
static int16_t s_target_angle;
static qm_mutex_t s_lock;
static qm_task_t s_move_task;

static uint16_t servo_clamp_pulse(uint16_t pulse_us)
{
    if (pulse_us < BSP_SERVO_PULSE_MIN_US) {
        QM_LOGW(LOG_TAG, "pulse %u < %u, clamp",
                (unsigned)pulse_us, (unsigned)BSP_SERVO_PULSE_MIN_US);
        return BSP_SERVO_PULSE_MIN_US;
    }
    if (pulse_us > BSP_SERVO_PULSE_MAX_US) {
        QM_LOGW(LOG_TAG, "pulse %u > %u, clamp",
                (unsigned)pulse_us, (unsigned)BSP_SERVO_PULSE_MAX_US);
        return BSP_SERVO_PULSE_MAX_US;
    }
    return pulse_us;
}

static uint32_t servo_pulse_to_duty(uint16_t pulse_us)
{
    pulse_us = servo_clamp_pulse(pulse_us);
    return (uint32_t)pulse_us * BSP_SERVO_DUTY_MAX / BSP_SERVO_PERIOD_US;
}

static uint16_t servo_angle_to_pulse(int16_t angle)
{
    if (angle < BSP_SERVO_PULSE_ANGLE_MIN) {
        angle = BSP_SERVO_PULSE_ANGLE_MIN;
    }
    if (angle > BSP_SERVO_PULSE_ANGLE_MAX) {
        angle = BSP_SERVO_PULSE_ANGLE_MAX;
    }

    return (uint16_t)(BSP_SERVO_PULSE_MIN_US +
                      ((BSP_SERVO_PULSE_MAX_US - BSP_SERVO_PULSE_MIN_US) *
                       (int32_t)angle) / BSP_SERVO_PULSE_ANGLE_MAX);
}

static int16_t servo_pulse_to_angle(uint16_t pulse_us)
{
    if (pulse_us <= BSP_SERVO_PULSE_MIN_US) {
        return BSP_SERVO_PULSE_ANGLE_MIN;
    }
    if (pulse_us >= BSP_SERVO_PULSE_MAX_US) {
        return BSP_SERVO_PULSE_ANGLE_MAX;
    }
    return (int16_t)(((int32_t)(pulse_us - BSP_SERVO_PULSE_MIN_US) *
                      BSP_SERVO_PULSE_ANGLE_MAX) /
                     (BSP_SERVO_PULSE_MAX_US - BSP_SERVO_PULSE_MIN_US));
}

static int servo_apply_pulse(uint16_t pulse_us)
{
    qm_pwm_config_t para;

    pulse_us = servo_clamp_pulse(pulse_us);
    para.freq = BSP_SERVO_PWM_HZ;
    para.pulse = servo_pulse_to_duty(pulse_us);
    if (qm_pwm_para_change(&s_pwm, para) != QM_EOK) {
        QM_LOGW(LOG_TAG, "duty %uus fail", (unsigned)pulse_us);
        return -1;
    }
    s_pulse_us = pulse_us;
    return 0;
}

static uint16_t servo_pulse_step_us(void)
{
    int32_t pulse_span = BSP_SERVO_PULSE_MAX_US - BSP_SERVO_PULSE_MIN_US;

    return (uint16_t)((pulse_span * BSP_SERVO_MOVE_STEP_DEG) /
                      BSP_SERVO_PULSE_ANGLE_MAX);
}

static void servo_move_task(void *arg)
{
    const uint16_t pulse_step = servo_pulse_step_us();

    (void)arg;
    QM_LOGI(LOG_TAG, "move task start step=%uus/%dms",
            (unsigned)pulse_step, BSP_SERVO_MOVE_STEP_MS);

    while (s_move_run) {
        uint16_t target_pulse;
        uint16_t current_pulse;
        uint16_t next_pulse;

        qm_mutex_lock(&s_lock, QM_WAIT_FOREVER);
        target_pulse = servo_angle_to_pulse(s_target_angle);
        current_pulse = s_pulse_us;
        qm_mutex_unlock(&s_lock);

        if (current_pulse == target_pulse) {
            qm_msleep(BSP_SERVO_MOVE_STEP_MS);
            continue;
        }

        if (target_pulse > current_pulse) {
            next_pulse = (uint16_t)(current_pulse + pulse_step);
            if (next_pulse > target_pulse) {
                next_pulse = target_pulse;
            }
        } else {
            next_pulse = (current_pulse > pulse_step) ?
                         (uint16_t)(current_pulse - pulse_step) :
                         BSP_SERVO_PULSE_MIN_US;
            if (next_pulse < target_pulse) {
                next_pulse = target_pulse;
            }
        }

        qm_mutex_lock(&s_lock, QM_WAIT_FOREVER);
        (void)servo_apply_pulse(next_pulse);
        qm_mutex_unlock(&s_lock);
        qm_msleep(BSP_SERVO_MOVE_STEP_MS);
    }

    qm_task_exit(NULL);
}

qm_err_t bsp_servo_init(void)
{
    uint16_t boot_pulse;

    if (s_inited) {
        return QM_EOK;
    }

    if (qm_mutex_new(&s_lock) != QM_EOK) {
        QM_LOGE(LOG_TAG, "mutex create fail");
        return -QM_ENOMEM;
    }

    boot_pulse = BSP_SERVO_ZERO_PULSE_US;
    s_pwm.config.pulse = servo_pulse_to_duty(boot_pulse);
    if (qm_pwm_init(&s_pwm) != QM_EOK) {
        QM_LOGE(LOG_TAG, "pwm init fail");
        return -QM_EIO;
    }
    if (qm_pwm_start(&s_pwm) != QM_EOK) {
        QM_LOGE(LOG_TAG, "pwm start fail");
        return -QM_EIO;
    }

    s_pulse_us = boot_pulse;
    s_target_angle = servo_pulse_to_angle(boot_pulse);
    s_move_run = 1;

    if (qm_task_new(&s_move_task, "servo", servo_move_task, NULL,
                    BSP_SERVO_MOVE_STACK, BSP_SERVO_MOVE_PRIO) != 0) {
        QM_LOGE(LOG_TAG, "move task create fail");
        s_move_run = 0;
        (void)qm_pwm_stop(&s_pwm);
        return -QM_EIO;
    }
    s_inited = 1;

    QM_LOGI(LOG_TAG, "init ok gpio%d %uHz pulse=%uus angle=%d",
            BSP_SERVO_PWM_PIN, (unsigned)BSP_SERVO_PWM_HZ,
            (unsigned)s_pulse_us, (int)s_target_angle);
    return QM_EOK;
}

void bsp_servo_deinit(void)
{
    if (!s_inited) {
        return;
    }

    s_move_run = 0;
    qm_msleep(BSP_SERVO_MOVE_STEP_MS * 2);
    (void)qm_pwm_stop(&s_pwm);
    s_inited = 0;
}

void bsp_servo_set_pulse_us(uint16_t pulse_us)
{
    if (!s_inited) {
        QM_LOGW(LOG_TAG, "set_pulse_us ignored, not inited");
        return;
    }

    qm_mutex_lock(&s_lock, QM_WAIT_FOREVER);
    (void)servo_apply_pulse(pulse_us);
    s_target_angle = servo_pulse_to_angle(s_pulse_us);
    qm_mutex_unlock(&s_lock);
}

uint16_t bsp_servo_get_pulse_us(void)
{
    uint16_t pulse;

    if (!s_inited) {
        return 0;
    }
    qm_mutex_lock(&s_lock, QM_WAIT_FOREVER);
    pulse = s_pulse_us;
    qm_mutex_unlock(&s_lock);
    return pulse;
}

void bsp_servo_set_angle(int16_t angle)
{
    int16_t from;

    if (!s_inited) {
        QM_LOGW(LOG_TAG, "set_angle ignored, not inited");
        return;
    }

    if (angle < BSP_SERVO_PULSE_ANGLE_MIN) {
        angle = BSP_SERVO_PULSE_ANGLE_MIN;
    }
    if (angle > BSP_SERVO_PULSE_ANGLE_MAX) {
        angle = BSP_SERVO_PULSE_ANGLE_MAX;
    }

    qm_mutex_lock(&s_lock, QM_WAIT_FOREVER);
    if (angle == s_target_angle) {
        qm_mutex_unlock(&s_lock);
        return;
    }
    from = servo_pulse_to_angle(s_pulse_us);
    s_target_angle = angle;
    qm_mutex_unlock(&s_lock);
    QM_LOGI(LOG_TAG, "cmd angle %d -> %d pulse=%u",
            (int)from, (int)angle, (unsigned)servo_angle_to_pulse(angle));
}

int16_t bsp_servo_get_angle(void)
{
    int16_t angle;

    if (!s_inited) {
        return 0;
    }
    qm_mutex_lock(&s_lock, QM_WAIT_FOREVER);
    angle = servo_pulse_to_angle(s_pulse_us);
    qm_mutex_unlock(&s_lock);
    return angle;
}
