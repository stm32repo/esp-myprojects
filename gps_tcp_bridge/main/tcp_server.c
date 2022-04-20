/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#define CONFIG_EXAMPLE_USE_CERT_BUNDLE

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_ota_ops.h"
#include "string.h"
#include "esp_http_client.h"
#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "nvs.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include <sys/socket.h>
#include "esp_https_ota.h"


#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/stream_buffer.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "esp_task_wdt.h"

#define EXAMPLE_ESP_WIFI_SSID "ZeleniySlonik"
#define EXAMPLE_ESP_WIFI_PASS "a1234-b6789"
#define HASH_LEN 32
#define TCP_BUFFER_SIZE 128

#define PORT                        CONFIG_EXAMPLE_PORT
#define KEEPALIVE_IDLE              CONFIG_EXAMPLE_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_EXAMPLE_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_EXAMPLE_KEEPALIVE_COUNT
#define PORT_CLI 8899

#define ECHO_UART_PORT_NUM 2
#define ECHO_UART_BAUD_RATE 115200
#define ECHO_TASK_STACK_SIZE 4096
#define UART_BUF_SIZE 256
#define ECHO_TEST_TXD 17
#define ECHO_TEST_RXD 16
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)
#define STREAM_BUFFER_SUZE 1024

#define GPIO_OUTPUT_IO_0 4
#define GPIO_OUTPUT_IO_1 4
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUTPUT_IO_0) | (1ULL<<GPIO_OUTPUT_IO_1))

SemaphoreHandle_t print_mux = NULL;
StreamBufferHandle_t stream_buffer_ex = NULL;
StreamBufferHandle_t stream_buffer_rx = NULL;

const char *TAG = "gps_tcp_bridge";
uint8_t uart_data[UART_BUF_SIZE];
uint8_t rx_buffer[TCP_BUFFER_SIZE];
uint8_t tx_buffer[TCP_BUFFER_SIZE];
uint8_t rx_buffer_cli[TCP_BUFFER_SIZE];

static int s_retry_num = 0;
/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

struct bridge_status {
	uint32_t count_uart_rx;
	uint32_t count_tcp_tx;
} status;

//static const char *bind_interface_name = "sta";
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
const uint8_t gps_uart_command1[] = {0xb5, 0x62, 0x09, 0x01, 0x10, 0x00, 0xc8, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x69, 0x21, 0x00, 0x00, 0x00, 0x02, 0x10, 0x2b, 0x22};
const uint8_t gps_uart_command2[] = {0xb5, 0x62, 0x09, 0x01, 0x10, 0x00, 0x0c, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x83, 0x69, 0x21, 0x00, 0x00, 0x00, 0x02, 0x11, 0x5f, 0xf0};
const uint8_t gps_uart_command3[] = {0xb5, 0x62, 0x06, 0x01, 0x08, 0x00, 0x03, 0x0a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x1d, 0x06};
const uint8_t gps_uart_command4[] = {0xb5, 0x62, 0x06, 0x01, 0x08, 0x00, 0x03, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x15, 0xce};
const uint8_t gps_uart_command5[] = {0xb5, 0x62, 0x06, 0x01, 0x08, 0x00, 0x01, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x31, 0x90};
const uint8_t gps_uart_command6[] = {0xb5, 0x62, 0x06, 0x01, 0x08, 0x00, 0x01, 0x22, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x33, 0x9e};
#define OTA_URL_SIZE 256
#define CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL "https://192.168.10.4:8443/tcp_server_v1.13.bin"

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

void simple_ota_example_task(void)
{
    ESP_LOGI(TAG, "Starting OTA example");
    esp_http_client_config_t config = {
        .url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .cert_pem = (char *)server_cert_pem_start,
#endif /* CONFIG_EXAMPLE_USE_CERT_BUNDLE */
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
    };

#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
    config.skip_cert_common_name_check = true;
#endif

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    ESP_LOGI(TAG, "Attempting to download update from %s", config.url);
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
    }
}

static void do_retransmit(const int sock)
{
    int len;

    do {
	vTaskDelay(2 / portTICK_PERIOD_MS);
        len = recv(sock, rx_buffer, TCP_BUFFER_SIZE, MSG_DONTWAIT);
	if (len < 0) {
	   if (errno != EAGAIN) {
		ESP_LOGW(TAG, "Connection errno: %d, closed", errno);
		return;
	   }
	}
	if (len > 0) uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) rx_buffer, len);
//xStreamBufferSend (stream_buffer_rx, (void *) data, len, 3 / portTICK_PERIOD_MS);
	uint32_t length  = xStreamBufferReceive (stream_buffer_ex, (void *) tx_buffer, TCP_BUFFER_SIZE, 2 / portTICK_PERIOD_MS);
	if(length>0) {
            int written = send(sock, tx_buffer, length, 0);
	    if(written>0) status.count_tcp_tx += written;
	    ESP_LOGI(TAG, "Send tcp len: %d", written);
            if (written < 0) {
                ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
		len=0;
            }
        } 
    } while (1);
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;
    //xSemaphoreTake(print_mux, portMAX_DELAY);

        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    vTaskDelay(500 / portTICK_PERIOD_MS);
    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {

        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // Convert ip address to string
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        do_retransmit(sock);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

static void do_cli_retransmit(const int sock)
{
    int len;

    do {
        vTaskDelay(20 / portTICK_PERIOD_MS);
        len = recv(sock, rx_buffer_cli, TCP_BUFFER_SIZE, 0);
        if (len <= 0) {
           if (errno != EAGAIN) {
                ESP_LOGW(TAG, "Connection errno: %d, closed", errno);
                return;
           }
        }
        if (len > 0) {
            if((memcmp(rx_buffer_cli, "reboot", 6)) == 0) {
		send(sock, "reboot", 6, 0);
		vTaskDelay(500 / portTICK_PERIOD_MS);
		esp_restart();
	    } 
	    if((memcmp(rx_buffer_cli, "free", 4)) == 0) {
		len = snprintf((char*)rx_buffer_cli, TCP_BUFFER_SIZE-1, "Free heap memory: %d\r\n", xPortGetFreeHeapSize());
		if (len>0) send(sock, rx_buffer_cli, len, 0);
		continue;
	    }
            if((memcmp(rx_buffer_cli, "status", 4)) == 0) {
                len = snprintf((char*)rx_buffer_cli, TCP_BUFFER_SIZE-1, "UART rx: %d\r\nTCP tx: %d\r\n", status.count_uart_rx, status.count_tcp_tx);
                if (len>0) send(sock, rx_buffer_cli, len, 0);
                continue;
            }
	    send(sock, "Incorrect command\r\n", 19, 0);
	}
    } while (1);
}

static void cli_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;
    //xSemaphoreTake(print_mux, portMAX_DELAY);

        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT_CLI);
        ip_protocol = IPPROTO_IP;

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP_CLI;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT_CLI);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP_CLI;
    }

    while (1) {

        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // Convert ip address to string
        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        do_cli_retransmit(sock);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP_CLI:
    close(listen_sock);
    vTaskDelete(NULL);
}

static void echo_task(void *arg)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = ECHO_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));
    
    uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) gps_uart_command1, sizeof(gps_uart_command1));
    vTaskDelay(1 / portTICK_PERIOD_MS);
    uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) gps_uart_command2, sizeof(gps_uart_command2));
    vTaskDelay(1 / portTICK_PERIOD_MS);
    uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) gps_uart_command3, sizeof(gps_uart_command3));
    vTaskDelay(1 / portTICK_PERIOD_MS);
    uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) gps_uart_command4, sizeof(gps_uart_command4));
    vTaskDelay(1 / portTICK_PERIOD_MS);
    uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) gps_uart_command5, sizeof(gps_uart_command5));
    vTaskDelay(1 / portTICK_PERIOD_MS);
    uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) gps_uart_command6, sizeof(gps_uart_command6));
    vTaskDelay(1 / portTICK_PERIOD_MS);
    //xSemaphoreGive(print_mux);
    // Configure a temporary buffer for the incoming data
    // uint8_t *data = (uint8_t *) malloc(UART_BUF_SIZE);

    while (1) {
        // Read data from the UART
	// xSemaphoreTake(print_mux, portMAX_DELAY);
        int len = uart_read_bytes(ECHO_UART_PORT_NUM, uart_data, UART_BUF_SIZE, 10 / portTICK_PERIOD_MS);
	//ESP_LOGI(TAG, "Recv uart len: %d", len);
        // Write data back to the UART
        // uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) data, len);
        if (len>0) {
	    status.count_uart_rx += len;
	    xStreamBufferSend (stream_buffer_ex, (void *) uart_data, len, 2 / portTICK_PERIOD_MS);
//            data[len] = '\0';
//            ESP_LOGI(TAG, "Recv str: %s", (char *) data);
        }
	//xSemaphoreGive(print_mux);
	vTaskDelay(2 / portTICK_PERIOD_MS);
    }
}

static void gpio_task(void *arg)
{
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    while(1) {
        gpio_set_level(GPIO_OUTPUT_IO_0, 1);
	vTaskDelay(500 / portTICK_PERIOD_MS);
	gpio_set_level(GPIO_OUTPUT_IO_0, 0);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG, "Event: %s, id: %d", event_base, event_id);
    if        (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 1.OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // 2.NVS partition contains data in new format and cannot be recognized by this version of code.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect()); */

    wifi_init_sta();
    simple_ota_example_task();   

    //print_mux = xSemaphoreCreateMutex();
    //xSemaphoreTake(print_mux, portMAX_DELAY);
    if (stream_buffer_ex == NULL) stream_buffer_ex = xStreamBufferCreate( STREAM_BUFFER_SUZE, 1);
    if (stream_buffer_rx == NULL) stream_buffer_rx = xStreamBufferCreate( STREAM_BUFFER_SUZE, 1);
    //data = (uint8_t *) malloc(BUF_SIZE);
    status.count_uart_rx = 0;
    status.count_tcp_tx = 0;

    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET, 5, NULL);
    xTaskCreate(cli_server_task, "cli_server", 4096, (void*)AF_INET, 6, NULL);
    xTaskCreate(echo_task, "uart_echo_task", ECHO_TASK_STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(gpio_task, "gpio_task", 4096, NULL, 10, NULL);
}
