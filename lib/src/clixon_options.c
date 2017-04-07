/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * CLICON options
 * See clicon_tutorial appendix and clicon.conf.cpp.cpp on documentation of
 * options
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_chunk.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_options.h"

/*
 * clicon_option_dump
 * Print registry on file. For debugging.
 */
void
clicon_option_dump(clicon_handle h, int dbglevel)
{
    clicon_hash_t *hash = clicon_options(h);
    int            i;
    char         **keys;
    void          *val;
    size_t         klen;
    size_t         vlen;
    
    if (hash == NULL)
	return;
    keys = hash_keys(hash, &klen);
    if (keys == NULL)
	return;
	
    for(i = 0; i < klen; i++) {
	val = hash_value(hash, keys[i], &vlen);
	if (vlen){
	    if (((char*)val)[vlen-1]=='\0') /* assume string */
		clicon_debug(dbglevel, "%s =\t \"%s\"", keys[i], (char*)val);
	    else
		clicon_debug(dbglevel, "%s =\t 0x%p , length %zu", keys[i], val, vlen);
	}
	else
	    clicon_debug(dbglevel, "%s = NULL", keys[i]);
    }
    free(keys);

}

/*! Read filename and set values to global options registry
 */
static int 
clicon_option_readfile(clicon_hash_t *copt, const char *filename)
{
    struct stat st;
    char opt[1024], val[1024];
    char line[1024], *cp;
    FILE *f;
    int retval = -1;

    if (filename == NULL || !strlen(filename)){
	clicon_err(OE_UNIX, 0, "Not specified");
	return -1;
    }
    if (stat(filename, &st) < 0){
	clicon_err(OE_UNIX, errno, "%s", filename);
	return -1;
    }
    if (!S_ISREG(st.st_mode)){
	clicon_err(OE_UNIX, 0, "%s is not a regular file", filename);
	return -1;
    }
    if ((f = fopen(filename, "r")) == NULL) {
	clicon_err(OE_UNIX, errno, "configure file: %s", filename);
	return -1;
    }
    clicon_debug(2, "Reading config file %s", __FUNCTION__, filename);
    while (fgets(line, sizeof(line), f)) {
	if ((cp = strchr(line, '\n')) != NULL) /* strip last \n */
	    *cp = '\0';
	/* Trim out comments, strip whitespace, and remove CR */
	if ((cp = strchr(line, '#')) != NULL)
	    memcpy(cp, "\n", 2);
	if (sscanf(line, "%s %s", opt, val) < 2)
	    continue;
	if ((hash_add(copt, 
		      opt,
		      val,
		      strlen(val)+1)) == NULL)
	    goto catch;
    }
    retval = 0;
  catch:
    fclose(f);
    return retval;
}

/*! Set default values of some options that may not appear in config-file
 */
static int
clicon_option_default(clicon_hash_t  *copt)
{
    char           *val;
    int             retval = 0;

    if (!hash_lookup(copt, "CLICON_YANG_MODULE_MAIN")){
	if (hash_add(copt, "CLICON_YANG_MODULE_MAIN", "clicon", strlen("clicon")+1) < 0)
	    goto catch;
    }
    if (!hash_lookup(copt, "CLICON_SOCK_GROUP")){
	val = CLICON_SOCK_GROUP;
	if (hash_add(copt, "CLICON_SOCK_GROUP", val, strlen(val)+1) < 0)
	    goto catch;
    }
    if (!hash_lookup(copt, "CLICON_CLI_MODE")){
	if (hash_add(copt, "CLICON_CLI_MODE", "base", strlen("base")+1) < 0)
	    goto catch;
    }
    if (!hash_lookup(copt, "CLICON_MASTER_PLUGIN")){
	val = CLICON_MASTER_PLUGIN;
	if (hash_add(copt, "CLICON_MASTER_PLUGIN", val, strlen(val)+1) < 0)
	    goto catch;
    }
    if (!hash_lookup(copt, "CLICON_CLI_GENMODEL")){
	if (hash_add(copt, "CLICON_CLI_GENMODEL", "1", strlen("1")+1) < 0)
	    goto catch;
    }
    if (!hash_lookup(copt, "CLICON_CLI_GENMODEL_TYPE")){
	if (hash_add(copt, "CLICON_CLI_GENMODEL_TYPE", "VARS", strlen("VARS")+1) < 0)
	    goto catch;
    }
    if (!hash_lookup(copt, "CLICON_AUTOCOMMIT")){
	if (hash_add(copt, "CLICON_AUTOCOMMIT", "0", strlen("0")+1) < 0)
	    goto catch;
    }
    /* Legacy is 1 but default should really be 0. New apps should use 0 */
    if (!hash_lookup(copt, "CLICON_CLI_VARONLY")){
	if (hash_add(copt, "CLICON_CLI_VARONLY", "1", strlen("1")+1) < 0)
	    goto catch;
    }
    if (!hash_lookup(copt, "CLICON_CLI_GENMODEL_COMPLETION")){
	if (hash_add(copt, "CLICON_CLI_GENMODEL_COMPLETION", "0", strlen("0")+1) < 0)
	    goto catch;
    }
    retval = 0;
  catch:
    unchunk_group(__FUNCTION__);
    return retval;
}

/*! Check that options are set
 */
static int 
clicon_option_sanity(clicon_hash_t *copt)
{
    int retval = -1;

    if (!hash_lookup(copt, "CLICON_CLI_DIR")){
	clicon_err(OE_UNIX, 0, "CLICON_CLI_DIR not defined in config file");
	goto done;
    }
    if (!hash_lookup(copt, "CLICON_CLISPEC_DIR")){
	clicon_err(OE_UNIX, 0, "CLICON_CLISPEC_DIR not defined in config file");
	goto done;
    }
    if (!hash_lookup(copt, "CLICON_BACKEND_DIR")){
	clicon_err(OE_UNIX, 0, "CLICON_BACKEND_PIDFILE not defined in config file");
	goto done;
    }
    if (!hash_lookup(copt, "CLICON_NETCONF_DIR")){
	clicon_err(OE_UNIX, 0, "CLICON_NETCONF_DIR not defined in config file");
	goto done;
    }
    if (!hash_lookup(copt, "CLICON_RESTCONF_DIR")){
	clicon_err(OE_UNIX, 0, "CLICON_RESTCONF_DIR not defined in config file");
	goto done;
    }
    if (!hash_lookup(copt, "CLICON_YANG_DIR")){
	clicon_err(OE_UNIX, 0, "CLICON_YANG_DIR not defined in config file");
	goto done;
    }
    if (!hash_lookup(copt, "CLICON_ARCHIVE_DIR")){
	clicon_err(OE_UNIX, 0, "CLICON_ARCHIVE_DIR not defined in config file");
	goto done;
    }
    if (!hash_lookup(copt, "CLICON_SOCK")){
	clicon_err(OE_UNIX, 0, "CLICON_SOCK not defined in config file");
	goto done;
    }
    if (!hash_lookup(copt, "CLICON_BACKEND_PIDFILE")){
	clicon_err(OE_UNIX, 0, "CLICON_BACKEND_PIDFILE not defined in config file");
	goto done;
    }
    retval = 0;
 done:
    return retval;
}


/*! Initialize option values
 *
 * Set default options, Read config-file, Check that all values are set.
 * @param[in]  h  clicon handle
 */
int
clicon_options_main(clicon_handle h)
{
    int            retval = -1;
    char          *configfile;
    clicon_hash_t *copt = clicon_options(h);

    /*
     * Set configure file if not set by command-line above
     */
    if (!hash_lookup(copt, "CLICON_CONFIGFILE")){ 
	clicon_err(OE_CFG, 0, "CLICON_CONFIGFILE (-f) not set");
	goto done;
    }
    configfile = hash_value(copt, "CLICON_CONFIGFILE", NULL);
    clicon_debug(1, "CLICON_CONFIGFILE=%s", configfile);
    /* Set default options */
    if (clicon_option_default(copt) < 0)  /* init registry from file */
	goto done;

    /* Read configfile */
    if (clicon_option_readfile(copt, configfile) < 0)
	goto done;

    if (clicon_option_sanity(copt) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
    
}

/*! Check if a clicon option has a value
 */
int
clicon_option_exists(clicon_handle h, const char *name)
{
    clicon_hash_t *copt = clicon_options(h);

    return (hash_lookup(copt, (char*)name) != NULL);
}

/*! Get a single string option string via handle
 *
 * @param[in] h       clicon_handle
 * @param[in] name    option name
 * @retval    NULL    If option not found, or value of option is NULL
 * @retval    string  value of option if found
 * clicon options should be strings.
 * @note To differentiate the two reasons why NULL may be returned, use function 
 * clicon_option_exists() before the call
 */
char *
clicon_option_str(clicon_handle h, 
		  const char   *name)
{
    clicon_hash_t *copt = clicon_options(h);

    if (hash_lookup(copt, (char*)name) == NULL)
	return NULL;
    return hash_value(copt, (char*)name, NULL);
}

/*! Set a single string option via handle 
 * @param[in] h       clicon_handle
 * @param[in] name    option name
 * @param[in] val     option value, must be null-terminated string
 */
int
clicon_option_str_set(clicon_handle h, 
		      const char   *name, 
		      char         *val)
{
    clicon_hash_t *copt = clicon_options(h);

    return hash_add(copt, (char*)name, val, strlen(val)+1)==NULL?-1:0;
}

/*! Get options as integer but stored as string

 * @param   h    clicon handle
 * @param   name name of option
 * @retval  int  An integer as aresult of atoi
 * @retval  -1   If option does not exist
 * @code
 *  if (clicon_option_exists(h, "X")
 *	return clicon_option_int(h, "X");
 *  else
 *      return 0;
 * @endcode
 * Note that -1 can be both error and value.
 * This means that it should be used together with clicon_option_exists() and
 * supply a defualt value as shown in the example.
 */
int
clicon_option_int(clicon_handle h, const char *name)
{
    char *s;

    if ((s = clicon_option_str(h, name)) == NULL)
	return -1;
    return atoi(s);
}

/*! set option given as int.
 */
int
clicon_option_int_set(clicon_handle h, const char *name, int val)
{
    char s[64];
    
    if (snprintf(s, sizeof(s)-1, "%u", val) < 0)
	return -1;
    return clicon_option_str_set(h, name, s);
}

/*! delete option 
 */
int
clicon_option_del(clicon_handle h, const char *name)
{
    clicon_hash_t *copt = clicon_options(h);

    return hash_del(copt, (char*)name);
}

/*-----------------------------------------------------------------
 * Specific option access functions.
 * These should be commonly accessible for all users of clicon lib 
 *-----------------------------------------------------------------*/

char *
clicon_configfile(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_CONFIGFILE");
}


/*! YANG database specification directory */
char *
clicon_yang_dir(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_YANG_DIR");
}

/*! YANG main module */
char *
clicon_yang_module_main(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_YANG_MODULE_MAIN");
}

/*! YANG revision */
char *
clicon_yang_module_revision(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_YANG_MODULE_REVISION");
}

char *
clicon_backend_dir(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_BACKEND_DIR");
}

/* contains .so files CLICON_CLI_DIR */
char *
clicon_cli_dir(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_CLI_DIR");
}

/* contains .cli files - CLICON_CLISPEC_DIR */
char *
clicon_clispec_dir(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_CLISPEC_DIR");
}

char *
clicon_netconf_dir(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_NETCONF_DIR");
}

char *
clicon_restconf_dir(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_RESTCONF_DIR");
}

char *
clicon_archive_dir(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_ARCHIVE_DIR");
}

/* get family of backend socket: AF_UNIX, AF_INET or AF_INET6 */
int
clicon_sock_family(clicon_handle h)
{
    char *s;

    if ((s = clicon_option_str(h, "CLICON_SOCK_FAMILY")) == NULL)
	return AF_UNIX;
    else  if (strcmp(s, "IPv4")==0)
	return AF_INET;
    else  if (strcmp(s, "IPv6")==0)
	return AF_INET6;
    else
	return AF_UNIX; /* default */
}

/*! Get information about socket: unix domain filepath, or addr:path */
char *
clicon_sock(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_SOCK");
}

/*! Get port for backend socket in case of AF_INET or AF_INET6 */
int
clicon_sock_port(clicon_handle h)
{
    char *s;

    if ((s = clicon_option_str(h, "CLICON_SOCK_PORT")) == NULL)
	return -1;
    return atoi(s);
}

char *
clicon_backend_pidfile(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_BACKEND_PIDFILE");
}

char *
clicon_sock_group(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_SOCK_GROUP");
}

char *
clicon_master_plugin(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_MASTER_PLUGIN");
}

/* return initial clicon cli mode */
char *
clicon_cli_mode(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_CLI_MODE");
}

/*! Whether to generate CLIgen syntax from datamodel or not (0 or 1)
 * Must be used with a previous clicon_option_exists().
 */
int
clicon_cli_genmodel(clicon_handle h)
{
    char const *opt = "CLICON_CLI_GENMODEL";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/* How to generate and show CLI syntax: VARS|ALL */
enum genmodel_type
clicon_cli_genmodel_type(clicon_handle h)
{
    char *s;
    enum genmodel_type gt = GT_ERR;

    if (!clicon_option_exists(h, "CLICON_CLI_GENMODEL_TYPE")){
	gt = GT_VARS;
	goto done;
    }
    s = clicon_option_str(h, "CLICON_CLI_GENMODEL_TYPE");
    if (strcmp(s, "NONE")==0)
	gt = GT_NONE;
    else
    if (strcmp(s, "VARS")==0)
	gt = GT_VARS;
    else
    if (strcmp(s, "ALL")==0)
	gt = GT_ALL;
  done:
    return gt;
}


/* eg -q option, dont print notifications on stdout */
char *
clicon_quiet_mode(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_QUIET");
}

int
clicon_autocommit(clicon_handle h)
{
    char const *opt = "CLICON_AUTOCOMMIT";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

int
clicon_autocommit_set(clicon_handle h, int val)
{
    return clicon_option_int_set(h, "CLICON_AUTOCOMMIT", val);
}

/*! Dont include keys in cvec in cli vars callbacks
 */
int
clicon_cli_varonly(clicon_handle h)
{
    char const *opt = "CLICON_CLI_VARONLY";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

int
clicon_cli_varonly_set(clicon_handle h, int val)
{
    return clicon_option_int_set(h, "CLICON_CLI_VARONLY", val);
}

int
clicon_cli_genmodel_completion(clicon_handle h)
{
    char const *opt = "CLICON_CLI_GENMODEL_COMPLETION";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/* Where are "running" and "candidate" databases? */
char *
clicon_xmldb_dir(clicon_handle h)
{
    return clicon_option_str(h, "CLICON_XMLDB_DIR");
}

/*! Get YANG specification
 * Must use hash functions directly since they are not strings.
 */
yang_spec *
clicon_dbspec_yang(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *p;

    if ((p = hash_value(cdat, "dbspec_yang", &len)) != NULL)
	return *(yang_spec **)p;
    return NULL;
}

/* 
 * Set dbspec (YANG variant)
 * ys must be a malloced pointer
 */
int
clicon_dbspec_yang_set(clicon_handle h, struct yang_spec *ys)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to ys that should be copied by hash,
       so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "dbspec_yang", &ys, sizeof(ys)) == NULL)
	return -1;
    return 0;
}

/* 
 * Get dbspec name as read from spec. Can be used in CLI '@' syntax.
 * XXX: this we muśt change,...
 */
char *
clicon_dbspec_name(clicon_handle h)
{
    if (!clicon_option_exists(h, "dbspec_name"))
	return NULL;
    return clicon_option_str(h, "dbspec_name");
}

/* 
 * Set dbspec name as read from spec. Can be used in CLI '@' syntax.
 */
int
clicon_dbspec_name_set(clicon_handle h, char *name)
{
    return clicon_option_str_set(h, "dbspec_name", name);
}
