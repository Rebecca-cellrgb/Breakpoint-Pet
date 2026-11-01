#include "agent_app.h"
#include "breakpoint_pet.h"
#include "ljyd_cloud.h"
#include "qm_config.h"

#if defined(CONFIG_LJYD_PLAYBACK_SUPPORT) && CONFIG_LJYD_PLAYBACK_SUPPORT
#include "playback.h"
#endif

#include "cJSON.h"
#include "qm_agent.h"
/* qdev has no public auth getter; keep this compatibility access app-local. */
#include "qm_agent_internal.h"
#include "qm_audio.h"
#include "qm_event.h"
#include "qm_http_client.h"
#include "qm_iot_dynreg.h"
#include "qm_log.h"
#include "qm_string.h"
#include "qm.h"
#include "voice.h"

#define LOG_TAG "agent"

static qm_agent_handle_t s_agent;
static uint8_t s_cloud_connected;
static uint8_t s_agent_started;
static uint8_t s_event_registered;
static qm_sem_t s_tts_complete_sem;
static uint8_t s_tts_complete_sem_ready;
static volatile uint8_t s_waiting_tts_complete;
static volatile uint8_t s_tts_wait_aborted;

static void abort_tts_wait(void);

#define AGENT_CORPUS_URL_MAX       160U
#define AGENT_CORPUS_TIMEOUT_MS    15000U

static const char *life_name(qm_agent_state_t state)
{
    switch (state) {
        case QM_AGENT_STATE_IDLE:
            return "idle";
        case QM_AGENT_STATE_READY:
            return "ready";
        case QM_AGENT_STATE_STARTING:
            return "starting";
        case QM_AGENT_STATE_RUNNING:
            return "running";
        case QM_AGENT_STATE_STOPPING:
            return "stopping";
        case QM_AGENT_STATE_ERROR:
            return "error";
        case QM_AGENT_STATE_NONE:
        default:
            return "none";
    }
}

static const char *dialog_name(qm_agent_dialog_state_t state)
{
    switch (state) {
        case QM_AGENT_DIALOG_STATE_SILENT:
            return "silent";
        case QM_AGENT_DIALOG_STATE_LISTENING:
            return "listening";
        case QM_AGENT_DIALOG_STATE_THINKING:
            return "thinking";
        case QM_AGENT_DIALOG_STATE_SPEAKING:
            return "speaking";
        case QM_AGENT_DIALOG_STATE_NONE:
        default:
            return "none";
    }
}

static void on_agent_event(void *event_handler_arg,
                           qm_event_base_t event_base,
                           int32_t event_id,
                           void *event_data,
                           size_t event_data_size)
{
    const qm_agent_event_notice_t *notice = (const qm_agent_event_notice_t *)event_data;

    (void)event_handler_arg;
    if (event_base != QM_AGENT_EVENT_BASE || notice == NULL ||
        event_data_size != sizeof(*notice)) {
        return;
    }

    switch ((qm_agent_event_id_t)event_id) {
        case QM_AGENT_EVENT_STATE_CHANGED:
            QM_LOGI(LOG_TAG, "state %s -> %s",
                    life_name(notice->data.state.old_state),
                    life_name(notice->data.state.new_state));
            break;
        case QM_AGENT_EVENT_CONNECTED:
            QM_LOGI(LOG_TAG, "agora connected");
            break;
        case QM_AGENT_EVENT_DISCONNECTED:
            QM_LOGW(LOG_TAG, "agora disconnected");
            break;
        case QM_AGENT_EVENT_ASR:
            QM_LOGI(LOG_TAG, "asr final=%d text=%s",
                    notice->data.text.final,
                    notice->data.text.text);
            if (notice->data.text.final && notice->data.text.text[0] != '\0') {
                (void)agent_app_abort_output();
                (void)breakpoint_pet_receive_voice(notice->data.text.text);
            }
            break;
        case QM_AGENT_EVENT_TTS:
            QM_LOGI(LOG_TAG, "tts final=%d text=%s",
                    notice->data.text.final,
                    notice->data.text.text);
            break;
        case QM_AGENT_EVENT_AGENT_TEXT:
            QM_LOGI(LOG_TAG, "agent text final=%d text=%s",
                    notice->data.text.final,
                    notice->data.text.text);
            break;
        case QM_AGENT_EVENT_AGENT_STATE:
            QM_LOGI(LOG_TAG, "dialog=%s",
                    dialog_name(notice->data.agent_state.state));
            /* The core may re-enable upload when it reports speaking. The
             * custom-corpus transaction owns upload until its local TTS ends. */
            if (s_waiting_tts_complete && s_agent != NULL) {
                qm_agent_set_upload_enabled(s_agent, QM_FALSE);
            }
            break;
        case QM_AGENT_EVENT_TTS_PLAYBACK_COMPLETE:
            if (s_waiting_tts_complete && s_tts_complete_sem_ready) {
                (void)qm_sem_signal(&s_tts_complete_sem);
            }
            QM_LOGI(LOG_TAG, "tts playback complete generation=%u",
                    (unsigned)notice->generation);
            break;
        case QM_AGENT_EVENT_ERROR:
            QM_LOGE(LOG_TAG, "agent error code=%d msg=%s",
                    notice->code,
                    notice->data.text.text);
            break;
        default:
            QM_LOGD(LOG_TAG, "agent event id=%d", (int)event_id);
            break;
    }
}

static int register_agent_events(void)
{
    int ret;

    if (s_event_registered) {
        return QM_EOK;
    }
    ret = qm_event_default_create();
    if (ret != QM_EOK && ret != -QM_EINIT) {
        return ret;
    }
    ret = qm_event_handler_register(QM_AGENT_EVENT_BASE,
                                    QM_EVENT_ANY_ID,
                                    on_agent_event,
                                    NULL);
    if (ret == QM_EOK) {
        s_event_registered = 1;
    }
    return ret;
}

static int agent_app_bind_cloud(void *iot_handle)
{
    qm_agent_config_t config;
    qm_audio_control_t *control;
    qm_audio_stream_format_t format;
    int ret;

    if (s_agent != NULL) {
        return QM_EOK;
    }
    if (iot_handle == NULL) {
        return -QM_EINVAL;
    }

    control = voice_get_control();
    if (control == NULL) {
        QM_LOGE(LOG_TAG, "bind failed: voice audio is null");
        return -QM_EINIT;
    }

    ret = qm_audio_get_input_stream_format(&format);
    if (ret != QM_EOK) {
        QM_LOGE(LOG_TAG, "get input format failed ret=%d", ret);
        return ret;
    }
    QM_LOGI(LOG_TAG, "voice uplink opus rate=%u ch=%u",
            (unsigned)format.pcm.sample_rate_hz,
            (unsigned)format.pcm.channels);

    ret = register_agent_events();
    if (ret != QM_EOK) {
        QM_LOGE(LOG_TAG, "event register failed ret=%d", ret);
        return ret;
    }

    qm_memset(&config, 0, sizeof(config));
    config.transport.type = QM_AGENT_TRANSPORT_AGORA;
    config.audio.control = control;
    config.audio.capture_enabled = QM_TRUE;
    /*
     * 跟设备对外格式一致：按包收 Opus，包长不固定。芯片 PCM 的编解码在设备里。
     * 零下行格式表示喇叭也走这份 Opus，设备再解成 PCM。
     */
    config.audio.uplink_format.codec = QM_AGENT_AUDIO_CODEC_OPUS;
    config.audio.uplink_format.data_type = QM_AGENT_AUDIO_CODEC_OPUS;
    config.audio.uplink_format.sample_rate = format.pcm.sample_rate_hz;
    config.audio.uplink_format.channel_num = format.pcm.channels;
    config.audio.uplink_format.bits_per_sample = 16;
    config.audio.uplink_format.frame_ms = (uint16_t)format.packet_duration_ms;
    config.audio.uplink_format.framing = format.packet_bytes == 0 ?
        QM_AGENT_AUDIO_FRAMING_OPUS_PACKET : QM_AGENT_AUDIO_FRAMING_OPUS_CBR;
    config.audio.uplink_format.packet_bytes = format.packet_bytes;
    /*
     * VB701N 的 0x0201 会把喇叭一起停。
     * 对话期间用 KEEP，TTS 下行时不要把麦停掉。
     */
    config.transport_capture_policy = QM_AGENT_CAPTURE_POLICY_KEEP;
    qm_agent_config_init_defaults(&config);
    /* app_id / channel / token / license / uid 由 qm_agent 从鉴权回包填入。 */
    config.transport.data.agora.audio_enable = QM_TRUE;
    config.transport.data.agora.video_enable = QM_FALSE;
    config.iot_handle = iot_handle;

    s_agent = qm_agent_create(&config);
    if (s_agent == NULL) {
        QM_LOGE(LOG_TAG, "qm_agent_create failed");
        return -QM_EINIT;
    }
    QM_LOGI(LOG_TAG, "session created state=%s",
            life_name(qm_agent_get_state(s_agent)));
    return QM_EOK;
}

static void agent_on_cloud_connected(void)
{
    s_cloud_connected = 1;
    if (s_agent == NULL) {
        return;
    }
    if (qm_agent_network_update(s_agent, QM_AGENT_NETWORK_UP) != QM_EOK) {
        QM_LOGW(LOG_TAG, "network up failed");
    }
}

static void agent_on_cloud_disconnected(void)
{
    int ret;

    s_cloud_connected = 0;
    s_agent_started = 0;
    abort_tts_wait();
    if (s_agent == NULL) {
        return;
    }
    (void)qm_agent_network_update(s_agent, QM_AGENT_NETWORK_DOWN);
    ret = qm_agent_stop(s_agent);
    QM_LOGI(LOG_TAG, "qm_agent_stop ret=%d state=%s",
            ret, life_name(qm_agent_get_state(s_agent)));
}

static void on_cloud_event(ljyd_cloud_event_t event, void *iot_handle, void *userdata)
{
    (void)userdata;
    (void)iot_handle;
    if (event == LJYD_CLOUD_EVENT_CONNECTED) {
        agent_on_cloud_connected();
        return;
    }
    if (event == LJYD_CLOUD_EVENT_DISCONNECTED) {
        agent_on_cloud_disconnected();
    }
}

int agent_app_leave(void)
{
    int ret;

    if (s_agent == NULL) {
        return -QM_EINVAL;
    }
    abort_tts_wait();
    ret = qm_agent_stop(s_agent);
    if (ret == QM_EOK) {
        s_agent_started = 0;
    }
    QM_LOGI(LOG_TAG, "leave stop ret=%d state=%s",
            ret, life_name(qm_agent_get_state(s_agent)));
    return ret;
}

int agent_app_is_active(void)
{
    return (s_agent_started != 0) ? 1 : 0;
}

int agent_app_play_local_audio(const void *data, uint32_t len)
{
    qm_audio_control_t *control = NULL;
    qm_audio_control_play_request_t request;
    qm_audio_operation_id_t operation = 0;
    int ret;

    if (data == NULL || len == 0U) {
        return -QM_EINVAL;
    }
    if (!voice_audio_ready()) {
        return -QM_EINIT;
    }

    ret = qm_audio_control_create(&control);
    if (ret != QM_EOK || control == NULL) {
        return ret != QM_EOK ? ret : -QM_EINIT;
    }

    qm_memset(&request, 0, sizeof(request));
    request.source.type = QM_AUDIO_SOURCE_MEMORY;
    request.source.value.memory.data = data;
    request.source.value.memory.size = len;
    request.codec.codec = QM_AUDIO_CODEC_OPUS;
    request.codec.framing = QM_AUDIO_CODEC_FRAMING_STREAM;
    request.codec.packet_duration_ms = 20;
    request.codec.pcm.sample_rate_hz = 16000;
    request.codec.pcm.channels = 1;
    request.codec.pcm.sample = QM_AUDIO_PCM_S16;
    request.priority = QM_AUDIO_PRIORITY_HIGH;
    request.admission = QM_AUDIO_ADMISSION_PREEMPT_LOWER;
    request.admission_timeout_ms = 800;
    ret = qm_audio_control_play(control, &request, &operation);
    if (ret == QM_EOK) {
        ret = qm_audio_control_wait(control, operation, 3000);
        (void)qm_audio_control_release(control, operation, 1000);
    }
    (void)qm_audio_control_shutdown(control, 1000);
    (void)qm_audio_control_destroy(control);
    QM_LOGI(LOG_TAG, "local prompt len=%u ret=%d", (unsigned)len, ret);
    return ret;
}

static void drain_tts_complete_sem(void)
{
    if (!s_tts_complete_sem_ready) {
        return;
    }
    while (qm_sem_wait(&s_tts_complete_sem, QM_NO_WAIT) == QM_EOK) {
    }
}

static void abort_tts_wait(void)
{
    if (!s_waiting_tts_complete) {
        return;
    }
    s_tts_wait_aborted = 1U;
    if (s_tts_complete_sem_ready) {
        (void)qm_sem_signal(&s_tts_complete_sem);
    }
}

int agent_app_set_resource_mode(qm_bool active)
{
    if (!voice_audio_ready()) {
        return -QM_EINIT;
    }
    if (s_agent != NULL) {
        qm_agent_set_upload_enabled(s_agent,
                                    active ? QM_FALSE :
                                    (s_cloud_connected && s_agent_started) ? QM_TRUE : QM_FALSE);
    }
    return QM_EOK;
}

int agent_app_abort_output(void)
{
    int ret = QM_EOK;

    if (s_agent != NULL) {
        ret = qm_agent_interrupt(s_agent);
    }
    abort_tts_wait();
    QM_LOGI(LOG_TAG, "abort output ret=%d", ret);
    return ret;
}

static const char *corpus_host_prefix(void)
{
    switch (qm_iot_dynreg_env_get()) {
        case QM_IOT_DYNREG_ENV_TEST:
            return "api-test";
        case QM_IOT_DYNREG_ENV_PROD:
            return "api";
        case QM_IOT_DYNREG_ENV_DEV:
        default:
            return "api-dev";
    }
}

static int get_agent_auth_info(qm_agent_auth_info_t *info)
{
    int ret;

    if (s_agent == NULL || info == NULL) {
        return -QM_EINVAL;
    }
    qm_memset(info, 0, sizeof(*info));
    if (s_agent->auth.helper == NULL || !s_agent->auth.has_auth) {
        return -QM_ENOENT;
    }
    ret = qm_agent_auth_get_token(s_agent->auth.helper, info);
    return ret;
}

int agent_app_user_corpus(const char *text)
{
    qm_agent_auth_info_t auth = {0};
    qm_http_client_request_t request = {0};
    qm_http_client_response_t response = {0};
    qm_http_header_t header;
    cJSON *root;
    char url[AGENT_CORPUS_URL_MAX];
    char *body;
    int ret;

    if (text == NULL || text[0] == '\0') {
        return -QM_EINVAL;
    }
    if (!s_cloud_connected || s_agent == NULL) {
        return -QM_EBUSY;
    }
    ret = get_agent_auth_info(&auth);
    if (ret != QM_EOK || auth.ljyd.token == NULL || auth.ljyd.token[0] == '\0') {
        if (ret == QM_EOK) {
            ret = -QM_ENOENT;
        }
        QM_LOGW(LOG_TAG, "user corpus auth unavailable ret=%d", ret);
        qm_agent_auth_info_deinit(&auth);
        return ret;
    }

    root = cJSON_CreateObject();
    if (root == NULL || !cJSON_AddStringToObject(root, "content", text)) {
        cJSON_Delete(root);
        qm_agent_auth_info_deinit(&auth);
        return -QM_ENOMEM;
    }
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        qm_agent_auth_info_deinit(&auth);
        return -QM_ENOMEM;
    }
    ret = qm_snprintf(url, sizeof(url),
                      "https://%s.ljyd.cn/agent-device/agent/sendSpeakRequest",
                      corpus_host_prefix());
    if (ret <= 0 || ret >= (int)sizeof(url)) {
        cJSON_free(body);
        qm_agent_auth_info_deinit(&auth);
        return -QM_EFULL;
    }

    drain_tts_complete_sem();
    s_tts_wait_aborted = 0U;
    s_waiting_tts_complete = 1U;
    header.name = "Authorization";
    header.value = auth.ljyd.token;
    request.method = QM_HTTP_POST;
    request.url = url;
    request.body = body;
    request.body_len = (int)qm_strlen(body);
    request.content_type = "application/json";
    request.headers = &header;
    request.header_count = 1;
    request.timeout_ms = AGENT_CORPUS_TIMEOUT_MS;
    QM_LOGI(LOG_TAG, "user corpus request url=%s text_len=%u",
            url, (unsigned)qm_strlen(text));
    ret = qm_http_client_execute(&request, &response);
    if (ret == QM_EOK && response.status_code != 200) {
        ret = -QM_EIO;
    }
    if (ret != QM_EOK) {
        s_waiting_tts_complete = 0U;
    }
    QM_LOGI(LOG_TAG, "user corpus response ret=%d status=%d",
            ret, response.status_code);
    cJSON_free(body);
    qm_agent_auth_info_deinit(&auth);
    return ret;
}

int agent_app_wait_tts_complete(uint32_t timeout_ms)
{
    int ret;
    qm_bool aborted;

    if (!s_tts_complete_sem_ready || !s_waiting_tts_complete) {
        return -QM_EINVAL;
    }
    ret = qm_sem_wait(&s_tts_complete_sem, timeout_ms);
    aborted = s_tts_wait_aborted ? QM_TRUE : QM_FALSE;
    s_waiting_tts_complete = 0U;
    s_tts_wait_aborted = 0U;
    if (aborted) {
        return -QM_EINTR;
    }
    if (ret != QM_EOK) {
        QM_LOGW(LOG_TAG, "wait tts playback complete timeout=%u",
                (unsigned)timeout_ms);
        return -QM_ETIMEOUT;
    }
    return QM_EOK;
}

static void on_wakeup(void)
{
    int ret;

    /* A wake word always takes ownership back from active output. */
#if defined(CONFIG_LJYD_PLAYBACK_SUPPORT) && CONFIG_LJYD_PLAYBACK_SUPPORT
    (void)playback_stop();
#endif

    if (s_agent == NULL || !s_cloud_connected) {
        QM_LOGI(LOG_TAG, "wakeup ignored, cloud=%u agent=%p",
                (unsigned)s_cloud_connected, (void *)s_agent);
        return;
    }
    if (s_agent_started) {
        (void)agent_app_abort_output();
        return;
    }
    ret = qm_agent_start(s_agent);
    QM_LOGI(LOG_TAG, "qm_agent_start ret=%d state=%s",
            ret, life_name(qm_agent_get_state(s_agent)));
    if (ret == QM_EOK) {
        s_agent_started = 1;
    }
}

int agent_app_init(void *iot_handle)
{
    int ret;

    if (!s_tts_complete_sem_ready) {
        ret = qm_sem_new(&s_tts_complete_sem, 0);
        if (ret != QM_EOK || !qm_sem_is_valid(&s_tts_complete_sem)) {
            return -QM_ENOMEM;
        }
        s_tts_complete_sem_ready = 1U;
    }
    ret = agent_app_bind_cloud(iot_handle);
    if (ret != QM_EOK) {
        return ret;
    }
    ljyd_cloud_set_event_cb(on_cloud_event, NULL);
    voice_set_wakeup_cb(on_wakeup);
    return QM_EOK;
}
