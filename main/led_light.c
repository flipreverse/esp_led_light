#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"
#include "cJSON.h"
#include "esp_sleep.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pwm.h"

#define LED_SWITCH_SET_TOPIC "homeassistant/custom/led_light/set"
#define LED_SWITCH_STATE_TOPIC "homeassistant/custom/led_light/state"
#if defined(CONFIG_ENABLE_DEBUG) || defined(CONFIG_USE_SLEEP_MODE)
#define LED_PIN GPIO_NUM_1
#define GPIO_PIN_SEL ((1 << LED_PIN))
#endif
// 1kHz of PWM frequency
#define PWM_PERIOD (1000)
#define PWM_CHANNELS (1)
#define TRANSISTOR_PWM_PIN GPIO_NUM_2

static const char *TAG = "LED_LIGHT";

#ifdef CONFIG_USE_SLEEP_MODE
static EventGroupHandle_t mqttEventGroup;
#endif
static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;
static int brightnessValue = 255;
static int stateValue = 0;

static void setLED(void) {
	double dutyCycle = 0.0;
	if (stateValue) {
#if defined(CONFIG_ENABLE_DEBUG) && !defined(CONFIG_USE_SLEEP_MODE)
		gpio_set_level(LED_PIN, 0);
#endif
		dutyCycle = (double)brightnessValue / 255.0 * ((double)PWM_PERIOD);
		ESP_LOGI(TAG, "Turning on!");
		ESP_LOGI(TAG, "Setting duty cycle to %d (%.2f)", (uint32_t)dutyCycle, dutyCycle);
		pwm_set_duty(0, (uint32_t)dutyCycle);
		pwm_start();
	} else {
#if defined(CONFIG_ENABLE_DEBUG) && !defined(CONFIG_USE_SLEEP_MODE)
		gpio_set_level(LED_PIN, 1);
#endif
		pwm_stop(0);
		ESP_LOGI(TAG, "Turning off!");
	}
}

static void sendState(esp_mqtt_client_handle_t client) {
	cJSON *status = cJSON_CreateObject();
	if (!status) {
		ESP_LOGE(TAG, "Cannot create status JSON structure");
		return;
	}
	if (stateValue) {
		if (!cJSON_AddStringToObject(status, "state", "ON")) {
			ESP_LOGE(TAG, "Cannot add state value to JSON structure");
			cJSON_Delete(status);
			return;
		}
	} else {
		if (!cJSON_AddStringToObject(status, "state", "OFF")) {
			ESP_LOGE(TAG, "Cannot add state value to JSON structure");
			cJSON_Delete(status);
			return;
		}
	}
	if (!cJSON_AddNumberToObject(status, "brightness", brightnessValue)) {
		ESP_LOGE(TAG, "Cannot add brightness value to JSON structure");
		cJSON_Delete(status);
		return;
	}
	char *statusString = cJSON_Print(status);
	if (esp_mqtt_client_publish(client, LED_SWITCH_STATE_TOPIC, statusString, 0, 0, 0) == -1) {
		ESP_LOGE(TAG, "Cannot publish state msg");
	}
	cJSON_Delete(status);
}

static void handleCmd(esp_mqtt_client_handle_t client, esp_mqtt_event_handle_t event) {
	cJSON *json = cJSON_Parse(event->data);
	if (json == NULL) {
		const char *error_ptr = cJSON_GetErrorPtr();
		if (!error_ptr) {
			ESP_LOGI(TAG, "Error parsing data: %s\n", error_ptr);
		}
		return;
	}
	if (!cJSON_HasObjectItem(json, "state")) {
		ESP_LOGE(TAG, "No such element: state\n");
		cJSON_Delete(json);
		return;
	}
	cJSON *state = cJSON_GetObjectItem(json, "state");
	if (!cJSON_IsString(state) || state->valuestring == NULL)  {
		ESP_LOGE(TAG, "Invalid state string\n");
		cJSON_Delete(json); 
		return;
	}
	if (strcmp(state->valuestring, "ON") == 0) {
		stateValue = 1;
	} else if (strcmp(state->valuestring, "OFF") == 0){
		stateValue = 0;
	} else {
		ESP_LOGE(TAG, "Error - Malformed state value: %s\n", state->valuestring);
		sendState(client);
		return;
	}

	cJSON *brightness = cJSON_GetObjectItem(json, "brightness");
	if (brightness == NULL) {
		ESP_LOGD(TAG, "No brightness value. Ignoring.");
		cJSON_Delete(json);
		setLED();
		sendState(client);
		return;
	}
	if (!cJSON_IsNumber(brightness))  {
		ESP_LOGE(TAG, "Invalid brightness value.");
		cJSON_Delete(json);
		sendState(client);
		return;
	}
	brightnessValue = (int)brightness->valuedouble;
	ESP_LOGI(TAG, "New brightness: %d", brightnessValue);
	cJSON_Delete(json);
	sendState(client);
	setLED();
}

static esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event)
{
	esp_mqtt_client_handle_t client = event->client;

	// your_context_t *context = event->context;
	switch (event->event_id) {
		case MQTT_EVENT_CONNECTED:
		{
			if (esp_mqtt_client_subscribe(client, LED_SWITCH_SET_TOPIC, 0) == -1) {
				ESP_LOGE(TAG, "Cannot subscribe to topic '%s'", LED_SWITCH_SET_TOPIC);
				break;
			}
			ESP_LOGI(TAG, "Connected broker '%s' as user '%s'", CONFIG_BROKER_URL, CONFIG_BROKER_USER);
#ifdef CONFIG_USE_SLEEP_MODE
			xEventGroupSetBits(mqttEventGroup, CONNECTED_BIT);
#endif
			break;
		}

		case MQTT_EVENT_DISCONNECTED:
			ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
			break;

		case MQTT_EVENT_SUBSCRIBED:
		{
			ESP_LOGI(TAG, "Subscribed to '%s'", LED_SWITCH_SET_TOPIC);
			sendState(client);
			break;
		}

		case MQTT_EVENT_UNSUBSCRIBED:
			ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED");
			break;

		case MQTT_EVENT_PUBLISHED:
			ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED");
			break;

		case MQTT_EVENT_DATA:
			handleCmd(client, event);
			break;

		case MQTT_EVENT_ERROR:
			ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
			break;
	}
	return ESP_OK;
}

static esp_err_t wifiEventHandler(void *ctx, system_event_t *event)
{
	/* For accessing reason codes in case of disconnection */
	system_event_info_t *info = &event->event_info;
	
	switch (event->event_id) {
		case SYSTEM_EVENT_STA_START:
			esp_wifi_connect();
			break;

		case SYSTEM_EVENT_STA_GOT_IP:
			xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
			break;

		case SYSTEM_EVENT_STA_DISCONNECTED:
			ESP_LOGE(TAG, "Disconnect reason : %d", info->disconnected.reason);
			if (info->disconnected.reason == WIFI_REASON_BASIC_RATE_NOT_SUPPORT) {
				/*Switch to 802.11 bgn mode */
				esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCAL_11B | WIFI_PROTOCAL_11G | WIFI_PROTOCAL_11N);
			}
			esp_wifi_connect();
			xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
			break;

		default:
			break;
	}
	return ESP_OK;
}

static void setupDevice(void) {
#if defined(CONFIG_ENABLE_DEBUG) || defined(CONFIG_USE_SLEEP_MODE)
	gpio_config_t io_conf;
#endif
	int16_t pwmPhase[PWM_CHANNELS] = { 0 };
	uint32_t pwmDuties[PWM_CHANNELS] = { 0 };
	uint32_t pwmPins[PWM_CHANNELS] = { TRANSISTOR_PWM_PIN };
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = CONFIG_WIFI_SSID,
			.password = CONFIG_WIFI_PASSWORD,
		},
	};
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
#ifdef CONFIG_USE_STATIC_IP
	tcpip_adapter_ip_info_t ip_config;
	ip4addr_aton(CONFIG_IP_ADDRESS, &ip_config.ip);
	ip4addr_aton(CONFIG_NETMASK, &ip_config.netmask);
	ip4addr_aton(CONFIG_GATEWAY, &ip_config.gw);
#endif

	// Initialize nvs flash
	nvs_flash_init();
#if defined(CONFIG_ENABLE_DEBUG) || defined(CONFIG_USE_SLEEP_MODE)
	// Set up LED
	//disable interrupt
	io_conf.intr_type = GPIO_INTR_DISABLE;
	//set as output mode
	io_conf.mode = GPIO_MODE_OUTPUT;
	//bit mask of the pins that you want to set
	io_conf.pin_bit_mask = GPIO_PIN_SEL;
	//disable pull-down mode
	io_conf.pull_down_en = 0;
	//disable pull-up mode
	io_conf.pull_up_en = 0;
	//configure GPIO with the given settings
	if (gpio_config(&io_conf) != ESP_OK) {
		ESP_LOGE(TAG, "Cannot configure LED pins");
	}
#endif
	// Initialize PWM controller
	if (pwm_init(PWM_PERIOD, pwmDuties, PWM_CHANNELS, pwmPins) != ESP_OK) {
		ESP_LOGE(TAG, "Cannot init PWM");
	}
	if (pwm_set_phases(pwmPhase) != ESP_OK) {
		ESP_LOGE(TAG, "Cannot set PWM phases");
	}
	// Set initial LED state
	setLED();

	// Init WiFi
	tcpip_adapter_init();
#ifdef CONFIG_USE_STATIC_IP
	tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
	tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &ip_config);
#endif
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_event_loop_init(wifiEventHandler, NULL));
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	ESP_LOGD(TAG, "start the WIFI SSID:[%s]", CONFIG_WIFI_SSID);
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_LOGD(TAG, "Waiting for wifi");
	xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
	ESP_LOGI(TAG, "Connected to SSID:[%s]", CONFIG_WIFI_SSID);
}

void app_main()
{
	esp_mqtt_client_config_t mqtt_cfg = {
		.uri = CONFIG_BROKER_URL,
		.event_handle = mqttEventHandler,
		.username = CONFIG_BROKER_USER,
		.password = CONFIG_BROKER_PASSWORD,
		// .user_context = (void *)your_context
	};
	esp_mqtt_client_handle_t client;

	esp_log_level_set("*", ESP_LOG_WARN);
#ifdef CONFIG_ENABLE_DEBUG
	esp_log_level_set(TAG, ESP_LOG_VERBOSE);
	esp_log_level_set("tcpip_adapter", ESP_LOG_VERBOSE);
#else
	esp_log_level_set(TAG, ESP_LOG_WARN);
#endif

	ESP_LOGD(TAG, "Startup ...");
	ESP_LOGD(TAG, "Compiled with IDF version: %s", esp_get_idf_version());

#if 0
	esp_log_level_set("MQTT_CLIENT", ESP_LOG_ERROR);
	esp_log_level_set("TRANSPORT_TCP", ESP_LOG_ERROR);
	esp_log_level_set("TRANSPORT_SSL", ESP_LOG_ERROR);
	esp_log_level_set("TRANSPORT", ESP_LOG_ERROR);
	esp_log_level_set("OUTBOX", ESP_LOG_ERROR);
#endif

	// Initialize peripherals
	setupDevice();
	// Start up MQTT client
#ifdef CONFIG_USE_SLEEP_MODE
	gpio_set_level(LED_PIN, 0);
	mqttEventGroup = xEventGroupCreate();
#endif
	client = esp_mqtt_client_init(&mqtt_cfg);
	esp_mqtt_client_start(client);
#ifdef CONFIG_USE_SLEEP_MODE
	// Wait until the MQTT client has connected successfully
	xEventGroupWaitBits(mqttEventGroup, CONNECTED_BIT, false, true, portMAX_DELAY);
	// Sleep for 1 second in order to receive any MQTT messages
	ESP_LOGD(TAG, "Waiting a bit...");
	vTaskDelay(1000 / portTICK_PERIOD_MS);
	ESP_LOGD(TAG, "Entering deep sleep");
	esp_mqtt_client_destroy(client);
	esp_wifi_stop();
	// Enter chip's deep sleep
	esp_deep_sleep_set_rf_option(0);
	gpio_set_level(LED_PIN, 1);
	esp_deep_sleep(CONFIG_WAKEUP_INTERVAL * 1000);
#endif
}
