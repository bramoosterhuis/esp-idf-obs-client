#ifndef _OBS_TYPES_H_
#define _OBS_TYPES_H_

#include <time.h>
#include <stdint.h>

typedef void *obs_message_payload_t;

typedef enum
{
    LEFT = 0x01,
    RIGHT = 0x02,
    TOP = 0x04,
    BOTTOM = 0x08
} obs_alignment_t;

typedef enum
{
    UNKNOWN = -1,
    INPUT = 0,
    FILTER,
    TRANSITION,
    SCENE,
} obs_type_t;

typedef enum
{
    OBS_BOUNDS_STRETCH = 0x00,
    OBS_BOUNDS_SCALE_INNER,
    OBS_BOUNDS_SCALE_OUTER,
    OBS_BOUNDS_SCALE_TO_WIDTH,
    OBS_BOUNDS_SCALE_TO_HEIGHT,
    OBS_BOUNDS_MAX_ONLY,
    OBS_BOUNDS_NONE
} obs_bound_type_t;

typedef struct obs_scene_item_t
{
    int cy;
    int cx;
    obs_alignment_t alignment;
    const char *name;
    int id;
    bool render;
    bool muted;
    bool locked;
    int source_cx;
    int source_cy;
    obs_type_t type;
    int volume;
    int x;
    int y;
    const char *parent_group_name;
    uint8_t n_group_children;
    /* obs_scene_item_t[] */
    uint8_t *group_children;
} obs_scene_item_t;

typedef struct obs_scene_item_transform_t
{
    double position_x;
    double position_y;
    obs_alignment_t alignment;
    double rotation;
    double scale_x;
    double scale_y;
    int crop_x;
    int crop_y;
    int crop_top;
    int crop_right;
    int crop_bottom;
    int crop_left;
    bool visible;
    bool locked;
    obs_bound_type_t bound_type;
    int source_width;
    int source_height;
    double width;
    double heigth;
    const char *parent_group_name;
    uint8_t n_group_children;
    /* obs_scene_item_transform_t[] */
    uint8_t *group_children;
} obs_scene_item_transform_t;

typedef struct obs_stat_t
{
    double fps;
    int render_total_frames;
    int render_missed_frames;
    int output_total_frames;
    int output_skipped_frames;
    double average_frame_time;
    double cpu_usage;
    double memory_usage;
    double free_disk_space;
} obs_stat_t;

typedef struct obs_output_t
{
    const char *name;
    const char *type;
    int width;
    int heigth;
    // flags;
    int flags_raw;
    bool flags_audio;
    bool flags_video;
    bool flags_encoded;
    bool flags_multi_track;
    bool flags_service;
    // settings;
    bool active;
    bool reconnecting;
    double congestion;
    int total_frames;
    int dropped_frames;
    int total_bytes;

} obs_output_t;

typedef struct obs_scene_t
{
    const char *name;
    uint8_t n_sources;
    obs_scene_item_t **sources;
} obs_scene_t;

typedef struct obs_tm
{
    int tm_min;  /* minutes */
    int tm_hour; /* hours */
    int tm_sec;  /* seconds */
    int tm_mses; /* milliseconds */
} obs_tm;

typedef struct timeval *obs_time_handle_t;

#endif