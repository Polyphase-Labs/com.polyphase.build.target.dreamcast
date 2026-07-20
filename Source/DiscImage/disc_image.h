#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Usage
 *
 * cd_image_t* image = cd_new_image();
 * cd_session_t* session = cd_new_session(image);
 * cd_track_t* track = cd_new_track(session, TRACK_TYPE_AUDIO, audio_data, audio_len);
 *
 * FILE* f = fopen("myimage.mds", "wb");
 * cd_write_to_cdi(image, f);
 * fclose(f);
 **/

typedef enum {
    TRACK_TYPE_AUDIO,
    TRACK_TYPE_DATA
} track_type_t;

typedef enum {
    TRACK_MODE_CDDA,
    TRACK_MODE_MODE1,
    TRACK_MODE_MODE2,
    TRACK_MODE_XA_MODE2_FORM1,
    TRACK_MODE_XA_MODE2_FORM2,
} track_mode_t;

struct _cd_session_t;
struct _cd_track_t;
struct _cd_image_t;

typedef struct _cd_session_t cd_session_t;
typedef struct _cd_track_t cd_track_t;
typedef struct _cd_image_t cd_image_t;

cd_image_t* cd_new_image();
void cd_free_image(cd_image_t** img);

size_t cd_image_session_count(const cd_image_t* img);
size_t cd_image_track_count(const cd_image_t* img);
size_t cd_image_length_in_sectors(const cd_image_t* img);

cd_session_t* cd_image_get_session(const cd_image_t* img, size_t idx);

void cd_image_set_volume_name(cd_image_t* img, const char* name);
const char* cd_image_volume_name(const cd_image_t* img);

cd_session_t* cd_new_session(cd_image_t* img);
size_t cd_session_track_count(const cd_session_t* session);
cd_track_t* cd_session_get_track(const cd_session_t* session, size_t idx);

size_t cd_track_pregap_sectors(const cd_track_t* track);
bool cd_track_set_pregap_sectors(cd_track_t* track, size_t pregap);
size_t cd_track_postgap_sectors(const cd_track_t* track);
bool cd_track_set_postgap_sectors(cd_track_t* track, size_t pregap);

/* Session length including lead in and lead out */
size_t cd_session_length_in_sectors(const cd_session_t* session);

/* Total sectors used to store data, including pregap */
size_t cd_session_data_length_in_sectors(const cd_session_t* session);

cd_track_t* cd_new_track(
    cd_session_t* session,
    track_type_t type,
    const uint8_t* data,
    const uint32_t data_len
);

cd_track_t* cd_new_track_blank(
    cd_session_t* session,
    track_type_t type,
    const uint32_t data_len
);

track_type_t cd_track_type(const cd_track_t* track);
uint8_t* cd_track_data(const cd_track_t* track);

size_t cd_track_data_size_in_bytes(const cd_track_t* track);
size_t cd_track_data_size_in_sectors(const cd_track_t* track);

size_t cd_track_total_size_in_sectors(const cd_track_t* track);

track_mode_t cd_track_mode(const cd_track_t* track);
void cd_track_set_mode(cd_track_t* track, track_mode_t mode);




void cd_track_set_start_lba(cd_track_t* track, uint32_t lba);
uint32_t cd_track_start_lba(const cd_track_t* track);

bool cd_write_to_cdi(const cd_image_t* image, FILE* output, const char* filename);
bool cd_write_to_mds(const cd_image_t* image, FILE* output);

cd_image_t* cd_load_from_cdi(FILE* input);
cd_image_t* cd_load_from_mds(FILE* input);

#ifdef __cplusplus
}
#endif
