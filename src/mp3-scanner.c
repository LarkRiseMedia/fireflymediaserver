/*
 * $Id$
 * Implementation file for mp3 scanner and monitor
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

#include <dirent.h>
#include <errno.h>
#include <id3tag.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "db-memory.h"
#include "err.h"
#include "mp3-scanner.h"

/*
 * Typedefs
 */
#define MAYBEFREE(a) { if((a)) free((a)); };

/*
 * Forwards
 */
int scan_foreground(char *path);
int scan_gettags(char *file, MP3FILE *pmp3);
int scan_freetags(MP3FILE *pmp3);

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
    if(db_is_empty()) {
	if(db_start_initial_update()) 
	    return -1;

	scan_foreground(path);

	if(db_end_initial_update())
	    return -1;
    } else {
	/* do deferred updating */
	return ENOTSUP;
    }

    return 0;
}

/*
 * scan_foreground
 *
 * Do a brute force scan of a path, finding all the MP3 files there
 */
int scan_foreground(char *path) {
    MP3FILE mp3file;
    DIR *current_dir;
    struct dirent de;
    struct dirent *pde;
    int err;
    char mp3_path[PATH_MAX];
    struct stat sb;

    if((current_dir=opendir(path)) == NULL) {
	return -1;
    }

    while(1) {
	pde=&de;
	err=readdir_r(current_dir,&de,&pde);
	if(err == -1) {
	    DPRINTF(ERR_DEBUG,"Error on readdir_r: %s\n",strerror(errno));
	    err=errno;
	    closedir(current_dir);
	    errno=err;
	    return -1;
	}

	if(!pde)
	    break;
	
	if(de.d_name[0] == '.')
	    continue;

	sprintf(mp3_path,"%s/%s",path,de.d_name);
	DPRINTF(ERR_DEBUG,"Found %s\n",mp3_path);
	if(stat(mp3_path,&sb)) {
	    DPRINTF(ERR_WARN,"Error statting: %s\n",strerror(errno));
	}

	if(sb.st_mode & S_IFDIR) { /* dir -- recurse */
	    DPRINTF(ERR_DEBUG,"Found dir %s... recursing\n",de.d_name);
	    scan_foreground(mp3_path);
	} else {
	    DPRINTF(ERR_DEBUG,"Processing file\n");
	    /* process the file */
	    if(strlen(de.d_name) > 4) {
		if(strcasecmp(".mp3",(char*)&de.d_name[strlen(de.d_name) - 4]) == 0) {
		    /* we found an mp3 file */
		    DPRINTF(ERR_DEBUG,"Found mp3: %s\n",de.d_name);

		    memset((void*)&mp3file,0,sizeof(mp3file));
		    mp3file.path=mp3_path;
		    mp3file.fname=de.d_name;
		    
		    /* Do the tag lookup here */
		    scan_gettags(mp3file.path,&mp3file);
		    
		    db_add(&mp3file);
		    
		    scan_freetags(&mp3file);
		}
	    }
	}
    }

    closedir(current_dir);
}

/*
 * scan_gettags
 *
 * Scan an mp3 file for id3 tags using libid3tag
 */
int scan_gettags(char *file, MP3FILE *pmp3) {
    struct id3_file *pid3file;
    struct id3_tag *pid3tag;
    struct id3_frame *pid3frame;
    int err;
    int index;
    int used;
    unsigned char *utf8_text;

    pid3file=id3_file_open(file,ID3_FILE_MODE_READONLY);
    if(!pid3file) {
	return -1;
    }

    pid3tag=id3_file_tag(pid3file);
    
    if(!pid3tag) {
	err=errno;
	id3_file_close(pid3file);
	errno=err;
	return -1;
    }

    index=0;
    while((pid3frame=id3_tag_findframe(pid3tag,"",index))) {
	used=0;
	utf8_text=NULL;

	if((pid3frame->id[0] == 'T')&&(id3_field_getnstrings(&pid3frame->fields[1])))
	    utf8_text=id3_ucs4_utf8duplicate(id3_field_getstrings(&pid3frame->fields[1],0));

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
	} else if(!strcmp(pid3frame->id,"TCON")) {
	    used=1;
	    pmp3->genre = utf8_text;
	    DPRINTF(ERR_DEBUG," Genre: %s\n",utf8_text);
	} else if(!strcmp(pid3frame->id,"COMM")) {
	    used=1;
	    pmp3->comment = utf8_text;
	    DPRINTF(ERR_DEBUG," Comment: %s\n",utf8_text);
	}

	if((!used) && (pid3frame->id[0]=='T') && utf8_text)
	    free(utf8_text);

	index++;
    }

    pmp3->got_id3=1;
    id3_file_close(pid3file);
}

/*
 * scan_freetags
 *
 * Free up the tags that were dynamically allocated
 */
int scan_freetags(MP3FILE *pmp3) {
    if(!pmp3->got_id3) 
	return 0;

    MAYBEFREE(pmp3->title);
    MAYBEFREE(pmp3->artist);
    MAYBEFREE(pmp3->album);
    MAYBEFREE(pmp3->genre);
    MAYBEFREE(pmp3->comment);

    return 0;
}
