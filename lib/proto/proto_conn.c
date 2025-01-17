#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include "events.h"
#include "network.h"
#include "sock.h"

#include "proto_crypt.h"
#include "proto_handshake.h"
#include "proto_pipe.h"

#include "proto_conn.h"

struct conn_state {
	int (* callback_dead)(void *, int);
	void * cookie;
	struct sock_addr ** sas;
	int decr;
	int nopfs;
	int requirepfs;
	int nokeepalive;
	const struct proto_secret * K;
	double timeo;
	int s;
	int t;
	void * connect_cookie;
	void * connect_timeout_cookie;
	void * handshake_cookie;
	void * handshake_timeout_cookie;
	struct proto_keys * k_f;
	struct proto_keys * k_r;
	void * pipe_f;
	void * pipe_r;
	int stat_f;
	int stat_r;
};

static int callback_connect_done(void *, int);
static int callback_connect_timeout(void *);
static int callback_handshake_done(void *, struct proto_keys *,
    struct proto_keys *);
static int callback_handshake_timeout(void *);
static int callback_pipestatus(void *);

/* Start a handshake. */
static int
starthandshake(struct conn_state * C, int s, int decr)
{

	/* Start the handshake timer. */
	if ((C->handshake_timeout_cookie = events_timer_register_double(
	    callback_handshake_timeout, C, C->timeo)) == NULL)
		goto err0;

	/* Start the handshake. */
	if ((C->handshake_cookie = proto_handshake(s, decr, C->nopfs,
	    C->requirepfs, C->K, callback_handshake_done, C)) == NULL)
		goto err1;

	/* Success! */
	return (0);

err1:
	events_timer_cancel(C->handshake_timeout_cookie);
	C->handshake_timeout_cookie = NULL;
err0:
	/* Failure! */
	return (-1);
}

/* Launch the two pipes. */
static int
launchpipes(struct conn_state * C)
{
	int on = C->nokeepalive ? 0 : 1;
	int one = 1;

	/*
	 * Attempt to turn keepalives on or off as requested.  We ignore
	 * failures here since the sockets might not be of a type for which
	 * SO_KEEPALIVE is valid -- it is a socket level option, but protocol
	 * specific.  In particular, it has no sensible meaning for UNIX
	 * sockets.
	 */
	(void)setsockopt(C->s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	(void)setsockopt(C->t, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

	/**
	 * Attempt to turn off nagling on both sockets.  If the TCP stack has
	 * enough window space that it is always able to send packets, then on
	 * the encrypted end this will result in every 1060-byte spiped packet
	 * getting its own TCP segment, including 40 bytes of TCP/IP headers;
	 * this is fine.  On the unencrypted end, we might send a single byte
	 * of data with 40 bytes of TCP/IP headers; this is not so good.
	 *
	 * However, a write over the unencrypted connection will only happen
	 * after an spiped packet has been read from the encrypted connection,
	 * so the worst case is 80 bytes of TCP/IP headers per 1061 bytes of
	 * TCP/IP payload (this may still be only a single byte of spiped
	 * payload, but that is not relevant to the question of overhead from
	 * small TCP/IP segments); and while the two sockets might not be on
	 * the same network, if they are on different networks it is almost
	 * guaranteed that the network over which the encrypted connection is
	 * passing would be a wider-area network which is both less secure and
	 * more expensive.  Consequently, the maximum TCP/IP overhead ratio of
	 * 80/1061 is almost certain to hold even with weighted byte costs.
	 *
	 * We ignore errors since (as with keep-alives) we may be dealing with
	 * a non-TCP socket; and also because while POSIX requires TCP_NODELAY
	 * to be defined, it is not required to be implemented as a socket
	 * option.
	 */
	(void)setsockopt(C->s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	(void)setsockopt(C->t, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	/* Create two pipes. */
	if ((C->pipe_f = proto_pipe(C->s, C->t, C->decr, C->k_f,
	    &C->stat_f, callback_pipestatus, C)) == NULL)
		goto err0;
	if ((C->pipe_r = proto_pipe(C->t, C->s, !C->decr, C->k_r,
	    &C->stat_r, callback_pipestatus, C)) == NULL)
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/**
 * proto_conn_drop(conn_cookie, reason):
 * Drop connection and free memory associated with ${conn_cookie}, due to
 * ${reason}.  Return success or failure.
 */
int
proto_conn_drop(void * conn_cookie, int reason)
{
	struct conn_state * C = conn_cookie;
	int rc;

	/* Close the incoming connection. */
	close(C->s);

	/* Close the outgoing connection if it is open. */
	if (C->t != -1)
		close(C->t);

	/* Stop connecting if a connection is in progress. */
	if (C->connect_cookie != NULL)
		network_connect_cancel(C->connect_cookie);

	/* Free the target addresses if we haven't already done so. */
	sock_addr_freelist(C->sas);

	/* Stop handshaking if a handshake is in progress. */
	if (C->handshake_cookie != NULL)
		proto_handshake_cancel(C->handshake_cookie);

	/* Kill timeouts if they are pending. */
	if (C->connect_timeout_cookie != NULL)
		events_timer_cancel(C->connect_timeout_cookie);
	if (C->handshake_timeout_cookie != NULL)
		events_timer_cancel(C->handshake_timeout_cookie);

	/* Free protocol keys. */
	proto_crypt_free(C->k_f);
	proto_crypt_free(C->k_r);

	/* Shut down pipes. */
	if (C->pipe_f != NULL)
		proto_pipe_cancel(C->pipe_f);
	if (C->pipe_r != NULL)
		proto_pipe_cancel(C->pipe_r);

	/* Notify the upstream that we've dropped a connection. */
	rc = (C->callback_dead)(C->cookie, reason);

	/* Free the connection cookie. */
	free(C);

	/* Return success/fail status. */
	return (rc);
}

/**
 * proto_conn_create(s, sas, sa_b, decr, nopfs, requirepfs, nokeepalive, K,
 *     timeo, callback_dead, cookie):
 * Create a connection with one end at ${s} and the other end connecting to
 * the target addresses ${sas}.  Bind outgoing address to ${sa_b} if it is
 * not NULL.  If ${decr} is 0, encrypt the outgoing data; if ${decr} is
 * nonzero, decrypt the incoming data.  If ${nopfs} is non-zero, don't use
 * perfect forward secrecy.  If ${requirepfs} is non-zero, drop the connection
 * if the other end tries to disable perfect forward secrecy.  Enable
 * transport layer keep-alives (if applicable) on both sockets if and only if
 * ${nokeepalive} is zero.  Drop the connection if the handshake or
 * connecting to the target takes more than ${timeo} seconds.  When the
 * connection is dropped, invoke ${callback_dead}(${cookie}).  Free ${sas}
 * once it is no longer needed.  Return a cookie which can be passed to
 * proto_conn_drop().  If there is a connection error after this
 * function returns, close ${s}.
 */
void *
proto_conn_create(int s, struct sock_addr ** sas, const struct sock_addr * sa_b,
    int decr, int nopfs, int requirepfs, int nokeepalive,
    const struct proto_secret * K, double timeo,
    int (* callback_dead)(void *, int), void * cookie)
{
	struct conn_state * C;

	/* Bake a cookie for this connection. */
	if ((C = malloc(sizeof(struct conn_state))) == NULL)
		goto err0;
	C->callback_dead = callback_dead;
	C->cookie = cookie;
	C->sas = sas;
	C->decr = decr;
	C->nopfs = nopfs;
	C->requirepfs = requirepfs;
	C->nokeepalive = nokeepalive;
	C->K = K;
	C->timeo = timeo;
	C->s = s;
	C->t = -1;
	C->connect_cookie = NULL;
	C->connect_timeout_cookie = NULL;
	C->handshake_cookie = NULL;
	C->handshake_timeout_cookie = NULL;
	C->k_f = C->k_r = NULL;
	C->pipe_f = C->pipe_r = NULL;
	C->stat_f = C->stat_r = 1;

	/* Start the connect timer. */
	if ((C->connect_timeout_cookie = events_timer_register_double(
	    callback_connect_timeout, C, C->timeo)) == NULL)
		goto err1;

	/* Connect to target. */
	if ((C->connect_cookie =
	    network_connect_bind(C->sas, sa_b, callback_connect_done, C)) == NULL)
		goto err2;

	/* If we're decrypting, start the handshake. */
	if (C->decr) {
		if (starthandshake(C, C->s, C->decr))
			goto err3;
	}

	/* Success! */
	return (C);

err3:
	network_connect_cancel(C->connect_cookie);
err2:
	events_timer_cancel(C->connect_timeout_cookie);
err1:
	free(C);
err0:
	/* Failure! */
	return (NULL);
}

/* We have connected to the target. */
static int
callback_connect_done(void * cookie, int t)
{
	struct conn_state * C = cookie;

	/* This connection attempt is no longer pending. */
	C->connect_cookie = NULL;

	/* Don't need the target address any more. */
	sock_addr_freelist(C->sas);
	C->sas = NULL;

	/* We beat the clock. */
	events_timer_cancel(C->connect_timeout_cookie);
	C->connect_timeout_cookie = NULL;

	/* Did we manage to connect? */
	if ((C->t = t) == -1)
		return (proto_conn_drop(C, PROTO_CONN_CONNECT_FAILED));

	/* If we're encrypting, start the handshake. */
	if (!C->decr) {
		if (starthandshake(C, C->t, C->decr))
			goto err1;
	}

	/* If the handshake already finished, start shuttling data. */
	if ((C->t != -1) && (C->k_f != NULL) && (C->k_r != NULL)) {
		if (launchpipes(C))
			goto err1;
	}

	/* Success! */
	return (0);

err1:
	proto_conn_drop(C, PROTO_CONN_ERROR);

	/* Failure! */
	return (-1);
}

/* Connecting to the target took too long. */
static int
callback_connect_timeout(void * cookie)
{
	struct conn_state * C = cookie;

	/* This timeout is no longer pending. */
	C->connect_timeout_cookie = NULL;

	/*
	 * We could free C->sas here, but from a semantic point of view it
	 * could still be in use by the not-yet-cancelled connect operation.
	 * Instead, we free it in proto_conn_drop, after cancelling the
	 * connect.
	 */

	/* Drop the connection. */
	return (proto_conn_drop(C, PROTO_CONN_ERROR));
}

/* We have performed the protocol handshake. */
static int
callback_handshake_done(void * cookie, struct proto_keys * f,
    struct proto_keys * r)
{
	struct conn_state * C = cookie;

	/* The handshake is no longer in progress. */
	C->handshake_cookie = NULL;

	/* We beat the clock. */
	events_timer_cancel(C->handshake_timeout_cookie);
	C->handshake_timeout_cookie = NULL;

	/* If the protocol handshake failed, drop the connection. */
	if ((f == NULL) && (r == NULL))
		return (proto_conn_drop(C, PROTO_CONN_HANDSHAKE_FAILED));

	/* We should have two keys. */
	assert(f != NULL);
	assert(r != NULL);

	/* Record the keys so we can free them later. */
	C->k_f = f;
	C->k_r = r;

	/* If we already connected to the target, start shuttling data. */
	if ((C->t != -1) && (C->k_f != NULL) && (C->k_r != NULL)) {
		if (launchpipes(C))
			goto err1;
	}

	/* Success! */
	return (0);

err1:
	proto_conn_drop(C, PROTO_CONN_ERROR);

	/* Failure! */
	return (-1);
}

/* The protocol handshake took too long. */
static int
callback_handshake_timeout(void * cookie)
{
	struct conn_state * C = cookie;

	/* This timeout is no longer pending. */
	C->handshake_timeout_cookie = NULL;

	/* Drop the connection. */
	return (proto_conn_drop(C, PROTO_CONN_ERROR));
}

/* The status of one of the directions has changed. */
static int
callback_pipestatus(void * cookie)
{
	struct conn_state * C = cookie;

	/* If we have an error in either direction, kill the connection. */
	if ((C->stat_f == -1) || (C->stat_r == -1))
		return (proto_conn_drop(C, PROTO_CONN_ERROR));

	/* If both directions have been shut down, kill the connection. */
	if ((C->stat_f == 0) && (C->stat_r == 0))
		return (proto_conn_drop(C, PROTO_CONN_CLOSED));

	/* Nothing to do. */
	return (0);
}
