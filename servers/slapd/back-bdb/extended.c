/* extended.c - bdb backend extended routines */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2003 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "back-bdb.h"
#include "external.h"
#include "lber_pvt.h"

static struct exop {
	struct berval *oid;
	BI_op_extended	*extended;
} exop_table[] = {
	{ (struct berval *)&slap_EXOP_MODIFY_PASSWD, bdb_exop_passwd },
	{ NULL, NULL }
};

int
bdb_extended( Operation *op, SlapReply *rs )
/*	struct berval		*reqoid,
	struct berval	*reqdata,
	char		**rspoid,
	struct berval	**rspdata,
	LDAPControl *** rspctrls,
	const char**	text,
	BerVarray	*refs 
) */
{
	int i;

	for( i=0; exop_table[i].extended != NULL; i++ ) {
		if( ber_bvcmp( exop_table[i].oid, &op->oq_extended.rs_reqoid ) == 0 ) {
			return (exop_table[i].extended)( op, rs );
		}
	}

	rs->sr_text = "not supported within naming context";
	return LDAP_UNWILLING_TO_PERFORM;
}

