#include "breakpoint_pet.h"

#include <stdio.h>
#include <string.h>

#include "agent_app.h"
#include "bsp_servo.h"
#include "bsp_ws2812.h"
#include "qm_kv.h"
#include "qm_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define LOG_TAG "breakpoint_pet"

#define PET_KV_KEY              "breakpoint_pet"
#define PET_MAGIC               0x42504B31UL
#define PET_VERSION             1U
#define PET_CONTENT_BYTES       192U
#define PET_QUEUE_LENGTH        6U
#define PET_TASK_STACK          4096
#define PET_TASK_PRIORITY       6
#define PET_SPEECH_TIMEOUT_MS   18000U
#define PET_CONFIRM_TIMEOUT_MS  20000U

typedef enum {
    PET_STATE_IDLE = 0,
    PET_STATE_CONFIRM_SAVE,
    PET_STATE_CAPTURE_CONTENT,
    PET_STATE_AWAY,
} pet_state_t;

typedef enum {
    PET_CMD_VOICE = 0,
    PET_CMD_PRE_LEAVE,
    PET_CMD_RETURN,
} pet_command_type_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t pending;
    uint8_t completed;
    uint8_t reminded;
    char content[PET_CONTENT_BYTES];
} pet_store_t;

typedef struct {
    pet_command_type_t type;
    char text[PET_CONTENT_BYTES];
} pet_command_t;

static QueueHandle_t s_queue;
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[PET_QUEUE_LENGTH * sizeof(pet_command_t)];
static qm_task_t s_task;
static pet_state_t s_state = PET_STATE_IDLE;
static pet_state_t s_capture_return_state = PET_STATE_IDLE;
static uint8_t s_servo_ready;
static uint8_t s_initialized;

static const char *const s_yes_words[] = {"是", "好的", "好", "需要", "记录", "保存", "要"};
static const char *const s_no_words[] = {"不需要", "不用", "不要", "取消", "否"};
static const char *const s_manual_words[] = {"记录断点", "保存断点", "帮我记录", "记一下"};
static const char *const s_complete_words[] = {"完成断点", "任务完成", "已经完成", "标记完成"};

static int contains_any(const char *text, const char *const *words, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (strstr(text, words[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

static void pet_set_state(pet_state_t state)
{
    s_state = state;
    switch (state) {
    case PET_STATE_IDLE:
        bsp_ws2812_set_state(BSP_WS2812_STATE_IDLE);
        if (s_servo_ready) {
            bsp_servo_set_angle(BSP_SERVO_POSE_CENTER_DEG);
        }
        break;
    case PET_STATE_CONFIRM_SAVE:
        bsp_ws2812_set_state(BSP_WS2812_STATE_CONFIRM_SAVE);
        if (s_servo_ready) {
            bsp_servo_set_angle(BSP_SERVO_POSE_LOOK_AT_USER_DEG);
        }
        break;
    case PET_STATE_CAPTURE_CONTENT:
        bsp_ws2812_set_state(BSP_WS2812_STATE_CAPTURE_CONTENT);
        if (s_servo_ready) {
            bsp_servo_set_angle(BSP_SERVO_POSE_LISTEN_DEG);
        }
        break;
    case PET_STATE_AWAY:
        bsp_ws2812_set_state(BSP_WS2812_STATE_AWAY);
        if (s_servo_ready) {
            bsp_servo_set_angle(BSP_SERVO_POSE_SLEEP_DEG);
        }
        break;
    }
}

static int store_load(pet_store_t *store)
{
    int len = (int)sizeof(*store);

    memset(store, 0, sizeof(*store));
    if (qm_kv_get(PET_KV_KEY, store, &len) != QM_EOK ||
        len != (int)sizeof(*store) || store->magic != PET_MAGIC ||
        store->version != PET_VERSION) {
        memset(store, 0, sizeof(*store));
        store->magic = PET_MAGIC;
        store->version = PET_VERSION;
        return -QM_ENOENT;
    }
    store->content[PET_CONTENT_BYTES - 1U] = '\0';
    return QM_EOK;
}

static int store_save(const pet_store_t *store)
{
    return qm_kv_set(PET_KV_KEY, store, (int)sizeof(*store), 1);
}

static void pet_say(const char *text)
{
    int ret;

    ret = agent_app_user_corpus(text);
    if (ret != QM_EOK) {
        QM_LOGW(LOG_TAG, "speech request failed ret=%d text=%s", ret, text);
        return;
    }
    ret = agent_app_wait_tts_complete(PET_SPEECH_TIMEOUT_MS);
    if (ret != QM_EOK) {
        QM_LOGW(LOG_TAG, "speech wait failed ret=%d", ret);
    }
}

static void pet_say_saved(const char *content)
{
    char speech[PET_CONTENT_BYTES + 64U];

    (void)snprintf(speech, sizeof(speech), "好的，已经记录：%s。", content);
    pet_say(speech);
}

static void handle_pre_leave(void)
{
    if (s_state != PET_STATE_IDLE && s_state != PET_STATE_AWAY) {
        return;
    }
    pet_set_state(PET_STATE_CONFIRM_SAVE);
    pet_say("检测到你准备离开，需要帮你记录当前任务断点吗？");
}

static void handle_return(void)
{
    pet_store_t store;
    char speech[PET_CONTENT_BYTES + 64U];

    if (s_state == PET_STATE_IDLE || s_state == PET_STATE_CAPTURE_CONTENT) {
        return;
    }
    if (s_state == PET_STATE_CONFIRM_SAVE) {
        pet_set_state(PET_STATE_IDLE);
        pet_say("欢迎回来。需要记录时，随时告诉我记录断点。");
        return;
    }
    if (s_state != PET_STATE_AWAY) {
        return;
    }

    (void)store_load(&store);
    pet_set_state(PET_STATE_IDLE);
    if (!store.pending || store.completed || store.content[0] == '\0') {
        pet_say("欢迎回来，目前没有未完成的任务断点。");
        return;
    }
    store.reminded = 1;
    (void)store_save(&store);
    (void)snprintf(speech, sizeof(speech), "欢迎回来。你之前记录的是：%s。", store.content);
    pet_say(speech);
}

static void handle_voice(const char *input)
{
    char text[PET_CONTENT_BYTES];
    pet_store_t store;

    if (input == NULL || input[0] == '\0') {
        pet_say("我没有听清，请再说一次。");
        return;
    }
    strncpy(text, input, sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';

    if (contains_any(text, s_complete_words, sizeof(s_complete_words) / sizeof(s_complete_words[0]))) {
        (void)store_load(&store);
        if (!store.pending || store.completed || store.content[0] == '\0') {
            pet_say("目前没有未完成的断点。");
            return;
        }
        store.pending = 0;
        store.completed = 1;
        (void)store_save(&store);
        pet_set_state(PET_STATE_IDLE);
        pet_say("好的，最近的断点已经标记为完成。");
        return;
    }

    if ((s_state == PET_STATE_IDLE || s_state == PET_STATE_AWAY) &&
        contains_any(text, s_manual_words, sizeof(s_manual_words) / sizeof(s_manual_words[0]))) {
        s_capture_return_state = s_state;
        pet_set_state(PET_STATE_CAPTURE_CONTENT);
        pet_say("请告诉我需要记录什么。");
        return;
    }

    if (s_state == PET_STATE_CONFIRM_SAVE) {
        if (contains_any(text, s_no_words, sizeof(s_no_words) / sizeof(s_no_words[0]))) {
            pet_set_state(PET_STATE_AWAY);
            pet_say("好的，这次不记录。等你回来。");
            return;
        }
        if (contains_any(text, s_yes_words, sizeof(s_yes_words) / sizeof(s_yes_words[0]))) {
            s_capture_return_state = PET_STATE_AWAY;
            pet_set_state(PET_STATE_CAPTURE_CONTENT);
            pet_say("请告诉我你做到哪里，以及回来后要继续做什么。");
            return;
        }
        pet_say("请回答需要或不需要。");
        return;
    }

    if (s_state == PET_STATE_CAPTURE_CONTENT) {
        memset(&store, 0, sizeof(store));
        store.magic = PET_MAGIC;
        store.version = PET_VERSION;
        store.pending = 1;
        strncpy(store.content, text, sizeof(store.content) - 1U);
        if (store_save(&store) != QM_EOK) {
            pet_say("断点保存失败，请稍后再试。");
            return;
        }
        pet_set_state(s_capture_return_state);
        pet_say_saved(store.content);
        return;
    }

    pet_say("你可以说记录断点，或者等待我检测到你准备离开。");
}

static void breakpoint_pet_task(void *arg)
{
    pet_command_t command;
    TickType_t wait_ticks;

    (void)arg;
    for (;;) {
        wait_ticks = (s_state == PET_STATE_CONFIRM_SAVE)
                         ? pdMS_TO_TICKS(PET_CONFIRM_TIMEOUT_MS)
                         : portMAX_DELAY;
        if (xQueueReceive(s_queue, &command, wait_ticks) != pdTRUE) {
            if (s_state == PET_STATE_CONFIRM_SAVE) {
                QM_LOGI(LOG_TAG, "save prompt timed out; entering away state");
                pet_set_state(PET_STATE_AWAY);
            }
            continue;
        }
        switch (command.type) {
        case PET_CMD_VOICE:
            handle_voice(command.text);
            break;
        case PET_CMD_PRE_LEAVE:
            handle_pre_leave();
            break;
        case PET_CMD_RETURN:
            handle_return();
            break;
        }
    }
}

static qm_err_t enqueue_command(pet_command_type_t type, const char *text)
{
    pet_command_t command;

    if (!s_initialized || s_queue == NULL) {
        return -QM_EINIT;
    }
    memset(&command, 0, sizeof(command));
    command.type = type;
    if (text != NULL) {
        strncpy(command.text, text, sizeof(command.text) - 1U);
    }
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        QM_LOGW(LOG_TAG, "event queue full type=%d", (int)type);
        return -QM_EBUSY;
    }
    return QM_EOK;
}

qm_err_t breakpoint_pet_init(void)
{
    if (s_initialized) {
        return QM_EOK;
    }
    s_queue = xQueueCreateStatic(PET_QUEUE_LENGTH, sizeof(pet_command_t),
                                 s_queue_storage, &s_queue_struct);
    if (s_queue == NULL) {
        return -QM_ENOMEM;
    }
    if (bsp_servo_init() == QM_EOK) {
        s_servo_ready = 1;
    } else {
        QM_LOGW(LOG_TAG, "servo unavailable, continue without servo feedback");
    }
    pet_set_state(PET_STATE_IDLE);
    if (qm_task_new(&s_task, "breakpoint_pet", breakpoint_pet_task, NULL,
                    PET_TASK_STACK, PET_TASK_PRIORITY) != 0) {
        s_queue = NULL;
        return -QM_EIO;
    }
    s_initialized = 1;
    QM_LOGI(LOG_TAG, "breakpoint pet ready");
    return QM_EOK;
}

qm_err_t breakpoint_pet_receive_voice(const char *text)
{
    return enqueue_command(PET_CMD_VOICE, text);
}

qm_err_t breakpoint_pet_pre_leave_detected(void)
{
    return enqueue_command(PET_CMD_PRE_LEAVE, NULL);
}

qm_err_t breakpoint_pet_return_detected(void)
{
    return enqueue_command(PET_CMD_RETURN, NULL);
}
