#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_twai.h"
#include "esp_twai_mcp2515.h"

static const char *TAG = "CAN_RX";

/* ---------- WiFi配置---------- */
#define WIFI_SSID       "Your SSID"
#define WIFI_PASS       "Your password"

/* ---------- UDP目标配置 ---------- */
#define UDP_SERVER_IP   "192.168.45.251"
#define UDP_SERVER_PORT 8888

/*这里使用的时UDP传输，你需要有一个LAN内的终端来接收汽车的CAN数据帧，这里的IP地址仅供参考*/
/*如果你不知道怎么写一个接收端的代码，项目目录下有一个CAN-receiver.py的脚本，只需运行就会自动记录CAN数据到文件中*/
/*注意使用这个脚本必须提前安装python的cantools*/

/* ---------- MCP2515接线 ---------- */
#define MCP2515_SPI_HOST      SPI2_HOST
#define MCP2515_SCLK_GPIO     GPIO_NUM_12
#define MCP2515_MOSI_GPIO     GPIO_NUM_11
#define MCP2515_MISO_GPIO     GPIO_NUM_13
#define MCP2515_CS_GPIO       GPIO_NUM_10
#define MCP2515_INT_GPIO      GPIO_NUM_9

#define CAN_BITRATE            500000
#define MCP2515_OSC_HZ          8000000

/* ---------- 队列深度---------- */
#define CAN_QUEUE_LEN           32
#define UDP_QUEUE_LEN           32

/* 至少间隔200ms发一次，节流保护 */
#define MIN_SEND_INTERVAL_US  200000

/* ---------- WiFi事件同步 ---------- */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* ---------- UDP socket ---------- */
static int s_udp_sock = -1;
static struct sockaddr_in s_dest_addr;

/* ---------- 上次发送时间戳 ---------- */
static int64_t s_last_send_time = 0;

/* ---------- CAN帧的独立拷贝结构，避免共享缓冲区竞态 ---------- */
typedef struct {
    uint32_t can_id;
    uint32_t dlc;
    uint8_t  data[8];
    uint64_t timestamp_us;
} can_frame_msg_t;

static QueueHandle_t s_can_queue = NULL;
static twai_node_handle_t s_node = NULL;
static QueueHandle_t s_udp_queue = NULL;

/* ================= WiFi 事件处理 ================= */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi断开，尝试重连...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "获取到IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "正在连接WiFi: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi连接成功");
    } else {
        ESP_LOGE(TAG, "WiFi连接超时，请检查SSID/密码");
    }
}

/* ================= UDP 初始化与发送 ================= */
static void udp_init(void)
{
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "创建UDP socket失败");
        return;
    }

    /* 关键修复：给socket发送设置超时，避免缓冲区满时永久阻塞卡死整个任务 */
    struct timeval send_timeout;
    send_timeout.tv_sec = 0;
    send_timeout.tv_usec = 100 * 1000;  /* 100ms超时 */
    setsockopt(s_udp_sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_port = htons(UDP_SERVER_PORT);
    inet_pton(AF_INET, UDP_SERVER_IP, &s_dest_addr.sin_addr);

    ESP_LOGI(TAG, "UDP目标地址已设置: %s:%d", UDP_SERVER_IP, UDP_SERVER_PORT);
}

static void udp_send_can_frame(const can_frame_msg_t *frame)
{
    if (s_udp_sock < 0) {
        return;
    }

    char payload[192];
    int offset = snprintf(payload, sizeof(payload),
                           "{\"ts\":%llu,\"id\":\"0x%03lX\",\"dlc\":%lu,\"data\":\"",
                           (unsigned long long)frame->timestamp_us,
                           (unsigned long)frame->can_id,
                           (unsigned long)frame->dlc);

    for (uint32_t i = 0; i < frame->dlc && offset < (int)sizeof(payload) - 4; i++) {
        offset += snprintf(payload + offset, sizeof(payload) - offset, "%02X", frame->data[i]);
    }
    snprintf(payload + offset, sizeof(payload) - offset, "\"}");

    int err = sendto(s_udp_sock, payload, strlen(payload), 0,
                      (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
    if (err < 0) {
        /* 发送失败不阻塞、不重试，直接丢弃这一帧，保证任务继续运行 */
        ESP_LOGW(TAG, "UDP发送失败: errno %d", errno);
    }
}

/* ================= CAN 接收回调：只做拷贝入队，不做任何耗时操作 ================= */
static bool IRAM_ATTR on_rx_done_cb(twai_node_handle_t handle,
                                     const twai_rx_done_event_data_t *edata,
                                     void *user_ctx)
{
    (void)edata;
    (void)user_ctx;

    static uint8_t isr_rx_buf[TWAI_FRAME_MAX_LEN];
    twai_frame_t isr_frame = {0};
    isr_frame.buffer = isr_rx_buf;
    isr_frame.buffer_len = sizeof(isr_rx_buf);

    if (twai_node_receive_from_isr(handle, &isr_frame) == ESP_OK) {
        can_frame_msg_t msg;
        msg.can_id = isr_frame.header.id;
        msg.dlc = isr_frame.buffer_len;
        if (msg.dlc > 8) {
            msg.dlc = 8;
        }
        memcpy(msg.data, isr_rx_buf, msg.dlc);
        msg.timestamp_us = esp_timer_get_time();

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(s_can_queue, &msg, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    return false;
}

/* ================= 打印与发送任务================= */


/* 打印任务：只负责打印和入队，不碰网络 */
static void print_and_send_task(void *arg)
{
    can_frame_msg_t msg;
    while (1) {
        if (xQueueReceive(s_can_queue, &msg, portMAX_DELAY) == pdTRUE) {
            printf("ID=0x%03lX  DLC=%lu  DATA=[",
                   (unsigned long)msg.can_id,
                   (unsigned long)msg.dlc);
            for (uint32_t i = 0; i < msg.dlc; i++) {
                printf("%02X ", msg.data[i]);
            }
            printf("]\n");

            /* 非阻塞尝试放进UDP发送队列，满了就直接丢弃，绝不等待 */
            xQueueSend(s_udp_queue, &msg, 0);
        }
    }
}

/* 独立的UDP发送任务，就算网络卡死，也不会影响上面的打印任务 */
static void udp_send_task(void *arg)
{
    can_frame_msg_t msg;
    while (1) {
        if (xQueueReceive(s_udp_queue, &msg, portMAX_DELAY) == pdTRUE) {
            int64_t now = esp_timer_get_time();
            if (now - s_last_send_time >= MIN_SEND_INTERVAL_US) {
                udp_send_can_frame(&msg);
                s_last_send_time = now;
            }
            /* 否则直接丢弃这一帧，不占用发送资源 */
        }
    }
}

static void status_monitor_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));  /* 每秒查一次 */

        twai_node_status_t status;
        twai_node_record_t record;
        esp_err_t err = twai_node_get_info(s_node, &status, &record);

        if (err == ESP_OK) {
            const char *state_str = "UNKNOWN";
            switch (status.state) {
                case TWAI_ERROR_ACTIVE:   state_str = "ERROR_ACTIVE(正常)"; break;
                case TWAI_ERROR_WARNING:  state_str = "ERROR_WARNING(警告)"; break;
                case TWAI_ERROR_PASSIVE:  state_str = "ERROR_PASSIVE(被动错误)"; break;
                case TWAI_ERROR_BUS_OFF:  state_str = "BUS_OFF(总线关闭!!)"; break;
                default: break;
            }
            ESP_LOGI(TAG, "[状态监控] state=%s TEC=%u REC=%u 累计总线错误=%lu 队列剩余=%lu",
                     state_str, status.tx_error_count, status.rx_error_count,
                     (unsigned long)record.bus_err_num,
                     (unsigned long)status.tx_queue_remaining);
        } else {
            ESP_LOGE(TAG, "[状态监控] 查询失败: %s", esp_err_to_name(err));
        }
    }
}


void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    udp_init();

    s_can_queue = xQueueCreate(CAN_QUEUE_LEN, sizeof(can_frame_msg_t));
    if (s_can_queue == NULL) {
        ESP_LOGE(TAG, "CAN队列创建失败");
        return;
    }

    s_udp_queue = xQueueCreate(UDP_QUEUE_LEN, sizeof(can_frame_msg_t));
    if (s_udp_queue == NULL) {
        ESP_LOGE(TAG, "UDP队列创建失败");
        return;
    }
    

    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = MCP2515_SCLK_GPIO,
        .mosi_io_num = MCP2515_MOSI_GPIO,
        .miso_io_num = MCP2515_MISO_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(MCP2515_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    twai_mcp2515_node_config_t node_cfg = {0};
    node_cfg.io_cfg.int_gpio = MCP2515_INT_GPIO;
    node_cfg.io_cfg.cs_gpio  = MCP2515_CS_GPIO;
    node_cfg.spi_clock_hz    = 1 * 1000 * 1000;
    node_cfg.oscillator_hz   = MCP2515_OSC_HZ;
    node_cfg.bit_timing.bitrate    = CAN_BITRATE;
    node_cfg.bit_timing.sp_permill = 875;
    node_cfg.tx_queue_depth  = 4;
    node_cfg.fail_retry_cnt  = -1;
    node_cfg.timestamp_resolution_hz = 1000;

    ESP_ERROR_CHECK(twai_new_node_mcp2515(MCP2515_SPI_HOST, &node_cfg, &s_node));

    twai_event_callbacks_t cbs = {0};
    cbs.on_rx_done = on_rx_done_cb;
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(s_node, &cbs, NULL));

    
    xTaskCreate(print_and_send_task, "print_send_task", 4096, NULL, 4, NULL);
    xTaskCreate(udp_send_task, "udp_send_task", 2048, NULL, 3, NULL);

    ESP_ERROR_CHECK(twai_node_enable(s_node));

    xTaskCreate(status_monitor_task, "status_monitor_task", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "MCP2515 CAN节点已启动，波特率=%d，等待接收并转发...", CAN_BITRATE);
}
