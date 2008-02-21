/**
 * $id: $
 *
 * Common includes
 */

#ifndef _INCLUDES_H_
#define _INCLUDES_H_

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <id3tag.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_DIRENT_H
# include <dirent.h>
#endif
#ifndef WIN32
#include <unistd.h>

# include <netinet/in.h>  /* htons and friends */
#endif
#include <sys/stat.h>


#endif /* _INCLUDES_H_ */

