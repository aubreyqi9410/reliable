
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
  bq_t *rec_bq;

  int send_seqno;
  int ackno;

  int sent_eof;

  int read_eof;
  int printed_eof;

  int nagle;

};
rel_t *rel_list;


typedef struct send_bq_element {
    int sent;
    struct timespec time_sent;
    packet_t pkt;
} send_bq_element_t;


/* ABSTRACT TODOS:
 *
 * Resend dropped packets properly
 * Nagle - single outstanding small packet
 * Sent & Received EOF - connection teardown
 */



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
  r->rec_bq = bq_new(cc->window, sizeof(packet_t));
  bq_increase_head_seq_to(r->rec_bq,1);

  r->send_seqno = 1;
  r->ackno = 1;

  r->printed_eof = 0;
  r->sent_eof = 0;
  r->read_eof = 0;

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

/* Creates and sends an ack packet.
 */
void 
rel_send_ack (rel_t *r, int ackno)
{

    /* Acks cannot regress */

    assert(ackno >= r->ackno);
    r->ackno = ackno;

    /* Build the ack packet */

    packet_t ack_packet;
    ack_packet.ackno = htonl(ackno);

    ack_packet.len = htons(8);
    ack_packet.cksum = 0;
    ack_packet.cksum = cksum(&ack_packet, 8);

    /* Send it off */

    conn_sendpkt (r->c, &ack_packet, 8);
}

/* Sends a buffered packet, and handles updating the meta data
 * associated with the packet.
 */
int 
rel_send_buffered_pkt(rel_t *r, send_bq_element_t* elem) 
{
    assert(r);
    assert(elem);
    assert(ntohl(elem->pkt.seqno) < bq_get_head_seq(r->send_bq) + r->window);
    assert(ntohl(elem->pkt.seqno) > 0);

    /* Update records associated with the packet */

    elem->sent = 1;
    clock_gettime (CLOCK_MONOTONIC, &elem->time_sent);

    /* Update to the current ack number */

    elem->pkt.ackno = htons(r->ackno);

    /* Do the dirty deed */

    conn_sendpkt(r->c, &(elem->pkt), ntohs(elem->pkt.len));

    /* Update our status if this was an EOF packet */
    if (ntohs(elem->pkt.len) == 12) {
        r->sent_eof = 1;
    }

    return 1;
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

/* This function gets called on every packet receipt, to handle the
 * ackno in the packet.
 */
void
rel_recvack (rel_t *r, int ackno)
{
    assert(r); 

    /* Shouldn't get acks for stuff we haven't sent,
     * or an ack that's lower than a previous ack */

    assert(ackno < bq_get_head_seq(r->send_bq) + r->window + 1);
    assert(ackno >= bq_get_head_seq(r->send_bq));

    /* Move the head of the window to the ackno */

    bq_increase_head_seq_to(r->send_bq, ackno);

    /* Assert that moving the head didn't mess with our buffered
     * packets. We shouldn't have buffered something beyond what
     * we read in. */

    assert(!bq_element_buffered(r->send_bq,r->send_seqno));

    /* Send any buffered packets that are newly within the window */

    int i;
    for (i = ackno; i < ackno + r->window; i++) {
        send_bq_element_t *elem = bq_get_element(r->send_bq, i);
        if (!elem->sent) {
            rel_send_buffered_pkt(r, elem);
        }
    }
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    if (!rel_packet_valid(pkt,n)) return;

    /* Do all the endinannness in one place */

    pkt->len = ntohs(pkt->len);
    pkt->seqno = ntohl(pkt->seqno);
    pkt->ackno = ntohl(pkt->ackno);

    /* Read ack nums on all packets */

    rel_recvack (r, pkt->ackno);

    /* Insert all data packets into the read buffer for
     * when we get some space for output. */

    if (n > 8) {
        bq_insert_at(r->rec_bq, pkt->seqno, pkt);

        /* TODO-HACK: rlib isn't calling rel_output on its own */

        rel_output(r);
    }
}

/* Reads up to 500 bytes of data from conn_input() into the
 * packet passed in, writing everything in network byte order,
 * and sets metadata.
 */

int
rel_read_input_into_packet(rel_t *r, send_bq_element_t *elem)
{

    /* Read data directly into our packet */

    int len = conn_input(r->c, &(elem->pkt.data[0]), 500);
    if (len == 0) return -1; /* no more data to read */
    if (len == -1) {
        len = 0; /* send an EOF */
    }

    elem->pkt.ackno = htonl(0); /* TODO */
    elem->pkt.seqno = htonl(r->send_seqno);
    elem->pkt.len = htons(12 + len);
    elem->pkt.cksum = 0;
    elem->pkt.cksum = cksum(&elem->pkt, 12 + len);

    /* Time sent is 1970, so when there's free window, it'll be sent */

    elem->time_sent.tv_sec = 0;
    elem->time_sent.tv_nsec = 0;
    elem->sent = 0;

    return len;
}

void
rel_read (rel_t *r)
{

    /* If we've read an EOF, no reason to even ge started */

    if (r->read_eof) return;

    send_bq_element_t elem;

    while (1) {

        /* Check for overrunning send buffer memory */

        assert(r->send_seqno <= bq_get_tail_seq(r->send_bq));

        /* Read up to 500 bytes into a packet, overwriting the old
         * contents of elem */

        int len = rel_read_input_into_packet(r, &elem);
        if (len == -1) return; /* no more data to read */

        /* If this packet sequence number is within the window,
         * then send it */

        if (r->send_seqno < bq_get_head_seq(r->send_bq) + r->window) {
            rel_send_buffered_pkt(r,&elem);
        }

        /* Record the packet in the queue, so that we can (re)send it in the 
         * future. */

        bq_insert_at(r->send_bq, r->send_seqno, &elem);

        /* Assert that this is the highest element we've inserted */
        
        assert(!bq_element_buffered(r->send_bq, r->send_seqno + 1));

        r->send_seqno ++;

        /* If we buffered an EOF, then we're done with reading */

        if (len == 0) {
            r->read_eof = 1;
            return;
        }
    }
}

void
rel_output (rel_t *r)
{

    /* If we've already printed an EOF, then we're done. */

    if (r->printed_eof) return;

    while (1) {

        /* Read from the head of the received packets buffer queue,
         * if we have any received packets waiting */

        int rec_seqno = bq_get_head_seq(r->rec_bq);
        if (!bq_element_buffered(r->rec_bq, rec_seqno)) return;
        packet_t *pkt = bq_get_element(r->rec_bq, rec_seqno);

        int bufspace = conn_bufspace(r->c);

        /* Print the whole packet, then ack */

        if (bufspace > pkt->len-12) {
            conn_output(r->c, pkt->data, pkt->len-12);
            bq_increase_head_seq_to(r->rec_bq, rec_seqno + 1);

            rel_send_ack(r, pkt->seqno + 1);

            /* If we just printed out an EOF, update our status */
        
            r->printed_eof = 1;
        }

        /* Edge case: only enough buffer to print part of the packet */

        else if (bufspace > 0) {
            conn_output(r->c, pkt->data, bufspace);

            /* Shift the packet data over, removing what we've already printed */

            memcpy(&(pkt->data[0]), &(pkt->data[bufspace]), pkt->len - bufspace);
            pkt->len -= bufspace;
            return;
        }

        /* If we have no space left, time to quit */

        else if (bufspace == 0) return;
    }
}

void
rel_timer ()
{
    /* Iterate over all the reliable connections */

    rel_t *r;
    for (r = rel_list; r != NULL; r = r->next) {

        /* Send window is [head of buffer queue, head of buffer queue + window size],
         * so we iterate over the send window, and send anything that's timed out. */

        printf("Timer checking buffered packets (%i):\n", r->send_seqno);
        int i = 0;
        for (i = bq_get_head_seq(r->send_bq); i < bq_get_head_seq(r->send_bq) + r->window; i++) {
            printf("%i : ",i);

            /* This is just a safety check, in case we haven't read in this part
             * of the send window yet */

            if (!bq_element_buffered(r->send_bq, i)) continue;

            /* Borrowed need_timer_in from rlib: just checks whether it's
             * been r->timeout ms since elem->time_sent */

            send_bq_element_t *elem = bq_get_element(r->send_bq, i);
            printf("%li\n",need_timer_in (&(elem->time_sent), r->timeout));
            if (need_timer_in (&(elem->time_sent), r->timeout)) {
                rel_send_buffered_pkt(r,elem);
            }
        }
    }
}
