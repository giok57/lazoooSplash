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

#ifndef _WL_SERVICE_H_
#define _WL_SERVICE_H_

#define WL_STATUS_OK					0
#define WL_STATUS_NO_CONNECTION			1
#define WL_STATUS_SERVICE_UNAVAILABLE	2

#define WL_URL_FORMAT   "https://wifi.lazooo.com/api/v1/ap/%s/events/"

#define BUFFER_SIZE  (256 * 1024)  /* 256 KB */

#define URL_SIZE     256

extern int wl_current_status;
extern int wl_ap_id;