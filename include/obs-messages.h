#ifndef _OBS_MESSAGES_H_
#define _OBS_MESSAGES_H_

#include <cJSON.h>

#include "obs-types.h"
#include "obs-messages-events.h"
#include "obs-messages-requests.h"

#define GENERATE_EVENT_ENUM(ENUM) ObsEvent##ENUM,
#define GENERATE_REQUEST_ENUM(ENUM) ObsRequest##ENUM,
#define GENERATE_STRING(STRING) #STRING,

typedef enum
{
    FOREACH_EVENT(GENERATE_EVENT_ENUM)
        OBS_EVENT_TYPE_END
} obs_event_message_type_t;

static const char *obs_event_message_type_string[] = {
    FOREACH_EVENT(GENERATE_STRING)};

typedef enum
{
    FOREACH_REQUEST(GENERATE_REQUEST_ENUM)
        OBS_REQUEST_TYPE_END
} obs_request_message_type_t;

static const char *obs_request_message_type_string[] = {
    FOREACH_REQUEST(GENERATE_STRING)};

typedef enum
{
    REQUEST = 0x01,
    EVENT = 0x02,
    RESONSE = 0x04
} obs_message_type_t;

typedef enum
{
    OBS_RESULT_UNKNOWN = 0xFF,
    OBS_RESULT_OK = 0x00,
    OBS_RESULT_ERROR = 0x01,
} obs_request_result_t;

typedef struct obs_base_message_t
{
    obs_message_type_t type;
    obs_message_payload_t payload;
} obs_base_message_t;

// Events are broadcast by the server to each connected client when a recognized action occurs within OBS.
//
// An event message will contain at least the following base fields:
//
//     update-type String: the type of event.
//     stream-timecode String (optional): time elapsed between now and stream start (only present if OBS Studio is streaming).
//     rec-timecode String (optional): time elapsed between now and recording start (only present if OBS Studio is recording).
//
// Timecodes are sent using the format: HH:MM:SS.mmm
//
// Additional fields may be present in the event message depending on the event type.
typedef struct obs_event_message_t
{
    obs_event_message_type_t type;
    obs_time_handle_t rec_timecode;
    obs_time_handle_t stream_timecode;
    cJSON *body;
} obs_event_message_t;

// Requests
//
// Requests are sent by the client and require at least the following two fields:
//
//     request-type String: String name of the request type.
//     message-id String: Client defined identifier for the message, will be echoed in the response.
//
// Once a request is sent, the server will return a JSON response with at least the following fields:
//
//     message-id String: The client defined identifier specified in the request.
//     status String: Response status, will be one of the following: ok, error
//     error String (Optional): An error message accompanying an error status.
//
// Additional information may be required/returned depending on the request type. See below for more information.
typedef struct obs_request_message_t
{
    obs_request_message_type_t type;
    cJSON *body;
} obs_request_message_t;

typedef struct obs_response_message_t
{
    uint32_t id;
    obs_request_result_t result;
    const char *error_message;
    cJSON *body;
} obs_response_message_t;
#endif

inline static obs_event_message_type_t obs_get_event_type_from_string(const char *type)
{
    obs_event_message_type_t result = OBS_EVENT_TYPE_END;

    for (uint8_t i = 0; i < OBS_EVENT_TYPE_END; i++)
    {
        if (
            (strlen(type) == strlen(obs_event_message_type_string[i])) &&
            (strcmp(type, obs_event_message_type_string[i]) == 0))
        {
            result = (obs_event_message_type_t)i;
        }
    }

    return result;
}

inline static obs_request_message_type_t obs_get_request_type_from_string(const char *type)
{
    obs_request_message_type_t result = OBS_REQUEST_TYPE_END;

    for (uint8_t i = 0; i < OBS_REQUEST_TYPE_END; i++)
    {
        if (
            (strlen(type) == strlen(obs_request_message_type_string[i])) &&
            (strcmp(type, obs_request_message_type_string[i]) == 0))
        {
            result = (obs_request_message_type_t)i;
        }
    }

    return result;
}