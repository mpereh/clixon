/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#define __USE_GNU /* For RTLD_DEFAULT */
#include <dlfcn.h>
#include <libgen.h>
#include <grp.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* clicon_cli */
#include "clixon_cli_api.h"
#include "cli_plugin.h"
#include "cli_handle.h"
#include "cli_generate.h"

/*
 * Constants
 */
#define CLI_DEFAULT_PROMPT      "cli> "

/*
 *
 * CLI PLUGIN INTERFACE, INTERNAL SECTION
 *
 */

/*! Generate CLIgen parse tree for syntax mode 
 *
 * @param[in]   h     Clicon handle
 * @param[in]   m     Syntax mode struct
 */
static int
gen_parse_tree(clicon_handle  h,
               char          *name,
               parse_tree    *pt,
               pt_head      **php)
{
    int       retval = -1;
    pt_head  *ph;
    
    if ((ph = cligen_ph_add(cli_cligen(h), name)) == NULL)
        goto done;
    if (cligen_ph_parsetree_set(ph, pt) < 0)
        goto done;
    if (cligen_ph_prompt_set(ph, CLI_DEFAULT_PROMPT) < 0){
        clicon_err(OE_UNIX, errno, "cligen_ph_prompt_set");
        goto done;
    }
    *php = ph;
    retval = 0;
 done:
    return retval;
}

/*! Dynamic linking loader string to function mapper
 *
 * Maps strings from the CLI specification file to real funtions using dlopen 
 * mapping. 
 * First look for function name in local namespace if handle given (given plugin)
 * Then check global namespace, i.e.m lib*.so
 * 
 * @param[in]  name    Name of function
 * @param[in]  handle  Handle to plugin .so module  as returned by dlopen
 * @param[out] error   Static error string, if set indicates error
 * @retval     fn      Function pointer
 * @retval     NULL    Function not found or symbol NULL (check error for proper handling)
 * @see see cli_plugin_load where (optional) handle opened
 * @note the returned function is not type-checked which may result in segv at runtime
 */
void *
clixon_str2fn(char  *name, 
              void  *handle, 
              char **error)
{
    void *fn = NULL;
        
    /* Reset error */
    *error = NULL;
    /* Special check for auto-cli. If the virtual callback is used, it should be overwritten later 
     * by a callback given in the clispec, eg: set @datamodel, cli_set();
     */
    if (strcmp(name, GENERATE_CALLBACK) == 0)
        return NULL;

    /* First check given plugin if any */
    if (handle) {
        dlerror();      /* Clear any existing error */
        fn = dlsym(handle, name);
        if ((*error = (char*)dlerror()) == NULL)
            return fn;  /* If no error we found the address of the callback */
    }

    /* Now check global namespace which includes any shared object loaded
     * into the global namespace. I.e. all lib*.so as well as the 
     * master plugin if it exists 
     */
    dlerror();  /* Clear any existing error */
    /* RTLD_DEFAULT instead of NULL for linux + FreeBSD:
     * Use default search algorithm. Thanks jdl@netgate.com */
    fn = dlsym(RTLD_DEFAULT, name);
    if ((*error = (char*)dlerror()) == NULL)
        return fn;  /* If no error we found the address of the callback */

    /* Return value not really relevant here as the error string is set to
     * signal an error. However, just checking the function pointer for NULL
     * should work in most cases, although it's not 100% correct. 
     */
   return NULL; 
}

/*! Load a file containing clispec syntax and append to specified modes, also load C plugin
 *
 * First load CLIgen file, 
 * Then find which .so to load by looking in the "CLICON_PLUGIN" variable in that file.
 * Make a lookup of plugins already loaded and resolve callbacks from cligen trees to
 * dl symbols in the plugin.
 * @param[in]  h        Clixon handle
 * @param[in]  filename Name of file where syntax is specified (in syntax-group dir)
 * @param[in]  dir      Name of dir, or NULL
 * @param[out] ptall    Universal CLIgen parse tree: apply to all modes
 * @param[out] modes    Keep track of all modes
 * @see clixon_plugins_load  Where .so plugin code has been loaded prior to this
 */
static int
clispec_load_file(clicon_handle h,
                  const char   *filename,
                  const char   *dir,
                  parse_tree   *ptall,
                  cvec         *modes)
{
    void          *handle = NULL;  /* Handle to plugin .so module */
    char          *mode = NULL;    /* Name of syntax mode to append new syntax */
    parse_tree    *pt = NULL;
    int            retval = -1;
    FILE          *f;
    char           filepath[MAXPATHLEN];
    cvec          *cvv = NULL;
    char          *prompt = NULL;
    char         **vec = NULL;
    int            i;
    int            nvec;
    char          *plgnam;
    pt_head           *ph;
#ifndef CLIXON_STATIC_PLUGINS
    clixon_plugin_t *cp;
#endif

    if ((pt = pt_new()) == NULL){
        clicon_err(OE_UNIX, errno, "pt_new");
        goto done;
    }
    if (dir)
        snprintf(filepath, MAXPATHLEN-1, "%s/%s", dir, filename);
    else
        snprintf(filepath, MAXPATHLEN-1, "%s", filename);
    if ((cvv = cvec_new(0)) == NULL){
        clicon_err(OE_PLUGIN, errno, "cvec_new");
        goto done;
    }
    /* Build parse tree from syntax spec. */
    if ((f = fopen(filepath, "r")) == NULL){
        clicon_err(OE_PLUGIN, errno, "fopen %s", filepath);
        goto done;
    }

    /* Assuming this plugin is first in queue */
    if (clispec_parse_file(h, f, filepath, NULL, pt, cvv) < 0){
        clicon_err(OE_PLUGIN, 0, "failed to parse cli file %s", filepath);
        fclose(f);
        goto done;
    }
    fclose(f);
    /* Get CLICON specific global variables:
     *  CLICON_MODE: which mode(s) this syntax applies to
     *  CLICON_PROMPT: Cli prompt in this mode (see cli_prompt_get)
     *  CLICON_PLUGIN: Name of C API plugin
     *  CLICON_PIPETREE: terminals are automatically expanded with this tree
     * Note: the base case is that it is:
     *   (1) a single mode or 
     *   (2) "*" all modes or "m1:m2" - a list of modes
     * but for (2), prompt and plgnam may have unclear semantics
     */
    mode = cvec_find_str(cvv, "CLICON_MODE");
    prompt = cvec_find_str(cvv, "CLICON_PROMPT");
    plgnam = cvec_find_str(cvv, "CLICON_PLUGIN");

#ifndef CLIXON_STATIC_PLUGINS
    if (plgnam != NULL) { /* Find plugin for callback resolving */
        if ((cp = clixon_plugin_find(h, plgnam)) != NULL)
            handle = clixon_plugin_handle_get(cp);
        if (handle == NULL){
            clicon_err(OE_PLUGIN, 0, "CLICON_PLUGIN set to '%s' in %s but plugin %s.so not found in %s", 
                       plgnam, filename, plgnam, 
                       clicon_cli_dir(h));
            goto done;
        }
    }
#endif

    /* Resolve callback names to function pointers. */
    if (cligen_callbackv_str2fn(pt, (cgv_str2fn_t*)clixon_str2fn, handle) < 0){     
        clicon_err(OE_PLUGIN, 0, "Mismatch between CLIgen file '%s' and CLI plugin file '%s'. Some possible errors:\n\t1. A function given in the CLIgen file does not exist in the plugin (ie link error)\n\t2. The CLIgen spec does not point to the correct plugin .so file (CLICON_PLUGIN=\"%s\" is wrong)", 
                   filename, plgnam, plgnam);
        goto done;
    }
     if (cligen_expandv_str2fn(pt, (expandv_str2fn_t*)clixon_str2fn, handle) < 0)     
         goto done;
     /* Variable translation functions */
     if (cligen_translate_str2fn(pt, (translate_str2fn_t*)clixon_str2fn, handle) < 0)     
         goto done;

    /* Make sure we have a syntax mode specified */
    if (mode == NULL || strlen(mode) < 1) { /* may be null if not given in file */
        mode = clicon_cli_mode(h);
        if (mode == NULL || strlen(mode) < 1) { /* may be null if not given in file */  
            clicon_err(OE_PLUGIN, 0, "No syntax mode specified in %s", filepath);
            goto done;
        }
    }
    /* Find all modes in CLICON_MODE string: where to append the pt syntax tree */
    if ((vec = clicon_strsep(mode, ":", &nvec)) == NULL) 
        goto done;

    if (nvec == 1 && strcmp(vec[0], "*") == 0){
        /* Special case: Add this to all modes. Add to special "universal" syntax
         * and add to all syntaxes after all files have been loaded. At this point
         * all modes may not be known (not yet loaded)
         */
        if (cligen_parsetree_merge(ptall, NULL, pt) < 0){
            clicon_err(OE_PLUGIN, errno, "cligen_parsetree_merge");
            goto done;
        }
    }
    else {
        for (i = 0; i < nvec; i++) {
            char             *name = vec[i];
            parse_tree       *ptnew = NULL;
            cg_var           *cv;
            if ((ph = cligen_ph_find(cli_cligen(h), name)) == NULL){
                if ((ptnew = pt_new()) == NULL){
                    clicon_err(OE_UNIX, errno, "pt_new");
                    goto done;
                }
                if (gen_parse_tree(h, name, ptnew, &ph) < 0)
                    goto done;
                if (ph == NULL)
                    goto done;
                if ((cv = cv_new(CGV_STRING)) == NULL){
                    clicon_err(OE_UNIX, errno, "cv_new");
                    goto done;
                }
                cv_string_set(cv, name);
                if (cvec_append_var(modes, cv) < 0){
                    clicon_err(OE_UNIX, errno, "cvec_append_var");
                    goto done;
                }
                if (cv)
                    cv_free(cv);
            }
            if (cligen_parsetree_merge(cligen_ph_parsetree_get(ph), NULL, pt) < 0){
                clicon_err(OE_PLUGIN, errno, "cligen_parsetree_merge");
                goto done;
            }
            if (prompt){
                if (cligen_ph_prompt_set(ph, prompt) < 0){
                    clicon_err(OE_UNIX, errno, "cligen_ph_prompt_set");
                    return -1;
                }
            }
        }
    }
    cligen_parsetree_free(pt, 1);
    retval = 0;
done:
    if (cvv)
        cvec_free(cvv);
    if (vec)
        free(vec);
    return retval;
}

/*! CLIgen spec syntax files and create CLIgen trees to drive the CLI syntax generator
 *
 * CLI .so plugins have been loaded: syntax table in place.
 * Now load cligen syntax files and create cligen pt trees.
 * @param[in]     h       Clicon handle
 * XXX The parsetree loading needs a rewrite for multiple parse-trees
 */
int
clispec_load(clicon_handle h)
{
    int                retval = -1;
    char              *clispec_dir = NULL;
    char              *clispec_file = NULL;
    int                ndp;
    int                i;
    struct dirent     *dp = NULL;
    cligen_susp_cb_t  *fns = NULL;
    cligen_interrupt_cb_t *fni = NULL;
    clixon_plugin_t     *cp;
    parse_tree        *ptall = NULL; /* Universal CLIgen parse tree all modes */
    cvec              *modes = NULL; /* Keep track of created modes */
    pt_head           *ph;
    cg_var            *cv = NULL;

    if ((ptall = pt_new()) == NULL){
        clicon_err(OE_UNIX, errno, "pt_new");
        goto done;
    }
    if ((modes = cvec_new(0)) == NULL){
        clicon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    /* Format plugin directory path */
    clispec_dir = clicon_clispec_dir(h);
    clispec_file = clicon_option_str(h, "CLICON_CLISPEC_FILE");

    /* Load single specific clispec file */
    if (clispec_file){
        if (clispec_load_file(h, clispec_file, NULL, ptall, modes) < 0)
            goto done;
    }
    /* Load all clispec .cli files in directory */
    if (clispec_dir){
        /* Get directory list of files */
        if ((ndp = clicon_file_dirent(clispec_dir, &dp, "(.cli)$", S_IFREG)) < 0)
            goto done;
        /* Load the syntax parse trees into cli_syntax stx structure */
        for (i = 0; i < ndp; i++) {
            clicon_debug(CLIXON_DBG_DEFAULT, "Loading clispec syntax: '%s/%s'", 
                         clispec_dir, dp[i].d_name);
            if (clispec_load_file(h, dp[i].d_name, clispec_dir, ptall, modes) < 0)
                goto done;
        }
    }
    /* Were any syntax modes successfully loaded? If not, leave */
    if (cvec_len(modes) == 0)
        goto ok;
    /* Go thorugh all modes and :
     * 1) Add the universal syntax 
     * 2) add syntax tree (of those modes - "activate" syntax from stx to CLIgen)
     */
    cv = NULL;
    while ((cv = cvec_each(modes, cv)) != NULL){
        if ((ph = cligen_ph_find(cli_cligen(h), cv_string_get(cv))) == NULL)
            continue;

        if (cligen_parsetree_merge(cligen_ph_parsetree_get(ph),
                                   NULL,
                                   ptall) < 0){
            clicon_err(OE_PLUGIN, errno, "cligen_parsetree_merge");
            goto done;
        }
    }
    /* Set susp and interrupt callbacks into  CLIgen */
    cp = NULL;
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
        if (fns==NULL && (fns = clixon_plugin_api_get(cp)->ca_suspend) != NULL)
            if (cli_susp_hook(h, fns) < 0)
                goto done;
        if (fni==NULL && (fni = clixon_plugin_api_get(cp)->ca_interrupt) != NULL)
            if (cli_interrupt_hook(h, fni) < 0)
                goto done;
    }
 ok:
    /* All good. We can now proudly return a new group */
    retval = 0;
done:
    cligen_parsetree_free(ptall, 1);
    if (modes)
        cvec_free(modes);
    if (dp)
        free(dp);
    return retval;
}

/*! Free resources in plugin
 * @param[in]     h       Clicon handle
 */
int
cli_plugin_finish(clicon_handle h)
{
    return 0;
}

/*! Help function to print a meaningful error string. 
 * Sometimes the libraries specify an error string, if so print that.
 * Otherwise just print 'command error'.
 * But do not print it if error is already logged in eg clicon_err() using STDERR logging
 * See eg https://github.com/clicon/clixon/issues/325
 * @param[in]  f   File handler to write error to.
 */
int 
cli_handler_err(FILE *f)
{
    if (clicon_errno){
        /* Check if error is already logged on stderr */
        if ((clicon_get_logflags() & CLICON_LOG_STDERR) == 0){
            fprintf(f,  "%s: %s", clicon_strerror(clicon_errno), clicon_err_reason);
            if (clicon_suberrno)
                fprintf(f, ": %s", strerror(clicon_suberrno));
            fprintf(f,  "\n");
        }
        else
            fprintf(f, "CLI command error\n");
    }
    return 0;
}

/*! Given a command string, parse and if match single command, eval it.
 * Parse and evaluate the string according to
 * the syntax parse tree of the syntax mode specified by *mode.
 * If there is no match in the tree for the command, the parse hook 
 * will be called to see if another mode should be evaluated. If a
 * match is found in another mode, the mode variable is updated to point at 
 * the new mode string.
 *
 * @param[in]     h           Clicon handle
 * @param[in]     cmd         Command string
 * @param[in,out] modenamep   Pointer to the mode string pointer
 * @param[out]    result      CLIgen match result, < 0: errors, >=0 number of matches
 * @param[out]    evalres     Evaluation result if result=1
 * @retval  0     OK
 * @retval  -1    Error
 */
int
clicon_parse(clicon_handle  h, 
             char          *cmd, 
             char         **modenamep, 
             cligen_result *result,          
             int           *evalres)
{
    int               retval = -1;
    char             *modename;
    int               ret;
    parse_tree       *pt;     /* Orig */
    cg_obj           *match_obj = NULL;
    cvec             *cvv = NULL;
    cg_callback      *callbacks = NULL;
    FILE             *f;
    char             *reason = NULL;
    cligen_handle     ch;
    pt_head          *ph;
    
    ch = cli_cligen(h);
    if (clicon_get_logflags()&CLICON_LOG_STDOUT)
        f = stdout;
    else
        f = stderr;
    modename = *modenamep;
    ph = cligen_ph_find(cli_cligen(h), modename);
    if (ph != NULL){
        if (cligen_ph_active_set_byname(ch, modename) < 0){
            fprintf(f, "No such parse-tree registered: %s\n", modename);
            goto done;
        }
        if ((pt = cligen_pt_active_get(ch)) == NULL){
            fprintf(f, "No such parse-tree registered: %s\n", modename);
            goto done;
        }
        if (cliread_parse(ch, cmd, pt, &match_obj, &cvv, &callbacks, result, &reason) < 0)
            goto done;
        /* Debug command and result code */
        clicon_debug(1, "%s result:%d command: \"%s\"", __FUNCTION__, *result, cmd);
        switch (*result) {
        case CG_EOF: /* eof */
        case CG_ERROR: 
            fprintf(f, "CLI parse error: %s\n", cmd); // In practice never happens
            break;
        case CG_NOMATCH: /* no match */
            fprintf(f, "CLI syntax error: \"%s\": %s\n", cmd, reason);
            break;
        case CG_MATCH:
            if (strcmp(modename, *modenamep)){  /* Command in different mode */
                *modenamep = modename;
                cli_set_syntax_mode(h, modename);
            }
            cli_output_reset();
            if (!cligen_exiting(ch)) {  
                clicon_err_reset();
                if ((ret = cligen_eval(ch, match_obj, cvv, callbacks)) < 0) {
                    cli_handler_err(stdout);
                    if (clicon_suberrno == ESHUTDOWN)
                        goto done;
                }
            }
            else
                ret = 0;
            if (evalres)
                *evalres = ret;
            break;
        default:
            fprintf(f, "CLI syntax error: \"%s\" is ambiguous\n", cmd);
            break;
        } /* switch result */
        if (cvv){
            cvec_free(cvv);
            cvv = NULL;
        }
    }
    retval = 0;
done:
    fflush(f);

    if (callbacks)
        co_callbacks_free(&callbacks);
    if (reason)
        free(reason);
    if (cvv)
        cvec_free(cvv);
    if (match_obj)
        co_free(match_obj, 0);
    return retval;
}

/*! Return a malloced expanded prompt string from printf-like format
 * @param[in]   h        Clixon handle
 * @param[in]   fmt      Format string, using %H, %
 * @retval      prompt   Malloced string, free after use
 * @retval      NULL     Error
 */
static char *
cli_prompt_get(clicon_handle h,
               char         *fmt)
{
    char   *s = fmt;
    char    hname[1024];
    char    tty[32];
    char   *tmp;
    cbuf   *cb = NULL;
    char   *path = NULL;
    char   *promptstr = NULL;
    char   *str0;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    /* Start with empty string */
    while(*s) {
        if (*s == '%' && *++s) {
            switch(*s) {
            case 'H': /* Hostname */
                if (gethostname(hname, sizeof(hname)) != 0)
                    strncpy(hname, "unknown", sizeof(hname)-1);
                cprintf(cb, "%s", hname);
                break;
            case 'U': /* Username */
                tmp = getenv("USER");
                cprintf(cb, "%s", tmp?tmp:"nobody");
                break;
            case 'T': /* TTY */
                if(ttyname_r(fileno(stdin), tty, sizeof(tty)-1) < 0)
                    strcpy(tty, "notty");
                cprintf(cb, "%s", tty);
                break;
            case 'W': /* Last element of working path */
                if (clicon_data_get(h, "cli-edit-mode", &path) == 0 &&
                    strlen(path)){
                    int i;

                    for (i=strlen(path)-1; i>=0; i--)
                        if (path[i] == '/' || path[i] == ':')
                            /* see yang2api_path_fmt_1() why occasional trailing / */
                            if (i < strlen(path)-1)
                                break;
                    if (i >= 0)
                        cprintf(cb, "%s", &path[i+1]);
                    else
                        cprintf(cb, "%s", path);
                }
                else
                    cprintf(cb, "/");
                break;
            case 'w': /* Full Working edit path */ 
                if (clicon_data_get(h, "cli-edit-mode", &path) == 0 &&
                    strlen(path))
                    cprintf(cb, "%s", path);
                else
                    cprintf(cb, "/");
                break;
            default:
                cprintf(cb, "%%");
                cprintf(cb, "%c", *s);
            }
        }
        else if (*s == '\\' && *++s) {
            switch(*s) {
            case 'n':
                cprintf(cb, "\n");
                break;
            default:
                cprintf(cb, "\\");
                cprintf(cb, "%c", *s);
            }
        }
        else 
            cprintf(cb, "%c", *s);
        s++;
    }
    str0 = cbuf_len(cb) ? cbuf_get(cb) : CLI_DEFAULT_PROMPT;
    if ((promptstr = strdup(str0)) == NULL){
        clicon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
 done:
    if (cb)
        cbuf_free(cb);
    return promptstr;
}

/*! Read command from CLIgen's cliread() using current syntax mode.
 *
 * @param[in]  h       Clicon handle
 * @param[in]  ph      Parse-tree head
 * @param[out] stringp Pointer to command buffer or NULL on EOF
 * @retval     1       OK
 * @retval     0       Fail but continue
 * @retval    -1       Error
 */
int
clicon_cliread(clicon_handle h,
               pt_head      *ph,
               char        **stringp)
{
    int               retval = -1;
    char             *name;
    char             *pfmt = NULL;
    cli_prompthook_t *fn;
    clixon_plugin_t  *cp;
    char             *promptstr;

    name = cligen_ph_name_get(ph);
    /* Get prompt from plugin callback? */
    cp = NULL;
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
        if ((fn = clixon_plugin_api_get(cp)->ca_prompt) == NULL)
            continue;
        pfmt = fn(h, name);
        break;
    }
    if (clicon_quiet_mode(h))
        cli_prompt_set(h, "");
    else{
        if ((promptstr = cli_prompt_get(h,
                                        pfmt?pfmt:cligen_ph_prompt_get(ph)
                                        )) == NULL)
            goto done;
        cli_prompt_set(h, promptstr);
        free(promptstr);
    }
    clicon_err_reset();
    if (cliread(cli_cligen(h), stringp) < 0){
        cli_handler_err(stdout);
        if (clicon_suberrno == ESHUTDOWN)
            goto done;
        goto fail;
    }

    retval = 1;
 done:
    if (pfmt)
        free(pfmt);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*
 *
 * CLI PLUGIN INTERFACE, PUBLIC SECTION
 *
 */

/*! Set syntax mode mode for existing current plugin group.
 * @param[in]     h       Clicon handle
 * @retval        1       OK
 * @retval        0       Not found / error
 */
int
cli_set_syntax_mode(clicon_handle h,
                    char         *name)
{

    pt_head          *ph;

    if ((ph = cligen_ph_find(cli_cligen(h), name)) == NULL)
        return 0;
    cligen_pt_head_active_set(cli_cligen(h), ph);
    return 1;
}

/*! Get syntax mode name
 * @param[in]     h       Clicon handle
 */
char *
cli_syntax_mode(clicon_handle h)
{
    pt_head          *ph;
    
    if ((ph =  cligen_pt_head_active_get(cli_cligen(h))) == NULL)
        return NULL;
    return cligen_ph_name_get(ph);
}

