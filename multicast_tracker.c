/*
 * Copyright (C) 2012 B.A.T.M.A.N. contributors:
 *
 * Linus Lüssing
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 *
 */

/* multicast_tracker.c - Active multicast path marking
 *
 * These functions combine the MLA and flow infrastructures: On sufficient
 * multicast data flow for a specific multicast destination MAC, small
 * tracker packets are actively sent to mark all paths towards destinations,
 * destinations which were previously announced via MLAs.
 */

#include "main.h"
#include "multicast_flow.h"
#include "multicast_tracker.h"
#include "multicast_forw.h"
#include "hash.h"
#include "originator.h"
#include "hard-interface.h"
#include "send.h"
#include "translation-table.h"

#define TRACKER_BURST_EXTRA 2

/**
 * batadv_tracker_packet_state - Tracker packet iterator state
 * @mcast_num:	Index of the current multicast address entry
 * @dest_num:	Index of the current destination address
 * @mcast_entry: Pointer to current multicast entry
 * @dest_entry:	Pointer to current destination entry
 * @break_flag:	Flag to handle loop break instructions correctly
 *
 * This is an iterator state used by batadv_tracker_packet_for_each_dest().
 * It memorizes the current position within the given tracker packet.
 *
 * Indices start with zero.
 */
struct batadv_tracker_packet_state {
	int mcast_num, dest_num;
	struct batadv_mcast_entry *mcast_entry;
	uint8_t *dest_entry;
	int break_flag;
};

static void batadv_mcast_entry_init_s(
			struct batadv_tracker_packet_state *state,
			struct batadv_mcast_tracker_packet *tracker_packet)
{
	state->mcast_num = 0;
	state->mcast_entry = (struct batadv_mcast_entry *)(tracker_packet + 1);
	state->dest_entry = (uint8_t *)(state->mcast_entry + 1);
	state->break_flag = 0;
}

static int batadv_mcast_entry_check_s(
			struct batadv_tracker_packet_state *state,
			struct batadv_mcast_tracker_packet *tracker_packet)
{
	if (state->mcast_num < tracker_packet->num_mcast_entries &&
	    !state->break_flag)
		return 1;

	return 0;
}

static void batadv_mcast_entry_inc_s(struct batadv_tracker_packet_state *state)
{
	if (state->break_flag)
		return;

	state->mcast_num++;
	state->mcast_entry = (struct batadv_mcast_entry *)state->dest_entry;
	state->dest_entry = (uint8_t *)(state->mcast_entry + 1);
}

static void batadv_mcast_dest_entry_init_s(
				struct batadv_tracker_packet_state *state)
{
	state->dest_num = 0;
	state->break_flag = 1;
}

static int batadv_mcast_dest_entry_check_s(
				struct batadv_tracker_packet_state *state)
{
	if (state->dest_num < state->mcast_entry->num_dest)
		return 1;

	state->break_flag = 0;
	return 0;
}

static void batadv_mcast_dest_entry_inc_s(
				struct batadv_tracker_packet_state *state)
{
	state->dest_num++;
	state->dest_entry += ETH_ALEN;
}

/*
 * batadv_tracker_packet_for_each_dest - Tracker packet iterator
 * @state:		A tracker packet iterator state
 * @tracker_packet:	A tracker packet to iterate over
 *
 * Iterates over destination entries of a given tracker packet and memorizes
 * the current position in the provided tracker packet state.
 */
#define batadv_tracker_packet_for_each_dest(state, tracker_packet)	\
	for (batadv_mcast_entry_init_s(state, tracker_packet);		\
	     batadv_mcast_entry_check_s(state, tracker_packet);		\
	     batadv_mcast_entry_inc_s(state))				\
		for (batadv_mcast_dest_entry_init_s(state);		\
		     batadv_mcast_dest_entry_check_s(state);		\
		     batadv_mcast_dest_entry_inc_s(state))

struct batadv_mcast_entries_list {
	struct list_head list;
	uint8_t mcast_addr[6];
	struct list_head dest_entries;
};

/**
 * batadv_mcast_tracker_send_delay - Tracker packet scheduling/send delay
 * @bat_priv:	bat_priv for the mesh we are preparing this tracker packet
 *
 * Fetches the configured tracker packet interval, adds some jitter and
 * returns the result.
 */
static int batadv_mcast_tracker_send_delay(struct batadv_priv *bat_priv)
{
	int tracker_interval = atomic_read(&bat_priv->mcast_tracker_interval);

	return msecs_to_jiffies(tracker_interval -
		   BATADV_JITTER + (random32() % 2 * BATADV_JITTER));
}

/**
 * batadv_mcast_tracker_start - Starts the tracker packet scheduler
 * @bat_priv:	bat_priv for the mesh we are preparing this tracker packet
 *
 * Activates the scheduler for the periodic sending of tracker packets.
 *
 * First tracker packet will be sent after the configured interval plus
 * some jitter.
 */
void batadv_mcast_tracker_start(struct batadv_priv *bat_priv)
{
	/* adding some jitter */
	unsigned long tracker_interval = batadv_mcast_tracker_send_delay(
								bat_priv);
	queue_delayed_work(batadv_event_workqueue,
			   &bat_priv->mcast.tracker_work, tracker_interval);
}

/**
 * batadv_mcast_tracker_stop - Stops the tracker packet scheduler
 * @bat_priv:	bat_priv for the mesh we are preparing this tracker packet
 *
 * Deactivates the scheduler for the periodic sending of tracker packets.
 */
void batadv_mcast_tracker_stop(struct batadv_priv *bat_priv)
{
	cancel_delayed_work_sync(&bat_priv->mcast.tracker_work);
}

/**
 * batadv_mcast_tracker_reset - Resets the tracker packet scheduler
 * @net_dev:	net device for the mesh we are preparing this tracker packet
 *
 * Resets the scheduler for the periodic sending of tracker packets.
 *
 * You will probably, especially want to use this when having changed the
 * tracker packet interval from a very high to a rather low one.
 */
void batadv_mcast_tracker_reset(struct net_device *net_dev)
{
	struct batadv_priv *bat_priv = netdev_priv(net_dev);

	batadv_mcast_tracker_stop(bat_priv);
	batadv_mcast_tracker_start(bat_priv);
}

/**
 * batadv_mcast_build_tracker_skb - Allocates an empty tracker packet
 * @tracker_packet_len:	Size of the to be allocated skb (excluding ethhdr)
 * @bat_priv:		bat_priv for the mesh we are preparing this packet
 *
 * This allocates an skb with enough space for a tracker packet of
 * the specified size plus space for an ethernet header.
 *
 * It further initializes the tracker packet header, including our
 * own primary interface mac address and returns this packet.
 *
 * Returns NULL on out-of-memory.
 */
static struct sk_buff *batadv_mcast_build_tracker_skb(int tracker_packet_len,
						struct batadv_priv *bat_priv)
{
	struct sk_buff *skb;
	struct batadv_mcast_tracker_packet *tracker_packet;
	struct batadv_hard_iface *primary_if = NULL;

	skb = dev_alloc_skb(tracker_packet_len + sizeof(struct ethhdr));
	if (!skb)
		goto out;

	primary_if = batadv_primary_if_get_selected(bat_priv);
	if (!primary_if)
		goto out;

	skb_reserve(skb, sizeof(struct ethhdr));
	tracker_packet = (struct batadv_mcast_tracker_packet *)
			 skb_put(skb, tracker_packet_len);

	tracker_packet->header.packet_type = BATADV_MCAST_TRACKER;
	tracker_packet->header.version = BATADV_COMPAT_VERSION;
	tracker_packet->header.ttl = BATADV_TTL;
	memcpy(tracker_packet->orig, bat_priv->primary_if->net_dev->dev_addr,
	       ETH_ALEN);
	tracker_packet->num_mcast_entries = 0;
	memset(tracker_packet->reserved, 0, sizeof(tracker_packet->reserved));

out:
	if (primary_if)
		batadv_hardif_free_ref(primary_if);
	return skb;
}

/**
 * batadv_mcast_tracker_dests_free - Frees a multicast destinations list
 * @mcast_dest_list:	The multicast destinations list to free
 *
 * Frees all multicast destination entries in the provided list.
 */
static void batadv_mcast_tracker_dests_free(struct list_head *dest_entries)
{
	struct batadv_dest_entries_list *dest_entry, *tmp;

	list_for_each_entry_safe(dest_entry, tmp, dest_entries, list) {
		list_del(&dest_entry->list);
		kfree(dest_entry);
	}
}

/**
 * batadv_mcast_tracker_collect_dests - Coll. destinations for a mcast address
 * @bat_priv:		bat_priv for the mesh we are preparing this packet
 * @mcast_entry:	Multicast address entry to save destinations for and in
 *
 * Collects destination addresses from the previously buffered
 * multicast announcements matching the multicast address provided by the given
 * multicast address entry and stores them in this multicast entry.
 */
static int batadv_mcast_tracker_collect_dests(
				struct batadv_priv *bat_priv,
				struct batadv_mcast_entries_list *mcast_entry)
{
	struct batadv_hashtable *hash = bat_priv->orig_hash;
	struct batadv_tt_global_entry *tt_global_entry = NULL;
	struct batadv_tt_orig_list_entry *orig_entry;
	struct hlist_node *walk;
	struct hlist_head *head;
	struct batadv_dest_entries_list *dest_entry;
	int num_dests = 0;

	if (!hash)
		goto out;

	tt_global_entry = batadv_tt_global_hash_find(bat_priv,
						     mcast_entry->mcast_addr);
	if (!tt_global_entry)
		goto out;

	head = &tt_global_entry->orig_list;

	rcu_read_lock();
	hlist_for_each_entry_rcu(orig_entry, walk, head, list) {
		dest_entry = kmalloc(sizeof(struct batadv_dest_entries_list),
				     GFP_ATOMIC);
		if (!dest_entry)
			goto free;

		memcpy(dest_entry->dest, orig_entry->orig_node->orig,
		       ETH_ALEN);
		list_add(&dest_entry->list, &mcast_entry->dest_entries);
		num_dests++;
		break;
	}
	rcu_read_unlock();

	goto out;

free:
	rcu_read_unlock();

	batadv_mcast_tracker_dests_free(&mcast_entry->dest_entries);
	num_dests = 0;

out:
	if (tt_global_entry)
		batadv_tt_global_entry_free_ref(tt_global_entry);

	return num_dests;
}

/**
 * batadv_mcast_tracker_collect_free - Frees a multicast address list
 * @mcast_dest_list:	The multicast address list to free
 *
 * Frees all multicast address entries and their destination entries
 * in the provided list.
 */
static void batadv_mcast_tracker_collect_free(struct list_head *mcast_dest_list)
{
	struct batadv_mcast_entries_list *mcast_entry, *tmp;

	list_for_each_entry_safe(mcast_entry, tmp, mcast_dest_list, list) {
		batadv_mcast_tracker_dests_free(&mcast_entry->dest_entries);
		list_del(&mcast_entry->list);
		kfree(mcast_entry);
	}
}

/**
 * batadv_mcast_addr_add_collect - Allocates & adds a multicast address
 * @mcast_addr:		Multicast address to add to the list
 * @mcast_dest_list:	List to save multicast addresses in
 *
 * This function allocates memory for a multicast address entry, copies the
 * provided multicast address to it and adds this new entry to the given
 * list.
 *
 * Returns -ENOMEM on out-of-memory. Otherwise returns 0.
 */
static int batadv_mcast_addr_add_collect(uint8_t *mcast_addr,
					 struct list_head *mcast_dest_list)
{
	struct batadv_mcast_entries_list *mcast_entry;

	mcast_entry = kmalloc(sizeof(struct batadv_mcast_entries_list),
			      GFP_ATOMIC);
	if (!mcast_entry)
		return -ENOMEM;

	memcpy(mcast_entry->mcast_addr, mcast_addr, ETH_ALEN);
	INIT_LIST_HEAD(&mcast_entry->dest_entries);
	list_add(&mcast_entry->list, mcast_dest_list);

	return 0;
}

/**
 * batadv_mcast_tracker_collect_mcasts - Collects multicast addresses
 * @bat_priv:		bat_priv for the mesh we are preparing this packet
 * @mcast_dest_list:	List to save multicast addresses in
 *
 * This method collects our multicast address candidates from the
 * multicast flow infrastructure, that is multicast addresses from
 * our local soft interface or bridged-in senders which
 * have reached the configured flow threshold, and stores them in
 * the provided list.
 */
static void batadv_mcast_tracker_collect_mcasts(
					struct batadv_priv *bat_priv,
					struct list_head *mcast_dest_list)
{
	struct batadv_mcast_flow_entry *flow_entry;
	struct hlist_node *node;
	int threshold_state;

	rcu_read_lock();
	hlist_for_each_entry_rcu(flow_entry, node,
				 &bat_priv->mcast.flow_table, list) {
		if (!atomic_inc_not_zero(&flow_entry->refcount))
			continue;

		threshold_state = batadv_mcast_flow_update_entry(flow_entry,
								 bat_priv, 0);
		if (threshold_state == BATADV_MCAST_THRESHOLD_LOW) {
			batadv_mcast_flow_entry_free_ref(flow_entry);
			continue;
		}

		if (batadv_mcast_addr_add_collect(flow_entry->mcast_addr,
						  mcast_dest_list) < 0)
			goto free;

		batadv_mcast_flow_entry_free_ref(flow_entry);
	}
	rcu_read_unlock();
	return;

free:
	/* Out-of-memory, free all */
	batadv_mcast_flow_entry_free_ref(flow_entry);
	batadv_mcast_tracker_collect_free(mcast_dest_list);
}

/**
 * batadv_mcast_tracker_collect - Collects destinations for multicast addresses
 * @bat_priv:		bat_priv for the mesh we are preparing this packet
 * @mcast_dest_list:	List of multicast addresses
 *
 * Every multicast address entry in the provided list will be filled with
 * matching destination addresses from previously buffered
 * multicast announcements.
 *
 * Returns 0 if no matching destination address was found. Or returns the
 * needed tracker packet size (tracker packet header plus body) otherwise.
 */
static int batadv_mcast_tracker_collect(struct batadv_priv *bat_priv,
					struct list_head *mcast_dest_list)
{
	struct batadv_mcast_entries_list *mcast_entry, *tmp;
	struct batadv_dest_entries_list *dest_entry;
	int tracker_packet_len = sizeof(struct batadv_mcast_tracker_packet);
	int used_mcast_entries = 0, reduced = 0;

	list_for_each_entry_safe(mcast_entry, tmp, mcast_dest_list, list) {
		tracker_packet_len += sizeof(struct batadv_mcast_entry);
		if (used_mcast_entries == UINT8_MAX ||
		    tracker_packet_len + ETH_ALEN > ETH_DATA_LEN) {
			reduced = 1;
			goto del;
		}

		tracker_packet_len += ETH_ALEN *
			batadv_mcast_tracker_collect_dests(bat_priv,
							   mcast_entry);

		if (list_empty(&mcast_entry->dest_entries)) {
del:
			tracker_packet_len -= sizeof(struct batadv_mcast_entry);
			list_del(&mcast_entry->list);
			kfree(mcast_entry);
			continue;
		}

		while (tracker_packet_len > ETH_DATA_LEN) {
			/* list won't get empty here due to the
			 * previous checks */
			reduced = 1;
			dest_entry = list_first_entry(
					       &mcast_entry->dest_entries,
					       struct batadv_dest_entries_list,
					       list);
			list_del(&dest_entry->list);
			kfree(dest_entry);
			tracker_packet_len -= ETH_ALEN;
		}

		used_mcast_entries++;
	}

	if (!used_mcast_entries)
		tracker_packet_len = 0;
	else if (reduced)
		pr_warn("mcast tracker packet got too large, forcing reduced size of %i Bytes\n",
			tracker_packet_len);

	return tracker_packet_len;
}

/**
 * batadv_mcast_tracker_skb_attach - Fills empty tracker packet
 * @skb:		The empty tracker packet to fill
 * @mcast_dest_list:	List of multicast (+destination) addresses to attach
 *
 * This copies all multicast addresses and their according destination
 * addresses into the body of the given tracker packet (while
 * leaving the original list untouched).
 *
 * It further sets the num_mcast_entries and every multicast entries'
 * num_dest field.
 *
 * Caller needs to ensure sufficient space in the provided skb.
 */
static void batadv_mcast_tracker_skb_attach(struct sk_buff *skb,
					    struct list_head *mcast_dest_list)
{
	struct batadv_mcast_tracker_packet *tracker_packet;
	struct batadv_mcast_entry *mcast_entry;
	struct batadv_mcast_entries_list *mcast;
	struct batadv_dest_entries_list *dest;
	uint8_t *dest_entry;

	tracker_packet = (struct batadv_mcast_tracker_packet *)skb->data;
	mcast_entry = (struct batadv_mcast_entry *)(tracker_packet + 1);

	list_for_each_entry(mcast, mcast_dest_list, list) {
		tracker_packet->num_mcast_entries++;
		mcast_entry->num_dest = 0;
		mcast_entry->reserved = 0;
		memcpy(mcast_entry->mcast_addr, mcast->mcast_addr, ETH_ALEN);
		dest_entry = (uint8_t *)(mcast_entry + 1);

		list_for_each_entry(dest, &mcast->dest_entries, list) {
			mcast_entry->num_dest++;
			memcpy(dest_entry, dest->dest, ETH_ALEN);

			dest_entry += ETH_ALEN;
		}
		mcast_entry = (struct batadv_mcast_entry *)dest_entry;
	}
}

/**
 * batadv_mcast_tracker_prepare - Creates a tracker packet from a list
 * @bat_priv:		bat_priv for the mesh we are preparing this packet
 * @mcast_dest_list:	List of multicast addresses
 *
 * First collects all unicast destination addresses for the given list
 * of multicast addresses.
 *
 * Then converts those lists into a tracker packet (and consumes them).
 *
 * Returns NULL if the provided multicast address list is empty or if
 * no destination addresses for the provided multicast addresses were
 * found. Otherwise returns the created, filled tracker packet.
 */
static struct sk_buff *batadv_mcast_tracker_prepare(
					struct batadv_priv *bat_priv,
					struct list_head *mcast_dest_list)
{
	struct sk_buff *skb = NULL;
	int tracker_packet_len;

	tracker_packet_len = batadv_mcast_tracker_collect(bat_priv,
							  mcast_dest_list);
	if (!tracker_packet_len)
		goto out;

	/* allocate empty tracker packet */
	skb = batadv_mcast_build_tracker_skb(tracker_packet_len, bat_priv);
	if (!skb)
		goto free;

	/* append all collected entries */
	batadv_mcast_tracker_skb_attach(skb, mcast_dest_list);

	/* pending cleanup */
free:
	batadv_mcast_tracker_collect_free(mcast_dest_list);
out:

	return skb;
}

/**
 * batadv_mcast_periodic_tracker_prepare - Creates a complete tracker packet
 * @bat_priv:		bat_priv for the mesh we are preparing this packet
 *
 * Creates a tracker packet with any multicast addresses which
 * met the threshold requirements of the multicast flow functions
 * and their according destinations determined by previously buffered
 * multicast announcements.
 *
 * Returns NULL if no tracker packet destinations were found.
 * Otherwise returns the created, filled tracker packet.
 */
static struct sk_buff *batadv_mcast_periodic_tracker_prepare(
						struct batadv_priv *bat_priv)
{
	struct list_head mcast_dest_list;

	INIT_LIST_HEAD(&mcast_dest_list);
	batadv_mcast_tracker_collect_mcasts(bat_priv, &mcast_dest_list);

	return batadv_mcast_tracker_prepare(bat_priv, &mcast_dest_list);
}

/**
 * batadv_mcast_reactive_tracker_prepare - Creates a specific tracker packet
 * @mcast_addr:	The multicast address to create this packet for
 * @bat_priv:	bat_priv for the mesh we are preparing this packet
 *
 * Creates a tracker packet for the given multicast address with its
 * according destinations determined by the previously buffered multicast
 * listener announcements.
 *
 * Returns NULL if no tracker packet destinations were found.
 * Otherwise returns the created, filled tracker packet.
 */
static struct sk_buff *batadv_mcast_reactive_tracker_prepare(
						uint8_t *mcast_addr,
						struct batadv_priv *bat_priv)
{
	struct list_head mcast_dest_list;

	INIT_LIST_HEAD(&mcast_dest_list);
	batadv_mcast_addr_add_collect(mcast_addr, &mcast_dest_list);

	return batadv_mcast_tracker_prepare(bat_priv, &mcast_dest_list);
}

/**
 * batadv_mcast_add_router_of_dest - Adds the next hop for a destination
 * @next_hops:		The list to add a new next hop to
 * @dest:		The destination to find the next hop for
 * @forw_if_list:	The routing table interface list to add to
 * @bat_priv:		bat_priv context for this mesh network
 *
 * Adds the router for the destination address to the next_hop list and its
 * interface to the forw_if_list - but only if this router has not been
 * added yet.
 */
static int batadv_mcast_add_router_of_dest(
				struct batadv_dest_entries_list *next_hops,
				const uint8_t *dest,
				struct hlist_head *forw_if_list,
				struct batadv_priv *bat_priv)
{
	struct batadv_dest_entries_list *next_hop_tmp, *next_hop_entry;
	struct batadv_orig_node *orig_node = NULL;
	struct batadv_neigh_node *router = NULL;
	int16_t if_num;
	int ret = 1;

	next_hop_entry = kmalloc(sizeof(struct batadv_dest_entries_list),
				 GFP_ATOMIC);
	if (!next_hop_entry)
		goto out;

	orig_node = batadv_orig_hash_find(bat_priv, dest);
	if (!orig_node)
		goto free;

	router = batadv_orig_node_get_router(orig_node);
	if (!router)
		goto free;

	rcu_read_lock();
	if (!router->if_incoming ||
	    !atomic_inc_not_zero(&router->if_incoming->refcount)) {
		rcu_read_unlock();
		goto free;
	}
	next_hop_entry->hard_iface = router->if_incoming;
	if_num = next_hop_entry->hard_iface->if_num;
	rcu_read_unlock();

	memcpy(next_hop_entry->dest, router->addr, ETH_ALEN);

	if (forw_if_list)
		batadv_mcast_forw_if_entry_prep(forw_if_list, if_num,
						next_hop_entry->dest);

	list_for_each_entry(next_hop_tmp, &next_hops->list, list)
		if (!memcmp(next_hop_tmp->dest, next_hop_entry->dest,
			    ETH_ALEN))
			goto kref_free;

	list_add(&next_hop_entry->list, &next_hops->list);

	ret = 0;
	goto out;

kref_free:
	batadv_hardif_free_ref(next_hop_entry->hard_iface);
free:
	kfree(next_hop_entry);
	if (router)
		batadv_neigh_node_free_ref(router);
	if (orig_node)
		batadv_orig_node_free_ref(orig_node);
out:
	return ret;
}

/**
 * batadv_mcast_tracker_next_hops - Determines next hops for a tracker packet
 * @tracker_packet:	The tracker packet we want to examine
 * @tracker_packet_len:	Size of the tracker packet
 * @next_hops:		The list to add any found next hops to
 * @forw_table:		A table for updating the multicast routing table
 * @bat_priv:		bat_priv for the mesh we are preparing this packet for
 *
 * Collects nexthops for all dest entries specified in this tracker packet
 * in a list.
 *
 * It also decrements/"repairs" the fields for the number of elements in the
 * tracker packet if they do not match the actual length of this tracker
 * packet (e.g. because of a received, broken tracker packet).
 *
 * It also collects information from the tracker packet needed for updating our
 * own multicast routing table in the specified forw_table.
 */
static int batadv_mcast_tracker_next_hops(
			struct batadv_mcast_tracker_packet *tracker_packet,
			int tracker_packet_len,
			struct batadv_dest_entries_list *next_hops,
			struct hlist_head *forw_table,
			struct batadv_priv *bat_priv)
{
	int num_next_hops = 0, ret;
	struct batadv_tracker_packet_state state;
	uint8_t *tail = (uint8_t *)tracker_packet + tracker_packet_len;
	struct hlist_head *forw_table_if = NULL;

	INIT_LIST_HEAD(&next_hops->list);
	INIT_HLIST_HEAD(forw_table);

	batadv_tracker_packet_for_each_dest(&state, tracker_packet) {
		/* avoid writing outside of unallocated memory later */
		if (state.dest_entry + ETH_ALEN > tail) {
			batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
				   "mcast tracker packet is broken, too many "
				   "entries claimed for its length, "
				   "repairing");

			tracker_packet->num_mcast_entries = state.mcast_num;

			if (state.dest_num) {
				tracker_packet->num_mcast_entries++;
				state.mcast_entry->num_dest = state.dest_num;
			}

			break;
		}

		if (state.dest_num)
			goto skip;

		forw_table_if = batadv_mcast_forw_table_entry_prep(
						forw_table,
						state.mcast_entry->mcast_addr,
						tracker_packet->orig);
skip:

		ret = batadv_mcast_add_router_of_dest(next_hops,
						      state.dest_entry,
						      forw_table_if,
						      bat_priv);
		if (!ret)
			num_next_hops++;
	}

	return num_next_hops;
}

/**
 * batadv_mcast_zero_tracker_packet - Zero destination entries
 * @tracker_packet:	The tracker packet we want to modify
 * @next_hop:		The address of a next hop
 * @bat_priv:		bat_priv for the mesh we are preparing this packet for
 *
 * Zeros destination entries (that is sets the address of an entry to
 * the MAC 00:00:00:00:00:00) in the tracker packet for destinations not
 * matching the specified next hop.
 */
static void batadv_mcast_zero_tracker_packet(
			struct batadv_mcast_tracker_packet *tracker_packet,
			uint8_t *next_hop,
			struct batadv_priv *bat_priv)
{
	struct batadv_tracker_packet_state state;
	struct batadv_orig_node *orig_node;
	struct batadv_neigh_node *router;

	batadv_tracker_packet_for_each_dest(&state, tracker_packet) {
		router = NULL;
		orig_node = batadv_orig_hash_find(bat_priv, state.dest_entry);
		/* we don't know this destination */
		if (!orig_node)
			goto erase;

		/* is the next hop already our destination? */
		if (!memcmp(orig_node->orig, next_hop, ETH_ALEN))
			goto erase;

		router = batadv_orig_node_get_router(orig_node);
		if (!router)
			goto erase;

		if (!memcmp(router->orig_node->primary_addr,
			    orig_node->orig, ETH_ALEN) ||
		    /* is this the wrong next hop for our
		     * destination? */
		    memcmp(router->addr, next_hop, ETH_ALEN))
			goto erase;

		goto free;
erase:
		memset(state.dest_entry, '\0', ETH_ALEN);
free:
		if (orig_node)
			batadv_orig_node_free_ref(orig_node);
		if (router)
			batadv_neigh_node_free_ref(router);
	}
}

/**
 * batadv_mcast_shrink_tracker_packet - Removes zeroed destination entries
 * @skb:	The tracker packet we want to modify
 *
 * Removes zeroed destination entries (that is an entry with a
 * 00:00:00:00:00:00 MAC) and empty multicast entries (that is multicast
 * entries with no (more) destination entries) afterwards in the given tracker
 * packet.
 *
 * Caller needs to ensure that the number fields in the tracker packet
 * are sane.
 */
static void batadv_mcast_shrink_tracker_packet(struct sk_buff *skb)
{
	struct batadv_mcast_tracker_packet *tracker_packet =
				(struct batadv_mcast_tracker_packet *)skb->data;
	struct batadv_tracker_packet_state state;
	unsigned char *tail = skb_tail_pointer(skb);
	int new_tracker_packet_len = sizeof(struct batadv_mcast_tracker_packet);

	batadv_tracker_packet_for_each_dest(&state, tracker_packet) {
		if (memcmp(state.dest_entry, "\0\0\0\0\0\0", ETH_ALEN)) {
			new_tracker_packet_len += ETH_ALEN;
			continue;
		}

		memmove(state.dest_entry, state.dest_entry + ETH_ALEN,
			tail - state.dest_entry - ETH_ALEN);

		state.mcast_entry->num_dest--;
		tail -= ETH_ALEN;

		if (state.mcast_entry->num_dest) {
			state.dest_num--;
			state.dest_entry -= ETH_ALEN;
			continue;
		}

		/* = mcast_entry */
		state.dest_entry -= sizeof(struct batadv_mcast_entry);

		memmove(state.dest_entry, state.dest_entry +
			sizeof(struct batadv_mcast_entry),
			tail - state.dest_entry -
			sizeof(struct batadv_mcast_entry));

		tracker_packet->num_mcast_entries--;
		tail -= sizeof(struct batadv_mcast_entry);

		state.mcast_num--;

		/* Avoid mcast_entry check of
		 * batadv_tracker_packet_for_each_dest's inner loop */
		state.break_flag = 0;
		break;
	}

	new_tracker_packet_len += sizeof(struct batadv_mcast_entry) *
				  tracker_packet->num_mcast_entries;

	skb_trim(skb, new_tracker_packet_len);
}

/**
 * batadv_mcast_tracker_dec_ttl - Decreases the TTL of a tracker packet
 * @packet:	A multicast tracker packet
 *
 * If the time-to-live of the given tracker packet is greater than one
 * then this decreases this TTL by one and returns 1.
 *
 * Otherwise returns 0 (without decrementing the TTL).
 */
static int batadv_mcast_tracker_dec_ttl(
				struct batadv_mcast_tracker_packet *packet)
{
	if (packet->header.ttl - 1 <= 0)
		return 0;

	packet->header.ttl--;
	return 1;
}

/**
 * batadv_mcast_tracker_packet_route - Split and send tracker packet
 * @skb:	A compact multicast tracker packet with all groups and
 *		destinations attached.
 * @bat_priv:	bat_priv for the mesh we are routing this tracker packet
 * @num_redundancy:	Number of extra packets to send
 *
 * This function iterates over the destination entries of the given
 * tracker packet and sorts them into new tracker packets matching
 * their next hops according to the unicast routing algorithm.
 *
 * It then sends those new tracker packets, this partition of the original
 * tracker packet, to their according next hop num_redundancy plus one
 * times each.
 *
 * Finally it also updates its own multicast routing table with the
 * information gained from incoming tracker packet.
 */
void batadv_mcast_tracker_packet_route(struct sk_buff *skb,
				       struct batadv_priv *bat_priv,
				       int num_redundancy)
{
	struct batadv_dest_entries_list next_hops, *tmp;
	struct batadv_dest_entries_list *next_hop;
	struct hlist_head forw_table;
	struct sk_buff *skb_tmp, *skb_cloned;
	int i, num_next_hops;

	num_next_hops = batadv_mcast_tracker_next_hops(
				(struct batadv_mcast_tracker_packet *)skb->data,
				skb->len, &next_hops, &forw_table, bat_priv);
	if (!num_next_hops)
		return;

	batadv_mcast_forw_table_update(&forw_table, bat_priv);

	if (!batadv_mcast_tracker_dec_ttl(
			(struct batadv_mcast_tracker_packet *)skb->data))
		return;

	list_for_each_entry(next_hop, &next_hops.list, list) {
		skb_tmp = skb_copy(skb, GFP_ATOMIC);
		if (!skb_tmp)
			goto free;

		/* cut the tracker packets for the according destinations */
		batadv_mcast_zero_tracker_packet(
				(struct batadv_mcast_tracker_packet *)
				skb_tmp->data, next_hop->dest, bat_priv);
		batadv_mcast_shrink_tracker_packet(skb_tmp);
		if (skb_tmp->len ==
		    sizeof(struct batadv_mcast_tracker_packet)) {
			dev_kfree_skb(skb_tmp);
			continue;
		}

		for (i = 0; i < num_redundancy; i++) {
			skb_cloned = skb_clone(skb_tmp, GFP_ATOMIC);
			if (!skb_cloned)
				break;

			batadv_send_skb_packet(skb_cloned,
					       next_hop->hard_iface,
					       next_hop->dest);
		}

		/* Send 'em! */
		batadv_send_skb_packet(skb_tmp, next_hop->hard_iface,
				       next_hop->dest);
	}

free:
	list_for_each_entry_safe(next_hop, tmp, &next_hops.list, list) {
		batadv_hardif_free_ref(next_hop->hard_iface);
		list_del(&next_hop->list);
		kfree(next_hop);
	}
}

/**
 * batadv_mcast_tracker_timer - Creates and sends tracker packets (+ rearms)
 * @work:	The work/timer/bat_priv context we got called from
 *
 * If multicast optimization is enabled then this creates and sends
 * a new tracker packet.
 *
 * Afterwards it reschedules itself for after the currently configured tracker
 * interval.
 */
void batadv_mcast_tracker_timer(struct work_struct *work)
{
	struct batadv_priv_mcast *priv_mcast =
					container_of(work,
						     struct batadv_priv_mcast,
						     tracker_work.work);
	struct batadv_priv *bat_priv = container_of(priv_mcast,
						    struct batadv_priv, mcast);
	struct sk_buff *tracker_packet = NULL;

	if (atomic_read(&bat_priv->mcast_group_awareness))
		tracker_packet = batadv_mcast_periodic_tracker_prepare(
								bat_priv);

	if (!tracker_packet)
		goto out;

	batadv_mcast_tracker_packet_route(tracker_packet, bat_priv, 0);
	dev_kfree_skb(tracker_packet);

out:
	/* Reschedule */
	batadv_mcast_tracker_start(bat_priv);
}

/**
 * batadv_mcast_tracker_burst - Creates and sends a burst of tracker packets
 * @mcast_addr:	The multicast address to create these packets for
 * @bat_priv:	bat_priv for the mesh we are preparing this tracker packet
 *
 * This method creates a tracker packet for the given multicast address,
 * splits it for their according next hops and their interfaces and
 * finally transmits them with a few redundant ones each.
 */
void batadv_mcast_tracker_burst(uint8_t *mcast_addr,
				struct batadv_priv *bat_priv)
{
	struct sk_buff *tracker_packet;

	tracker_packet = batadv_mcast_reactive_tracker_prepare(mcast_addr,
							       bat_priv);
	if (!tracker_packet)
		return;

	batadv_mcast_tracker_packet_route(tracker_packet, bat_priv,
					  TRACKER_BURST_EXTRA);
	dev_kfree_skb(tracker_packet);
}
