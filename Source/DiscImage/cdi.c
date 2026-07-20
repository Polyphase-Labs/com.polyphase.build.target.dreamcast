#include <string.h>

#include "disc_image.h"
#include "private.h"
#include "edc/ecc.h"
#include "edc/libedc.h"


static void write_cdda_pregap(FILE* output, size_t sectors) {
    uint8_t pregap_sector[2352] = {0};
    for(int i = 0; i < sectors; ++i) {
        fwrite(pregap_sector, sizeof(pregap_sector), 1, output);
    }
}

static void write_mode2_pregap(FILE* output, size_t sectors) {
    const uint8_t pregap_header[] = {
        0x00, 0x00, 0x20, 0x00,
        0x00, 0x00, 0x20, 0x00
    };

    const uint8_t pregap_edc [] = {
        0x3F, 0x13, 0xB0, 0xBE
    };

    uint8_t pregap_sector[2336] = {0};
    memcpy(pregap_sector, pregap_header, sizeof(pregap_header));
    memcpy(
        pregap_sector + sizeof(pregap_sector) - sizeof(pregap_edc),
        pregap_edc,
        sizeof(pregap_edc)
    );

    for(int i = 0; i < sectors; ++i) {
        fwrite(pregap_sector, sizeof(pregap_sector), 1, output);
    }
}

static void write_track_cdda(const cd_track_t* track, FILE* output) {        
    write_cdda_pregap(output, track->pregap_sectors);

    size_t sectors_written = 0;

    uint8_t null_sector[2352] = {0};
    fwrite(track->data, track->data_len, 1, output);  /* Write the data raw */

    /* Pad until the end of the sector */
    size_t remainder = (track->data_len % 2352);
    if(remainder) {
        size_t padding = 2352 - remainder;
        fwrite(null_sector, padding, 1, output);
    }

    sectors_written += cd_track_data_size_in_sectors(track);

    write_cdda_pregap(output, track->postgap_sectors);
    sectors_written += track->postgap_sectors;

    if(sectors_written < 302) {
        write_cdda_pregap(output, 302 - sectors_written);
    }
}

static void write_track_xa_mode2_form1(const cd_track_t* track, FILE* output) {
    size_t sectors_written = track->pregap_sectors;
    write_mode2_pregap(output, sectors_written);

    const uint8_t SUBHEADER[8] = {
        0x00, 0x00, 0x09, 0x00,  /* Data subheader, duplicated */
        0x00, 0x00, 0x09, 0x00
    };

    uint8_t sector_with_sync[12 + 4 + 8 + 2048 + 4 + 276];
    uint8_t* sector = sector_with_sync + 12;
    uint8_t* sector_subheader = sector + 4;
    uint8_t* user_data = sector + 4 + 8;

    memcpy(sector_subheader, SUBHEADER, sizeof(SUBHEADER));

    size_t bytes_written = 0;
    for(size_t i = 0; i < track->data_len; i += 2048) {
        memcpy(user_data, track->data + i, 2048);

        do_encode_L2(sector_with_sync, MODE_2_FORM_1, sectors_written);

        fwrite(sector_subheader, 1, 8 + 2048 + 4 + 276, output);
        bytes_written += 2048;
        sectors_written++;
    }

    size_t remaining = track->data_len - bytes_written;
    if(remaining) {
        memset(user_data, 0, 2048);
        memcpy(user_data, track->data + bytes_written, remaining);
        do_encode_L2(sector_with_sync, MODE_2_FORM_1, sectors_written);
        fwrite(sector_subheader, 1, 8 + 2048 + 4 + 276, output);
        sectors_written++;
    }

    /* Write the post-gap */
    write_mode2_pregap(output, track->postgap_sectors);
}

static void write_track_xa_mode2_form2(const cd_track_t* track, FILE* output) {

}

/* The DiscJuggler descriptor structs below are written raw to disk, so their
 * layout must be byte-exact. mkdcdisc used GCC's __attribute__((packed)); use a
 * portable #pragma pack region instead so this compiles under MSVC too. */
#pragma pack(push, 1)
static void write_cdi_header(const cd_image_t* image, FILE* output, const char* filename) {

#define TRACK_MARKER {0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF}

    uint8_t session_count = cd_image_session_count(image);
    uint8_t total_tracks = cd_image_track_count(image);

    uint32_t header_offset = ftell(output) - 1;

    fwrite(&session_count, sizeof(session_count), 1, output);

    struct {
        uint8_t marker0[10];
        uint8_t marker1[10];
        uint8_t settings[3];
        uint8_t total_tracks;
        uint8_t filename_length;
        uint8_t filename[32];
        uint8_t unknown0[11];
        uint32_t unknown1;
        uint32_t unknown2;
        uint32_t unknown3;
        uint32_t max_cd_length;
        uint32_t unknown4;
    } track_header = {
        TRACK_MARKER, TRACK_MARKER,
        {0xAB, 0x0, 0x10},
        total_tracks,
        32,
        {0}, /* Fill the filename with spaces in a mo */
        {0}, 2, 0, 0x80000000, /* Unknowns */
        360000,
        0x980000
    };

    struct {
        uint8_t padding;
        uint16_t track_count;
        uint32_t unknown;
    } session_header = {0, 0, 0};

    memset(track_header.filename, 0x20, sizeof(track_header.filename));
    strncpy((char*) track_header.filename, filename, sizeof(track_header.filename));

    for(int j = 0; j < session_count; ++j) {
        cd_session_t* s = cd_image_get_session(image, j);

        session_header.track_count = cd_session_track_count(s);

        fwrite(&session_header, sizeof(session_header), 1, output);

        for(int i = 0; i < session_header.track_count; ++i) {
            fwrite(&track_header, sizeof(track_header), 1, output);

            cd_track_t* t = cd_session_get_track(s, i);

            uint32_t track_mode = (cd_track_mode(t) == TRACK_MODE_CDDA) ? 0 :
                (cd_track_mode(t) == TRACK_MODE_MODE1) ? 1 : 2;

            /*
             *    0: Mode1,        800h, 2048
                  1: Mode2,        920h, 2336
                  2: Audio,        930h, 2352
                  3: Raw+PQ,       940h, 2352+16 non-interleaved (P=only 1bit)
                  4: Raw+PQRSTUVW, 990h, 2352+96 interleaved */
            uint32_t read_mode = (cd_track_type(t) == TRACK_TYPE_AUDIO) ? 2 : 1; /* (0 = 2048, 1 = 2336, 2 = 2352)  */

            uint32_t track_length = cd_track_data_size_in_sectors(t) + cd_track_postgap_sectors(t);

            struct {
                uint16_t index_count;
                uint32_t pregap_sectors;
                uint32_t sector_count;
                uint8_t unknown0[6];
                uint32_t track_mode;
                uint32_t unknown1;
                uint32_t session_number;
                uint32_t track_number;
                uint32_t start_lba;
                uint32_t total_length;  /* Including pregap (+postgap?) */
                uint8_t unknown2[16];
                uint32_t read_mode;
                uint32_t control;
                uint8_t unknown3;
                uint32_t total_length2; /* ??? */
                uint32_t unknown4;
                uint8_t isrc_code[12];
                uint32_t isrc_valid;
                uint8_t unknown5;
                uint8_t unknown6[8];
                uint32_t unknown7;
                uint32_t unknown8;
                uint32_t unknown9;
                uint32_t unknown10;
                uint32_t audio_frequency;
                uint8_t unknown11[42];
                uint32_t unknown12;
                uint8_t unknown13[12];
                uint8_t session_type; /* ONLY if last track of a session (else 0) (0=Audio/CD-DA, 1=Mode1/CD-ROM, 2=Mode2/CD-XA) */
                uint8_t unknown14[5];
                uint8_t has_next_track;
                uint8_t unknown15;
                uint32_t unknown16; /* Address of last track in a session? */
            } track_data = {
                2,
                cd_track_pregap_sectors(t),
                track_length,
                {0},
                track_mode,
                0,
                j,
                i,
                cd_track_start_lba(t),
                track_length + cd_track_pregap_sectors(t),
                {0},
                read_mode,
                (cd_track_type(t) == TRACK_TYPE_AUDIO) ? 0 : 4,
                0,
                track_length + cd_track_pregap_sectors(t),
                0,
                {0},
                0,
                0, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 1, 0x00000080, 0x00000002, 0x00000010, 44100, {0}, 0xFFFFFFFF, {0},   // Unknowns
                (t->next_track) ? 0 : track_mode,
                {0},
                t->next_track != NULL,
                0,
                cd_track_start_lba(t) };

            /* The final 8 bytes are only written on the last track of the session! */
            int size = (t->next_track) ? sizeof(track_data) - 8 : sizeof(track_data);
            fwrite(&track_data, size, 1, output);
        }
    }

    uint32_t end_lba = 0;
    for(int k = 0; k < session_count; ++k) {
        end_lba += cd_session_length_in_sectors(cd_image_get_session(image, k));
    }

    struct {
        uint32_t total_sectors;
        uint8_t vol_id_length;
        uint8_t vol_id[32];
        uint8_t unknown0;
        uint32_t unknown1;
        uint32_t unknown2;
        uint8_t ean_13_code[13];
        uint32_t ean_code_valid;
        uint8_t cd_text_length;
        uint32_t unknown3;
        uint32_t unknown4;
        uint8_t unknown5[3];
        uint32_t image_version;
    } disc_info = {
        end_lba, 32, {0}, 0, 1, 1, {0}, 0, 0, 0, 0, {0}, 0x80000006
    };

    memset(disc_info.vol_id, 0x20, sizeof(disc_info.vol_id));
    strncpy((char*) disc_info.vol_id, filename, sizeof(disc_info.vol_id));

    session_header.track_count = 0;
    fwrite(&session_header, sizeof(session_header), 1, output);
    fwrite(&track_header, sizeof(track_header), 1, output);
    fwrite(&disc_info, sizeof(disc_info), 1, output);

    header_offset = (ftell(output) - 1) - header_offset + 4;
    fwrite(&header_offset, sizeof(header_offset), 1, output);
}
#pragma pack(pop)

bool cd_write_to_cdi(const cd_image_t* image, FILE* output, const char* filename) {
    /* First of all we need to write the track data. This apparently is
     * written in sectors, but ignoring the first 16 bytes of header on
     * data tracks, but including the subheader and error correction/detection */

    for(size_t s = 0; s < cd_image_session_count(image); ++s) {
        cd_session_t* session = cd_image_get_session(image, s);
        for(size_t t = 0; t < cd_session_track_count(session); ++t) {
            cd_track_t* track = cd_session_get_track(session, t);

            switch(cd_track_mode(track)) {
            case TRACK_MODE_CDDA:
                write_track_cdda(track, output);
            break;
            case TRACK_MODE_MODE1:

            break;
            case TRACK_MODE_MODE2:

            break;
            case TRACK_MODE_XA_MODE2_FORM1:
                write_track_xa_mode2_form1(track, output);
            break;
            case TRACK_MODE_XA_MODE2_FORM2:
                write_track_xa_mode2_form2(track, output);
            break;
            }
        }
    }

    write_cdi_header(image, output, filename);    
    return false;
}

cd_image_t* cd_load_from_cdi(FILE* input) {
    return NULL;
}
