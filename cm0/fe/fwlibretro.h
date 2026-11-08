// fwlibretro.h - the part of the libretro API (v1) that cm0/fe/fwmame.c uses.
// Values and layouts follow libretro.h from libretro-common (MIT); only what
// a minimal frontend for MAME 2003-Plus needs is here.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RETRO_API_VERSION 1

#define RETRO_DEVICE_JOYPAD 1
enum {
    RETRO_DEVICE_ID_JOYPAD_B = 0, RETRO_DEVICE_ID_JOYPAD_Y = 1, RETRO_DEVICE_ID_JOYPAD_SELECT = 2,
    RETRO_DEVICE_ID_JOYPAD_START = 3, RETRO_DEVICE_ID_JOYPAD_UP = 4, RETRO_DEVICE_ID_JOYPAD_DOWN = 5,
    RETRO_DEVICE_ID_JOYPAD_LEFT = 6, RETRO_DEVICE_ID_JOYPAD_RIGHT = 7, RETRO_DEVICE_ID_JOYPAD_A = 8,
    RETRO_DEVICE_ID_JOYPAD_X = 9, RETRO_DEVICE_ID_JOYPAD_L = 10, RETRO_DEVICE_ID_JOYPAD_R = 11,
    RETRO_DEVICE_ID_JOYPAD_L2 = 12, RETRO_DEVICE_ID_JOYPAD_R2 = 13, RETRO_DEVICE_ID_JOYPAD_L3 = 14,
    RETRO_DEVICE_ID_JOYPAD_R3 = 15, RETRO_DEVICE_ID_JOYPAD_MASK = 256
};

#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000
enum {
    RETRO_ENVIRONMENT_SET_ROTATION = 1,
    RETRO_ENVIRONMENT_GET_OVERSCAN = 2,
    RETRO_ENVIRONMENT_GET_CAN_DUPE = 3,
    RETRO_ENVIRONMENT_SET_MESSAGE = 6,
    RETRO_ENVIRONMENT_SHUTDOWN = 7,
    RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL = 8,
    RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY = 9,
    RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10,
    RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS = 11,
    RETRO_ENVIRONMENT_GET_VARIABLE = 15,
    RETRO_ENVIRONMENT_SET_VARIABLES = 16,
    RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE = 17,
    RETRO_ENVIRONMENT_GET_LOG_INTERFACE = 27,
    RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY = 31,
    RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO = 32,
    RETRO_ENVIRONMENT_SET_CONTROLLER_INFO = 35,
    RETRO_ENVIRONMENT_SET_GEOMETRY = 37,
    RETRO_ENVIRONMENT_GET_INPUT_BITMASKS = 51 | RETRO_ENVIRONMENT_EXPERIMENTAL,
    RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION = 52,
};

enum retro_pixel_format { RETRO_PIXEL_FORMAT_0RGB1555 = 0, RETRO_PIXEL_FORMAT_XRGB8888 = 1, RETRO_PIXEL_FORMAT_RGB565 = 2 };
enum retro_log_level { RETRO_LOG_DEBUG = 0, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR };

struct retro_message { const char *msg; unsigned frames; };
struct retro_variable { const char *key; const char *value; };
typedef void (*retro_log_printf_t)(enum retro_log_level level, const char *fmt, ...);
struct retro_log_callback { retro_log_printf_t log; };

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool need_fullpath;
    bool block_extract;
};
struct retro_game_geometry {
    unsigned base_width, base_height, max_width, max_height;
    float aspect_ratio;
};
struct retro_system_timing { double fps, sample_rate; };
struct retro_system_av_info { struct retro_game_geometry geometry; struct retro_system_timing timing; };
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };

typedef bool (*retro_environment_t)(unsigned cmd, void *data);
typedef void (*retro_video_refresh_t)(const void *data, unsigned width, unsigned height, size_t pitch);
typedef void (*retro_audio_sample_t)(int16_t left, int16_t right);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t *data, size_t frames);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device, unsigned index, unsigned id);
