/*
 * fw_update.c
 *
 *  Created on: Oct 16, 2019
 *      Author: kaktus
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "fw_update.h"

#define FW_UPDATE_TASK_TAG	"FW UPDATE TASK"

/* This example demonstrates how to create file server
 * using esp_http_server. This file has only startup code.
 * Look in file_server.c for the implementation */

/* Declare the function which starts the file server.
 * Implementation of this function is to be found in
 * file_server.c */
esp_err_t start_file_server(const char *base_path);

void FW_UPDATE_Task(void *arg)
{
	const TickType_t xDelay = 10 / portTICK_PERIOD_MS;

	ESP_LOGI(FW_UPDATE_TASK_TAG,"Started!");

    /* Start the file server */
    ESP_ERROR_CHECK(start_file_server("/spiffs"));

	while(42)
	{
		vTaskDelay(xDelay);
	}
}


