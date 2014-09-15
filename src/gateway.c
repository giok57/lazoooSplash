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
#include <syslog.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

/* for strerror() */
#include <string.h>

/* for wait() */
#include <sys/wait.h>

/* for requests */
#include <curl/curl.h>

/* for unix socket communication*/
#include <sys/socket.h>
#include <sys/un.h>

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
/* The internal web server from HTTPS*/
httpd * webserver_s = NULL;

/* Time when nodogsplash started  */
time_t started_time = 0;


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

	/* This function releases resources acquired by curl_global_init in main(). */
	curl_global_cleanup();

	debug(LOG_NOTICE, "Exiting...");
	exit(s == 0 ? 1 : 0);
}

void
init_webserver(httpd* webserver, int port){

    s_config *config = config_get_config();
	/* Initializes the web server */
	if ((webserver = httpdCreate(config->gw_address, port)) == NULL) {
		debug(LOG_ERR, "Could not create web server: %s", strerror(errno));
		exit(1);
	}
	debug(LOG_NOTICE, "Created web server on %s:%d", config->gw_address, port);

	/* Set web root for server */
	debug(LOG_DEBUG, "Setting web root: %s",config->webroot);
	httpdSetFileBase(webserver,config->webroot);

	/* Add images files to server: any file in config->imagesdir can be served */
	debug(LOG_DEBUG, "Setting images subdir: %s",config->imagesdir);
	httpdAddWildcardContent(webserver,config->imagesdir,NULL,config->imagesdir);

	/* Add pages files to server: any file in config->pagesdir can be served */
	debug(LOG_DEBUG, "Setting pages subdir: %s",config->pagesdir);
	httpdAddWildcardContent(webserver,config->pagesdir,NULL,config->pagesdir);


	debug(LOG_DEBUG, "Registering callbacks to web server");

	httpdAddCContent(webserver, "/", "", 0, NULL, http_nodogsplash_callback_index);
	httpdAddCWildcardContent(webserver, config->authdir, NULL, http_nodogsplash_callback_auth);
	httpdAddCWildcardContent(webserver, config->denydir, NULL, http_nodogsplash_callback_deny);
	httpdAddC404Content(webserver, http_nodogsplash_callback_404);
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

/**
* This is the 'run' function called by a new thread and catches all
* the requests made by users to port 80 (HTTP)
*/
void
t_listener(void){

	wait_for_requests_loop(webserver);
}

/**
* This is the 'run' function called by a new thread and catches all
* the requests made by users to port 443 (HTTPS)
*/
void
t_listener_s(void){

	wait_for_requests_loop(webserver_s);
}

int
wait_for_requests_loop(httpd* server){

    request *r;
	while(1) {
		r = httpdGetConnection(server, NULL);

		/* We can't convert this to a switch because there might be
		 * values that are not -1, 0 or 1. */
		if (server->lastError == -1) {
			/* Interrupted system call */
			continue; /* continue loop from the top */
		} else if (server->lastError < -1) {
			/*
			 * FIXME
			 * An error occurred - should we abort?
			 * reboot the device ?
			 */
			debug(LOG_ERR, "FATAL: httpdGetConnection returned unexpected value %d, exiting.", server->lastError);
			termination_handler(0);
		} else if (r != NULL) {
			/* We got a connection */
			/* put here the call to wifilazooo waiting for a new client authentication*/
			handle_http_request(server, r);
		} else {
			/* webserver->lastError should be 2 */
			/* XXX We failed an ACL.... No handling because
			 * we don't set any... */
		}
	}
}

/**@internal
 * Main execution loop
 */
static void
main_loop(void)
{
	int result;
	pthread_t	tid, req_loop, req_loop_s;
	s_config *config = config_get_config();
	struct timespec wait_time;
	int msec;
	request *r;
	void **params;
	int* thread_serial_num_p;

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

	/* init web server for connections on port 80 */
	init_webserver(webserver, config->gw_port);
	/* init web server for connections on port 443, port is gw port + 1 same as fw_iptables on initialization */
	init_webserver(webserver_s, config->gw_port + 1);

	/* FIREWALL AREA */
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
	/* END FIREWALL AREA */

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

	/*
	 * Enter the httpd request handling loop
	 */
	debug(LOG_NOTICE, "Waiting for connections");

	/* Start thread that waits for connections made to port 80 */
	result = pthread_create(&req_loop, NULL, (void *)t_listener, NULL);
	if (result != 0) {
		debug(LOG_ERR, "FATAL: Failed to create thread_listener_loop - exiting");
		termination_handler(0);
	}
	pthread_detach(req_loop);

	/* Start thread that waits for connections made to port 443 */
	result = pthread_create(&req_loop_s, NULL, (void *)t_listener_s, NULL);
	if (result != 0) {
		debug(LOG_ERR, "FATAL: Failed to create thread_listener_loop_s - exiting");
		termination_handler(0);
	}
	pthread_detach(req_loop_s);

}

/** Main entry point for nodogsplash.
 * Reads the configuration file and then starts the main loop.
 */
int main(int argc, char **argv) {

	s_config *config = config_get_config();
	config_init();

	parse_commandline(argc, argv);

	/* Initialize the config */
	debug(LOG_NOTICE,"Reading and validating configuration file %s", config->configfile);
	config_read(config->configfile);
	config_validate();

	/* initializes the global curl environment */
	curl_global_init(CURL_GLOBAL_ALL);

	/* wifilazooo initialization */
	wl_init();

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
