/*
 * $Id$
 * Implementation for simple gdbm db
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

#define _XOPEN_SOURCE 600

#include <errno.h>
#include <gdbm.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "err.h"
#include "mp3-scanner.h"
#include "playlist.h"
#define RB_FREE
#include "redblack.h"

#include "db-memory.h"

/*
 * Defines
 */
#define DB_VERSION 3
#define STRLEN(a) (a) ? strlen((a)) + 1 : 1 
#define MAYBEFREE(a) { if((a)) free((a)); };

/* For old version of gdbm */
#ifndef GDBM_SYNC
# define GDBM_SYNC 0
#endif

#ifndef GDBM_NOLOCK
# define GDBM_NOLOCK 0
#endif


/*
 * Typedefs
 */
typedef struct tag_mp3record MP3RECORD;
struct tag_mp3record {
    MP3FILE mp3file;
    MP3RECORD* next;
};

typedef struct tag_playlistentry {
    unsigned int id;
    struct tag_playlistentry *next;
} DB_PLAYLISTENTRY;

typedef struct tag_playlist {
    unsigned int id;
    int songs;
    int is_smart;
    char *name;
    struct tag_playlistentry *nodes;
    struct tag_playlist *next;
} DB_PLAYLIST;

typedef struct tag_mp3packed {
    int version;
    int struct_size; // so we can upgrade in place next time... doh!
    int bitrate;
    int samplerate;
    int song_length;
    int file_size;
    int year;
    
    int track;
    int total_tracks;

    int disc;
    int total_discs;

    int time_added;
    int time_modified;
    int time_played;

    int bpm; // DB Version 3

    unsigned int id; /* inode */

    int path_len;
    int fname_len;
    int title_len;
    int artist_len;
    int album_len;
    int genre_len;
    int comment_len;
    int type_len;
    int composer_len;
    int orchestra_len;
    int conductor_len;
    int grouping_len;

    /* DB VERSION 4 AND ABOVE GO HERE! */
    char data[1];
} MP3PACKED;

typedef struct {
    MP3RECORD*	root;
    MP3RECORD*	next;
} MP3HELPER;;

/*
 * Globals 
 */
int db_version_no;
int db_update_mode=0;
int db_song_count;
int db_playlist_count=0;
int db_last_scan;
DB_PLAYLIST db_playlists;
pthread_rwlock_t db_rwlock; /* OSX doesn't have PTHREAD_RWLOCK_INITIALIZER (?!?!?!?!) */
pthread_once_t db_initlock=PTHREAD_ONCE_INIT;
GDBM_FILE db_songs;
struct rbtree *db_removed;

/*
 * Forwards
 */

int db_start_initial_update(void);
int db_end_initial_update(void);
int db_is_empty(void);
int db_open(char *parameters, int reload);
int db_init();
int db_deinit(void);
int db_version(void);
int db_add(MP3FILE *mp3file);
int db_delete(int id);
int db_add_playlist(unsigned int playlistid, char *name, int is_smart);
int db_add_playlist_song(unsigned int playlistid, unsigned int itemid);
int db_unpackrecord(datum *pdatum, MP3FILE *pmp3);
int db_scanning(void);
datum *db_packrecord(MP3FILE *pmp3);

int db_get_song_count(void);
int db_get_playlist_count(void);
int db_get_playlist_is_smart(int playlistid); 
int db_get_playlist_entry_count(int playlistid);
char *db_get_playlist_name(int playlistid);

MP3FILE *db_find(int id);

void db_freefile(MP3FILE *pmp3);
int db_compare_rb_nodes(const void *pa, const void *pb, const void *cfg);


/* 
 * db_readlock
 *
 * If this fails, something is so amazingly hosed, we might just as well 
 * terminate.  
 */
void db_readlock(void) {
    int err;

    if((err=pthread_rwlock_rdlock(&db_rwlock))) {
	DPRINTF(ERR_FATAL,"cannot lock rdlock: %s\n",strerror(err));
    }
}

/* 
 * db_writelock
 * 
 * same as above
 */
void db_writelock(void) {
    int err;

    if((err=pthread_rwlock_wrlock(&db_rwlock))) {
	DPRINTF(ERR_FATAL,"cannot lock rwlock: %s\n",strerror(err));
    }
}

/*
 * db_unlock
 * 
 * useless, but symmetrical 
 */
int db_unlock(void) {
    return pthread_rwlock_unlock(&db_rwlock);
}


/*
 * db_compare_rb_nodes
 *
 * compare redblack nodes, which are just ints
 */
int db_compare_rb_nodes(const void *pa, const void *pb, const void *cfg) {
    if(*(int*)pa < *(int *)pb) return -1;
    if(*(int*)pb < *(int *)pa) return 1;
    return 0;
}


/*
 * db_init_once
 *
 * Must dynamically initialize the rwlock, as Mac OSX 10.3 (at least)
 * doesn't have a static initializer for rwlocks
 */
void db_init_once(void) {
    pthread_rwlock_init(&db_rwlock,NULL);
}

/*
 * db_open
 *
 * Open the database, so we can drop privs
 */
int db_open(char *parameters, int reload) {
    char db_path[PATH_MAX + 1];

    if(pthread_once(&db_initlock,db_init_once))
	return -1;

    snprintf(db_path,sizeof(db_path),"%s/%s",parameters,"songs.gdb");

    reload = reload ? GDBM_NEWDB : GDBM_WRCREAT;
    db_songs=gdbm_open(db_path, 0, reload | GDBM_SYNC | GDBM_NOLOCK,
		       0600,NULL);
    if(!db_songs) {
	DPRINTF(ERR_FATAL,"Could not open songs database (%s)\n",
		db_path);
	return -1;
    }

    return 0;
}


/*
 * db_init
 *
 * Initialize the database.  Parameter is the directory
 * of the database files
 */
int db_init(void) {
    MP3FILE mp3file;
    datum tmp_key,tmp_nextkey,song_data;
    
    pl_register();

    db_version_no=1;
    db_song_count=0;

    DPRINTF(ERR_DEBUG,"Building playlists\n");

    /* count the actual songs and build the playlists */
    tmp_key=gdbm_firstkey(db_songs);

    MEMNOTIFY(tmp_key.dptr);

    while(tmp_key.dptr) {
	/* Fetch that key */
	song_data=gdbm_fetch(db_songs,tmp_key);
	MEMNOTIFY(song_data.dptr);
	if(song_data.dptr) {
	    if(!db_unpackrecord(&song_data,&mp3file)) {
		/* Check against playlist */
		pl_eval(&mp3file);
		db_freefile(&mp3file);
	    }
	    free(song_data.dptr);
	}
	
	tmp_nextkey=gdbm_nextkey(db_songs,tmp_key);
	MEMNOTIFY(tmp_nextkey.dptr);
	free(tmp_key.dptr); 
	tmp_key=tmp_nextkey;
	db_song_count++;
    }

    DPRINTF(ERR_DEBUG,"Loaded database... found %d songs\n",db_song_count);

    /* and the playlists */
    return 0;
}

/*
 * db_deinit
 *
 * Close the db, in this case freeing memory
 */
int db_deinit(void) {
    DB_PLAYLIST *plist;
    DB_PLAYLISTENTRY *pentry;

    gdbm_close(db_songs);

    while(db_playlists.next) {
	plist=db_playlists.next;
	db_playlists.next=plist->next;
	free(plist->name);
	/* free all the nodes */
	while(plist->nodes) {
	    pentry=plist->nodes;
	    plist->nodes = pentry->next;
	    free(pentry);
	}
	free(plist);
    }

    return 0;
}

/*
 * db_scanning
 *
 * is the db currently in scanning mode?
 */
int db_scanning(void) {
    return db_update_mode;
}

/*
 * db_version
 *
 * return the db version
 */
int db_version(void) {
    int version;

    db_readlock();
    version=db_version_no;
    db_unlock();

    return version;
}

/*
 * db_start_initial_update
 *
 * Set the db to bulk import mode
 */
int db_start_initial_update(void) {
    datum tmp_key,tmp_nextkey;
    int err;

    /* we need a write lock on the db -- stop enums from happening */
    db_writelock();

    if((db_removed=rbinit(db_compare_rb_nodes,NULL)) == NULL) {
	errno=ENOMEM;
	return -1;
    }

    /* load up the red-black tree with all the current songs in the db */

    tmp_key=gdbm_firstkey(db_songs);

    MEMNOTIFY(tmp_key.dptr);

    while(tmp_key.dptr) {
	/* Add it to the rbtree */
	if(!rbsearch((void*)tmp_key.dptr,db_removed)) {
	    errno=ENOMEM;
	    return -1;
	}
	
	tmp_nextkey=gdbm_nextkey(db_songs,tmp_key);
	MEMNOTIFY(tmp_nextkey.dptr);
	// free(tmp_key.dptr); /* we'll free it in update mode */
	tmp_key=tmp_nextkey;
    }

    db_update_mode=1;

    return 0;
}

/*
 * db_end_initial_update
 *
 * Take the db out of bulk import mode
 */
int db_end_initial_update(void) {
    const void *val;

    db_update_mode=0;

    DPRINTF(ERR_DEBUG,"Initial update over.  Removing stale items\n");
    for(val=rblookup(RB_LUFIRST,NULL,db_removed); val != NULL; 
	val=rblookup(RB_LUNEXT,val,db_removed)) {
	db_delete(*((int*)val));
    }

    DPRINTF(ERR_DEBUG,"Done removing stale items\n");

    rbdestroy(db_removed);

    db_unlock();

    return 0;
}

/*
 * db_is_empty
 *
 * See if the db is empty or not -- that is, should
 * the scanner start up in bulk update mode or in
 * background update mode
 */
int db_is_empty(void) {
    return !db_song_count;
}


/*
 * db_add_playlist
 *
 * Add a new playlist
 *
 * FIXME: this assume we are in db_update mode... if this is called at random,
 * then we have to get a write lock.
 */
int db_add_playlist(unsigned int playlistid, char *name, int is_smart) {
    int err;
    DB_PLAYLIST *pnew;
    
    pnew=(DB_PLAYLIST*)malloc(sizeof(DB_PLAYLIST));
    if(!pnew)
	return -1;

    pnew->name=strdup(name);
    pnew->id=playlistid;
    pnew->nodes=NULL;
    pnew->songs=0;
    pnew->is_smart=is_smart;

    if(!pnew->name) {
	free(pnew);
	return -1;
    }

    DPRINTF(ERR_DEBUG,"Adding new playlist %s\n",name);

    pnew->next=db_playlists.next;
    db_playlists.next=pnew;

    db_version_no++;

    DPRINTF(ERR_DEBUG,"Added playlist\n");
    return 0;
}

/*
 * db_add_playlist_song
 *
 * Add a song to a particular playlist
 *
 * FIXME: as db_add playlist, this assumes we already have a writelock from
 * db_udpate_mode.
 */
int db_add_playlist_song(unsigned int playlistid, unsigned int itemid) {
    DB_PLAYLIST *current;
    DB_PLAYLISTENTRY *pnew;
    int err;
    
    pnew=(DB_PLAYLISTENTRY*)malloc(sizeof(DB_PLAYLISTENTRY));
    if(!pnew)
	return -1;

    pnew->id=itemid;
    pnew->next=NULL;

    DPRINTF(ERR_DEBUG,"Adding item %d to %d\n",itemid,playlistid); 

    current=db_playlists.next;
    while(current && (current->id != playlistid))
	current=current->next;

    if(!current) {
	DPRINTF(ERR_WARN,"Could not find playlist attempting to add to\n");
	if(!db_update_mode)
	    db_unlock();
	db_unlock();
	free(pnew);
	return -1;
    }

    if(!current->songs)
	db_playlist_count++;

    current->songs++;
    DPRINTF(ERR_DEBUG,"Playlist now has %d entries\n",current->songs);
    pnew->next = current->nodes;
    current->nodes = pnew;

    db_version_no++;

    DPRINTF(ERR_DEBUG,"Added playlist item\n");
    return 0;
}

/*
 * db_packrecord 
 *
 * Given an MP3 record, turn it into a datum
 */
datum *db_packrecord(MP3FILE *pmp3) {
    int len;
    datum *result;
    MP3PACKED *ppacked;
    int offset;

    len=sizeof(MP3PACKED); 
    len += STRLEN(pmp3->path);
    len += STRLEN(pmp3->fname);
    len += STRLEN(pmp3->title);
    len += STRLEN(pmp3->artist);
    len += STRLEN(pmp3->album);
    len += STRLEN(pmp3->genre);
    len += STRLEN(pmp3->comment);
    len += STRLEN(pmp3->type);
    len += STRLEN(pmp3->composer);
    len += STRLEN(pmp3->orchestra);
    len += STRLEN(pmp3->conductor);
    len += STRLEN(pmp3->grouping);

    result = (datum*) malloc(sizeof(datum));
    if(!result)
	return NULL;

    result->dptr = (void*)malloc(len);
    result->dsize=len;

    if(!result->dptr) {
	free(result);
	return NULL;
    }

    memset(result->dptr,0x00,len);

    /* start packing! */
    ppacked=(MP3PACKED *)result->dptr;

    ppacked->version=DB_VERSION;
    ppacked->struct_size=sizeof(MP3PACKED);

    ppacked->bitrate=pmp3->bitrate;
    ppacked->samplerate=pmp3->samplerate;
    ppacked->song_length=pmp3->song_length;
    ppacked->file_size=pmp3->file_size;
    ppacked->year=pmp3->year;
    ppacked->track=pmp3->track;
    ppacked->total_tracks=pmp3->total_tracks;
    ppacked->disc=pmp3->disc;
    ppacked->total_discs=pmp3->total_discs;
    ppacked->time_added=pmp3->time_added;
    ppacked->time_modified=pmp3->time_modified;
    ppacked->time_played=pmp3->time_played;
    ppacked->bpm=pmp3->bpm;
    ppacked->id=pmp3->id;

    ppacked->path_len=STRLEN(pmp3->path);
    ppacked->fname_len=STRLEN(pmp3->fname);
    ppacked->title_len=STRLEN(pmp3->title);
    ppacked->artist_len=STRLEN(pmp3->artist);
    ppacked->album_len=STRLEN(pmp3->album);
    ppacked->genre_len=STRLEN(pmp3->genre);
    ppacked->comment_len=STRLEN(pmp3->comment);
    ppacked->type_len=STRLEN(pmp3->type);
    ppacked->composer_len=STRLEN(pmp3->composer);
    ppacked->orchestra_len=STRLEN(pmp3->orchestra);
    ppacked->conductor_len=STRLEN(pmp3->conductor);
    ppacked->grouping_len=STRLEN(pmp3->grouping);

    offset=0;
    if(pmp3->path)
	memcpy(&ppacked->data[offset],pmp3->path,ppacked->path_len);
    offset+=ppacked->path_len;

    if(pmp3->fname)
	memcpy(&ppacked->data[offset],pmp3->fname,ppacked->fname_len);
    offset+=ppacked->fname_len;

    if(pmp3->title)
	memcpy(&ppacked->data[offset],pmp3->title,ppacked->title_len);
    offset+=ppacked->title_len;

    if(pmp3->artist)
	memcpy(&ppacked->data[offset],pmp3->artist,ppacked->artist_len);
    offset+=ppacked->artist_len;

    if(pmp3->album)
	memcpy(&ppacked->data[offset],pmp3->album,ppacked->album_len);
    offset+=ppacked->album_len;

    if(pmp3->genre)
	memcpy(&ppacked->data[offset],pmp3->genre,ppacked->genre_len);
    offset+=ppacked->genre_len;

    if(pmp3->comment)
	memcpy(&ppacked->data[offset],pmp3->comment,ppacked->comment_len);
    offset+=ppacked->comment_len;

    if(pmp3->type)
	memcpy(&ppacked->data[offset],pmp3->type,ppacked->type_len);
    offset+=ppacked->type_len;

    if(pmp3->composer)
	memcpy(&ppacked->data[offset],pmp3->composer,ppacked->composer_len);
    offset+=ppacked->composer_len;

    if(pmp3->orchestra)
	memcpy(&ppacked->data[offset],pmp3->orchestra,ppacked->orchestra_len);
    offset+=ppacked->orchestra_len;

    if(pmp3->conductor)
	memcpy(&ppacked->data[offset],pmp3->conductor,ppacked->conductor_len);
    offset+=ppacked->conductor_len;

    if(pmp3->grouping)
	memcpy(&ppacked->data[offset],pmp3->grouping,ppacked->grouping_len);
    offset+=ppacked->grouping_len;

    /* whew */
    return result;
}


/*
 * db_unpackrecord
 *
 * Given a datum, return an MP3 record
 */
int db_unpackrecord(datum *pdatum, MP3FILE *pmp3) {
    MP3PACKED *ppacked;
    int offset;

    /* should check minimum length (for v1) */

    memset(pmp3,0x0,sizeof(MP3FILE));

    /* VERSION 1 */
    ppacked=(MP3PACKED*)pdatum->dptr;

    if(ppacked->version != DB_VERSION) 
	DPRINTF(ERR_FATAL,"ON-DISK DATABASE VERSION HAS CHANGED\n"
		"Delete your songs.gdb and restart.\n");

    pmp3->bitrate=ppacked->bitrate;
    pmp3->samplerate=ppacked->samplerate;
    pmp3->song_length=ppacked->song_length;
    pmp3->file_size=ppacked->file_size;
    pmp3->year=ppacked->year;
    pmp3->track=ppacked->track;
    pmp3->total_tracks=ppacked->total_tracks;
    pmp3->disc=ppacked->disc;
    pmp3->total_discs=ppacked->total_discs;
    pmp3->time_added=ppacked->time_added;
    pmp3->time_modified=ppacked->time_modified;
    pmp3->time_played=ppacked->time_played;
    pmp3->bpm=ppacked->bpm;
    pmp3->id=ppacked->id;

    offset=0;
    if(ppacked->path_len > 1)
	pmp3->path=strdup(&ppacked->data[offset]);
    offset += ppacked->path_len;

    if(ppacked->fname_len > 1)
	pmp3->fname=strdup(&ppacked->data[offset]);
    offset += ppacked->fname_len;

    if(ppacked->title_len > 1)
	pmp3->title=strdup(&ppacked->data[offset]);
    offset += ppacked->title_len;

    if(ppacked->artist_len > 1)
	pmp3->artist=strdup(&ppacked->data[offset]);
    offset += ppacked->artist_len;

    if(ppacked->album_len > 1)
	pmp3->album=strdup(&ppacked->data[offset]);
    offset += ppacked->album_len;

    if(ppacked->genre_len > 1)
	pmp3->genre=strdup(&ppacked->data[offset]);
    offset += ppacked->genre_len;

    if(ppacked->comment_len > 1)
	pmp3->comment=strdup(&ppacked->data[offset]);
    offset += ppacked->comment_len;

    if(ppacked->type_len > 1)
	pmp3->type=strdup(&ppacked->data[offset]);
    offset += ppacked->type_len;

    if(ppacked->composer_len > 1)
	pmp3->composer=strdup(&ppacked->data[offset]);
    offset += ppacked->composer_len;

    if(ppacked->orchestra_len > 1)
	pmp3->orchestra=strdup(&ppacked->data[offset]);
    offset += ppacked->orchestra_len;

    if(ppacked->conductor_len > 1) 
	pmp3->conductor=strdup(&ppacked->data[offset]);
    offset += ppacked->conductor_len;

    if(ppacked->grouping_len > 1)
	pmp3->grouping=strdup(&ppacked->data[offset]);
    offset += ppacked->grouping_len;
    
    /* shouldn't this have been done when scanning? */
    make_composite_tags(pmp3);

    return 0;
}

/*
 * db_add
 *
 * add an MP3 file to the database.
 *
 * FIXME: Like the playlist adds, this assumes that this will only be called
 * during a db_update... that the writelock is already held
 */
int db_add(MP3FILE *pmp3) {
    int err;
    datum *pnew;
    datum dkey;
    MP3PACKED *ppacked;
    unsigned int id;

    DPRINTF(ERR_DEBUG,"Adding %s\n",pmp3->path);

    if(!(pnew=db_packrecord(pmp3))) {
	errno=ENOMEM;
	return -1;
    }

    /* insert the datum into the underlying database */
    dkey.dptr=(void*)&(pmp3->id);
    dkey.dsize=sizeof(unsigned int);

    /* dummy this up in case the client didn't */
    ppacked=(MP3PACKED *)pnew->dptr;
    if(!ppacked->time_added)
	ppacked->time_added=(int)time(NULL);
    ppacked->time_modified=(int)time(NULL);
    ppacked->time_played=0; /* do we want to keep track of this? */

    if(gdbm_store(db_songs,dkey,*pnew,GDBM_REPLACE)) {
	DPRINTF(ERR_FATAL,"Error inserting file %s in database\n",pmp3->fname);
    }

    DPRINTF(ERR_DEBUG,"Testing for %d\n",pmp3->id);
    id=pmp3->id;
    dkey.dptr=(void*)&id;
    dkey.dsize=sizeof(unsigned int);

    if(!gdbm_exists(db_songs,dkey)) {
	DPRINTF(ERR_FATAL,"Error.. could not find just added file\n");
    }

    free(pnew->dptr);
    free(pnew);

    db_version_no++;

    db_song_count++;
    
    DPRINTF(ERR_DEBUG,"Added file\n");
    return 0;
}

/*
 * db_freefile
 *
 * free a complete mp3record
 */
void db_freefile(MP3FILE *pmp3) {
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
}

/*
 * db_enum_begin
 *
 * Begin to walk through an enum of the database.  In order to retain
 * some sanity in song ordering we scan all the songs and perform an
 * insertion sort based on album, track, record id.
 *
 * Since the list is built entirely within this routine, it's also
 * safe to release the lock once the list is built.
 *
 * Also we eliminate the static db_enum_helper which allows (the
 * admittedly unnecessary) possibility of reentrance.
 *
 * this should be done quickly, as we'll be holding
 * a reader lock on the db
 */
static int nullstrcmp(const char* a, const char* b)
{
    if(a == 0)
	return b == 0 ? 1 : 0;
    if(b == 0)
	return -1;

    return strcmp(a, b);
}

int compare(MP3RECORD* a, MP3RECORD* b)
{
    int	comp;

    if((comp = nullstrcmp(a->mp3file.album, b->mp3file.album)) != 0)
	return comp;

    if((comp = (a->mp3file.disc - b->mp3file.disc)) != 0)
	return comp;

    if((comp = (a->mp3file.track - b->mp3file.track)) != 0)
	return comp;

    return a->mp3file.id - b->mp3file.id;
}

ENUMHANDLE db_enum_begin(void) {
    int err;
    MP3HELPER* helper;
    datum	key, next;

    db_readlock();

    helper = calloc(1, sizeof(MP3HELPER));
    
    for(key = gdbm_firstkey(db_songs) ; key.dptr ; key = next)
    {
	MP3RECORD*	entry = calloc(1, sizeof(MP3RECORD));
	MP3RECORD**	root;
	datum		data;

	data = gdbm_fetch(db_songs, key);
	if(!data.dptr)
	    DPRINTF(ERR_FATAL, "Cannot find item... corrupt database?\n");

	if(db_unpackrecord(&data, &entry->mp3file))
	    DPRINTF(ERR_FATAL, "Cannot unpack item... corrupt database?\n");

	for(root = &helper->root ; *root ; root = &(**root).next)
	{
	    if(compare(*root, entry) > 0)
		break;
	}

	entry->next = *root;
	*root = entry;

	next = gdbm_nextkey(db_songs, key);
	free(key.dptr);
    }

    helper->next = helper->root;
    return helper;
}

/*
 * db_playlist_enum_begin
 *
 * Start enumerating playlists
 */
ENUMHANDLE db_playlist_enum_begin(void) {
    int err;
    DB_PLAYLIST *current;

    db_readlock();

    /* find first playlist with a song in it! */
    current=db_playlists.next;
    while(current && (!current->songs))
	current=current->next;

    return current;
}

/*
 * db_playlist_items_enum_begin
 *
 * Start enumerating playlist items
 */
ENUMHANDLE db_playlist_items_enum_begin(int playlistid) {
    DB_PLAYLIST *current;
    int err;

    db_readlock();

    current=db_playlists.next;
    while(current && (current->id != playlistid))
	current=current->next;
    
    if(!current)
	return NULL;

    return current->nodes;
}


/*
 * db_enum
 *
 * Walk to the next entry
 */
MP3FILE *db_enum(ENUMHANDLE *current) {
    MP3HELPER*	helper;
    MP3RECORD*	record;

    if(!current)
	return 0;

    helper = *(MP3HELPER**) current;
    record = helper->next;

    if(helper->next == 0)
	return 0;

    helper->next = helper->next->next;

    return &record->mp3file;
}

/*
 * db_playlist_enum
 *
 * walk to the next entry
 */
int db_playlist_enum(ENUMHANDLE* handle) {
    DB_PLAYLIST** current = (DB_PLAYLIST**) handle;
    int retval;
    DB_PLAYLIST *p;

    if(*current) {
	retval = (*current)->id;
	p=*current;
	p=p->next;
	while(p && (!p->songs))
	    p=p->next;

	*current=p;
	return retval;
    }
    return -1;
}

/*
 * db_playlist_items_enum
 *
 * walk to the next entry
 */
int db_playlist_items_enum(ENUMHANDLE* handle) {
    DB_PLAYLISTENTRY **current;
    int retval;

    if(!handle)
	return -1;

    current = (DB_PLAYLISTENTRY**) handle;

    if(*current) {
	retval = (*current)->id;
	*current=(*current)->next;
	return retval;
    }

    return -1;
}

/*
 * db_enum_end
 *
 * dispose of the list we built up in db_enum_begin, the lock's
 * already been released.
 */
int db_enum_end(ENUMHANDLE handle) {
    MP3HELPER*	helper = (MP3HELPER*) handle;
    MP3RECORD*	record;

    while(helper->root)
    {
	MP3RECORD*	record = helper->root;

	helper->root = record->next;

	db_freefile(&record->mp3file);
    }

    free(helper);

    db_unlock();
    return 0;
}

/*
 * db_playlist_enum_end
 *
 * quit walking the database
 */
int db_playlist_enum_end(ENUMHANDLE handle) {
    return db_unlock();
}

/*
 * db_playlist_items_enum_end
 *
 * Quit walking the database
 */
int db_playlist_items_enum_end(ENUMHANDLE handle) {
    return db_unlock();
}


/*
 * db_find
 *
 * Find a MP3FILE entry based on file id  
 */
MP3FILE *db_find(int id) {  /* FIXME: Not reentrant */
    static MP3FILE *pmp3=NULL;
    datum key, content;

    key.dptr=(char*)&id;
    key.dsize=sizeof(int);

    content=gdbm_fetch(db_songs,key);
    MEMNOTIFY(content.dptr);
    if(!content.dptr)
	return NULL;

    if(pmp3) {
	db_freefile(pmp3);
	free(pmp3);
    }

    pmp3=(MP3FILE*)malloc(sizeof(MP3FILE));
    if(!pmp3)
	return NULL;

    db_unpackrecord(&content,pmp3);
    return pmp3;
}


/*
 * db_get_playlist_count
 *
 * return the number of playlists
 */
int db_get_playlist_count(void) {
    return db_playlist_count;
}

/*
 * db_get_song_count
 *
 * return the number of songs in the database.  Used for the /database
 * request
 */
int db_get_song_count(void) {
    return db_song_count;
}


/*
 * db_get_playlist_is_smart
 *
 * return whether or not the playlist is a "smart" playlist
 */
int db_get_playlist_is_smart(int playlistid) {
    DB_PLAYLIST *current;
    int err;
    int result;

    db_readlock();

    current=db_playlists.next;
    while(current && (current->id != playlistid))
	current=current->next;

    if(!current) {
	result=0;
    } else {
	result=current->is_smart;
    }

    db_unlock();
    return result;
}

/*
 * db_get_playlist_entry_count
 *
 * return the number of songs in a particular playlist
 */
int db_get_playlist_entry_count(int playlistid) {
    int count;
    DB_PLAYLIST *current;
    int err;

    db_readlock();

    current=db_playlists.next;
    while(current && (current->id != playlistid))
	current=current->next;

    if(!current) {
	count = -1;
    } else {
	count = current->songs;
    }

    db_unlock();
    return count;
}

/*
 * db_get_playlist_name
 *
 * return the name of a playlist
 *
 * FIXME: Small race here
 */
char *db_get_playlist_name(int playlistid) {
    char *name;
    DB_PLAYLIST *current;
    int err;

    db_readlock();

    current=db_playlists.next;
    while(current && (current->id != playlistid))
	current=current->next;

    if(!current) {
	name = NULL;
    } else {
	name = current->name;
    }

    db_unlock();
    return name;
}


/*
 * db_exists
 *
 * Check if a particular ID exists or not
 */
int db_exists(int id) {
    int *node;
    int err;
    MP3FILE *pmp3;
    datum key,content;

    DPRINTF(ERR_DEBUG,"Checking if node %d in db\n");
    key.dptr=(char*)&id;
    key.dsize=sizeof(int);

    content=gdbm_fetch(db_songs,key);
    MEMNOTIFY(content.dptr);
    if(!content.dptr) {
	DPRINTF(ERR_DEBUG,"Nope!  Not in DB\n");
	return 0;
    }

    if(db_update_mode) {
	/* knock it off the maybe list */
	node = (int*)rbdelete((void*)&id,db_removed);
	if(node) {
	    DPRINTF(ERR_DEBUG,"Knocked node %d from the list\n",*node);
	    free(node);
	}
    }

    free(content.dptr);
    DPRINTF(ERR_DEBUG,"Yup, in database\n");
    return 1;
}


/*
 * db_last_modified
 *
 * See when the file was last updated in the database
 */
int db_last_modified(int id) {
    int retval;
    MP3FILE *pmp3;
    int err;

    pmp3=db_find(id);
    if(!pmp3) {
	retval=0;
    } else {
	retval=pmp3->time_modified;
    }


    return retval;
}

/*
 * db_delete
 *
 * Delete an item from the database, and also remove it
 * from any playlists.
 */
int db_delete(int id) {
    int err;
    datum key;
    DB_PLAYLIST *pcurrent;
    DB_PLAYLISTENTRY *phead, *ptail;

    DPRINTF(ERR_DEBUG,"Removing item %d\n",id);

    if(db_exists(id)) {
	key.dptr=(void*)&id;
	key.dsize=sizeof(int);
	gdbm_delete(db_songs,key);
	db_song_count--;
	db_version_no++;

	/* walk the playlists and remove the item */
	pcurrent=db_playlists.next;
	while(pcurrent) {
	    phead=ptail=pcurrent->nodes;
	    while(phead && (phead->id != id)) {
		ptail=phead;
		phead=phead->next;
	    }

	    if(phead) { /* found it */
		DPRINTF(ERR_DEBUG,"Removing from playlist %d\n",
			pcurrent->id);
		if(phead == pcurrent->nodes) {
		    pcurrent->nodes=phead->next;
		} else {
		    ptail->next=phead->next;
		}
		free(phead);

		if(pcurrent->nodes == NULL) {
		    DPRINTF(ERR_DEBUG,"Empty Playlist!\n");
		    db_playlist_count--;
		}
	    }
	    pcurrent=pcurrent->next;
	}
    }

    db_version_no++;

    return 0;
}
