/**************************************************************************
 *
 * Copyright (C) 2006 Steve Karg <skarg@users.sourceforge.net>
 *
 * SPDX-License-Identifier: MIT
 *
 *********************************************************************/
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
/* BACnet Stack API */
#include "bacnet/bacdcode.h"
#include "bacnet/whohas.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/tsm/tsm.h"

/** @file h_whohas.c  Handles Who-Has requests. */

/** Local function which responds with either the requested object name
 *  or object ID, if the Device has a match.
 *  @param data [in] The decoded who-has payload from the request.
 */
static void match_name_or_object(const BACNET_WHO_HAS_DATA *data)
{
    BACNET_OBJECT_TYPE object_type = OBJECT_NONE;
    uint32_t object_instance = 0;
    bool found = false;
    BACNET_CHARACTER_STRING object_name;

    /* do we have such an object?  If so, send an I-Have.
       note: we should have only 1 of such an object */
    if (data->is_object_name) {
        /* valid name in my device? */
        found = Device_Valid_Object_Name(
            &data->object.name, &object_type, &object_instance);
        if (found) {
            Send_I_Have(
                Device_Object_Instance_Number(),
                (BACNET_OBJECT_TYPE)object_type, object_instance,
                &data->object.name);
        }
    } else {
        /* valid object_name copy in my device? */
        found = Device_Object_Name_Copy(
            (BACNET_OBJECT_TYPE)data->object.identifier.type,
            data->object.identifier.instance, &object_name);
        if (found) {
            Send_I_Have(
                Device_Object_Instance_Number(),
                (BACNET_OBJECT_TYPE)data->object.identifier.type,
                data->object.identifier.instance, &object_name);
        }
    }
}

/** Handler for Who-Has requests, with broadcast I-Have response.
 * Will respond if the device Object ID matches, and we have
 * the Object or Object Name requested.
 *
 * @ingroup DMDOB
 * @param service_request [in] The received message to be handled.
 * @param service_len [in] Length of the service_request message.
 * @param src [in] The BACNET_ADDRESS of the message's source.
 */
void handler_who_has(
    uint8_t *service_request, uint16_t service_len, BACNET_ADDRESS *src)
{
    int len = 0;
    BACNET_WHO_HAS_DATA data;
    bool directed_to_me = false;

    (void)src;
    len = whohas_decode_service_request(service_request, service_len, &data);
    if (len > 0) {
        if ((data.low_limit == -1) || (data.high_limit == -1)) {
            directed_to_me = true;
        } else if (
            (Device_Object_Instance_Number() >= (uint32_t)data.low_limit) &&
            (Device_Object_Instance_Number() <= (uint32_t)data.high_limit)) {
            directed_to_me = true;
        }
        if (directed_to_me) {
            match_name_or_object(&data);
        }
    }
}

#ifdef BAC_ROUTING /* was for BAC_ROUTING - delete in 2/2012 if still unused \
                    */
/* EKH: I restored this to BAC_ROUTING (from DEPRECATED) because I found that
   the server demo with the built-in
   virtual Router did not insert the SADRs of the virtual devices on the virtual
   network without it */

/** Handler for Who-Has requests in the virtual routing setup,
 * with broadcast I-Have response.
 * Will respond if the device Object ID matches, and we have
 * the Object or Object Name requested.
 *
 * @ingroup DMDOB
 * @param service_request [in] The received message to be handled.
 * @param service_len [in] Length of the service_request message.
 * @param src [in] The BACNET_ADDRESS of the message's source (ignored).
 */
void handler_who_has_for_routing(
    uint8_t *service_request, uint16_t service_len, BACNET_ADDRESS *src)
{
    int len = 0;
    BACNET_WHO_HAS_DATA data;
    int32_t dev_instance;
    int cursor = 0; /* Starting hint */
    int my_list[2] = { 0, -1 }; /* Not really used, so dummy values */
    BACNET_ADDRESS bcast_net;

    (void)src;
    len = whohas_decode_service_request(service_request, service_len, &data);
    if (len > 0) {
        /* Go through all devices, starting with the root gateway Device */
        memset(&bcast_net, 0, sizeof(BACNET_ADDRESS));
        bcast_net.net =
            BACNET_BROADCAST_NETWORK; /* That's all we have to set */
        while (Routed_Device_GetNext(&bcast_net, my_list, &cursor)) {
            dev_instance = Device_Object_Instance_Number();
            if ((data.low_limit == -1) || (data.high_limit == -1) ||
                ((dev_instance >= data.low_limit) &&
                 (dev_instance <= data.high_limit))) {
                match_name_or_object(&data);
            }
        }
    }
}
#endif /* BAC_ROUTING */
