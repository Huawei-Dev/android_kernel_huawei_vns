/*
 * net/tipc/node.c: TIPC node management routines
 *
 * Copyright (c) 2000-2006, 2012-2015, Ericsson AB
 * Copyright (c) 2005-2006, 2010-2014, Wind River Systems
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "core.h"
#include "link.h"
#include "node.h"
#include "name_distr.h"
#include "socket.h"
#include "bcast.h"
#include "discover.h"

/* Node FSM states and events:
 */
enum {
	SELF_DOWN_PEER_DOWN    = 0xdd,
	SELF_UP_PEER_UP        = 0xaa,
	SELF_DOWN_PEER_LEAVING = 0xd1,
	SELF_UP_PEER_COMING    = 0xac,
	SELF_COMING_PEER_UP    = 0xca,
	SELF_LEAVING_PEER_DOWN = 0x1d,
	NODE_FAILINGOVER       = 0xf0,
	NODE_SYNCHING          = 0xcc
};

enum {
	SELF_ESTABL_CONTACT_EVT = 0xece,
	SELF_LOST_CONTACT_EVT   = 0x1ce,
	PEER_ESTABL_CONTACT_EVT = 0x9ece,
	PEER_LOST_CONTACT_EVT   = 0x91ce,
	NODE_FAILOVER_BEGIN_EVT = 0xfbe,
	NODE_FAILOVER_END_EVT   = 0xfee,
	NODE_SYNCH_BEGIN_EVT    = 0xcbe,
	NODE_SYNCH_END_EVT      = 0xcee
};

static void tipc_node_link_down(struct tipc_node *n, int bearer_id);
static void node_lost_contact(struct tipc_node *n_ptr);
static void node_established_contact(struct tipc_node *n_ptr);
static void tipc_node_delete(struct tipc_node *node);
static void tipc_node_timeout(unsigned long data);
static void tipc_node_fsm_evt(struct tipc_node *n, int evt);

struct tipc_sock_conn {
	u32 port;
	u32 peer_port;
	u32 peer_node;
	struct list_head list;
};

static const struct nla_policy tipc_nl_node_policy[TIPC_NLA_NODE_MAX + 1] = {
	[TIPC_NLA_NODE_UNSPEC]		= { .type = NLA_UNSPEC },
	[TIPC_NLA_NODE_ADDR]		= { .type = NLA_U32 },
	[TIPC_NLA_NODE_UP]		= { .type = NLA_FLAG }
};

/*
 * A trivial power-of-two bitmask technique is used for speed, since this
 * operation is done for every incoming TIPC packet. The number of hash table
 * entries has been chosen so that no hash chain exceeds 8 nodes and will
 * usually be much smaller (typically only a single node).
 */
static unsigned int tipc_hashfn(u32 addr)
{
	return addr & (NODE_HTABLE_SIZE - 1);
}

static void tipc_node_kref_release(struct kref *kref)
{
	struct tipc_node *node = container_of(kref, struct tipc_node, kref);

	tipc_node_delete(node);
}

void tipc_node_put(struct tipc_node *node)
{
	kref_put(&node->kref, tipc_node_kref_release);
}

static void tipc_node_get(struct tipc_node *node)
{
	kref_get(&node->kref);
}

/*
 * tipc_node_find - locate specified node object, if it exists
 */
struct tipc_node *tipc_node_find(struct net *net, u32 addr)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct tipc_node *node;

	if (unlikely(!in_own_cluster_exact(net, addr)))
		return NULL;

	rcu_read_lock();
	hlist_for_each_entry_rcu(node, &tn->node_htable[tipc_hashfn(addr)],
				 hash) {
		if (node->addr == addr) {
			tipc_node_get(node);
			rcu_read_unlock();
			return node;
		}
	}
	rcu_read_unlock();
	return NULL;
}

struct tipc_node *tipc_node_create(struct net *net, u32 addr)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct tipc_node *n_ptr, *temp_node;

	spin_lock_bh(&tn->node_list_lock);
	n_ptr = tipc_node_find(net, addr);
	if (n_ptr)
		goto exit;
	n_ptr = kzalloc(sizeof(*n_ptr), GFP_ATOMIC);
	if (!n_ptr) {
		pr_warn("Node creation failed, no memory\n");
		goto exit;
	}
	n_ptr->addr = addr;
	n_ptr->net = net;
	kref_init(&n_ptr->kref);
	spin_lock_init(&n_ptr->lock);
	INIT_HLIST_NODE(&n_ptr->hash);
	INIT_LIST_HEAD(&n_ptr->list);
	INIT_LIST_HEAD(&n_ptr->publ_list);
	INIT_LIST_HEAD(&n_ptr->conn_sks);
	skb_queue_head_init(&n_ptr->bclink.namedq);
	__skb_queue_head_init(&n_ptr->bclink.deferdq);
	hlist_add_head_rcu(&n_ptr->hash, &tn->node_htable[tipc_hashfn(addr)]);
	list_for_each_entry_rcu(temp_node, &tn->node_list, list) {
		if (n_ptr->addr < temp_node->addr)
			break;
	}
	list_add_tail_rcu(&n_ptr->list, &temp_node->list);
	n_ptr->state = SELF_DOWN_PEER_LEAVING;
	n_ptr->signature = INVALID_NODE_SIG;
	n_ptr->active_links[0] = INVALID_BEARER_ID;
	n_ptr->active_links[1] = INVALID_BEARER_ID;
	tipc_node_get(n_ptr);
	setup_timer(&n_ptr->timer, tipc_node_timeout, (unsigned long)n_ptr);
	n_ptr->keepalive_intv = U32_MAX;
exit:
	spin_unlock_bh(&tn->node_list_lock);
	return n_ptr;
}

static void tipc_node_calculate_timer(struct tipc_node *n, struct tipc_link *l)
{
	unsigned long tol = l->tolerance;
	unsigned long intv = ((tol / 4) > 500) ? 500 : tol / 4;
	unsigned long keepalive_intv = msecs_to_jiffies(intv);

	/* Link with lowest tolerance determines timer interval */
	if (keepalive_intv < n->keepalive_intv)
		n->keepalive_intv = keepalive_intv;

	/* Ensure link's abort limit corresponds to current interval */
	l->abort_limit = l->tolerance / jiffies_to_msecs(n->keepalive_intv);
}

static void tipc_node_delete(struct tipc_node *node)
{
	list_del_rcu(&node->list);
	hlist_del_rcu(&node->hash);
	kfree_rcu(node, rcu);
}

void tipc_node_stop(struct net *net)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct tipc_node *node, *t_node;

	spin_lock_bh(&tn->node_list_lock);
	list_for_each_entry_safe(node, t_node, &tn->node_list, list) {
		if (del_timer(&node->timer))
			tipc_node_put(node);
		tipc_node_put(node);
	}
	spin_unlock_bh(&tn->node_list_lock);
}

int tipc_node_add_conn(struct net *net, u32 dnode, u32 port, u32 peer_port)
{
	struct tipc_node *node;
	struct tipc_sock_conn *conn;
	int err = 0;

	if (in_own_node(net, dnode))
		return 0;

	node = tipc_node_find(net, dnode);
	if (!node) {
		pr_warn("Connecting sock to node 0x%x failed\n", dnode);
		return -EHOSTUNREACH;
	}
	conn = kmalloc(sizeof(*conn), GFP_ATOMIC);
	if (!conn) {
		err = -EHOSTUNREACH;
		goto exit;
	}
	conn->peer_node = dnode;
	conn->port = port;
	conn->peer_port = peer_port;

	tipc_node_lock(node);
	list_add_tail(&conn->list, &node->conn_sks);
	tipc_node_unlock(node);
exit:
	tipc_node_put(node);
	return err;
}

void tipc_node_remove_conn(struct net *net, u32 dnode, u32 port)
{
	struct tipc_node *node;
	struct tipc_sock_conn *conn, *safe;

	if (in_own_node(net, dnode))
		return;

	node = tipc_node_find(net, dnode);
	if (!node)
		return;

	tipc_node_lock(node);
	list_for_each_entry_safe(conn, safe, &node->conn_sks, list) {
		if (port != conn->port)
			continue;
		list_del(&conn->list);
		kfree(conn);
	}
	tipc_node_unlock(node);
	tipc_node_put(node);
}

/* tipc_node_timeout - handle expiration of node timer
 */
static void tipc_node_timeout(unsigned long data)
{
	struct tipc_node *n = (struct tipc_node *)data;
	struct sk_buff_head xmitq;
	struct tipc_link *l;
	struct tipc_media_addr *maddr;
	int bearer_id;
	int rc = 0;

	__skb_queue_head_init(&xmitq);

	for (bearer_id = 0; bearer_id < MAX_BEARERS; bearer_id++) {
		tipc_node_lock(n);
		l = n->links[bearer_id].link;
		if (l) {
			/* Link tolerance may change asynchronously: */
			tipc_node_calculate_timer(n, l);
			rc = tipc_link_timeout(l, &xmitq);
			if (rc & TIPC_LINK_DOWN_EVT)
				tipc_node_link_down(n, bearer_id);
		}
		tipc_node_unlock(n);
		maddr = &n->links[bearer_id].maddr;
		tipc_bearer_xmit(n->net, bearer_id, &xmitq, maddr);
	}
	if (!mod_timer(&n->timer, jiffies + n->keepalive_intv))
		tipc_node_get(n);
	tipc_node_put(n);
}

/**
 * tipc_node_link_up - handle addition of link
 *
 * Link becomes active (alone or shared) or standby, depending on its priority.
 */
static void tipc_node_link_up(struct tipc_node *n, int bearer_id,
			      struct sk_buff_head *xmitq)
{
	int *slot0 = &n->active_links[0];
	int *slot1 = &n->active_links[1];
	struct tipc_link *ol = node_active_link(n, 0);
	struct tipc_link *nl = n->links[bearer_id].link;

	if (n->working_links > 1) {
		pr_warn("Attempt to establish 3rd link to %x\n", n->addr);
		return;
	}
	n->working_links++;
	n->action_flags |= TIPC_NOTIFY_LINK_UP;
	n->link_id = nl->peer_bearer_id << 16 | bearer_id;

	/* Leave room for tunnel header when returning 'mtu' to users: */
	n->links[bearer_id].mtu = nl->mtu - INT_H_SIZE;

	tipc_bearer_add_dest(n->net, bearer_id, n->addr);

	pr_debug("Established link <%s> on network plane %c\n",
		 nl->name, nl->net_plane);

	/* First link? => give it both slots */
	if (!ol) {
		*slot0 = bearer_id;
		*slot1 = bearer_id;
		nl->exec_mode = TIPC_LINK_OPEN;
		node_established_contact(n);
		return;
	}

	/* Second link => redistribute slots */
	if (nl->priority > ol->priority) {
		pr_debug("Old link <%s> becomes standby\n", ol->name);
		*slot0 = bearer_id;
		*slot1 = bearer_id;
	} else if (nl->priority == ol->priority) {
		*slot0 = bearer_id;
	} else {
		pr_debug("New link <%s> is standby\n", nl->name);
	}

	/* Prepare synchronization with first link */
	tipc_link_tnl_prepare(ol, nl, SYNCH_MSG, xmitq);
}

/**
 * tipc_node_link_down - handle loss of link
 */
static void tipc_node_link_down(struct tipc_node *n, int bearer_id)
{
	int *slot0 = &n->active_links[0];
	int *slot1 = &n->active_links[1];
	struct tipc_media_addr *maddr = &n->links[bearer_id].maddr;
	int i, highest = 0;
	struct tipc_link *l, *_l, *tnl;
	struct sk_buff_head xmitq;

	l = n->links[bearer_id].link;
	if (!l || !tipc_link_is_up(l))
		return;

	__skb_queue_head_init(&xmitq);

	n->working_links--;
	n->action_flags |= TIPC_NOTIFY_LINK_DOWN;
	n->link_id = l->peer_bearer_id << 16 | bearer_id;

	tipc_bearer_remove_dest(n->net, l->bearer_id, n->addr);

	pr_debug("Lost link <%s> on network plane %c\n",
		 l->name, l->net_plane);

	/* Select new active link if any available */
	*slot0 = INVALID_BEARER_ID;
	*slot1 = INVALID_BEARER_ID;
	for (i = 0; i < MAX_BEARERS; i++) {
		_l = n->links[i].link;
		if (!_l || !tipc_link_is_up(_l))
			continue;
		if (_l == l)
			continue;
		if (_l->priority < highest)
			continue;
		if (_l->priority > highest) {
			highest = _l->priority;
			*slot0 = i;
			*slot1 = i;
			continue;
		}
		*slot1 = i;
	}

	if (!tipc_node_is_up(n)) {
		tipc_link_reset(l);
		node_lost_contact(n);
		return;
	}

	/* There is still a working link => initiate failover */
	tnl = node_active_link(n, 0);
	tipc_node_fsm_evt(n, NODE_FAILOVER_BEGIN_EVT);
	n->sync_point = tnl->rcv_nxt + (U16_MAX / 2 - 1);
	tipc_link_tnl_prepare(l, tnl, FAILOVER_MSG, &xmitq);
	tipc_link_reset(l);
	tipc_bearer_xmit(n->net, tnl->bearer_id, &xmitq, maddr);
}

bool tipc_node_is_up(struct tipc_node *n)
{
	return n->active_links[0] != INVALID_BEARER_ID;
}

void tipc_node_check_dest(struct tipc_node *n, struct tipc_bearer *b,
			  bool *link_up, bool *addr_match,
			  struct tipc_media_addr *maddr)
{
	struct tipc_link *l = n->links[b->identity].link;
	struct tipc_media_addr *curr = &n->links[b->identity].maddr;

	*link_up = l && tipc_link_is_up(l);
	*addr_match = l && !memcmp(curr, maddr, sizeof(*maddr));
}

bool tipc_node_update_dest(struct tipc_node *n,  struct tipc_bearer *b,
			   struct tipc_media_addr *maddr)
{
	struct tipc_link *l = n->links[b->identity].link;
	struct tipc_media_addr *curr = &n->links[b->identity].maddr;
	struct sk_buff_head *inputq = &n->links[b->identity].inputq;

	if (!l) {
		l = tipc_link_create(n, b, maddr, inputq, &n->bclink.namedq);
		if (!l)
			return false;
		tipc_node_calculate_timer(n, l);
		if (n->link_cnt == 1) {
			if (!mod_timer(&n->timer, jiffies + n->keepalive_intv))
				tipc_node_get(n);
		}
	}
	memcpy(&l->media_addr, maddr, sizeof(*maddr));
	memcpy(curr, maddr, sizeof(*maddr));
	tipc_node_link_down(n, b->identity);
	return true;
}

void tipc_node_delete_links(struct net *net, int bearer_id)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct tipc_link *l;
	struct tipc_node *n;

	rcu_read_lock();
	list_for_each_entry_rcu(n, &tn->node_list, list) {
		tipc_node_lock(n);
		l = n->links[bearer_id].link;
		if (l) {
			tipc_node_link_down(n, bearer_id);
			n->links[bearer_id].link = NULL;
			n->link_cnt--;
		}
		tipc_node_unlock(n);
		kfree(l);
	}
	rcu_read_unlock();
}

static void tipc_node_reset_links(struct tipc_node *n)
{
	char addr_string[16];
	u32 i;

	tipc_node_lock(n);

	pr_warn("Resetting all links to %s\n",
		tipc_addr_string_fill(addr_string, n->addr));

	for (i = 0; i < MAX_BEARERS; i++) {
		if (!n->links[i].link)
			continue;
		tipc_node_link_down(n, i);
	}
	tipc_node_unlock(n);
}

void tipc_node_attach_link(struct tipc_node *n_ptr, struct tipc_link *l_ptr)
{
	n_ptr->links[l_ptr->bearer_id].link = l_ptr;
	n_ptr->link_cnt++;
}

void tipc_node_detach_link(struct tipc_node *n_ptr, struct tipc_link *l_ptr)
{
	int i;

	for (i = 0; i < MAX_BEARERS; i++) {
		if (l_ptr != n_ptr->links[i].link)
			continue;
		n_ptr->links[i].link = NULL;
		n_ptr->link_cnt--;
	}
}

/* tipc_node_fsm_evt - node finite state machine
 * Determines when contact is allowed with peer node
 */
static void tipc_node_fsm_evt(struct tipc_node *n, int evt)
{
	int state = n->state;

	switch (state) {
	case SELF_DOWN_PEER_DOWN:
		switch (evt) {
		case SELF_ESTABL_CONTACT_EVT:
			state = SELF_UP_PEER_COMING;
			break;
		case PEER_ESTABL_CONTACT_EVT:
			state = SELF_COMING_PEER_UP;
			break;
		case SELF_LOST_CONTACT_EVT:
		case PEER_LOST_CONTACT_EVT:
			break;
		case NODE_SYNCH_END_EVT:
		case NODE_SYNCH_BEGIN_EVT:
		case NODE_FAILOVER_BEGIN_EVT:
		case NODE_FAILOVER_END_EVT:
		default:
			goto illegal_evt;
		}
		break;
	case SELF_UP_PEER_UP:
		switch (evt) {
		case SELF_LOST_CONTACT_EVT:
			state = SELF_DOWN_PEER_LEAVING;
			break;
		case PEER_LOST_CONTACT_EVT:
			state = SELF_LEAVING_PEER_DOWN;
			break;
		case NODE_SYNCH_BEGIN_EVT:
			state = NODE_SYNCHING;
			break;
		case NODE_FAILOVER_BEGIN_EVT:
			state = NODE_FAILINGOVER;
			break;
		case SELF_ESTABL_CONTACT_EVT:
		case PEER_ESTABL_CONTACT_EVT:
		case NODE_SYNCH_END_EVT:
		case NODE_FAILOVER_END_EVT:
			break;
		default:
			goto illegal_evt;
		}
		break;
	case SELF_DOWN_PEER_LEAVING:
		switch (evt) {
		case PEER_LOST_CONTACT_EVT:
			state = SELF_DOWN_PEER_DOWN;
			break;
		case SELF_ESTABL_CONTACT_EVT:
		case PEER_ESTABL_CONTACT_EVT:
		case SELF_LOST_CONTACT_EVT:
			break;
		case NODE_SYNCH_END_EVT:
		case NODE_SYNCH_BEGIN_EVT:
		case NODE_FAILOVER_BEGIN_EVT:
		case NODE_FAILOVER_END_EVT:
		default:
			goto illegal_evt;
		}
		break;
	case SELF_UP_PEER_COMING:
		switch (evt) {
		case PEER_ESTABL_CONTACT_EVT:
			state = SELF_UP_PEER_UP;
			break;
		case SELF_LOST_CONTACT_EVT:
			state = SELF_DOWN_PEER_LEAVING;
			break;
		case SELF_ESTABL_CONTACT_EVT:
		case PEER_LOST_CONTACT_EVT:
			break;
		case NODE_SYNCH_END_EVT:
		case NODE_SYNCH_BEGIN_EVT:
		case NODE_FAILOVER_BEGIN_EVT:
		case NODE_FAILOVER_END_EVT:
		default:
			goto illegal_evt;
		}
		break;
	case SELF_COMING_PEER_UP:
		switch (evt) {
		case SELF_ESTABL_CONTACT_EVT:
			state = SELF_UP_PEER_UP;
			break;
		case PEER_LOST_CONTACT_EVT:
			state = SELF_LEAVING_PEER_DOWN;
			break;
		case SELF_LOST_CONTACT_EVT:
		case PEER_ESTABL_CONTACT_EVT:
			break;
		case NODE_SYNCH_END_EVT:
		case NODE_SYNCH_BEGIN_EVT:
		case NODE_FAILOVER_BEGIN_EVT:
		case NODE_FAILOVER_END_EVT:
		default:
			goto illegal_evt;
		}
		break;
	case SELF_LEAVING_PEER_DOWN:
		switch (evt) {
		case SELF_LOST_CONTACT_EVT:
			state = SELF_DOWN_PEER_DOWN;
			break;
		case SELF_ESTABL_CONTACT_EVT:
		case PEER_ESTABL_CONTACT_EVT:
		case PEER_LOST_CONTACT_EVT:
			break;
		case NODE_SYNCH_END_EVT:
		case NODE_SYNCH_BEGIN_EVT:
		case NODE_FAILOVER_BEGIN_EVT:
		case NODE_FAILOVER_END_EVT:
		default:
			goto illegal_evt;
		}
		break;
	case NODE_FAILINGOVER:
		switch (evt) {
		case SELF_LOST_CONTACT_EVT:
			state = SELF_DOWN_PEER_LEAVING;
			break;
		case PEER_LOST_CONTACT_EVT:
			state = SELF_LEAVING_PEER_DOWN;
			break;
		case NODE_FAILOVER_END_EVT:
			state = SELF_UP_PEER_UP;
			break;
		case NODE_FAILOVER_BEGIN_EVT:
		case SELF_ESTABL_CONTACT_EVT:
		case PEER_ESTABL_CONTACT_EVT:
			break;
		case NODE_SYNCH_BEGIN_EVT:
		case NODE_SYNCH_END_EVT:
		default:
			goto illegal_evt;
		}
		break;
	case NODE_SYNCHING:
		switch (evt) {
		case SELF_LOST_CONTACT_EVT:
			state = SELF_DOWN_PEER_LEAVING;
			break;
		case PEER_LOST_CONTACT_EVT:
			state = SELF_LEAVING_PEER_DOWN;
			break;
		case NODE_SYNCH_END_EVT:
			state = SELF_UP_PEER_UP;
			break;
		case NODE_FAILOVER_BEGIN_EVT:
			state = NODE_FAILINGOVER;
			break;
		case NODE_SYNCH_BEGIN_EVT:
		case SELF_ESTABL_CONTACT_EVT:
		case PEER_ESTABL_CONTACT_EVT:
			break;
		case NODE_FAILOVER_END_EVT:
		default:
			goto illegal_evt;
		}
		break;
	default:
		pr_err("Unknown node fsm state %x\n", state);
		break;
	}
	n->state = state;
	return;

illegal_evt:
	pr_err("Illegal node fsm evt %x in state %x\n", evt, state);
}

bool tipc_node_filter_pkt(struct tipc_node *n, struct tipc_msg *hdr)
{
	int state = n->state;

	if (likely(state == SELF_UP_PEER_UP))
		return true;

	if (state == SELF_LEAVING_PEER_DOWN)
		return false;

	if (state == SELF_DOWN_PEER_LEAVING) {
		if (msg_peer_node_is_up(hdr))
			return false;
	}

	return true;
}

static void node_established_contact(struct tipc_node *n_ptr)
{
	tipc_node_fsm_evt(n_ptr, SELF_ESTABL_CONTACT_EVT);
	n_ptr->action_flags |= TIPC_NOTIFY_NODE_UP;
	n_ptr->bclink.oos_state = 0;
	n_ptr->bclink.acked = tipc_bclink_get_last_sent(n_ptr->net);
	tipc_bclink_add_node(n_ptr->net, n_ptr->addr);
}

static void node_lost_contact(struct tipc_node *n_ptr)
{
	char addr_string[16];
	struct tipc_sock_conn *conn, *safe;
	struct list_head *conns = &n_ptr->conn_sks;
	struct sk_buff *skb;
	struct tipc_net *tn = net_generic(n_ptr->net, tipc_net_id);
	uint i;

	pr_debug("Lost contact with %s\n",
		 tipc_addr_string_fill(addr_string, n_ptr->addr));

	/* Flush broadcast link info associated with lost node */
	if (n_ptr->bclink.recv_permitted) {
		__skb_queue_purge(&n_ptr->bclink.deferdq);

		if (n_ptr->bclink.reasm_buf) {
			kfree_skb(n_ptr->bclink.reasm_buf);
			n_ptr->bclink.reasm_buf = NULL;
		}

		tipc_bclink_remove_node(n_ptr->net, n_ptr->addr);
		tipc_bclink_acknowledge(n_ptr, INVALID_LINK_SEQ);

		n_ptr->bclink.recv_permitted = false;
	}

	/* Abort any ongoing link failover */
	for (i = 0; i < MAX_BEARERS; i++) {
		struct tipc_link *l_ptr = n_ptr->links[i].link;
		if (!l_ptr)
			continue;
		l_ptr->exec_mode = TIPC_LINK_OPEN;
		kfree_skb(l_ptr->failover_reasm_skb);
		l_ptr->failover_reasm_skb = NULL;
		tipc_link_reset_fragments(l_ptr);
	}
	/* Prevent re-contact with node until cleanup is done */
	tipc_node_fsm_evt(n_ptr, SELF_LOST_CONTACT_EVT);

	/* Notify publications from this node */
	n_ptr->action_flags |= TIPC_NOTIFY_NODE_DOWN;

	/* Notify sockets connected to node */
	list_for_each_entry_safe(conn, safe, conns, list) {
		skb = tipc_msg_create(TIPC_CRITICAL_IMPORTANCE, TIPC_CONN_MSG,
				      SHORT_H_SIZE, 0, tn->own_addr,
				      conn->peer_node, conn->port,
				      conn->peer_port, TIPC_ERR_NO_NODE);
		if (likely(skb)) {
			skb_queue_tail(n_ptr->inputq, skb);
			n_ptr->action_flags |= TIPC_MSG_EVT;
		}
		list_del(&conn->list);
		kfree(conn);
	}
}

/**
 * tipc_node_get_linkname - get the name of a link
 *
 * @bearer_id: id of the bearer
 * @node: peer node address
 * @linkname: link name output buffer
 *
 * Returns 0 on success
 */
int tipc_node_get_linkname(struct net *net, u32 bearer_id, u32 addr,
			   char *linkname, size_t len)
{
	struct tipc_link *link;
	int err = -EINVAL;
	struct tipc_node *node = tipc_node_find(net, addr);

	if (!node)
		return err;

	if (bearer_id >= MAX_BEARERS)
		goto exit;

	tipc_node_lock(node);
	link = node->links[bearer_id].link;
	if (link) {
		strncpy(linkname, link->name, len);
		err = 0;
	}
exit:
	tipc_node_unlock(node);
	tipc_node_put(node);
	return err;
}

void tipc_node_unlock(struct tipc_node *node)
{
	struct net *net = node->net;
	u32 addr = 0;
	u32 flags = node->action_flags;
	u32 link_id = 0;
	struct list_head *publ_list;
	struct sk_buff_head *inputq = node->inputq;
	struct sk_buff_head *namedq;

	if (likely(!flags || (flags == TIPC_MSG_EVT))) {
		node->action_flags = 0;
		spin_unlock_bh(&node->lock);
		if (flags == TIPC_MSG_EVT)
			tipc_sk_rcv(net, inputq);
		return;
	}

	addr = node->addr;
	link_id = node->link_id;
	namedq = node->namedq;
	publ_list = &node->publ_list;

	node->action_flags &= ~(TIPC_MSG_EVT |
				TIPC_NOTIFY_NODE_DOWN | TIPC_NOTIFY_NODE_UP |
				TIPC_NOTIFY_LINK_DOWN | TIPC_NOTIFY_LINK_UP |
				TIPC_WAKEUP_BCAST_USERS | TIPC_BCAST_MSG_EVT |
				TIPC_NAMED_MSG_EVT | TIPC_BCAST_RESET);

	spin_unlock_bh(&node->lock);

	if (flags & TIPC_NOTIFY_NODE_DOWN)
		tipc_publ_notify(net, publ_list, addr);

	if (flags & TIPC_WAKEUP_BCAST_USERS)
		tipc_bclink_wakeup_users(net);

	if (flags & TIPC_NOTIFY_NODE_UP)
		tipc_named_node_up(net, addr);

	if (flags & TIPC_NOTIFY_LINK_UP)
		tipc_nametbl_publish(net, TIPC_LINK_STATE, addr, addr,
				     TIPC_NODE_SCOPE, link_id, addr);

	if (flags & TIPC_NOTIFY_LINK_DOWN)
		tipc_nametbl_withdraw(net, TIPC_LINK_STATE, addr,
				      link_id, addr);

	if (flags & TIPC_MSG_EVT)
		tipc_sk_rcv(net, inputq);

	if (flags & TIPC_NAMED_MSG_EVT)
		tipc_named_rcv(net, namedq);

	if (flags & TIPC_BCAST_MSG_EVT)
		tipc_bclink_input(net);

	if (flags & TIPC_BCAST_RESET)
		tipc_node_reset_links(node);
}

/* Caller should hold node lock for the passed node */
static int __tipc_nl_add_node(struct tipc_nl_msg *msg, struct tipc_node *node)
{
	void *hdr;
	struct nlattr *attrs;

	hdr = genlmsg_put(msg->skb, msg->portid, msg->seq, &tipc_genl_family,
			  NLM_F_MULTI, TIPC_NL_NODE_GET);
	if (!hdr)
		return -EMSGSIZE;

	attrs = nla_nest_start(msg->skb, TIPC_NLA_NODE);
	if (!attrs)
		goto msg_full;

	if (nla_put_u32(msg->skb, TIPC_NLA_NODE_ADDR, node->addr))
		goto attr_msg_full;
	if (tipc_node_is_up(node))
		if (nla_put_flag(msg->skb, TIPC_NLA_NODE_UP))
			goto attr_msg_full;

	nla_nest_end(msg->skb, attrs);
	genlmsg_end(msg->skb, hdr);

	return 0;

attr_msg_full:
	nla_nest_cancel(msg->skb, attrs);
msg_full:
	genlmsg_cancel(msg->skb, hdr);

	return -EMSGSIZE;
}

static struct tipc_link *tipc_node_select_link(struct tipc_node *n, int sel,
					       int *bearer_id,
					       struct tipc_media_addr **maddr)
{
	int id = n->active_links[sel & 1];

	if (unlikely(id < 0))
		return NULL;

	*bearer_id = id;
	*maddr = &n->links[id].maddr;
	return n->links[id].link;
}

/**
 * tipc_node_xmit() is the general link level function for message sending
 * @net: the applicable net namespace
 * @list: chain of buffers containing message
 * @dnode: address of destination node
 * @selector: a number used for deterministic link selection
 * Consumes the buffer chain, except when returning -ELINKCONG
 * Returns 0 if success, otherwise errno: -ELINKCONG,-EHOSTUNREACH,-EMSGSIZE
 */
int tipc_node_xmit(struct net *net, struct sk_buff_head *list,
		   u32 dnode, int selector)
{
	struct tipc_link *l = NULL;
	struct tipc_node *n;
	struct sk_buff_head xmitq;
	struct tipc_media_addr *maddr;
	int bearer_id;
	int rc = -EHOSTUNREACH;

	__skb_queue_head_init(&xmitq);
	n = tipc_node_find(net, dnode);
	if (likely(n)) {
		tipc_node_lock(n);
		l = tipc_node_select_link(n, selector, &bearer_id, &maddr);
		if (likely(l))
			rc = tipc_link_xmit(l, list, &xmitq);
		if (unlikely(rc == -ENOBUFS))
			tipc_node_link_down(n, bearer_id);
		tipc_node_unlock(n);
		tipc_node_put(n);
	}
	if (likely(!rc)) {
		tipc_bearer_xmit(net, bearer_id, &xmitq, maddr);
		return 0;
	}
	if (likely(in_own_node(net, dnode))) {
		tipc_sk_rcv(net, list);
		return 0;
	}
	return rc;
}

/* tipc_node_xmit_skb(): send single buffer to destination
 * Buffers sent via this functon are generally TIPC_SYSTEM_IMPORTANCE
 * messages, which will not be rejected
 * The only exception is datagram messages rerouted after secondary
 * lookup, which are rare and safe to dispose of anyway.
 * TODO: Return real return value, and let callers use
 * tipc_wait_for_sendpkt() where applicable
 */
int tipc_node_xmit_skb(struct net *net, struct sk_buff *skb, u32 dnode,
		       u32 selector)
{
	struct sk_buff_head head;
	int rc;

	skb_queue_head_init(&head);
	__skb_queue_tail(&head, skb);
	rc = tipc_node_xmit(net, &head, dnode, selector);
	if (rc == -ELINKCONG)
		kfree_skb(skb);
	return 0;
}

/**
 * tipc_node_check_state - check and if necessary update node state
 * @skb: TIPC packet
 * @bearer_id: identity of bearer delivering the packet
 * Returns true if state is ok, otherwise consumes buffer and returns false
 */
static bool tipc_node_check_state(struct tipc_node *n, struct sk_buff *skb,
				  int bearer_id)
{
	struct tipc_msg *hdr = buf_msg(skb);
	int usr = msg_user(hdr);
	int mtyp = msg_type(hdr);
	u16 oseqno = msg_seqno(hdr);
	u16 iseqno = msg_seqno(msg_get_wrapped(hdr));
	u16 exp_pkts = msg_msgcnt(hdr);
	u16 rcv_nxt, syncpt, dlv_nxt;
	int state = n->state;
	struct tipc_link *l, *pl = NULL;
	struct sk_buff_head;
	int i;

	l = n->links[bearer_id].link;
	if (!l)
		return false;
	rcv_nxt = l->rcv_nxt;


	if (likely((state == SELF_UP_PEER_UP) && (usr != TUNNEL_PROTOCOL)))
		return true;

	/* Find parallel link, if any */
	for (i = 0; i < MAX_BEARERS; i++) {
		if ((i != bearer_id) && n->links[i].link) {
			pl = n->links[i].link;
			break;
		}
	}

	/* Update node accesibility if applicable */
	if (state == SELF_UP_PEER_COMING) {
		if (!tipc_link_is_up(l))
			return true;
		if (!msg_peer_link_is_up(hdr))
			return true;
		tipc_node_fsm_evt(n, PEER_ESTABL_CONTACT_EVT);
	}

	if (state == SELF_DOWN_PEER_LEAVING) {
		if (msg_peer_node_is_up(hdr))
			return false;
		tipc_node_fsm_evt(n, PEER_LOST_CONTACT_EVT);
	}

	/* Ignore duplicate packets */
	if (less(oseqno, rcv_nxt))
		return true;

	/* Initiate or update failover mode if applicable */
	if ((usr == TUNNEL_PROTOCOL) && (mtyp == FAILOVER_MSG)) {
		syncpt = oseqno + exp_pkts - 1;
		if (pl && tipc_link_is_up(pl)) {
			tipc_node_link_down(n, pl->bearer_id);
			pl->exec_mode = TIPC_LINK_BLOCKED;
		}
		/* If pkts arrive out of order, use lowest calculated syncpt */
		if (less(syncpt, n->sync_point))
			n->sync_point = syncpt;
	}

	/* Open parallel link when tunnel link reaches synch point */
	if ((n->state == NODE_FAILINGOVER) && (more(rcv_nxt, n->sync_point))) {
		tipc_node_fsm_evt(n, NODE_FAILOVER_END_EVT);
		if (pl)
			pl->exec_mode = TIPC_LINK_OPEN;
		return true;
	}

	/* Initiate or update synch mode if applicable */
	if ((usr == TUNNEL_PROTOCOL) && (mtyp == SYNCH_MSG)) {
		syncpt = iseqno + exp_pkts - 1;
		if (n->state == SELF_UP_PEER_UP) {
			n->sync_point = syncpt;
			tipc_node_fsm_evt(n, NODE_SYNCH_BEGIN_EVT);
		}
		l->exec_mode = TIPC_LINK_TUNNEL;
		if (less(syncpt, n->sync_point))
			n->sync_point = syncpt;
	}

	/* Open tunnel link when parallel link reaches synch point */
	if ((n->state == NODE_SYNCHING) && (l->exec_mode == TIPC_LINK_TUNNEL)) {
		if (pl)
			dlv_nxt = mod(pl->rcv_nxt - skb_queue_len(pl->inputq));
		if (!pl || more(dlv_nxt, n->sync_point)) {
			tipc_node_fsm_evt(n, NODE_SYNCH_END_EVT);
			l->exec_mode = TIPC_LINK_OPEN;
			return true;
		}
		if ((usr == TUNNEL_PROTOCOL) && (mtyp == SYNCH_MSG))
			return true;
		if (usr == LINK_PROTOCOL)
			return true;
		return false;
	}
	return true;
}

/**
 * tipc_rcv - process TIPC packets/messages arriving from off-node
 * @net: the applicable net namespace
 * @skb: TIPC packet
 * @bearer: pointer to bearer message arrived on
 *
 * Invoked with no locks held. Bearer pointer must point to a valid bearer
 * structure (i.e. cannot be NULL), but bearer can be inactive.
 */
void tipc_rcv(struct net *net, struct sk_buff *skb, struct tipc_bearer *b)
{
	struct sk_buff_head xmitq;
	struct tipc_node *n;
	struct tipc_msg *hdr = buf_msg(skb);
	int usr = msg_user(hdr);
	int bearer_id = b->identity;
	struct tipc_link_entry *le;
	int rc = 0;

	__skb_queue_head_init(&xmitq);

	/* Ensure message is well-formed */
	if (unlikely(!tipc_msg_validate(skb)))
		goto discard;

	/* Handle arrival of a non-unicast link packet */
	if (unlikely(msg_non_seq(hdr))) {
		if (usr ==  LINK_CONFIG)
			tipc_disc_rcv(net, skb, b);
		else
			tipc_bclink_rcv(net, skb);
		return;
	}

	/* Locate neighboring node that sent packet */
	n = tipc_node_find(net, msg_prevnode(hdr));
	if (unlikely(!n))
		goto discard;
	le = &n->links[bearer_id];

	tipc_node_lock(n);

	/* Is reception permitted at the moment ? */
	if (!tipc_node_filter_pkt(n, hdr))
		goto unlock;

	if (unlikely(msg_user(hdr) == LINK_PROTOCOL))
		tipc_bclink_sync_state(n, hdr);

	/* Release acked broadcast messages */
	if (unlikely(n->bclink.acked != msg_bcast_ack(hdr)))
		tipc_bclink_acknowledge(n, msg_bcast_ack(hdr));

	/* Check and if necessary update node state */
	if (likely(tipc_node_check_state(n, skb, bearer_id))) {
		rc = tipc_link_rcv(le->link, skb, &xmitq);
		skb = NULL;
	}

	if (unlikely(rc & TIPC_LINK_UP_EVT))
		tipc_node_link_up(n, bearer_id, &xmitq);

	if (unlikely(rc & TIPC_LINK_DOWN_EVT))
		tipc_node_link_down(n, bearer_id);
unlock:
	tipc_node_unlock(n);

	if (!skb_queue_empty(&le->inputq))
		tipc_sk_rcv(net, &le->inputq);

	if (!skb_queue_empty(&xmitq))
		tipc_bearer_xmit(net, bearer_id, &xmitq, &le->maddr);

	tipc_node_put(n);
discard:
	kfree_skb(skb);
}

int tipc_nl_node_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	int err;
	struct net *net = sock_net(skb->sk);
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	int done = cb->args[0];
	int last_addr = cb->args[1];
	struct tipc_node *node;
	struct tipc_nl_msg msg;

	if (done)
		return 0;

	msg.skb = skb;
	msg.portid = NETLINK_CB(cb->skb).portid;
	msg.seq = cb->nlh->nlmsg_seq;

	rcu_read_lock();
	if (last_addr) {
		node = tipc_node_find(net, last_addr);
		if (!node) {
			rcu_read_unlock();
			/* We never set seq or call nl_dump_check_consistent()
			 * this means that setting prev_seq here will cause the
			 * consistence check to fail in the netlink callback
			 * handler. Resulting in the NLMSG_DONE message having
			 * the NLM_F_DUMP_INTR flag set if the node state
			 * changed while we released the lock.
			 */
			cb->prev_seq = 1;
			return -EPIPE;
		}
		tipc_node_put(node);
	}

	list_for_each_entry_rcu(node, &tn->node_list, list) {
		if (last_addr) {
			if (node->addr == last_addr)
				last_addr = 0;
			else
				continue;
		}

		tipc_node_lock(node);
		err = __tipc_nl_add_node(&msg, node);
		if (err) {
			last_addr = node->addr;
			tipc_node_unlock(node);
			goto out;
		}

		tipc_node_unlock(node);
	}
	done = 1;
out:
	cb->args[0] = done;
	cb->args[1] = last_addr;
	rcu_read_unlock();

	return skb->len;
}
