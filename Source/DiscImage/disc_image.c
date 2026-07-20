#include <stdlib.h>
#include <string.h>

#include "disc_image.h"
#include "private.h"

cd_image_t* cd_new_image() {
    cd_image_t* img = (cd_image_t*) malloc(sizeof(cd_image_t));
    img->first_session = NULL;
    strncpy(img->volume_name, "UNNAMED", 255);
    return img;
}

void cd_image_set_volume_name(cd_image_t* img, const char* name) {
    strncpy(img->volume_name, name, 255);
}

const char* cd_image_volume_name(const cd_image_t* img) {
    return img->volume_name;
}

static void destroy_track(cd_track_t** t) {
    free((*t)->data);
    free(*t);
    t = NULL;
}

static void destroy_session(cd_session_t** s) {
    free(*s);
    s = NULL;
}

void cd_free_image(cd_image_t** img) {
    cd_session_t* s = (*img)->first_session;
    while(s) {
        cd_track_t* t = s->first_track;
        s->first_track = NULL;

        while(t) {
            cd_track_t* nt = t->next_track;
            destroy_track(&t);
            t = nt;
        }

        cd_session_t* ns = s->next_session;
        destroy_session(&s);
        s = ns;
    }

    free(*img);
    *img = NULL;
}

size_t cd_image_session_count(const cd_image_t* img) {
    size_t c = 0;
    cd_session_t* s = img->first_session;

    while(s) {
        c++;
        s = s->next_session;
    }

    return c;
}

size_t cd_image_length_in_sectors(const cd_image_t* img) {
    cd_session_t* s = img->first_session;
    size_t total = 0;
    while(s) {
        total += cd_session_length_in_sectors(s);
        s = s->next_session;
    }

    return total;
}

size_t cd_session_length_in_sectors(const cd_session_t* session) {
    bool is_first_session = session->image->first_session == session;
    const size_t LEAD_IN_SIZE = 4500;
    const size_t LEAD_OUT_SIZE = (is_first_session) ? 6750 : 2250;

    size_t total = LEAD_IN_SIZE + LEAD_OUT_SIZE;

    cd_track_t* t = session->first_track;
    while(t) {
        size_t track_sectors = cd_track_data_size_in_sectors(t);

        total += t->pregap_sectors;
        total += track_sectors;
        total += t->postgap_sectors;

        t = t->next_track;
    }

    return total;
}

size_t cd_session_data_length_in_sectors(const cd_session_t* session) {
    size_t total = 0;

    cd_track_t* t = session->first_track;
    while(t) {
        size_t track_sectors = cd_track_data_size_in_sectors(t);
        total += t->pregap_sectors;
        total += track_sectors;
        total += t->postgap_sectors;
        t = t->next_track;
    }

    return total;
}

cd_session_t* cd_image_get_session(const cd_image_t* img, size_t idx) {
    cd_session_t* found = img->first_session;

    while(idx > 0) {
        if(found) {
            found = found->next_session;
        }

        --idx;
    }

    return found;
}

size_t cd_image_track_count(const cd_image_t* img) {
    size_t c = 0;
    cd_session_t* s = img->first_session;

    while(s) {
        c += cd_session_track_count(s);
        s = s->next_session;
    }

    return c;
}

cd_session_t* cd_new_session(cd_image_t* img) {
    cd_session_t* s = (cd_session_t*) malloc(sizeof(cd_session_t));

    s->image = img;
    s->next_session = NULL;
    s->first_track = NULL;

    cd_session_t* it = img->first_session;
    if(!it) {
        img->first_session = s;
    } else {
        while(it->next_session) {
            it = it->next_session;
        }

        it->next_session = s;
    }
    return s;
}

size_t cd_session_track_count(const cd_session_t* session) {
    cd_track_t* t = session->first_track;
    size_t c = 0;
    while(t) {
        ++c;
        t = t->next_track;
    }
    return c;
}

cd_track_t* cd_session_get_track(const cd_session_t* session, size_t idx) {
    cd_track_t* found = session->first_track;

    while(idx > 0) {
        if(found) {
            found = found->next_track;
        }

        --idx;
    }

    return found;
}

cd_track_t* cd_new_track(cd_session_t* session, track_type_t type, const uint8_t* data, const uint32_t data_len) {
    cd_track_t* t = (cd_track_t*) malloc(sizeof(cd_track_t));
    memset(t, 0, sizeof(cd_track_t));

    t->session = session;
    t->type = type;
    t->mode = (type == TRACK_TYPE_AUDIO) ? TRACK_MODE_CDDA : TRACK_MODE_XA_MODE2_FORM1;
    t->data_len = data_len;
    t->data = malloc(data_len);
    t->start_lba = 0;
    t->pregap_sectors = 150;
    t->postgap_sectors = 0;

    memcpy(t->data, data, data_len);

    cd_track_t* i = session->first_track;
    if(!i) {
        session->first_track = t;
    } else {
        while(i->next_track) {
            i = i->next_track;
        }

        i->next_track = t;
    }
    return t;
}

cd_track_t* cd_new_track_blank(cd_session_t* session, track_type_t type, const uint32_t data_len) {
    /* mkdcdisc used a C99 VLA here; MSVC rejects VLAs, so use a heap buffer. */
    uint8_t* data = (uint8_t*) calloc(data_len, 1);
    cd_track_t* track = cd_new_track(session, type, data, data_len);
    free(data);
    return track;
}

track_type_t cd_track_type(const cd_track_t* track) {
    return track->type;
}

uint8_t* cd_track_data(const cd_track_t* track) {
    return track->data;
}

size_t cd_track_data_size_in_bytes(const cd_track_t* track) {
    return track->data_len;
}

size_t cd_track_total_size_in_sectors(const cd_track_t* track) {
    return track->pregap_sectors + track->postgap_sectors + cd_track_data_size_in_sectors(track);
}

track_mode_t cd_track_mode(const cd_track_t* track) {
    return track->mode;
}

void cd_track_set_mode(cd_track_t* track, track_mode_t mode) {
    track->mode = mode;
}

void cd_track_set_start_lba(cd_track_t* track, uint32_t lba) {
    track->start_lba = lba;
}

uint32_t cd_track_start_lba(const cd_track_t* track) {
    cd_image_t* img = track->session->image;
    cd_session_t* s = img->first_session;

    size_t lba = 0;

    while(s && s != track->session) {
        lba += cd_session_length_in_sectors(s);
        s = s->next_session;
    }

    cd_track_t* t = track->session->first_track;
    while(t && t != track) {
        lba += cd_track_data_size_in_sectors(t) + t->pregap_sectors + t->postgap_sectors;
        t = t->next_track;
    }

    return lba;
}

size_t cd_track_pregap_sectors(const cd_track_t* track) {
    return track->pregap_sectors;
}

size_t cd_track_postgap_sectors(const cd_track_t* track) {
    return track->postgap_sectors;
}

size_t cd_track_data_size_in_sectors(const cd_track_t* track) {
    track_mode_t mode = cd_track_mode(track);
    size_t sector_user_data = (mode == TRACK_MODE_CDDA) ? 2352 :
        (mode == TRACK_MODE_MODE1) ? 2048 :
        (mode == TRACK_MODE_MODE2) ? 2336 :
        (mode == TRACK_MODE_XA_MODE2_FORM1) ? 2048 : 2324;

    size_t track_sectors = track->data_len / sector_user_data;
    if(track->data_len % sector_user_data) {
        track_sectors++;
    }

    return track_sectors;
}

bool cd_track_set_pregap_sectors(cd_track_t* track, size_t pregap) {
    track->pregap_sectors = pregap;
    return true;
}

bool cd_track_set_postgap_sectors(cd_track_t* track, size_t postgap) {
    track->postgap_sectors = postgap;
    return true;
}
