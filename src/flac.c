/*
 * $Id$
 * Implementation file for server side format conversion.
 *
 * Copyright (C) 2005 Timo J. Rinne (tri@iki.fi)
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
#include "db-memory.h"
#include "err.h"
#include "mp3-scanner.h"

#ifndef HAVE_STRCASESTR
# include "strcasestr.h"
#endif

#include <FLAC/metadata.h>

int scan_get_flacfileinfo(char *filename, MP3FILE *pmp3)
{
    FLAC__bool ok;
    FLAC__Metadata_Chain *chain;
    FLAC__Metadata_Iterator *iterator;
    FLAC__StreamMetadata *block;
    int rv = -1;
    unsigned int sec, ms;
    FILE *f;

    if (!(f = fopen(filename, "rb"))) {
	DPRINTF(E_WARN,L_SCAN,"Could not open %s for reading\n", filename);
	return -1;
    }
    fseek(f, 0, SEEK_END);
    pmp3->file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fclose(f);

    chain = FLAC__metadata_chain_new();
    if (! chain) {
	DPRINTF(E_WARN,L_SCAN,"Cannot allocate FLAC metadata chain\n");
        return rv;
    }
    if (! FLAC__metadata_chain_read(chain, filename)) {
	DPRINTF(E_WARN,L_SCAN,"Cannot read FLAC metadata from %s\n", filename);
	FLAC__metadata_chain_delete(chain);
	return rv;
    }

    iterator = FLAC__metadata_iterator_new();
    if (! iterator) {
	DPRINTF(E_WARN,L_SCAN,"Cannot allocate FLAC metadata iterator\n");
	FLAC__metadata_chain_delete(chain);
	return rv;
    }

    FLAC__metadata_iterator_init(iterator, chain);
    do {
	block = FLAC__metadata_iterator_get_block(iterator);
	if (block->type == FLAC__METADATA_TYPE_STREAMINFO) {
	    sec = (unsigned int)(block->data.stream_info.total_samples /
				 block->data.stream_info.sample_rate);
	    ms = (unsigned int)(((block->data.stream_info.total_samples %
				  block->data.stream_info.sample_rate) * 1000) /
				block->data.stream_info.sample_rate);
	    if ((sec == 0) && (ms == 0))
		break; /* Info is crap, escape div-by-zero. */
	    pmp3->song_length = (sec * 1000) + ms;
	    pmp3->bitrate = (pmp3->file_size) / (((sec * 1000) + ms) / 8);
	    pmp3->samplerate = block->data.stream_info.sample_rate;
	    rv = 0;
	    break;
	}
    } while (FLAC__metadata_iterator_next(iterator));

    if (rv < 0) {
	DPRINTF(E_WARN,L_SCAN,"Cannot find FLAC metadata in %s\n", filename);
    }

    FLAC__metadata_iterator_delete(iterator);
    FLAC__metadata_chain_delete(chain);
    return rv;
}

#define GET_VORBIS_COMMENT(comment, name, len)				\
        (((strncasecmp(name, (comment).entry, strlen(name)) == 0) &&	\
	  ((comment).entry[strlen(name)] == '=')) ?			\
	 ((*(len) = (comment).length - (strlen(name) + 1)),		\
	  (&((comment).entry[strlen(name) + 1]))) :			\
	 NULL)

int scan_get_flactags(char *filename, MP3FILE *pmp3)
{
    FLAC__bool ok;
    FLAC__Metadata_Chain *chain;
    FLAC__Metadata_Iterator *iterator;
    FLAC__StreamMetadata *block;
    int i;
    char *val;
    size_t len;
    char tmp;
    int found = 0;

    chain = FLAC__metadata_chain_new();
    if (! chain) {
	DPRINTF(E_WARN,L_SCAN,"Cannot allocate FLAC metadata chain\n");
        return 0;
    }
    if (! FLAC__metadata_chain_read(chain, filename)) {
	DPRINTF(E_WARN,L_SCAN,"Cannot read FLAC metadata from %s\n", filename);
	FLAC__metadata_chain_delete(chain);
	return 0;
    }

    iterator = FLAC__metadata_iterator_new();
    if (! iterator) {
	DPRINTF(E_WARN,L_SCAN,"Cannot allocate FLAC metadata iterator\n");
	FLAC__metadata_chain_delete(chain);
	return 0;
    }

    FLAC__metadata_iterator_init(iterator, chain);
    do {
	block = FLAC__metadata_iterator_get_block(iterator);
	if (block->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
	    {
		for (i = 0; i < block->data.vorbis_comment.num_comments; i++) {
		    if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
						 "ARTIST", &len)) {
			if ((pmp3->artist = calloc(len + 1, 1)) != NULL)
			    strncpy(pmp3->artist, val, len);
		    } else if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
							"TITLE", &len)) {
			if ((pmp3->title = calloc(len + 1, 1)) != NULL)
			    strncpy(pmp3->title, val, len);
		    } else if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
							"ALBUM", &len)) {
			if ((pmp3->album = calloc(len + 1, 1)) != NULL)
			    strncpy(pmp3->album, val, len);
		    } else if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
							"GENRE", &len)) {
			if ((pmp3->genre = calloc(len + 1, 1)) != NULL)
			    strncpy(pmp3->genre, val, len);
		    } else if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
							"COMPOSER", &len)) {
			if ((pmp3->composer = calloc(len + 1, 1)) != NULL)
			    strncpy(pmp3->composer, val, len);
		    } else if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
							"COMMENT", &len)) {
			if ((pmp3->comment = calloc(len + 1, 1)) != NULL)
			    strncpy(pmp3->comment, val, len);
		    } else if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
							"TRACKNUMBER", &len)) {
			tmp = *(val + len);
			*(val + len) = '\0';
			pmp3->track = atoi(val);
			*(val + len) = tmp;
		    } else if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
							"DISCNUMBER", &len)) {
			tmp = *(val + len);
			*(val + len) = '\0';
			pmp3->disc = atoi(val);
			*(val + len) = tmp;
		    } else if (val = GET_VORBIS_COMMENT(block->data.vorbis_comment.comments[i],
							"YEAR", &len)) {
			tmp = *(val + len);
			*(val + len) = '\0';
			pmp3->year = atoi(val);
			*(val + len) = tmp;
		    }
		}
		break;
	    }
	    found = 1;
	}
    } while (FLAC__metadata_iterator_next(iterator));

    if (! found) {
	DPRINTF(E_WARN,L_SCAN,"Cannot find FLAC metadata in %s\n", filename);
    }

    FLAC__metadata_iterator_delete(iterator);
    FLAC__metadata_chain_delete(chain);
    return 0;
}
