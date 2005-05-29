/*
 * $Id$
 * Implementation file for mp3 scanner and monitor
 *
 * Ironically, this now scans file types other than mp3 files,
 * but the name is the same for historical purposes, not to mention
 * the fact that sf.net makes it virtually impossible to manage a cvs
 * root reasonably.  Perhaps one day soon they will move to subversion.
 * 
 * /me crosses his fingers
 *
 * Copyright (C) 2003 Ron Pedde (ron@pedde.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#define _POSIX_PTHREAD_SEMANTICS
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <id3tag.h>
#include <limits.h>
#include <restart.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>  /* htons and friends */
#include <sys/stat.h>
#include <dirent.h>      /* why here?  For osx 10.2, of course! */

#include "daapd.h"
#include "db-generic.h"
#include "err.h"
#include "mp3-scanner.h"
#include "ssc.h"

#ifndef HAVE_STRCASESTR
# include "strcasestr.h"
#endif

/*
 * Typedefs
 */

/**
 * Struct to keep info about the information gleaned from
 * the mp3 frame header.
 */
typedef struct tag_scan_frameinfo {
    int layer;               /**< 1, 2, or 3, representing Layer I, II, and III */
    int bitrate;             /**< Bitrate in kbps (128, 64, etc) */
    int samplerate;          /**< Samplerate (e.g. 44100) */
    int stereo;              /**< Any kind of stereo.. joint, dual mono, etc */

    int frame_length;        /**< Frame length in bytes - calculated */
    int crc_protected;       /**< Is the frame crc protected? */
    int samples_per_frame;   /**< Samples per frame - calculated field */
    int padding;             /**< Whether or not there is a padding sample */
    int xing_offset;         /**< Where the xing header should be relative to end of hdr */
    int number_of_frames;    /**< Number of frames in the song */

    int frame_offset;        /**< Where this frame was found */

    double version;          /**< MPEG version (e.g. 2.0, 2.5, 1.0) */

    int is_valid;
} SCAN_FRAMEINFO;


typedef struct tag_scan_id3header {
    unsigned char id[3];
    unsigned char version[2];
    unsigned char flags;
    unsigned char size[4];
} __attribute((packed)) SCAN_ID3HEADER;

#define MAYBEFREE(a) { if((a)) free((a)); };

/*
 * Globals
 */
int scan_br_table[5][16] = {
    { 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0 }, /* MPEG1, Layer 1 */
    { 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0 },    /* MPEG1, Layer 2 */
    { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 },     /* MPEG1, Layer 3 */
    { 0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0 },    /* MPEG2/2.5, Layer 1 */
    { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0 }          /* MPEG2/2.5, Layer 2/3 */
};

int scan_sample_table[3][4] = {
    { 44100, 48000, 32000, 0 },  /* MPEG 1 */
    { 22050, 24000, 16000, 0 },  /* MPEG 2 */
    { 11025, 12000, 8000, 0 }    /* MPEG 2.5 */
};

char *scan_winamp_genre[] = {
    "Blues",              // 0
    "Classic Rock",
    "Country",
    "Dance",
    "Disco",
    "Funk",               // 5
    "Grunge",
    "Hip-Hop",
    "Jazz",
    "Metal",
    "New Age",            // 10
    "Oldies",
    "Other",
    "Pop",
    "R&B",
    "Rap",                // 15
    "Reggae",
    "Rock",
    "Techno",
    "Industrial",
    "Alternative",        // 20
    "Ska",
    "Death Metal",
    "Pranks",
    "Soundtrack",
    "Euro-Techno",        // 25
    "Ambient",
    "Trip-Hop",
    "Vocal",
    "Jazz+Funk",
    "Fusion",             // 30
    "Trance",
    "Classical",
    "Instrumental",
    "Acid",
    "House",              // 35
    "Game",
    "Sound Clip",
    "Gospel",
    "Noise",
    "AlternRock",         // 40
    "Bass",
    "Soul",
    "Punk",
    "Space",
    "Meditative",         // 45
    "Instrumental Pop",
    "Instrumental Rock",
    "Ethnic",
    "Gothic",
    "Darkwave",           // 50
    "Techno-Industrial",
    "Electronic",
    "Pop-Folk",
    "Eurodance",
    "Dream",              // 55
    "Southern Rock",
    "Comedy",
    "Cult",
    "Gangsta",
    "Top 40",             // 60
    "Christian Rap",
    "Pop/Funk",
    "Jungle",
    "Native American",
    "Cabaret",            // 65
    "New Wave",
    "Psychadelic",
    "Rave",
    "Showtunes",
    "Trailer",            // 70
    "Lo-Fi",
    "Tribal",
    "Acid Punk",
    "Acid Jazz",
    "Polka",              // 75
    "Retro",
    "Musical",
    "Rock & Roll",
    "Hard Rock",
    "Folk",               // 80
    "Folk/Rock",
    "National folk",
    "Swing",
    "Fast-fusion",
    "Bebob",              // 85
    "Latin",
    "Revival",
    "Celtic",
    "Bluegrass",
    "Avantgarde",         // 90
    "Gothic Rock",
    "Progressive Rock",
    "Psychedelic Rock",
    "Symphonic Rock",
    "Slow Rock",          // 95
    "Big Band",
    "Chorus",
    "Easy Listening",
    "Acoustic",
    "Humour",             // 100
    "Speech",
    "Chanson",
    "Opera",
    "Chamber Music",
    "Sonata",             // 105 
    "Symphony",
    "Booty Bass",
    "Primus",
    "Porn Groove",
    "Satire",             // 110
    "Slow Jam",
    "Club",
    "Tango",
    "Samba",
    "Folklore",           // 115
    "Ballad",
    "Powder Ballad",
    "Rhythmic Soul",
    "Freestyle",
    "Duet",               // 120
    "Punk Rock",
    "Drum Solo",
    "A Capella",
    "Euro-House",
    "Dance Hall",         // 125
    "Goa",
    "Drum & Bass",
    "Club House",
    "Hardcore",
    "Terror",             // 130
    "Indie",
    "BritPop",
    "NegerPunk",
    "Polsk Punk",
    "Beat",               // 135
    "Christian Gangsta",
    "Heavy Metal",
    "Black Metal",
    "Crossover",
    "Contemporary C",     // 140
    "Christian Rock",
    "Merengue",
    "Salsa",
    "Thrash Metal",
    "Anime",              // 145
    "JPop",
    "SynthPop",
    "Unknown"
};

/* 
 * Typedefs
 */

typedef struct {
    char *suffix;
    int	(*tags)(char* file, MP3FILE* pmp3);
    int	(*files)(char* file, MP3FILE* pmp3);
    char *type;         /* daap.songformat */
    char *codectype;    /* song.codectype */
    char *description;  /* daap.songdescription */
} TAGHANDLER;



/*
 * Forwards
 */
static int scan_path(char *path);
static int scan_gettags(char *file, MP3FILE *pmp3);
static int scan_get_mp3tags(char *file, MP3FILE *pmp3);
static int scan_get_aactags(char *file, MP3FILE *pmp3);
static int scan_get_nultags(char *file, MP3FILE *pmp3) { return 0; };
static int scan_get_fileinfo(char *file, MP3FILE *pmp3);
static int scan_get_mp3fileinfo(char *file, MP3FILE *pmp3);
static int scan_get_aacfileinfo(char *file, MP3FILE *pmp3);
static int scan_get_wavfileinfo(char *file, MP3FILE *pmp3);
//static int scan_get_nulfileinfo(char *file, MP3FILE *pmp3) { return 0; };
static int scan_get_urlfileinfo(char *file, MP3FILE *pmp3);

static int scan_freetags(MP3FILE *pmp3);
static void scan_static_playlist(char *path);
static void scan_music_file(char *path, struct dirent *pde, struct stat *psb);

static int scan_decode_mp3_frame(unsigned char *frame, SCAN_FRAMEINFO *pfi);


static TAGHANDLER *scan_gethandler(char *type);


/* EXTERNAL SCANNERS */

#ifdef OGGVORBIS
/** @see scan-ogg.c */
extern int scan_get_ogginfo(char *filename, MP3FILE *pmp3);
#endif

#ifdef FLAC
/** @see scan-flac.c */
extern int scan_get_flacinfo(char *filename, MP3FILE *pmp3);
#endif

/** @see scan-wma.c */
extern int scan_get_wmainfo(char *filename, MP3FILE *pmp3);

/** @see scan-aac.c */
extern int scan_get_aacinfo(char *filename, MP3FILE *pmp3);


/* playlist scanners */
/** @see scan-xml.c */
int scan_xml_playlist(char *filename);


/* For known types, I'm gong to use the "official" apple
 * daap.songformat, daap.songdescription, and daap.songcodecsubtype.
 * If I we don't have "official" ones, we can make them up the
 * way we currently are:  using extension or whatver.  
 *
 * This means that you can test to see if something is, say, an un-drmed
 * aac file by just testing for ->type "m4a", rather than checking every
 * different flavor of file extension.
 * 
 * NOTE: Although they are represented here as strings, the codectype is
 * *really* an unsigned short.  So when it gets serialized, it gets 
 * serialized as a short int. If you put something other than 3 or 4 
 * characters as your codectype, you'll see strange results.
 *
 * FIXME: url != pls -- this method of dispatching handlers based on file type
 * is completely wrong.  There needs to be a separate type that gets carried 
 * around with it, at least outside the database that says where the info 
 * CAME FROM.
 *
 * This system is broken, and won't work with something like a .cue file
 */
static TAGHANDLER taghandlers[] = {
    { "aac", scan_get_nultags, scan_get_aacinfo, "m4a", "mp4a", "AAC audio file" },
    { "mp4", scan_get_nultags, scan_get_aacinfo, "m4a", "mp4a", "AAC audio file" },
    { "m4a", scan_get_nultags, scan_get_aacinfo, "m4a", "mp4a", "AAC audio file" },
    { "m4p", scan_get_nultags, scan_get_aacinfo, "m4p", "mp4a", "AAC audio file" },
    { "mp3", scan_get_mp3tags, scan_get_mp3fileinfo, "mp3", "mpeg", "MPEG audio file" },
    { "wav", scan_get_nultags, scan_get_wavfileinfo, "wav", "wav", "WAV audio file" },
    { "wma", scan_get_nultags, scan_get_wmainfo, "wma", "wma", "WMA audio file" },
    { "url", scan_get_nultags, scan_get_urlfileinfo, "pls", NULL, "Playlist URL" },
    { "pls", scan_get_nultags, scan_get_urlfileinfo, "pls", NULL, "Playlist URL" },
#ifdef OGGVORBIS
    { "ogg", scan_get_nultags, scan_get_ogginfo, "ogg", "ogg", "Ogg Vorbis audio file" },
#endif
#ifdef FLAC
    { "flac", scan_get_nultags, scan_get_flacinfo, "flac","flac", "FLAC audio file" },
    { "fla", scan_get_nultags, scan_get_flacinfo,  "flac","flac", "FLAC audio file" },
#endif
    { NULL, NULL, NULL, NULL, NULL, NULL }
};

typedef struct tag_playlistlist {
    char *path;
    struct tag_playlistlist *next;
} PLAYLISTLIST;

static PLAYLISTLIST scan_playlistlist = { NULL, NULL };


/**
 * add a playlist to the playlistlist.  The playlistlist is a
 * list of playlists that need to be processed once the current
 * scan is done.  THIS IS NOT REENTRANT, and it meant to be 
 * called only inside the rescan loop.  
 *
 * \param path path of the playlist to add
 */
void scan_add_playlistlist(char *path) {
    PLAYLISTLIST *plist;

    DPRINTF(E_DBG,L_SCAN,"Adding %s for deferred processing.\n",path);

    plist=(PLAYLISTLIST*)malloc(sizeof(PLAYLISTLIST));
    if(!plist) {
	DPRINTF(E_FATAL,L_SCAN,"Malloc error\n");
	return;
    }

    plist->path=strdup(path);
    plist->next=scan_playlistlist.next;
    scan_playlistlist.next=plist;
}

/**
 * process the playlistlist
 *
 */
void scan_process_playlistlist(void) {
    PLAYLISTLIST *pnext;
    char *ext;

    while(scan_playlistlist.next) {
	pnext=scan_playlistlist.next;

	ext=pnext->path;
	if(strrchr(pnext->path,'.')) {
	    ext = strrchr(pnext->path,'.');
	}

	if(strcasecmp(ext,".xml") == 0) {
	    scan_xml_playlist(pnext->path);
	} else if(strcasecmp(ext,".m3u") == 0) {
	    scan_static_playlist(pnext->path);
	} else {
	    DPRINTF(E_LOG,L_SCAN,"Unknown playlist type: %s\n",ext);
	}

	free(pnext->path);
	scan_playlistlist.next=pnext->next;
	free(pnext);
    }
}



/*
 * scan_init
 *
 * This assumes the database is already initialized.
 *
 * Ideally, this would check to see if the database is empty.
 * If it is, it sets the database into bulk-import mode, and scans
 * the MP3 directory.
 *
 * If not empty, it would start a background monitor thread
 * and update files on a file-by-file basis
 */

int scan_init(char *path) {
    int err=0;

    if(db_start_scan()) 
	return -1;

    DPRINTF(E_DBG,L_SCAN,"Scanning for MP3s in %s\n",path);

    scan_playlistlist.next=NULL;
    err=scan_path(path);

    if(db_end_song_scan())
	return -1;

    scan_process_playlistlist();
    
    if(db_end_scan())
	return -1;

    return err;
}

/*
 * scan_path
 *
 * Do a brute force scan of a path, finding all the MP3 files there
 */
int scan_path(char *path) {
    DIR *current_dir;
    char de[sizeof(struct dirent) + MAXNAMLEN + 1]; /* extra for solaris */
    struct dirent *pde;
    int err;
    char relative_path[PATH_MAX];
    char mp3_path[PATH_MAX];
    struct stat sb;
    int modified_time;
    char *ext;
    MP3FILE *pmp3;

    if((current_dir=opendir(path)) == NULL) {
	DPRINTF(E_WARN,L_SCAN,"opendir: %s\n",strerror(errno));
	return -1;
    }

    while(1) {
	if(config.stop) {
	    DPRINTF(E_WARN,L_SCAN,"Stop req.  Aborting scan of %s.\n",path);
	    closedir(current_dir);
	    return 0;
	}

	pde=(struct dirent *)&de;

	err=readdir_r(current_dir,(struct dirent *)de,&pde);
	if(err == -1) {
	    DPRINTF(E_DBG,L_SCAN,"Error on readdir_r: %s\n",strerror(errno));
	    err=errno;
	    closedir(current_dir);
	    errno=err;
	    return -1;
	}

	if(!pde)
	    break;
	
	if(pde->d_name[0] == '.') /* skip hidden and directories */
	    continue;

	snprintf(relative_path,PATH_MAX,"%s/%s",path,pde->d_name);
	mp3_path[0] = '\x0';
	realpath(relative_path,mp3_path);
	DPRINTF(E_DBG,L_SCAN,"Found %s\n",relative_path);
	if(stat(mp3_path,&sb)) {
	    DPRINTF(E_WARN,L_SCAN,"Error statting: %s\n",strerror(errno));
	} else {
	    if(sb.st_mode & S_IFDIR) { /* dir -- recurse */
		DPRINTF(E_DBG,L_SCAN,"Found dir %s... recursing\n",pde->d_name);
		scan_path(mp3_path);
	    } else {
		/* process the file */
		if(strlen(pde->d_name) > 4) {
		    if((strcasecmp(".m3u",(char*)&pde->d_name[strlen(pde->d_name) - 4]) == 0) &&
		       config.process_m3u){
			/* we found an m3u file */
			scan_add_playlistlist(mp3_path);
		    } else if((strcasecmp(pde->d_name,"iTunes Music Library.xml")==0)) {
			scan_add_playlistlist(mp3_path);
		    } else if (((ext = strrchr(pde->d_name, '.')) != NULL) &&
			       (strcasestr(config.extensions, ext))) {
			/* only scan if it's been changed, or empty db */
			modified_time=sb.st_mtime;
			pmp3=db_fetch_path(mp3_path);

			if((!pmp3) || (pmp3->db_timestamp < modified_time) || 
			   (pmp3->force_update)) {
			    scan_music_file(path,pde,&sb);
			} else {
			    DPRINTF(E_DBG,L_SCAN,"Skipping file... not modified\n");
			}
			db_dispose_item(pmp3);
		    }
		}
	    }
	}
    }

    closedir(current_dir);
    return 0;
}

/*
 * scan_static_playlist
 *
 * Scan a file as a static playlist
 */

void scan_static_playlist(char *path) {
    char base_path[PATH_MAX];
    char file_path[PATH_MAX];
    char real_path[PATH_MAX];
    char linebuffer[PATH_MAX];
    int fd;
    int playlistid;
    M3UFILE *pm3u;
    MP3FILE *pmp3;
    struct stat sb;
    char *current;

    DPRINTF(E_WARN,L_SCAN|L_PL,"Processing static playlist: %s\n",path);
    if(stat(path,&sb)) {
	DPRINTF(E_WARN,L_SCAN,"Error statting %s: %s\n",path,strerror(errno));
	return;
    }

    if((current=strrchr(path,'/')) == NULL) {
	current = path;
    } else {
	current++;
    }

    /* temporarily use base_path for m3u name */
    strcpy(base_path,current);
    if((current=strrchr(base_path,'.'))) {
	*current='\x0';
    }

    pm3u = db_fetch_playlist(path,0);
    if(pm3u && (pm3u->db_timestamp > sb.st_mtime)) {
	/* already up-to-date */
	db_dispose_playlist(pm3u);
	return;
    }

    if(pm3u) 
	db_delete_playlist(pm3u->id);

    fd=open(path,O_RDONLY);
    if(fd != -1) {
	if(db_add_playlist(base_path,PL_STATICFILE,NULL,path,0,&playlistid) != DB_E_SUCCESS) {
	    DPRINTF(E_LOG,L_SCAN,"Error adding m3u playlist %s\n",path);
	    db_dispose_playlist(pm3u);
	    return;
	}
	/* now get the *real* base_path */
	strcpy(base_path,path);
	if((current=strrchr(base_path,'/'))) {
	    *(current+1) = '\x0';
	} /* else something is fubar */

	DPRINTF(E_INF,L_SCAN|L_PL,"Added playlist as id %d\n",playlistid);

	memset(linebuffer,0x00,sizeof(linebuffer));
	while(readline(fd,linebuffer,sizeof(linebuffer)) > 0) {
	    while((linebuffer[strlen(linebuffer)-1] == '\n') ||
		  (linebuffer[strlen(linebuffer)-1] == '\r')) 
		linebuffer[strlen(linebuffer)-1] = '\0';

	    if((linebuffer[0] == ';') || (linebuffer[0] == '#'))
		continue;

	    // FIXME - should chomp trailing comments

	    // otherwise, assume it is a path
	    if(linebuffer[0] == '/') {
		strcpy(file_path,linebuffer);
	    } else {
		snprintf(file_path,sizeof(file_path),"%s%s",base_path,linebuffer);
	    }

	    realpath(file_path,real_path);
	    DPRINTF(E_DBG,L_SCAN|L_PL,"Checking %s\n",real_path);

	    // might be valid, might not...
	    if((pmp3=db_fetch_path(real_path))) {
		db_add_playlist_item(playlistid,pmp3->id);
		db_dispose_item(pmp3);
	    } else {
		DPRINTF(E_WARN,L_SCAN|L_PL,"Playlist entry %s bad: %s\n",
			path,strerror(errno));
	    }
	}
	close(fd);
    }

    db_dispose_playlist(pm3u);
    DPRINTF(E_WARN,L_SCAN|L_PL,"Done processing playlist\n");
}


/*
 * scan_music_file
 *
 * scan a particular file as a music file
 */
void scan_music_file(char *path, struct dirent *pde, struct stat *psb) {
    MP3FILE mp3file;
    char mp3_path[PATH_MAX];
    char *current=NULL;
    char *type;
    TAGHANDLER *ptaghandler;
    char fdescr[50];

    snprintf(mp3_path,sizeof(mp3_path),"%s/%s",path,pde->d_name);

    /* we found an mp3 file */
    DPRINTF(E_INF,L_SCAN,"Found music file: %s\n",pde->d_name);
    
    memset((void*)&mp3file,0,sizeof(mp3file));
    mp3file.path=strdup(mp3_path);
    mp3file.fname=strdup(pde->d_name);
    if(strlen(pde->d_name) > 4) {
	type = strrchr(pde->d_name, '.') + 1;
	if(type) {
	    /* see if there is "official" format and info for it */
	    ptaghandler=scan_gethandler(type);
	    if(ptaghandler) {
		/* yup, use the official format */
		mp3file.type=strdup(ptaghandler->type);
		if(ptaghandler->description)
		    mp3file.description=strdup(ptaghandler->description);

		if(ptaghandler->codectype)
		    mp3file.codectype=strdup(ptaghandler->codectype);

		DPRINTF(E_DBG,L_SCAN,"Codec type: %s\n",mp3file.codectype);
	    } else {
		/* just dummy up songformat, codectype and description */
		mp3file.type=strdup(type);

		/* upper-case types cause some problems */
		current=mp3file.type;
		while(*current) {
		    *current=tolower(*current);
		    current++;
		}
		
		sprintf(fdescr,"%s audio file",mp3file.type);
		mp3file.description = strdup(fdescr);
		/* we'll just dodge the codectype */
	    }
	}
    }
    
    /* Do the tag lookup here */
    if(!scan_gettags(mp3file.path,&mp3file) && 
       !scan_get_fileinfo(mp3file.path,&mp3file)) {
	make_composite_tags(&mp3file);
	/* fill in the time_added.  I'm not sure of the logic in this.
	   My thinking is to use time created, but what is that?  Best
	   guess would be earliest of st_mtime and st_ctime...
	*/
	mp3file.time_added=psb->st_mtime;
	if(psb->st_ctime < mp3file.time_added)
	    mp3file.time_added=psb->st_ctime;
        mp3file.time_modified=psb->st_mtime;

	DPRINTF(E_DBG,L_SCAN," Date Added: %d\n",mp3file.time_added);

	DPRINTF(E_DBG,L_SCAN," Codec: %s\n",mp3file.codectype);

	db_add(&mp3file);
    } else {
	DPRINTF(E_WARN,L_SCAN,"Skipping %s - scan_gettags failed\n",pde->d_name);
    }
    
    scan_freetags(&mp3file);
}

/**
 * fetch the taghandler for this file type
 */
TAGHANDLER *scan_gethandler(char *type) {
    TAGHANDLER *phdl = taghandlers;

    while((phdl->suffix) && (strcasecmp(phdl->suffix,type)))
	phdl++;

    if(phdl->suffix)
	return phdl;

    return NULL;
}


/**
 * Dispatch the appropriate handler to get specific tag metainfomation
 *
 * \param file file to get tag info for
 * \param pmp3 mp3 file struct to fill info into
 */
int scan_gettags(char *file, MP3FILE *pmp3) {
    TAGHANDLER *hdl;

    /* dispatch to appropriate tag handler */
    hdl = scan_gethandler(pmp3->type);
    if(hdl && hdl->tags)
	return hdl->tags(file,pmp3);

    /* otherwise, it's a file type we don't understand yet */
    return 0;
}

/**
 * decide if a string is numeric or not... 
 *
 * @param str string to evaluate
 * @returns 1 if number, 0 otherwise
 */
int scan_is_numeric(char *str) {
    char *ptr=str;

    while(*ptr) {
	if(!isdigit(*ptr))
	    return 0;
	ptr++;
    }
    return 1;
}

int scan_get_mp3tags(char *file, MP3FILE *pmp3) {
    struct id3_file *pid3file;
    struct id3_tag *pid3tag;
    struct id3_frame *pid3frame;
    int err;
    int index;
    int used;
    unsigned char *utf8_text;
    int genre=WINAMP_GENRE_UNKNOWN;
    int have_utf8;
    int have_text;
    id3_ucs4_t const *native_text;
    char *tmp;
    int got_numeric_genre;

    pid3file=id3_file_open(file,ID3_FILE_MODE_READONLY);
    if(!pid3file) {
	DPRINTF(E_WARN,L_SCAN,"Cannot open %s\n",file);
	return -1;
    }

    pid3tag=id3_file_tag(pid3file);
    
    if(!pid3tag) {
	err=errno;
	id3_file_close(pid3file);
	errno=err;
	DPRINTF(E_WARN,L_SCAN,"Cannot get ID3 tag for %s\n",file);
	return -1;
    }

    index=0;
    while((pid3frame=id3_tag_findframe(pid3tag,"",index))) {
	used=0;
	utf8_text=NULL;
	native_text=NULL;
	have_utf8=0;
	have_text=0;

	if(!strcmp(pid3frame->id,"YTCP")) { /* for id3v2.2 */
	    pmp3->compilation = 1;
	    DPRINTF(E_DBG,L_SCAN,"Compilation: %d\n", pmp3->compilation);
	}

	if(((pid3frame->id[0] == 'T')||(strcmp(pid3frame->id,"COMM")==0)) &&
	   (id3_field_getnstrings(&pid3frame->fields[1]))) 
	    have_text=1;

	if(have_text) {
	    native_text=id3_field_getstrings(&pid3frame->fields[1],0);

	    if(native_text) {
		have_utf8=1;
		if(config.latin1_tags) {
		    utf8_text=id3_ucs4_latin1duplicate(native_text);
		} else {
		    utf8_text=id3_ucs4_utf8duplicate(native_text);
		}
		MEMNOTIFY(utf8_text);

		if(!strcmp(pid3frame->id,"TIT2")) { /* Title */
		    used=1;
		    pmp3->title = utf8_text;
		    DPRINTF(E_DBG,L_SCAN," Title: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TPE1")) {
		    used=1;
		    pmp3->artist = utf8_text;
		    DPRINTF(E_DBG,L_SCAN," Artist: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TALB")) {
		    used=1;
		    pmp3->album = utf8_text;
		    DPRINTF(E_DBG,L_SCAN," Album: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TCOM")) {
		    used=1;
		    pmp3->composer = utf8_text;
		    DPRINTF(E_DBG,L_SCAN," Composer: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TIT1")) {
		    used=1;
		    pmp3->grouping = utf8_text;
		    DPRINTF(E_DBG,L_SCAN," Grouping: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TPE2")) {
		    used=1;
		    pmp3->orchestra = utf8_text;
		    DPRINTF(E_DBG,L_SCAN," Orchestra: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TPE3")) {
		    used=1;
		    pmp3->conductor = utf8_text;
		    DPRINTF(E_DBG,L_SCAN," Conductor: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TCON")) {
		    used=1;
		    pmp3->genre = utf8_text;
		    got_numeric_genre=0;
		    DPRINTF(E_DBG,L_SCAN," Genre: %s\n",utf8_text);
		    if(pmp3->genre) {
			if(!strlen(pmp3->genre)) {
			    genre=WINAMP_GENRE_UNKNOWN;
			    got_numeric_genre=1;
			} else if (scan_is_numeric(pmp3->genre)) {
			    genre=atoi(pmp3->genre);
			    got_numeric_genre=1;
			} else if ((pmp3->genre[0] == '(') && (isdigit(pmp3->genre[1]))) {
			    genre=atoi((char*)&pmp3->genre[1]);
			    got_numeric_genre=1;
			} 

			if(got_numeric_genre) {
			    if((genre < 0) || (genre > WINAMP_GENRE_UNKNOWN))
				genre=WINAMP_GENRE_UNKNOWN;
			    free(pmp3->genre);
			    pmp3->genre=strdup(scan_winamp_genre[genre]);
			}
		    }
		} else if(!strcmp(pid3frame->id,"COMM")) {
		    used=1;
		    pmp3->comment = utf8_text;
		    DPRINTF(E_DBG,L_SCAN," Comment: %s\n",pmp3->comment);
		} else if(!strcmp(pid3frame->id,"TPOS")) {
		    tmp=(char*)utf8_text;
		    strsep(&tmp,"/");
		    if(tmp) {
			pmp3->total_discs=atoi(tmp);
		    }
		    pmp3->disc=atoi((char*)utf8_text);
		    DPRINTF(E_DBG,L_SCAN," Disc %d of %d\n",pmp3->disc,pmp3->total_discs);
		} else if(!strcmp(pid3frame->id,"TRCK")) {
		    tmp=(char*)utf8_text;
		    strsep(&tmp,"/");
		    if(tmp) {
			pmp3->total_tracks=atoi(tmp);
		    }
		    pmp3->track=atoi((char*)utf8_text);
		    DPRINTF(E_DBG,L_SCAN," Track %d of %d\n",pmp3->track,pmp3->total_tracks);
		} else if(!strcmp(pid3frame->id,"TDRC")) {
		    pmp3->year = atoi(utf8_text);
		    DPRINTF(E_DBG,L_SCAN," Year: %d\n",pmp3->year);
		} else if(!strcmp(pid3frame->id,"TLEN")) {
		    pmp3->song_length = atoi(utf8_text); /* now in ms */
		    DPRINTF(E_DBG,L_SCAN," Length: %d\n", pmp3->song_length);
		} else if(!strcmp(pid3frame->id,"TBPM")) {
		    pmp3->bpm = atoi(utf8_text);
		    DPRINTF(E_DBG,L_SCAN,"BPM: %d\n", pmp3->bpm);
		} else if(!strcmp(pid3frame->id,"TCMP")) { /* for id3v2.3 */
                    pmp3->compilation = (char)atoi(utf8_text);
                    DPRINTF(E_DBG,L_SCAN,"Compilation: %d\n", pmp3->compilation);
                }
	    }
	}

	/* can check for non-text tags here */
	if((!used) && (have_utf8) && (utf8_text))
	    free(utf8_text);

	/* v2 COMM tags are a bit different than v1 */
	if((!strcmp(pid3frame->id,"COMM")) && (pid3frame->nfields == 4)) {
	    /* Make sure it isn't a application-specific comment...
	     * This currently includes the following:
	     *
	     * iTunes_CDDB_IDs
	     * iTunNORM
	     * 
	     * If other apps stuff crap into comment fields, then we'll ignore them
	     * here.
	     */
	    native_text=id3_field_getstring(&pid3frame->fields[2]);
	    if(native_text) {
		utf8_text=id3_ucs4_utf8duplicate(native_text);
		if((utf8_text) && (strncasecmp(utf8_text,"iTun",4) != 0)) {
		    /* it's a real comment */
		    if(utf8_text)
			free(utf8_text);

		    native_text=id3_field_getfullstring(&pid3frame->fields[3]);
		    if(native_text) {
			if(pmp3->comment)
			    free(pmp3->comment);
			utf8_text=id3_ucs4_utf8duplicate(native_text);
			if(utf8_text) {
			    pmp3->comment=utf8_text;
			    MEMNOTIFY(pmp3->comment);
			}
		    }
		} else {
		    if(utf8_text)
			free(utf8_text);
		}
	    }
	}

	index++;
    }

    id3_file_close(pid3file);
    DPRINTF(E_DBG,L_SCAN,"Got id3 tag successfully\n");
    return 0;
}

/*
 * scan_freetags
 *
 * Free up the tags that were dynamically allocated
 */
int scan_freetags(MP3FILE *pmp3) {
    MAYBEFREE(pmp3->path);
    MAYBEFREE(pmp3->fname);
    MAYBEFREE(pmp3->title);
    MAYBEFREE(pmp3->artist);
    MAYBEFREE(pmp3->album);
    MAYBEFREE(pmp3->genre);
    MAYBEFREE(pmp3->comment);
    MAYBEFREE(pmp3->type);
    MAYBEFREE(pmp3->composer);
    MAYBEFREE(pmp3->orchestra);
    MAYBEFREE(pmp3->conductor);
    MAYBEFREE(pmp3->grouping);
    MAYBEFREE(pmp3->description);
    MAYBEFREE(pmp3->codectype);

    return 0;
}


/**
 * Dispatch to actual file info handlers
 *
 * \param file file to read file metainfo for
 * \param pmp3 struct to stuff with info gleaned
 */
int scan_get_fileinfo(char *file, MP3FILE *pmp3) {
    FILE *infile;
    off_t file_size;

    TAGHANDLER *hdl;

    /* dispatch to appropriate tag handler */
    hdl = scan_gethandler(pmp3->type);
    if(hdl && hdl->files)
	return hdl->files(file,pmp3);

    /* a file we don't know anything about... ogg or aiff maybe */
    if(!(infile=fopen(file,"rb"))) {
	DPRINTF(E_WARN,L_SCAN,"Could not open %s for reading\n",file);
	return -1;
    }

    /* we can at least get this */
    fseek(infile,0,SEEK_END);
    file_size=ftell(infile);
    fseek(infile,0,SEEK_SET);

    pmp3->file_size=file_size;

    fclose(infile);
    return 0;
}

/*
 * scan_get_urlfileinfo
 *
 * Get info from a "url" file -- a media stream file
 */
int scan_get_urlfileinfo(char *file, MP3FILE *pmp3) {
    FILE *infile;
    char *head, *tail;
    char linebuffer[256];

    DPRINTF(E_DBG,L_SCAN,"Getting URL file info\n");

    if(!(infile=fopen(file,"rb"))) {
	DPRINTF(E_WARN,L_SCAN,"Could not open %s for reading\n",file);
	return -1;
    }

    fgets(linebuffer,sizeof(linebuffer),infile);
    while((linebuffer[strlen(linebuffer)-1] == '\n') ||
	  (linebuffer[strlen(linebuffer)-1] == '\r')) {
	linebuffer[strlen(linebuffer)-1] = '\0';
    }

    head=linebuffer;
    tail=strchr(head,',');
    if(!tail) {
	DPRINTF(E_LOG,L_SCAN,"Badly formatted .url file - must be bitrate,descr,url\n");
	fclose(infile);
	return -1;
    }

    pmp3->bitrate=atoi(head);
    head=++tail;
    tail=strchr(head,',');
    if(!tail) {
	DPRINTF(E_LOG,L_SCAN,"Badly formatted .url file - must be bitrate,descr,url\n");
	fclose(infile);
	return -1;
    }

    *tail++='\0';
    
    pmp3->title=strdup(head);
    pmp3->url=strdup(tail);
    fclose(infile);

    DPRINTF(E_DBG,L_SCAN,"  Title:    %s\n",pmp3->title);
    DPRINTF(E_DBG,L_SCAN,"  Bitrate:  %d\n",pmp3->bitrate);
    DPRINTF(E_DBG,L_SCAN,"  URL:      %s\n",pmp3->url);

    return 0;
}

#define GET_WAV_INT32(p) ((((unsigned long)((p)[3])) << 24) |	\
		          (((unsigned long)((p)[2])) << 16) |	\
		          (((unsigned long)((p)[1])) << 8) |	\
		          (((unsigned long)((p)[0]))))

#define GET_WAV_INT16(p) ((((unsigned long)((p)[1])) << 8) |	\
		          (((unsigned long)((p)[0]))))

/*
 * scan_get_wavfileinfo
 *
 * Get info from the actual wav headers
 */
int scan_get_wavfileinfo(char *file, MP3FILE *pmp3) {
    FILE *infile;
    size_t rl;
    unsigned char hdr[44];
    unsigned long chunk_data_length;
    unsigned long format_data_length;
    unsigned long compression_code;
    unsigned long channel_count;
    unsigned long sample_rate;
    unsigned long sample_bit_length;
    unsigned long bit_rate;
    unsigned long data_length;
    unsigned long sec, ms;

    DPRINTF(E_DBG,L_SCAN,"Getting WAV file info\n");

    if(!(infile=fopen(file,"rb"))) {
	DPRINTF(E_WARN,L_SCAN,"Could not open %s for reading\n",file);
	return -1;
    }

    fseek(infile,0,SEEK_END);
    pmp3->file_size = ftell(infile);
    fseek(infile,0,SEEK_SET);

    rl = fread(hdr, 1, 44, infile);
    fclose(infile);
    if (rl != 44) {
	DPRINTF(E_WARN,L_SCAN,"Could not read wav header from %s\n",file);
        return -1;
    }

    if (strncmp(hdr + 0, "RIFF", 4) ||
	strncmp(hdr + 8, "WAVE", 4) ||
	strncmp(hdr + 12, "fmt ", 4) ||
	strncmp(hdr + 36, "data", 4)) {
	DPRINTF(E_WARN,L_SCAN,"Invalid wav header in %s\n",file);
        return -1;
    }

    chunk_data_length = GET_WAV_INT32(hdr + 4);
    format_data_length = GET_WAV_INT32(hdr + 16);
    compression_code = GET_WAV_INT16(hdr + 20);
    channel_count = GET_WAV_INT16(hdr + 22);
    sample_rate = GET_WAV_INT32(hdr + 24);
    sample_bit_length = GET_WAV_INT16(hdr + 34);
    data_length = GET_WAV_INT32(hdr + 40);

    if ((format_data_length != 16) ||
	(compression_code != 1) ||
	(channel_count < 1)) {
	DPRINTF(E_WARN,L_SCAN,"Invalid wav header in %s\n",file);
        return -1;
    }

    bit_rate = sample_rate * channel_count * ((sample_bit_length + 7) / 8) * 8;
    pmp3->bitrate = bit_rate / 1000;
    pmp3->samplerate = sample_rate;
    sec = data_length / (bit_rate / 8);
    ms = ((data_length % (bit_rate / 8)) * 1000) / (bit_rate / 8);
    pmp3->song_length = (sec * 1000) + ms;

    return 0;
}


/**
 * Decode an mp3 frame header.  Determine layer, bitrate, 
 * samplerate, etc, and fill in the passed structure.
 *
 * @param frame 4 byte mp3 frame header
 * @param pfi pointer to an allocated SCAN_FRAMEINFO struct
 * @return 0 on success (valid frame), -1 otherwise
 */
int scan_decode_mp3_frame(unsigned char *frame, SCAN_FRAMEINFO *pfi) {
    int ver;
    int layer_index;
    int sample_index;
    int bitrate_index;
    int samplerate_index;

    if((frame[0] != 0xFF) || (frame[1] < 224)) {
	pfi->is_valid=0;
	return -1;
    }

    ver=(frame[1] & 0x18) >> 3;
    pfi->layer = 4 - ((frame[1] & 0x6) >> 1);

    layer_index=-1;
    sample_index=-1;

    switch(ver) {
    case 0:
	pfi->version = 2.5;
	sample_index=2;
	if(pfi->layer == 1)
	    layer_index = 3;
	if((pfi->layer == 2) || (pfi->layer == 3))
	    layer_index = 4;
	break;
    case 2:                 
	pfi->version = 2.0;
	sample_index=1;
	if(pfi->layer == 1)
	    layer_index=3;
	if((pfi->layer == 2) || (pfi->layer == 3))
	    layer_index=4;
	break;
    case 3:
	pfi->version = 1.0;
	sample_index=0;
	if(pfi->layer == 1)
	    layer_index = 0;
	if(pfi->layer == 2)
	    layer_index = 1;
	if(pfi->layer == 3)
	    layer_index = 2;
	break;
    }

    if((layer_index < 0) || (layer_index > 4)) {
	pfi->is_valid=0;
	return -1;
    }

    if((sample_index < 0) || (sample_index > 2)) {
	pfi->is_valid=0;
	return -1;
    }

    if(pfi->layer==1) pfi->samples_per_frame=384;
    if(pfi->layer==2) pfi->samples_per_frame=1152;
    if(pfi->layer==3) {
	if(pfi->version == 1.0) {
	    pfi->samples_per_frame=1152;
	} else {
	    pfi->samples_per_frame=576;
	}
    }

    bitrate_index=(frame[2] & 0xF0) >> 4;
    samplerate_index=(frame[2] & 0x0C) >> 2;

    if((bitrate_index == 0xF) || (bitrate_index==0x0)) {
	pfi->is_valid=0;
	return -1;
    }

    if(samplerate_index == 3) {
	pfi->is_valid=0;
	return -1;
    }
	
    pfi->bitrate = scan_br_table[layer_index][bitrate_index];
    pfi->samplerate = scan_sample_table[sample_index][samplerate_index];

    if((frame[3] & 0xC0 >> 6) == 3) 
	pfi->stereo = 0;
    else
	pfi->stereo = 1;

    if(frame[2] & 0x02) { /* Padding bit set */
	pfi->padding=1;
    } else {
	pfi->padding=0;
    }

    if(pfi->version == 1.0) {
	if(pfi->stereo) {
	    pfi->xing_offset=32;
	} else {
	    pfi->xing_offset=17;
	}
    } else {
	if(pfi->stereo) {
	    pfi->xing_offset=17;
	} else {
	    pfi->xing_offset=9;
	}
    }

    pfi->crc_protected=(frame[1] & 0xFE);

    if(pfi->layer == 1) {
	pfi->frame_length = (12 * pfi->bitrate * 1000 / pfi->samplerate + pfi->padding) * 4;
    } else {
	pfi->frame_length = 144 * pfi->bitrate * 1000 / pfi->samplerate + pfi->padding;
    }

    if((pfi->frame_length > 2880) || (pfi->frame_length <= 0)) {
	pfi->is_valid=0;
	return -1;
    }

    pfi->is_valid=1;
    return 0;
}

/**
 * Scan 10 frames from the middle of the file and determine an
 * average bitrate from that.  It might not be as accurate as a full
 * frame count, but it's probably Close Enough (tm)
 *
 * @param infile file to scan for average bitrate
 * @param pfi pointer to frame info struct to put the bitrate into
 */
void scan_get_average_bitrate(FILE *infile, SCAN_FRAMEINFO *pfi) {
    off_t file_size;
    unsigned char frame_buffer[2900];
    unsigned char header[4];
    int index=0;
    int found=0;
    off_t pos;
    SCAN_FRAMEINFO fi;
    int frame_count=0;
    int bitrate_total=0;

    DPRINTF(E_DBG,L_SCAN,"Starting averaging bitrate\n");

    fseek(infile,0,SEEK_END);
    file_size=ftell(infile);

    pos=file_size/2;

    /* now, find the first frame */
    fseek(infile,pos,SEEK_SET);
    if(fread(frame_buffer,1,sizeof(frame_buffer),infile) != sizeof(frame_buffer)) 
	return;

    while(!found) {
	while((frame_buffer[index] != 0xFF) && (index < (sizeof(frame_buffer)-4)))
	    index++;

	if(index >= (sizeof(frame_buffer)-4)) { /* largest mp3 frame is 2880 bytes */
	    DPRINTF(E_DBG,L_SCAN,"Could not find frame... quitting\n");
	    return;
	}
	    
	if(!scan_decode_mp3_frame(&frame_buffer[index],&fi)) { 
	    /* see if next frame is valid */
	    fseek(infile,pos + index + fi.frame_length,SEEK_SET);
	    if(fread(header,1,sizeof(header),infile) != sizeof(header)) {
		DPRINTF(E_DBG,L_SCAN,"Could not read frame header\n");
		return;
	    }

	    if(!scan_decode_mp3_frame(header,&fi))
		found=1;
	}
	
	if(!found)
	    index++;
    }

    pos += index;

    /* found first frame.  Let's move */
    while(frame_count < 10) {
	fseek(infile,pos,SEEK_SET);
	if(fread(header,1,sizeof(header),infile) != sizeof(header)) {
	    DPRINTF(E_DBG,L_SCAN,"Could not read frame header\n");
	    return;
	}
	if(scan_decode_mp3_frame(header,&fi)) {
	    DPRINTF(E_DBG,L_SCAN,"Invalid frame header while averaging\n");
	    return;
	}
    
	bitrate_total += fi.bitrate;
	frame_count++;
	pos += fi.frame_length;
    }

    DPRINTF(E_DBG,L_SCAN,"Old bitrate: %d\n",pfi->bitrate);
    pfi->bitrate = bitrate_total/frame_count;
    DPRINTF(E_DBG,L_SCAN,"New bitrate: %d\n",pfi->bitrate);

    return;
}

/**
 * do a full frame-by-frame scan of the file, counting frames
 * as we go to try and get a more accurate song length estimate.
 * If the song turns out to be CBR, then we'll not set the frame
 * length.  Instead we'll use the file size estimate, since it is 
 * more consistent with iTunes.
 *
 * @param infile file to scan for frame count
 * @param pfi pointer to frame info struct to put framecount into
 */
void scan_get_frame_count(FILE *infile, SCAN_FRAMEINFO *pfi) {
    int pos;
    int frames=0;
    unsigned char frame_buffer[4];
    SCAN_FRAMEINFO fi;
    off_t file_size;
    int err=0;
    int cbr=1;
    int last_bitrate=0;

    DPRINTF(E_DBG,L_SCAN,"Starting frame count\n");
    
    fseek(infile,0,SEEK_END);
    file_size=ftell(infile);

    pos=pfi->frame_offset;

    while(1) {
	err=1;
	DPRINTF(E_SPAM,L_SCAN,"Seeking to %d\n",pos);

	fseek(infile,pos,SEEK_SET);
	if(fread(frame_buffer,1,sizeof(frame_buffer),infile) == sizeof(frame_buffer)) {
	    /* check for valid frame */
	    if(!scan_decode_mp3_frame(frame_buffer,&fi)) {
		frames++;
		pos += fi.frame_length;
		err=0;

		if((last_bitrate) && (fi.bitrate != last_bitrate))
		    cbr=0;
		last_bitrate=fi.bitrate;

		/* no point in brute scan of a cbr file... */
		if(cbr && (frames > 100)) {
		    DPRINTF(E_DBG,L_SCAN,"File appears to be CBR... quitting frame count\n");
		    return;
		}
	    }
	}

	if(err) {
	    if(pos > (file_size - 4096)) {  /* probably good enough */
		pfi->number_of_frames=frames;
		DPRINTF(E_DBG,L_SCAN,"Estimated frame count: %d\n",frames);
		return;
	    } else {
		DPRINTF(E_DBG,L_SCAN,"Frame count aborted on error.  Pos=%d, Count=%d\n",
			pos, frames);
		return;
	    }
	}
    }
}


/**
 * Get information from the file headers itself -- like
 * song length, bit rate, etc.
 *
 * @param file File to get info for
 * @param pmp3 where to put the found information
 */
int scan_get_mp3fileinfo(char *file, MP3FILE *pmp3) {
    FILE *infile;
    SCAN_ID3HEADER *pid3;
    SCAN_FRAMEINFO fi;
    unsigned int size=0;
    off_t fp_size=0;
    off_t file_size;
    unsigned char buffer[1024];
    int index;

    int xing_flags;
    int found;

    int first_check=0;
    char frame_buffer[4];

    if(!(infile=fopen(file,"rb"))) {
	DPRINTF(E_WARN,L_SCAN,"Could not open %s for reading\n",file);
	return -1;
    }

    memset((void*)&fi,0x00,sizeof(fi));

    fseek(infile,0,SEEK_END);
    file_size=ftell(infile);
    fseek(infile,0,SEEK_SET);

    pmp3->file_size=file_size;

    if(fread(buffer,1,sizeof(buffer),infile) != sizeof(buffer)) {
	if(ferror(infile)) {
	    DPRINTF(E_LOG,L_SCAN,"Error reading: %s\n",strerror(errno));
	} else {
	    DPRINTF(E_LOG,L_SCAN,"Short file: %s\n",file);
	}
	fclose(infile);
	return -1;
    }

    pid3=(SCAN_ID3HEADER*)buffer;

    found=0;
    fp_size=0;

    if(strncmp(pid3->id,"ID3",3)==0) {
	/* found an ID3 header... */
	DPRINTF(E_DBG,L_SCAN,"Found ID3 header\n");
	size = (pid3->size[0] << 21 | pid3->size[1] << 14 | 
		pid3->size[2] << 7 | pid3->size[3]);
	fp_size=size + sizeof(SCAN_ID3HEADER);
	first_check=1;
	DPRINTF(E_DBG,L_SCAN,"Header length: %d\n",size);
    }

    index = 0;

    /* Here we start the brute-force header seeking.  Sure wish there
     * weren't so many crappy mp3 files out there
     */

    while(!found) {
	fseek(infile,fp_size,SEEK_SET);
	DPRINTF(E_DBG,L_SCAN,"Reading in new block at %d\n",(int)fp_size);
	if(fread(buffer,1,sizeof(buffer),infile) < sizeof(buffer)) {
	    DPRINTF(E_LOG,L_SCAN,"Short read: %s\n",file);
	    fclose(infile);
	    return 0;
	}

	index=0;
	while(!found) {
	    while((buffer[index] != 0xFF) && (index < (sizeof(buffer)-50)))
		index++;

	    if((first_check) && (index)) {
		fp_size=0;
		DPRINTF(E_DBG,L_SCAN,"Bad header... dropping back for full frame search\n");
		first_check=0;
		break;
	    }

	    if(index > sizeof(buffer) - 50) {
		fp_size += index;
		DPRINTF(E_DBG,L_SCAN,"Block exhausted\n");
		break;
	    }

	    if(!scan_decode_mp3_frame(&buffer[index],&fi)) {
		DPRINTF(E_DBG,L_SCAN,"valid header at %d\n",index);
		if(strncasecmp((char*)&buffer[index+fi.xing_offset+4],"XING",4) == 0) {
		    /* no need to check further... if there is a xing header there,
		     * this is definately a valid frame */
		    found=1;
		    fp_size += index;
		} else {
		    /* No Xing... check for next frame */
		    DPRINTF(E_DBG,L_SCAN,"Found valid frame at %04x\n",(int)fp_size+index);
		    DPRINTF(E_DBG,L_SCAN,"Checking at %04x\n",(int)fp_size+index+fi.frame_length);
		    fseek(infile,fp_size + index + fi.frame_length,SEEK_SET);
		    if(fread(frame_buffer,1,sizeof(frame_buffer),infile) == sizeof(frame_buffer)) {
			if(!scan_decode_mp3_frame(frame_buffer,&fi)) {
			    found=1;
			    fp_size += index;
			} 
		    } else {
			DPRINTF(E_LOG,L_SCAN,"Could not read frame header: %s\n",file);
			fclose(infile);
			return 0;
		    }

		    if(!found) {
			DPRINTF(E_DBG,L_SCAN,"Didn't pan out.\n");
		    }
		}
	    }
	    
	    if(!found) {
		index++;
		if (first_check) {
		    /* if the header info was wrong about where the data started,
		     * then start a brute-force scan from the beginning of the file.
		     * don't want to just scan forward, because we might have already 
		     * missed the xing header
		     */
		    DPRINTF(E_DBG,L_SCAN,"Bad header... dropping back for full frame search\n");
		    first_check=0;
		    fp_size=0;
		    break;
		}
	    }
	}
    }

    file_size -= fp_size;
    fi.frame_offset=fp_size;

    if(scan_decode_mp3_frame(&buffer[index],&fi)) {
	fclose(infile);
	DPRINTF(E_LOG,L_SCAN,"Could not find sync frame: %s\n",file);
	DPRINTF(E_LOG,L_SCAN,"If this is a valid mp3 file that plays in "
		"other applications, please email me at rpedde@users.sourceforge.net "
		"and tell me you got this error.  Thanks");
	return 0;
    }

    DPRINTF(E_DBG,L_SCAN," MPEG Version: %0.1g\n",fi.version);
    DPRINTF(E_DBG,L_SCAN," Layer: %d\n",fi.layer);
    DPRINTF(E_DBG,L_SCAN," Sample Rate: %d\n",fi.samplerate);
    DPRINTF(E_DBG,L_SCAN," Bit Rate: %d\n",fi.bitrate);
	
    /* now check for an XING header */
    if(strncasecmp((char*)&buffer[index+fi.xing_offset+4],"XING",4) == 0) {
	DPRINTF(E_DBG,L_SCAN,"Found Xing header\n");
	xing_flags=*((int*)&buffer[index+fi.xing_offset+4+4]);
	xing_flags=ntohs(xing_flags);

	DPRINTF(E_DBG,L_SCAN,"Xing Flags: %02X\n",xing_flags);

	if(xing_flags & 0x1) {
	    /* Frames field is valid... */
	    fi.number_of_frames=*((int*)&buffer[index+fi.xing_offset+4+8]);
	    fi.number_of_frames=ntohs(fi.number_of_frames);
	}
    }

    if((config.scan_type != 0) &&
       (fi.number_of_frames == 0) &&
       (!pmp3->song_length)) {
	/* We have no good estimate of song time, and we want more
	 * aggressive scanning */
	DPRINTF(E_DBG,L_SCAN,"Starting aggressive file length scan\n");
	if(config.scan_type == 1) {
	    /* get average bitrate */
	    scan_get_average_bitrate(infile, &fi);
	} else {
	    /* get full frame count */
	    scan_get_frame_count(infile, &fi);
	}
    }

    pmp3->bitrate=fi.bitrate;
    pmp3->samplerate=fi.samplerate;
    
    /* guesstimate the file length */
    if(!pmp3->song_length) { /* could have gotten it from the tag */
	/* DWB: use ms time instead of seconds, use doubles to
	   avoid overflow */
	if(!fi.number_of_frames) { /* not vbr */
	    pmp3->song_length = (int) ((double) file_size * 8. /
				       (double) fi.bitrate);

	} else {
	    pmp3->song_length = (int) ((double)(fi.number_of_frames*fi.samples_per_frame*1000.)/
				       (double) fi.samplerate);
	}

    }
    DPRINTF(E_DBG,L_SCAN," Song Length: %d\n",pmp3->song_length);

    fclose(infile);
    return 0;
}

/**
 * Manually build tags.  Set artist to computer/orchestra
 * if there is already no artist.  Perhaps this could be 
 * done better, but I'm not sure what else to do here.
 *
 * @param song MP3FILE of the file to build composite tags for
 */
void make_composite_tags(MP3FILE *song) {
    int len;

    len=0;

    if(!song->artist && (song->orchestra || song->conductor)) {
	if(song->orchestra)
	    len += strlen(song->orchestra);
	if(song->conductor)
	    len += strlen(song->conductor);

	len += 3;

	song->artist=(char*)calloc(len, 1);
	if(song->artist) {
	    if(song->orchestra)
		strcat(song->artist,song->orchestra);

	    if(song->orchestra && song->conductor)
		strcat(song->artist," - ");

	    if(song->conductor)
		strcat(song->artist,song->conductor);
	}
    }

    if(song->url) {
	song->data_kind=1;
    } else {
	song->data_kind=0;
    }

    if(!song->title)
	song->title = strdup(song->fname);

    song->item_kind = 2; /* music, I think. */
}

