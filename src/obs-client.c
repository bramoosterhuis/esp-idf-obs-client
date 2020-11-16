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

#include <obs-client.h>

static const char *TAG = "obs-client";

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
    const char *host;                    /*!< Domain or IP as string */
    int port;                            /*!< Port to connect */
    const char *password;                /*!< Using for Http authentication */
    TickType_t connection_timeout_ticks; /*!< Time out for port related matters */
    void *user_context;                  /*!< User data context */
    TickType_t inactivity_timeout_ticks;
    bool auto_reconnect;
} obs_config_storage_t;

typedef enum
{
    OBS_CLIENT_STATE_ERROR = -1,
    OBS_CLIENT_STATE_UNKNOW = 0,
    OBS_CLIENT_STATE_INIT,
    OBS_CLIENT_STATE_CONNECTED,
    OBS_CLIENT_STATE_WAIT_TIMEOUT,
    OBS_CLIENT_STATE_CLOSING,
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
    TaskHandle_t worker;
    uint32_t last_id;
    int payload_len;
    int payload_offset;
};

// Private
static esp_err_t obs_client_dispatch_event(obs_client_handle_t client,
                                           obs_event_id_t event,
                                           const void *data,
                                           int data_len)
{
    esp_err_t err;
    obs_event_data_t event_data;

    event_data.client = client;
    event_data.user_context = client->config->user_context;
    event_data.data_ptr = data;
    event_data.data_len = data_len;

    if ((err = esp_event_post_to(client->event_handle,
                                 OBS_EVENTS, event,
                                 &event_data,
                                 sizeof(obs_event_data_t),
                                 client->config->connection_timeout_ticks)) != ESP_OK)
    {
        return err;
    }
    return esp_event_loop_run(client->event_handle, 0);
}

static void inactivity_signaler(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "client inactive for too long");

    
}

static esp_err_t obs_client_destroy_config(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;

    obs_config_storage_t *cfg = client->config;

    if (client->config == NULL)
        return ESP_ERR_INVALID_ARG;

    free((void *)cfg->host);
    free((void *)cfg->port);
    free((void *)cfg->password);

    memset(cfg, 0, sizeof(obs_config_storage_t));
    free((void *)client->config);
    client->config = NULL;

    return ESP_OK;
}

static esp_err_t obs_client_set_config(obs_client_handle_t client, const obs_client_config_t *config)
{
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
        ESP_OBS_CLIENT_MEM_CHECK(TAG, cfg->password, return ESP_ERR_NO_MEM);
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

    cfg->user_context = config->user_context;

    if (config->disable_auto_reconnect == true)
    {
        cfg->auto_reconnect = false;
    }
    else
    {
        cfg->auto_reconnect = true;
    }

    return ESP_OK;
}

esp_err_t obs_authenicate(obs_client_handle_t client, cJSON *json)
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
                ESP_LOGI(TAG, "salt: %s", salt->valuestring);
                ESP_LOGI(TAG, "challenge: %s", challenge->valuestring);
                ESP_LOGI(TAG, "password: %s", client->config->password);

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

                ESP_LOGI(TAG, "hashb64[%d]: %s", hashb64_len, secret);

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
                ESP_LOGI(TAG, "hashb64[%d]: %s", hashb64_len, auth);

                cJSON *body = cJSON_CreateObject();

                cJSON_AddStringToObject(body, "auth", (char *)auth);

                obs_request_message_t message = {0};

                message.type = ObsRequestAuthenticate;
                message.body = body;

                internal_message = calloc(1, sizeof(obs_send_message_item_t));

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

void obs_parse_event(obs_client_handle_t client, cJSON *json)
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

void obs_parse_response(obs_client_handle_t client, cJSON *json)
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
        ESP_LOGI(TAG, "message id 0x%04X", id);
    }
    cJSON_Delete(messageId);

    if (internal_message != NULL)
    {
        if ((id == internal_message->id) && (internal_message->type == ObsRequestGetAuthRequired))
        {
            free((void *)internal_message);
            internal_message = NULL;
            internal = true;

            obs_authenicate(client, json);
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

void obs_parse_raw_data(obs_client_handle_t client, int data_len, const char *data)
{
    cJSON *json = cJSON_Parse(data);
    if (json != NULL ){
        // ESP_LOGI(TAG, "parsed=%s", cJSON_Print(json));

        const cJSON *status = cJSON_GetObjectItemCaseSensitive(json, "status");
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(json, "update-type");

        if (cJSON_IsString(status) && (status->valuestring != NULL))
        {
            obs_parse_response(client, json);
        }
        else if (cJSON_IsString(event) && (event->valuestring != NULL))
        {
            obs_parse_event(client, json);
        }

        cJSON_Delete(json);
    } else {
        ESP_LOGE(TAG, "Error could not parse raw data to json");
    }
}

esp_err_t obs_check_auth_requiered(obs_client_handle_t client)
{

    if (client == NULL)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;

    obs_request_message_t message = {0};

    message.type = ObsRequestGetAuthRequired;

    internal_message = calloc(1, sizeof(obs_send_message_item_t)); // will be cleared by the response message;

    internal_message->type = message.type;

    result = obs_client_send_request(client, message, &internal_message->id);

    return result;
}

void obs_websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    obs_client_handle_t client = (obs_client_handle_t)handler_args;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
    {
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        if (client->state <= OBS_CLIENT_STATE_INIT)
        {
            obs_check_auth_requiered(client);
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    {
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        client->state = OBS_CLIENT_STATE_DISCONNECTED;
        obs_client_dispatch_event(client, OBS_EVENT_CLIENT_DISCONNETED, NULL, 0);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
    {
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
        if (data->op_code == 0x08)
        {
            ESP_LOGI(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
            client->state = OBS_CLIENT_STATE_CONNECTED;
            obs_client_dispatch_event(client, OBS_EVENT_CLIENT_DISCONNETED, NULL, 0);
        }
        else if (data->data_len > 0)
        {
            obs_parse_raw_data(client, data->data_len, data->data_ptr);
        }
        
        ESP_LOGI(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);

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

// Public
obs_client_handle_t obs_client_init(const obs_client_config_t *config)
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

    client->lock = xSemaphoreCreateRecursiveMutex();
    ESP_OBS_CLIENT_MEM_CHECK(TAG, client->lock, goto _obs_init_fail);

    client->config = calloc(1, sizeof(obs_config_storage_t));
    ESP_OBS_CLIENT_MEM_CHECK(TAG, client->config, goto _obs_init_fail);

    if (obs_client_set_config(client, config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set the configuration");
        goto _obs_init_fail;
    }

    if ((client->config->inactivity_timeout_ticks != portMAX_DELAY) && (client->config->inactivity_timeout_ticks > 0))
    {
        client->inactive_signal_timer = xTimerCreate("Inactivity Timer", client->config->inactivity_timeout_ticks,
                                                     pdFALSE, NULL, inactivity_signaler);
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

    esp_websocket_client_config_t websocket_cfg = {
        .uri = client->config->host,
        .port = client->config->port};

    client->connection = esp_websocket_client_init(&websocket_cfg);

    result = esp_websocket_register_events(client->connection, WEBSOCKET_EVENT_ANY, obs_websocket_event_handler, (void *)client);

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

    ESP_LOGI(TAG, "Disconnected from OBS host");

    return result;
}

esp_err_t obs_client_destroy(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;

    client->state = OBS_CLIENT_STATE_CLOSING;

    if (client->connection != NULL)
    {
        result = obs_client_stop(client);
        result = esp_websocket_client_destroy(client->connection);
        client->connection = NULL;
    }

    if (client->worker != NULL)
    {
        vTaskDelete(client->worker);
        client->worker = NULL;
    }

    return result;
}

bool obs_client_is_connected(obs_client_handle_t client)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;
    return (client->state == OBS_CLIENT_STATE_CONNECTED);
}

esp_err_t obs_client_register_events(obs_client_handle_t client,
                                     obs_event_id_t event,
                                     esp_event_handler_t event_handler,
                                     void *event_handler_arg)
{
    if (client == NULL)
        return ESP_ERR_INVALID_ARG;
    return esp_event_handler_register_with(client->event_handle, OBS_EVENTS, event, event_handler, event_handler_arg);
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

    payload = cJSON_Print(json);

    ESP_LOGI(TAG, "Send JSON request message %s", payload);

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