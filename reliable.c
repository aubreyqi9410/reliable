
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

#define SEND_BUFFER_SIZE 10000


struct reliable_state {
    rel_t *next;	/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;		/* This is the connection object */

    /* Sock addr storage */

    struct sockaddr_storage ss;

    /* Configurations */

    int timeout;
    int window;
    int single_connection;

    /* Buffer queue for sending and receiving */

    bq_t *send_bq;
    bq_t *rec_bq;

    /* State for sending and receiving */

    int seqno;
    int ackno;

    /* Connection teardown state */

    int read_eof;

    int printed_eof;

    /* Nagle state */

    int nagle_outstanding;
};
rel_t *rel_list;


typedef struct send_bq_element {
    int sent;
    struct timespec time_sent;
    packet_t pkt;
} send_bq_element_t;

/* PRIVATE FUNCTIONS:
 *
 * See implementations for comments.
 */

int rel_recv_ack (rel_t *r, int ackno);
int rel_send_buffered_pkt(rel_t *r, send_bq_element_t* elem);
void rel_send_ack (rel_t *r, int ackno);
int rel_read_input_into_packet(rel_t *r, send_bq_element_t *elem);
int rel_check_finished (rel_t *r);
void rel_ack_check_nagle (rel_t *r, int ackno);
int rel_nagle_constrain_sending_buffered_pkt(rel_t *r, send_bq_element_t* elem);
int rel_packet_valid (packet_t *pkt, size_t n);
int rel_seqno_in_send_window(rel_t *r, int seqno);

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */

rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
    assert((c != NULL && ss == NULL) || (c == NULL && ss != NULL));
    assert(cc);

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

    /* Set the sockaddr_storage for this connection */

    if (ss) memcpy(&r->ss,ss,sizeof(struct sockaddr_storage));

    /* Save the configurations we'll need */

    r->timeout = cc->timeout;
    r->window = cc->window;
    r->single_connection = cc->single_connection;

    /* Create a buffer queue for sending and receiving, starting at
    * index 1 */

    r->send_bq = bq_new(SEND_BUFFER_SIZE, sizeof(send_bq_element_t));
    bq_increase_head_seq_to(r->send_bq,1);
    r->rec_bq = bq_new(cc->window, sizeof(packet_t));
    bq_increase_head_seq_to(r->rec_bq,1);

    /* Send an receive state */

    r->seqno = 1;
    r->ackno = 1;

    /* Connection teardown state */

    r->read_eof = 0;
    r->printed_eof = 0;

    /* Nagle state */

    r->nagle_outstanding = 0;

    return r;
}

/* Destroys a rel_t, freeing associated queues, and managing the linked
 * list.
 */

void
rel_destroy (rel_t *r)
{
    assert(r);

    if (r->next)
        r->next->prev = r->prev;
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free the buffer queues */

    bq_destroy(r->send_bq);
    bq_destroy(r->rec_bq);
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
    assert(cc);
    assert(ss);
    assert(pkt);
    assert(len >= 0);

    rel_t *r;
    for (r = rel_list; r != NULL; r = r->next) {
        if (addreq(ss, &r->ss)) {
            rel_recvpkt(r, pkt, len);
            return;
        }
    }
    
    /* Before we create a new rel_t, we need to check
     * that this packet has a seqno == 1, otherwise
     * we're starting a flow part way in, and that's 
     * against the rules. */

    if (ntohl(pkt->seqno) != 1) {
        return;
    }

    /* If we reach here, then we need a new rel_t
     * for this connection, so we add it at the
     * head of the linked list of rel_t objects. */

    rel_t *new_r = rel_create (NULL, ss, cc);
    rel_recvpkt(new_r, pkt, len);
}

/* Called on the receipt of each packet, after its been mapped to
 * a rel_t.
 */

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    assert(r);
    assert(pkt);
    assert(n >= 0);

    if (!rel_packet_valid(pkt,n)) return;

    /* Do all the endinannness in one place */

    pkt->len = ntohs(pkt->len);
    pkt->seqno = ntohl(pkt->seqno);
    pkt->ackno = ntohl(pkt->ackno);

    /* Read ack nums on all packets, regardless of data or
     * ack. */

    if (rel_recv_ack (r, pkt->ackno)) {

        /* A return of 1 means that that ack was enough for
         * us to close the connection, so our rel_t has been
         * destroyed. Time to quit. */

        return;
    }

    /* Insert all data packets into the read buffer for
     * when we get some space for output. */

    if (n > 8) {
        bq_insert_at(r->rec_bq, pkt->seqno, pkt);

        /* Print try to print the output. If this returns
         * 0, it means that no new ack could be sent, so
         * we should send a duplicate ack, just in case the
         * last one was lost (even though it serves no congestion
         * control purpose in this lab). */

        if (!rel_output(r)) {
            rel_send_ack(r, r->ackno);
        }
    }
}

/* Called whenever there is new content to read from the buffer. Reads
 * input into packets. Sends any packets that are within the send 
 * window, buffers the rest to be sent later.
 */

void
rel_read (rel_t *r)
{
    assert(r);

    /* If we've read an EOF, no reason to even get started */

    if (r->read_eof) return;

    send_bq_element_t elem;

    while (1) {

        /* Check for overrunning send buffer memory */

        assert(r->seqno <= bq_get_tail_seq(r->send_bq));

        /* Read up to 500 bytes into a packet, overwriting the old
         * contents of elem */

        int len = rel_read_input_into_packet(r, &elem);
        if (len == -1) return; /* no more data to read */

        /* If we read an EOF, then we should check if we should
         * close the connection. */

        if (len == 0) {
            r->read_eof = 1;
            if (rel_check_finished(r)) return;
        }

        /* If this packet sequence number is within the window,
         * then send it */

        if (rel_seqno_in_send_window(r,r->seqno)) {
            rel_send_buffered_pkt(r,&elem);
        }

        /* Record the packet in the queue, so that we can (re)send it in the 
         * future. */

        bq_insert_at(r->send_bq, r->seqno, &elem);

        /* Assert that this is the highest seqno element we've inserted */
        
        assert(!bq_element_buffered(r->send_bq, r->seqno + 1));

        r->seqno ++;

    }
}

/* Called whenever there is free buffer space to write output. Handles
 * ack'ing packets after they are written to the terminal, or writing
 * parts of packets when there isn't enough buffer space to fit the whole
 * thing on the terminal. Returns 1 if at least one new ack was sent, 0
 * otherwise.
 */

int
rel_output (rel_t *r)
{
    assert(r);
    
    int sent_ack = 0;

    /* If we've already printed an EOF, then we're done. */

    if (r->printed_eof) return 0;

    while (1) {

        /* Read from the head of the received packets buffer queue,
         * if we have any received packets waiting */

        int rec_seqno = bq_get_head_seq(r->rec_bq);
        if (!bq_element_buffered(r->rec_bq, rec_seqno)) return 0;
        packet_t *pkt = bq_get_element(r->rec_bq, rec_seqno);

        int bufspace = conn_bufspace(r->c);

        /* Print the whole packet, then ack */

        if (bufspace > pkt->len-12) {
            conn_output(r->c, pkt->data, pkt->len-12);
            bq_increase_head_seq_to(r->rec_bq, rec_seqno + 1);

            sent_ack = pkt->seqno + 1;

            /* If we just printed out an EOF, update our status */
        
            if (pkt->len == 12) {
                r->printed_eof = 1; 

                /* If we just destroyed rel_t, we don't want to produce 
                 * a redundant ack, so we return sent_ack=true. */

                if (rel_check_finished(r)) return 1;
            }
        }

        /* Edge case: only enough buffer to print part of the packet */

        else if (bufspace > 0) {
            conn_output(r->c, pkt->data, bufspace);

            /* Shift the packet data over, removing what we've already printed */

            memcpy(&(pkt->data[0]), &(pkt->data[bufspace]), pkt->len - bufspace);
            pkt->len -= bufspace;
            return 0;
        }

        /* If we have no buffer space left, time to quit */

        else if (bufspace == 0) return 0;
    }

    rel_send_ack(r, send_ack);

    return sent_ack;
}

/* Called periodically. Checks every outstanding packet that hasn't yet been ack'd,
 * and if the timeout period expired, then it re-sends the packet and updates the
 * meta-data about last time sent.
 */

void
rel_timer ()
{
    /* Iterate over all the reliable connections */

    rel_t *r;
    for (r = rel_list; r != NULL; r = r->next) {
    
        /* Send window is [head of buffer queue, head of buffer queue + window size],
         * so we iterate over the send window, and send anything that's timed out. */

        int i = 0;
        for (i = bq_get_head_seq(r->send_bq); rel_seqno_in_send_window(r,i); i++) {

            /* This is just a safety check, in case we haven't read in this part
             * of the send window yet */

            if (!bq_element_buffered(r->send_bq, i)) continue;

            /* Borrowed need_timer_in from rlib: just checks whether it's
             * been r->timeout ms since elem->time_sent */

            send_bq_element_t *elem = bq_get_element(r->send_bq, i);
            if (need_timer_in (&(elem->time_sent), r->timeout) == 0) {
                rel_send_buffered_pkt(r,elem);
            }
        }
    }
}

/***********************************
 * Helper function implementations *
 ***********************************/

/* This function gets called on every packet receipt, to handle the
 * ackno in the packet. It moves the buffer queue's head index (see
 * bq.h for more details) to free up the space used by packets that
 * have been ack'd. It also sends any buffered packets that are newly
 * within the window, and haven't yet been sent.
 *
 * Returns 1 if that ack resulted in closing the connection, 0 if not.
 */

int
rel_recv_ack (rel_t *r, int ackno)
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

    assert(!bq_element_buffered(r->send_bq,r->seqno));

    /* Check if this is an ack for a Nagle packet */

    rel_ack_check_nagle(r, ackno);

    /* Check if that was the last of outstanding packet acks, and
     * we can shutdown the connection. */

    if (rel_check_finished(r)) {

        /* If it was, rel_t has already been destroyed, and any further 
         * action could crash the program */

        return 1;
    }

    /* Send any buffered packets that are newly within the window */

    int i;
    for (i = ackno; i < ackno + r->window; i++) {

        /* If we reach a point we haven't buffered in, we're done. */

        if (!bq_element_buffered(r->send_bq, i)) return 0;

        /* Otherwise send out the packet, if noone has sent it yet. */

        send_bq_element_t *elem = bq_get_element(r->send_bq, i);
        if (!elem->sent) {
            rel_send_buffered_pkt(r, elem);
        }
    }

    return 0;
}

/* Sends a buffered packet, and handles updating the meta data
 * associated with the packet. Will also put in the latest ackno
 * as a piggyback for the packet, and recalculate the cksum.
 */

int 
rel_send_buffered_pkt(rel_t *r, send_bq_element_t* elem) 
{
    assert(r);
    assert(elem);
    assert(ntohl(elem->pkt.seqno) < bq_get_head_seq(r->send_bq) + r->window);
    assert(ntohl(elem->pkt.seqno) > 0);

    /* If this is a small packet, check Nagle conditions */

    if (rel_nagle_constrain_sending_buffered_pkt(r, elem)) return 0;

    /* Update records associated with the packet */

    elem->sent = 1;
    clock_gettime (CLOCK_MONOTONIC, &elem->time_sent);

    /* Update to the current ack number */

    elem->pkt.ackno = htonl(r->ackno);

    /* Recalculate the checksum, cause we changed the ackno */

    elem->pkt.cksum = 0;
    elem->pkt.cksum = cksum(&elem->pkt, ntohs(elem->pkt.len));

    /* Do the dirty deed */

    conn_sendpkt(r->c, &(elem->pkt), ntohs(elem->pkt.len));

    return 1;
}

/* Creates and sends an ack packet. This doesn't effect the
 * Nagle constraint, because it's not a data packet.
 */

void 
rel_send_ack (rel_t *r, int ackno)
{
    assert(r);
    assert(ackno >= r->ackno); /* Acks cannot regress */

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

/* Reads up to 500 bytes of data from conn_input() into the
 * packet passed in, writing everything in network byte order,
 * and sets metadata so that the packet will be sent at the
 * next available opportunity.
 */

int
rel_read_input_into_packet(rel_t *r, send_bq_element_t *elem)
{
    assert(r);
    assert(elem);

    /* Read data directly into our packet */

    int len = conn_input(r->c, &(elem->pkt.data[0]), 500);
    if (len == 0) return -1; /* no more data to read */
    if (len == -1) {
        len = 0; /* send an EOF */
    }

    /* Build packet frame data */

    elem->pkt.ackno = htonl(r->ackno);
    elem->pkt.seqno = htonl(r->seqno);
    elem->pkt.len = htons(12 + len);
    elem->pkt.cksum = 0;
    elem->pkt.cksum = cksum(&elem->pkt, 12 + len);

    /* Time sent is 1970, so when there's free window, it'll be sent */

    elem->time_sent.tv_sec = 0;
    elem->time_sent.tv_nsec = 0;
    elem->sent = 0;

    return len;
}

/* Checks if a rel_t has both received and sent an EOF, and if
 * it has, then it calls rel_destroy on the rel_t.
 *
 * Specific conditions are:
 *
 * - received an EOF from other size
 * - written all data to conn_output
 *   (both of the above are true if written an EOF)
 * - read an EOF from our input (but not necessarily sent it)
 * - gotten acks for all our outstanding packets
 *
 * returns 1 if rel_t was just destroyed, 0 otherwise.
 */

int
rel_check_finished (rel_t *r)
{
    assert(r);

    if (!r->read_eof || !r->printed_eof) return 0;

    /* Send window is [head of buffer queue, head of buffer queue + window size],
     * so we iterate over the send window, and check if there's anything in it that
     * we've sent (cause that would mean we haven't gotten an ack for that yet). */

    int i = 0;
    for (i = bq_get_head_seq(r->send_bq); rel_seqno_in_send_window(r,i); i++) {

        /* If the element isn't buffered, we definately haven't sent it */

        if (!bq_element_buffered(r->send_bq, i)) continue;

        /* Otherwise, we check the sent flag */

        send_bq_element_t *elem = bq_get_element(r->send_bq, i);
        if (elem->sent) return 0;
    }

    /* If we reach here, then we've received all acks for packets we sent, and
     * both other conditions are met. Destroy this rel_t. */

    rel_destroy(r);

    return 1;
}

/* This function gets called every ack to update the Nagle 
 * constraint, in case the ack means that the last undersized 
 * data packet has left the network, which would mean that 
 * it's safe to send more data.
 */

void
rel_ack_check_nagle (rel_t *r, int ackno) 
{
    assert(r);
    assert(ackno > 0);

    if (ackno > r->nagle_outstanding) {
        r->nagle_outstanding = 0;
    }
}

/* This function is called on every packet send. If the packet 
 * is undersize, and there is another small packets in the 
 * network, it returns 1 to indicate that this packet should 
 * not be sent at this time. Otherwise, it returns 0 to indicate 
 * that it's ok to send this packet. Handles noting outstanding 
 * small packets internally. rel_ack_nagle will release the
 * constraint, if it receives an ack >= to the seqno of the
 * outstanding Nagle packet.
 */

int
rel_nagle_constrain_sending_buffered_pkt(rel_t *r, send_bq_element_t* elem)
{
    assert(r);
    assert(elem);

    if (ntohs(elem->pkt.len) < 512) {

        /* If there's another small packet unacknowledged, don't send this one. */

        if (r->nagle_outstanding != 0 && r->nagle_outstanding != ntohl(elem->pkt.seqno)) {
            return 1;
        }

        /* Otherwise record that this our small packet oustanding, but still send it */

        else {
            r->nagle_outstanding = ntohl(elem->pkt.seqno);
            return 0;
        }
    }

    /* If it's a full size packet, no restrictions apply */

    return 0;
}

/* Checks whether a packet has been corrupted, either by cksum or 
 * because the length is shorter than advertised. Returns 1 if packet 
 * is ok, and 0 otherwise.
 */

int 
rel_packet_valid (packet_t *pkt, size_t n)
{
    assert(pkt);
    assert(n >= 0);

    /* Reject for received length shorter than pkt claims */

    if (ntohs(pkt->len) > n) return 0;

    /* Reject for incorrect cksum */

    int cksum_buf = pkt->cksum;
    pkt->cksum = 0;
    if (cksum_buf != cksum(pkt, n)) return 0;

    /* Otherwise accept */

    return 1;
}

/* Returns whether or not a seqno is within the current send window
 */

int
rel_seqno_in_send_window(rel_t *r, int seqno) 
{
    assert(r);
    assert(seqno >= 0);

    int head_seq = bq_get_head_seq(r->send_bq);
    return (seqno >= head_seq) && (seqno < head_seq + r->window);
}
