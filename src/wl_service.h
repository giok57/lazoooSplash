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

/** @file wl_service.h
    @brief wifilazooo service communications
    @author Copyright (C) 2014 wifiLazooo, Gioele Meoni <giok57@lazooo.com>
*/

#include "client_list.h"

#ifndef _WL_SERVICE_H_
#define _WL_SERVICE_H_
#endif

#define WL_STATUS_OK					0
#define WL_STATUS_NO_CONNECTION			1
#define WL_STATUS_SERVICE_UNAVAILABLE	2

#define FALSE 0
#define TRUE 1    

#define BUFFER_SIZE  (256 * 1024)  /* 256 KB */

#define URL_FORMAT_REGISTER   "https://wifi.lazooo.com/api/v1/business/from/ap/%s/register"
#define URL_FORMAT_EVENTS   "https://wifi.lazooo.com/api/v1/business/from/ap/events?tokenAP=%s"
#define UUID_FILE_PATH   "/etc/nodogsplash/WL_UUID"
#define URL_SIZE     256
#define WAIT_SECONDS 3

#define EVENT_CONNECT 1    
#define EVENT_DISCONNECT 0
    
extern int wl_current_status;
extern char* wl_ap_id;
extern char* UUID;

void wl_init(void);