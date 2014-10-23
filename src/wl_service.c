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

#include <jansson.h>
#include <curl/curl.h>
#include <microhttpd.h>

#include "client_list.h"
#include "auth.h"
#include "safe.h"
#include "debug.h"
#include "wl_service.h"
#include "gateway.h"


int wl_current_status;
char* wl_ap_token;
char* UUID;
int last_req_code;

/* Defined in clientlist.c */
extern  pthread_mutex_t client_list_mutex;
extern  pthread_mutex_t config_mutex;


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
        auth_client_action(ip, client->token, AUTH_MAKE_DEAUTHENTICATED);
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
        auth_client_action(ip, client->token, AUTH_MAKE_AUTHENTICATED);
        debug(LOG_NOTICE, "MAC %s Authenticated!", connect_event.token);
    } else {

        debug(LOG_DEBUG, "Cannot connect mac: %s because is no more on client list", connect_event.token);
        UNLOCK_CLIENT_LIST();
    }
    free(ip);
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

    wl_current_status = WL_STATUS_NO_CONNECTION;
    sleep(WAIT_SECONDS);
}

/**
* the wifilazooo service is currently down
*/
static void
wl_down(void) {

    wl_current_status = WL_STATUS_SERVICE_UNAVAILABLE;
    sleep(WAIT_SECONDS);
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
    char *url_clean = url;
    curl_easy_setopt(curl, CURLOPT_URL, url_clean);

    /* pass a User-Agent header to wifiLazooo service */
    headers = curl_slist_append(headers, "User-Agent: wifiLazooo-router");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /*######### TODO: REMEMBER --> for development use only! ########*/
    /* in a normal behaviour we need to check here the authenticity of the server */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_result);

    status = curl_easy_perform(curl);
    if(status != 0)
    {
        debug(LOG_DEBUG, "Unable to contact wifiLazooo during the request made at: %s, you are offline for %d seconds", url_clean, WAIT_SECONDS);
        wl_offline();
        goto error;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if(code != 200)
    {
        debug(LOG_DEBUG, "During the request made at: %s, server returned code: %d", url_clean, code);
        wl_down();
        goto error;
    }
    last_req_code = code;
    free(url_clean);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_global_cleanup();

    /* zero-terminate the result */
    data[write_result.pos] = '\0';

    return data;

error:
    if(url_clean)
        free(url_clean);
    if(data)
        free(data);
    if(curl)
        curl_easy_cleanup(curl);
    if(headers)
        curl_slist_free_all(headers);
    return NULL;
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

    debug(LOG_NOTICE, "Initializing wifiLazooo poller.");
    size_t i;
    wl_current_status = WL_STATUS_OK;
	wl_ap_token = NULL;
    char *url_events, *url_register, *text;
    UUID = get_ap_UUID();
    debug(LOG_NOTICE, "Starting wifiLazooo service with UUID: %s", UUID);
    json_t *root, *data, *event, *token, *seconds, *speed, *type;
    json_error_t error;

    safe_asprintf(&url_register, URL_FORMAT_REGISTER, UUID);

    while(1) {

        if(wl_ap_token == NULL || last_req_code  != 200) {

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
                            sleep(WAIT_SECONDS);
                        }
                    }
                }
            }else{
                debug(LOG_DEBUG, "WifiLazooo AP registration returns no value.");
            }
        }/* ap registration */
        if(wl_ap_token != NULL){

            debug(LOG_DEBUG, "Making a request to wifilazooo api for new events.");
            safe_asprintf(&url_events, URL_FORMAT_EVENTS, wl_ap_token);
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

                                token = json_object_get(event, "userToken");
                                if(!json_is_string(token)){

                                    debug(LOG_DEBUG, "Cannot find 'userToken' on event");
                                }
                                type = json_object_get(event, "eventType");
                                if(!json_is_integer(type)){

                                    debug(LOG_DEBUG, "Cannot find 'evetType' on event");
                                }
                                if(((int)json_number_value(type)) == EVENT_CONNECT){

                                    /* new Connection */
                                    debug(LOG_DEBUG, "Captured a new CONNECTION event");

                                    seconds = json_object_get(event, "connectionTime");
                                    if(!json_is_integer(seconds)){

                                        debug(LOG_DEBUG, "Cannot find 'connectionTime' on event");
                                    }
                                    speed = json_object_get(event, "allowedBW");
                                    if(!json_is_integer(speed)){

                                        debug(LOG_DEBUG, "Cannot find 'allowedBW' on event");
                                    }
                                    EVENT event_connect = {
                                                    .token = json_string_value(token),
                                                    .type = (int)json_number_value(type),
                                                    .seconds = (int)json_number_value(seconds),
                                                    .speed = (int)json_number_value(speed)
                                                };
                                    manage_connect(event_connect);
                                } else if (((int)json_number_value(type)) == EVENT_DISCONNECT){

                                    /* Disconnection */
                                    debug(LOG_DEBUG, "Captured a new DISCONNECTION event");

                                    EVENT event_disconnect = {
                                                    .token = json_string_value(token),
                                                    .type = (int)json_number_value(type)
                                                };
                                    manage_disconnect(event_disconnect);
                                } else {

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
            sleep(WAIT_SECONDS);
        }
    }/* main loop */
}