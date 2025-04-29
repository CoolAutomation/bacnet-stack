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

/** @file linux/dlmstp.c  Provides Linux-specific DataLink functions for MS/TP.
 */

/* Number of MS/TP Packets Rx/Tx */
static uint16_t MSTP_Packets = 0;

/* packet queues */
static DLMSTP_PACKET Receive_Packet;
/* mechanism to wait for a packet */
static pthread_cond_t Receive_Packet_Flag;
static pthread_mutex_t Receive_Packet_Mutex;
static pthread_mutex_t Ring_Buffer_Mutex;
static pthread_mutex_t Thread_Mutex;
static pthread_t hThread;
static bool run_thread;
static char ifname[32];

/* data structure for MS/TP PDU Queue */
struct mstp_pdu_packet {
    bool data_expecting_reply;
    uint8_t destination_mac;
    uint16_t length;
    uint8_t buffer[DLMSTP_MPDU_MAX];
};
/* count must be a power of 2 for ringbuf library */
#ifndef MSTP_PDU_PACKET_COUNT
#define MSTP_PDU_PACKET_COUNT 8
#endif
static struct mstp_pdu_packet PDU_Buffer[MSTP_PDU_PACKET_COUNT];
static RING_BUFFER PDU_Queue;

static int This_Station;
static int Nmax_info_frames;
static int Nmax_master;
static int baud;
static struct ubus_context *ctx;
static struct blob_buf b;

#define NS_PER_S 1000000000 /* nano-seconds per second */

/**
 * Add a certain number of nanoseconds to the specified time.
 *
 * @param ts - The time to which to add to.
 * @param ns - The number of nanoseconds to add.  Allowed range
 *      is -NS_PER_S..NS_PER_S (i.e., plus minus one second).
 */
static void timespec_add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    if (ts->tv_nsec > NS_PER_S) {
        ts->tv_nsec -= NS_PER_S;
        ts->tv_sec += 1;
    } else if (ts->tv_nsec < 0) {
        ts->tv_nsec += NS_PER_S;
        ts->tv_sec -= 1;
    }
}

static void get_abstime(struct timespec *abstime, unsigned long milliseconds)
{
    clock_gettime(CLOCK_MONOTONIC, abstime);
    if (milliseconds > 1000) {
        log_err(
            "DLMSTP: limited timeout of %lums to 1000ms",
            milliseconds);
        milliseconds = 1000;
    }
    timespec_add_ns(abstime, 1000000 * milliseconds);
}

void dlmstp_cleanup(void)
{
    /* TODO: ubus disconnect */
}

/* returns number of bytes sent on success, zero on failure */
int dlmstp_send_pdu(
    BACNET_ADDRESS *dest, /* destination address */
    BACNET_NPDU_DATA *npdu_data, /* network information */
    uint8_t *pdu, /* any data to be sent - may be null */
    unsigned pdu_len)
{ /* number of bytes of data */
    int bytes_sent = 0;
    struct mstp_pdu_packet *pkt;
    unsigned i = 0;
    pthread_mutex_lock(&Ring_Buffer_Mutex);
    pkt = (struct mstp_pdu_packet *)Ringbuf_Data_Peek(&PDU_Queue);
    if (pkt) {
        pkt->data_expecting_reply = npdu_data->data_expecting_reply;
        for (i = 0; i < pdu_len; i++) {
            pkt->buffer[i] = pdu[i];
        }
        pkt->length = pdu_len;
        if (dest && dest->mac_len) {
            pkt->destination_mac = dest->mac[0];
        } else {
            /* mac_len = 0 is a broadcast address */
            pkt->destination_mac = MSTP_BROADCAST_ADDRESS;
        }
        if (Ringbuf_Data_Put(&PDU_Queue, (uint8_t *)pkt)) {
            bytes_sent = pdu_len;
        }
    }
    pthread_mutex_unlock(&Ring_Buffer_Mutex);

    return bytes_sent;
}

uint16_t dlmstp_receive(
    BACNET_ADDRESS *src, /* source address */
    uint8_t *pdu, /* PDU data */
    uint16_t max_pdu, /* amount of space available in the PDU  */
    unsigned timeout)
{ /* milliseconds to wait for a packet */
    uint16_t pdu_len = 0;
    struct timespec abstime;

    (void)max_pdu;
    /* see if there is a packet available, and a place
       to put the reply (if necessary) and process it */
    pthread_mutex_lock(&Receive_Packet_Mutex);
    get_abstime(&abstime, timeout);
    pthread_cond_timedwait(
        &Receive_Packet_Flag, &Receive_Packet_Mutex, &abstime);
    if (Receive_Packet.ready) {
        if (Receive_Packet.pdu_len) {
            MSTP_Packets++;
            if (src) {
                memmove(
                    src, &Receive_Packet.address,
                    sizeof(Receive_Packet.address));
            }
            if (pdu) {
                memmove(pdu, &Receive_Packet.pdu, sizeof(Receive_Packet.pdu));
            }
            pdu_len = Receive_Packet.pdu_len;
        }
        Receive_Packet.ready = false;
    }
    pthread_mutex_unlock(&Receive_Packet_Mutex);

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
    uint8_t src;
    static const struct blobmsg_policy policy =
        { "payload", BLOBMSG_TYPE_UNSPEC };

    (void)ev;
    (void)type;
    (void)ctx;

    blobmsg_parse( &policy, 1, tb, blob_data(msg), blob_len(msg) );
    if( !tb[0] )
    {
        log_warn("Failed to parse ubus event %s", type );
        return;
    }

    pthread_mutex_lock(&Receive_Packet_Mutex);
    if (Receive_Packet.ready) {
        log_err("MS/TP: Dropped! Not Ready.");
    } else {
        /* bounds check - maybe this should send an abort? */
        pdu_len = blobmsg_data_len(tb[0]) - 1;
        pdu_data = blobmsg_data(tb[0]);
        if (pdu_len > sizeof(Receive_Packet.pdu)) {
            pdu_len = sizeof(Receive_Packet.pdu);
        }
        if (pdu_len == 0) {
            log_warn("MS/TP: PDU Length is 0!");
        }
        src = pdu_data[0];
        memmove(
            (void *)&Receive_Packet.pdu[0], (void *)&pdu_data[1],
            pdu_len);
        dlmstp_fill_bacnet_address(
            &Receive_Packet.address, src);
        Receive_Packet.pdu_len = pdu_len;
        Receive_Packet.ready = true;
        pthread_cond_signal(&Receive_Packet_Flag);
    }
    pthread_mutex_unlock(&Receive_Packet_Mutex);
}

void dlmstp_set_baud_rate(uint32_t _baud)
{
    baud = _baud;
}

uint32_t dlmstp_baud_rate(void)
{
    return baud;
}

static void* dlmstp_ubus_task( void *arg )
{
    struct pollfd fds[1];
    char eventname[64];
	struct ubus_event_handler listener;
    struct mstp_pdu_packet *pkt;
    static uint8_t buffer[1/*DST*/+1/*DER*/+DLMSTP_MPDU_MAX];

    int ret;

    (void)arg;

    /* Initialize ubus connection */
    ctx = ubus_connect( NULL );
    if( !ctx )
    {
        log_err( "Connection to ubus failed" );
        return NULL;
    }

    /* Register handler for RX messages from ubus */
    snprintf( eventname, ARRAY_SIZE(eventname), "%s/rx", ifname );
	memset( &listener, 0, sizeof(listener) );
	listener.cb = dlmstp_ubus_receive;
    ret = ubus_register_event_handler( ctx, &listener, eventname );

    snprintf( eventname, ARRAY_SIZE(eventname), "%s/tx", ifname );

    fds[0].fd = ctx->sock.fd;
    fds[0].events = POLLIN;

    /* TODO: use mq queue instead of ringbuf for poll(2)? */
    /*
    fds[1].fd = ld->RX.mqdes;
    fds[1].events = POLLIN;
    */
    while( run_thread )
    {
        /* TODO: lock */
        while( !Ringbuf_Empty(&PDU_Queue) )
        {
            /* ubus send event to /tx */
            pkt = (struct mstp_pdu_packet *)Ringbuf_Peek( &PDU_Queue );
            blob_buf_init( &b, 0 );
            buffer[0] = pkt->destination_mac;
            buffer[1] = pkt->data_expecting_reply;
            memcpy( &buffer[2], &pkt->buffer[0], pkt->length );
            blobmsg_add_field( &b, BLOBMSG_TYPE_UNSPEC, "payload",
                               buffer, 1/*DST*/+1/*DER*/+pkt->length );
            ubus_send_event( ctx, eventname, b.head );
            (void)Ringbuf_Pop( &PDU_Queue, NULL );
        }
        /* TODO: unlock */

        ret = poll( fds, ARRAY_SIZE(fds), 10/*ms*/ );
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
                ubus_handle_event( ctx );
            }
        }
        else
        {
            /* No data */
        }
    }

    /* Never */
    return NULL;
}

bool dlmstp_init( char *_ifname )
{
    pthread_condattr_t attr;
    int rv = 0;

    strncpy( ifname, _ifname, ARRAY_SIZE(ifname)-1 );
    pthread_condattr_init(&attr);
    if ((rv = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) != 0) {
        log_err(
            "[MS/TP Interface: %s] failed to set MONOTONIC clock",
            ifname);
        exit(1);
    }

    pthread_mutex_init(&Ring_Buffer_Mutex, NULL);
    pthread_mutex_init(&Thread_Mutex, NULL);

    /* initialize PDU queue */
    Ringbuf_Init(
        &PDU_Queue, (uint8_t *)&PDU_Buffer, sizeof(struct mstp_pdu_packet),
        MSTP_PDU_PACKET_COUNT);
    /* initialize packet queue */
    Receive_Packet.ready = false;
    Receive_Packet.pdu_len = 0;
    rv = pthread_cond_init(&Receive_Packet_Flag, &attr);
    if (rv != 0) {
        log_err(
            "[MS/TP Interface: %s] cannot allocate PThread Condition.",
            ifname);
        exit(1);
    }
    rv = pthread_mutex_init(&Receive_Packet_Mutex, NULL);
    if (rv != 0) {
        log_err(
            "[MS/TP Interface: %s] cannot allocate PThread Mutex.",
            ifname);
        exit(1);
    }
    /* start one thread */
    run_thread = true;
    rv = pthread_create(&hThread, NULL, dlmstp_ubus_task, NULL);
    if (rv != 0) {
        log_fatal("Failed to start dlmstp ubus task (ret=%d)", rv);
    }

    return true;
}
