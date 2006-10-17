/*
 * $Id$
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

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifndef WIN32
#include <netinet/in.h>
#endif

#include "err.h"
#include "mp3-scanner.h"
#include "scan-aac.h"

/* Forwards */
extern time_t scan_aac_mac_to_unix_time(int t);


/* FIXME: This is really just a copy of scan-aac.c
 * there should really be some mpeg4-specific stuff in here,
 * but this seems to do a fair job.  Should check it against
 * .mov files.
 */

/**
 * main mp4 scanning routing.
 *
 * @param filename file to scan
 * @param pmp3 pointer to the MP3FILE to fill with data
 * @returns FALSE if file should not be added to database, TRUE otherwise
 */
int scan_get_mp4info(char *filename, MP3FILE *pmp3) {
    FILE *fin;
    long atom_offset;
    unsigned int atom_length;

    long current_offset=0;
    int current_size;
    char current_atom[4];
    char *current_data;
    unsigned short us_data;
    int genre;
    int len;

    int sample_size;
    int samples;
    unsigned int bit_rate;
    int ms;
    unsigned char buffer[2];
    int time = 0;


    if(!(fin=fopen(filename,"rb"))) {
        DPRINTF(E_INF,L_SCAN,"Cannot open file %s for reading\n",filename);
        return FALSE;
    }

    atom_offset=scan_aac_drilltoatom(fin, "moov:udta:meta:ilst", &atom_length);
    if(atom_offset != -1) {
        /* found the tag section - need to walk through now */
      
        while(current_offset < (long) atom_length) {
            if(fread((void*)&current_size,1,sizeof(int),fin) != sizeof(int))
                break;

            DPRINTF(E_SPAM,L_SCAN,"Current size: %d\n");

            current_size=ntohl(current_size);
                                    
            if(current_size <= 7) /* something not right */
                break;

            if(fread(current_atom,1,4,fin) != 4) 
                break;
            
            DPRINTF(E_SPAM,L_SCAN,"Current Atom: %c%c%c%c\n",
                    current_atom[0],current_atom[1],current_atom[2],
                    current_atom[3]);

            if(current_size > 4096) { /* Does this break anything? */
                /* too big!  cover art, maybe? */
                fseek(fin,current_size - 8,SEEK_CUR);
            } else {
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
                } else if(!memcmp(current_atom,"\xA9" "cmt",4)) {
                    pmp3->comment=strdup((char*)&current_data[16]);
                } else if(!memcmp(current_atom,"\xA9" "wrt",4)) {
                    pmp3->composer=strdup((char*)&current_data[16]);
                } else if(!memcmp(current_atom,"\xA9" "grp",4)) {
                    pmp3->grouping=strdup((char*)&current_data[16]);
                } else if(!memcmp(current_atom,"\xA9" "gen",4)) {
                    /* can this be a winamp genre??? */
                    pmp3->genre=strdup((char*)&current_data[16]);
                } else if(!memcmp(current_atom,"tmpo",4)) {
                    us_data=*((unsigned short *)&current_data[16]);
                    us_data=ntohs(us_data);
                    pmp3->bpm=us_data;
                } else if(!memcmp(current_atom,"trkn",4)) {
                    us_data=*((unsigned short *)&current_data[18]);
                    us_data=ntohs(us_data);
                    
                    pmp3->track=us_data;

                    us_data=*((unsigned short *)&current_data[20]);
                    us_data=ntohs(us_data);

                    pmp3->total_tracks=us_data;
                } else if(!memcmp(current_atom,"disk",4)) {
                    us_data=*((unsigned short *)&current_data[18]);
                    us_data=ntohs(us_data);
                    
                    pmp3->disc=us_data;
                    
                    us_data=*((unsigned short *)&current_data[20]);
                    us_data=ntohs(us_data);
                    
                    pmp3->total_discs=us_data;
                } else if(!memcmp(current_atom,"\xA9" "day",4)) {
                    pmp3->year=atoi((char*)&current_data[16]);
                } else if(!memcmp(current_atom,"gnre",4)) {
                    genre=(int)(*((char*)&current_data[17]));
                    genre--;
                    
                    if((genre < 0) || (genre > WINAMP_GENRE_UNKNOWN))
                        genre=WINAMP_GENRE_UNKNOWN;
                    
                    pmp3->genre=strdup(scan_winamp_genre[genre]);
                } else if (!memcmp(current_atom, "cpil", 4)) {
                    pmp3->compilation = current_data[16];
                }
                
                free(current_data);
                current_offset+=current_size;
            }
        }
    }

    /* got the tag info, now let's get bitrate, etc */
    atom_offset = scan_aac_drilltoatom(fin, "moov:mvhd", &atom_length);
    if(atom_offset != -1) {
        fseek(fin, 4, SEEK_CUR);
        fread((void *)&time, sizeof(int), 1, fin);
        time = ntohl(time);
        pmp3->time_added = (int) scan_aac_mac_to_unix_time(time);

        fread((void *)&time, sizeof(int), 1, fin);
        time = ntohl(time);
        pmp3->time_modified = (int) scan_aac_mac_to_unix_time(time);
        fread((void*)&sample_size,1,sizeof(int),fin);
        fread((void*)&samples,1,sizeof(int),fin);

        sample_size=ntohl(sample_size);
        samples=ntohl(samples);

        /* avoid overflowing on large sample_sizes (90000) */
        ms=1000;
        while((ms > 9) && (!(sample_size % 10))) {
            sample_size /= 10;
            ms /= 10;
        }

        /* DWB: use ms time instead of sec */
        pmp3->song_length=(int)((samples * ms) / sample_size);
        DPRINTF(E_DBG,L_SCAN,"Song length: %d seconds\n", 
                pmp3->song_length / 1000);
    }

    pmp3->bitrate = 0;

    /* Get the sample rate from the 'mp4a' atom (timescale). This is also
       found in the 'mdhd' atom which is a bit closer but we need to 
       navigate to the 'mp4a' atom anyways to get to the 'esds' atom. */
    atom_offset=scan_aac_drilltoatom(fin, 
                                     "moov:trak:mdia:minf:stbl:stsd:mp4a", 
                                     &atom_length);
    if(atom_offset == -1) {
        atom_offset=scan_aac_drilltoatom(fin,
                                         "moov:trak:mdia:minf:stbl:stsd:drms",
                                         &atom_length);
    }

    if (atom_offset != -1) {
        fseek(fin, atom_offset + 32, SEEK_SET);

        /* Timescale here seems to be 2 bytes here (the 2 bytes before it are
         * "reserved") though the timescale in the 'mdhd' atom is 4. Not sure 
         * how this is dealt with when sample rate goes higher than 64K. */
        fread(buffer, sizeof(unsigned char), 2, fin);

        pmp3->samplerate = (buffer[0] << 8) | (buffer[1]);

        /* Seek to end of atom. */
        fseek(fin, 2, SEEK_CUR);

        /* Get the bit rate from the 'esds' atom. We are already positioned
           in the parent atom so just scan ahead. */
        atom_offset = scan_aac_findatom(fin, 
                                        atom_length-(ftell(fin)-atom_offset), 
                                        "esds", &atom_length);

        if (atom_offset != -1) {
            /* Roku Soundbridge seems to believe anything above 320K is
             * an ALAC encoded m4a.  We'll lie on their behalf.
             */
            fseek(fin, atom_offset + 22, SEEK_CUR);
            fread((void *)&bit_rate, sizeof(unsigned int), 1, fin);
            pmp3->bitrate = ntohl(bit_rate) / 1000;
            DPRINTF(E_DBG,L_SCAN,"esds bitrate: %d\n",pmp3->bitrate);

            if(pmp3->bitrate > 320) {
                pmp3->bitrate = 320;
            }
        } else {
            DPRINTF(E_DBG,L_SCAN, "Couldn't find 'esds' atom for bit rate.\n");
        }
    } else {
        DPRINTF(E_DBG,L_SCAN, "Couldn't find 'mp4a' atom for sample rate.\n");
    }

    /* Fallback if we can't find the info in the atoms. */
    if (pmp3->bitrate == 0) {
        /* calculate bitrate from song length... Kinda cheesy */
        DPRINTF(E_DBG,L_SCAN, "Guesstimating bit rate.\n");
        atom_offset=scan_aac_drilltoatom(fin,"mdat",&atom_length);
        if ((atom_offset != -1) && (pmp3->song_length > 1000)) {
            pmp3->bitrate = atom_length / ((pmp3->song_length / 1000) * 128);
        }
    }

    fclose(fin);

    pmp3->has_video=1;

    return TRUE;  /* we'll return as much as we got. */
}
