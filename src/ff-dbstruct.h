/*
 * $Id: $
 *
 * Database structure and fields
 */

#ifndef _FF_DBSTRUCT_H_
#define _FF_DBSTRUCT_H_

#define PL_ID               0
#define PL_TITLE            1
#define PL_TYPE             2
#define PL_ITEMS            3
#define PL_QUERY            4
#define PL_DB_TIMESTAMP     5
#define PL_PATH             6
#define PL_IDX              7
#define PL_LAST             8

#define SG_ID               0
#define SG_PATH             1
#define SG_FNAME            2
#define SG_TITLE            3
#define SG_ARTIST           4
#define SG_ALBUM            5
#define SG_GENRE            6
#define SG_COMMENT          7
#define SG_TYPE             8
#define SG_COMPOSER         9
#define SG_ORCHESTRA       10
#define SG_CONDUCTOR       11
#define SG_GROUPING        12
#define SG_URL             13
#define SG_BITRATE         14
#define SG_SAMPLERATE      15
#define SG_SONG_LENGTH     16
#define SG_FILE_SIZE       17
#define SG_YEAR            18
#define SG_TRACK           19
#define SG_TOTAL_TRACKS    20
#define SG_DISC            21
#define SG_TOTAL_DISCS     22
#define SG_BPM             23
#define SG_COMPILATION     24
#define SG_RATING          25
#define SG_PLAY_COUNT      26
#define SG_DATA_KIND       27
#define SG_ITEM_KIND       28
#define SG_DESCRIPTION     29
#define SG_TIME_ADDED      30
#define SG_TIME_MODIFIED   31
#define SG_TIME_PLAYED     32
#define SG_DISABLED        33
#define SG_SAMPLE_COUNT    34
#define SG_CODECTYPE       35
#define SG_IDX             36
#define SG_HAS_VIDEO       37
#define SG_CONTENTRATING   38
#define SG_BITS_PER_SAMPLE 39
#define SG_ALBUM_ARTIST    40
#define SG_LAST            41  /* semaphore */

/* Packed and unpacked formats */
typedef struct tag_mp3file {
    char *path;
    uint32_t idx;
    char *fname;
    char *title;     /* TIT2 */
    char *artist;    /* TPE1 */
    char *album;     /* TALB */
    char *genre;     /* TCON */
    char *comment;   /* COMM */
    char *type;
    char *composer;  /* TCOM */
    char *orchestra; /* TPE2 */
    char *conductor; /* TPE3 */
    char *grouping;  /* TIT1 */
    char *url;       /* daap.songdataurl (asul) */

    uint32_t bitrate;
    uint32_t samplerate;
    uint32_t song_length;
    uint64_t file_size; /* ?? */
    uint32_t year;        /* TDRC */

    uint32_t track;       /* TRCK */
    uint32_t total_tracks;

    uint32_t disc;        /* TPOS */
    uint32_t total_discs;

    uint32_t time_added; /* really should be a time_t */
    uint32_t time_modified;
    uint32_t time_played;

    uint32_t play_count;
    uint32_t rating;

    uint32_t disabled;
    uint32_t bpm;         /* TBPM */

    uint32_t id;

    char *description;  /* long file type */
    char *codectype;          /* song.codectype */

    uint32_t item_kind;              /* song or movie */
    uint32_t data_kind;              /* dmap.datakind (asdk) */
    uint64_t sample_count;
    uint32_t compilation;

    /* iTunes 5+ */
    uint32_t contentrating;

    /* iTunes 6.0.2 */
    uint32_t has_video;
    uint32_t bits_per_sample;

    char *album_artist;
} MP3FILE,MEDIA_NATIVE;

typedef struct tag_m3ufile {
    uint32_t id;          /**< integer id (miid) */
    char *title;          /**< playlist name as displayed in iTunes (minm) */
    uint32_t type;        /**< see PL_ types */
    uint32_t items;       /**< number of items (mimc) */
    char *query;          /**< where clause if type 1 (MSPS) */
    uint32_t db_timestamp;/**< time last updated */
    char *path;           /**< path of underlying playlist (if type 2) */
    uint32_t idx;         /**< index of playlist for paths with multiple playlists */
} M3UFILE, PLAYLIST_NATIVE;

typedef struct tag_packed_m3ufile {
    char *id;
    char *title;
    char *type;
    char *items;
    char *query;
    char *db_timestamp;
    char *path;
    char *idx;
} PACKED_M3UFILE,PLAYLIST_STRING;

typedef struct tag_packed_mp3file {
    char *id;
    char *path;
    char *fname;
    char *title;
    char *artist;
    char *album;             /* 05 */
    char *genre;
    char *comment;
    char *type;
    char *composer;
    char *orchestra;         /* 10 */
    char *conductor;
    char *grouping;
    char *url;
    char *bitrate;
    char *samplerate;        /* 15 */
    char *song_length;
    char *file_size;
    char *year;
    char *track;
    char *total_tracks;      /* 20 */
    char *disc;
    char *total_discs;
    char *bpm;
    char *compilation;
    char *rating;            /* 25 */
    char *play_count;
    char *data_kind;
    char *item_kind;
    char *description;
    char *time_added;        /* 30 */
    char *time_modified;
    char *time_played;
    char *disabled;
    char *sample_count;
    char *codectype;         /* 35 */
    char *idx;
    char *has_video;
    char *contentrating;
    char *bits_per_sample;
    char *album_artist;      /* 40 */
} PACKED_MP3FILE, MEDIA_STRING;

#define QUERY_TYPE_ITEMS     0
#define QUERY_TYPE_PLAYLISTS 1
#define QUERY_TYPE_DISTINCT  2

#define FILTER_TYPE_FIREFLY  0
#define FILTER_TYPE_APPLE    1
#define FILTER_TYPE_NONE     2

/* query info for db enums */

#define QUERY_TYPE_ITEMS     0
#define QUERY_TYPE_PLAYLISTS 1
#define QUERY_TYPE_DISTINCT  2

typedef struct tag_db_query { /* this will get union'ed */
    /* all */
    int query_type;
    int offset;
    int limit;
    uint32_t playlist_id;       /* for items query */
    int totalcount;             /* returned total count */
    void *priv;                 /* implementation private storage */

    /* distincts */
    int distinct_field;   /**< field to distinct on */

    /* items */
    int filter_type;
    char *filter;
} DB_QUERY;

#define FT_INT32         0
#define FT_INT64         1
#define FT_STRING        2

typedef struct field_lookup_t {
    char *name;
    char *dmap_name;
    int type;
    int offset;
} FIELD_LOOKUP;

extern FIELD_LOOKUP ff_field_data[];

#define PL_STATIC     0x10
#define PL_DYNAMIC    0x20
#define PL_HIDDEN     0x40

#define PL_STATICWEB  0x11
#define PL_STATICFILE 0x12
#define PL_STATICXML  0x13
#define PL_SMART      0x24
#define PL_FOLDER     0x05

/* DB hints */
#define DB_HINT_PRESCAN_START  1
#define DB_HINT_PRESCAN_END    2
#define DB_HINT_FULLSCAN_START 3
#define DB_HINT_FULLSCAN_END   4

#endif /* _FF_DBSTRUCT_H_ */
