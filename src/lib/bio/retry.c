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
 * @file lib/bio/retry.c
 * @brief Binary IO abstractions for retrying packets.
 *
 * @copyright 2024 Network RADIUS SAS (legal@networkradius.com)
 */

#include <freeradius-devel/bio/bio_priv.h>
#include <freeradius-devel/bio/null.h>
#include <freeradius-devel/bio/buf.h>
#include <freeradius-devel/util/rb.h>
#include <freeradius-devel/util/dlist.h>

#define _BIO_RETRY_PRIVATE
#include <freeradius-devel/bio/retry.h>

typedef struct fr_bio_retry_list_s	fr_bio_retry_list_t;
typedef struct fr_bio_retry_s	fr_bio_retry_t;

/*
 *	Define type-safe wrappers for head and entry definitions.
 */
FR_DLIST_TYPES(fr_bio_retry_list)

struct fr_bio_retry_entry_s {
	void		*uctx;
	void		*packet_ctx;
	fr_bio_retry_rewrite_t rewrite;		//!< per-packet rewrite callback
	void		*rewrite_ctx;		//!< context specifically for rewriting this packet

	fr_retry_t	retry;			//!< retry timers and counters

	union {
		fr_rb_node_t	timer_node;		//!< for the timers
		FR_DLIST_ENTRY(fr_bio_retry_list) entry; //!< for the free list
	};
	fr_rb_node_t	expiry_node;		//!< for the timers

	fr_bio_retry_t	*my;			//!< so we can get to it from the event timer callback

	uint8_t const	*buffer;
	size_t		size;	

	bool		cancelled;		//!< was this item cancelled?
	bool		reserved;		//!< for application-layer watchdog
};

FR_DLIST_FUNCS(fr_bio_retry_list, fr_bio_retry_entry_t, entry)

struct fr_bio_retry_s {
	FR_BIO_COMMON;

	fr_event_list_t		*el;
	fr_rb_tree_t		timer_tree;
	fr_rb_tree_t		expiry_tree;

	fr_bio_retry_info_t	info;

	fr_retry_config_t	retry_config;

	ssize_t			error;

	fr_event_timer_t const	*ev;

	/*
	 *	The first item is cached here so that we can detect when it changes.  The insert / delete
	 *	code can just do its work without worrying about timers.  And then when the tree manipulation
	 *	is done, call the fr_bio_retry_timer_reset() function to reset (or not) the timer.
	 */
	fr_bio_retry_entry_t	*timer_item;		//!< for timers

	/*
	 *	Cache a partial write when IO is blocked.  Partial
	 *	packets are left in the timer tree so that they can be expired.
	 */
	fr_bio_retry_entry_t	*partial;	//!< for partial writes

	fr_bio_retry_sent_t	sent;
	fr_bio_retry_rewrite_t	rewrite;
	fr_bio_retry_response_t	response;
	fr_bio_retry_release_t	release;

	fr_bio_buf_t		buffer;

	FR_DLIST_HEAD(fr_bio_retry_list) free;
};

static void fr_bio_retry_timer(UNUSED fr_event_list_t *el, fr_time_t now, void *uctx);
static ssize_t fr_bio_retry_write(fr_bio_t *bio, void *packet_ctx, void const *buffer, size_t size);
static ssize_t fr_bio_retry_blocked(fr_bio_retry_t *my, fr_bio_retry_entry_t *item, ssize_t rcode);

#define fr_bio_retry_timer_clear(_x) do { \
		talloc_const_free((_x)->ev); \
		(_x)->timer_item = NULL; \
	} while (0)

/** Reset the timer after changing the rb tree.
 *
 */
static int fr_bio_retry_timer_reset(fr_bio_retry_t *my)
{
	fr_bio_retry_entry_t *first;

	/*
	 *	Nothing to do, don't set any timers.
	 */
	first = fr_rb_first(&my->timer_tree);
	if (!first) {
	cancel_timer:
		fr_bio_retry_timer_clear(my);
		return 0;
	}

	/*
	 *	We're partially writing a response.  Don't bother with the timer, and delete any existing
	 *	timer.  It will be reset when the partial entry is placed back into the queue.
	 */
	if (first == my->partial) goto cancel_timer;

	/*
	 *	The timer is already set correctly, we're done.
	 */
	if (first == my->timer_item) return 0;

	/*
	 *	Update the timer.  This should never fail.
	 */
	if (fr_event_timer_at(my, my->el, &my->ev, first->retry.next, fr_bio_retry_timer, my) < 0) return -1;

	my->timer_item = first;
	return 0;
}

/** Release an entry back to the free list.
 *
 */
static void fr_bio_retry_release(fr_bio_retry_t *my, fr_bio_retry_entry_t *item, fr_bio_retry_release_reason_t reason)
{
	/*
	 *	Remove the item before calling the application "release" function.
	 */
	if (my->partial != item) {
		if (!item->reserved) {
			(void) fr_rb_remove_by_inline_node(&my->timer_tree, &item->timer_node);
			(void) fr_rb_remove_by_inline_node(&my->expiry_tree, &item->expiry_node);
		}
	} else {
		item->cancelled = true;
	}

	/*
	 *	Tell the caller that we've released it before doing anything else.  That way we can safely
	 *	modify anything we want.
	 */
	my->release((fr_bio_t *) my, item, reason);

	/*
	 *	We've partially written this item.  Don't bother changing it's position in any of the lists,
	 *	as it's in progress.
	 */
	if (my->partial == item) return;

	/*
	 *	We're deleting the timer entry.  Go reset the timer.
	 */
	if (my->timer_item == item) {
		my->timer_item = NULL;
		(void) fr_bio_retry_timer_reset(my);
	}

	item->packet_ctx = NULL;

	fr_assert(my->timer_item != item);
	fr_bio_retry_list_insert_head(&my->free, item);
}

/** Write one item.
 *
 * @return
 *	- <0 on error
 *	- 0 for "can't write any more"
 *	- 1 for "wrote a packet"
 */
static int fr_bio_retry_write_item(fr_bio_retry_t *my, fr_bio_retry_entry_t *item, fr_time_t now)
{
	ssize_t rcode;
	fr_retry_state_t state;

	fr_assert(!my->partial);
	fr_assert(!item->reserved);

	/*
	 *	Are we there yet?
	 *
	 *	Release it, indicating whether or not we successfully got a reply.
	 */
	state = fr_retry_next(&item->retry, now);
	if (state != FR_RETRY_CONTINUE) {
		fr_bio_retry_release(my, item, (item->retry.replies > 0) ? FR_BIO_RETRY_DONE : FR_BIO_RETRY_NO_REPLY);
		return 1;
	}

	/*
	 *	Track when we last sent a NEW packet.  Also track when we first sent a packet after becoming
	 *	active again.
	 */
	if ((item->retry.count == 1) && fr_time_lt(my->info.last_sent, now)) {
		my->info.last_sent = now;

		if (fr_time_lteq(my->info.first_sent, my->info.last_idle)) my->info.first_sent = now;
	}

	/*
	 *	Write out the packet.  On failure release this item.
	 *
	 *	If there's an error, we hope that the next "real" write will find the error, and do any
	 *	necessary cleanups.  Note that we can't call bio shutdown here, as the bio is controlled by the
	 *	application, and not by us.
	 */
	if (item->rewrite) {
		rcode = item->rewrite(&my->bio, item, item->buffer, item->size);
	} else {
		rcode = my->rewrite(&my->bio, item, item->buffer, item->size);
	}
	if (rcode < 0) {
		if (rcode == fr_bio_error(IO_WOULD_BLOCK)) return rcode;

		fr_bio_retry_release(my, item, FR_BIO_RETRY_WRITE_ERROR);
		return rcode;
	}

	/*
	 *	We didn't write the whole packet, we're blocked.
	 */
	if ((size_t) rcode < item->size) {
		if (fr_bio_retry_blocked(my, item, rcode) < 0) return fr_bio_error(GENERIC); /* oom */

		return 0;
	}

	/*
	 *	We wrote the whole packet.  Re-insert it, which is done _without_ doing calls to
	 *	cmp(), so we it's OK for us to rewrite item->retry.next.
	 */
	(void) fr_rb_remove_by_inline_node(&my->timer_tree, &item->timer_node);
	(void) fr_rb_insert(&my->timer_tree, item);

	return 1;
}

/*
 *	Check for the "next next" retry.  If that's still in the past,
 *	then skip it.  But _don't_ update retry.count, as we don't
 *	send packets.  Instead, just enforce MRD, etc.
 */
static int fr_bio_retry_write_delayed(fr_bio_retry_t *my, fr_time_t now)
{
	fr_bio_retry_entry_t *item;

	/*
	 *	We can't be in this function if there's a partial packet.  We must be in
	 *	fr_bio_retry_write_partial().
	 */
	fr_assert(!my->partial);

	while ((item = fr_rb_first(&my->timer_tree)) != NULL) {
		int rcode;

		/*
		 *	This item needs to be sent in the future, we're done.
		 */
		if (fr_time_cmp(now, item->retry.next) > 0) break;

		/*
		 *	Write one item, and don't update timers.
		 */
		rcode = fr_bio_retry_write_item(my, item, now);
		if (rcode <= 0) return rcode;
	}

	/*
	 *	Now that we've written multiple items, reset the timer.
	 *
	 *	We do this at the end of the loop so that we don't update it for each item in the loop.
	 *
	 *	@todo - set generic write error?
	 */
	(void) fr_bio_retry_timer_reset(my);

	return 1;
}


/** There's a partial packet written.  Write all of that one first, before writing another packet.
 *
 *  The packet can either be cancelled, or IO blocked.  In either case, we must write the full packet before
 *  going on to the next one, OR retrying another packet.
 */
static ssize_t fr_bio_retry_write_partial(fr_bio_t *bio, void *packet_ctx, const void *buffer, size_t size)
{
	size_t used;
	ssize_t rcode;
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);
	fr_bio_t *next;
	fr_bio_retry_entry_t *item = my->partial;

	fr_assert(!my->timer_item);
	fr_assert(!my->ev);
	fr_assert(my->partial != NULL);
	fr_assert(my->buffer.start);

	used = fr_bio_buf_used(&my->buffer);
	fr_assert(used > 0);

	/*
	 *	There must be a next bio.
	 */
	next = fr_bio_next(&my->bio);
	fr_assert(next != NULL);

	rcode = next->write(next, NULL, my->buffer.read, used);
	if (rcode <= 0) return rcode;

	my->buffer.read += rcode;

	/*
	 *	Still data in the buffer.  We can't send more packets until we finished writing this one.
	 */
	if (fr_bio_buf_used(&my->buffer) > 0) return 0;

	/*
	 *	We're done.  Reset the buffer and clean up our cached partial packet.
	 */
	fr_bio_buf_reset(&my->buffer);
	my->partial = NULL;

	/*
	 *	The item was cancelled, which means it's no longer in the timer tree.
	 *
	 *	If it's not cancelled, then we leave it in the tree, and run its timers s normal.
	 */
	if (item->cancelled) {
		item->packet_ctx = NULL;

		fr_bio_retry_list_insert_head(&my->free, item);
	}

	rcode = fr_bio_retry_write_resume(&my->bio);
	if (rcode < 0) return rcode;

	/*
	 *	Couldn't resume writes.
	 */
	if (rcode == 0) {
		my->bio.write = fr_bio_null_write;
		return 0;
	}

	/*
	 *	Try to write the packet which we were given.
	 */
	my->bio.write = fr_bio_retry_write;
	return fr_bio_retry_write(bio, packet_ctx, buffer, size);
}

/** The write is blocked.
 *
 *  We couldn't write out the entire packet, the bio is blocked.  Don't write anything else until we become
 *  unblocked!
 *
 *  And free the timer.  There's no point in trying to write things if the socket is blocked.
 */
static ssize_t fr_bio_retry_blocked(fr_bio_retry_t *my, fr_bio_retry_entry_t *item, ssize_t rcode)
{
	fr_assert(!my->partial);
	fr_assert(rcode > 0);
	fr_assert((size_t) rcode < item->size);

	/*
	 *	(re)-alloc the buffer for partial writes.
	 */
	if (!my->buffer.start ||
	    (item->size > fr_bio_buf_size(&my->buffer))) {
		if (fr_bio_buf_alloc(my, &my->buffer, item->size)) return fr_bio_error(GENERIC);
	}

	fr_assert(fr_bio_buf_used(&my->buffer) == 0);
	fr_assert(my->buffer.read == my->buffer.start);

	fr_bio_buf_write(&my->buffer, item->buffer + rcode, item->size - rcode);

	my->partial = item;
	my->info.write_blocked = true;

	/*
	 *	There's no timer, as the write is blocked, so we can't retry.
	 */
	fr_bio_retry_timer_clear(my);

	my->bio.write = fr_bio_retry_write_partial;

	/*
	 *	We leave the entry in the timer tree so that the expiry timer will get hit.
	 *
	 *	And then return the size of the partial data we wrote.
	 */
	return rcode;
}


/**  Resend a packet.
 *
 *  This function should be called by the rewrite() callback, after (possibly) re-encoding the packet.
 *
 *  @param bio		the binary IO handler
 *  @param item		the retry context from #fr_bio_retry_sent_t
 *  @param buffer	raw data for the packet.  May be NULL, in which case the previous packet is retried
 *  @param size		size of the raw data
 *  @return
 *	- <0 on error
 *	- 0 for "wrote no data"
 *	- >0 for "wrote data".
 */
ssize_t fr_bio_retry_rewrite(fr_bio_t *bio, fr_bio_retry_entry_t *item, const void *buffer, size_t size)
{
	ssize_t rcode;
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);
	fr_bio_t *next;

	/*
	 *	The caller may (accidentally or intentionally) call this function when there's a partial
	 *	packet.  The intention for rewrite() is that it is only called from timers, and those only run
	 *	when the socket isn't blocked.  But the caller might not pay attention to those issues.
	 */
	if (my->partial) return 0;
	
	/*
	 *	There must be a next bio.
	 */
	next = fr_bio_next(&my->bio);
	fr_assert(next != NULL);

	/*
	 *	The caller should pass NULL for "use the previous packet".
	 */
	if (buffer) {
		item->buffer = buffer;
		item->size = size;
	}

	/*
	 *	Write out the packet, if everything is OK, return.
	 *
	 *	Note that we don't update any timers if the write succeeded.  That is handled by the caller.
	 */
	rcode = next->write(next, item->packet_ctx, item->buffer, item->size);
	if ((size_t) rcode == size) return rcode;

	/*
	 *	Can't write anything, be sad.
	 */
	if (rcode == 0) return 0;

	/*
	 *	There's an error writing the packet.  Release it, and move the item to the free list.
	 *
	 *	Note that we don't bother resetting the timer.  There's no point in changing the timer when
	 *	the bio is likely dead.
	 */
	if (rcode < 0) {
		if (rcode == fr_bio_error(IO_WOULD_BLOCK)) return rcode;

		fr_bio_retry_release(my, item, FR_BIO_RETRY_WRITE_ERROR);
		return rcode;
	}

	/*
	 *	We had previously written the packet, so save the re-sent one, too.
	 */
	return fr_bio_retry_blocked(my, item, rcode);
}

/** A previous timer write had a fatal error, so we forbid further writes.
 *
 */
static ssize_t fr_bio_retry_write_fatal(fr_bio_t *bio, UNUSED void *packet_ctx, UNUSED void const *buffer, UNUSED size_t size)
{
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);
	ssize_t rcode = my->error;

	my->error = 0;
	my->bio.write = fr_bio_null_write;

	return rcode;
}

/** Run a timer event.  Usually to write out another packet.
 *
 */
static void fr_bio_retry_timer(UNUSED fr_event_list_t *el, fr_time_t now, void *uctx)
{
	ssize_t rcode;
	fr_bio_retry_t *my = talloc_get_type_abort(uctx, fr_bio_retry_t);
	fr_bio_retry_entry_t *item;

	/*
	 *	For the timer to be running, there must be a "first" entry which causes the timer to fire.
	 *
	 *	There must also be no partially written entry.  If the IO is blocked, then all timers are
	 *	suspended.
	 */
	fr_assert(my->timer_item != NULL);
	fr_assert(my->partial == NULL);

	item = my->timer_item;
	my->timer_item = NULL;

	/*
	 *	Retry one item.
	 */
	rcode = fr_bio_retry_write_item(my, item, now);
	if (rcode < 0) {
		if (rcode == fr_bio_error(IO_WOULD_BLOCK)) return;

		my->error = rcode;
		my->bio.write = fr_bio_retry_write_fatal;
		return;
	}

	/*
	 *	Partial write - no timers get set.  We need to wait until the descriptor is writable.
	 */
	if (rcode == 0) {
		fr_assert(my->partial != NULL);
		return;
	}

	/*
	 *	We successfull wrote this item.  Reset the timer to the next one, which is likely to be a
	 *	different one from the item we just updated.
	 */
	(void) fr_bio_retry_timer_reset(my);
}

/** Write a request, and see if we have a reply.
 *
 */
static ssize_t fr_bio_retry_write(fr_bio_t *bio, void *packet_ctx, void const *buffer, size_t size)
{
	ssize_t rcode;
	fr_bio_retry_entry_t *item;
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);
	fr_bio_t *next;

	fr_assert(!my->partial);

	/*
	 *	There must be a next bio.
	 */
	next = fr_bio_next(&my->bio);
	fr_assert(next != NULL);
	
	/*
	 *	The caller is trying to flush partial data.  But we don't have any partial data, so just call
	 *	the next bio to flush it.
	 */
	if (!buffer) {
		return next->write(next, packet_ctx, NULL, size);
	}

	/*
	 *	Catch the corner case where the max number of saved packets is exceeded.
	 */
	if (fr_bio_retry_list_num_elements(&my->free) == 0) {
		item = fr_rb_last(&my->timer_tree);

		fr_assert(item != NULL);

		if (!item->retry.replies) return fr_bio_error(BUFFER_FULL);

		if (fr_bio_retry_entry_cancel(bio, item) < 0) return fr_bio_error(BUFFER_FULL);

		/*
		 *	We now have a free item, so we can use it.
		 */
		fr_assert(fr_bio_retry_list_num_elements(&my->free) > 0);
	}

	/*
	 *	Write out the packet.  If there's an error, OR we wrote nothing, return.
	 *
	 *	Note that we don't mark the socket as blocked if the next bio didn't write anything.  We want
	 *	the caller to know that the write didn't succeed, and the caller takes care of managing the
	 *	current packet.  So there's no need for us to do that.
	 */
	rcode = next->write(next, packet_ctx, buffer, size);
	if (rcode <= 0) return rcode;

	/*
	 *	Initialize the retry timers after writing the packet.
	 */
	item = fr_bio_retry_list_pop_head(&my->free);
	fr_assert(item != NULL);

	fr_assert(item->my == my);
	*item = (fr_bio_retry_entry_t) {
		.my = my,
		.retry.start = fr_time(),
		.packet_ctx = packet_ctx,
		.buffer = buffer,
		.size = size,
	};

	/*
	 *	Tell the application that we've saved the packet.  The "item" pointer allows the application
	 *	to cancel this packet if necessary.
	 */
	my->sent(bio, packet_ctx, buffer, size, item);

	if (!item->retry.config) {
		fr_retry_init(&item->retry, item->retry.start, &my->retry_config);
	}

	/*
	 *	This should never fail.
	 */
	(void) fr_rb_insert(&my->timer_tree, item);
	(void) fr_rb_insert(&my->expiry_tree, item);

	/*
	 *	We only wrote part of the packet, remember to write the rest of it.
	 */
	if ((size_t) rcode < size) {
		return fr_bio_retry_blocked(my, item, rcode);
	}

	/*
	 *	We've just inserted this packet into the timer tree, so it can't be used as the current timer.
	 *	Once we've inserted it, we update the timer.
	 */
	fr_assert(my->timer_item != item);

	/*
	 *	If we can't set the timer, then release this item.
	 */
	if (fr_bio_retry_timer_reset(my) < 0) {
		fr_strerror_const("Failed adding timer for packet");

		fr_bio_retry_release(my, item, FR_BIO_RETRY_CANCELLED);
		return fr_bio_error(GENERIC);
	}

	return size;
}

static ssize_t fr_bio_retry_read(fr_bio_t *bio, void *packet_ctx, void *buffer, size_t size)
{
	ssize_t rcode;
	fr_bio_retry_entry_t *item;
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);
	fr_bio_t *next;

	/*
	 *	There must be a next bio.
	 */
	next = fr_bio_next(&my->bio);
	fr_assert(next != NULL);

	/*
	 *	Read the packet.  If error or nothing, return immediately.
	 */
	rcode = next->read(next, packet_ctx, buffer, size);
	if (rcode <= 0) return rcode;

	/*
	 *	Not a valid response to a request, OR a duplicate response to a request: don't return it to
	 *	the caller.
	 *
	 *	But if it is a duplicate response, update the counters and do cleanups as necessary.
	 */
	item = NULL;
	if (!my->response(bio, &item, packet_ctx, buffer, size)) {
		if (!item) return 0;

		item->retry.replies++;

		/*
		 *	We have enough replies.  Release it.
		 */
		if ((item->retry.replies >= item->retry.count) || !fr_time_delta_ispos(my->retry_config.mrd)) {
			fr_bio_retry_release(my, item, FR_BIO_RETRY_DONE);
		}

		return 0;
	}

	fr_assert(item != NULL);
	fr_assert(item->retry.replies == 0);
	fr_assert(item != my->partial);
       
	/*
	 *	Track when the "most recently sent" packet has a reply.  This metric is better than most
	 *	others for judging the liveliness of the destination.
	 */
	if (fr_time_lt(my->info.mrs_time, item->retry.start)) my->info.mrs_time = item->retry.start;

	/*
	 *	We have a new reply, remember when that happened.  Note that we don't update this timer for
	 *	duplicate replies, but perhaps we should?
	 */
	my->info.last_reply = fr_time();

	/*
	 *	We have a new reply.  If we've received all of the replies (i.e. one), OR we don't have a
	 *	maximum lifetime for this request, then release it immediately.
	 */
	item->retry.replies++;

	/*
	 *	We don't retry application-layer watchdog packets.  And we don't run timers for them.  The
	 *	application is responsible for managing those timers itself.
	 */
	if (item->reserved) return rcode;

	/*
	 *	There are no more packets to send, so this connection is idle.
	 *
	 *	Note that partial packets aren't tracked in the timer tree.  We can't do retransmits until the
	 *	socket is writable.
	 */
	if (fr_bio_retry_outstanding((fr_bio_t *) my) == 1) my->info.last_idle = my->info.last_reply;

	/*
	 *	We have enough replies.  Release it.
	 */
	if ((item->retry.replies >= item->retry.count) || !fr_time_delta_ispos(my->retry_config.mrd)) {
		fr_bio_retry_release(my, item, FR_BIO_RETRY_DONE);
		return rcode;
	}

	/*
	 *	There are more replies pending.  Wait passively for more replies, and clean up the item
	 *	when the timer has expired.
	 */
	item->retry.next = fr_time_add_time_delta(item->retry.start, my->retry_config.mrd);

	(void) fr_rb_remove_by_inline_node(&my->timer_tree, &item->timer_node);
	(void) fr_rb_insert(&my->timer_tree, item);
	(void) fr_bio_retry_timer_reset(my);

	return rcode;
}

static int8_t _timer_cmp(void const *one, void const *two)
{
	fr_bio_retry_entry_t const *a = one;
	fr_bio_retry_entry_t const *b = two;

	fr_assert(a->buffer);
	fr_assert(b->buffer);

	return fr_time_cmp(a->retry.next, b->retry.next);
}

static int8_t _expiry_cmp(void const *one, void const *two)
{
	fr_bio_retry_entry_t const *a = one;
	fr_bio_retry_entry_t const *b = two;

	fr_assert(a->buffer);
	fr_assert(b->buffer);

	return fr_time_cmp(a->retry.end, b->retry.end);
}

/** Cancel one item.
 *
 *  If "item" is NULL, the last entry in the timer tree is cancelled.
 *
 *  @param bio		the binary IO handler
 *  @param item		the retry context from #fr_bio_retry_sent_t
 *  @return
 *	- <0 error
 *	- 0 - didn't cancel
 *	- 1 - did cancel
 */
int fr_bio_retry_entry_cancel(fr_bio_t *bio, fr_bio_retry_entry_t *item)
{
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);

	/*
	 *	No item passed, try to cancel the oldest one.
	 */
	if (!item) {
		item = fr_rb_last(&my->timer_tree);
		if (!item) return 0;

		/*
		 *	This item hasn't had a response, we can't cancel it.
		 */
		if (!item->retry.replies) return 0;
	}

	/*
	 *	If the caller has cached a previously finished item, then that's a fatal error.
	 */
	fr_assert(item->buffer != NULL);

	fr_bio_retry_release(my, item, (item->retry.replies > 0) ? FR_BIO_RETRY_DONE : FR_BIO_RETRY_CANCELLED);

	return 1;
}

/**  Set a per-packet retry config 
 *
 *  This function should be called from the #fr_bio_retry_sent_t callback to set a unique retry timer for this
 *  packet.  If no retry configuration is set, then the main one from the alloc() function is used.
 */
int fr_bio_retry_entry_start(UNUSED fr_bio_t *bio, fr_bio_retry_entry_t *item, fr_retry_config_t const *cfg)
{
	fr_assert(item->buffer != NULL);

	if (item->retry.config) return -1;

	fr_assert(fr_time_delta_unwrap(cfg->irt) != 0);

	fr_retry_init(&item->retry, item->retry.start, cfg);

	return 0;
}

/**  Allow the callbacks / application to know when things are being retried.
 *
 *  This is not initialized util _after_ fr_bio_retry_entry_start() has been called.
 */
const fr_retry_t *fr_bio_retry_entry_info(UNUSED fr_bio_t *bio, fr_bio_retry_entry_t *item)
{
	fr_assert(item->buffer != NULL);

	if (!item->retry.config) return NULL;

	return &item->retry;
}


/**  Cancel all outstanding packets.
 *
 */
static int fr_bio_retry_destructor(fr_bio_retry_t *my)
{
	fr_rb_iter_inorder_t iter;
	fr_bio_retry_entry_t *item;

	fr_bio_retry_timer_clear(my);

	/*
	 *	Cancel all outgoing packets.  Don't bother updating the tree or the free list, as all of the
	 *	entries will be deleted when the memory is freed.
	 */
	while ((item = fr_rb_iter_init_inorder(&iter, &my->timer_tree)) != NULL) {
		my->release((fr_bio_t *) my, item, FR_BIO_RETRY_CANCELLED);
	}

	return 0;
}

/**  Allocate a #fr_bio_retry_t
 *
 */
fr_bio_t *fr_bio_retry_alloc(TALLOC_CTX *ctx, size_t max_saved,
			     fr_bio_retry_sent_t sent,
			     fr_bio_retry_response_t response,
			     fr_bio_retry_rewrite_t rewrite,
			     fr_bio_retry_release_t release,
			     fr_bio_retry_config_t const *cfg,
			     fr_bio_t *next)
{
	size_t i;
	fr_bio_retry_t *my;
	fr_bio_retry_entry_t *items;

	fr_assert(cfg->el);

	/*
	 *	Limit to reasonable values.
	 */
	if (!max_saved) return NULL;
	if (max_saved > 65536) return NULL;

	my = talloc_zero(ctx, fr_bio_retry_t);
	if (!my) return NULL;

	/*
	 *	Allocate everything up front, to get better locality of reference, less memory fragmentation,
	 *	and better reuse of data structures.
	 */
	items = talloc_array(my, fr_bio_retry_entry_t, max_saved);
	if (!items) return NULL;

	/*
	 *	Insert the entries into the free list in order.
	 */
	fr_bio_retry_list_init(&my->free);
	for (i = 0; i < max_saved; i++) {
		items[i].my = my;
		fr_bio_retry_list_insert_tail(&my->free, &items[i]);
	}

	(void) fr_rb_inline_init(&my->timer_tree, fr_bio_retry_entry_t, timer_node, _timer_cmp, NULL);
	(void) fr_rb_inline_init(&my->expiry_tree, fr_bio_retry_entry_t, expiry_node, _expiry_cmp, NULL);

	my->sent = sent;
	if (!rewrite) {
		my->rewrite = fr_bio_retry_rewrite;
	} else {
		my->rewrite = rewrite;
	}
	my->response = response;
	my->release = release;

	my->el = cfg->el;
	my->info.last_idle = fr_time();
	my->retry_config = cfg->retry_config;

	my->bio.write = fr_bio_retry_write;
	my->bio.read = fr_bio_retry_read;


	fr_bio_chain(&my->bio, next);

	talloc_set_destructor(my, fr_bio_retry_destructor);

	return (fr_bio_t *) my;
}

fr_bio_retry_info_t const *fr_bio_retry_info(fr_bio_t *bio)
{
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);

	return &my->info;
}

size_t fr_bio_retry_outstanding(fr_bio_t *bio)
{
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);
	size_t num;

	num = fr_rb_num_elements(&my->timer_tree);

	if (!my->partial) return num;

	/*
	 *	Only count partially written items if they haven't been cancelled.
	 */
	return num + !my->partial->cancelled;
}

/**  Reserve an entry for later use with fr_bio_retry_rewrite()
 *
 *  So that application-layer watchdogs can bypass the normal write / retry routines.
 */
fr_bio_retry_entry_t *fr_bio_retry_item_reserve(fr_bio_t *bio)
{
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);
	fr_bio_retry_entry_t *item;

	item = fr_bio_retry_list_pop_head(&my->free);
	if (!item) return NULL;

	fr_assert(item->my == my);
	*item = (fr_bio_retry_entry_t) {
		.my = my,
		.reserved = true,
	};

	return item;
}


/** Writes are blocked.
 *
 *  For now, stop the retry timers.
 *
 *  We likely want to change this to a specific "expiry" timer, so that we still expire entries.
 */
int fr_bio_retry_write_blocked(fr_bio_t *bio)
{
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);

	if (my->info.write_blocked) {
		fr_assert(!my->ev);
		return 1;
	}

	fr_bio_retry_timer_clear(my);
	my->info.write_blocked = true;

	return 1;
}

/** Resume writes.
 *
 *  On resume, we try to flush any pending packets which should have been sent.
 */
int fr_bio_retry_write_resume(fr_bio_t *bio)
{
	fr_bio_retry_t *my = talloc_get_type_abort(bio, fr_bio_retry_t);
	int rcode;

	if (!my->info.write_blocked) return 1;

	rcode = fr_bio_retry_write_delayed(my, fr_time());
	if (rcode <= 0) return rcode;

	(void) fr_bio_retry_timer_reset(my);

	return 1;
}
