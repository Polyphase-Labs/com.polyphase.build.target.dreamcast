#pragma once

#include "disc_image.h"

struct _cd_track_t {
    cd_session_t* session;

    track_type_t type;
    track_mode_t mode;

    uint8_t* data;
    size_t data_len;

    size_t pregap_sectors;
    size_t postgap_sectors;

    uint32_t start_lba;

    cd_track_t* next_track;
};

struct _cd_session_t {
    cd_image_t* image;
    cd_track_t* first_track;
    cd_session_t* next_session;
};

struct _cd_image_t {
    char volume_name[255];
    cd_session_t* first_session;
};
