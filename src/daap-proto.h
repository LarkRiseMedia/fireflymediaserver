/*
 * $Id$
 * Helper functions for formatting a daap message
 *
 * Copyright (C) 2003 Ron Pedde (ron@corbey.com)
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

#ifndef _DAAP_PROTO_H_
#define _DAAP_PROTO_H_


typedef struct daap_block_tag {
    char tag[4];
    int reported_size;
    int size;
    int free;
    char *value;
    char svalue[4]; /* for statics up to 4 bytes */
    struct daap_block_tag *parent;
    struct daap_block_tag *children;
    struct daap_block_tag *next;
} DAAP_BLOCK;

DAAP_BLOCK *daap_add_int(DAAP_BLOCK *parent, char *tag, int value);
DAAP_BLOCK *daap_add_data(DAAP_BLOCK *parent, char *tag, int len, void *value);
DAAP_BLOCK *daap_add_string(DAAP_BLOCK *parent, char *tag, char *value);
DAAP_BLOCK *daap_add_empty(DAAP_BLOCK *parent, char *tag);
DAAP_BLOCK *daap_add_char(DAAP_BLOCK *parent, char *tag, char value);
DAAP_BLOCK *daap_add_short(DAAP_BLOCK *parent, char *tag, short int value);
DAAP_BLOCK *daap_add_long(DAAP_BLOCK *parent, char *tag, int v1, int v2);
int daap_serialize(DAAP_BLOCK *root, int fd, int gzip);
int daap_free(DAAP_BLOCK *root);

#endif

