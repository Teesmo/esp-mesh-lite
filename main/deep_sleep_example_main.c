/*
 * SPDX-FileCopyrightText: 2017-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdbool.h>
#include "deep_sleep_example.h"
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
//// #include "sdkconfig.h"
//#include "soc/soc_caps.h"
//#include "freertos/FreeRTOS.h"
//#include "freertos/task.h"
//#include "esp_sleep.h"
//#include "esp_log.h"
//#include "driver/rtc_io.h"
//// #include "nvs_flash.h"
//// #include "nvs.h"
//#include "deep_sleep_example.h"
//#include <stdio.h>
//#include "esp_sleep.h"
//#include "sdkconfig.h"
//#include "driver/rtc_io.h"
#define RING_BUFFER_SIZE 4
#define USAGE_BUFFER_SIZE 3
#define SENSOR_READING_INTERVAL_MS (10*1000) // 10 seconds in milliseconds
#define ESP_EXT0_WAKEUP_LEVEL_HIGH 1
//const int ext_wakeup_pin_0 = 8;
//
//typedef struct {
//	float data[RING_BUFFER_SIZE];
//	int head;
//	int tail;
//	int count;
//	bool is_full;
//}ring_buffer_t;
//
//RTC_DATA_ATTR ring_buffer_t buffer; //persist ring buffer on RTC memory
//float voltage;
//char readings_buffer[30];
RTC_DATA_ATTR float flushed_data[USAGE_BUFFER_SIZE]; //persist meter readings on RTC memory

ring_buffer_t ring_buffer_init(ring_buffer_t *buffer){
	if (esp_sleep_get_wakeup_cause()== ESP_SLEEP_WAKEUP_EXT0){
		printf("Wake up from ext0\n");
		printf("%.2f,%.2f,%.2f,%.2f,%d\n", buffer->data[0],buffer->data[1], buffer->data[2], buffer->data[3], buffer->count);
		return *buffer;
	}else{	
       //initialize ring buffer variables
         buffer->head = 0;
	 buffer->tail = 0;
	 buffer->count = 0;
	 buffer->is_full = false;
	}
	return *buffer;
}

bool RTC_IRAM_ATTR ring_buffer_flush(ring_buffer_t *buffer, float *flushed_data){
	if (buffer->count == 0){
		//Buffer is empty
		return false;
	}

	int i;
	//populate flushed_data with meter readings
	for (i = 0; i < 3; i++){
		flushed_data[(USAGE_BUFFER_SIZE-1) - i] = buffer->data[(RING_BUFFER_SIZE-1) - i]- buffer->data[(RING_BUFFER_SIZE-1)-(i+1)];
		printf("Usage: %.2f\n", flushed_data[(USAGE_BUFFER_SIZE-1) - i]);
	}
	//Reset ring buffer
	buffer->head = buffer->data[3];
	buffer->tail = 0;
	buffer->count = 1;
	buffer->is_full = false;

	return true;
}

bool RTC_IRAM_ATTR ring_buffer_append(ring_buffer_t *buffer, float item) {
	if (buffer->is_full){
		//Buffer is full, flush it
		ring_buffer_flush(buffer, flushed_data);
	}
	buffer->data[buffer->head] =item;
	buffer->head =(buffer->head + 1) % RING_BUFFER_SIZE;
	buffer->count++;
	if (buffer->count == RING_BUFFER_SIZE){
	       buffer->is_full = true;
	}
	return true;
}

//void data_extraction_and_processing_task(void *pvParameters)
//{	
//	ring_buffer_init(&buffer);
//	//ring_buffer_append(&buffer,sensor_reading);
//	voltage = 230;
//	//xTaskCreate(sensorReadingTask, "Sensor Reading Task", configMINIMAL_STACK_SIZE,NULL, 5, NULL);
//   	printf("Enabling EXT0 wakeup on pin GPIO%d\n", ext_wakeup_pin_0);
//    	ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(ext_wakeup_pin_0, ESP_EXT0_WAKEUP_LEVEL_HIGH));
//    	// Configure pullup/downs via RTCIO to tie wakeup pins to inactive level during deep sleep.
//    	// EXT0 resides in the same power domain (RTC_PERIPH) as the RTC IO pullup/downs.
//    	// No need to keep that power domain explicitly, unlike EXT1.
//    	ESP_ERROR_CHECK(rtc_gpio_pullup_dis(ext_wakeup_pin_0));
//    	ESP_ERROR_CHECK(rtc_gpio_pulldown_en(ext_wakeup_pin_0));
//
//    	while (1){
//       		//Simulate sensor readings
//		float sensor_reading = (float)(rand() % 100); //Generate a random sensor reading between 0 and 99 and cast it as a float
//				
//		//Monitor the EXT0 wakeup pin and sleep when triggering condition is met
//		if (gpio_get_level(ext_wakeup_pin_0) != ESP_EXT0_WAKEUP_LEVEL_HIGH){
//	    		printf("Entering deep sleep\n");
//    	    		esp_deep_sleep_start();
//    			}		
//		else{
//			if (!buffer.is_full){
//			vTaskDelay(SENSOR_READING_INTERVAL_MS / portTICK_PERIOD_MS);
//			ring_buffer_append(&buffer,sensor_reading);
//			printf("Buffer is not full, appending values: %.2f\n", sensor_reading);
//			printf("Interim buffer values are %.2f,%.2f,%.2f,%.2f and the count is %d\n",buffer.data[0],buffer.data[1],buffer.data[2],buffer.data[3], buffer.count);	
//
//			}else{
//				printf("Buffer full, flushing...\n");
//				if (ring_buffer_flush(&buffer, flushed_data)){
//					printf("Flushed data: %.2f, %.2f, %.2f\n", flushed_data[0], flushed_data[1], flushed_data[2]);
//					snprintf(readings_buffer, sizeof(readings_buffer), "%.2f,%.2f,%.2f,%.2f", voltage, flushed_data[0],flushed_data[1], flushed_data[2]);
//					printf("%s\n",readings_buffer);
//				}	
//			}
//		}
//	}
//	
//}
