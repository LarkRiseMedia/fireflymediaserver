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
#include <sys/types.h>
#include <dirent.h>      /* why here?  For osx 10.2, of course! */

#include "daapd.h"
#include "db-memory.h"
#include "err.h"
#include "mp3-scanner.h"
#include "playlist.h"

#ifndef HAVE_STRCASESTR
# include "strcasestr.h"
#endif

/*
 * Typedefs
 */

typedef struct tag_scan_id3header {
    unsigned char id[3];
    unsigned char version[2];
    unsigned char flags;
    unsigned char size[4];
} SCAN_ID3HEADER;

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
    { 44100, 48000, 32000, 0 },
    { 22050, 24000, 16000, 0 },
    { 11025, 12000, 8000, 0 }
};



int scan_mode_foreground=1;

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

#define WINAMP_GENRE_UNKNOWN 148


/*
 * Forwards
 */
int scan_path(char *path);
int scan_gettags(char *file, MP3FILE *pmp3);
int scan_get_mp3tags(char *file, MP3FILE *pmp3);
int scan_get_aactags(char *file, MP3FILE *pmp3);
int scan_get_fileinfo(char *file, MP3FILE *pmp3);
int scan_get_mp3fileinfo(char *file, MP3FILE *pmp3);
int scan_get_aacfileinfo(char *file, MP3FILE *pmp3);

int scan_freetags(MP3FILE *pmp3);
void scan_static_playlist(char *path, struct dirent *pde, struct stat *psb);
void scan_music_file(char *path, struct dirent *pde, struct stat *psb);

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
    int err;

    scan_mode_foreground=0;
    if(db_is_empty()) {
	scan_mode_foreground=1;
    }

    if(db_start_initial_update()) 
	return -1;

    DPRINTF(ERR_DEBUG,"%s scanning for MP3s in %s\n",
	    scan_mode_foreground ? "Foreground" : "Background",
	    path);

    err=scan_path(path);

    if(db_end_initial_update())
	return -1;

    scan_mode_foreground=0;

    return err;
}

/*
 * scan_path
 *
 * Do a brute force scan of a path, finding all the MP3 files there
 */
int scan_path(char *path) {
    DIR *current_dir;
    char de[sizeof(struct dirent) + MAXNAMLEN + 1]; /* overcommit for solaris */
    struct dirent *pde;
    int err;
    char mp3_path[PATH_MAX];
    struct stat sb;
    int modified_time;

    if((current_dir=opendir(path)) == NULL) {
	return -1;
    }

    while(1) {
	pde=(struct dirent *)&de;

	err=readdir_r(current_dir,(struct dirent *)de,&pde);
	if(err == -1) {
	    DPRINTF(ERR_DEBUG,"Error on readdir_r: %s\n",strerror(errno));
	    err=errno;
	    closedir(current_dir);
	    errno=err;
	    return -1;
	}

	if(!pde)
	    break;
	
	if(pde->d_name[0] == '.') /* skip hidden and directories */
	    continue;

	sprintf(mp3_path,"%s/%s",path,pde->d_name);
	DPRINTF(ERR_DEBUG,"Found %s\n",mp3_path);
	if(stat(mp3_path,&sb)) {
	    DPRINTF(ERR_WARN,"Error statting: %s\n",strerror(errno));
	} else {
	    if(sb.st_mode & S_IFDIR) { /* dir -- recurse */
		DPRINTF(ERR_DEBUG,"Found dir %s... recursing\n",pde->d_name);
		scan_path(mp3_path);
	    } else {
		/* process the file */
		if(strlen(pde->d_name) > 4) {
		    if(strcasecmp(".m3u",(char*)&pde->d_name[strlen(pde->d_name) - 4]) == 0) {
			/* we found an m3u file */
			scan_static_playlist(path, pde, &sb);
		    } else if (strcasestr(config.extensions,
					  (char*)&pde->d_name[strlen(pde->d_name) - 4])) {
			
			/* only scan if it's been changed, or empty db */
			modified_time=sb.st_mtime;
			if((scan_mode_foreground) || 
			   !db_exists(sb.st_ino) ||
			   db_last_modified(sb.st_ino) < modified_time) {
			    scan_music_file(path,pde,&sb);
			} else {
			    DPRINTF(ERR_DEBUG,"Skipping file... not modified\n");
			}
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
void scan_static_playlist(char *path, struct dirent *pde, struct stat *psb) {
    char playlist_path[PATH_MAX];
    char m3u_path[PATH_MAX];
    char linebuffer[PATH_MAX];
    int fd;
    int playlistid;
    struct stat sb;

    DPRINTF(ERR_DEBUG,"Found static playlist: %s\n",pde->d_name);
    strcpy(m3u_path,pde->d_name);
    snprintf(playlist_path,sizeof(playlist_path),"%s/%s",path,pde->d_name);
    m3u_path[strlen(pde->d_name) - 4] = '\0';
    playlistid=psb->st_ino;
    fd=open(playlist_path,O_RDONLY);
    if(fd != -1) {
	db_add_playlist(playlistid,m3u_path,0);
	DPRINTF(ERR_DEBUG,"Added playlist as id %d\n",playlistid);

	while(readline(fd,linebuffer,sizeof(linebuffer)) > 0) {
	    while((linebuffer[strlen(linebuffer)-1] == '\n') ||
		  (linebuffer[strlen(linebuffer)-1] == '\r'))   /* windows? */
		linebuffer[strlen(linebuffer)-1] = '\0';

	    if((linebuffer[0] == ';') || (linebuffer[0] == '#'))
		continue;

	    /* FIXME - should chomp trailing comments */

	    /* otherwise, assume it is a path */
	    if(linebuffer[0] == '/') {
		strcpy(m3u_path,linebuffer);
	    } else {
		snprintf(m3u_path,sizeof(m3u_path),"%s/%s",path,linebuffer);
	    }

	    DPRINTF(ERR_DEBUG,"Checking %s\n",m3u_path);

	    /* might be valid, might not... */
	    if(!stat(m3u_path,&sb)) {
		/* FIXME: check to see if valid inode! */
		db_add_playlist_song(playlistid,sb.st_ino);
	    } else {
		DPRINTF(ERR_WARN,"Playlist entry %s bad: %s\n",
			m3u_path,strerror(errno));
	    }
	}
	close(fd);
    }
}

/*
 * scan_music_file
 *
 * scan a particular file as a music file
 */
void scan_music_file(char *path, struct dirent *pde, struct stat *psb) {
    MP3FILE mp3file;
    char mp3_path[PATH_MAX];

    snprintf(mp3_path,sizeof(mp3_path),"%s/%s",path,pde->d_name);

    /* we found an mp3 file */
    DPRINTF(ERR_DEBUG,"Found music file: %s\n",pde->d_name);
    
    memset((void*)&mp3file,0,sizeof(mp3file));
    mp3file.path=mp3_path;
    mp3file.fname=pde->d_name;
    if(strlen(pde->d_name) > 4)
	mp3file.type=strdup((char*)&pde->d_name[strlen(pde->d_name) - 4]);
    
    /* FIXME; assumes that st_ino is a u_int_32 */
    mp3file.id=psb->st_ino;
    
    /* Do the tag lookup here */
    if(!scan_gettags(mp3file.path,&mp3file) && 
       !scan_get_fileinfo(mp3file.path,&mp3file)) {
	db_add(&mp3file);
	pl_eval(&mp3file); /* FIXME: move to db_add? */
    } else {
	DPRINTF(ERR_INFO,"Skipping %s\n",pde->d_name);
    }
    
    scan_freetags(&mp3file);
}

/*
 * scan_aac_findatom
 *
 * Find an AAC atom
 */
long scan_aac_findatom(FILE *fin, long max_offset, char *which_atom, int *atom_size) {
    long current_offset=0;
    int size;
    char atom[4];

    while(current_offset < max_offset) {
	if(fread((void*)&size,1,sizeof(int),fin) != sizeof(int))
	    return -1;

	size=ntohl(size);

	if(size < 0) /* something not right */
	    return -1;

	if(fread(atom,1,4,fin) != 4) 
	    return -1;

	if(strncasecmp(atom,which_atom,4) == 0) {
	    *atom_size=size;
	    return current_offset;
	}

	fseek(fin,size-8,SEEK_CUR);
	current_offset+=size;
    }

    return -1;
}

/*
 * scan_get_aactags
 *
 * Get tags from an AAC (m4a) file
 */
int scan_get_aactags(char *file, MP3FILE *pmp3) {
    FILE *fin;
    long atom_offset;
    int atom_length;
    long file_size;

    long current_offset=0;
    int current_size;
    char current_atom[4];
    char *current_data;
    unsigned short us_data;
    int genre;
    int len;

    if(!(fin=fopen(file,"rb"))) {
	DPRINTF(ERR_INFO,"Cannot open file %s for reading\n",file);
	return -1;
    }

    fseek(fin,0,SEEK_END);
    file_size=ftell(fin);
    fseek(fin,0,SEEK_SET);

    atom_offset=scan_aac_findatom(fin,file_size,"moov",&atom_length);
    if(atom_offset != -1) {
	atom_offset=scan_aac_findatom(fin,atom_length - 8,"udta",&atom_length);
	if(atom_offset != -1) {
	    atom_offset=scan_aac_findatom(fin,atom_length - 8, "meta", &atom_length);
	    if(atom_offset != -1) { 
		fseek(fin,4,SEEK_CUR);   /* ???? */
		atom_offset=scan_aac_findatom(fin, atom_length - 8, "ilst", &atom_length);
		if(atom_offset != -1) {
		    /* found the tag section - need to walk through now */

		    while(current_offset < atom_length) {
			if(fread((void*)&current_size,1,sizeof(int),fin) != sizeof(int))
			    break;
			
			current_size=ntohl(current_size);
			
			if(current_size < 0) /* something not right */
			    break;

			if(fread(current_atom,1,4,fin) != 4) 
			    break;
			
			len=current_size-7;  /* for ill-formed too-short tags */
			if(len < 22)
			    len=22;

			current_data=(char*)malloc(len);  /* extra byte */
			memset(current_data,0x00,len);

			if(fread(current_data,1,current_size-8,fin) != current_size-8) 
			    break;

			if(!memcmp(current_atom,"\xA9" "nam",4)) { /* Song name */
			    pmp3->title=strdup((char*)&current_data[16]);
			} else if(!memcmp(current_atom,"\xA9" "ART",4)) {
			    pmp3->artist=strdup((char*)&current_data[16]);
			} else if(!memcmp(current_atom,"\xA9" "alb",4)) {
			    pmp3->album=strdup((char*)&current_data[16]);
			} else if(!memcmp(current_atom,"\xA9" "gen",4)) {
			    /* can this be a winamp genre??? */
			    pmp3->genre=strdup((char*)&current_data[16]);
			} else if(!memcmp(current_atom,"trkn",4)) {
			    us_data=*((unsigned short *)&current_data[18]);
			    us_data=htons(us_data);

			    pmp3->track=us_data;

			    us_data=*((unsigned short *)&current_data[20]);
			    us_data=htons(us_data);

			    pmp3->total_tracks=us_data;
			} else if(!memcmp(current_atom,"disk",4)) {
			    us_data=*((unsigned short *)&current_data[18]);
			    us_data=htons(us_data);

			    pmp3->disc=us_data;

			    us_data=*((unsigned short *)&current_data[20]);
			    us_data=htons(us_data);

			    pmp3->total_discs=us_data;
			} else if(!memcmp(current_atom,"\xA9" "day",4)) {
			    pmp3->year=atoi((char*)&current_data[16]);
			} else if(!memcmp(current_atom,"gnre",4)) {
			    genre=(int)(*((char*)&current_data[17]));
			    genre--;
			    
			    if((genre < 0) || (genre > WINAMP_GENRE_UNKNOWN))
				genre=WINAMP_GENRE_UNKNOWN;

			    pmp3->genre=strdup(scan_winamp_genre[genre]);
			}

			free(current_data);
			current_offset+=current_size;
		    }
		}
	    }
	}
    }

    fclose(fin);
    return 0;  /* we'll return as much as we got. */
}


/*
 * scan_gettags
 *
 * Scan an mp3 file for id3 tags using libid3tag
 */
int scan_gettags(char *file, MP3FILE *pmp3) {
    /* dispatch to appropriate tag handler */

    /* perhaps it would be better to just blindly try each
     * in turn, just in case the extensions are wrong/lying
     */

    if(!strcasecmp(pmp3->type,".aac")) 
	return scan_get_aactags(file,pmp3);

    if(!strcasecmp(pmp3->type,".m4a"))
	return scan_get_aactags(file,pmp3);

    if(!strcasecmp(pmp3->type,".m4p"))
	return scan_get_aactags(file,pmp3);

    if(!strcasecmp(pmp3->type,".mp4"))
	return scan_get_aactags(file,pmp3);

    /* should handle mp3 in the same way */
    if(!strcasecmp(pmp3->type,".mp3"))
	return scan_get_mp3tags(file,pmp3);

    /* maybe this is an extension that we've manually
     * specified in the config file, but don't know how
     * to extract tags from.  Ogg, maybe.
     */

    return 0;
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

    if(strcasecmp(pmp3->type,".mp3"))  /* can't get tags for non-mp3 */
	return 0;

    pid3file=id3_file_open(file,ID3_FILE_MODE_READONLY);
    if(!pid3file) {
	DPRINTF(ERR_WARN,"Cannot open %s\n",file);
	return -1;
    }

    pid3tag=id3_file_tag(pid3file);
    
    if(!pid3tag) {
	err=errno;
	id3_file_close(pid3file);
	errno=err;
	DPRINTF(ERR_WARN,"Cannot get ID3 tag for %s\n",file);
	return -1;
    }

    index=0;
    while((pid3frame=id3_tag_findframe(pid3tag,"",index))) {
	used=0;
	utf8_text=NULL;
	native_text=NULL;
	have_utf8=0;
	have_text=0;

	if(((pid3frame->id[0] == 'T')||(strcmp(pid3frame->id,"COMM")==0)) &&
	   (id3_field_getnstrings(&pid3frame->fields[1]))) 
	    have_text=1;

	if(have_text) {
	    native_text=id3_field_getstrings(&pid3frame->fields[1],0);

	    if(native_text) {
		have_utf8=1;
		utf8_text=id3_ucs4_utf8duplicate(native_text);
		MEMNOTIFY(utf8_text);

		if(!strcmp(pid3frame->id,"TIT2")) { /* Title */
		    used=1;
		    pmp3->title = utf8_text;
		    DPRINTF(ERR_DEBUG," Title: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TPE1")) {
		    used=1;
		    pmp3->artist = utf8_text;
		    DPRINTF(ERR_DEBUG," Artist: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TALB")) {
		    used=1;
		    pmp3->album = utf8_text;
		    DPRINTF(ERR_DEBUG," Album: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TCOM")) {
		    used=1;
		    pmp3->composer = utf8_text;
		    DPRINTF(ERR_DEBUG," Composer: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TIT1")) {
		    used=1;
		    pmp3->grouping = utf8_text;
		    DPRINTF(ERR_DEBUG," Grouping: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TPE2")) {
		    used=1;
		    pmp3->orchestra = utf8_text;
		    DPRINTF(ERR_DEBUG," Orchestra: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TPE3")) {
		    used=1;
		    pmp3->conductor = utf8_text;
		    DPRINTF(ERR_DEBUG," Conductor: %s\n",utf8_text);
		} else if(!strcmp(pid3frame->id,"TCON")) {
		    used=1;
		    pmp3->genre = utf8_text;
		    got_numeric_genre=0;
		    DPRINTF(ERR_DEBUG," Genre: %s\n",utf8_text);
		    if(pmp3->genre) {
			if(!strlen(pmp3->genre)) {
			    genre=WINAMP_GENRE_UNKNOWN;
			    got_numeric_genre=1;
			} else if (isdigit(pmp3->genre[0])) {
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
		    DPRINTF(ERR_DEBUG," Comment: %s\n",pmp3->comment);
		} else if(!strcmp(pid3frame->id,"TPOS")) {
		    tmp=(char*)utf8_text;
		    strsep(&tmp,"/");
		    if(tmp) {
			pmp3->total_discs=atoi(tmp);
		    }
		    pmp3->disc=atoi((char*)utf8_text);
		    DPRINTF(ERR_DEBUG," Disc %d of %d\n",pmp3->disc,pmp3->total_discs);
		} else if(!strcmp(pid3frame->id,"TRCK")) {
		    tmp=(char*)utf8_text;
		    strsep(&tmp,"/");
		    if(tmp) {
			pmp3->total_tracks=atoi(tmp);
		    }
		    pmp3->track=atoi((char*)utf8_text);
		    DPRINTF(ERR_DEBUG," Track %d of %d\n",pmp3->track,pmp3->total_tracks);
		} else if(!strcmp(pid3frame->id,"TDRC")) {
		    pmp3->year = atoi(utf8_text);
		    DPRINTF(ERR_DEBUG," Year: %d\n",pmp3->year);
		}
	    }
	}

	/* can check for non-text tags here */
	if((!used) && (have_utf8) && (utf8_text))
	    free(utf8_text);

	index++;
    }

    id3_file_close(pid3file);
    DPRINTF(ERR_DEBUG,"Got id3 tag successfully\n");
    return 0;
}

/*
 * scan_freetags
 *
 * Free up the tags that were dynamically allocated
 */
int scan_freetags(MP3FILE *pmp3) {
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

    return 0;
}


/*
 * scan_get_fileinfo
 *
 * Dispatch to actual file info handlers
 */
int scan_get_fileinfo(char *file, MP3FILE *pmp3) {
    FILE *infile;
    off_t file_size;

    if(strcasestr(".aac.m4a.m4p.mp4",pmp3->type))
	return scan_get_aacfileinfo(file,pmp3);
       
    if(!strcasecmp(pmp3->type,".mp3"))
	return scan_get_mp3fileinfo(file,pmp3);
    
    /* a file we don't know anything about... ogg or aiff maybe */
    if(!(infile=fopen(file,"rb"))) {
	DPRINTF(ERR_WARN,"Could not open %s for reading\n",file);
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
 * scan_get_aacfileinfo
 *
 * Get info from the actual aac headers
 */
int scan_get_aacfileinfo(char *file, MP3FILE *pmp3) {
    FILE *infile;
    long atom_offset;
    int atom_length;
    int temp_int;
    off_t file_size;

    DPRINTF(ERR_DEBUG,"Getting AAC file info\n");

    if(!(infile=fopen(file,"rb"))) {
	DPRINTF(ERR_WARN,"Could not open %s for reading\n",file);
	return -1;
    }

    fseek(infile,0,SEEK_END);
    file_size=ftell(infile);
    fseek(infile,0,SEEK_SET);

    pmp3->file_size=file_size;

    /* now, hunt for the mvhd atom */
    atom_offset=scan_aac_findatom(infile,file_size,"moov",&atom_length);
    if(atom_offset != -1) {
	atom_offset=scan_aac_findatom(infile,atom_length-8,"mvhd",&atom_length);
	if(atom_offset != -1) {
	    fseek(infile,16,SEEK_CUR);
	    fread((void*)&temp_int,1,sizeof(int),infile);
	    temp_int=ntohl(temp_int);
	    pmp3->song_length=temp_int/600;
	    DPRINTF(ERR_DEBUG,"Song length: %d seconds\n",temp_int/600);
	}
    }
    fclose(infile);
    return 0;
}


/*
 * scan_get_mp3fileinfo
 *
 * Get information from the file headers itself -- like
 * song length, bit rate, etc.
 */
int scan_get_mp3fileinfo(char *file, MP3FILE *pmp3) {
    FILE *infile;
    SCAN_ID3HEADER *pid3;
    unsigned int size=0;
    off_t fp_size=0;
    off_t file_size;
    unsigned char buffer[1024];
    int time_seconds;
    int index;
    int layer_index;
    int sample_index;

    int ver=0;
    int layer=0;
    int bitrate=0;
    int samplerate=0;
    int stereo=0;

    if(!(infile=fopen(file,"rb"))) {
	DPRINTF(ERR_WARN,"Could not open %s for reading\n",file);
	return -1;
    }

    fseek(infile,0,SEEK_END);
    file_size=ftell(infile);
    fseek(infile,0,SEEK_SET);

    pmp3->file_size=file_size;

    fread(buffer,1,sizeof(buffer),infile);
    pid3=(SCAN_ID3HEADER*)buffer;
    
    if(strncmp(pid3->id,"ID3",3)==0) {
	/* found an ID3 header... */
	DPRINTF(ERR_DEBUG,"Found ID3 header\n");
	size = (pid3->size[0] << 21 | pid3->size[1] << 14 | 
		pid3->size[2] << 7 | pid3->size[3]);
	fp_size=size + sizeof(SCAN_ID3HEADER);
	DPRINTF(ERR_DEBUG,"Header length: %d\n",size);
    }

    file_size -= fp_size;

    fseek(infile,fp_size,SEEK_SET);
    fread(buffer,1,sizeof(buffer),infile);

    index=0;
    while(((buffer[index] != 0xFF) || (buffer[index+1] < 224)) &&
	  (index < (sizeof(buffer)-10))) {
	index++;
    }

    if(index) {
	DPRINTF(ERR_DEBUG,"Scanned forward %d bytes to find frame header\n",index);
    }

    if((buffer[index] == 0xFF)&&(buffer[index+1] >= 224)) {
	ver=(buffer[index+1] & 0x18) >> 3;
	layer=(buffer[index+1] & 0x6) >> 1;

	layer_index=-1;
	sample_index=-1;

	switch(ver) {
	case 0: /* MPEG Version 2.5 */
	    sample_index=2;
	    if(layer == 3)
		layer_index=3;
	    else
		layer_index=4;
	    break;
	case 2: /* MPEG Version 2 */
	    sample_index=1;
	    if(layer == 3)
		layer_index=3;
	    else
		layer_index=4;
	    break;
	case 3: /* MPEG Version 1 */
	    sample_index=0;
	    if(layer == 3) /* layer 1 */
		layer_index=0;
	    if(layer == 2) /* layer 2 */
		layer_index=1;
	    if(layer == 1) /* layer 3 */
		layer_index=2;
	    break;
	}


	if(layer_index != -1) {
	    bitrate=(buffer[index+2] & 0xF0) >> 4;
	    bitrate=scan_br_table[layer_index][bitrate];
	    samplerate=(buffer[index+2] & 0x0C) >> 2;
	    samplerate=scan_sample_table[sample_index][samplerate];
	    pmp3->bitrate=bitrate;
	    pmp3->samplerate=samplerate;
	    stereo=buffer[index+3] & 0xC0 >> 6;
	    if(stereo == 3)
		stereo=0;
	    else
		stereo=1;
	} else {
	    /* not an mp3... */
	    DPRINTF(ERR_DEBUG,"File is not a MPEG file\n");
	    return -1;
	}


	/* guesstimate the file length */
	time_seconds = ((int)(file_size * 8)) / (bitrate * 1024);
	if(!pmp3->song_length) /* could have gotten it from the tag */
	    pmp3->song_length=time_seconds;
    } else {
	/* FIXME: should really scan forward to next sync frame */
	fclose(infile);
	DPRINTF(ERR_DEBUG,"Could not find sync frame\n");
	return 0;
    }
    

    fclose(infile);
    return 0;
}
