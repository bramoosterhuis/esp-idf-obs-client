#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/timers.h>
#include <freertos/atomic.h>

#include <esp_system.h>
#include <esp_log.h>

#include <esp_websocket_client.h>

#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <cJSON.h>

#include <nvs.h>
#include <nvs_flash.h>

#include <obs-client.h>

static const char *TAG = "obs-client";

const char obs_client_nvs_namespace[] = "obsclt";
const char obs_client_nvs_host_key[] = "ochst";
const char obs_client_nvs_port_key[] = "ocprt";
const char obs_client_nvs_password_key[] = "ocpwd";
const char obs_client_nvs_inactive_timeout_ticks_key[] = "ocitt";
const char obs_client_nvs_connection_timeout_ticks_key[] = "occtt";
const char obs_client_nvs_reconect_key[] = "ocrct";

#define SHA256_HASH_LEN 32 /* SHA-256 digest length */

#define OBS_CLIENT_TCP_DEFAULT_PORT (4444)
#define OBS_CLIENT_BUFFER_SIZE_BYTE (1024)
#define OBS_CLIENT_RECONNECT_TIMEOUT_MS (20 * 1000)
#define OBS_CLIENT_EVENT_QUEUE_SIZE (1)
#define OBS_CLIENT_TASK_PRIORITY (5)
#define OBS_CLIENT_TASK_STACK (4 * 1024)
#define OBS_CLIENT_NETWORK_TIMEOUT_MS (20 * 1000)
#define OBS_CLIENT_PING_TIMEOUT_MS (20 * 1000)
#define OBS_CLIENT_PINGPONG_TIMEOUT_SEC (120)

static uint32_t obs_message_id = 0;

typedef struct obs_client_message_buffer_t
{
    char *buffer;
    int size;
    int position;
} obs_client_message_buffer_t;

static obs_client_message_buffer_t *_obs_client_current_message = NULL;

static void obs_get_message_id(uint32_t *id, char *str_id)
{
    *id = Atomic_Increment_u32(&obs_message_id);
    snprintf(str_id, 5, "%04X", *id);
}

ESP_EVENT_DEFINE_BASE(OBS_EVENTS);

#define ESP_OBS_CLIENT_MEM_CHECK(TAG, a, action)                                 \
    if (!(a))                                                                    \
    {                                                                            \
        ESP_LOGE(TAG, "%s(%d): %s", __FUNCTION__, __LINE__, "Memory exhausted"); \
        action;                                                                  \
    }

#define OBS_CLIENT_STATE_CHECK(TAG, a, action)                                                \
    if ((a->state) < OBS_STATE_INIT)                                                          \
    {                                                                                         \
        ESP_LOGE(TAG, "%s:%d (%s): %s", __FILE__, __LINE__, __FUNCTION__, "Already stopped"); \
        action;                                                                               \
    }

typedef struct obs_send_message_item_t
{
    uint32_t id;
    obs_request_message_type_t type;
} obs_send_message_item_t;

static obs_send_message_item_t *internal_message = NULL;

typedef struct
{
    char *host;                          /*!< Domain or IP as string */
    int port;                            /*!< Port to connect */
    char *password;                      /*!< Using for Http authentication */
    TickType_t connection_timeout_ticks; /*!< Time-out for websocket */
    TickType_t inactivity_timeout_ticks; /*!< Time-out for obs messages */
    bool auto_reconnect;                 /*!< Do we want to reconnect to OBS if connection fails */
} obs_config_storage_t;

typedef enum
{
    OBS_CLIENT_STATE_ERROR = -1,
    OBS_CLIENT_STATE_UNKNOWN = 0,
    OBS_CLIENT_STATE_INIT,
    OBS_CLIENT_STATE_CONNECTING,
    OBS_CLIENT_STATE_CONNECTED,
    OBS_CLIENT_STATE_WAIT_TIMEOUT,
    OBS_CLIENT_STATE_DISCONNECTING,
    OBS_CLIENT_STATE_DISCONNECTED
} obs_client_state_t;

struct obs_client
{
    esp_event_loop_handle_t event_handle;
    TimerHandle_t inactive_signal_timer;
    esp_websocket_client_handle_t connection;
    obs_config_storage_t *config;
    obs_client_state_t state;
    xSemaphoreHandle lock;
    TaskHandle_t message_parse_worker;
    QueueHandle_t to_do_list;
};

// Private
void obs_client_lock(obs_client_handle_t client)
{
    xSemaphoreTake(client->lock, portMAX_DELAY);
}

void obs_client_unlock(obs_client_handle_t client)
{
    xSemaphoreGive(client->lock);
}

static esp_err_t obs_client_dispatch_event(obs_client_handle_t client,
                                           obs_event_id_t event,
                                           const void *data,
                                           int data_len)
{
    esp_err_t err = ESP_OK;
    obs_event_data_t event_data;

    event_data.client = client;
    event_data.data_ptr = data;
    event_data.data_len = data_len;

    err = esp_event_post_to(client->event_handle,
                            OBS_EVENTS, event,
                            &event_data,
                            sizeof(obs_event_data_t),
                            (client->config != NULL) ? client->config->connection_timeout_ticks : portMAX_DELAY);
    if (err == ESP_OK)
        err = esp_event_loop_run(client->event_handle, 0);

    return err;
}

static void obs_client_inactivity_signaler(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "client inactive for too long");
}

static esp_err_t obs_client_destroy_config(obs_config_storage_t *config)
{
    if (config == NULL)
        return ESP_ERR_INVALID_ARG;

    if (config->host)
    {
        free((void *)config->host);
    }

    if (config->password)
    {
        free((void *)config->password);
    }

    memset(config, 0, sizeof(obs_config_storage_t));

    return ESP_OK;
}

static esp_err_t obs_client_clean_config(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    obs_client_lock(client);

    if (client->config == NULL)
    {
        obs_client_unlock(client);
        return ESP_ERR_NOT_FOUND;
    }

    obs_client_destroy_config(client->config);

    free((void *)client->config);
    client->config = NULL;

    obs_client_unlock(client);
    return ESP_OK;
}

esp_err_t obs_client_destroy_message(obs_client_message_buffer_t *message)
{
    if (message == NULL)
        return ESP_ERR_INVALID_ARG;

    if (message->buffer)
    {
        free(message->buffer);
    }

    memset(message, 0, sizeof(obs_client_message_buffer_t));

    return ESP_OK;
}

esp_err_t obs_client_set_config_priv(obs_client_handle_t client, const obs_client_config_t *config)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    obs_client_clean_config(client);

    obs_client_lock(client);

    client->config = calloc(1, sizeof(obs_config_storage_t));
    ESP_OBS_CLIENT_MEM_CHECK(TAG, client->config, goto _obs_init_fail);

    obs_config_storage_t *cfg = client->config;

    if (config->host)
    {
        cfg->host = strdup(config->host);
        ESP_OBS_CLIENT_MEM_CHECK(TAG, cfg->host, return ESP_ERR_NO_MEM);
    }

    if (config->port)
    {
        cfg->port = config->port;
    }
    else
    {
        cfg->port = OBS_CLIENT_TCP_DEFAULT_PORT;
    }

    if (config->password)
    {
        free((void *)cfg->password);
        cfg->password = strdup(config->password);
        ESP_OBS_CLIENT_MEM_CHECK(TAG, cfg->password, goto _obs_init_fail);
    }

    if (config->connection_timeout_ms)
    {
        cfg->connection_timeout_ticks = pdMS_TO_TICKS(config->connection_timeout_ms);
    }
    else
    {
        cfg->connection_timeout_ticks = portMAX_DELAY;
    }

    if (config->inactivity_timeout_ms)
    {
        cfg->inactivity_timeout_ticks = pdMS_TO_TICKS(config->inactivity_timeout_ms);
    }
    else
    {
        cfg->inactivity_timeout_ticks = portMAX_DELAY;
    }

    if (config->disable_auto_reconnect == true)
    {
        cfg->auto_reconnect = false;
    }
    else
    {
        cfg->auto_reconnect = true;
    }

    obs_client_unlock(client);
    return ESP_OK;

_obs_init_fail:
    obs_client_unlock(client);
    return ESP_ERR_NO_MEM;
}

esp_err_t obs_client_authenicate(obs_client_handle_t client, cJSON *json)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    esp_err_t result = ESP_OK;

    if (json != NULL)
    {
        const cJSON *required = cJSON_GetObjectItemCaseSensitive(json, "authRequired");

        if (cJSON_IsTrue(required) == true)
        {
            const cJSON *salt = cJSON_GetObjectItemCaseSensitive(json, "salt");
            const cJSON *challenge = cJSON_GetObjectItemCaseSensitive(json, "challenge");

            if (
                (cJSON_IsString(salt) && (salt->valuestring != NULL)) &&
                (cJSON_IsString(challenge) && (challenge->valuestring != NULL)))
            {
                ESP_LOGD(TAG, "salt: %s", salt->valuestring);
                ESP_LOGD(TAG, "challenge: %s", challenge->valuestring);
                ESP_LOGD(TAG, "password: %s", client->config->password);

                mbedtls_sha256_context *sha256_ctx = (mbedtls_sha256_context *)malloc(sizeof(mbedtls_sha256_context));
                mbedtls_sha256_free(sha256_ctx);

                unsigned char sha256[SHA256_HASH_LEN];
                size_t hashb64_len = 0;

                memset(sha256, 0, sizeof(sha256));
                mbedtls_sha256_init(sha256_ctx);
                mbedtls_sha256_starts_ret(sha256_ctx, false);
                mbedtls_sha256_update_ret(sha256_ctx, (unsigned char *)client->config->password, strlen(client->config->password));
                mbedtls_sha256_update_ret(sha256_ctx, (unsigned char *)salt->valuestring, strlen(salt->valuestring));
                mbedtls_sha256_finish_ret(sha256_ctx, sha256);
                mbedtls_sha256_free(sha256_ctx);

                //obs_print_buffer(sizeof(sha256), (uint8_t *)sha256);

                unsigned char secret[128];
                memset(secret, 0, sizeof(secret));

                mbedtls_base64_encode(secret, sizeof(secret), &hashb64_len, sha256, sizeof(sha256));

                ESP_LOGD(TAG, "hashb64[%d]: %s", hashb64_len, secret);

                memset(sha256, 0, sizeof(sha256));
                mbedtls_sha256_init(sha256_ctx);
                mbedtls_sha256_starts_ret(sha256_ctx, false);
                mbedtls_sha256_update_ret(sha256_ctx, secret, hashb64_len);
                mbedtls_sha256_update_ret(sha256_ctx, (unsigned char *)challenge->valuestring, strlen(challenge->valuestring));
                mbedtls_sha256_finish_ret(sha256_ctx, sha256);
                mbedtls_sha256_free(sha256_ctx);

                //obs_print_buffer(sizeof(sha256), (uint8_t *)sha256);

                unsigned char auth[128];
                memset(auth, 0, sizeof(auth));

                mbedtls_base64_encode(auth, sizeof(auth), &hashb64_len, sha256, sizeof(sha256));
                ESP_LOGD(TAG, "hashb64[%d]: %s", hashb64_len, auth);

                cJSON *body = cJSON_CreateObject();

                cJSON_AddStringToObject(body, "auth", (char *)auth);

                obs_request_message_t message = {0};

                message.type = ObsRequestAuthenticate;
                message.body = body;

                internal_message = calloc(1, sizeof(obs_send_message_item_t));
                ESP_OBS_CLIENT_MEM_CHECK(TAG, internal_message, {
                    cJSON_Delete(body);
                    return ESP_ERR_NO_MEM;
                });

                internal_message->type = message.type;

                result = obs_client_send_request(client, message, &internal_message->id);

                cJSON_Delete(body);
            }
        }
        else
        {
            client->state = OBS_CLIENT_STATE_CONNECTED;
            obs_client_dispatch_event(client, OBS_EVENT_CLIENT_CONNECTED, NULL, 0);
        }
    }
    return result;
}

void obs_client_parse_event(obs_client_handle_t client, cJSON *json)
{
    obs_event_message_t event = {0};

    cJSON *update_type = cJSON_DetachItemFromObjectCaseSensitive(json, "update-type");

    obs_event_message_type_t type = OBS_EVENT_TYPE_END;

    if (cJSON_IsString(update_type) && (update_type->valuestring != NULL))
    {
        type = obs_get_event_type_from_string(update_type->valuestring);
    }

    cJSON_Delete(update_type);

    event.type = type;
    event.body = json;

    // TODO: add filter to not spam the user too much.
    obs_client_dispatch_event(client, OBS_EVENT_MESSAGE_EVENT, &event, sizeof(event));
}

void obs_client_parse_response(obs_client_handle_t client, cJSON *json)
{
    obs_request_result_t status = OBS_RESULT_UNKNOWN;
    uint32_t id = 0;
    bool internal = false;

    cJSON *j_status = cJSON_DetachItemFromObjectCaseSensitive(json, "status");
    if (strcmp(j_status->valuestring, "ok") == 0)
    {
        status = OBS_RESULT_OK;
    }
    else
    {
        status = OBS_RESULT_ERROR;
    }
    cJSON_Delete(j_status);

    cJSON *messageId = cJSON_DetachItemFromObjectCaseSensitive(json, "message-id");
    if (cJSON_IsString(messageId) && (messageId->valuestring != NULL))
    {
        id = (uint32_t)strtoul(messageId->valuestring, NULL, 16);
        ESP_LOGD(TAG, "Parsed response of message id 0x%04X", id);
    }
    cJSON_Delete(messageId);

    if (internal_message != NULL)
    {
        if ((id == internal_message->id) && (internal_message->type == ObsRequestGetAuthRequired))
        {
            free((void *)internal_message);
            internal_message = NULL;
            internal = true;

            obs_client_authenicate(client, json);
        }
        else if ((id == internal_message->id) && (internal_message->type == ObsRequestAuthenticate))
        {
            free((void *)internal_message);
            internal_message = NULL;
            internal = true;

            client->state = OBS_CLIENT_STATE_CONNECTED;
            ESP_LOGI(TAG, "Connected to OBS");
            obs_client_dispatch_event(client, OBS_EVENT_CLIENT_CONNECTED, NULL, 0);
        }
        else
        {
            ESP_LOGE(TAG, "Internal message is set but no handeler is found.");
        }
    }

    if (internal == false)
    {
        obs_response_message_t response = {0};

        response.id = id;
        response.result = status;
        response.body = json;

        obs_client_dispatch_event(client, OBS_EVENT_MESSAGE_RESONSE, &response, sizeof(response));
    }
}

void obs_client_parse_raw_data(obs_client_handle_t client, int data_len, const char *data)
{
    cJSON *json = cJSON_Parse(data);

    if (json != NULL)
    {
        // ESP_LOGD(TAG, "parsed=%s", cJSON_Print(json));

        const cJSON *status = cJSON_GetObjectItemCaseSensitive(json, "status");
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(json, "update-type");

        if (cJSON_IsString(status) && (status->valuestring != NULL))
        {
            obs_client_parse_response(client, json);
        }
        else if (cJSON_IsString(event) && (event->valuestring != NULL))
        {
            obs_client_parse_event(client, json);
        }

        cJSON_Delete(json);
    }
    else
    {
        ESP_LOGE(TAG, "Error could not parse raw data to json");
    }
}

esp_err_t obs_client_check_auth_requiered(obs_client_handle_t client)
{

    if (client == NULL)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;

    obs_request_message_t message = {0};

    message.type = ObsRequestGetAuthRequired;

    internal_message = calloc(1, sizeof(obs_send_message_item_t)); // will be cleared by the response message;
    ESP_OBS_CLIENT_MEM_CHECK(TAG, internal_message, return ESP_ERR_NO_MEM);

    internal_message->type = message.type;

    result = obs_client_send_request(client, message, &internal_message->id);

    return result;
}

void obs_client_websocket_event_handler(void *user_data, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    obs_client_handle_t client = (obs_client_handle_t)user_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
    {
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        if (client->state <= OBS_CLIENT_STATE_INIT)
        {
            obs_client_check_auth_requiered(client);
            client->state = OBS_CLIENT_STATE_CONNECTING;
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    {
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        client->state = OBS_CLIENT_STATE_DISCONNECTED;
        obs_client_dispatch_event(client, OBS_EVENT_CLIENT_DISCONNECTED, NULL, 0);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
    {
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGD(TAG, "Received opcode=%d", data->op_code);
        ESP_LOGD(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n",
                 data->payload_len, data->data_len, data->payload_offset);

        if (data->op_code == 0x08)
        {
            ESP_LOGI(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
            client->state = OBS_EVENT_CLIENT_DISCONNECTED;
            obs_client_dispatch_event(client, OBS_EVENT_CLIENT_DISCONNECTED, NULL, 0);
        }
        else if (data->payload_len > 0)
        {
            // lets hope that data send by multiple events are garanteed to be received in order.
            if (_obs_client_current_message == NULL)
            {
                _obs_client_current_message = calloc(1, sizeof(obs_client_message_buffer_t));

                ESP_OBS_CLIENT_MEM_CHECK(TAG, _obs_client_current_message, {
                    return;
                });

                _obs_client_current_message->buffer = calloc(data->payload_len, sizeof(char));

                ESP_OBS_CLIENT_MEM_CHECK(TAG, _obs_client_current_message->buffer, {
                    free(_obs_client_current_message);
                    return;
                });

                _obs_client_current_message->size = data->payload_len;
                _obs_client_current_message->position = 0;
            }

            memmove(
                &_obs_client_current_message->buffer[_obs_client_current_message->position],
                data->data_ptr,
                data->data_len);

            _obs_client_current_message->position += data->data_len;

            // all data complete, send it over to the parser
            if (_obs_client_current_message->position == _obs_client_current_message->size)
            {
                obs_client_message_buffer_t *message = _obs_client_current_message;
                _obs_client_current_message = NULL;

                xQueueSend(client->to_do_list, message, portMAX_DELAY);
            }
        }

        if (client->config->inactivity_timeout_ticks != portMAX_DELAY)
        {
            xTimerReset(client->inactive_signal_timer, client->config->inactivity_timeout_ticks);
        }

        break;
    }
    case WEBSOCKET_EVENT_ERROR:
    {
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
        obs_client_stop(client);
        obs_client_dispatch_event(client, OBS_EVENT_CLIENT_ERROR, NULL, 0);
        break;
    }
    }
}

void obs_client_message_worker(void *pvParameters)
{
    BaseType_t xStatus;
    obs_client_message_buffer_t data;

    obs_client_handle_t client = (obs_client_handle_t)pvParameters;

    for (;;)
    {
        xStatus = xQueueReceive(client->to_do_list, &data, portMAX_DELAY);

        if (xStatus == pdPASS)
        {
            ESP_LOGD(TAG, "parse data(%d)=%p", data.size, data.buffer);
            obs_client_parse_raw_data(client, data.size, data.buffer);
            obs_client_destroy_message(&data);
        }
    }

    vTaskDelete(NULL);
}

esp_err_t obs_client_recall_config_from_nvs(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    if ((handle <= 0) || (key == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;

    uint8_t buffer[64];
    size_t size = sizeof(buffer);

    result = nvs_get_blob(handle, key, buffer, &size);

    if ((result == ESP_OK) && (value == NULL) && (length != NULL))
    {
        *length = size;
        ESP_LOGD(TAG, "obs_client_recall_config_from_nvs key %s has length %d", key, *length);
    }
    else if ((result == ESP_OK) && (size > *length))
    {
        ESP_LOGE(TAG, "obs_client_recall_config_from_nvs key %s with length %d will not fit %d.", key, size, *length);
        result = ESP_ERR_NO_MEM;
    }

    if ((result == ESP_OK) && (length != NULL) && (value != NULL) && (*length >= size))
    {
        memcpy(value, buffer, size);
    }

    return result;
}

esp_err_t obs_client_recall_config(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    nvs_handle handle = 0;
    esp_err_t result = ESP_FAIL;

    result = nvs_open(obs_client_nvs_namespace, NVS_READONLY, &handle);

    if (result == ESP_OK)
    {
        obs_config_storage_t *config = calloc(1, sizeof(obs_config_storage_t));
        ESP_OBS_CLIENT_MEM_CHECK(TAG, config, {
            return ESP_ERR_NO_MEM;
        });

        size_t size = sizeof(config->port);
        result = obs_client_recall_config_from_nvs(handle, obs_client_nvs_port_key, &(config->port), &size);
        ESP_LOGD(TAG, "obs_client_recall_config_from_nvs obs_client_nvs_port_key result: 0x%08X.", result);

        if (result == ESP_OK)
        {
            size = 0;
            result = obs_client_recall_config_from_nvs(handle, obs_client_nvs_host_key, NULL, &size);

            if ((size > 0) && (result == ESP_OK))
            {
                config->host = calloc(size + 1, sizeof(char));
                ESP_OBS_CLIENT_MEM_CHECK(TAG, config->host, {
                    obs_client_destroy_config(&config);
                    free(config);
                    return ESP_ERR_NO_MEM;
                });

                result = obs_client_recall_config_from_nvs(handle, obs_client_nvs_host_key, config->host, &size);
                ESP_LOGD(TAG, "obs_client_recall_config_from_nvs result obs_client_nvs_host_key: 0x%08X.", result);
            }
        }

        if (result == ESP_OK)
        {
            size = 0;
            result = obs_client_recall_config_from_nvs(handle, obs_client_nvs_password_key, NULL, &size);

            if ((size > 0) && (result == ESP_OK))
            {
                config->password = calloc(size + 1, sizeof(char));
                ESP_OBS_CLIENT_MEM_CHECK(TAG, config->password, {
                    obs_client_destroy_config(&config);
                    free(config);
                    return ESP_ERR_NO_MEM;
                });

                result = obs_client_recall_config_from_nvs(handle, obs_client_nvs_password_key, config->password, &size);
                ESP_LOGD(TAG, "obs_client_recall_config_from_nvs obs_client_nvs_password_key result: 0x%08X.", result);
            }
        }

        if (result == ESP_OK)
        {
            size = sizeof(config->inactivity_timeout_ticks);
            result = obs_client_recall_config_from_nvs(handle, obs_client_nvs_inactive_timeout_ticks_key, &(config->inactivity_timeout_ticks), &size);
            ESP_LOGD(TAG, "obs_client_recall_config_from_nvs obs_client_nvs_inactive_timeout_ticks_key result: 0x%08X.", result);
        }

        if (result == ESP_OK)
        {
            size = sizeof(config->connection_timeout_ticks);
            result = obs_client_recall_config_from_nvs(handle, obs_client_nvs_connection_timeout_ticks_key, &(config->connection_timeout_ticks), &size);
            ESP_LOGD(TAG, "obs_client_recall_config_from_nvs obs_client_nvs_connection_timeout_ticks_key result: 0x%08X.", result);
        }

        if (result == ESP_OK)
        {
            size = sizeof(config->auto_reconnect);
            result = obs_client_recall_config_from_nvs(handle, obs_client_nvs_reconect_key, &(config->auto_reconnect), &size);
            ESP_LOGD(TAG, "obs_client_recall_config_from_nvs obs_client_nvs_reconect_key result: 0x%08X.", result);
        }

        if (result == ESP_OK)
        {
            client->config = config;
            ESP_LOGI(TAG, "obs_client_recall_config_from_nvs applied config: host=%s port=%d password=%s result: 0x%08X.", config->host, config->port, config->password, result);
        }
        else
        {
            obs_client_destroy_config(config);
            free(config);
            client->config = NULL;
            ESP_LOGD(TAG, "obs_client_recall_config_from_nvs failed result: 0x%08X.", result);
        }

        nvs_close(handle);
    }
    else
    {
        client->config = NULL;
    }

    return result;
}

esp_err_t obs_client_write_config_to_nvs(nvs_handle_t handle, const char *key, const void *value, const size_t length, bool *changed)
{
    if ((handle <= 0) || (key == NULL) || (value == NULL) || (length <= 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    uint8_t buffer[64];
    size_t size = sizeof(buffer);

    result = nvs_get_blob(handle, key, buffer, &size);

    if ((result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) &&
        ((size != length) || (size == 0) || (memcmp(buffer, value, size) != 0)))
    {
        ESP_LOGD(TAG, "obs_client_write_config_to_nvs handle=%d key=%s, value_ptr=%p, value_len=%d", handle, key, value, length);

        result = nvs_set_blob(handle, key, value, length);

        if (changed != NULL)
        {
            *changed = true;
        }
    }
    else
    {
        if (changed != NULL)
        {
            *changed = false;
        }
    }

    return result;
}

// Public
esp_err_t obs_client_persist_config(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    nvs_handle handle = 0;
    esp_err_t result = ESP_FAIL;
    bool change;

    result = nvs_open(obs_client_nvs_namespace, NVS_READWRITE, &handle);

    if (result == ESP_OK)
    {
        result = obs_client_write_config_to_nvs(handle, obs_client_nvs_port_key, &(client->config->port), sizeof(client->config->port), &change);
        ESP_LOGD(TAG, "obs_client_write_config_to_nvs obs_client_nvs_port_key result: 0x%08X.", result);
        result = obs_client_write_config_to_nvs(handle, obs_client_nvs_host_key, client->config->host, strlen(client->config->host), &change);
        ESP_LOGD(TAG, "obs_client_write_config_to_nvs result obs_client_nvs_host_key: 0x%08X.", result);
        result = obs_client_write_config_to_nvs(handle, obs_client_nvs_password_key, client->config->password, strlen(client->config->password), &change);
        ESP_LOGD(TAG, "obs_client_write_config_to_nvs obs_client_nvs_password_key result: 0x%08X.", result);
        result = obs_client_write_config_to_nvs(handle, obs_client_nvs_inactive_timeout_ticks_key, &(client->config->inactivity_timeout_ticks), sizeof(client->config->inactivity_timeout_ticks), &change);
        ESP_LOGD(TAG, "obs_client_write_config_to_nvs obs_client_nvs_inactive_timeout_ticks_key result: 0x%08X.", result);
        result = obs_client_write_config_to_nvs(handle, obs_client_nvs_connection_timeout_ticks_key, &(client->config->connection_timeout_ticks), sizeof(client->config->connection_timeout_ticks), &change);
        ESP_LOGD(TAG, "obs_client_write_config_to_nvs obs_client_nvs_connection_timeout_ticks_key result: 0x%08X.", result);
        result = obs_client_write_config_to_nvs(handle, obs_client_nvs_reconect_key, &(client->config->auto_reconnect), sizeof(client->config->auto_reconnect), &change);
        ESP_LOGD(TAG, "obs_client_write_config_to_nvs obs_client_nvs_reconect_key result: 0x%08X.", result);

        if ((change == true) && ((result == ESP_OK) || (result == ESP_OK)))
        {
            result = nvs_commit(handle);
            ESP_LOGW(TAG, "Config changed and was saved to flash.");
        }
        else if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "Config was not saved to flash because of error: 0x%08X.", result);
        }
        else
        {
            ESP_LOGW(TAG, "Config was not saved to flash because no change has been detected.");
        }

        nvs_close(handle);
    }

    return result;
}

esp_err_t obs_client_clear_config(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    nvs_handle handle = 0;
    esp_err_t result = ESP_FAIL;

    result = nvs_open(obs_client_nvs_namespace, NVS_READWRITE, &handle);

    if (result == ESP_OK)
    {
        result = nvs_erase_all(handle);

        nvs_commit(handle);
        nvs_close(handle);
    }

    result = obs_client_clean_config(client);

    return result;
}

esp_err_t obs_client_get_config(obs_client_handle_t client,
                                obs_client_config_t *config)
{
    if ((client == NULL) || (config == NULL))
        return ESP_ERR_INVALID_ARG;

    obs_client_lock(client);

    esp_err_t result = ESP_OK;

    if (client->config == NULL)
    {
        result = ESP_ERR_NOT_FOUND;
    }
    else
    {
        if (memset(config, 0, sizeof(obs_client_config_t)))
        {
            memcpy(config, client->config, sizeof(obs_client_config_t));
        }
        else
        {
            result = ESP_ERR_NO_MEM;
        }
    }

    obs_client_unlock(client);

    return result;
}

esp_err_t obs_client_set_config(obs_client_handle_t client,
                                const obs_client_config_t *config)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    esp_err_t result = ESP_OK;

    bool reconnect = (client->state == OBS_CLIENT_STATE_CONNECTED); // obs_client_is_connected(client);

    if (reconnect)
    {
        result = obs_client_stop(client);
    }

    if (obs_client_set_config_priv(client, config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set the configuration");
        result = ESP_FAIL;
    }

    if ((client->config->inactivity_timeout_ticks != portMAX_DELAY) && (client->config->inactivity_timeout_ticks > 0))
    {
        client->inactive_signal_timer = xTimerCreate("Inactivity Timer", client->config->inactivity_timeout_ticks,
                                                     pdFALSE, NULL, obs_client_inactivity_signaler);
    }

    if (reconnect)
    {
        result = obs_client_start(client);
    }

    return result;
}

obs_client_handle_t obs_client_create()
{
    obs_client_handle_t client = calloc(1, sizeof(struct obs_client));
    ESP_OBS_CLIENT_MEM_CHECK(TAG, client, return NULL);

    esp_event_loop_args_t event_args = {
        .queue_size = OBS_CLIENT_EVENT_QUEUE_SIZE,
        .task_name = NULL // no task will be created
    };

    if (esp_event_loop_create(&event_args, &client->event_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error create event handler for websocket client");
        free(client);
        return NULL;
    }

    client->lock = xSemaphoreCreateMutex();
    ESP_OBS_CLIENT_MEM_CHECK(TAG, client->lock, goto _obs_init_fail);

    client->config = NULL;

    client->state = OBS_CLIENT_STATE_INIT;

    client->to_do_list = xQueueCreate(3, sizeof(obs_client_message_buffer_t));
    ESP_OBS_CLIENT_MEM_CHECK(TAG, client->to_do_list, goto _obs_init_fail);

    if (nvs_flash_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Error flash init failed");
        goto _obs_init_fail;
    }

    return client;

_obs_init_fail:
    obs_client_destroy(client);
    return NULL;
}

esp_err_t obs_client_start(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    esp_err_t result = ESP_OK;

    if (client->config == NULL)
    {
        result = obs_client_recall_config(client);

        if (client->config == NULL)
        {
            obs_client_dispatch_event(client, OBS_EVENT_CLIENT_NOT_CONFIGURED, NULL, 0);
            return result;
        }
    }

    xTaskCreate(&obs_client_message_worker, "messages parse worker", 3072, (void *)client, 5, &client->message_parse_worker);

    char uri[34];

    strcpy(uri, "ws://");
    strcat(uri, client->config->host);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
        .port = client->config->port};

    client->connection = esp_websocket_client_init(&websocket_cfg);

    result = esp_websocket_register_events(client->connection, WEBSOCKET_EVENT_ANY, obs_client_websocket_event_handler, (void *)client);

    ESP_LOGI(TAG, "Connecting to %s...", websocket_cfg.uri);

    result = esp_websocket_client_start(client->connection);

    if ((client->config->inactivity_timeout_ticks != portMAX_DELAY) && (client->config->inactivity_timeout_ticks > 0))
    {
        xTimerStart(client->inactive_signal_timer, client->config->inactivity_timeout_ticks);
    }

    return result;
}

esp_err_t obs_client_stop(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    if (client->state != OBS_CLIENT_STATE_CONNECTED)
        return ESP_ERR_INVALID_STATE;

    esp_err_t result = ESP_OK;

    if ((client->config->inactivity_timeout_ticks != portMAX_DELAY) && (client->config->inactivity_timeout_ticks > 0))
    {
        xTimerStop(client->inactive_signal_timer, client->config->inactivity_timeout_ticks);
    }

    if (client->connection != NULL)
    {
        result = esp_websocket_client_close(client->connection, client->config->connection_timeout_ticks);
        client->connection = NULL;
    }

    if (client->message_parse_worker != NULL)
    {
        vTaskDelete(client->message_parse_worker);
        client->message_parse_worker = NULL;
    }

    ESP_LOGI(TAG, "Disconnected from OBS host");

    return result;
}

esp_err_t obs_client_destroy(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    esp_err_t result = ESP_OK;

    client->state = OBS_CLIENT_STATE_DISCONNECTING;

    if (client->connection != NULL)
    {
        result = obs_client_stop(client);
        result = esp_websocket_client_destroy(client->connection);
        client->connection = NULL;
    }

    if (client->event_handle != NULL)
    {
        result = esp_event_loop_delete(client->event_handle);
        client->event_handle = NULL;
    }

    if (client->to_do_list != NULL)
    {
        vQueueDelete(client->to_do_list);
        client->to_do_list = NULL;
    }

    if (client->lock != NULL)
    {
        vSemaphoreDelete(client->lock);
        client->lock = NULL;
    }

    nvs_flash_deinit();

    return result;
}

bool obs_client_is_connected(obs_client_handle_t client)
{
    return ((client != NULL) && (client->state == OBS_CLIENT_STATE_CONNECTED));
}

esp_err_t obs_client_register_events(obs_client_handle_t client,
                                     obs_event_id_t event,
                                     esp_event_handler_t event_handler,
                                     void *user_data)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;
    return esp_event_handler_register_with(client->event_handle, OBS_EVENTS, event, event_handler, user_data);
}

esp_err_t obs_client_unregister_events(obs_client_handle_t client,
                                       obs_event_id_t event,
                                       esp_event_handler_t event_handler)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;
    return esp_event_handler_unregister_with(client->event_handle, OBS_EVENTS, event, event_handler);
}

esp_err_t obs_client_send_request(obs_client_handle_t client,
                                  obs_request_message_t message,
                                  uint32_t *message_id)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;

    cJSON *json = NULL;

    char str_id[8] = {0};
    char *payload = NULL;

    if (message.body == NULL)
    {
        json = cJSON_CreateObject();
    }
    else
    {
        json = message.body;
    }

    obs_get_message_id(message_id, str_id);

    cJSON_AddStringToObject(json, "message-id", str_id);
    cJSON_AddStringToObject(json, "request-type", obs_request_message_type_string[message.type]);

    payload = cJSON_PrintUnformatted(json);

    ESP_LOGD(TAG, "Send JSON request message %s", payload);

    result = esp_websocket_client_send_text(client->connection, payload, strlen(payload), client->config->connection_timeout_ticks);

    if (payload != NULL)
    {
        free((void *)payload);
    }

    if (message.body == NULL)
    {
        cJSON_Delete(json);
    }

    return result;
}
