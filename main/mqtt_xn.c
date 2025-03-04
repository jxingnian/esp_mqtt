// 包含必要的头文件
#include <stdio.h>          // 标准输入输出函数
#include <stdint.h>         // 标准整数类型定义
#include <stddef.h>         // 标准定义，如size_t
#include <string.h>         // 字符串操作函数
#include <ctype.h>          // 字符类型函数

// ESP-IDF 特定头文件
#include "esp_system.h"     // ESP32 系统函数
#include "esp_event.h"      // ESP32 事件循环库
#include "driver/gpio.h"    // GPIO 驱动
#include "hal/gpio_hal.h"   // GPIO HAL驱动

// 项目特定头文件
#include "esp_log.h"        // ESP32 日志功能
#include "mqtt_xn.h"        // 自定义MQTT功能
#include "mqtt_client.h"    // ESP32 MQTT客户端

// 定义GPIO引脚
#define POWER_SWITCH_GPIO   11

// 定义日志标签
static const char *TAG = "MQTT5_EXAMPLE";

// 开关状态
static bool switch_state = false;

// 初始化GPIO
static void init_gpio(void) {
    // 复位GPIO配置
    gpio_reset_pin(POWER_SWITCH_GPIO);
    
    // 设置GPIO方向为输出
    gpio_set_direction(POWER_SWITCH_GPIO, GPIO_MODE_OUTPUT);
    
    // 禁用中断
    gpio_set_intr_type(POWER_SWITCH_GPIO, GPIO_INTR_DISABLE);
    
    // 设置初始状态为关闭（高电平）
    gpio_set_level(POWER_SWITCH_GPIO, 1);
    
    ESP_LOGI(TAG, "GPIO %d initialized as output", POWER_SWITCH_GPIO);
}

// 控制开关
static void control_switch(bool state) {
    // 注意：这里我们反转逻辑，因为继电器通常是低电平触发
    gpio_set_level(POWER_SWITCH_GPIO, state ? 0 : 1);
    switch_state = state;
    ESP_LOGI(TAG, "Switch set to %s (GPIO level: %d)", state ? "ON" : "OFF", state ? 0 : 1);
}

// 发送开关状态
static void send_switch_state(esp_mqtt_client_handle_t client) {
    char status[32];
    snprintf(status, sizeof(status), "{\"state\": \"%s\"}", switch_state ? "ON" : "OFF");
    int msg_id = esp_mqtt_client_publish(client, "home/bedroom/switch1/status", status, 0, 0, 0);
    ESP_LOGI(TAG, "sent switch state: %s, msg_id=%d", status, msg_id);
}

// 如果错误代码非零，则记录错误信息
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

// 定义用户属性数组
static esp_mqtt5_user_property_item_t user_property_arr[] = {
        {"board", "esp32"},
        {"u", "user"},
        {"p", "password"}
    };

// 计算用户属性数组的大小
#define USE_PROPERTY_ARR_SIZE   sizeof(user_property_arr)/sizeof(esp_mqtt5_user_property_item_t)

// 定义发布属性配置
static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = 1000,
    .topic_alias = 0,
    .response_topic = "/topic/test/response",
    .correlation_data = "123456",
    .correlation_data_len = 6,
};

// 定义订阅属性配置
static esp_mqtt5_subscribe_property_config_t subscribe_property = {
    .subscribe_id = 25555,
    .no_local_flag = false,
    .retain_as_published_flag = false,
    .retain_handle = 0,
    .is_share_subscribe = true,
    .share_name = "group1",
};

// 定义另一个订阅属性配置
static esp_mqtt5_subscribe_property_config_t subscribe1_property = {
    .subscribe_id = 25555,
    .no_local_flag = true,
    .retain_as_published_flag = false,
    .retain_handle = 0,
};

// 定义取消订阅属性配置
static esp_mqtt5_unsubscribe_property_config_t unsubscribe_property = {
    .is_share_subscribe = true,
    .share_name = "group1",
};

// 定义断开连接属性配置
static esp_mqtt5_disconnect_property_config_t disconnect_property = {
    .session_expiry_interval = 60,
    .disconnect_reason = 0,
};

// 打印用户属性
static void print_user_property(mqtt5_user_property_handle_t user_property)
{
    if (user_property) {
        uint8_t count = esp_mqtt5_client_get_user_property_count(user_property);
        if (count) {
            esp_mqtt5_user_property_item_t *item = malloc(count * sizeof(esp_mqtt5_user_property_item_t));
            if (esp_mqtt5_client_get_user_property(user_property, item, &count) == ESP_OK) {
                for (int i = 0; i < count; i ++) {
                    esp_mqtt5_user_property_item_t *t = &item[i];
                    ESP_LOGI(TAG, "key is %s, value is %s", t->key, t->value);
                    free((char *)t->key);
                    free((char *)t->value);
                }
            }
            free(item);
        }
    }
}

/*
 * @brief MQTT事件处理函数
 *
 * 这个函数被MQTT客户端事件循环调用。
 *
 * @param handler_args 注册到事件的用户数据。
 * @param base 处理程序的事件基础（在这个例子中总是MQTT Base）。
 * @param event_id 接收到的事件的ID。
 * @param event_data 事件的数据，esp_mqtt_event_handle_t。
 */
static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    ESP_LOGD(TAG, "free heap size is %d, maxminu %d", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            
            // 发送 Home Assistant 配置消息
            const char *ha_config = "{"
                "\"unique_id\": \"HA-XINGNIAN-SWITCH-POWER-001\","
                "\"name\": \"POWER SWITCH\","
                "\"icon\": \"mdi:power\","
                "\"command_topic\": \"home/bedroom/switch1/commands\","
                "\"state_topic\": \"home/bedroom/switch1/status\","
                "\"json_attributes_topic\": \"home/bedroom/switch1/status\","
                "\"device\": {"
                    "\"identifiers\": \"HA-XINGNIAN-SWITCH-POWER-001\","
                    "\"manufacturer\": \"星年\","
                    "\"model\": \"HA\","
                    "\"name\": \"电源开关\","
                    "\"sw_version\": \"0.0.3\""
                "}"
            "}";
            
            msg_id = esp_mqtt_client_publish(client, "homeassistant/switch/HA-XINGNIAN-SWITCH-POWER-001/config", ha_config, 0, 0, 0);
            ESP_LOGI(TAG, "sent ha config message, msg_id=%d", msg_id);
            
            // 订阅相关主题
            msg_id = esp_mqtt_client_subscribe(client, "home/bedroom/switch1/commands", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
            
            // 发送初始状态
            send_switch_state(client);
            break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        print_user_property(event->property->user_property);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        print_user_property(event->property->user_property);
        esp_mqtt5_client_set_publish_property(client, &publish_property);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        print_user_property(event->property->user_property);
        esp_mqtt5_client_set_user_property(&disconnect_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
        esp_mqtt5_client_set_disconnect_property(client, &disconnect_property);
        esp_mqtt5_client_delete_user_property(disconnect_property.user_property);
        disconnect_property.user_property = NULL;
        esp_mqtt_client_disconnect(client);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        print_user_property(event->property->user_property);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        print_user_property(event->property->user_property);
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
        
        // 处理开关控制命令
        if (strncmp(event->topic, "home/bedroom/switch1/commands", event->topic_len) == 0) {
            // 创建一个临时缓冲区来存储命令数据
            char command[10] = {0};
            size_t len = event->data_len < sizeof(command) - 1 ? event->data_len : sizeof(command) - 1;
            strncpy(command, event->data, len);
            
            // 转换为大写以进行比较
            for(int i = 0; command[i]; i++) {
                command[i] = toupper(command[i]);
            }
            
            if (strcmp(command, "ON") == 0) {
                control_switch(true);
                ESP_LOGI(TAG, "Switch turned ON");
            } else if (strcmp(command, "OFF") == 0) {
                control_switch(false);
                ESP_LOGI(TAG, "Switch turned OFF");
            }
            // 发送状态更新
            send_switch_state(client);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        print_user_property(event->property->user_property);
        ESP_LOGI(TAG, "MQTT5 return code is %d", event->error_handle->connect_return_code);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

// MQTT5应用程序启动函数
void mqtt5_app_start(void)
{
    // 初始化GPIO
    init_gpio();
    
    // 定义连接属性配置
    esp_mqtt5_connection_property_config_t connect_property = {
        // 会话过期间隔，单位为秒
        .session_expiry_interval = 10,
        // 最大数据包大小，单位为字节
        .maximum_packet_size = 1024,
        // 接收最大值，表示客户端能够并行处理的最大 QoS 1 和 QoS 2 发布消息数
        .receive_maximum = 65535,
        // 主题别名最大值
        .topic_alias_maximum = 2,
        // 请求响应信息标志
        .request_resp_info = true,
        // 请求问题信息标志
        .request_problem_info = true,
        // 遗嘱消息延迟间隔，单位为秒
        .will_delay_interval = 10,
        // 有效载荷格式指示符
        .payload_format_indicator = true,
        // 消息过期间隔，单位为秒
        .message_expiry_interval = 10,
        // 响应主题
        .response_topic = "/test/response",
        // 关联数据
        .correlation_data = "123456",
        // 关联数据长度
        .correlation_data_len = 6,
    };

    // 定义MQTT5客户端配置
    esp_mqtt_client_config_t mqtt5_cfg = {
        // MQTT代理的URL地址
        .broker.address.uri = CONFIG_BROKER_URL,
        // 设置MQTT协议版本为5
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        // 禁用自动重连功能
        .network.disable_auto_reconnect = true,
        // 设置MQTT用户名
        .credentials.username = "xingnian",
        // 设置MQTT密码
        .credentials.authentication.password = "12345678",
        // 设置遗嘱消息的主题
        .session.last_will.topic = "/topic/will",
        // 设置遗嘱消息的内容
        .session.last_will.msg = "i will leave",
        // 设置遗嘱消息的长度
        .session.last_will.msg_len = 12,
        // 设置遗嘱消息的QoS级别为1
        .session.last_will.qos = 1,
        // 设置遗嘱消息为保留消息
        .session.last_will.retain = true,
    };

#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt5_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt5_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    // 初始化MQTT客户端
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt5_cfg);

    // 设置连接属性和用户属性
    esp_mqtt5_client_set_user_property(&connect_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_user_property(&connect_property.will_user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_connect_property(client, &connect_property);

    // 删除用户属性（释放内存）
    esp_mqtt5_client_delete_user_property(connect_property.user_property);
    esp_mqtt5_client_delete_user_property(connect_property.will_user_property);

    // 注册MQTT事件处理函数
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    // 启动MQTT客户端
    esp_mqtt_client_start(client);
}