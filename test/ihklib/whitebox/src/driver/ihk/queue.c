/**
 * \file ikc/queue.c
 * \brief IHK-IKC: Queue functions
 *
 * Copyright (C) 2011-2012 Taku Shimosawa <shimosawa@is.s.u-tokyo.ac.jp>
 * Lock-free implementation: Balazs Gerofi <bgerofi@riken.jp>
 */
#include <ikc/ihk.h>
#include <ikc/queue.h>
#include <ikc/msg.h>

#define PRINT kprintf

#include "driver/ihk_host_user.h"
#include "driver/okng_driver.h"
#include "branch_info.h"

//#define DEBUG_QUEUE

#ifdef DEBUG_PRINT_IKC
#ifndef dkprintf
#define dkprintf kprintf
#else
#undef DDEBUG_DEFAULT
#define DDEBUG_DEFAULT DDEBUG_PRINT
#endif
#else
#ifndef dkprintf
#define dkprintf(...)
#endif
#endif

#define IHK_IKC_WRITE_QUEUE_RETRY	128

#ifndef __HEADER_IHK_HOST_DRIVER_H
int ihk_host_validate_os(void *os)
{
	return 1;
}
#endif

void ihk_ikc_notify_remote_read(struct ihk_ikc_channel_desc *c);
void ihk_ikc_notify_remote_write(struct ihk_ikc_channel_desc *c);

/*
 * Do copy by long
 */

static void *memcpyl(void *dest, const void *src, size_t n)
{
	unsigned long *d = dest;
	const unsigned long *s = src;

	n /= sizeof(unsigned long);

	while (n > 0) {
		*(d++) = *(s++);
		n--;
	}

	return dest;
}

/*
 * NOTE: Local CPU is responsible to call the init
 */
int ihk_ikc_init_queue(struct ihk_ikc_queue_head *q,
                       int id, int type, int size, int packetsize)
{
	if (!q) {
		return -EINVAL;
	}

	memset(q, 0, sizeof(*q));

	q->id = id;
	q->type = type;
	q->pktsize = packetsize;
	q->pktcount = (size - sizeof(struct ihk_ikc_queue_head)) / packetsize;

	q->read_off = q->max_read_off = q->write_off = 0;
	q->read_cpu = 0;
	q->write_cpu = 0;
	q->queue_size = q->pktsize * q->pktcount;
	dkprintf("%s: queue %p pktcount: %lu\n",
		__FUNCTION__, (void *)virt_to_phys(q), q->pktcount);

	return 0;
}

int ihk_ikc_queue_is_empty(struct ihk_ikc_queue_head *q)
{
	if (!q) {
		return -EINVAL;
	}
	return q->read_off == q->max_read_off;
}

int ihk_ikc_queue_is_full(struct ihk_ikc_queue_head *q)
{
	uint64_t r, w;

	if (!q) {
		return -EINVAL;
	}

	r = q->read_off;
	w = q->write_off;

	barrier();

	if ((w - r) == (q->pktcount - 1))
		return 1;

	return 0;
}

int ihk_ikc_read_queue(struct ihk_ikc_queue_head *q, void *packet, int flag)
{
	uint64_t r, m;

	if(!q || !packet) {
		return -EINVAL;
	}

retry:
	r = q->read_off;
	m = q->max_read_off;
	barrier();

	/* Is the queue empty? */
	if (r == m) {
		return -1;
	}

	/* Try to advance the queue, but see if someone else has done it already */
	if (cmpxchg(&q->read_off, r, r + 1) != r) {
		goto retry;
	}
	dkprintf("%s: queue %p r: %llu, m: %llu\n",
			__FUNCTION__, (void *)virt_to_phys(q), r, m);

	memcpyl(packet,	(char *)q + sizeof(*q) +
		((r % q->pktcount) * q->pktsize), q->pktsize);

	return 0;
}

int ihk_ikc_read_queue_handler(struct ihk_ikc_queue_head *q,
                               struct ihk_ikc_channel_desc *c,
                               int (*h)(struct ihk_ikc_channel_desc *,
                                        void *, void *), void *harg, int flag)
{
	uint64_t r, m;

retry:
	r = q->read_off;
	m = q->max_read_off;
	barrier();

	/* Is the queue empty? */
	if (r == m) {
		return -1;
	}

	/* Try to advance the queue, but see if someone else has done it already */
	if (cmpxchg(&q->read_off, r, r + 1) != r) {
		goto retry;
	}
	dkprintf("%s: queue %p r: %llu, m: %llu\n",
			__FUNCTION__, (void *)virt_to_phys(q), r, m);

	h(c, (char *)q + sizeof(*q) + ((r % q->pktcount) * q->pktsize), harg);

	return 0;
}

int ihk_ikc_write_queue_orig(struct ihk_ikc_queue_head *q, void *packet, int flag)
{
	uint64_t r, w;
	int attempt = 0;

	if (!q || !packet) {
		return -EINVAL;
	}

retry:
	r = q->read_off;
	w = q->write_off;
	barrier();

	/* Is the queue full? */
	if ((w - r) == (q->pktcount - 1)) {
		/* Did we run out of attempts? */
		if (++attempt > IHK_IKC_WRITE_QUEUE_RETRY) {
			kprintf("%s: queue %p r: %llu, w: %llu is full\n",
					    __FUNCTION__, (void *)virt_to_phys(q), r, w);
			return -EBUSY;
		}

		dkprintf("%s: queue %p r: %llu, w: %llu full, retrying\n",
			       __FUNCTION__, (void *)virt_to_phys(q), r, w);
		goto retry;
	}

	/* Try to advance the queue, but see if someone else has done it already */
	if (cmpxchg(&q->write_off, w, w + 1) != w) {
		goto retry;
	}
	dkprintf("%s: queue %p r: %llu, w: %llu\n",
			     __FUNCTION__, (void *)virt_to_phys(q), r, w);

	memcpyl((char *)q + sizeof(*q) + ((w % q->pktcount) * q->pktsize),
			    packet, q->pktsize);

	/*
	 * Advance the max read index so that the element is visible to readers,
	 * this has to succeed eventually, but we cannot afford to be interrupted
	 * by another request which would then end up waiting for this hence
	 * IRQs are disabled during queue operations.
	 */
	while (cmpxchg(&q->max_read_off, w, w + 1) != w) {}

	return 0;
}

int ihk_ikc_write_queue(struct ihk_ikc_queue_head *q, void *packet, int flag)
{
  if (g_ihk_test_mode != TEST_IHK_IKC_WRITE_QUEUE)  // Disable test code
    return ihk_ikc_write_queue_orig(q, packet, flag);

  unsigned long ivec = 0;
  unsigned long total_branch = 3;

  branch_info_t b_infos[] = {
    { -EINVAL, "invalid queue or packet" },
    { -EBUSY,  "queue is full" },
    { 0,       "main case" },
  };

  /* save previous state */
  if (!q) return -EINVAL;
  uint64_t w_prev = q->write_off;;
  uint64_t max_read_prev = q->max_read_off;

  for (ivec = 0; ivec < total_branch; ++ivec) {
    START(b_infos[ivec].name);

  	uint64_t r, w;
    uint64_t w_after, max_read_after;
    int ret = 0;
  	int attempt = 0;

  	if (ivec == 0 || (!q || !packet)) {
  		ret = -EINVAL;
      if (ivec != 0) return ret;
      goto out;
  	}

  retry:
  	r = q->read_off;
  	w = q->write_off;
  	barrier();

  	/* Is the queue full? */
  	if (ivec == 1 || ((w - r) == (q->pktcount - 1))) {
  		/* Did we run out of attempts? */
  		if (++attempt > IHK_IKC_WRITE_QUEUE_RETRY) {
        ret = -EBUSY;
        if (ivec != 1) {
  			  kprintf("%s: queue %p r: %llu, w: %llu is full\n",
  					      __FUNCTION__, (void *)virt_to_phys(q), r, w);
          return ret;
        }
        goto out;
  		}

      if (ivec > 1)
  		  dkprintf("%s: queue %p r: %llu, w: %llu full, retrying\n",
  			         __FUNCTION__, (void *)virt_to_phys(q), r, w);
  		goto retry;
  	}

  	/* Try to advance the queue, but see if someone else has done it already */
  	if (cmpxchg(&q->write_off, w, w + 1) != w) {
  		goto retry;
  	}
  	dkprintf("%s: queue %p r: %llu, w: %llu\n",
  			     __FUNCTION__, (void *)virt_to_phys(q), r, w);

  	memcpyl((char *)q + sizeof(*q) + ((w % q->pktcount) * q->pktsize),
  			    packet, q->pktsize);

  	/*
  	 * Advance the max read index so that the element is visible to readers,
  	 * this has to succeed eventually, but we cannot afford to be interrupted
  	 * by another request which would then end up waiting for this hence
  	 * IRQs are disabled during queue operations.
  	 */
  	while (cmpxchg(&q->max_read_off, w, w + 1) != w) {}

   out:
    BRANCH_RET_CHK(ret, b_infos[ivec].expected);

    /* check current state */
    w_after = q->write_off;;
    max_read_after = q->max_read_off;
    if (ivec == total_branch - 1) {
      OKNG(w_after == w_prev + 1, "write offset should be increased by 1\n");
      OKNG((max_read_after == max_read_prev + 1) &&
			 		 (max_read_prev + 1 == w_after),
           "check max read offset\n");
    } else {
      OKNG(w_after == w_prev, "write offset should not be changed\n");
      OKNG(max_read_after == max_read_prev,
           "max read offset should not be changed\n");
    }
  }
	return 0;
 err:
  return -EINVAL;
}

/*
 * Channel and queue descriptors
 */
void ihk_ikc_init_desc_orig(struct ihk_ikc_channel_desc *c,
                            ihk_os_t ros, int port,
                            struct ihk_ikc_queue_head *rq,
                            struct ihk_ikc_queue_head *wq,
                            ihk_ikc_ph_t packet_handler,
					                  struct ihk_ikc_channel_desc *master)
{
	struct list_head *all_list = ihk_ikc_get_channel_list(ros);
	ihk_spinlock_t *all_lock = ihk_ikc_get_channel_list_lock(ros);
	unsigned long flags;

	INIT_LIST_HEAD(&c->list_all);
	INIT_LIST_HEAD(&c->packet_pool);

	c->remote_os = ros;
	c->port = port;
	c->channel_id = ihk_ikc_get_unique_channel_id(ros);
	c->recv.queue = rq;
	c->send.queue = wq;
	if (rq) {
		c->recv.queue->channel_id = c->channel_id;
		c->recv.queue->read_cpu = ihk_ikc_get_processor_id();
		c->recv.cache = *rq;
	}
	if (wq) {
		c->remote_channel_id = c->send.cache.channel_id;
		c->send.queue->write_cpu = ihk_ikc_get_processor_id();
		c->send.cache = *wq;
	}
	c->handler = packet_handler;
	c->master = master;

	ihk_ikc_spinlock_init(&c->recv.lock);
	ihk_ikc_spinlock_init(&c->send.lock);
	ihk_ikc_spinlock_init(&c->packet_pool_lock);

	flags = ihk_ikc_spinlock_lock(all_lock);
	list_add_tail(&c->list_all, all_list);
	ihk_ikc_spinlock_unlock(all_lock, flags);
}

void ihk_ikc_init_desc(struct ihk_ikc_channel_desc *c,
                       ihk_os_t ros, int port,
                       struct ihk_ikc_queue_head *rq,
                       struct ihk_ikc_queue_head *wq,
                       ihk_ikc_ph_t packet_handler,
					             struct ihk_ikc_channel_desc *master)
{
  if (g_ihk_test_mode != TEST_IHK_IKC_INIT_DESC)  // Disable test code
    return ihk_ikc_init_desc_orig(c, ros, port, rq, wq, packet_handler, master);

  unsigned long ivec = 0;
  unsigned long total_branch = 3;

  branch_info_t b_infos[] = {
    { 0, "invalid channel desc" },
    { 0, "invalid parameter" },
    { 0, "main case" },
  };

  /* save previous state */
  struct list_head *all_list = ihk_ikc_get_channel_list(ros);
  ihk_spinlock_t *all_lock = ihk_ikc_get_channel_list_lock(ros);
  struct ihk_ikc_channel_desc *c_it;
  int count_list_all_prev = 0;
  unsigned long flags = ihk_ikc_spinlock_lock(all_lock);
  list_for_each_entry(c_it, all_list, list_all) {
    count_list_all_prev++;
  }
  ihk_ikc_spinlock_unlock(all_lock, flags);

  for (ivec = 0; ivec < total_branch; ++ivec) {
    START(b_infos[ivec].name);

    int count_list_all_after = 0;

    if (ivec == 0 || (!c || !master)) {
      if (ivec != 0) return;
      goto out;
    }

    if (ivec == 1 || (ihk_host_validate_os(ros) || !rq || !wq)) {
      if (ivec != 1) return;
      goto out;
    }

  	INIT_LIST_HEAD(&c->list_all);
  	INIT_LIST_HEAD(&c->packet_pool);

  	c->remote_os = ros;
  	c->port = port;
  	c->channel_id = ihk_ikc_get_unique_channel_id(ros);
  	c->recv.queue = rq;
  	c->send.queue = wq;
  	if (rq) {
  		c->recv.queue->channel_id = c->channel_id;
  		c->recv.queue->read_cpu = ihk_ikc_get_processor_id();
  		c->recv.cache = *rq;
  	}
  	if (wq) {
  		c->remote_channel_id = c->send.cache.channel_id;
  		c->send.queue->write_cpu = ihk_ikc_get_processor_id();
  		c->send.cache = *wq;
  	}
  	c->handler = packet_handler;
  	c->master = master;

  	ihk_ikc_spinlock_init(&c->recv.lock);
  	ihk_ikc_spinlock_init(&c->send.lock);
  	ihk_ikc_spinlock_init(&c->packet_pool_lock);

  	flags = ihk_ikc_spinlock_lock(all_lock);
  	list_add_tail(&c->list_all, all_list);
  	ihk_ikc_spinlock_unlock(all_lock, flags);

   out:
    flags = ihk_ikc_spinlock_lock(all_lock);
    list_for_each_entry(c_it, all_list, list_all) {
      count_list_all_after++;
    }
   ihk_ikc_spinlock_unlock(all_lock, flags);

    if (ivec == total_branch - 1) {
      OKNG(count_list_all_after == count_list_all_prev + 1,
           "channel list should be added a new item\n");
    } else {
      OKNG(count_list_all_after == count_list_all_prev,
           "channel list should not be changed\n");
    }
  }
 err:
  return;
}

/*
 * Packet pool functions.
 */
struct ihk_ikc_free_packet *ihk_ikc_alloc_packet(
	struct ihk_ikc_channel_desc *c)
{
	unsigned long flags;
	struct ihk_ikc_free_packet *p = NULL;
	struct ihk_ikc_free_packet *p_iter;

	flags = ihk_ikc_spinlock_lock(&c->packet_pool_lock);
	list_for_each_entry(p_iter, &c->packet_pool, list) {
		p = p_iter;
		list_del(&p->list);
		break;
	}
	ihk_ikc_spinlock_unlock(&c->packet_pool_lock, flags);

	/* No packet? Allocate new */
	if (!p) {
retry_alloc:
		p = (struct ihk_ikc_free_packet *)ihk_ikc_malloc(c->recv.queue->pktsize);
		if (!p) {
			kprintf("%s: ERROR allocating packet, retrying\n", __FUNCTION__);
			goto retry_alloc;
		}
		dkprintf("%s: packet %p kmalloc'd on channel %p %s\n",
			__FUNCTION__, p, c, c == c->master ? "(master)" : "");
	}
	else {
		dkprintf("%s: packet %p obtained from pool on channel %p %s\n",
			__FUNCTION__, p, c, c == c->master ? "(master)" : "");
	}

	return p;
}

void ihk_ikc_release_packet_orig(struct ihk_ikc_free_packet *p)
{
	unsigned long flags;
	struct ihk_ikc_channel_desc *c;

	if (!p) {
		return;
	}

	c = p->header.channel;
	if (!c) {
		kprintf("%s: WARNING: channel of packet (%p) is NULL\n", __func__, p);
		ihk_ikc_free(p);
		return;
	}

	flags = ihk_ikc_spinlock_lock(&c->packet_pool_lock);
	list_add_tail(&p->list, &c->packet_pool);
	ihk_ikc_spinlock_unlock(&c->packet_pool_lock, flags);
	dkprintf("%s: packet %p released to pool on channel %p %s\n",
			     __FUNCTION__, p, c, c == c->master ? "(master)" : "");
}

void ihk_ikc_release_packet(struct ihk_ikc_free_packet *p)
{
  if (g_ihk_test_mode != TEST_IHK_IKC_RELEASE_PACKET)  // Disable test code
    return ihk_ikc_release_packet_orig(p);

  unsigned long ivec = 0;
  unsigned long total_branch = 3;

  branch_info_t b_infos[] = {
    { 0, "invalid packet" },
    { 0, "channel of packet is null" },
    { 0, "main case" },
  };

  int count_pkt_pool_prev = 0;
  struct ihk_ikc_free_packet *p_iter;
  unsigned long flags;
  if (!p || !p->header.channel) goto skip_count_prev;

	flags = ihk_ikc_spinlock_lock(&p->header.channel->packet_pool_lock);
	list_for_each_entry(p_iter, &p->header.channel->packet_pool, list) {
    count_pkt_pool_prev++;
	}

 skip_count_prev:
  for (ivec = 0; ivec < total_branch; ++ivec) {
    START(b_infos[ivec].name);

  	struct ihk_ikc_channel_desc *c;
    int count_pkt_pool_after = 0;

  	if (ivec == 0 || !p) {
      if (ivec != 0) return;
      goto out;
  	}

  	c = p->header.channel;
  	if (ivec == 1 || !c) {
      if (ivec != 1) {
    		kprintf("%s: WARNING: channel of packet (%p) is NULL\n", __func__, p);
    		ihk_ikc_free(p);
        ihk_ikc_spinlock_unlock(&c->packet_pool_lock, flags);
    		return;
      }
      goto out;
  	}

  	list_add_tail(&p->list, &c->packet_pool);

  	dkprintf("%s: packet %p released to pool on channel %p %s\n",
  			     __FUNCTION__, p, c, c == c->master ? "(master)" : "");

   out:
    list_for_each_entry(p_iter, &p->header.channel->packet_pool, list) {
      count_pkt_pool_after++;
    }

    if (ivec == total_branch - 1) {
      OKNG(count_pkt_pool_after == count_pkt_pool_prev + 1,
           "check # of packets in pool.\n");
    } else {
      OKNG(count_pkt_pool_after == count_pkt_pool_prev,
           "check # of packets in pool\n");
    }
  }
 err:
  ihk_ikc_spinlock_unlock(&p->header.channel->packet_pool_lock, flags);
  return;
}

void ihk_ikc_channel_set_cpu(struct ihk_ikc_channel_desc *c, int cpu)
{
	c->send.queue->write_cpu = c->recv.queue->read_cpu = cpu;
}

int ihk_ikc_set_remote_queue(struct ihk_ikc_queue_desc *q, ihk_os_t os,
                             unsigned long rphys, unsigned long qsize)
{
	int qpages;

	qpages = (qsize + PAGE_SIZE - 1) >> PAGE_SHIFT;

	ihk_ikc_spinlock_init(&q->lock);
	q->qrphys = rphys;
	q->qphys = ihk_ikc_map_memory(os, q->qrphys, qpages * PAGE_SIZE);
	q->queue = ihk_ikc_map_virtual(ihk_os_to_dev(os), q->qphys,
	                               qpages,
	                               IHK_IKC_QUEUE_PT_ATTR);
	q->cache = *q->queue;

	return 0;
}

struct ihk_ikc_channel_desc *ihk_ikc_create_channel(ihk_os_t os,
                                                    int port,
                                                    int packet_size,
                                                    unsigned long qsize,
                                                    unsigned long *rq,
                                                    unsigned long *sq,
                                                    enum ihk_ikc_channel_flag f)
{
	unsigned long phys;
	struct ihk_ikc_channel_desc *desc;
	struct ihk_ikc_queue_head *recvq, *sendq;
	int qpages;

	qpages = (qsize + PAGE_SIZE - 1) >> PAGE_SHIFT;

	desc = ihk_ikc_malloc(sizeof(struct ihk_ikc_channel_desc)
	                      + packet_size);
	if (!desc) {
		return NULL;
	}

	memset(desc, 0, sizeof(*desc));

	desc->flag = f;

	if (!*rq) {
		recvq = ihk_ikc_alloc_queue(qpages);
		if (!recvq) {
			ihk_ikc_free(desc);
			return NULL;
		}

		ihk_ikc_init_queue(recvq, 1, port, PAGE_SIZE * qpages,
		                   packet_size);
		*rq = virt_to_phys(recvq);

		desc->recv.qrphys = 0;
		desc->recv.qphys = *rq;
	} else {
		phys = ihk_ikc_map_memory(os, *rq, qpages * PAGE_SIZE);
		recvq = ihk_ikc_map_virtual(ihk_os_to_dev(os), phys,
		                            qpages,
		                            IHK_IKC_QUEUE_PT_ATTR);

		desc->recv.qrphys = *rq;
		desc->recv.qphys = phys;
	}
	/* XXX: This do not assume local send queue */
	if (*sq) {
		phys = ihk_ikc_map_memory(os, *sq, qpages * PAGE_SIZE);
		sendq = ihk_ikc_map_virtual(ihk_os_to_dev(os), phys,
		                            qpages,
		                            IHK_IKC_QUEUE_PT_ATTR);

		desc->send.qrphys = *sq;
		desc->send.qphys = phys;
	} else {
		sendq = NULL;
	}

	ihk_ikc_init_desc(desc, os, port, recvq, sendq, NULL,
			ihk_ikc_get_master_channel(os));

	return desc;
}

void ihk_ikc_free_channel_orig(struct ihk_ikc_channel_desc *desc)
{
	ihk_os_t os = desc->remote_os;
	int qpages;
	ihk_spinlock_t *lock = ihk_ikc_get_channel_list_lock(os);
	struct ihk_ikc_free_packet *p_iter, *p_next;
	unsigned long flags;

	flags = ihk_ikc_spinlock_lock(lock);
	list_del(&desc->list_all);
	ihk_ikc_spinlock_unlock(lock, flags);

	flags = ihk_ikc_spinlock_lock(&desc->packet_pool_lock);
	list_for_each_entry_safe(p_iter, p_next, &desc->packet_pool, list) {
		list_del(&p_iter->list);
		ihk_ikc_free(p_iter);
	}
	ihk_ikc_spinlock_unlock(&desc->packet_pool_lock, flags);

	if (desc->recv.queue) {
		qpages = (desc->recv.queue->queue_size
		          + sizeof(struct ihk_ikc_queue_head) + PAGE_SIZE - 1)
			>> PAGE_SHIFT;
		if (desc->recv.qrphys) {
			ihk_ikc_unmap_virtual(ihk_os_to_dev(os), desc->recv.queue, qpages);
			ihk_ikc_unmap_memory(os, desc->recv.qphys, qpages);
		} else {
			ihk_ikc_free_queue(desc->recv.queue);
		}
	}

	if (desc->send.queue) {
		qpages = (desc->send.queue->queue_size
		          + sizeof(struct ihk_ikc_queue_head) + PAGE_SIZE - 1)
			>> PAGE_SHIFT;
		if (desc->send.qrphys) {
			ihk_ikc_unmap_virtual(ihk_os_to_dev(os), desc->send.queue, qpages);
			ihk_ikc_unmap_memory(os, desc->send.qphys, qpages);
		} else {
			ihk_ikc_free_queue(desc->send.queue);
		}
	}

	ihk_ikc_free(desc);
}

void ihk_ikc_free_channel(struct ihk_ikc_channel_desc *desc)
{
  if (g_ihk_test_mode != TEST_IHK_IKC_FREE_CHANNEL)  // Disable test code
    return ihk_ikc_free_channel_orig(desc);

  unsigned long ivec = 0;
  unsigned long total_branch = 3;

  branch_info_t b_infos[] = {
    { 0, "invalid channel desc" },
    { 0, "packet pool is empty" },
    { 0, "main case" },
  };

  if (!desc) return;
  ihk_os_t os = desc->remote_os;
  struct list_head *all_list = ihk_ikc_get_channel_list(os);
  ihk_spinlock_t *all_lock = ihk_ikc_get_channel_list_lock(os);
  int all_list_count_prev = 0;
  struct ihk_ikc_channel_desc *c_it;
  unsigned long flags = ihk_ikc_spinlock_lock(all_lock);
  list_for_each_entry(c_it, all_list, list_all) {
    all_list_count_prev++;
  }
  ihk_ikc_spinlock_unlock(all_lock, flags);

  for (ivec = 0; ivec < total_branch; ++ivec) {
    START(b_infos[ivec].name);

    int all_list_count_after = 0;
  	int qpages;
  	struct ihk_ikc_free_packet *p_iter, *p_next;

    if (ivec == 0 || !desc) {
      if (ivec != 0) return;
      goto out;
    }

  	flags = ihk_ikc_spinlock_lock(all_lock);
  	list_del(&desc->list_all);
  	ihk_ikc_spinlock_unlock(all_lock, flags);

  	flags = ihk_ikc_spinlock_lock(&desc->packet_pool_lock);
    if (ivec == 1 || list_empty(&desc->packet_pool)) {
      if (ivec == 1) {
        ihk_ikc_spinlock_unlock(&desc->packet_pool_lock, flags);

        /* reset state before go out */
        flags = ihk_ikc_spinlock_lock(all_lock);
      	list_add_tail(&desc->list_all, all_list);
      	ihk_ikc_spinlock_unlock(all_lock, flags);
        goto out;
      }
    }
  	list_for_each_entry_safe(p_iter, p_next, &desc->packet_pool, list) {
  		list_del(&p_iter->list);
  		ihk_ikc_free(p_iter);
  	}
  	ihk_ikc_spinlock_unlock(&desc->packet_pool_lock, flags);

  	if (desc->recv.queue) {
  		qpages = (desc->recv.queue->queue_size
  		          + sizeof(struct ihk_ikc_queue_head) + PAGE_SIZE - 1)
  			>> PAGE_SHIFT;
  		if (desc->recv.qrphys) {
  			ihk_ikc_unmap_virtual(ihk_os_to_dev(os), desc->recv.queue, qpages);
  			ihk_ikc_unmap_memory(os, desc->recv.qphys, qpages);
  		} else {
  			ihk_ikc_free_queue(desc->recv.queue);
  		}
  	}

  	if (desc->send.queue) {
  		qpages = (desc->send.queue->queue_size
  		          + sizeof(struct ihk_ikc_queue_head) + PAGE_SIZE - 1)
  			>> PAGE_SHIFT;
  		if (desc->send.qrphys) {
  			ihk_ikc_unmap_virtual(ihk_os_to_dev(os), desc->send.queue, qpages);
  			ihk_ikc_unmap_memory(os, desc->send.qphys, qpages);
  		} else {
  			ihk_ikc_free_queue(desc->send.queue);
  		}
  	}

  	ihk_ikc_free(desc);

   out:
    flags = ihk_ikc_spinlock_lock(all_lock);
    list_for_each_entry(c_it, all_list, list_all) {
      all_list_count_after++;
    }
    ihk_ikc_spinlock_unlock(all_lock, flags);

    if (ivec == total_branch - 1) {
      OKNG(list_empty(&desc->packet_pool), "packet pool should be empty\n");
      OKNG(all_list_count_prev == all_list_count_after + 1,
           "the number of items in channel list should be decreased by 1\n");
    } else {
      OKNG(all_list_count_prev == all_list_count_after,
           "the number of items in channel list should be unchanged\n");
    }
  }
 err:
  return;
}


int ihk_ikc_recv(struct ihk_ikc_channel_desc *channel, void *p, int opt)
{
	int r;
	unsigned long flags;

	if (!channel || !p) {
		return -EINVAL;
	}

#ifdef IHK_OS_MANYCORE
	flags = cpu_disable_interrupt_save();
#else
	local_irq_save(flags);
#endif
	if (ihk_ikc_channel_enabled(channel)) {
		r = ihk_ikc_read_queue(channel->recv.queue, p, opt);

		/* We set channel here instead of setting it on
		 * allocation and skipping those bytes when receiving
		 * because we can ignore the overhead of 8-byte redundant
		 * copy
		 */
		if (!r) {
			((struct ihk_ikc_packet_header *)p)->channel = channel;
		}

		/* XXX: Optimal interrupt */
		if (!(opt & IKC_NO_NOTIFY)) {
			ihk_ikc_notify_remote_read(channel);
		}
	} else {
		r = -EINVAL;
	}
#ifdef IHK_OS_MANYCORE
	cpu_restore_interrupt(flags);
#else
	local_irq_restore(flags);
#endif

	return r;
}

#if 0
static int __ihk_ikc_recv_nocopy(struct ihk_ikc_channel_desc *channel,
                                 ihk_ikc_ph_t h, void *harg, int opt)
{
	unsigned long flags;
	int r;

	flags = ihk_ikc_spinlock_lock(&channel->recv.lock);
	if (ihk_ikc_channel_enabled(channel) &&
	    !ihk_ikc_queue_is_empty(channel->recv.queue)) {
		while (ihk_ikc_read_queue_handler(channel->recv.queue,
		                                  channel,
		                                  h, harg, opt) == 0);
		/* XXX: Optimal interrupt */
		ihk_ikc_notify_remote_read(channel);

		r = 0;
	} else {
		r = -EINVAL;
	}
	ihk_ikc_spinlock_unlock(&channel->recv.lock, flags);

	return r;
}
#endif

int ihk_ikc_recv_handler(struct ihk_ikc_channel_desc *channel,
		ihk_ikc_ph_t h, void *harg, int opt)
{
	char *p = NULL;
	int r = -ENOENT;

	if (!channel) {
		kprintf("%s: ERROR: channel doesn't exist\n", __FUNCTION__);
		return -EINVAL;
	}

	/* Get free packet from channel pool */
	p = (char *)ihk_ikc_alloc_packet(channel);

	if (!p) {
		kprintf("%s: error allocating packet\n", __FUNCTION__);
		return -ENOMEM;
	}

	if ((r = ihk_ikc_recv(channel, p, opt | IKC_NO_NOTIFY)) != 0) {
		kprintf("%s: WARNING: ihk_ikc_recv returned %d%s\n",
			__func__, r, r == -1 ? " (empty queue)" : "");
		ihk_ikc_release_packet((struct ihk_ikc_free_packet *)p);
		goto out;
	}

	/*
	 * XXX: Handler must release the packet eventually using
	 * ihk_ikc_release_packet().
	 *
	 * (syscall_packet_handler() is the function called for syscalls)
	 */
	h(channel, p, harg);

	if (channel->flag & IKC_FLAG_NO_COPY) {
		ihk_ikc_notify_remote_read(channel);
	}
out:
	return r;
}

void ihk_ikc_notify_remote_read(struct ihk_ikc_channel_desc *c)
{
	ihk_ikc_send_interrupt(c);
}
void ihk_ikc_notify_remote_write(struct ihk_ikc_channel_desc *c)
{
	ihk_ikc_send_interrupt(c);
}

void __ihk_ikc_enable_channel(struct ihk_ikc_channel_desc *channel)
{
	channel->flag |= IKC_FLAG_ENABLED;
}

void __ihk_ikc_disable_channel(struct ihk_ikc_channel_desc *channel)
{
	channel->flag &= ~IKC_FLAG_ENABLED;
}

void ihk_ikc_enable_channel(struct ihk_ikc_channel_desc *channel)
{
	unsigned long flags;

	dkprintf("Channel %d enabled. Recv CPU = %d.\n",
	        channel->channel_id, channel->send.queue->read_cpu);

	flags = ihk_ikc_spinlock_lock(&channel->recv.lock);
	__ihk_ikc_enable_channel(channel);
	ihk_ikc_spinlock_unlock(&channel->recv.lock, flags);
}

void ihk_ikc_disable_channel(struct ihk_ikc_channel_desc *channel)
{
	unsigned long flags;

	flags = ihk_ikc_spinlock_lock(&channel->recv.lock);
	__ihk_ikc_disable_channel(channel);
	ihk_ikc_spinlock_unlock(&channel->recv.lock, flags);
}

struct ihk_ikc_channel_desc *ihk_ikc_find_channel(ihk_os_t os, int id)
{
	ihk_spinlock_t *lock = ihk_ikc_get_channel_list_lock(os);
	struct list_head *channels = ihk_ikc_get_channel_list(os);
	struct ihk_ikc_channel_desc *c;
	unsigned long flags;

	flags = ihk_ikc_spinlock_lock(lock);
	list_for_each_entry(c, channels, list_all) {
		if (c->channel_id == id) {
			ihk_ikc_spinlock_unlock(lock, flags);
			return c;
		}
	}
	ihk_ikc_spinlock_unlock(lock, flags);

	return NULL;
}

IHK_EXPORT_SYMBOL(ihk_ikc_recv);
IHK_EXPORT_SYMBOL(ihk_ikc_recv_handler);
IHK_EXPORT_SYMBOL(ihk_ikc_enable_channel);
IHK_EXPORT_SYMBOL(ihk_ikc_disable_channel);
IHK_EXPORT_SYMBOL(ihk_ikc_free_channel);
IHK_EXPORT_SYMBOL(ihk_ikc_find_channel);
IHK_EXPORT_SYMBOL(ihk_ikc_channel_set_cpu);
IHK_EXPORT_SYMBOL(ihk_ikc_release_packet);
