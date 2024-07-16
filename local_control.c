/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
		 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <dirent.h>
#include <inttypes.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
//#include "protocol_examples_common.h"

#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
#include "esp_mac.h"
#endif

#include "esp_bridge.h"
#include "esp_mesh_lite.h"
#include "mqtt_client.h"
//#include "esp_netif_sntp.h"
#include "lwip/ip_addr.h"
#include "esp_sntp.h"
#include "soc/soc_caps.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "deep_sleep_example.h"
#include "esp_littlefs.h"

#define PAYLOAD_LEN       (1456) /**< Max payload size(in bytes) */
#define MQTT_HOSTNAME     "10.42.0.1"
#define MQTT_PORT         1883

//#define RING_BUFFER_SIZE 4
#define USAGE_BUFFER_SIZE 3
#define VOLTAGE_READING_INTERVAL_MS (1*1000) // 1 seconds in milliseconds
#define ESP_EXT0_WAKEUP_LEVEL_HIGH 1
#define ESP_LITTLEFS_BASEPATH "/file_storage"
#define ESP_LITTLEFS_PARTITION "storage"

uint8_t sta_mac[6] = {0};
char mac[18] = {0};
char MQTT_TOPIC[64] = "/topic/data/";
esp_mqtt_topic_t MQTT_SUBSCRIBED_TOPIC_LIST[2] = {{.filter = "/topic/command/", .qos = 2}, 
	{.filter = "/topic/request/", .qos = 2}};
int NUM_TOPICS = sizeof(MQTT_SUBSCRIBED_TOPIC_LIST)/sizeof(esp_mqtt_topic_t);
time_t now;
char timestamp[20];
struct tm timeinfo;
const int ext_wakeup_pin_0 = 8;
static int file_count = 0;
bool mqtt_flag = false;
bool mqtt_connection_handler_flag = false;

static esp_mqtt_client_handle_t client = NULL;
static int msg_id = 0;
RTC_DATA_ATTR ring_buffer_t buffer; //persist ring buffer on RTC memory
static float voltage = 230;
char readings_buffer[30];
extern RTC_DATA_ATTR float flushed_data[USAGE_BUFFER_SIZE]; //persist meter readings on RTC memory

static int g_sockfd    = -1;
static const char *TAG = "local_control";

/**
 * @brief Create a tcp client
 */
static int socket_tcp_client_create(const char *ip, uint16_t port)
{
    ESP_LOGI(TAG, "Create a tcp client, ip: %s, port: %d", ip, port);

    esp_err_t ret = ESP_OK;
    int sockfd    = -1;
    struct ifreq iface;
    memset(&iface, 0x0, sizeof(iface));
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip),
    };

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "socket create, sockfd: %d", sockfd);
        goto ERR_EXIT;
    } 

    esp_netif_get_netif_impl_name(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), iface.ifr_name);
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE,  &iface, sizeof(struct ifreq)) != 0) {
        ESP_LOGI(TAG, "Bind [sock=%d] to interface %s fail", sockfd, iface.ifr_name);
    }

    ret = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        ESP_LOGE(TAG, "socket connect, ret: %d, ip: %s, port: %d",
                   ret, ip, port);
        goto ERR_EXIT;
    }
    return sockfd;

ERR_EXIT:

    if (sockfd != -1) {
        close(sockfd);
    }

    return -1;
}

void tcp_client_write_task(void *arg)
{
    size_t size        = 0;
    int count          = 0;
    char *data         = NULL;
    esp_err_t ret      = ESP_OK;
    uint8_t sta_mac[6] = {0};

    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);

    ESP_LOGI(TAG, "TCP client write task is running");

    while (1) {
        if (g_sockfd == -1) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        vTaskDelay(3000 / portTICK_PERIOD_MS);

        size = asprintf(&data, "{\"src_addr\": \"" MACSTR "\",\"data\": \"Hello TCP Server!\",\"count\": %d}",
                        MAC2STR(sta_mac), count++);

        ESP_LOGD(TAG, "TCP write, size: %d, data: %s", size, data);
        ret = write(g_sockfd, data, size);
        free(data);

        if (ret <= 0) {
            ESP_LOGE(TAG, "<%s> TCP write", strerror(errno));
            continue;
        }
    }

    ESP_LOGI(TAG, "TCP client write task is exit");

    close(g_sockfd);
    g_sockfd = -1;
    free(data);
    vTaskDelete(NULL);
}

/**
 * @brief Timed printing system information
 */
static void print_system_info_timercb(TimerHandle_t timer)
{
    uint8_t primary                 = 0;
    uint8_t sta_mac[6]              = {0};
    wifi_ap_record_t ap_info        = {0};
    wifi_second_chan_t second       = 0;
    wifi_sta_list_t wifi_sta_list   = {0x0};

    esp_wifi_sta_get_ap_info(&ap_info);
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    esp_wifi_ap_get_sta_list(&wifi_sta_list);
    esp_wifi_get_channel(&primary, &second);

    ESP_LOGI(TAG, "System information, channel: %d, layer: %d, self mac: " MACSTR ", parent bssid: " MACSTR
             ", parent rssi: %d, free heap: %"PRIu32"", primary,
             esp_mesh_lite_get_level(), MAC2STR(sta_mac), MAC2STR(ap_info.bssid),
             (ap_info.rssi != 0 ? ap_info.rssi : -120), esp_get_free_heap_size());

    for (int i = 0; i < wifi_sta_list.num; i++) {
        ESP_LOGI(TAG, "Child mac: " MACSTR, MAC2STR(wifi_sta_list.sta[i].mac));
    }
}

static void ip_event_sta_got_ip_handler(void *arg, esp_event_base_t event_base,
                                        int32_t event_id, void *event_data)
{
    if (g_sockfd != -1) {
        close(g_sockfd);
        g_sockfd = -1;
    }

    g_sockfd = socket_tcp_client_create(CONFIG_SERVER_IP, CONFIG_SERVER_PORT);

    static bool tcp_task = false;

    if (!tcp_task) {
        xTaskCreate(tcp_client_write_task, "tcp_client_write_task", 4 * 1024,
                    NULL, 5, NULL);
        tcp_task = true;
    }
}

static esp_err_t esp_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

static esp_err_t wifi_init(void)
{
    // Station
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ROUTER_SSID,
            .password = CONFIG_ROUTER_PASSWORD,
        },
    };
    esp_bridge_wifi_set(WIFI_MODE_STA, (char *)wifi_config.sta.ssid, (char *)wifi_config.sta.password, NULL);

    // Softap
    memset(&wifi_config, 0x0, sizeof(wifi_config_t));
    size_t softap_ssid_len = sizeof(wifi_config.ap.ssid);
    if (esp_mesh_lite_get_softap_ssid_from_nvs((char *)wifi_config.ap.ssid, &softap_ssid_len) != ESP_OK) {
        snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s", CONFIG_BRIDGE_SOFTAP_SSID);
    }
    size_t softap_psw_len = sizeof(wifi_config.ap.password);
    if (esp_mesh_lite_get_softap_psw_from_nvs((char *)wifi_config.ap.password, &softap_psw_len) != ESP_OK) {
        strlcpy((char *)wifi_config.ap.password, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(wifi_config.ap.password));
    }
    esp_bridge_wifi_set(WIFI_MODE_AP, (char *)wifi_config.ap.ssid, (char *)wifi_config.ap.password, NULL);

    return ESP_OK;
}

int get_file_size(const char *filename)
{
	FILE *file = fopen(filename, "rb");
	if(file == NULL){
		ESP_LOGE(TAG,"Could not open file %s for reading", filename);
		return -1;
	}
	fseek(file,0, SEEK_END);
	int size = ftell(file);
	fclose(file);
	printf("File size: %d\n",size);
	return size;
}

int esp_littlefs_num_files(const char *path) 
{
	DIR *dir = opendir(path);
	if (dir == NULL){
		ESP_LOGE(TAG, "Could not open directory %s", path);
		return -1;
	}
	struct dirent * entry = NULL;
	while((entry ==readdir(dir)) != NULL){
		if (entry->d_type == DT_REG) { // Check if it is a regular file
			printf("File_count: %d", file_count);
			file_count++;
						}
		}
		closedir(dir);
		printf("Number of files: %d", file_count);
		return file_count;
	}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    //int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
	for (int i = 0; i < NUM_TOPICS ; i++){
    	    printf("Number of topics: %d\n Topic: %s\n", NUM_TOPICS, MQTT_SUBSCRIBED_TOPIC_LIST[i].filter);
	   // snprintf(MQTT_SUBSCRIBED_TOPIC_LIST[i].filter,"%s", mac);
	   char *unique_topic = malloc(strlen(MQTT_SUBSCRIBED_TOPIC_LIST[i].filter) + strlen(mac) + 1);
	   sprintf(unique_topic,"%s%s",MQTT_SUBSCRIBED_TOPIC_LIST[i].filter, mac);
	   MQTT_SUBSCRIBED_TOPIC_LIST[i].filter = unique_topic;
	   printf("%s", MQTT_SUBSCRIBED_TOPIC_LIST[i].filter);

    }

	msg_id = esp_mqtt_client_subscribe_multiple(client, MQTT_SUBSCRIBED_TOPIC_LIST, 2);
	mqtt_connection_handler_flag = false;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
	mqtt_connection_handler_flag = true;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
	ESP_LOGI(TAG, "Payload: %s", MQTT_PAYLOAD);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
	//TODO: Tweak this to parse receive messages;
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_config_init(void)
{
    //Configure MQTT
    esp_mqtt_client_config_t mqtt_cfg = {
            .broker={
            	.address = {
                .hostname = MQTT_HOSTNAME,
                .port = MQTT_PORT,
                .transport = MQTT_TRANSPORT_OVER_TCP,
            		},
        	},
	    .session={
            	.disable_clean_session = true,
            	.message_retransmit_timeout = 3000,
       		},
            .network={
            	.timeout_ms = 864000000,
        	},
            .credentials={
            	.client_id = mac,
            	},
	    .outbox={
		.limit = 229376,
	    	},

	    
    	};
    
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    ESP_LOGI(TAG,"MQTT Client started"); 
}

static void voltage_task(void *pvParameters)
{
//Configure LittleFS filesystem
    esp_vfs_littlefs_conf_t conf ={
	    .base_path = ESP_LITTLEFS_BASEPATH,
	    .partition_label = ESP_LITTLEFS_PARTITION,
	    .format_if_mount_failed = true,
	    .dont_mount = false,
    };
  
    //Mount LittleFS filesystem using the above settings.
    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    //Check if mounting successful

    if (ret != ESP_OK) {
	    if (ret == ESP_FAIL) {
		    ESP_LOGE(TAG, "Failed to mount or format filesystem");
	    }else if (ret == ESP_ERR_NOT_FOUND) {
		    ESP_LOGE(TAG, "Failed to find LittleFS partition");
	    }else {
		    ESP_LOGE(TAG,"Failed to initialize LittelFS (%s)", esp_err_to_name(ret));
	    }
    }

    
    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK){
	 ESP_LOGE(TAG,"Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    	 esp_littlefs_format(conf.partition_label);
    }else{
	ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

   //Configure MQTT 
    mqtt_config_init();	 
     
        
    //msg_id = esp_mqtt_client_subscribe_multiple(client, MQTT_SUBSCRIBED_TOPIC_LIST, 2);      
   
    while (1){
    //Monitor the EXT0 wakeup pin and sleep when triggering condition is met
    printf("Current GPIO%d state: %d\n", ext_wakeup_pin_0, gpio_get_level(ext_wakeup_pin_0));
    if (gpio_get_level(ext_wakeup_pin_0) == 0){
    	printf("Entering deep sleep\n");
     	esp_deep_sleep_start();
    }	
    printf("Enabling EXT0 wakeup on pin GPIO%d\n", ext_wakeup_pin_0);

	float instant_voltage = (float)(rand() % (231 -210) + 210);
	voltage = (instant_voltage < voltage)?instant_voltage:voltage;
	
	if (mqtt_flag){
		char MQTT_PAYLOAD[100];	

        	//Prepare payload
        	time(&now);
        	setenv("TZ", "GMT-2", 1);
        	tzset();
        	localtime_r(&now, &timeinfo);
        	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
        	snprintf(MQTT_PAYLOAD, sizeof(MQTT_PAYLOAD), "%s, %s, %s", mac, timestamp,readings_buffer);
        	//Publish MQTT payload
		if (client !=NULL){
			if(mqtt_connection_handler_flag == true){
				ESP_LOGI(TAG, "MQTT disconnected");
				msg_id = esp_mqtt_client_enqueue(client, MQTT_TOPIC,MQTT_PAYLOAD, 0, 2, 0, 0);
				ESP_LOGI(TAG, "MQTT payload queued in outbox, msg_id=%d, payload: %s", msg_id, MQTT_PAYLOAD);
				
				//Write to file
				char filename[128];
				sprintf(filename, "/file_storage/%s_%s_%d.txt", mac, timestamp, msg_id);
				FILE *f = fopen(filename,"w");
				if (f == NULL){
   					ESP_LOGE(TAG, "Failed to open file for writing");
 				}
				fprintf(f, MQTT_PAYLOAD);
	       			fclose(f);
				ESP_LOGI(TAG, "File writtten");
				get_file_size(filename);
				esp_littlefs_num_files(ESP_LITTLEFS_BASEPATH);
				ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
		
				voltage = 230;
				mqtt_flag = false;
			}else {
        		msg_id = esp_mqtt_client_enqueue(client, MQTT_TOPIC,MQTT_PAYLOAD, 0, 2, 0, 0);
        		ESP_LOGI(TAG, "MQTT payload published, msg_id=%d, payload: %s",msg_id, MQTT_PAYLOAD);

			//TODO: Use a generic function for file generation, to be used for different commands (requests, normal data etc.)
			//This implementation is temporary
			char filename[128];
			sprintf(filename, "/file_storage/%s_%s_%d.txt", mac, timestamp, msg_id);
			FILE *f = fopen(filename,"w");
			if (f == NULL){
   				ESP_LOGE(TAG, "Failed to open file for writing");
 			}
			fprintf(f, MQTT_PAYLOAD);
	       		fclose(f);
			ESP_LOGI(TAG, "File writtten");
			get_file_size(filename);
			esp_littlefs_num_files(ESP_LITTLEFS_BASEPATH);
			ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
		
			voltage = 230;
			mqtt_flag = false;
			}
			}else{
				ESP_LOGI(TAG, "MQTT client not configured");
			}
		}
	printf("Voltage: %.2f\n", voltage);
	vTaskDelay(VOLTAGE_READING_INTERVAL_MS/portTICK_PERIOD_MS);
	}
}

static void energy_readings_timercb(TimerHandle_t energy_redings_timer)
 {
    float sensor_reading = (float)(rand() % 100); //Generate a random sensor reading between 0 and 99 and cast it as a float
    printf("Voltage: %.2f\n", voltage);		
 		
    if (!buffer.is_full){
	//TaskDelay(SENSOR_READING_INTERVAL_MS/portTICK_PERIOD_MS);
	ring_buffer_append(&buffer,sensor_reading);
	printf("Buffer is not full, appending values: %.2f\n", sensor_reading);
	printf("Interim buffer values are %.2f,%.2f,%.2f,%.2f and the count is %d\n",buffer.data[0],buffer.data[1],buffer.data[2],buffer.data[3], buffer.count);	
        if(buffer.is_full){
		printf("Buffer full, flushing...\n");
		if (ring_buffer_flush(&buffer, flushed_data)){
			printf("Flushed data: %.2f, %.2f, %.2f\n", flushed_data[0], flushed_data[1], flushed_data[2]);
			snprintf(readings_buffer, sizeof(readings_buffer), "%.2f,%.2f,%.2f,%.2f", voltage, flushed_data[0],flushed_data[1], flushed_data[2]);
			printf("%s\n",readings_buffer);
		}
		mqtt_flag = true;	
    	
	}
    }
}

void app_main()
{
    /**
     * @brief Set the log level for serial port printing.
     */
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_storage_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_bridge_create_all_netif();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    wifi_init();
    
    esp_mesh_lite_config_t mesh_lite_config = ESP_MESH_LITE_DEFAULT_INIT();
    esp_mesh_lite_init(&mesh_lite_config);
    
    //esp_mqtt_init();
    
    //esp_littlefs_init();
    //Get MAC address
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    sprintf(mac, ""MACSTR"", MAC2STR(sta_mac));
    	
    //Concatenate MAC address to the MQTT topic to make it unique	
    strcat(MQTT_TOPIC, mac);
   
    ring_buffer_init(&buffer);
    //ring_buffer_append(&buffer,sensor_reading);
    //voltage = 230;	
    //printf("Enabling EXT0 wakeup on pin GPIO%d\n", ext_wakeup_pin_0);
    
    //mqtt_task();
    /**
     * @brief Create MQTT publish task
     */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(ext_wakeup_pin_0, ESP_EXT0_WAKEUP_LEVEL_HIGH));
    // Configure pullup/downs via RTCIO to tie wakeup pins to inactive level during deep sleep.
    // EXT0 resides in the same power domain (RTC_PERIPH) as the RTC IO pullup/downs.
    // No need to keep that power domain explicitly, unlike EXT1.
    ESP_ERROR_CHECK(rtc_gpio_pullup_dis(ext_wakeup_pin_0));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_en(ext_wakeup_pin_0));

    xTaskCreate(&voltage_task,"voltage_task", 4096, NULL, 5, NULL);
    //xTaskCreate(&data_extraction_and_processing_task, "data_procecing_task", 4096,NULL, 6, NULL);

    /**
     * @breif Create handler
     */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_sta_got_ip_handler, NULL, NULL));
    
    TimerHandle_t timer = xTimerCreate("print_system_info", 11000 / portTICK_PERIOD_MS,
                                       true, NULL, print_system_info_timercb);
    //TimerHandle_t voltage_timer = xTimerCreate("get_voltage", 1000 / portTICK_PERIOD_MS,
     //                                  true, NULL, get_voltage_timercb);
    TimerHandle_t energy_readings_timer = xTimerCreate("mqtt_task", 120000 / portTICK_PERIOD_MS,
                                       true, NULL, energy_readings_timercb);

    xTimerStart(timer, 0);
    //xTimerStart(voltage_timer,0);
    xTimerStart(energy_readings_timer,0);
}
