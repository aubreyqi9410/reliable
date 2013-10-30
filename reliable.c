
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"
#include "bq.h"

#define SEND_BUFFER_SIZE 500


struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* My additions */

  int timeout;
  int window;
  bq_t *send_bq;
  int send_seqno;
  bq_t *rec_bq;

  int sent_eof;
  int received_eof;

};
rel_t *rel_list;


typedef struct send_bq_element {
    int sent;
    clock_t time_sent;
    packet_t pkt;
} send_bq_element_t;



/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  /* My additions */

  r->timeout = cc->timeout;
  r->window = cc->window;
  r->send_bq = bq_new(SEND_BUFFER_SIZE, sizeof(send_bq_element_t));
  bq_increase_head_seq_to(r->send_bq,1);
  r->send_seqno = 1;
  r->rec_bq = bq_new(cc->window, sizeof(packet_t));
  bq_increase_head_seq_to(r->rec_bq,1);

  r->received_eof = 0;
  r->sent_eof = 0;

  return r;
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* My additions */

  bq_destroy(r->send_bq);
  bq_destroy(r->rec_bq);
}

void
rel_DEBUG (char *c, size_t n)
{
    printf("\nDEBUG %i chars\n", (int)n);
    int i;
    for (i = 0; i < n; i++) {
        printf("%c",c[i]);
    }
    printf("\n\n");
}

int 
rel_packet_valid (packet_t *pkt, size_t n)
{
    if (ntohs(pkt->len) > n) return 0;
    int cksum_buf = pkt->cksum;
    pkt->cksum = 0;
    if (cksum_buf != cksum(pkt, n)) return 0;
    return 1;
}

void 
rel_send_ack (rel_t *r, int ackno)
{
    packet_t ack_packet;
    ack_packet.ackno = htonl(ackno);

    ack_packet.len = htons(8);
    ack_packet.cksum = 0;
    ack_packet.cksum = cksum(&ack_packet, 8);

    conn_sendpkt (r->c, &ack_packet, 8);
}

/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{
}

void
rel_recvack (rel_t *r, int ackno)
{
    int i;

    /* Scan from our queue head to ackno + window size */

    for (i = bq_get_head_seq(r->send_bq); i < ackno + r->window; i++) {

        /* If we have a buffered element that hasn't been sent, send it */

        if (bq_element_buffered(r->send_bq, i)) {
            send_bq_element_t *elem = bq_get_element(r->send_bq, i);
            if (!elem->sent) {
                elem->time_sent = clock();
                elem->sent = 1;
                conn_sendpkt(r->c, &(elem->pkt), ntohs(elem->pkt.len));
            }
        }
    }

    bq_increase_head_seq_to(r->send_bq, ackno);
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    if (!rel_packet_valid(pkt,n)) return;

    pkt->len = ntohs(pkt->len);
    pkt->seqno = ntohl(pkt->seqno);
    pkt->ackno = ntohl(pkt->ackno);

    /* Ack */

    rel_recvack (r, pkt->ackno);

    /* Data */

    if (n > 8) {
        bq_insert_at(r->rec_bq, pkt->seqno, pkt);
        /* TODO-HACK: rlib isn't calling rel_output on its own */
        rel_output(r);
    }
}

void
rel_read (rel_t *r)
{
    send_bq_element_t elem;

    while (1) {
        assert(r->send_seqno > bq_get_tail_seq(r->send_bq));

        /* Read data directly into our packet */

        int len = conn_input(r->c, &(elem.pkt.data[0]), 500);
        if (len == 0) return;
        if (len == -1) {
            len = 0; /* send an EOF */
        }

        elem.pkt.ackno = htonl(0); /* TODO */
        elem.pkt.seqno = htonl(r->send_seqno);
        elem.pkt.len = htons(12 + len);
        elem.pkt.cksum = 0;
        elem.pkt.cksum = cksum(&elem.pkt, 12 + len);

        /* Send window is [head of buffer queue, head of buffer queue + window size] */

        if (r->send_seqno < bq_get_head_seq(r->send_bq) + r->window) {
            elem.time_sent = clock();
            elem.sent = 1;
            conn_sendpkt(r->c, &elem.pkt, 12 + len);
        }
        else {
            elem.time_sent = 0; /* Time sent is 1970, so when there's free window, it'll be sent */
            elem.sent = 0;
        }

        bq_insert_at(r->send_bq, r->send_seqno, &elem);

        r->send_seqno ++;
    }
}

void
rel_output (rel_t *r)
{
    while (1) {

        /* Read from the head of the received packets buffer queue */

        int rec_seqno = bq_get_head_seq(r->rec_bq);
        if (!bq_element_buffered(r->rec_bq, rec_seqno)) return;
        packet_t *pkt = bq_get_element(r->rec_bq, rec_seqno);

        int bufspace = conn_bufspace(r->c);

        /* Print the whole packet, then ack */

        if (bufspace > pkt->len) {
            conn_output(r->c, pkt->data, pkt->len-12);
            bq_increase_head_seq_to(r->rec_bq, rec_seqno + 1);

            rel_send_ack(r, pkt->seqno + 1);
        }

        /* Edge case: only enough buffer to print part of the packet */

        else if (bufspace > 0) {
            conn_output(r->c, pkt->data, bufspace);

            /* Shift the packet data over, removing what we've already printed */

            memcpy(&(pkt->data[0]), &(pkt->data[bufspace]), pkt->len - bufspace);
            pkt->len -= bufspace;
            return;
        }
        else if (bufspace == 0) return;
    }
}

void
rel_timer ()
{
    /* Retransmit any packets that need to be retransmitted */

    rel_t *r = rel_list;
    while (r != NULL) {
        int i = 0;

        /* Send window is [head of buffer queue, head of buffer queue + window size] */

        for (i = bq_get_head_seq(r->send_bq); i < bq_get_head_seq(r->send_bq) + r->window; i++) {
            if (bq_element_buffered(r->send_bq, i)) {
                send_bq_element_t *elem = bq_get_element(r->send_bq, i);

                /* Get milliseconds since packet was last sent.
                 * Packets that haven't been sent have ms_diff = 40 yrs */

                clock_t now = clock();
                int ms_diff = (int)((((float)now - (float)elem->time_sent) / CLOCKS_PER_SEC) * 1000);

                if (ms_diff > r->timeout) {
                    elem->time_sent = now;
                    elem->sent = 1;
                    conn_sendpkt(r->c, &elem->pkt, ntohl(elem->pkt.len));
                }
            }
        }
        r = r->next;
        break;
    }
}
