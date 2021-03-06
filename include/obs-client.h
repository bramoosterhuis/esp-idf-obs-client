#ifndef _OBSCLIENT_H_
#define _OBSCLIENT_H_

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_websocket_client.h"

#include <cJSON.h>

#include "obs-messages.h"

#ifdef __cplusplus
extern "C"
{
#endif
  typedef struct obs_client *obs_client_handle_t;

  ESP_EVENT_DECLARE_BASE(OBS_EVENTS); // declaration of the task events family

  /**
 * @brief OBS Client events id
 */
  typedef enum obs_event_id_t
  {
    OBS_EVENT_ANY = -1,
    OBS_EVENT_MESSAGE_RESONSE = 0,
    OBS_EVENT_MESSAGE_EVENT,
    OBS_EVENT_CLIENT_NOT_CONFIGURED,
    OBS_EVENT_CLIENT_CONNECTED,
    OBS_EVENT_CLIENT_DISCONNECTED,
    OBS_EVENT_CLIENT_ERROR,
    OBS_EVENT_CLIENT_INACTIVE_WARNING,
    OBS_EVENT_MAX
  } obs_event_id_t;

  /**
 * @brief OBS client setup configuration
 */
  typedef struct obs_client_config_t
  {
    const char *host;            /*!< Domain or IP as string */
    int port;                    /*!< Port to connect */
    const char *password;        /*!< Using for Http authentication */
    int connection_timeout_ms;   /*!< Time out for port related matters */
    bool disable_auto_reconnect; /*!< Do not reconect automatically. */
    int inactivity_timeout_ms;   /*!< Time out for port related matters */
  } obs_client_config_t;

  /**
 * @brief OBS event data
 */
  typedef struct obs_event_data_t
  {
    obs_client_handle_t client; /*!< esp_websocket_client_handle_t context */
    const void *data_ptr;       /*!< Data pointer */
    int data_len;               /*!< Data length */
  } obs_event_data_t;

  /**
 * @brief      Creates a empty OBS client
 *             This function must be the first function to call,
 *             and it returns a obs_client_handle_t that you must use as input to other functions in the interface.
 *             This call MUST have a corresponding call to obs_client_destroy when the operation is complete.
 * 
 * @return
 *     - `obs_client_handle_t`
 *     - NULL if any errors
 */
  obs_client_handle_t obs_client_create();

  /**
 * @brief      Destroy the OBS connection and free all resources.
 *             This function must be the last function to call for an session.
 *             It is the opposite of the obs_client_init function and must be called with the same handle as input that a obs_client_init call returned.
 *             This might close all connections this handle has used.
 *
 *  Notes:
 *  - Cannot be called from the websocket event handler
 * 
 * @param[in]  client  The client
 *
 * @return     esp_err_t
 */
  esp_err_t obs_client_destroy(obs_client_handle_t client);

  /**
 * @brief Returns the current config used by the OBS client
 *
 * @param[in]  client            The client handle
 * @param[out] config            The current config
 * @return esp_err_t
 */
  esp_err_t obs_client_get_config(obs_client_handle_t client,
                                  obs_client_config_t *config);

  /**
 * @brief Sets a new config to us for the OBS client
 *
 * @param[in]  client            The client handle
 * @param[in]  config            The new config
 * @return esp_err_t
 */
  esp_err_t obs_client_set_config(obs_client_handle_t client,
                                  const obs_client_config_t *config);

  /**
 * @brief      Open the OBS connection
 *
 * @param[in]  client  The client
 *
 * @return     esp_err_t
 */
  esp_err_t obs_client_start(obs_client_handle_t client);

  /**
 * @brief      Stops the OBS connection without websocket closing handshake
 *
 * This API stops ws client and closes TCP connection directly without sending
 * close frames. It is a good practice to close the connection in a clean way
 * using obs_client_close().
 *
 *  Notes:
 *  - Cannot be called from the websocket event handler 
 *
 * @param[in]  client  The client
 *
 * @return     esp_err_t
 */
  esp_err_t obs_client_stop(obs_client_handle_t client);

  /**
 * @brief      Check the OBS client connection state
 *
 * @param[in]  client  The client handle
 *
 * @return
 *     - true
 *     - false
 */
  bool obs_client_is_connected(obs_client_handle_t client);

  /**
 * @brief Register the OBS Events
 *
 * @param[in] client            The client handle
 * @param[in] event             The event id
 * @param[in] event_handler     The callback function
 * @param[in] event_handler_arg User context
 * @return esp_err_t
 */
  esp_err_t obs_client_register_events(obs_client_handle_t client,
                                       obs_event_id_t event,
                                       esp_event_handler_t event_handler,
                                       void *event_handler_arg);

  /**
 * @brief Unregister the OBS Events
 *
 * @param[in] client            The client handle
 * @param[in] event             The event id
 * @param[in] event_handler     The callback function
 * @return esp_err_t
 */
  esp_err_t obs_client_unregister_events(obs_client_handle_t client,
                                         obs_event_id_t event,
                                         esp_event_handler_t event_handler);

  /**
 * @brief Send a OBS Request
 *
 * @param[in] client            The client handle
 * @param[in] message           The message  
 * @param[in] message_id           The message id of the result message to be received.
 * @return esp_err_t
 */
  esp_err_t obs_client_send_request(obs_client_handle_t client,
                                    obs_request_message_t message,
                                    uint32_t *message_id);

  /**
 * @brief Persist the current config of the client. 
 *
 * @param[in] client            The client handle
 * @return esp_err_t
 */
  esp_err_t obs_client_persist_config(obs_client_handle_t client);

  /**
 * @brief Clears the persisted the config of the client. 
 *
 * @param[in] client            The client handle
 * @return esp_err_t
 */
  esp_err_t obs_client_clear_config(obs_client_handle_t client);

#ifdef __cplusplus
}
#endif

#endif //_OBSCLIENT_H_