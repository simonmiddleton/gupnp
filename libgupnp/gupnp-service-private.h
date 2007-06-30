/* 
 * Copyright (C) 2007 OpenedHand Ltd.
 *
 * Author: Jorn Baayen <jorn@openedhand.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GUPNP_SERVICE_PRIVATE_H__
#define __GUPNP_SERVICE_PRIVATE_H__

#include <libxml/tree.h>

#include "gupnp-context.h"
#include "gupnp-service.h"

GUPnPService *
_gupnp_service_new_from_element (GUPnPContext *context,
                                 xmlNode      *element,
                                 const char   *location,
                                 const char   *udn,
                                 const char   *url_base);

#endif /* __GUPNP_SERVICE_PRIVATE_H__ */