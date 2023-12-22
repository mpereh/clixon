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
#include <signal.h>
#include <syslog.h>
#include <errno.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_sig.h"

/*! Set a signal handler.
 *
 * @param[in]  signo      Signal number 
 * @param[in]  handler    Function to call when signal occurs
 * @param[out] oldhandler Pointer to old handler
 */
int
set_signal(int     signo,
           void  (*handler)(int),
           void (**oldhandler)(int))
{
    return set_signal_flags(signo,
#if defined(HAVE_SIGACTION)
                            SA_RESTART,
#else
                            0,
#endif
                            handler, oldhandler);
}

/*! Set a signal handler, but without SA_RESTART
 *
 * @param[in]  signo      Signal number 
 * @param[in]  flags      Flags (to sigaction)
 * @param[in]  handler    Function to call when signal occurs
 * @param[out] oldhandler Pointer to old handler
 */
int
set_signal_flags(int     signo,
                 int     flags,
                 void  (*handler)(int),
                 void (**oldhandler)(int))
{
#if defined(HAVE_SIGACTION)
    struct sigaction sold, snew;

    snew.sa_handler = handler;
    sigemptyset(&snew.sa_mask);
    snew.sa_flags = flags;
    if (sigaction(signo, &snew, &sold) < 0){
        clixon_err(OE_UNIX, errno, "sigaction");
        return -1;
    }
    if (oldhandler)
        *oldhandler = sold.sa_handler;
    return 0;
#elif defined(HAVE_SIGVEC)
    return 0;
#endif
}

/*! Block signal. 
 *
 * @param[in] sig   Signal number to block, If 0, block all signals
 */
void
clicon_signal_block(int sig)
{
    sigset_t        set;

    sigemptyset(&set);
    if (sig)
        sigaddset(&set, sig);
    else
        sigfillset(&set);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

/*! Unblock signal. 
 *
 * @param[in] sig   Signal number to unblock. If 0, unblock all signals
 */
void
clicon_signal_unblock(int sig)
{
    sigset_t        set;

    sigemptyset(&set);
    if (sig)
        sigaddset(&set, sig);
    else
        sigfillset(&set);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

/*! Save complete signal context
 */
int
clixon_signal_save(sigset_t        *sigset,
                   struct sigaction sigaction_vec[32])
{
    int retval = -1;
    int i;

    if (sigprocmask(0, NULL, sigset) < 0){
        clixon_err(OE_UNIX, errno, "sigprocmask");
        goto done;
    }
    for (i=1; i<32; i++){
        if (sigaction(i, NULL, &sigaction_vec[i]) < 0){
            clixon_err(OE_UNIX, errno, "sigaction");
            goto done;
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Restore complete signal context
 *
 * @retval     0       OK
 * @retval    -1       Error
 * @note: sigaction may not restore SIGKILL or SIGSTOP, which cannot be caught or ignored.
 */
int
clixon_signal_restore(sigset_t        *sigset,
                      struct sigaction sigaction_vec[32])
{
    int retval = -1;
    int i;

    if (sigprocmask(SIG_SETMASK, sigset, NULL) < 0){
        clixon_err(OE_UNIX, errno, "sigprocmask");
        goto done;
    }
    for (i=1; i<32; i++){
        if (i == SIGKILL || i == SIGSTOP)
            continue;
        if (sigaction(i, &sigaction_vec[i], NULL) < 0){
            clixon_err(OE_UNIX, errno, "sigaction");
            goto done;
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Read pidfile and return pid using file descriptor
 *
 * @param[in]  pidfile  Name of pidfile
 * @param[out] pid      Process id of (eventual) existing daemon process
 * @retval     0        OK. if pid > 0 old process exists w that pid
 */
int
pidfile_get_fd(FILE  *f,
                pid_t *pid0)
{
    char   *ptr;
    char    buf[32];
    pid_t   pid;

    *pid0 = 0;
    ptr = fgets(buf, sizeof(buf), f);
    if (ptr != NULL && (pid = atoi(ptr)) > 1) {
        if (kill(pid, 0) == 0 || errno != ESRCH) {
            /* Yes there is a process */
            *pid0 = pid;
        }
    }
    return 0;
}

/*! Read pidfile and return pid, if any
 *
 * @param[in]  pidfile  Name of pidfile
 * @param[out] pid      Process id of (eventual) existing daemon process
 * @retval    0         OK. if pid > 0 old process exists w that pid
 */
int
pidfile_get(char  *pidfile,
            pid_t *pid)
{
    FILE   *f;

    *pid = 0;
    if ((f = fopen(pidfile, "r")) != NULL){
        pidfile_get_fd(f, pid);
        fclose(f);
    }
    return 0;
}

/*! Given a pid, kill that process

 *
 * @param[in] pid   Process id
 * @retval    0     Killed OK
 * @retval   -1     Could not kill.
 * Maybe should not belong to pidfile code,..
 */
int
pidfile_zapold(pid_t pid)
{
    int retval = -1;

    clixon_log(NULL, LOG_NOTICE, "Killing old daemon with pid: %d", pid);
    killpg(pid, SIGTERM);
    kill(pid, SIGTERM);
    /* Need to sleep process properly and then check again */
    if (usleep(100000) < 0){
        clixon_err(OE_UNIX, errno, "usleep");
        goto done;
    }
    if ((kill(pid, 0)) < 0){
        if (errno != ESRCH){
            clixon_err(OE_DAEMON, errno, "Killing old daemon");
            goto done;
        }
    }
    retval = 0;
 done:
    return retval;
}

/*! Write a pid-file
 *
 * @param[in] pidfile  Name of pidfile
 * @retval    0        OK
 * @retval   -1        Error
 */
int
pidfile_write(char *pidfile)
{
    int   retval = -1;
    FILE *f = NULL;

    /* Here, there should be no old agent and no pidfile */
    if ((f = fopen(pidfile, "w")) == NULL){
        if (errno == EACCES)
            clixon_err(OE_DAEMON, errno, "Creating pid-file %s (Try run as root?)", pidfile);
        else
            clixon_err(OE_DAEMON, errno, "Creating pid-file %s", pidfile);
        goto done;
    }
    if ((retval = fprintf(f, "%ld\n", (long) getpid())) < 1){
        clixon_err(OE_DAEMON, errno, "Could not write pid to %s", pidfile);
        goto done;
    }
    clixon_debug(CLIXON_DBG_DEFAULT, "Opened pidfile %s with pid %d", pidfile, getpid());
    retval = 0;
 done:
    if (f != NULL)
        fclose(f);
    return retval;
}
