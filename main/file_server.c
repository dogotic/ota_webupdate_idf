/* HTTP File Server Example

 This example code is in the Public Domain (or CC0 licensed, at your option.)

 Unless required by applicable law or agreed to in writing, this
 software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 CONDITIONS OF ANY KIND, either express or implied.
 */

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"

/* Max length a file path can have on storage */
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE   (1024*1024) // 1 MB
#define MAX_FILE_SIZE_STR "1MB"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192

struct file_server_data
{
	/* Base path of file storage */
	char base_path[ESP_VFS_PATH_MAX + 1];

	/* Scratch buffer for temporary storage during file transfer */
	char scratch[SCRATCH_BUFSIZE];
};

static const char *TAG = "file_server";

/* Handler to redirect incoming GET request for /index.html to /
 * This can be overridden by uploading file with same name */
static esp_err_t index_html_get_handler(httpd_req_t *req)
{
	httpd_resp_set_status(req, "307 Temporary Redirect");
	httpd_resp_set_hdr(req, "Location", "/");
	httpd_resp_send(req, NULL, 0);  // Response body can be empty
	return ESP_OK;
}

/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
	extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
	extern const unsigned char favicon_ico_end[] asm("_binary_favicon_ico_end");
	const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
	httpd_resp_set_type(req, "image/x-icon");
	httpd_resp_send(req, (const char*) favicon_ico_start, favicon_ico_size);
	return ESP_OK;
}

/* Send HTTP response with a run-time generated html consisting of
 * a list of all files and folders under the requested path.
 * In case of SPIFFS this returns empty list when path is any
 * string other than '/', since SPIFFS doesn't support directories */
static esp_err_t http_resp_dir_html(httpd_req_t *req, const char *dirpath)
{
	/* Get handle to embedded file upload script */
	extern const unsigned char upload_script_start[] asm("_binary_upload_script_html_start");
	extern const unsigned char upload_script_end[] asm("_binary_upload_script_html_end");
	const size_t upload_script_size = (upload_script_end - upload_script_start);

	/* Add file upload form and script which on execution sends a POST request to /upload */
	httpd_resp_send_chunk(req, (const char*) upload_script_start,
			upload_script_size);

	/* Send empty chunk to signal HTTP response completion */
	httpd_resp_sendstr_chunk(req, NULL);
	return ESP_OK;
}

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req,
		const char *filename)
{
	if (IS_FILE_EXT(filename, ".pdf"))
	{
		return httpd_resp_set_type(req, "application/pdf");
	}
	else if (IS_FILE_EXT(filename, ".html"))
	{
		return httpd_resp_set_type(req, "text/html");
	}
	else if (IS_FILE_EXT(filename, ".jpeg"))
	{
		return httpd_resp_set_type(req, "image/jpeg");
	}
	else if (IS_FILE_EXT(filename, ".ico"))
	{
		return httpd_resp_set_type(req, "image/x-icon");
	}
	/* This is a limited set only */
	/* For any other type always set as plain text */
	return httpd_resp_set_type(req, "text/plain");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char* get_path_from_uri(char *dest, const char *base_path,
		const char *uri, size_t destsize)
{
	const size_t base_pathlen = strlen(base_path);
	size_t pathlen = strlen(uri);

	const char *quest = strchr(uri, '?');
	if (quest)
	{
		pathlen = MIN(pathlen, quest - uri);
	}
	const char *hash = strchr(uri, '#');
	if (hash)
	{
		pathlen = MIN(pathlen, hash - uri);
	}

	if (base_pathlen + pathlen + 1 > destsize)
	{
		/* Full path string won't fit into destination buffer */
		return NULL;
	}

	/* Construct full path (base + path) */
	strcpy(dest, base_path);
	strlcpy(dest + base_pathlen, uri, pathlen + 1);

	/* Return pointer to path, skipping the base */
	return dest + base_pathlen;
}

/* Handler to download a file kept on the server */
static esp_err_t download_get_handler(httpd_req_t *req)
{
	char filepath[FILE_PATH_MAX];
	FILE *fd = NULL;
	struct stat file_stat;

	const char *filename = get_path_from_uri(filepath,
			((struct file_server_data*) req->user_ctx)->base_path, req->uri,
			sizeof(filepath));
	if (!filename)
	{
		ESP_LOGE(TAG, "Filename is too long");
		/* Respond with 500 Internal Server Error */
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				"Filename too long");
		return ESP_FAIL;
	}

	/* If name has trailing '/', respond with directory contents */
	if (filename[strlen(filename) - 1] == '/')
	{
		return http_resp_dir_html(req, filepath);
	}

	if (stat(filepath, &file_stat) == -1)
	{
		/* If file not present on SPIFFS check if URI
		 * corresponds to one of the hardcoded paths */
		if (strcmp(filename, "/index.html") == 0)
		{
			return index_html_get_handler(req);
		}
		else if (strcmp(filename, "/favicon.ico") == 0)
		{
			return favicon_get_handler(req);
		}
		ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
		/* Respond with 404 Not Found */
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
		return ESP_FAIL;
	}

	fd = fopen(filepath, "r");
	if (!fd)
	{
		ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
		/* Respond with 500 Internal Server Error */
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				"Failed to read existing file");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Sending file : %s (%ld bytes)...", filename,
			file_stat.st_size);
	set_content_type_from_file(req, filename);

	/* Retrieve the pointer to scratch buffer for temporary storage */
	char *chunk = ((struct file_server_data*) req->user_ctx)->scratch;
	size_t chunksize;
	do
	{
		/* Read file in chunks into the scratch buffer */
		chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

		/* Send the buffer contents as HTTP response chunk */
		if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK)
		{
			fclose(fd);
			ESP_LOGE(TAG, "File sending failed!");
			/* Abort sending file */
			httpd_resp_sendstr_chunk(req, NULL);
			/* Respond with 500 Internal Server Error */
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
					"Failed to send file");
			return ESP_FAIL;
		}

		/* Keep looping till the whole file is sent */
	} while (chunksize != 0);

	/* Close file after sending complete */
	fclose(fd);
	ESP_LOGI(TAG, "File sending complete");

	/* Respond with an empty chunk to signal HTTP response completion */
	httpd_resp_send_chunk(req, NULL, 0);
	return ESP_OK;
}

/* Handler to upload a file onto the server */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
	char filepath[FILE_PATH_MAX];
	struct stat file_stat;

	/* -------------------- ota init ------------------------------------------- */

	esp_err_t err;
	/* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
	esp_ota_handle_t update_handle = 0;
	const esp_partition_t *update_partition = NULL;

	const esp_partition_t *configured = esp_ota_get_boot_partition();
	const esp_partition_t *running = esp_ota_get_running_partition();

	if (configured != running)
	{
		ESP_LOGW(TAG,
				"Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
				configured->address, running->address);
		ESP_LOGW(TAG,
				"(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
	}
	ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
			running->type, running->subtype, running->address);

	update_partition = esp_ota_get_next_update_partition(NULL);
	ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
			update_partition->subtype, update_partition->address);
	assert(update_partition != NULL);

	int binary_file_length = 0;
	/*deal with all receive packet*/
	bool image_header_was_checked = false;

	/* ------------- end of ota init ----------------------------------------- */

	/* Skip leading "/upload" from URI to get filename */
	/* Note sizeof() counts NULL termination hence the -1 */
	const char *filename = get_path_from_uri(filepath,
			((struct file_server_data*) req->user_ctx)->base_path,
			req->uri + sizeof("/upload") - 1, sizeof(filepath));
	if (!filename)
	{
		/* Respond with 500 Internal Server Error */
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				"Filename too long");
		return ESP_FAIL;
	}

	/* Filename cannot have a trailing '/' */
	if (filename[strlen(filename) - 1] == '/')
	{
		ESP_LOGE(TAG, "Invalid filename : %s", filename);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				"Invalid filename");
		return ESP_FAIL;
	}

	if (stat(filepath, &file_stat) == 0)
	{
		ESP_LOGE(TAG, "File already exists : %s", filepath);
		/* Respond with 400 Bad Request */
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File already exists");
		return ESP_FAIL;
	}

	/* File cannot be larger than a limit */
	if (req->content_len > MAX_FILE_SIZE)
	{
		ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
		/* Respond with 400 Bad Request */
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
				"File size must be less than "
				MAX_FILE_SIZE_STR "!");
		/* Return failure to close underlying connection else the
		 * incoming file content will keep the socket busy */
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Receiving file : %s...", filename);

	/* Retrieve the pointer to scratch buffer for temporary storage */
	char *ota_write_data = ((struct file_server_data*) req->user_ctx)->scratch;
	int data_read;

	/* Content length of the request gives
	 * the size of the file being uploaded */
	int remaining = req->content_len;
	int file_len = remaining;

	while (remaining > 0)
	{
		// ESP_LOGI(TAG, "Remaining size : %d", remaining);

		data_read = httpd_req_recv(req, ota_write_data,
				MIN(remaining, SCRATCH_BUFSIZE));
		if (data_read <= 0)
		{
			if (data_read == HTTPD_SOCK_ERR_TIMEOUT)
			{
				continue;
			}

			ESP_LOGE(TAG, "File reception failed!");
			/* Respond with 500 Internal Server Error */
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
					"Failed to receive file");
			return ESP_FAIL;
		}

		if (image_header_was_checked == false)
		{
			esp_app_desc_t new_app_info;
			if (data_read
					> sizeof(esp_image_header_t)
							+ sizeof(esp_image_segment_header_t)
							+ sizeof(esp_app_desc_t))
			{
				// check current version with downloading
				memcpy(&new_app_info,
						&ota_write_data[sizeof(esp_image_header_t)
								+ sizeof(esp_image_segment_header_t)],
						sizeof(esp_app_desc_t));
				ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

				esp_app_desc_t running_app_info;
				if (esp_ota_get_partition_description(running,
						&running_app_info) == ESP_OK)
				{
					ESP_LOGI(TAG, "Running firmware version: %s",
							running_app_info.version);
				}

				const esp_partition_t *last_invalid_app =
						esp_ota_get_last_invalid_partition();
				esp_app_desc_t invalid_app_info;
				if (esp_ota_get_partition_description(last_invalid_app,
						&invalid_app_info) == ESP_OK)
				{
					ESP_LOGI(TAG, "Last invalid firmware version: %s",
							invalid_app_info.version);
				}

				// check current version with last invalid partition
				if (last_invalid_app != NULL)
				{
					if (memcmp(invalid_app_info.version, new_app_info.version,
							sizeof(new_app_info.version)) == 0)
					{
						ESP_LOGW(TAG,
								"New version is the same as invalid version.");
						ESP_LOGW(TAG,
								"Previously, there was an attempt to launch the firmware with %s version, but it failed.",
								invalid_app_info.version);
						ESP_LOGW(TAG,
								"The firmware has been rolled back to the previous version.");
					}
				}

				if (memcmp(new_app_info.version, running_app_info.version,
						sizeof(new_app_info.version)) == 0)
				{
					ESP_LOGW(TAG,
							"Current running version is the same as a new. We will not continue the update.");
				}

				image_header_was_checked = true;

				err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN,
						&update_handle);
				if (err != ESP_OK)
				{
					ESP_LOGE(TAG, "esp_ota_begin failed (%s)",
							esp_err_to_name(err));
				}
				ESP_LOGI(TAG, "esp_ota_begin succeeded");
			}
			else
			{
				ESP_LOGE(TAG, "received package is not fit len");
			}
		}

		err = esp_ota_write(update_handle, (const void*) ota_write_data,
				data_read);
		if (err != ESP_OK)
		{

		}

		/* Keep track of remaining size of
		 * the file left to be uploaded */
		remaining -= data_read;
		binary_file_length += data_read;

		printf("%d%%\n",(int)(((double)binary_file_length / file_len)*100));
	}

	ESP_LOGI(TAG, "Total Write binary data length : %d", binary_file_length);

	if (esp_ota_end(update_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_end failed!");
	}

	err = esp_ota_set_boot_partition(update_partition);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!",
				esp_err_to_name(err));
	}
	ESP_LOGI(TAG, "Prepare to restart system!");
	esp_restart();

	return ESP_OK;
}

/* Function to start the file server */
esp_err_t start_file_server(const char *base_path)
{
	static struct file_server_data *server_data = NULL;

	/* Validate file storage base path */
	if (!base_path || strcmp(base_path, "/spiffs") != 0)
	{
		ESP_LOGE(TAG,
				"File server presently supports only '/spiffs' as base path");
		return ESP_ERR_INVALID_ARG;
	}

	if (server_data)
	{
		ESP_LOGE(TAG, "File server already started");
		return ESP_ERR_INVALID_STATE;
	}

	/* Allocate memory for server data */
	server_data = calloc(1, sizeof(struct file_server_data));
	if (!server_data)
	{
		ESP_LOGE(TAG, "Failed to allocate memory for server data");
		return ESP_ERR_NO_MEM;
	}
	strlcpy(server_data->base_path, base_path, sizeof(server_data->base_path));

	httpd_handle_t server = NULL;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();

	/* Use the URI wildcard matching function in order to
	 * allow the same handler to respond to multiple different
	 * target URIs which match the wildcard scheme */
	config.uri_match_fn = httpd_uri_match_wildcard;

	ESP_LOGI(TAG, "Starting HTTP Server");
	if (httpd_start(&server, &config) != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to start file server!");
		return ESP_FAIL;
	}

	/* URI handler for getting uploaded files */
	httpd_uri_t file_download =
	{ .uri = "/*",  // Match all URIs of type /path/to/file
			.method = HTTP_GET, .handler = download_get_handler, .user_ctx =
					server_data    // Pass server data as context
			};
	httpd_register_uri_handler(server, &file_download);

	/* URI handler for uploading files to server */
	httpd_uri_t file_upload =
	{ .uri = "/upload/*",   // Match all URIs of type /upload/path/to/file
			.method = HTTP_POST, .handler = upload_post_handler, .user_ctx =
					server_data    // Pass server data as context
			};
	httpd_register_uri_handler(server, &file_upload);

	return ESP_OK;
}
