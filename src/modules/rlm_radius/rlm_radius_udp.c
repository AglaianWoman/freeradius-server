// @todo - finish it!
// * implement remove(), which removes packets from the tracking tree
// * don't make request_io_ctx talloc'd from rlm_radius_link_t, as the link can be used
// * for other connections.  it's simpler to just have one remove() func, than to muck with
//   more allocations and talloc destructors.
// * add fd_active / fd_idle callbacks.  They will suppress the 'idle' call in rlm_radius
// - i.e. if UDP wants to send a Status-Server, it can't be idle...
// figure out a way to tell rlm_radius that the connection is zombie / alive?

/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_radius_udp.c
 * @brief RADIUS UDP transport
 *
 * @copyright 2017  Network RADIUS SARL
 */
RCSID("$Id$")

#include <freeradius-devel/io/application.h>
#include <freeradius-devel/udp.h>
#include <freeradius-devel/connection.h>
#include <freeradius-devel/rad_assert.h>

#include "rlm_radius.h"
#include "track.h"

/** Static configuration for the module.
 *
 */
typedef struct rlm_radius_udp_t {
	rlm_radius_t		*parent;		//!< rlm_radius instance

	fr_ipaddr_t		dst_ipaddr;		//!< IP of the home server
	fr_ipaddr_t		src_ipaddr;		//!< IP we open our socket on
	uint16_t		dst_port;		//!< port of the home server
	char const		*secret;		//!< shared secret

	char const		*interface;		//!< Interface to bind to.

	uint32_t		recv_buff;		//!< How big the kernel's receive buffer should be.
	uint32_t		send_buff;		//!< How big the kernel's send buffer should be.

	uint32_t		max_packet_size;	//!< maximum packet size

	bool			recv_buff_is_set;	//!< Whether we were provided with a recv_buf
	bool			send_buff_is_set;	//!< Whether we were provided with a send_buf
} rlm_radius_udp_t;


/** Per-thread configuration for the module.
 *
 *  This data structure holds the connections, etc. for this IO submodule.
 */
typedef struct rlm_radius_udp_thread_t {
	rlm_radius_udp_t	*inst;			//!< IO submodule instance
	fr_event_list_t		*el;			//!< event list

	bool			pending;		//!< are there pending requests?
	fr_dlist_t		queued;			//!< queued requests for some new connection

	fr_dlist_t		active;	       	//!< active connections
	fr_dlist_t		frozen;      	//!< frozen connections
	fr_dlist_t		opening;      	//!< opening connections
} rlm_radius_udp_thread_t;

typedef struct rlm_radius_udp_connection_t {
	rlm_radius_udp_t const	*inst;		//!< our module instance
	rlm_radius_udp_thread_t *thread;       	//!< our thread-specific data
	fr_connection_t		*conn;		//!< Connection to our destination.
	char const		*name;		//!< from IP PORT to IP PORT

	fr_dlist_t		entry;		//!< in the linked list of connections

	struct timeval		last_sent_with_reply;	//!< most recent sent time which had a reply

	bool			pending;	//!< are there packets pending?
	fr_dlist_t		queued;		//!< list of packets queued for sending
	fr_dlist_t		sent;		//!< list of sent packets

	uint32_t		max_packet_size; //!< our max packet size. may be different from the parent...
	int			fd;		//!< file descriptor

	fr_ipaddr_t		dst_ipaddr;	//!< IP of the home server. stupid 'const' issues..
	uint16_t		dst_port;	//!< port of the home server
	fr_ipaddr_t		src_ipaddr;	//!< my source IP
	uint16_t	       	src_port;	//!< my source port

	// @todo - track status-server, open, signaling, etc.

	uint8_t			*buffer;	//!< receive buffer
	size_t			buflen;		//!< receive buffer length

	rlm_radius_id_t		*id[FR_MAX_PACKET_CODE]; //!< ID tracking
} rlm_radius_udp_connection_t;


/** Link a packet to a connection
 *
 */
typedef struct rlm_radius_udp_request_t {
	fr_dlist_t		entry;		//!< in the connection list of packets

	int			code;		//!< packet code
	rlm_radius_udp_connection_t	*c;     //!< the connection
	rlm_radius_link_t	*link;		//!< more link stuff
	rlm_radius_request_t	*rr;		//!< the ID tracking, resend count, etc.

} rlm_radius_udp_request_t;


static const CONF_PARSER module_config[] = {
	{ FR_CONF_OFFSET("ipaddr", FR_TYPE_COMBO_IP_ADDR, rlm_radius_udp_t, dst_ipaddr), },
	{ FR_CONF_OFFSET("ipv4addr", FR_TYPE_IPV4_ADDR, rlm_radius_udp_t, dst_ipaddr) },
	{ FR_CONF_OFFSET("ipv6addr", FR_TYPE_IPV6_ADDR, rlm_radius_udp_t, dst_ipaddr) },

	{ FR_CONF_OFFSET("port", FR_TYPE_UINT16, rlm_radius_udp_t, dst_port) },

	{ FR_CONF_OFFSET("secret", FR_TYPE_STRING | FR_TYPE_REQUIRED, rlm_radius_udp_t, secret) },

	{ FR_CONF_OFFSET("interface", FR_TYPE_STRING, rlm_radius_udp_t, interface) },

	{ FR_CONF_IS_SET_OFFSET("recv_buff", FR_TYPE_UINT32, rlm_radius_udp_t, recv_buff) },
	{ FR_CONF_IS_SET_OFFSET("send_buff", FR_TYPE_UINT32, rlm_radius_udp_t, send_buff) },

	{ FR_CONF_OFFSET("max_packet_size", FR_TYPE_UINT32, rlm_radius_udp_t, max_packet_size),
	  .dflt = "4096" },

	{ FR_CONF_OFFSET("src_ipaddr", FR_TYPE_COMBO_IP_ADDR, rlm_radius_udp_t, src_ipaddr) },
	{ FR_CONF_OFFSET("src_ipv4addr", FR_TYPE_IPV4_ADDR, rlm_radius_udp_t, src_ipaddr) },
	{ FR_CONF_OFFSET("src_ipv6addr", FR_TYPE_IPV6_ADDR, rlm_radius_udp_t, src_ipaddr) },

	CONF_PARSER_TERMINATOR
};

static void conn_error(UNUSED fr_event_list_t *el, UNUSED int fd, UNUSED int flags, int fd_errno, void *uctx);
static void conn_read(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx);
static void conn_writable(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx);


/** Set the socket to idle
 *
 *  But keep the read event open, just in case the other end sends us
 *  data  That way we can process it.
 *
 * @param[in] c		Connection data structure
 */
static void fd_idle(rlm_radius_udp_connection_t *c)
{
	rlm_radius_udp_thread_t	*t = c->thread;

	DEBUG3("Marking socket %s as idle", c->name);
	if (fr_event_fd_insert(c->conn, t->el, c->fd,
			       conn_read, NULL, conn_error, c) < 0) {
		PERROR("Failed inserting FD event");
		talloc_free(c);
	}
}

/** Set the socket to active
 *
 * We have messages we want to send, so need to know when the socket is writable.
 *
 * @param[in] c		Connection data structure
 */
static void fd_active(rlm_radius_udp_connection_t *c)
{
	rlm_radius_udp_thread_t	*t = c->thread;

	DEBUG3("Marking socket %s as active - Draining requests", c->name);

	if (fr_event_fd_insert(c->conn, t->el, c->fd,
			       conn_read, conn_writable, conn_error, c) < 0) {
		PERROR("Failed inserting FD event");
		talloc_free(c);
	}
}


/** Connection errored
 *
 */
static void conn_error(UNUSED fr_event_list_t *el, UNUSED int fd, UNUSED int flags, int fd_errno, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);

	ERROR("Connection failed %s: %s", c->name, fr_syserror(fd_errno));

	/*
	 *	Something bad happened... Fix it...
	 */
	fr_connection_reconnect(c->conn);
}


/** Read reply packets.
 *
 */
static void conn_read(fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);
	decode_fail_t reason;
	size_t packet_len;
	ssize_t data_len;

	data_len = read(fd, c->buffer, c->buflen);
	if (data_len == 0) return;

	if (data_len < 0) {
		conn_error(el, fd, 0, errno, c);
		return;
	}

	packet_len = data_len;
	if (!fr_radius_ok(c->buffer, &packet_len, false, &reason)) {
		DEBUG("Ignoring malformed packet");
		return;
	}

	// look the packet up by ID, and find the matching one
	// allowing multiple packet codes in the same socket is annoying,
	// because each request code can have one of multiple reply codes..
	// and they all share the same ID space. :(

	// verify it
	// get the REQUEST for it

	// remove it from c->sent
	// update the status of this connection
	// unlang_resumable(request);
}

/** There's space available to write data, so do that...
 *
 */
static void conn_writable(fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);
	fr_dlist_t *entry, *next;

	/*
	 *	Clear our backlog
	 */
	for (entry = FR_DLIST_FIRST(c->queued);
	     entry != NULL;
	     entry = next) {
		rlm_radius_udp_request_t *u;
		ssize_t packet_len;
		ssize_t rcode;

		next = FR_DLIST_NEXT(c->queued, entry);

		u = fr_ptr_to_type(rlm_radius_udp_request_t, entry, entry);

		packet_len = fr_radius_encode(c->buffer, c->buflen, NULL,
					      c->inst->secret, u->rr->id, u->code, 0,
					      u->link->request->packet->vps);
		if (packet_len <= 0) break;

		rcode = write(fd, c->buffer, packet_len);
		if (rcode < 0) {
			if (errno == EWOULDBLOCK) return;

			conn_error(el, fd, 0, errno, c);
			return;
		}

		fr_dlist_remove(&u->entry);
		fr_dlist_insert_tail(&c->sent, &u->entry);
	}

	/*
	 *	Check if we have to enable or disable writing on the socket.
	 */
	entry = FR_DLIST_FIRST(c->queued);
	if (!entry) {
		c->pending = false;
		fd_idle(c);

	} else if (!c->pending) {
		c->pending = true;
		fd_active(c);
	}
}

/** Shutdown/close a file descriptor
 *
 */
static void conn_close(int fd, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);

	DEBUG3("Closing socket %s", c->name);
	if (shutdown(fd, SHUT_RDWR) < 0) DEBUG3("Shutdown on socket %s failed: %s", c->name, fr_syserror(errno));
	if (close(fd) < 0) DEBUG3("Closing socket %s failed: %s", c->name, fr_syserror(errno));

	c->fd = -1;
}

/** Process notification that fd is open
 *
 */
static fr_connection_state_t conn_open(UNUSED fr_event_list_t *el, UNUSED int fd, void *uctx)
{
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);
	rlm_radius_udp_thread_t *t = c->thread;
	fr_dlist_t *entry, *next;

	talloc_const_free(&c->name);
	c->name = talloc_strdup(c, "connected");

	/*
	 *	Remove the connection from the "opening" list, and add
	 *	it to the "active" list.
	 */
	fr_dlist_remove(&c->entry);
	fr_dlist_insert_tail(&t->active, &c->entry);

	/*
	 *	Clear the global backlog first.
	 */
	for (entry = FR_DLIST_FIRST(t->queued);
	     entry != NULL;
	     entry = next) {
		rlm_radius_udp_request_t *u;

		next = FR_DLIST_NEXT(t->queued, entry);

		u = fr_ptr_to_type(rlm_radius_udp_request_t, entry, entry);

		// @todo - figure out if we can the request to the
		// connection, by tracking used codes, etc.

		fr_dlist_remove(entry);
		fr_dlist_insert_tail(&c->queued, &u->entry);
		c->pending = true;
	}


	/*
	 *	If we have data pending, add the writable event immediately
	 */
	if (c->pending) {
		fd_active(c);
	} else {
		fd_idle(c);
	}

	return FR_CONNECTION_STATE_CONNECTED;
}


/** Initialise a new outbound connection
 *
 * @param[out] fd_out	Where to write the new file descriptor.
 * @param[in] uctx	A #rlm_radius_thread_t.
 */
static fr_connection_state_t conn_init(int *fd_out, void *uctx)
{
	int fd;
	rlm_radius_udp_connection_t *c = talloc_get_type_abort(uctx, rlm_radius_udp_connection_t);

	/*
	 *	Open the outgoing socket.
	 *
	 *	@todo - pass src_ipaddr, and remove later call to fr_socket_bind()
	 *	which does return the src_port, but doesn't set the "don't fragment" bit.
	 */
	fd = fr_socket_client_udp(&c->src_ipaddr, &c->dst_ipaddr, c->dst_port, true);
	if (fd < 0) {
		DEBUG("Failed opening RADIUS client UDP socket: %s", fr_strerror());
		return FR_CONNECTION_STATE_FAILED;
	}

#if 0
	if (fr_socket_bind(fd, &io->src_ipaddr, &io->src_port, inst->interface) < 0) {
		DEBUG("Failed binding RADIUS client UDP socket: %s FD %d %pV port %u interface %s", fr_strerror(), fd, fr_box_ipaddr(io->src_ipaddr),
			io->src_port, inst->interface);
		return FR_CONNECTION_STATE_FAILED;
	}
#endif

	// @todo - set name properly
	c->name = talloc_strdup(c, "connecting...");

	// @todo - set recv_buff and send_buff socket options

	c->fd = fd;

	// @todo - initialize the tracking memory, etc.

	*fd_out = fd;

	return FR_CONNECTION_STATE_CONNECTING;
}

static void mod_connect(rlm_radius_udp_t *inst, rlm_radius_udp_thread_t *t)
{
	rlm_radius_udp_connection_t *c;

	c = talloc_zero(t, rlm_radius_udp_connection_t);
	c->inst = inst;
	c->thread = t;
	c->dst_ipaddr = inst->dst_ipaddr;
	c->dst_port = inst->dst_port;
	c->src_ipaddr = inst->src_ipaddr;
	c->src_port = 0;
	c->max_packet_size = inst->max_packet_size;

	c->buffer = talloc_array(c, uint8_t, c->max_packet_size);
	if (!c->buffer) {
		talloc_free(c);
		return;
	}
	c->buflen = c->max_packet_size;

	c->conn = fr_connection_alloc(c, t->el, &inst->parent->connection_timeout, &inst->parent->reconnection_delay,
				      conn_init, conn_open, conn_close, inst->parent->name, c);
	if (!c->conn) return;

	fr_connection_start(c->conn);

	fr_dlist_insert_head(&t->opening, &c->entry);

	// @todo - set destructor for connection which removes it from the various lists?
}

static rlm_radius_udp_connection_t *mod_connection_get(rlm_radius_udp_thread_t *t, UNUSED int code)
{
	rlm_radius_udp_connection_t *c;
	fr_dlist_t *entry;

	entry = FR_DLIST_FIRST(t->active);
	if (!entry) return NULL;

	c = fr_ptr_to_type(rlm_radius_udp_connection_t, entry, entry);
	(void) talloc_get_type_abort(c, rlm_radius_udp_connection_t);

	return c;
}

static void mod_connection_add(UNUSED rlm_radius_udp_connection_t *c, UNUSED rlm_radius_udp_request_t *u)
{
	// do stuff
}


static void mod_clear_backlog(rlm_radius_udp_thread_t *t)
{
	fr_dlist_t *entry, *next;

	for (entry = FR_DLIST_FIRST(t->queued);
	     entry != NULL;
	     entry = next) {
		rlm_radius_udp_request_t *u;
		rlm_radius_udp_connection_t *c;

		next = FR_DLIST_NEXT(t->queued, entry);

		u = fr_ptr_to_type(rlm_radius_udp_request_t, entry, entry);
		c = mod_connection_get(t, u->code);
		if (!c) return;

		fr_dlist_remove(entry);
		mod_connection_add(c, u);
	}
}


static int mod_push(void *instance, REQUEST *request, rlm_radius_link_t *link, void *thread)
{
	rlm_radius_udp_t *inst = talloc_get_type_abort(instance, rlm_radius_udp_t);
	rlm_radius_udp_thread_t *t = talloc_get_type_abort(thread, rlm_radius_udp_thread_t);
	rlm_radius_udp_request_t *u = link->request_io_ctx;
	rlm_radius_udp_connection_t *c;

	rad_assert(request->packet->code > 0);
	rad_assert(request->packet->code < FR_MAX_PACKET_CODE);

	/*
	 *	Clear the backlog before sending any new packets.
	 */
	if (t->pending) mod_clear_backlog(t);

	u->link = link;
	u->code = request->packet->code;

	/*
	 *	Get a connection.  If they're all full, try to open a
	 *	new one.
	 */
	c = mod_connection_get(t, u->code);
	if (!c) {
		fr_dlist_t *entry;

		entry = FR_DLIST_FIRST(t->opening);
		if (!entry) mod_connect(inst, t);

		/*
		 *	Add the request to the backlog.  It will be
		 *	sent either when the new connection is open,
		 *	or when an existing connection has
		 *	availability.
		 */
		t->pending = true;
		fr_dlist_insert_head(&t->queued, &u->entry);
		return 0;
	}

	/*
	 *	Insert it into the pending queue
	 */
	fr_dlist_insert_head(&c->queued, &u->entry);

	/*
	 *	If there are no active packets, try to write one
	 *	immediately.  This avoids a few context switches in
	 *	the case where the socket is writable.
	 */
	if (!c->pending) {
		conn_writable(t->el, c->fd, 0, c);
	}

	return 0;
}


/** Bootstrap the module
 *
 * Bootstrap I/O and type submodules.
 *
 * @param[in] instance	Ctx data for this module
 * @param[in] conf    our configuration section parsed to give us instance.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_bootstrap(UNUSED void *instance, UNUSED CONF_SECTION *conf)
{
	rlm_radius_udp_t *inst = talloc_get_type_abort(instance, rlm_radius_udp_t);

	(void) talloc_set_type(inst, rlm_radius_udp_t);

	return 0;
}


/** Instantiate the module
 *
 * Instantiate I/O and type submodules.
 *
 * @param[in] instance	Ctx data for this module
 * @param[in] conf	our configuration section parsed to give us instance.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_instantiate(rlm_radius_t *parent, void *instance, CONF_SECTION *conf)
{
	rlm_radius_udp_t *inst = talloc_get_type_abort(instance, rlm_radius_udp_t);

	inst->parent = parent;

	/*
	 *	Ensure that we have a destination address.
	 */
	if (inst->dst_ipaddr.af == AF_UNSPEC) {
		cf_log_err(conf, "A value must be given for 'ipaddr'");
		return -1;
	}

	/*
	 *	If src_ipaddr isn't set, make sure it's INADDR_ANY, of
	 *	the same address family as dst_ipaddr.
	 */
	if (inst->src_ipaddr.af == AF_UNSPEC) {
		memset(&inst->src_ipaddr, 0, sizeof(inst->src_ipaddr));

		inst->src_ipaddr.af = inst->dst_ipaddr.af;

		if (inst->src_ipaddr.af == AF_INET) {
			inst->src_ipaddr.prefix = 32;
		} else {
			inst->src_ipaddr.prefix = 128;
		}
	}

	else if (inst->src_ipaddr.af != inst->dst_ipaddr.af) {
		cf_log_err(conf, "The 'ipaddr' and 'src_ipaddr' configuration items must be both of the same address family");
		return -1;
	}

	if (!inst->dst_port) {
		cf_log_err(conf, "A value must be given for 'port'");
		return -1;
	}

	if (inst->recv_buff_is_set) {
		FR_INTEGER_BOUND_CHECK("recv_buff", inst->recv_buff, >=, inst->max_packet_size);
		FR_INTEGER_BOUND_CHECK("recv_buff", inst->recv_buff, <=, INT_MAX);
	}

	if (inst->send_buff_is_set) {
		FR_INTEGER_BOUND_CHECK("send_buff", inst->send_buff, >=, inst->max_packet_size);
		FR_INTEGER_BOUND_CHECK("send_buff", inst->send_buff, <=, INT_MAX);
	}

	FR_INTEGER_BOUND_CHECK("max_packet_size", inst->max_packet_size, >=, 64);
	FR_INTEGER_BOUND_CHECK("max_packet_size", inst->max_packet_size, <=, 65535);

	return 0;
}


/** Instantiate thread data for the submodule.
 *
 */
static int mod_thread_instantiate(UNUSED CONF_SECTION const *cs, void *instance, fr_event_list_t *el, void *thread)
{
	rlm_radius_udp_thread_t *t = thread;

	(void) talloc_set_type(t, rlm_radius_udp_thread_t);
	t->inst = instance;
	t->el = el;

	t->pending = false;
	FR_DLIST_INIT(t->queued);
	FR_DLIST_INIT(t->active);
	FR_DLIST_INIT(t->frozen);
	FR_DLIST_INIT(t->opening);

	// @todo - get parent, and initialize the list of IDs by code, from what is permitted by rlm_radius

	mod_connect(t->inst, t);

	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern fr_radius_client_io_t rlm_radius_udp;
fr_radius_client_io_t rlm_radius_udp = {
	.magic		= RLM_MODULE_INIT,
	.name		= "radius_udp",
	.inst_size	= sizeof(rlm_radius_udp_t),
	.request_inst_size = sizeof(rlm_radius_udp_request_t),
	.thread_inst_size	= sizeof(rlm_radius_udp_thread_t),

	.config		= module_config,
	.bootstrap	= mod_bootstrap,
	.instantiate	= mod_instantiate,
	.thread_instantiate = mod_thread_instantiate,

	.push		= mod_push,
};