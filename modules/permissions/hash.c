/*
 * Hash functions for cached trusted table
 *
 * Copyright (C) 2003-2006 Juha Heinanen
 *
 * This file is part of openser, a free SIP server.
 *
 * openser is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * openser is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <sys/types.h>
#include <regex.h>
#include "../../mem/shm_mem.h"
#include "../../parser/parse_from.h"
#include "../../ut.h"
#include "../../hash_func.h"
#include "../../usr_avp.h"
#include "hash.h"
#include "trusted.h"

#define perm_hash(_s)  core_hash( &(_s), 0, PERM_HASH_SIZE)

/* tag AVP specs */
static int     tag_avp_type = 0;
static int_str tag_avp = (int_str)0;
static str     tag_str;


/*
 * Parse and set tag AVP specs
 */
int init_tag_avp(char *tag_avp_param)
{
	if (tag_avp_param && *tag_avp_param) {
		tag_str.s = tag_avp_param;
		tag_str.len = strlen(tag_str.s);
		if (parse_avp_spec( &tag_str, &tag_avp_type, &tag_avp)<0) {
			LOG(L_CRIT,"ERROR:permissions:init_tag_avp: "
				"invalid tag AVP spec \"%s\"\n", tag_avp_param);
			return -1;
		}
	}
	return 0;
}


/*
 * Gets tag avp specs
 */
void get_tag_avp(int_str *tag_avp_p, int *tag_avp_type_p)
{
	*tag_avp_p = tag_avp;
	*tag_avp_type_p = tag_avp_type;
}


/*
 * Create and initialize a hash table
 */
struct trusted_list** new_hash_table(void)
{
	struct trusted_list** ptr;

	/* Initializing hash tables and hash table variable */
	ptr = (struct trusted_list **)shm_malloc
		(sizeof(struct trusted_list*) * PERM_HASH_SIZE);
	if (!ptr) {
		LOG(L_ERR, "new_hash_table(): No memory for hash table\n");
		return 0;
	}

	memset(ptr, 0, sizeof(struct trusted_list*) * PERM_HASH_SIZE);
	return ptr;
}


/*
 * Release all memory allocated for a hash table
 */
void free_hash_table(struct trusted_list** table)
{
	if (!table)
		return;

	empty_hash_table(table);
	shm_free(table);
}


/* 
 * Add <src_ip, proto, pattern, tag> into hash table, where proto is integer
 * representation of string argument proto.
 */
int hash_table_insert(struct trusted_list** hash_table, char* src_ip, 
		      char* proto, char* pattern, char* tag)
{
	struct trusted_list *np;
	unsigned int hash_val;

	np = (struct trusted_list *) shm_malloc(sizeof(*np));
	if (np == NULL) {
		LOG(L_CRIT, "hash_table_insert(): Cannot allocate shm memory "
			"for table entry\n");
		return -1;
	}

	if (strcmp(proto, "any") == 0) {
		np->proto = PROTO_NONE;
	} else if (strcmp(proto, "udp") == 0) {
		np->proto = PROTO_UDP;
	} else if (strcmp(proto, "tcp") == 0) {
		np->proto = PROTO_TCP;
	} else if (strcmp(proto, "tls") == 0) {
		np->proto = PROTO_TLS;
	} else if (strcmp(proto, "sctp") == 0) {
		np->proto = PROTO_SCTP;
	} else if (strcmp(proto, "none") == 0) {
	        shm_free(np);
		return 1;
	} else {
		LOG(L_CRIT, "hash_table_insert(): Unknown protocol\n");
	        shm_free(np);
		return -1;
	}

	np->src_ip.len = strlen(src_ip);
	np->src_ip.s = (char *) shm_malloc(np->src_ip.len);

	if (np->src_ip.s == NULL) {
		LOG(L_CRIT, "hash_table_insert(): Cannot allocate memory for src_ip "
			"string\n");
		shm_free(np);
		return -1;
	}

	(void) strncpy(np->src_ip.s, src_ip, np->src_ip.len);

	if (pattern) {
	    np->pattern = (char *) shm_malloc(strlen(pattern)+1);
	    if (np->pattern == NULL) {
		LOG(L_CRIT, "hash_table_insert(): Cannot allocate memory "
		    "for pattern string\n");
		shm_free(np->src_ip.s);
		shm_free(np);
		return -1;
	    }
	    (void) strcpy(np->pattern, pattern);
	} else {
	    np->pattern = 0;
	}

	if (tag) {
	    np->tag.len = strlen(tag);
	    np->tag.s = (char *) shm_malloc((np->tag.len) + 1);
	    if (np->tag.s == NULL) {
		LOG(L_CRIT, "hash_table_insert(): Cannot allocate memory "
		    "for pattern string\n");
		shm_free(np->src_ip.s);
		shm_free(np->pattern);
		shm_free(np);
		return -1;
	    }
	    (void) strcpy(np->tag.s, tag);
	} else {
	    np->tag.len = 0;
	    np->tag.s = 0;
	}

	hash_val = perm_hash(np->src_ip);
	np->next = hash_table[hash_val];
	hash_table[hash_val] = np;

	return 1;
}


/* 
 * Check if an entry exists in hash table that has given src_ip and protocol
 * value and pattern that matches to From URI.  If, assign 
 */
int match_hash_table(struct trusted_list** table, struct sip_msg* msg)
{
	str uri;
	char uri_string[MAX_URI_SIZE + 1];
	regex_t preg;
	struct trusted_list *np;
	str src_ip;
	int_str val;

	src_ip.s = ip_addr2a(&msg->rcv.src_ip);
	src_ip.len = strlen(src_ip.s);

	if (parse_from_header(msg) < 0) return -1;
	uri = get_from(msg)->uri;
	if (uri.len > MAX_URI_SIZE) {
		LOG(L_ERR, "match_hash_table(): From URI too large\n");
		return -1;
	}
	memcpy(uri_string, uri.s, uri.len);
	uri_string[uri.len] = (char)0;

	for (np = table[perm_hash(src_ip)]; np != NULL; np = np->next) {
	    if ((np->src_ip.len == src_ip.len) && 
		(strncasecmp(np->src_ip.s, src_ip.s, src_ip.len) == 0) &&
		((np->proto == PROTO_NONE) || (np->proto == msg->rcv.proto))) {
		if (!(np->pattern)) goto found;
		if (regcomp(&preg, np->pattern, REG_NOSUB)) {
		    LOG(L_ERR, "match_hash_table(): Error in regular expression\n");
		    return -1;
		}
		if (regexec(&preg, uri_string, 0, (regmatch_t *)0, 0)) {
		    regfree(&preg);
		} else {
		    regfree(&preg);
		    goto found;
		}
	    }
	}
	return -1;
found:
	if (tag_avp.n && np->tag.s) {
	    val.s = np->tag;
	    if (add_avp(tag_avp_type|AVP_VAL_STR, tag_avp, val) != 0) {
		LOG(L_ERR, "match_hash_table(): ERROR: setting of "
		    "tag_avp failed\n");
		return -1;
	    }
	}
	return 1;
}


/* 
 * Print domains stored in hash table 
 */
void hash_table_print(struct trusted_list** hash_table, FILE* reply_file)
{
	int i;
	struct trusted_list *np;

	for (i = 0; i < PERM_HASH_SIZE; i++) {
		np = hash_table[i];
		while (np) {
			fprintf(reply_file, "%4d <%.*s, %d, %s, %s>\n", i,
				np->src_ip.len, ZSW(np->src_ip.s), np->proto,
				np->pattern?np->pattern:"NULL", np->tag.len?np->tag.s:"NULL");
			np = np->next;
		}
	}
}


/* 
 * Free contents of hash table, it doesn't destroy the
 * hash table itself
 */
void empty_hash_table(struct trusted_list **hash_table)
{
	int i;
	struct trusted_list *np, *next;

	for (i = 0; i < PERM_HASH_SIZE; i++) {
		np = hash_table[i];
		while (np) {
			if (np->src_ip.s) shm_free(np->src_ip.s);
			if (np->pattern) shm_free(np->pattern);
			if (np->tag.s) shm_free(np->tag.s);
			next = np->next;
			shm_free(np);
			np = next;
		}
		hash_table[i] = 0;
	}
}
