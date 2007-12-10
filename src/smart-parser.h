/*
 * $Id$
 */

#ifndef _SMART_PARSER_H_
#define _SMART_PARSER_H_

#include "ff-dbstruct.h"

typedef void* PARSETREE;

extern PARSETREE sp_init(void);
extern int sp_parse(PARSETREE tree, char *term, int type);
extern int sp_dispose(PARSETREE tree);
extern char *sp_get_error(PARSETREE tree);
extern char *sp_sql_clause(PARSETREE tree);
int sp_matches_native(PARSETREE tree, MEDIA_NATIVE *pmn);
int sp_matches_string(PARSETREE tree, MEDIA_STRING *pms);

#define SP_TYPE_PLAYLIST 0
#define SP_TYPE_QUERY    1

#endif /* _SMART_PARSER_H_ */

