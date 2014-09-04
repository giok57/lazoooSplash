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

#include <jansson.h>
#include <curl/curl.h>

int wl_current_status;
int wl_ap_id;

void
wl_init(void){
    json_t *root;
    json_error_t error;
    root = json_loads("fava", 0, &error);
}
