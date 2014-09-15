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

#include "debug.h"
#include "wl_service.h"


int wl_current_status;
char* wl_ap_token;


struct write_result
{
    char *data;
    int pos;
};

void
wl_init(void){
	
	wl_current_status = WL_STATUS_OK;
	wl_ap_token = NULL;
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

static char *
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
    if(status != 0)
    {
        debug(LOG_DEBUG, "Unable to contact wifiLazooo during the request made at: %s, you are offline for %d seconds", url, WAIT_SECONDS);
        wl_offline();
        goto error;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if(code != 200)
    {
        debug(LOG_INFO, "During the request made at: %s, server returned code: %d", url, code);
        wl_down();
        goto error;
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_global_cleanup();

    /* zero-terminate the result */
    data[write_result.pos] = '\0';

    return data;

error:
    if(data)
        free(data);
    if(curl)
        curl_easy_cleanup(curl);
    if(headers)
        curl_slist_free_all(headers);
    return NULL;
}

