/*****************************************************************************
 * aggregartp.c: split an RTP stream for several contribution links
 *****************************************************************************
 * Copyright (C) 2009, 2011, 2014-2015 VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/* POLLRDHUP */
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <syslog.h>

#include <bitstream/ietf/rtp.h>

#include "util.h"

#define DEFAULT_RETX_BUFFER 500 /* ms */

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
typedef struct block_t
{
    uint8_t *p_data;
    unsigned int i_size;
    uint64_t i_date;
    struct block_t *p_next;
} block_t;

typedef struct output_t
{
    int i_fd;
    unsigned int i_weight;

    unsigned int i_weighted_size, i_remainder;
} output_t;

static size_t i_asked_payload_size = DEFAULT_PAYLOAD_SIZE;
static size_t i_rtp_header_size = RTP_HEADER_SIZE;

static int i_input_fd;
static bool b_input_tcp;
static block_t *p_input_block = NULL;
static output_t *p_outputs = NULL;
static int i_nb_outputs = 0;
static unsigned int i_max_weight = 0;
static bool b_overwrite_timestamps = false;
static bool b_overwrite_ssrc = false;
static in_addr_t i_ssrc = 0;
static uint16_t i_rtp_seqnum = 0;

static int i_retx_fd = -1;
static bool b_retx_tcp;
static block_t *p_retx_block = NULL;
static block_t *p_retx_first = NULL, *p_retx_last = NULL;
static uint64_t i_retx_buffer = DEFAULT_RETX_BUFFER * 27000;

static void usage(void)
{
    msg_Raw( NULL, "Usage: aggregartp [-i <RT priority>] [-l <syslogtag>] [-t <ttl>] [-w] [-o <SSRC IP>] [-U] [-x <retx buffer>] [-X <retx URL>] [-m <payload size>] [-R <RTP header>] @<src host> <dest host 1>[,<weight 1>] ... [<dest host N>,<weight N>]" );
    msg_Raw( NULL, "    host format: [<connect addr>[:<connect port>]][@[<bind addr][:<bind port>]]" );
    msg_Raw( NULL, "    -w: overwrite RTP timestamps" );
    msg_Raw( NULL, "    -o: overwrite RTP SSRC" );
    msg_Raw( NULL, "    -U: prepend RTP header" );
    msg_Raw( NULL, "    -x: length of the buffer for retransmission requests in ms [default 500]" );
    msg_Raw( NULL, "    -X: retransmission service @host:port[/tcp]" );
    msg_Raw( NULL, "    -m: size of the payload chunk, excluding optional RTP header (default 1316)" );
    msg_Raw( NULL, "    -R: size of the optional RTP header (default 12)" );
    exit(EXIT_FAILURE);
}

/*****************************************************************************
 * NextOutput: pick the output for the next packet
 *****************************************************************************/
static output_t *NextOutput(void)
{
    unsigned int i_min_size = p_outputs[0].i_weighted_size;
    int i, i_output = 0;

    for ( i = 1; i < i_nb_outputs && p_outputs[i].i_weight; i++ )
    {
        if ( p_outputs[i].i_weighted_size < i_min_size )
        {
            i_min_size = p_outputs[i].i_weighted_size;
            i_output = i;
        }
    }

    for ( i = 0; i < i_nb_outputs && p_outputs[i].i_weight; i++ )
        p_outputs[i].i_weighted_size -= i_min_size;

    return &p_outputs[i_output];
}

/*****************************************************************************
 * SendBlock: send a block to a file descriptor
 *****************************************************************************/
static void SendBlock( int i_fd, struct sockaddr *p_sout,
                       socklen_t i_len, block_t *p_block )
{
    if ( sendto( i_fd, p_block->p_data, p_block->i_size, 0, p_sout, i_len )
          < 0 )
    {
        if ( errno == EBADF || errno == ECONNRESET || errno == EPIPE )
        {
            msg_Err( NULL, "write error (%s)", strerror(errno) );
            exit(EXIT_FAILURE);
        }
        else
            /* otherwise do not die because these errors can be transient */
            msg_Warn( NULL, "write error (%s)", strerror(errno) );
    }
}

/*****************************************************************************
 * RetxQueue: store a packet in the retx queue
 *****************************************************************************/
static void RetxQueue( block_t *p_block, uint64_t i_current_date )
{
    p_block->i_date = i_current_date;
    p_block->p_next = NULL;
    rtp_set_marker( p_block->p_data );

    /* Queue block */
    if ( p_retx_last != NULL )
    {
        p_retx_last->p_next = p_block;
        p_retx_last = p_block;
    }
    else
        p_retx_last = p_retx_first = p_block;

    /* Purge old blocks */
    while ( p_retx_first != NULL &&
            p_retx_first->i_date < i_current_date - i_retx_buffer )
    {
        block_t *p_next = p_retx_first->p_next;
        free(p_retx_first);
        p_retx_first = p_next;
    }
    if ( p_retx_first == NULL )
        p_retx_last = NULL;
}

/*****************************************************************************
 * RetxHandle: handle a retx query
 *****************************************************************************/
static void RetxHandle( int i_fd )
{
    ssize_t i_size = RETX_HEADER_SIZE - p_retx_block->i_size;
    uint8_t *p_buffer = p_retx_block->p_data + p_retx_block->i_size;
    sockaddr_t sout;
    socklen_t i_len = sizeof(sout);

    i_size = recvfrom( i_fd, p_buffer, i_size, 0, &sout.so, &i_len );
    if ( i_size < 0 && errno != EAGAIN && errno != EINTR &&
         errno != ECONNREFUSED )
    {
        msg_Err( NULL, "unrecoverable read error, dying (%s)",
                 strerror(errno) );
        exit(EXIT_FAILURE);
    }
    if ( i_size <= 0 ) return;

    p_retx_block->i_size += i_size;

    if ( p_retx_block->i_size != RETX_HEADER_SIZE )
    {
        if ( b_retx_tcp ) return;
        msg_Err( NULL, "invalid retx packet received, dying" );
        exit(EXIT_FAILURE);
    }

    if ( !retx_check(p_retx_block->p_data) )
    {
        msg_Err( NULL, "invalid retx packet, dying" );
        exit(EXIT_FAILURE);
    }

    uint16_t i_seqnum = retx_get_seqnum(p_retx_block->p_data);
    uint16_t i_num = retx_get_num(p_retx_block->p_data);
    block_t *p_block = p_retx_first;
    p_retx_block->i_size = 0;

    while ( p_block != NULL )
    {
        if ( rtp_get_seqnum(p_block->p_data) == i_seqnum )
            break;
        p_block = p_block->p_next;
    }

    if ( p_block == NULL )
    {
        msg_Warn( NULL, "unable to find packet %hu for retx", i_seqnum );
        return;
    }

    while ( i_num && p_block != NULL )
    {
        if ( i_retx_fd == -1 )
        {
            output_t *p_output = NextOutput();
            SendBlock( p_output->i_fd, NULL, 0, p_block );
        }
        else
        {
            SendBlock( i_retx_fd, i_len ? &sout.so : NULL, i_len, p_block );
        }
        p_block = p_block->p_next;
        i_num--;
    }

    if ( i_num )
        msg_Warn( NULL, "unable to find %hu packets after %hu", i_num,
                  i_seqnum );
}

/*****************************************************************************
 * Entry point
 *****************************************************************************/
int main( int i_argc, char **pp_argv )
{
    int c;
    int i_priority = -1;
    const char *psz_syslog_tag = NULL;
    int i_ttl = 0;
    bool b_udp = false;
    struct pollfd *pfd = malloc(sizeof(struct pollfd));
    int i_nb_retx = 1;
    int i_fd;

#define ADD_RETX                                                            \
    pfd = realloc( pfd, ++i_nb_retx * sizeof(struct pollfd) );              \
    pfd[i_nb_retx - 1].fd = i_fd;                                           \
    pfd[i_nb_retx - 1].events = POLLIN;

    while ( (c = getopt( i_argc, pp_argv, "i:l:t:wo:x:X:Um:R:h" )) != -1 )
    {
        switch ( c )
        {
        case 'i':
            i_priority = strtol( optarg, NULL, 0 );
            break;

        case 'l':
            psz_syslog_tag = optarg;
            break;

        case 't':
            i_ttl = strtol( optarg, NULL, 0 );
            break;

        case 'w':
            b_overwrite_timestamps = true;
            break;

        case 'o':
        {
            struct in_addr maddr;
            if ( !inet_aton( optarg, &maddr ) )
                usage();
            i_ssrc = maddr.s_addr;
            b_overwrite_ssrc = true;
            break;
        }

        case 'x':
            i_retx_buffer = strtoll( optarg, NULL, 0 ) * 27000;
            break;

        case 'X':
            i_retx_fd = i_fd = OpenSocket( optarg, 0, 0, 0, NULL, &b_retx_tcp, NULL );
            if ( i_fd == -1 )
            {
                msg_Err( NULL, "unable to set up retx with %s\n", optarg );
                exit(EXIT_FAILURE);
            }

            ADD_RETX
            break;

        case 'U':
            b_udp = true;
            break;

        case 'm':
            i_asked_payload_size = strtol( optarg, NULL, 0 );
            break;

        case 'R':
            i_rtp_header_size = strtol( optarg, NULL, 0 );
            break;

        case 'h':
        default:
            usage();
            break;
        }
    }
    if ( optind >= i_argc - 1 )
        usage();

    if ( psz_syslog_tag != NULL )
        msg_Openlog( psz_syslog_tag, LOG_NDELAY, LOG_USER );

    i_input_fd = OpenSocket( pp_argv[optind], 0, DEFAULT_PORT, 0, NULL,
                             &b_input_tcp, NULL );
    if ( i_input_fd == -1 )
    {
        msg_Err( NULL, "unable to open input socket" );
        exit(EXIT_FAILURE);
    }

    optind++;
    pfd[0].fd = i_input_fd;
    pfd[0].events = POLLIN | POLLERR | POLLRDHUP | POLLHUP;

    while ( optind < i_argc )
    {
        p_outputs = realloc( p_outputs, ++i_nb_outputs * sizeof(output_t) );
        p_outputs[i_nb_outputs - 1].i_fd = i_fd =
            OpenSocket( pp_argv[optind++], i_ttl, 0, DEFAULT_PORT,
                        &p_outputs[i_nb_outputs - 1].i_weight, NULL, NULL );
        if ( p_outputs[i_nb_outputs - 1].i_fd == -1 )
        {
            msg_Err( NULL, "unable to open output socket" );
            exit(EXIT_FAILURE);
        }

        p_outputs[i_nb_outputs - 1].i_weighted_size =
            p_outputs[i_nb_outputs - 1].i_remainder = 0;
        i_max_weight += p_outputs[i_nb_outputs - 1].i_weight;

        if ( i_retx_fd == -1 )
        {
            ADD_RETX
        }
    }
    msg_Dbg( NULL, "%d outputs weight %u%s", i_nb_outputs, i_max_weight,
             i_retx_fd != -1 ? ", with retx" : "" );

    p_retx_block = malloc( sizeof(block_t) + RETX_HEADER_SIZE );
    p_retx_block->p_data = (uint8_t *)p_retx_block + sizeof(block_t);
    p_retx_block->i_size = 0;

    if ( i_priority > 0 )
    {
        struct sched_param param;
        int i_error;

        memset( &param, 0, sizeof(struct sched_param) );
        param.sched_priority = i_priority;
        if ( (i_error = pthread_setschedparam( pthread_self(), SCHED_RR,
                                               &param )) )
        {
            msg_Warn( NULL, "couldn't set thread priority: %s",
                      strerror(i_error) );
        }
    }

    for ( ; ; )
    {
        uint64_t i_current_date;
        if ( poll( pfd, i_nb_retx + 1, -1 ) < 0 )
        {
            int saved_errno = errno;
            msg_Warn( NULL, "couldn't poll(): %s", strerror(errno) );
            if ( saved_errno == EINTR ) continue;
            exit(EXIT_FAILURE);
        }
        i_current_date = wall_Date();

        if ( pfd[0].revents & POLLIN )
        {
            /* Read input block */
            ssize_t i_size, i_wanted_size;
            uint8_t *p_read_buffer;

            if ( b_udp )
                i_wanted_size = i_asked_payload_size + RTP_HEADER_SIZE;
            else
                i_wanted_size = i_asked_payload_size + i_rtp_header_size;

            if ( p_input_block == NULL )
            {
                if ( b_udp )
                {
                    p_input_block = malloc( sizeof(block_t) +
                                            i_asked_payload_size +
                                            RTP_HEADER_SIZE );
                    p_input_block->i_size = RTP_HEADER_SIZE;
                }
                else
                {
                    p_input_block = malloc( sizeof(block_t) +
                                            i_asked_payload_size +
                                            i_rtp_header_size );
                    p_input_block->p_data = (uint8_t *)p_input_block +
                                            sizeof(block_t);
                    p_input_block->i_size = 0;
                }
                p_input_block->p_data = (uint8_t *)p_input_block +
                                        sizeof(block_t);
            }

            p_read_buffer = p_input_block->p_data + p_input_block->i_size;
            i_wanted_size -= p_input_block->i_size;
            i_size = read( i_input_fd, p_read_buffer, i_wanted_size );

            if ( i_size < 0 && errno != EAGAIN && errno != EINTR &&
                 errno != ECONNREFUSED )
            {
                msg_Err( NULL, "unrecoverable read error, dying (%s)",
                         strerror(errno) );
                exit(EXIT_FAILURE);
            }
            if ( i_size <= 0 ) continue;

            p_input_block->i_size += i_size;

            if ( b_input_tcp && i_size != i_wanted_size )
                continue;

            if ( b_udp )
            {
                rtp_set_hdr( p_input_block->p_data );
                rtp_set_type( p_input_block->p_data, RTP_TYPE_TS );
                rtp_set_seqnum( p_input_block->p_data, i_rtp_seqnum );
                i_rtp_seqnum++;
                rtp_set_ssrc( p_input_block->p_data, (uint8_t *)&i_ssrc );
                /* this isn't RFC-compliant, but we assume that at the other
                 * end, the RTP header will be stripped */
                rtp_set_timestamp( p_input_block->p_data,
                                   i_current_date / 300 );
            }
            else
            {
                if ( b_overwrite_ssrc )
                    rtp_set_ssrc( p_input_block->p_data,
                                  (uint8_t *)&i_ssrc );
                if ( b_overwrite_timestamps )
                    rtp_set_timestamp( p_input_block->p_data,
                                       i_current_date / 300 );
            }

            /* Output block */
            output_t *p_output = NextOutput();
            SendBlock( p_output->i_fd, NULL, 0, p_input_block );

            p_output->i_weighted_size += (i_size + p_output->i_remainder)
                                           / p_output->i_weight;
            p_output->i_remainder = (i_size + p_output->i_remainder)
                                           % p_output->i_weight;

            RetxQueue( p_input_block, i_current_date );
            p_input_block = NULL;
        }
        else if ( (pfd[0].revents & (POLLERR | POLLRDHUP | POLLHUP)) )
        {
            msg_Err( NULL, "poll error\n" );
            exit(EXIT_FAILURE);
        }

        int i;
        for ( i = 0; i < i_nb_retx; i++ )
        {
            if ( pfd[i + 1].revents & POLLIN )
            {
                RetxHandle( pfd[i + 1].fd );
            }
        }
    }

    if ( psz_syslog_tag != NULL )
        msg_Closelog();

    return EXIT_SUCCESS;
}
