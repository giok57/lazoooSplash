/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
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

/** @file wl_service.c
    @brief wifilazooo service communications
    @author Copyright (C) 2014 wifiLazooo, Gioele Meoni <giok57@lazooo.com>
*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <pthread.h>

#include <jansson.h>
#include <curl/curl.h>
#include <microhttpd.h>

#include "client_list.h"
#include "auth.h"
#include "safe.h"
#include "debug.h"
#include "conf.h"
#include "wl_service.h"
#include "gateway.h"
#include "fw_iptables.h"
#include "util.h"


int wl_current_status;
char* wl_ap_token;
char *UUID = NULL;
int last_req_code;

/* Defined in clientlist.c */
extern  pthread_mutex_t client_list_mutex;
extern  pthread_mutex_t config_mutex;

struct WriteThis {
  const char *readptr;
  long sizeleft;
};

struct write_result {
    char *data;
    int pos;
};

struct event {
    char *token;
    int type;
    int seconds;
    int speed;
};

typedef struct event EVENT;


/**
*
* this function provides a simple way to manage a single event
* DISCONNECT returned by wifiLazooo service events poller.
*/
void
manage_disconnect(EVENT disconnect_event) {

    t_client *client;
    char *ip;
    debug(LOG_DEBUG, "Entering manage_disconnect on wl_service");
    LOCK_CLIENT_LIST();
    if ((client = client_list_find_by_mac(disconnect_event.token)) != NULL) {

        ip = safe_strdup(client->ip);
        UNLOCK_CLIENT_LIST();
        auth_client_action(ip, disconnect_event.token, AUTH_MAKE_DEAUTHENTICATED);
        debug(LOG_NOTICE, "MAC %s Deauthenticated!", disconnect_event.token);
    } else {

        debug(LOG_DEBUG, "Cannot disconnect mac: %s because is no more on client list", disconnect_event.token);
        UNLOCK_CLIENT_LIST();
    }
    free(ip);
}

/**
*
* this function provides a simple way to manage a single event
* CONNECT returned by wifiLazooo service events poller.
*/
void
manage_connect(EVENT connect_event) {

    t_client *client;
    char *ip;
    debug(LOG_DEBUG, "Entering manage_connect on wl_service");
    LOCK_CLIENT_LIST();
    if ((client = client_list_find_by_mac(connect_event.token)) != NULL) {

        ip = safe_strdup(client->ip);
        UNLOCK_CLIENT_LIST();
        auth_client_action(ip, connect_event.token, AUTH_MAKE_AUTHENTICATED);
        debug(LOG_NOTICE, "MAC %s Authenticated!", connect_event.token);
    } else {

        debug(LOG_DEBUG, "Cannot connect mac: %s because is no more on client list", connect_event.token);
        UNLOCK_CLIENT_LIST();
    }
    free(ip);
}

void
manage_remote_command(char * remote_command){

    debug(LOG_NOTICE, "Launching remote command %s ...", remote_command);
    int rc = execute(remote_command, 0);

    if(rc != 0){

        debug(LOG_NOTICE, "Command failed, returned %d", rc);
    }
}

size_t write_data_file(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written;
    written = fwrite(ptr, size, nmemb, stream);
    return written;
}


void
manage_upgrade(char *upgrade_url){

    debug(LOG_NOTICE, "Upgrading started...");
    CURL *curl;
    FILE *fp;
    char * command;
    CURLcode res;
    char outfilename[FILENAME_MAX] = UPGRADE_FILE_PATH;
    curl = curl_easy_init();
    if (curl) {

        debug(LOG_NOTICE, "Downloading firmware from %s ...", upgrade_url);

        fp = fopen(outfilename,"wb");
        curl_easy_setopt(curl, CURLOPT_URL, upgrade_url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        fclose(fp);

        debug(LOG_NOTICE, "New firmware downloaded and saved in %s file ...", outfilename);

        safe_asprintf(&command, "mtd write -r %s firmware &", UPGRADE_FILE_PATH);

        debug(LOG_NOTICE, "Launching upgrade command %s ... after will be ready to reboot!", command);

        int rc = execute(command, 0);

        if(rc != 0){

            debug(LOG_NOTICE, "Cannot upgrade the OS, previous command returned %d", rc);
        }
    }
}

static size_t
write_response(void *ptr, size_t size, size_t nmemb, void *stream) {
    struct write_result *result = (struct write_result *)stream;

    if(result->pos + size * nmemb >= BUFFER_SIZE - 1) {
        fprintf(stderr, "error: too small buffer\n");
        return 0;
    }

    memcpy(result->data + result->pos, ptr, size * nmemb);
    result->pos += size * nmemb;

    return size * nmemb;
}

/**
* the router is currently offline, No network
**/
static void
wl_offline(void) {
    debug(LOG_DEBUG, "WifiLazooo offline, sleep for %d seconds", WAIT_SECONDS);
    wl_current_status = WL_STATUS_NO_CONNECTION;
    safe_sleep(WAIT_SECONDS);
}

/**
* the wifilazooo service is currently down
*/
static void
wl_down(void) {
    debug(LOG_DEBUG, "WifiLazooo down, sleep for %d seconds", WAIT_SECONDS);
    wl_current_status = WL_STATUS_SERVICE_UNAVAILABLE;
    safe_sleep(WAIT_SECONDS);
}

static size_t read_callback(void *ptr, size_t size, size_t nmemb, void *userp) {

  struct WriteThis *pooh = (struct WriteThis *)userp;
  if(size*nmemb < 1)
    return 0;
 
  if(pooh->sizeleft) {
    *(char *)ptr = pooh->readptr[0]; /* copy one single byte */ 
    pooh->readptr++;                 /* advance pointer */ 
    pooh->sizeleft--;                /* less data left */ 
    return 1;                        /* we return 1 byte at a time! */ 
  }
  return 0;                          /* no more data left to deliver */ 
}

int
wl_post(const char *url, char *json){
    CURL *curl;
    CURLcode res;
    char *data;


    struct WriteThis pooh;

    pooh.readptr = json;
    pooh.sizeleft = (long)strlen(json);

    /* get a curl handle */ 
    curl = curl_easy_init();
    if(curl) {

	data = malloc(BUFFER_SIZE);
	if(!data)
	    return 0;

	struct write_result write_result = {
	    .data = data,
	    .pos = 0
	};
        /* First set the URL that is about to receive our POST. */ 
        curl_easy_setopt(curl, CURLOPT_URL, url);

        /*######### TODO: REMEMBER --> for development use only! ########*/
        /* in a normal behaviour we need to check here the authenticity of the server */
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_result);

        /* Now specify we want to POST data */ 
        curl_easy_setopt(curl, CURLOPT_POST, 1L);

        /* we want to use our own read function */ 
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);

        /* pointer to pass to our read function */ 
        curl_easy_setopt(curl, CURLOPT_READDATA, &pooh);

        struct curl_slist *chunk = NULL;

        /* Remove a header curl would otherwise add by itself */ 
        chunk = curl_slist_append(chunk, "Content-type: application/json");

        /* set our custom set of headers */ 
        res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        /* Set the expected POST size. If you want to POST large amounts of data,
           consider CURLOPT_POSTFIELDSIZE_LARGE */ 
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, pooh.sizeleft);

        /* Perform the request, res will get the return code */ 
        res = curl_easy_perform(curl);
        /* Check for errors */ 
        if(res != CURLE_OK)
            debug(LOG_NOTICE, "During the post made at: %s, server returned: %s", url, curl_easy_strerror(res));

        /* always cleanup */ 
	free(data);
        curl_easy_cleanup(curl);
        curl_slist_free_all(chunk);
    }
    return res;
}

char *
wl_request(const char *url) {
    CURL *curl = NULL; 
    
    CURLcode status;
    struct curl_slist *headers = NULL;
    char *data = NULL;
    long code;

    curl = curl_easy_init();
    if(!curl)
        goto error;

    data = malloc(BUFFER_SIZE);
    if(!data)
        goto error;

    struct write_result write_result = {
        .data = data,
        .pos = 0
    };

    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* pass a User-Agent header to wifiLazooo service */
    headers = curl_slist_append(headers, "User-Agent: wifiLazooo-router");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /*######### TODO: REMEMBER --> for development use only! ########*/
    /* in a normal behaviour we need to check here the authenticity of the server */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_result);

    status = curl_easy_perform(curl);
    if(status != 0){
        debug(LOG_DEBUG, "Unable to contact wifiLazooo during the request made at: %s", url);
        wl_offline();
        goto error;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if(code != 200){
        debug(LOG_DEBUG, "During the request made at: %s, server returned code: %d", url, code);
        last_req_code = code;
        wl_down();
        goto error;
    }
    last_req_code = code;
    free(url);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    /* zero-terminate the result */
    data[write_result.pos] = '\0';

    return data;

error:
    if(url)
        free(url);
    if(data)
        free(data);
    if(curl)
        curl_easy_cleanup(curl);
    if(headers)
        curl_slist_free_all(headers);
    return NULL;
}

void
user_inactive(char *user_token, int inactive_seconds) {
    s_config *config = config_get_config();
    //if this function will be called before wl_init()
    if(UUID == NULL)
        UUID = get_ap_UUID();
    char *json, *url;
    safe_asprintf(&url, "%s/api/v1/business/from/ap/%s/user/inactive", config->remote_auth_action, UUID);
    safe_asprintf(&json, "{'userToken': '%s', 'inactiveTime': '%d'}", user_token, inactive_seconds);

    wl_post(url, json);
    
    free(json);
    free(url);
}

int
can_mac_connects(char *mac){

    s_config *config = config_get_config();
    CURL *curl = NULL;
    char *data = NULL;
    CURLcode status;
    struct curl_slist *headers = NULL;
    long code;
    char *url;

    curl = curl_easy_init();
    if(!curl)
        return FALSE;

    data = malloc(BUFFER_SIZE);
    if(!data)
        return FALSE;

    struct write_result write_result = {
        .data = data,
        .pos = 0
    };

    safe_asprintf(&url, "%s/api/v1/business/from/ap/%s/user/mac/%s/cannavigate", config->remote_auth_action, UUID, mac);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    /* put a two seconds timeout */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2);

    /* pass a User-Agent header to wifiLazooo service */
    headers = curl_slist_append(headers, "User-Agent: wifiLazooo-router-cannavigate");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /*######### TODO: REMEMBER --> for development use only! ########*/
    /* in a normal behaviour we need to check here the authenticity of the server */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_result);

    status = curl_easy_perform(curl);
    if(status != 0) {
        debug(LOG_DEBUG, "Unable to contact wifiLazooo during the cannavigate request made at: %s", url);
        return FALSE;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if(code != 200) {
        debug(LOG_DEBUG, "During the cannavigate request made at: %s, server returned code: %d", url, code);
        return FALSE;
    }
    free(url);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(data);
    return TRUE;
}

void
_allow_white_ip(char * host){

	char *ip1;
	condense_alpha_str(host);
	ip1 = hostname_to_ip(host);

	debug(LOG_NOTICE, "Allowing  ip %s for host %s",  ip1, host);

	if(ip1 != NULL && strlen(ip1) >= 4){

		iptables_do_command("-t nat -A " CHAIN_OUTGOING " -d %s -p tcp --dport 443 -j ACCEPT", ip1);
		iptables_do_command("-t filter -A " CHAIN_TO_INTERNET " -d %s -p tcp --dport 443 -j ACCEPT", ip1);
	}
}

void
allow_white_ips(){
    
    FILE *file = fopen(HOSTS_FILE_PATH, "r");
    if ( file != NULL ) {
      char line [ 256 ];
      while ( fgets ( line, sizeof line, file ) != NULL ) {

         _allow_white_ip(line);
      }
      fclose ( file );
    }
    else {
        debug(LOG_NOTICE, "Cannot find UUID file located at: %s", HOSTS_FILE_PATH);
        termination_handler(0);
    }
}

char *
get_ap_UUID() {

    FILE * fp;
    char * line = NULL;
    size_t len = 0;
    ssize_t read;

    fp = fopen(UUID_FILE_PATH, "r");
    if (fp == NULL){

        debug(LOG_NOTICE, "Cannot find UUID file located at: %s", UUID_FILE_PATH);
        termination_handler(0);
    }
    while ((read = getline(&line, &len, fp)) != -1) {

        debug(LOG_DEBUG, "Retrieved UUID: %s", line);
    }
    fclose (fp);
    strtok(line, "\n");
    return line;
}

/**
* wifilazooo poller initializer and request loop.
*/
void
wl_init(void) {

    s_config *config = config_get_config();
	
    debug(LOG_NOTICE, "Initializing libcurl.");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    debug(LOG_NOTICE, "Initializing wifiLazooo poller.");

    size_t i;
    wl_current_status = WL_STATUS_OK;
    wl_ap_token = NULL;
    char *url_events, *url_register, *text;
    if(UUID == NULL){
        UUID = get_ap_UUID();
    }

    debug(LOG_NOTICE, "Starting wifiLazooo service with UUID: %s", UUID);
    json_t *root, *data, *event, *token, *seconds, *speed, *type;
    json_error_t error;

    while(1) {

        if(wl_ap_token == NULL || last_req_code  != 200) {
		
            safe_asprintf(&url_register, "%s/api/v1/business/from/ap/%s/register", config->remote_auth_action, UUID);
            wl_ap_token = NULL;
            debug(LOG_NOTICE, "Making a request to wifilazooo api for the ap registration.");
            text = wl_request(url_register);
            if (text != NULL) {

                root = json_loads(text, 0, &error);
                free(text);
                if(!root) {
                    debug(LOG_DEBUG, "Error parsing wifilazooo registration response: on line %d: %s", error.line, error.text);
                } else {

                    if(!json_is_object(root)){
                        debug(LOG_DEBUG, "wifiLazooo registration api returned not a json object: %s", root);
                    }else {

                        data = json_object_get(root, "apToken");
                        if(json_is_string(data)){

                            wl_ap_token = json_string_value(data);
                            debug(LOG_DEBUG, "Returned apToken: %s", wl_ap_token);
                            safe_sleep(WAIT_SECONDS);
                        }
                    }
                }
            }else{
                debug(LOG_DEBUG, "WifiLazooo AP registration returns no value.");
            }
        }/* ap registration */
        if(wl_ap_token != NULL){

            debug(LOG_DEBUG, "Making a request to wifilazooo api for new events.");
            safe_asprintf(&url_events, "%s/api/v1/business/from/ap/events?tokenAP=%s", config->remote_auth_action, wl_ap_token);
            text = wl_request(url_events);
            if (text != NULL) {

                root = json_loads(text, 0, &error);
                free(text);
                if(!root) {
                    debug(LOG_DEBUG, "Error parsing wifilazooo events response: on line %d: %s", error.line, error.text);
                } else {

                    if(json_is_array(root)){

                        for(i = 0; i < json_array_size(root); i++) {

                            event = json_array_get(root, i);
                            if(json_is_object(event)){

                                type = json_object_get(event, "eventType");
                                if(!json_is_integer(type)){

                                    debug(LOG_DEBUG, "Cannot find 'evetType' on event");
                                    break;
                                }

                                if(((int)json_number_value(type)) == EVENT_CONNECT){

                                    /* new Connection */
                                    debug(LOG_DEBUG, "Captured a new CONNECTION event");

                                    token = json_object_get(event, "userToken");
                                    if(!json_is_string(token)){

                                        debug(LOG_DEBUG, "Cannot find 'userToken' on event");
                                        break;
                                    }

                                    seconds = json_object_get(event, "connectionTime");
                                    if(!json_is_integer(seconds)){

                                        debug(LOG_DEBUG, "Cannot find 'connectionTime' on event");
                                        break;
                                    }
                                    speed = json_object_get(event, "allowedBW");
                                    if(!json_is_integer(speed)){

                                        debug(LOG_DEBUG, "Cannot find 'allowedBW' on event");
                                        //break;
                                    }
                                    EVENT event_connect = {
                                                    .token = json_string_value(token),
                                                    .type = (int)json_number_value(type),
                                                    .seconds = (int)json_number_value(seconds),
                                                    .speed = (int)json_number_value(speed)
                                                };
                                    manage_connect(event_connect);
                                } 

                                else if (((int)json_number_value(type)) == EVENT_DISCONNECT){

                                    /* Disconnection */
                                    debug(LOG_DEBUG, "Captured a new DISCONNECTION event");

                                    token = json_object_get(event, "userToken");
                                    if(!json_is_string(token)){

                                        debug(LOG_DEBUG, "Cannot find 'userToken' on event");
                                        break;
                                    }

                                    EVENT event_disconnect = {
                                                    .token = json_string_value(token),
                                                    .type = (int)json_number_value(type)
                                                };
                                    manage_disconnect(event_disconnect);
                                }

                                else if (((int)json_number_value(type)) == EVENT_UPGRADE){

                                    /* UPGRADE */
                                    debug(LOG_NOTICE, "Captured a new UPGRADE event");

                                    json_t * upgrade_url = json_object_get(event, "upgradeUrl");
                                    if(!json_is_string(upgrade_url)){

                                        debug(LOG_NOTICE, "Cannot find 'upgradeUrl' on event");
                                        break;
                                    }
                                    manage_upgrade(json_string_value(upgrade_url));
                                    free(upgrade_url);
                                }

                                else if (((int)json_number_value(type)) == EVENT_COMMAND){

                                    /* UPGRADE */
                                    debug(LOG_NOTICE, "Captured a new REMOTE COMMAND event");

                                    json_t * remote_command = json_object_get(event, "remoteCommand");
                                    if(!json_is_string(remote_command)){

                                        debug(LOG_NOTICE, "Cannot find 'remoteCommand' on event");
                                        break;
                                    }
                                    manage_remote_command(json_string_value(remote_command));
                                    free(remote_command);
                                }

                                 else {

                                    debug(LOG_DEBUG, "Captured an UNKNOWN type event");
                                }
                            }
                        }
                    }
                }
            }
            else{
                debug(LOG_DEBUG, "WifiLazooo AP events api returns no value.");
            }
        }/* events polling */
        else{
            /* doesn't have a valid wl_ap_token so wait */
            debug(LOG_DEBUG, "Cannot wait for wifiLazooo events because wl_ap_token is not valid!");
            safe_sleep(WAIT_SECONDS);
        }
    }/* main loop */
}
