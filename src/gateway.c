/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free:Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
 \********************************************************************/

/* $Id: gateway.c 1104 2006-10-09 00:58:46Z acv $ */
/** @internal
  @file gateway.c
  @brief Main loop
  @author Copyright (C) 2004 Philippe April <papril777@yahoo.com>
  @author Copyright (C) 2004 Alexandre Carmel-Veilleux <acv@miniguru.ca>
  @author Copyright (C) 2008 Paul Kube <nodogsplash@kokoro.ucsd.edu>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

/* for strerror() */
#include <string.h>

/* for wait() */
#include <sys/wait.h>

/* for unix socket communication*/
#include <sys/socket.h>
#include <sys/un.h>

#include <sys/types.h>
#include <sys/select.h>
#include <string.h>
#include <microhttpd.h>

#include "common.h"
#include "httpd.h"
#include "safe.h"
#include "debug.h"
#include "conf.h"
#include "gateway.h"
#include "firewall.h"
#include "commandline.h"
#include "auth.h"
#include "http.h"
#include "client_list.h"
#include "ndsctl_thread.h"
#include "httpd_handler.h"
#include "util.h"
#include "wl_service.h"



/** XXX Ugly hack
 * We need to remember the thread IDs of threads that simulate wait with pthread_cond_timedwait
 * so we can explicitly kill them in the termination handler
 */
static pthread_t tid_client_check = 0;

/* The internal web server */
httpd * webserver = NULL;

/* Time when nodogsplash started  */
time_t started_time = 0;

static long
get_file_size (const char *filename) {
	FILE *fp;

	fp = fopen (filename, "rb");
	if (fp) {
	  long size;

	  if ((0 != fseek (fp, 0, SEEK_END)) || (-1 == (size = ftell (fp))))
	    size = 0;

	  fclose (fp);

	  return size;
	}
	else
	return 0;
}

static char *
load_file (const char *filename) {
	FILE *fp;
	char *buffer;
	long size;

	size = get_file_size (filename);
	if (size == 0)
	return NULL;

	fp = fopen (filename, "rb");
	if (!fp)
	return NULL;

	buffer = malloc (size);
	if (!buffer) {
	  fclose (fp);
	  return NULL;
	}

	if (size != fread (buffer, 1, size, fp)) {
	  free (buffer);
	  buffer = NULL;
	}

	fclose (fp);
	return buffer;
}

/**@internal
 * @brief Handles SIGCHLD signals to avoid zombie processes
 *
 * When a child process exits, it causes a SIGCHLD to be sent to the
 * parent process. This handler catches it and reaps the child process so it
 * can exit. Otherwise we'd get zombie processes.
 */
void
sigchld_handler(int s)
{
	int	status;
	pid_t rc;

	debug(LOG_DEBUG, "SIGCHLD handler: Trying to reap a child");

	rc = waitpid(-1, &status, WNOHANG | WUNTRACED);

	if(rc == -1) {
		if(errno == ECHILD) {
			debug(LOG_DEBUG, "SIGCHLD handler: waitpid(): No child exists now.");
		} else {
			debug(LOG_ERR, "SIGCHLD handler: Error reaping child (waitpid() returned -1): %s", strerror(errno));
		}
		return;
	}

	if(WIFEXITED(status)) {
		debug(LOG_DEBUG, "SIGCHLD handler: Process PID %d exited normally, status %d", (int)rc, WEXITSTATUS(status));
		return;
	}

	if(WIFSIGNALED(status)) {
		debug(LOG_DEBUG, "SIGCHLD handler: Process PID %d exited due to signal %d", (int)rc, WTERMSIG(status));
		return;
	}

	debug(LOG_DEBUG, "SIGCHLD handler: Process PID %d changed state, status %d not exited, ignoring", (int)rc, status);
	return;
}

/** Exits cleanly after cleaning up the firewall.
 *  Use this function anytime you need to exit after firewall initialization */
void
termination_handler(int s)
{
	curl_global_cleanup();
	static	pthread_mutex_t	sigterm_mutex = PTHREAD_MUTEX_INITIALIZER;

	debug(LOG_NOTICE, "Handler for termination caught signal %d", s);

	/* Makes sure we only call fw_destroy() once. */
	if (pthread_mutex_trylock(&sigterm_mutex)) {
		debug(LOG_INFO, "Another thread already began global termination handler. I'm exiting");
		pthread_exit(NULL);
	} else {
		debug(LOG_INFO, "Cleaning up and exiting");
	}

	debug(LOG_INFO, "Flushing firewall rules...");
	fw_destroy();

	/* XXX Hack
	 * Aparently pthread_cond_timedwait under openwrt prevents signals (and therefore
	 * termination handler) from happening so we need to explicitly kill the threads
	 * that use that
	 */
	if (tid_client_check) {
		debug(LOG_INFO, "Explicitly killing the fw_counter thread");
		pthread_kill(tid_client_check, SIGKILL);
	}

	debug(LOG_NOTICE, "Exiting...");
	exit(s == 0 ? 1 : 0);
}


/** @internal
 * Registers all the signal handlers
 */
static void
init_signals(void)
{
	struct sigaction sa;

	debug(LOG_DEBUG, "Setting SIGCHLD handler to sigchld_handler()");
	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		debug(LOG_ERR, "sigaction(): %s", strerror(errno));
		exit(1);
	}

	/* Trap SIGPIPE */
	/* This is done so that when libhttpd does a socket operation on
	 * a disconnected socket (i.e.: Broken Pipes) we catch the signal
	 * and do nothing. The alternative is to exit. SIGPIPE are harmless
	 * if not desirable.
	 */
	debug(LOG_DEBUG, "Setting SIGPIPE  handler to SIG_IGN");
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &sa, NULL) == -1) {
		debug(LOG_ERR, "sigaction(): %s", strerror(errno));
		exit(1);
	}

	debug(LOG_DEBUG, "Setting SIGTERM,SIGQUIT,SIGINT  handlers to termination_handler()");
	sa.sa_handler = termination_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;

	/* Trap SIGTERM */
	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		debug(LOG_ERR, "sigaction(): %s", strerror(errno));
		exit(1);
	}

	/* Trap SIGQUIT */
	if (sigaction(SIGQUIT, &sa, NULL) == -1) {
		debug(LOG_ERR, "sigaction(): %s", strerror(errno));
		exit(1);
	}

	/* Trap SIGINT */
	if (sigaction(SIGINT, &sa, NULL) == -1) {
		debug(LOG_ERR, "sigaction(): %s", strerror(errno));
		exit(1);
	}
}

void
init_wl_service(void){

	/* wifilazooo initialization */
	wl_init();
}

void
allow_ips_loop(void){

	while(1==1){

		allow_white_ips();
		//30 secs
		safe_sleep(30);
	}
}


/**@internal
 * Main execution loop
 */
static void
main_loop(void)
{
	int result;
	pthread_t	tid, wl_service, allow_ips;
	s_config *config = config_get_config();
	struct timespec wait_time;
	int msec;
	request *r;
	void **params;
	int* thread_serial_num_p;
	struct MHD_Daemon *daemon;
	struct MHD_Daemon *ssl_daemon;
  	char *key_pem;
  	char *cert_pem;

	/* Set the time when nodogsplash started */
	if (!started_time) {
		debug(LOG_INFO, "Setting started_time");
		started_time = time(NULL);
	} else if (started_time < MINIMUM_STARTED_TIME) {
		debug(LOG_WARNING, "Detected possible clock skew - re-setting started_time");
		started_time = time(NULL);
	}

	/* If we don't have the Gateway IP address, get it. Exit on failure. */
	if (!config->gw_address) {
		debug(LOG_DEBUG, "Finding IP address of %s", config->gw_interface);
		if ((config->gw_address = get_iface_ip(config->gw_interface)) == NULL) {
			debug(LOG_ERR, "Could not get IP address information of %s, exiting...", config->gw_interface);
			exit(1);
		}
		debug(LOG_NOTICE, "Detected gateway %s at %s", config->gw_interface, config->gw_address);
	}

	/* Reset the firewall (cleans it, in case we are restarting after nodogsplash crash) */

	fw_destroy();
	/* Then initialize it */
	debug(LOG_NOTICE, "Initializing firewall rules");
	if( fw_init() != 0 ) {
		debug(LOG_ERR, "Error initializing firewall rules! Cleaning up");
		fw_destroy();
		debug(LOG_ERR, "Exiting because of error initializing firewall rules");
		exit(1);
	}

	/* Start thread that loops for white IPS */
	result = pthread_create(&allow_ips, NULL, (void *)allow_ips_loop, NULL);
	if (result != 0) {
		debug(LOG_ERR, "FATAL: Failed to create thread for allow white ips - exiting");
		termination_handler(0);
	}
	pthread_detach(allow_ips);

	/* Start client statistics and timeout clean-up thread */
	result = pthread_create(&tid_client_check, NULL, (void *)thread_client_timeout_check, NULL);
	if (result != 0) {
		debug(LOG_ERR, "FATAL: Failed to create thread_client_timeout_check - exiting");
		termination_handler(0);
	}
	pthread_detach(tid_client_check);

	/* Start control thread */
	result = pthread_create(&tid, NULL, (void *)thread_ndsctl, (void *)safe_strdup(config->ndsctl_sock));
	if (result != 0) {
		debug(LOG_ERR, "FATAL: Failed to create thread_ndsctl - exiting");
		termination_handler(0);
	}
	pthread_detach(tid);

	/* Start thread that waits for wifiLazooo events */
	result = pthread_create(&wl_service, NULL, (void *)init_wl_service, NULL);
	if (result != 0) {
		debug(LOG_ERR, "FATAL: Failed to create thread for wl_service - exiting");
		termination_handler(0);
	}
	pthread_detach(wl_service);

	//daemon = MHD_start_daemon (MHD_USE_THREAD_PER_CONNECTION, config->gw_port, &on_client_connect,
	//                  NULL, &answer_to_connection, NULL, MHD_OPTION_END);
	daemon = MHD_start_daemon(
						 MHD_USE_EPOLL_INTERNALLY_LINUX_ONLY,
						 config->gw_port,
						 NULL, NULL,
						 &answer_to_connection, NULL,
						 MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int) 10,
						 MHD_OPTION_LISTENING_ADDRESS_REUSE, 1,
						 MHD_OPTION_END);
	if (NULL == daemon ) {
	  debug(LOG_ERR, "FATAL: Failed to create the server daemon");
		
	  termination_handler(0);
	}
	debug(LOG_NOTICE, "Microhttpd started, pausing main thread");
	pause();
}

/** Main entry point for nodogsplash.
 * Reads the configuration file and then starts the main loop.
 */
int main(int argc, char **argv)
{
	s_config *config = config_get_config();
	config_init();

	parse_commandline(argc, argv);

	/* Initialize the config */
	debug(LOG_NOTICE,"Reading and validating configuration file %s", config->configfile);
	config_read(config->configfile);
	config_validate();

	/* Initializes the linked list of connected clients */
	client_list_init();

	/* Init the signals to catch chld/quit/etc */
	debug(LOG_NOTICE,"Initializing signal handlers");
	init_signals();

	if (config->daemon) {

		debug(LOG_NOTICE, "Starting as daemon, forking to background");

		switch(safe_fork()) {
		case 0: /* child */
			setsid();
			main_loop();
			break;

		default: /* parent */
			exit(0);
			break;
		}
	} else {
		main_loop();
	}

	return(0); /* never reached */
}
