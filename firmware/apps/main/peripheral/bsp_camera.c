#include "bsp_camera.h"

#include "esp_cam_ctlr_spi.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "breakpoint_pet.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <unistd.h>

#define LOG_TAG "bsp_camera"

#define CAM_BUF_NUM             2
#define CAM_I2C_FREQ_HZ         100000
#define CAM_STREAM_PRIO         12
#define CAM_STREAM_CORE         1
#define CAM_DQBUF_POLL_MS       20
#define CAM_LOG_EVERY_FRAMES    30
#define CAM_STREAM_STACK        (6 * 1024)
#define CAM_STACK_CAPS          (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CAM_DISCOVERY_PORT      8764
#define CAM_STREAM_PORT         8765
#define CAM_DISCOVERY_QUERY     "CAMDISC1"
#define CAM_FRAME_HEADER_SIZE   12
#define CAM_PET_PRE_LEAVE       "PET1:PRE_LEAVE"
#define CAM_PET_RETURN          "PET1:RETURN"
#define CAM_PET_ACK_PRE_LEAVE   "PET1:ACK:PRE_LEAVE"
#define CAM_PET_ACK_RETURN      "PET1:ACK:RETURN"

typedef struct {
    int fd;
    uint8_t *buffer[CAM_BUF_NUM];
    size_t buffer_len[CAM_BUF_NUM];
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    bool stream_on;
} bsp_cam_t;

static bsp_cam_t s_cam = {
    .fd = -1,
};
static bool s_available;
static bool s_video_ready;
static TaskHandle_t s_stream_task;

static bool cam_send_all(int fd, const uint8_t *data, size_t len)
{
    while (len > 0) {
        int sent = send(fd, data, len, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        len -= (size_t)sent;
    }
    return true;
}

static void cam_write_u16_be(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void cam_write_u32_be(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static int cam_open_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(LOG_TAG, "TCP socket failed, errno=%d", errno);
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CAM_STREAM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 1) != 0) {
        ESP_LOGE(LOG_TAG, "TCP listen failed, errno=%d", errno);
        close(fd);
        return -1;
    }
    return fd;
}

static int cam_open_discovery(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(LOG_TAG, "UDP socket failed, errno=%d", errno);
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CAM_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(LOG_TAG, "UDP discovery bind failed, errno=%d", errno);
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ESP_LOGE(LOG_TAG, "UDP discovery non-blocking setup failed, errno=%d", errno);
        close(fd);
        return -1;
    }
    return fd;
}

static bool cam_send_frame(int client_fd, const uint8_t *data, size_t len)
{
    uint8_t header[CAM_FRAME_HEADER_SIZE] = {'U', 'Y', 'V', 'Y'};
    cam_write_u16_be(&header[4], (uint16_t)s_cam.width);
    cam_write_u16_be(&header[6], (uint16_t)s_cam.height);
    cam_write_u32_be(&header[8], (uint32_t)len);
    return cam_send_all(client_fd, header, sizeof(header)) && cam_send_all(client_fd, data, len);
}

static void cam_process_discovery_messages(int discovery_fd)
{
    for (;;) {
        char packet[32];
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int received = recvfrom(discovery_fd, packet, sizeof(packet), 0,
                                (struct sockaddr *)&peer, &peer_len);
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ESP_LOGW(LOG_TAG, "UDP receive failed, errno=%d", errno);
            }
            return;
        }

        if (received == (int)(sizeof(CAM_DISCOVERY_QUERY) - 1) &&
            memcmp(packet, CAM_DISCOVERY_QUERY, sizeof(CAM_DISCOVERY_QUERY) - 1) == 0) {
            static const char response[] = "CAMERA1";
            (void)sendto(discovery_fd, response, sizeof(response) - 1, 0,
                         (struct sockaddr *)&peer, peer_len);
            ESP_LOGI(LOG_TAG, "viewer discovered from %s", inet_ntoa(peer.sin_addr));
        } else if (received == (int)(sizeof(CAM_PET_PRE_LEAVE) - 1) &&
                   memcmp(packet, CAM_PET_PRE_LEAVE, sizeof(CAM_PET_PRE_LEAVE) - 1) == 0) {
            if (breakpoint_pet_pre_leave_detected() == QM_EOK) {
                static const char response[] = CAM_PET_ACK_PRE_LEAVE;
                (void)sendto(discovery_fd, response, sizeof(response) - 1, 0,
                             (struct sockaddr *)&peer, peer_len);
                ESP_LOGI(LOG_TAG, "posture event: pre-leave");
            }
        } else if (received == (int)(sizeof(CAM_PET_RETURN) - 1) &&
                   memcmp(packet, CAM_PET_RETURN, sizeof(CAM_PET_RETURN) - 1) == 0) {
            if (breakpoint_pet_return_detected() == QM_EOK) {
                static const char response[] = CAM_PET_ACK_RETURN;
                (void)sendto(discovery_fd, response, sizeof(response) - 1, 0,
                             (struct sockaddr *)&peer, peer_len);
                ESP_LOGI(LOG_TAG, "posture event: return");
            }
        }
    }
}

static void cam_pwdn_hold_low(void)
{
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << BSP_CAM_PWDN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&conf));
    ESP_ERROR_CHECK(gpio_set_level(BSP_CAM_PWDN_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(gpio_set_level(BSP_CAM_PWDN_GPIO, 0));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(LOG_TAG, "PWDN GPIO%d pulsed then held low", BSP_CAM_PWDN_GPIO);
}

static void cam_teardown(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (s_cam.fd >= 0 && s_cam.stream_on) {
        ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
        s_cam.stream_on = false;
    }
    for (int i = 0; i < CAM_BUF_NUM; i++) {
        if (s_cam.buffer[i] && s_cam.buffer[i] != MAP_FAILED) {
            munmap(s_cam.buffer[i], s_cam.buffer_len[i]);
            s_cam.buffer[i] = NULL;
            s_cam.buffer_len[i] = 0;
        }
    }
    if (s_cam.fd >= 0) {
        close(s_cam.fd);
        s_cam.fd = -1;
    }
}

static esp_err_t cam_video_init(void)
{
    static const esp_video_init_spi_config_t spi_config = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = BSP_CAM_I2C_PORT,
                .scl_pin = BSP_CAM_SCL_GPIO,
                .sda_pin = BSP_CAM_SDA_GPIO,
            },
            .freq = CAM_I2C_FREQ_HZ,
        },
        .intf = ESP_CAM_CTLR_SPI_CAM_INTF_SPI,
        .io_mode = ESP_CAM_CTLR_SPI_CAM_IO_MODE_1BIT,
        .spi_port = SPI2_HOST,
        .spi_cs_pin = BSP_CAM_CS_GPIO,
        .spi_sclk_pin = BSP_CAM_SCLK_GPIO,
        .spi_data0_io_pin = BSP_CAM_DATA0_GPIO,
        .spi_data1_io_pin = -1,
        .spi_data2_io_pin = -1,
        .spi_data3_io_pin = -1,
        .reset_pin = BSP_CAM_RESET_GPIO,
        .pwdn_pin = -1,
        .xclk_source = ESP_CAM_SENSOR_XCLK_LEDC,
        .xclk_freq = 0,
        .xclk_pin = BSP_CAM_XCLK_GPIO,
#if CONFIG_CAMERA_XCLK_USE_LEDC
        .xclk_ledc_cfg = {
            .timer = LEDC_TIMER_3,
            .clk_cfg = LEDC_AUTO_CLK,
            .channel = LEDC_CHANNEL_7,
        },
#endif
    };
    const esp_video_init_config_t cam_config = {
        .spi = &spi_config,
    };

    ESP_LOGI(LOG_TAG,
             "pins SDA=%d SCL=%d CS=%d SCLK=%d DATA0=%d PWDN=%d SPI_HOST=%d",
             BSP_CAM_SDA_GPIO, BSP_CAM_SCL_GPIO, BSP_CAM_CS_GPIO,
             BSP_CAM_SCLK_GPIO, BSP_CAM_DATA0_GPIO, BSP_CAM_PWDN_GPIO,
             (int)SPI2_HOST);
    return esp_video_init(&cam_config);
}

static esp_err_t cam_setup_buffers(void)
{
    struct v4l2_format format;
    struct v4l2_requestbuffers req;

    if (s_cam.fd >= 0 && s_cam.buffer[0] != NULL) {
        return ESP_OK;
    }

    if (s_cam.fd < 0) {
        s_cam.fd = open(ESP_VIDEO_SPI_DEVICE_NAME, O_RDWR);
        ESP_RETURN_ON_FALSE(s_cam.fd >= 0, ESP_ERR_NOT_FOUND, LOG_TAG,
                            "open %s failed, errno=%d", ESP_VIDEO_SPI_DEVICE_NAME, errno);
    }

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_ERROR(ioctl(s_cam.fd, VIDIOC_G_FMT, &format) == 0 ? ESP_OK : ESP_FAIL,
                        LOG_TAG, "VIDIOC_G_FMT failed, errno=%d", errno);
    s_cam.width = format.fmt.pix.width;
    s_cam.height = format.fmt.pix.height;
    s_cam.pixel_format = format.fmt.pix.pixelformat;
    ESP_LOGI(LOG_TAG, "format %" PRIu32 "x%" PRIu32 " fourcc=0x%08" PRIx32,
             s_cam.width, s_cam.height, s_cam.pixel_format);

    memset(&req, 0, sizeof(req));
    req.count = CAM_BUF_NUM;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_ERROR(ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) == 0 ? ESP_OK : ESP_FAIL,
                        LOG_TAG, "VIDIOC_REQBUFS failed, errno=%d", errno);

    for (int i = 0; i < CAM_BUF_NUM; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_RETURN_ON_ERROR(ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) == 0 ? ESP_OK : ESP_FAIL,
                            LOG_TAG, "VIDIOC_QUERYBUF[%d] failed, errno=%d", i, errno);
        s_cam.buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                               s_cam.fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(s_cam.buffer[i] != MAP_FAILED, ESP_ERR_NO_MEM, LOG_TAG,
                            "mmap[%d] failed, errno=%d", i, errno);
        s_cam.buffer_len[i] = buf.length;
        ESP_RETURN_ON_ERROR(ioctl(s_cam.fd, VIDIOC_QBUF, &buf) == 0 ? ESP_OK : ESP_FAIL,
                            LOG_TAG, "VIDIOC_QBUF[%d] failed, errno=%d", i, errno);
    }

    ESP_LOGI(LOG_TAG, "buffers ready, STREAMON deferred");
    return ESP_OK;
}

static esp_err_t cam_stream_on(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (s_cam.stream_on) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ioctl(s_cam.fd, VIDIOC_STREAMON, &type) == 0 ? ESP_OK : ESP_FAIL,
                        LOG_TAG, "VIDIOC_STREAMON failed, errno=%d", errno);
    s_cam.stream_on = true;
    ESP_LOGI(LOG_TAG, "stream on, %s", ESP_VIDEO_SPI_DEVICE_NAME);
    return ESP_OK;
}

static void cam_stream_task(void *arg)
{
    uint32_t frames = 0;
    uint32_t good = 0;
    uint32_t empty = 0;
    TickType_t t0 = xTaskGetTickCount();
    const uint32_t expect = s_cam.width * s_cam.height * 2;
    int listener_fd = -1;
    int discovery_fd = -1;
    int client_fd = -1;

    (void)arg;
    ESP_LOGI(LOG_TAG, "stream task on core %d", (int)xPortGetCoreID());

    for (;;) {
        if (listener_fd < 0 || discovery_fd < 0) {
            if (listener_fd >= 0) close(listener_fd);
            if (discovery_fd >= 0) close(discovery_fd);
            listener_fd = cam_open_listener();
            discovery_fd = cam_open_discovery();
            if (listener_fd < 0 || discovery_fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(LOG_TAG, "live view ready: UDP %d, TCP %d",
                     CAM_DISCOVERY_PORT, CAM_STREAM_PORT);
        }

        if (client_fd < 0) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listener_fd, &read_fds);
            FD_SET(discovery_fd, &read_fds);
            int max_fd = listener_fd > discovery_fd ? listener_fd : discovery_fd;
            struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
            int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
            if (ready < 0) {
                ESP_LOGE(LOG_TAG, "select failed, errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            if (ready == 0) {
                continue;
            }
            if (FD_ISSET(discovery_fd, &read_fds)) {
                cam_process_discovery_messages(discovery_fd);
            }
            if (FD_ISSET(listener_fd, &read_fds)) {
                client_fd = accept(listener_fd, NULL, NULL);
                if (client_fd >= 0) {
                    struct timeval send_timeout = {.tv_sec = 2, .tv_usec = 0};
                    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                               &send_timeout, sizeof(send_timeout));
                    ESP_LOGI(LOG_TAG, "live viewer connected");
                }
            }
            continue;
        }

        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
            if (errno != EAGAIN && errno != EINTR) {
                ESP_LOGE(LOG_TAG, "VIDIOC_DQBUF failed, errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(CAM_DQBUF_POLL_MS));
            }
            continue;
        }

        frames++;
        cam_process_discovery_messages(discovery_fd);
        if (buf.bytesused == expect) {
            good++;
        } else {
            empty++;
        }
        if ((frames % CAM_LOG_EVERY_FRAMES) == 0) {
            TickType_t now = xTaskGetTickCount();
            uint32_t ms = (uint32_t)((now - t0) * portTICK_PERIOD_MS);
            ESP_LOGI(LOG_TAG, "got %u frames good=%u empty=%u last_bytes=%u dt=%u ms",
                     (unsigned)frames, (unsigned)good, (unsigned)empty,
                     (unsigned)buf.bytesused, (unsigned)ms);
            t0 = now;
        }

        if (buf.bytesused == expect && !cam_send_frame(client_fd, s_cam.buffer[buf.index], buf.bytesused)) {
            ESP_LOGW(LOG_TAG, "viewer disconnected during frame transfer");
            close(client_fd);
            client_fd = -1;
        }

        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(LOG_TAG, "VIDIOC_QBUF return failed, errno=%d", errno);
        }
        if (buf.bytesused != expect) {
            vTaskDelay(pdMS_TO_TICKS(CAM_DQBUF_POLL_MS));
        }
    }
}

esp_err_t bsp_camera_early_init(void)
{
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 1; attempt <= 3; attempt++) {
        cam_pwdn_hold_low();

        if (!s_video_ready) {
            ret = cam_video_init();
            if (ret != ESP_OK) {
                ESP_LOGW(LOG_TAG, "camera init attempt %d/3 failed: %s",
                         attempt, esp_err_to_name(ret));
                goto retry;
            }
            s_video_ready = true;
        }

        ret = cam_setup_buffers();
        if (ret == ESP_OK) {
            s_available = true;
            ESP_LOGI(LOG_TAG, "early init ok, wait for stream_start");
            return ESP_OK;
        }

        ESP_LOGW(LOG_TAG, "camera buffer setup attempt %d/3 failed: %s",
                 attempt, esp_err_to_name(ret));
        cam_teardown();
        esp_video_deinit();
        s_video_ready = false;

retry:
        if (attempt < 3) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

    ESP_LOGE(LOG_TAG, "camera init failed after 3 attempts");
    return ret;
}

esp_err_t bsp_camera_stream_start(void)
{
    if (s_stream_task != NULL) {
        return ESP_OK;
    }
    if (!s_available) {
        ESP_LOGW(LOG_TAG, "camera not ready; retrying early_init before stream_start");
        if (bsp_camera_early_init() != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (cam_stream_on() != ESP_OK) {
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCoreWithCaps(cam_stream_task, "cam_stream", CAM_STREAM_STACK,
                                        NULL, CAM_STREAM_PRIO, &s_stream_task,
                                        CAM_STREAM_CORE, CAM_STACK_CAPS) != pdPASS) {
        s_stream_task = NULL;
        ESP_LOGE(LOG_TAG, "camera task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(LOG_TAG, "stream task in PSRAM, stack=%d", CAM_STREAM_STACK);
    return ESP_OK;
}

esp_err_t bsp_camera_start(void)
{
    if (bsp_camera_early_init() != ESP_OK) {
        return ESP_FAIL;
    }
    return bsp_camera_stream_start();
}

bool bsp_camera_is_available(void)
{
    return s_available;
}
