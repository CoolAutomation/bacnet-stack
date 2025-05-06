/**************************************************************************
 *
 * Copyright (C) 2008 Steve Karg <skarg@users.sourceforge.net>
 * Updated by Nikola Jelic 2011 <nikola.jelic@euroicc.com>
 *
 * SPDX-License-Identifier: MIT
 *
 *********************************************************************/
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <libubus.h>
#include <poll.h>
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
/* BACnet Stack API */
#include "bacnet/bacaddr.h"
#include "bacnet/npdu.h"
#include "bacnet/datalink/mstp.h"
#include "bacnet/datalink/dlmstp.h"
#include "bacnet/basic/sys/ringbuf.h"
#include "bacnet/basic/sys/debug.h"
/* OS Specific include */
#include "bacport.h"

#define LOG_MODULE "ports/linux/dlmstp_ubus"
#include "bacnet/basic/sys/log.h"

/* count must be a power of 2 for ringbuf library */
#define MSTP_RX_QUEUE_SIZE 8
static DLMSTP_PACKET RX_Buffer[MSTP_RX_QUEUE_SIZE];
static RING_BUFFER RX_Queue;

static int This_Station;
static int Nmax_info_frames;
static int Nmax_master;
static int baud;

static char ifname[32];
static struct ubus_context *ctx;
static struct ubus_event_handler listener;
static struct blob_buf b;

void dlmstp_cleanup(void)
{
    ubus_free( ctx );
}

/* returns number of bytes sent on success, zero on failure */
int dlmstp_send_pdu(
    BACNET_ADDRESS *dest, /* destination address */
    BACNET_NPDU_DATA *npdu_data, /* network information */
    uint8_t *pdu, /* any data to be sent - may be null */
    unsigned pdu_len)
{ /* number of bytes of data */
    static uint8_t buffer[1/*DST*/+1/*DER*/+DLMSTP_MPDU_MAX];
    char eventname[64];

    uint8_t data_expecting_reply;
    uint8_t destination_mac;

    data_expecting_reply = npdu_data->data_expecting_reply == true ? 1 : 0;
    if (dest && dest->mac_len) {
        destination_mac = dest->mac[0];
    } else {
        /* mac_len = 0 is a broadcast address */
        destination_mac = MSTP_BROADCAST_ADDRESS;
    }

    /* Send ubus event containing [DST] [DER] [PDU] */
    blob_buf_init( &b, 0 );
    buffer[0] = destination_mac;
    buffer[1] = data_expecting_reply;
    memcpy( &buffer[2], &pdu[0], pdu_len );
    blobmsg_add_field( &b, BLOBMSG_TYPE_UNSPEC, "payload",
                       buffer, 1/*DST*/+1/*DER*/+pdu_len );

    snprintf( eventname, ARRAY_SIZE(eventname), "%s/tx", ifname );
    ubus_send_event( ctx, eventname, b.head );
    return pdu_len;
}

uint16_t dlmstp_receive(
    BACNET_ADDRESS *src, /* source address */
    uint8_t *pdu, /* PDU data */
    uint16_t max_pdu, /* amount of space available in the PDU  */
    unsigned timeout_ms )
{ /* milliseconds to wait for a packet */
    uint16_t pdu_len = 0;
    struct pollfd fds[1];
    DLMSTP_PACKET *pkt;
    int ret;

    (void)max_pdu;
    /* see if there is a packet available, and a place
       to put the reply (if necessary) and process it */

    fds[0].fd = ctx->sock.fd;
    fds[0].events = POLLIN;

    /* No need to wait if RX queue already has pending packet */
    if( !Ringbuf_Empty(&RX_Queue) )
        timeout_ms = 0;

    ret = poll( fds, ARRAY_SIZE(fds), timeout_ms );
    if( ret == -1 )
    {
        log_warn( "poll() failed with errno %d (%s)",
                   errno, strerror(errno) );
    }
    else if( ret )
    {
        /* Check if there are any ubus events to handle */
        if( fds[0].revents & POLLIN )
        {
            /*
            * If there are ubus events containing a PDU, the RX_Queue
            * will be filled with them thanks to `dlmstp_ubus_receive`
            */
            ubus_handle_event( ctx );
        }
    }

    /* Have we received any packets? */
    if( !Ringbuf_Empty(&RX_Queue) )
    {
        pkt = (DLMSTP_PACKET *)Ringbuf_Peek( &RX_Queue );
        if( pkt->pdu_len )
        {
            pdu_len = pkt->pdu_len;
            if( src )
            {
                memmove( src, &pkt->address, sizeof(pkt->address) );
            }
            if( pdu )
            {
                memmove( pdu, &pkt->pdu,
                         pdu_len <= max_pdu ? pdu_len : max_pdu );
            }
        }
        (void)Ringbuf_Pop( &RX_Queue, NULL );
        log_debug( "MS/TP: Packet received from RX queue (%d/%d)",
                   Ringbuf_Count(&RX_Queue), MSTP_RX_QUEUE_SIZE );
    }

    return pdu_len;
}

void dlmstp_fill_bacnet_address(BACNET_ADDRESS *src, uint8_t mstp_address)
{
    int i = 0;

    if (mstp_address == MSTP_BROADCAST_ADDRESS) {
        /* mac_len = 0 if broadcast address */
        src->mac_len = 0;
        src->mac[0] = 0;
    } else {
        src->mac_len = 1;
        src->mac[0] = mstp_address;
    }
    /* fill with 0's starting with index 1; index 0 filled above */
    for (i = 1; i < MAX_MAC_LEN; i++) {
        src->mac[i] = 0;
    }
    src->net = 0;
    src->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        src->adr[i] = 0;
    }
}

void dlmstp_set_mac_address(uint8_t mac_address)
{
    /* Master Nodes can only have address 0-127 */
    if (mac_address <= 127) {
        This_Station = mac_address;
        if (mac_address > Nmax_master) {
            dlmstp_set_max_master(mac_address);
        }
    }

    return;
}

uint8_t dlmstp_mac_address(void)
{
    return This_Station;
}

/* This parameter represents the value of the Max_Info_Frames property of */
/* the node's Device object. The value of Max_Info_Frames specifies the */
/* maximum number of information frames the node may send before it must */
/* pass the token. Max_Info_Frames may have different values on different */
/* nodes. This may be used to allocate more or less of the available link */
/* bandwidth to particular nodes. If Max_Info_Frames is not writable in a */
/* node, its value shall be 1. */
void dlmstp_set_max_info_frames(uint8_t max_info_frames)
{
    if (max_info_frames >= 1) {
        Nmax_info_frames = max_info_frames;
    }

    return;
}

uint8_t dlmstp_max_info_frames(void)
{
    return Nmax_info_frames;
}

/* This parameter represents the value of the Max_Master property of the */
/* node's Device object. The value of Max_Master specifies the highest */
/* allowable address for master nodes. The value of Max_Master shall be */
/* less than or equal to 127. If Max_Master is not writable in a node, */
/* its value shall be 127. */
void dlmstp_set_max_master(uint8_t max_master)
{
    if (max_master <= 127) {
        if (This_Station <= max_master) {
            Nmax_master = max_master;
        }
    }

    return;
}

uint8_t dlmstp_max_master(void)
{
    return Nmax_master;
}

void dlmstp_get_my_address(BACNET_ADDRESS *my_address)
{
    int i = 0; /* counter */

    my_address->mac_len = 1;
    my_address->mac[0] = This_Station;
    my_address->net = 0; /* local only, no routing */
    my_address->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        my_address->adr[i] = 0;
    }

    return;
}

void dlmstp_get_broadcast_address(BACNET_ADDRESS *dest)
{ /* destination address */
    int i = 0; /* counter */

    if (dest) {
        dest->mac_len = 1;
        dest->mac[0] = MSTP_BROADCAST_ADDRESS;
        dest->net = BACNET_BROADCAST_NETWORK;
        dest->len = 0; /* always zero when DNET is broadcast */
        for (i = 0; i < MAX_MAC_LEN; i++) {
            dest->adr[i] = 0;
        }
    }

    return;
}


static void dlmstp_ubus_receive( struct ubus_context *ctx,
                                 struct ubus_event_handler *ev,
                                 const char *type,
                                 struct blob_attr *msg )
{
    struct blob_attr* tb[1];
    uint16_t pdu_len = 0;
    unsigned char *pdu_data;
    uint8_t *payload;
    uint8_t src;
    DLMSTP_PACKET *pkt;
    static const struct blobmsg_policy policy =
        { "payload", BLOBMSG_TYPE_UNSPEC };

    (void)ev;
    (void)type;
    (void)ctx;

    blobmsg_parse( &policy, 1, tb, blob_data(msg), blob_len(msg) );
    if( !tb[0] )
    {
        log_warn( "Failed to parse ubus event %s", type );
        return;
    }

    pkt = (DLMSTP_PACKET *)Ringbuf_Data_Peek( &RX_Queue );
    if( pkt == NULL )
    {
        log_err( "MS/TP: RX packet queue is full" );
        return;
    }

    /* payload contains [SRC] [PDU] */
    payload = blobmsg_data(tb[0]);

    pdu_len = blobmsg_data_len(tb[0]) - 1;
    pdu_data = &payload[1];
    if( pdu_len > sizeof(RX_Buffer[0].pdu) )
    {
        pdu_len = sizeof(RX_Buffer[0].pdu);
    }
    if( pdu_len == 0 )
    {
        log_warn( "MS/TP: PDU Length is 0!" );
    }

    src = payload[0];
    memmove( (void *)&pkt->pdu[0], (void *)&pdu_data[0], pdu_len );
    dlmstp_fill_bacnet_address( &pkt->address, src );

    pkt->pdu_len = pdu_len;
    Ringbuf_Data_Put( &RX_Queue, (uint8_t *)pkt );
    log_debug( "MS/TP: Packet sent to RX queue (%d/%d)",
               Ringbuf_Count(&RX_Queue), MSTP_RX_QUEUE_SIZE );
}

void dlmstp_set_baud_rate(uint32_t _baud)
{
    baud = _baud;
}

uint32_t dlmstp_baud_rate(void)
{
    return baud;
}

bool dlmstp_init( char *_ifname )
{
    char eventname[64];

    /* Save interface name */
    strncpy( ifname, _ifname, ARRAY_SIZE(ifname)-1 );

    /* Initialize ubus connection */
    ctx = ubus_connect( NULL );
    if( !ctx )
    {
        log_err( "Connection to ubus failed" );
        return false;
    }

    /* Register handler for RX messages from ubus */
    snprintf( eventname, ARRAY_SIZE(eventname), "%s/rx", ifname );
	memset( &listener, 0, sizeof(listener) );
	listener.cb = dlmstp_ubus_receive;
    ubus_register_event_handler( ctx, &listener, eventname );
    log_info( "MSTP: Subscribe to ubus event %s", eventname );

    /* Initialize RX packet queue */
    Ringbuf_Init(
        &RX_Queue, (uint8_t *)&RX_Buffer, sizeof(DLMSTP_PACKET),
        MSTP_RX_QUEUE_SIZE );
    return true;
}
