/*
    net_packet.c -- Handles in- and outgoing VPN packets
    Copyright (C) 1998-2005 Ivo Timmermans,
                  2000-2009 Guus Sliepen <guus@tinc-vpn.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#include "system.h"

#include <zlib.h>
#include LZO1X_H

#include "splay_tree.h"
#include "cipher.h"
#include "conf.h"
#include "connection.h"
#include "crypto.h"
#include "digest.h"
#include "device.h"
#include "ethernet.h"
#include "graph.h"
#include "list.h"
#include "logger.h"
#include "net.h"
#include "netutl.h"
#include "protocol.h"
#include "process.h"
#include "route.h"
#include "utils.h"
#include "xalloc.h"

#ifdef WSAEMSGSIZE
#define EMSGSIZE WSAEMSGSIZE
#endif

int keylifetime = 0;
int keyexpires = 0;
static char lzo_wrkmem[LZO1X_999_MEM_COMPRESS > LZO1X_1_MEM_COMPRESS ? LZO1X_999_MEM_COMPRESS : LZO1X_1_MEM_COMPRESS];

static void send_udppacket(node_t *, vpn_packet_t *);

#define MAX_SEQNO 1073741824

static void send_mtu_probe_handler(int fd, short events, void *data) {
	node_t *n = data;
	vpn_packet_t packet;
	int len, i;
	
	cp();

	n->mtuprobes++;

	if(n->mtuprobes >= 10 && !n->minmtu) {
		ifdebug(TRAFFIC) logger(LOG_INFO, _("No response to MTU probes from %s (%s)"), n->name, n->hostname);
		return;
	}

	for(i = 0; i < 3; i++) {
		if(n->mtuprobes >= 30 || n->minmtu >= n->maxmtu) {
			n->mtu = n->minmtu;
			ifdebug(TRAFFIC) logger(LOG_INFO, _("Fixing MTU of %s (%s) to %d after %d probes"), n->name, n->hostname, n->mtu, n->mtuprobes);
			return;
		}

		len = n->minmtu + 1 + random() % (n->maxmtu - n->minmtu);
		if(len < 64)
			len = 64;
		
		memset(packet.data, 0, 14);
		randomize(packet.data + 14, len - 14);
		packet.len = len;
		packet.priority = 0;

		ifdebug(TRAFFIC) logger(LOG_INFO, _("Sending MTU probe length %d to %s (%s)"), len, n->name, n->hostname);

		send_udppacket(n, &packet);
	}

	event_add(&n->mtuevent, &(struct timeval){1, 0});
}

void send_mtu_probe(node_t *n) {
	if(!timeout_initialized(&n->mtuevent))
		timeout_set(&n->mtuevent, send_mtu_probe_handler, n);
	send_mtu_probe_handler(0, 0, n);
}

void mtu_probe_h(node_t *n, vpn_packet_t *packet, length_t len) {
	ifdebug(TRAFFIC) logger(LOG_INFO, _("Got MTU probe length %d from %s (%s)"), packet->len, n->name, n->hostname);

	if(!packet->data[0]) {
		packet->data[0] = 1;
		send_packet(n, packet);
	} else {
		if(n->minmtu < len)
			n->minmtu = len;
	}
}

static length_t compress_packet(uint8_t *dest, const uint8_t *source, length_t len, int level) {
	if(level == 10) {
		lzo_uint lzolen = MAXSIZE;
		lzo1x_1_compress(source, len, dest, &lzolen, lzo_wrkmem);
		return lzolen;
	} else if(level < 10) {
		unsigned long destlen = MAXSIZE;
		if(compress2(dest, &destlen, source, len, level) == Z_OK)
			return destlen;
		else
			return -1;
	} else {
		lzo_uint lzolen = MAXSIZE;
		lzo1x_999_compress(source, len, dest, &lzolen, lzo_wrkmem);
		return lzolen;
	}
	
	return -1;
}

static length_t uncompress_packet(uint8_t *dest, const uint8_t *source, length_t len, int level) {
	if(level > 9) {
		lzo_uint lzolen = MAXSIZE;
		if(lzo1x_decompress_safe(source, len, dest, &lzolen, NULL) == LZO_E_OK)
			return lzolen;
		else
			return -1;
	} else {
		unsigned long destlen = MAXSIZE;
		if(uncompress(dest, &destlen, source, len) == Z_OK)
			return destlen;
		else
			return -1;
	}
	
	return -1;
}

/* VPN packet I/O */

static void receive_packet(node_t *n, vpn_packet_t *packet) {
	cp();

	ifdebug(TRAFFIC) logger(LOG_DEBUG, _("Received packet of %d bytes from %s (%s)"),
			   packet->len, n->name, n->hostname);

	route(n, packet);
}

static bool try_mac(node_t *n, const vpn_packet_t *inpkt)
{
	if(!digest_active(&n->indigest) || !n->inmaclength || inpkt->len < sizeof inpkt->seqno + n->inmaclength)
		return false;

	return digest_verify(&n->indigest, &inpkt->seqno, inpkt->len, &inpkt->seqno + inpkt->len);
}

static void receive_udppacket(node_t *n, vpn_packet_t *inpkt)
{
	vpn_packet_t pkt1, pkt2;
	vpn_packet_t *pkt[] = { &pkt1, &pkt2, &pkt1, &pkt2 };
	int nextpkt = 0;
	vpn_packet_t *outpkt = pkt[0];
	size_t outlen;
	int i;

	cp();

	if(!cipher_active(&n->incipher)) {
		ifdebug(TRAFFIC) logger(LOG_DEBUG, _("Got packet from %s (%s) but he hasn't got our key yet"),
					n->name, n->hostname);
		return;
	}

	/* Check packet length */

	if(inpkt->len < sizeof inpkt->seqno + digest_length(&n->indigest)) {
		ifdebug(TRAFFIC) logger(LOG_DEBUG, _("Got too short packet from %s (%s)"),
					n->name, n->hostname);
		return;
	}

	/* Check the message authentication code */

	if(digest_active(&n->indigest) && !digest_verify(&n->indigest, &inpkt->seqno, inpkt->len, &inpkt->seqno + inpkt->len)) {
		ifdebug(TRAFFIC) logger(LOG_DEBUG, _("Got unauthenticated packet from %s (%s)"), n->name, n->hostname);
		return;
	}

	/* Decrypt the packet */

	if(cipher_active(&n->incipher)) {
		outpkt = pkt[nextpkt++];
		outlen = MAXSIZE;

		if(!cipher_decrypt(&n->incipher, &inpkt->seqno, inpkt->len, &outpkt->seqno, &outlen, true)) {
			ifdebug(TRAFFIC) logger(LOG_DEBUG, _("Error decrypting packet from %s (%s)"), n->name, n->hostname);
			return;
		}
		
		outpkt->len = outlen;
		inpkt = outpkt;
	}

	/* Check the sequence number */

	inpkt->len -= sizeof inpkt->seqno;
	inpkt->seqno = ntohl(inpkt->seqno);

	if(inpkt->seqno != n->received_seqno + 1) {
		if(inpkt->seqno >= n->received_seqno + sizeof n->late * 8) {
			logger(LOG_WARNING, _("Lost %d packets from %s (%s)"),
					   inpkt->seqno - n->received_seqno - 1, n->name, n->hostname);
			
			memset(n->late, 0, sizeof n->late);
		} else if (inpkt->seqno <= n->received_seqno) {
			if((n->received_seqno >= sizeof n->late * 8 && inpkt->seqno <= n->received_seqno - sizeof n->late * 8) || !(n->late[(inpkt->seqno / 8) % sizeof n->late] & (1 << inpkt->seqno % 8))) {
				logger(LOG_WARNING, _("Got late or replayed packet from %s (%s), seqno %d, last received %d"),
					   n->name, n->hostname, inpkt->seqno, n->received_seqno);
				return;
			}
		} else {
			for(i = n->received_seqno + 1; i < inpkt->seqno; i++)
				n->late[(i / 8) % sizeof n->late] |= 1 << i % 8;
		}
	}
	
	n->late[(inpkt->seqno / 8) % sizeof n->late] &= ~(1 << inpkt->seqno % 8);

	if(inpkt->seqno > n->received_seqno)
		n->received_seqno = inpkt->seqno;
			
	if(n->received_seqno > MAX_SEQNO)
		regenerate_key();

	/* Decompress the packet */

	length_t origlen = inpkt->len;

	if(n->incompression) {
		outpkt = pkt[nextpkt++];

		if((outpkt->len = uncompress_packet(outpkt->data, inpkt->data, inpkt->len, n->incompression)) < 0) {
			ifdebug(TRAFFIC) logger(LOG_ERR, _("Error while uncompressing packet from %s (%s)"),
				  		 n->name, n->hostname);
			return;
		}

		inpkt = outpkt;

		origlen -= MTU/64 + 20;
	}

	inpkt->priority = 0;

	if(!inpkt->data[12] && !inpkt->data[13])
		mtu_probe_h(n, inpkt, origlen);
	else
		receive_packet(n, inpkt);
}

void receive_tcppacket(connection_t *c, char *buffer, int len) {
	vpn_packet_t outpkt;

	cp();

	outpkt.len = len;
	if(c->options & OPTION_TCPONLY)
		outpkt.priority = 0;
	else
		outpkt.priority = -1;
	memcpy(outpkt.data, buffer, len);

	receive_packet(c->node, &outpkt);
}

static void send_udppacket(node_t *n, vpn_packet_t *origpkt) {
	vpn_packet_t pkt1, pkt2;
	vpn_packet_t *pkt[] = { &pkt1, &pkt2, &pkt1, &pkt2 };
	vpn_packet_t *inpkt = origpkt;
	int nextpkt = 0;
	vpn_packet_t *outpkt;
	int origlen;
	size_t outlen;
	static int priority = 0;
	int origpriority;
	int sock;

	cp();

	/* Make sure we have a valid key */

	if(!n->status.validkey) {
		ifdebug(TRAFFIC) logger(LOG_INFO,
				   _("No valid key known yet for %s (%s), forwarding via TCP"),
				   n->name, n->hostname);

		if(!n->status.waitingforkey)
			send_req_key(n);

		n->status.waitingforkey = true;

		send_tcppacket(n->nexthop->connection, origpkt);

		return;
	}

	if(n->options & OPTION_PMTU_DISCOVERY && !n->minmtu && (inpkt->data[12] | inpkt->data[13])) {
		ifdebug(TRAFFIC) logger(LOG_INFO,
				_("No minimum MTU established yet for %s (%s), forwarding via TCP"),
				n->name, n->hostname);

		send_tcppacket(n->nexthop->connection, origpkt);

		return;
	}

	origlen = inpkt->len;
	origpriority = inpkt->priority;

	/* Compress the packet */

	if(n->outcompression) {
		outpkt = pkt[nextpkt++];

		if((outpkt->len = compress_packet(outpkt->data, inpkt->data, inpkt->len, n->outcompression)) < 0) {
			ifdebug(TRAFFIC) logger(LOG_ERR, _("Error while compressing packet to %s (%s)"),
				   n->name, n->hostname);
			return;
		}

		inpkt = outpkt;
	}

	/* Add sequence number */

	inpkt->seqno = htonl(++(n->sent_seqno));
	inpkt->len += sizeof inpkt->seqno;

	/* Encrypt the packet */

	if(cipher_active(&n->outcipher)) {
		outpkt = pkt[nextpkt++];
		outlen = MAXSIZE;

		if(!cipher_encrypt(&n->outcipher, &inpkt->seqno, inpkt->len, &outpkt->seqno, &outlen, true)) {
			ifdebug(TRAFFIC) logger(LOG_ERR, _("Error while encrypting packet to %s (%s)"), n->name, n->hostname);
			goto end;
		}

		outpkt->len = outlen;
		inpkt = outpkt;
	}

	/* Add the message authentication code */

	if(digest_active(&n->outdigest)) {
		digest_create(&n->outdigest, &inpkt->seqno, inpkt->len, &inpkt->seqno + inpkt->len);
		inpkt->len += digest_length(&n->outdigest);
	}

	/* Determine which socket we have to use */

	for(sock = 0; sock < listen_sockets; sock++)
		if(n->address.sa.sa_family == listen_socket[sock].sa.sa.sa_family)
			break;

	if(sock >= listen_sockets)
		sock = 0;				/* If none is available, just use the first and hope for the best. */

	/* Send the packet */

#if defined(SOL_IP) && defined(IP_TOS)
	if(priorityinheritance && origpriority != priority
	   && listen_socket[sock].sa.sa.sa_family == AF_INET) {
		priority = origpriority;
		ifdebug(TRAFFIC) logger(LOG_DEBUG, _("Setting outgoing packet priority to %d"), priority);
		if(setsockopt(listen_socket[sock].udp, SOL_IP, IP_TOS, &priority, sizeof priority))	/* SO_PRIORITY doesn't seem to work */
			logger(LOG_ERR, _("System call `%s' failed: %s"), "setsockopt", strerror(errno));
	}
#endif

	if((sendto(listen_socket[sock].udp, (char *) &inpkt->seqno, inpkt->len, 0, &(n->address.sa), SALEN(n->address.sa))) < 0) {
		if(errno == EMSGSIZE) {
			if(n->maxmtu >= origlen)
				n->maxmtu = origlen - 1;
			if(n->mtu >= origlen)
				n->mtu = origlen - 1;
		} else
			logger(LOG_ERR, _("Error sending packet to %s (%s): %s"), n->name, n->hostname, strerror(errno));
	}

end:
	origpkt->len = origlen;
}

/*
  send a packet to the given vpn ip.
*/
void send_packet(const node_t *n, vpn_packet_t *packet) {
	node_t *via;

	cp();

	if(n == myself) {
		if(overwrite_mac)
			 memcpy(packet->data, mymac.x, ETH_ALEN);
		write_packet(packet);
		return;
	}

	ifdebug(TRAFFIC) logger(LOG_ERR, _("Sending packet of %d bytes to %s (%s)"),
			   packet->len, n->name, n->hostname);

	if(!n->status.reachable) {
		ifdebug(TRAFFIC) logger(LOG_INFO, _("Node %s (%s) is not reachable"),
				   n->name, n->hostname);
		return;
	}

	via = (packet->priority == -1 || n->via == myself) ? n->nexthop : n->via;

	if(via != n)
		ifdebug(TRAFFIC) logger(LOG_INFO, _("Sending packet to %s via %s (%s)"),
			   n->name, via->name, n->via->hostname);

	if(packet->priority == -1 || ((myself->options | via->options) & OPTION_TCPONLY)) {
		if(!send_tcppacket(via->connection, packet))
			terminate_connection(via->connection, true);
	} else
		send_udppacket(via, packet);
}

/* Broadcast a packet using the minimum spanning tree */

void broadcast_packet(const node_t *from, vpn_packet_t *packet) {
	splay_node_t *node;
	connection_t *c;

	cp();

	ifdebug(TRAFFIC) logger(LOG_INFO, _("Broadcasting packet of %d bytes from %s (%s)"),
			   packet->len, from->name, from->hostname);

	if(from != myself) {
		send_packet(myself, packet);

		// In TunnelServer mode, do not forward broadcast packets.
                // The MST might not be valid and create loops.
		if(tunnelserver)
			return;
	}

	for(node = connection_tree->head; node; node = node->next) {
		c = node->data;

		if(c->status.active && c->status.mst && c != from->nexthop->connection)
			send_packet(c->node, packet);
	}
}

static node_t *try_harder(const sockaddr_t *from, const vpn_packet_t *pkt) {
	splay_node_t *node;
	edge_t *e;
	node_t *n = NULL;

	for(node = edge_weight_tree->head; node; node = node->next) {
		e = node->data;

		if(sockaddrcmp_noport(from, &e->address))
			continue;

		if(!n)
			n = e->to;

		if(!try_mac(e->to, pkt))
			continue;

		n = e->to;
		break;
	}

	return n;
}

void handle_incoming_vpn_data(int sock, short events, void *data)
{
	vpn_packet_t pkt;
	char *hostname;
	sockaddr_t from;
	socklen_t fromlen = sizeof from;
	node_t *n;

	cp();

	pkt.len = recvfrom(sock, (char *) &pkt.seqno, MAXSIZE, 0, &from.sa, &fromlen);

	if(pkt.len < 0) {
		logger(LOG_ERR, _("Receiving packet failed: %s"), strerror(errno));
		return;
	}

	sockaddrunmap(&from);		/* Some braindead IPv6 implementations do stupid things. */

	n = lookup_node_udp(&from);

	if(!n) {
		n = try_harder(&from, &pkt);
		if(n)
			update_node_udp(n, &from);
		else ifdebug(PROTOCOL) {
			hostname = sockaddr2hostname(&from);
			logger(LOG_WARNING, _("Received UDP packet from unknown source %s"), hostname);
			free(hostname);
			return;
		}
		else
			return;
	}

	receive_udppacket(n, &pkt);
}

void handle_device_data(int sock, short events, void *data) {
	vpn_packet_t packet;

	if(read_packet(&packet))
		route(myself, &packet);
}
