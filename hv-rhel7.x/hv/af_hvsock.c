/*
 * Hyper-V Socket driver
 *
 * Copyright(c) 2016, Microsoft Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include "include/linux/af_hvsock.h"

static struct proto hvsock_proto = {
	.name = "HV_SOCK",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct hvsock_sock),
};

#define SS_LISTEN 255

static LIST_HEAD(hvsock_bound_list);
static LIST_HEAD(hvsock_connected_list);
static DEFINE_MUTEX(hvsock_mutex);

static bool uuid_equals(uuid_le u1, uuid_le u2)
{
	return !uuid_le_cmp(u1, u2);
}

/* NOTE: hvsock_mutex must be held when the below helper functions, whose
 * names begin with __ hvsock, are invoked.
 */
static void __hvsock_insert_bound(struct list_head *list,
				  struct hvsock_sock *hvsk)
{
	sock_hold(&hvsk->sk);
	list_add(&hvsk->bound_list, list);
}

static void __hvsock_insert_connected(struct list_head *list,
				      struct hvsock_sock *hvsk)
{
	sock_hold(&hvsk->sk);
	list_add(&hvsk->connected_list, list);
}

static void __hvsock_remove_bound(struct hvsock_sock *hvsk)
{
	list_del_init(&hvsk->bound_list);
	sock_put(&hvsk->sk);
}

static void __hvsock_remove_connected(struct hvsock_sock *hvsk)
{
	list_del_init(&hvsk->connected_list);
	sock_put(&hvsk->sk);
}

static struct sock *__hvsock_find_bound_socket(const struct sockaddr_hv *addr)
{
	struct hvsock_sock *hvsk;

	list_for_each_entry(hvsk, &hvsock_bound_list, bound_list)
		if (uuid_equals(addr->shv_service_id,
				hvsk->local_addr.shv_service_id))
			return hvsock_to_sk(hvsk);
	return NULL;
}

static struct sock *__hvsock_find_connected_socket_by_channel(
	const struct vmbus_channel *channel)
{
	struct hvsock_sock *hvsk;

	list_for_each_entry(hvsk, &hvsock_connected_list, connected_list)
		if (hvsk->channel == channel)
			return hvsock_to_sk(hvsk);
	return NULL;
}

static bool __hvsock_in_bound_list(struct hvsock_sock *hvsk)
{
	return !list_empty(&hvsk->bound_list);
}

static bool __hvsock_in_connected_list(struct hvsock_sock *hvsk)
{
	return !list_empty(&hvsk->connected_list);
}

static void hvsock_insert_connected(struct hvsock_sock *hvsk)
{
	__hvsock_insert_connected(&hvsock_connected_list, hvsk);
}

static
void hvsock_enqueue_accept(struct sock *listener, struct sock *connected)
{
	struct hvsock_sock *hvlistener;
	struct hvsock_sock *hvconnected;

	hvlistener = sk_to_hvsock(listener);
	hvconnected = sk_to_hvsock(connected);

	sock_hold(connected);
	sock_hold(listener);

	mutex_lock(&hvlistener->accept_queue_mutex);
	list_add_tail(&hvconnected->accept_queue, &hvlistener->accept_queue);
	listener->sk_ack_backlog++;
	mutex_unlock(&hvlistener->accept_queue_mutex);
}

static struct sock *hvsock_dequeue_accept(struct sock *listener)
{
	struct hvsock_sock *hvlistener;
	struct hvsock_sock *hvconnected;

	hvlistener = sk_to_hvsock(listener);

	mutex_lock(&hvlistener->accept_queue_mutex);

	if (list_empty(&hvlistener->accept_queue)) {
		mutex_unlock(&hvlistener->accept_queue_mutex);
		return NULL;
	}

	hvconnected = list_entry(hvlistener->accept_queue.next,
				 struct hvsock_sock, accept_queue);

	list_del_init(&hvconnected->accept_queue);
	listener->sk_ack_backlog--;

	mutex_unlock(&hvlistener->accept_queue_mutex);

	sock_put(listener);
	/* The caller will need a reference on the connected socket so we let
	 * it call sock_put().
	 */

	return hvsock_to_sk(hvconnected);
}

static bool hvsock_is_accept_queue_empty(struct sock *sk)
{
	struct hvsock_sock *hvsk = sk_to_hvsock(sk);
	int ret;

	mutex_lock(&hvsk->accept_queue_mutex);
	ret = list_empty(&hvsk->accept_queue);
	mutex_unlock(&hvsk->accept_queue_mutex);

	return ret;
}

static void hvsock_addr_init(struct sockaddr_hv *addr, uuid_le service_id)
{
	memset(addr, 0, sizeof(*addr));
	addr->shv_family = AF_HYPERV;
	addr->shv_service_id = service_id;
}

static int hvsock_addr_validate(const struct sockaddr_hv *addr)
{
	if (!addr)
		return -EFAULT;

	if (addr->shv_family != AF_HYPERV)
		return -EAFNOSUPPORT;

	if (addr->reserved != 0)
		return -EINVAL;

	if (!uuid_equals(addr->shv_vm_id, NULL_UUID_LE))
		return -EINVAL;

	return 0;
}

static bool hvsock_addr_bound(const struct sockaddr_hv *addr)
{
	return !uuid_equals(addr->shv_service_id, SHV_SERVICE_ID_ANY);
}

static int hvsock_addr_cast(const struct sockaddr *addr, size_t len,
			    struct sockaddr_hv **out_addr)
{
	if (len < sizeof(**out_addr))
		return -EFAULT;

	*out_addr = (struct sockaddr_hv *)addr;
	return hvsock_addr_validate(*out_addr);
}

static int __hvsock_do_bind(struct hvsock_sock *hvsk,
			    struct sockaddr_hv *addr)
{
	struct sockaddr_hv hv_addr;
	int ret = 0;

	hvsock_addr_init(&hv_addr, addr->shv_service_id);

	mutex_lock(&hvsock_mutex);

	if (uuid_equals(addr->shv_service_id, SHV_SERVICE_ID_ANY)) {
		do {
			uuid_le_gen(&hv_addr.shv_service_id);
		} while (__hvsock_find_bound_socket(&hv_addr));
	} else {
		if (__hvsock_find_bound_socket(&hv_addr)) {
			ret = -EADDRINUSE;
			goto out;
		}
	}

	hvsock_addr_init(&hvsk->local_addr, hv_addr.shv_service_id);
	__hvsock_insert_bound(&hvsock_bound_list, hvsk);

out:
	mutex_unlock(&hvsock_mutex);

	return ret;
}

static int __hvsock_bind(struct sock *sk, struct sockaddr_hv *addr)
{
	struct hvsock_sock *hvsk = sk_to_hvsock(sk);
	int ret;

	if (hvsock_addr_bound(&hvsk->local_addr))
		return -EINVAL;

	switch (sk->sk_socket->type) {
	case SOCK_STREAM:
		ret = __hvsock_do_bind(hvsk, addr);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* Autobind this socket to the local address if necessary. */
static int hvsock_auto_bind(struct hvsock_sock *hvsk)
{
	struct sock *sk = hvsock_to_sk(hvsk);
	struct sockaddr_hv local_addr;

	if (hvsock_addr_bound(&hvsk->local_addr))
		return 0;
	hvsock_addr_init(&local_addr, SHV_SERVICE_ID_ANY);
	return __hvsock_bind(sk, &local_addr);
}

static void hvsock_sk_destruct(struct sock *sk)
{
	struct hvsock_sock *hvsk = sk_to_hvsock(sk);
	struct vmbus_channel *channel = hvsk->channel;

	if (!channel)
		return;

	vmbus_hvsock_device_unregister(channel);
}

static void __hvsock_release(struct sock *sk)
{
	struct hvsock_sock *hvsk;
	struct sock *pending;

	hvsk = sk_to_hvsock(sk);

	mutex_lock(&hvsock_mutex);
	if (__hvsock_in_bound_list(hvsk))
		__hvsock_remove_bound(hvsk);

	if (__hvsock_in_connected_list(hvsk))
		__hvsock_remove_connected(hvsk);
	mutex_unlock(&hvsock_mutex);

	lock_sock(sk);
	sock_orphan(sk);
	sk->sk_shutdown = SHUTDOWN_MASK;

	/* Clean up any sockets that never were accepted. */
	while ((pending = hvsock_dequeue_accept(sk)) != NULL) {
		__hvsock_release(pending);
		sock_put(pending);
	}

	release_sock(sk);
	sock_put(sk);
}

static int hvsock_release(struct socket *sock)
{
	/* If accept() is interrupted by a signal, the temporary socket
	 * struct's sock->sk is NULL.
	 */
	if (sock->sk) {
		__hvsock_release(sock->sk);
		sock->sk = NULL;
	}

	sock->state = SS_FREE;
	return 0;
}

static struct sock *__hvsock_create(struct net *net, struct socket *sock,
				    gfp_t priority, unsigned short type)
{
	struct hvsock_sock *hvsk;
	struct sock *sk;

	sk = sk_alloc(net, AF_HYPERV, priority, &hvsock_proto);
	if (!sk)
		return NULL;

	sock_init_data(sock, sk);

	/* sk->sk_type is normally set in sock_init_data, but only if sock is
	 * non-NULL. We make sure that our sockets always have a type by
	 * setting it here if needed.
	 */
	if (!sock)
		sk->sk_type = type;

	hvsk = sk_to_hvsock(sk);
	hvsock_addr_init(&hvsk->local_addr, SHV_SERVICE_ID_ANY);
	hvsock_addr_init(&hvsk->remote_addr, SHV_SERVICE_ID_ANY);

	sk->sk_destruct = hvsock_sk_destruct;

	/* Looks stream-based socket doesn't need this. */
	sk->sk_backlog_rcv = NULL;

	sk->sk_state = 0;
	sock_reset_flag(sk, SOCK_DONE);

	INIT_LIST_HEAD(&hvsk->bound_list);
	INIT_LIST_HEAD(&hvsk->connected_list);

	INIT_LIST_HEAD(&hvsk->accept_queue);
	mutex_init(&hvsk->accept_queue_mutex);

	hvsk->peer_shutdown = 0;

	hvsk->recv.data_len = 0;
	hvsk->recv.data_offset = 0;

	return sk;
}

static int hvsock_bind(struct socket *sock, struct sockaddr *addr,
		       int addr_len)
{
	struct sockaddr_hv *hv_addr;
	struct sock *sk;
	int ret;

	sk = sock->sk;

	if (hvsock_addr_cast(addr, addr_len, &hv_addr) != 0)
		return -EINVAL;

	lock_sock(sk);
	ret = __hvsock_bind(sk, hv_addr);
	release_sock(sk);

	return ret;
}

static int hvsock_getname(struct socket *sock,
			  struct sockaddr *addr, int *addr_len, int peer)
{
	struct sockaddr_hv *hv_addr;
	struct hvsock_sock *hvsk;
	struct sock *sk;
	int ret;

	sk = sock->sk;
	hvsk = sk_to_hvsock(sk);
	ret = 0;

	lock_sock(sk);

	if (peer) {
		if (sock->state != SS_CONNECTED) {
			ret = -ENOTCONN;
			goto out;
		}
		hv_addr = &hvsk->remote_addr;
	} else {
		hv_addr = &hvsk->local_addr;
	}

	__sockaddr_check_size(sizeof(*hv_addr));

	memcpy(addr, hv_addr, sizeof(*hv_addr));
	*addr_len = sizeof(*hv_addr);

out:
	release_sock(sk);
	return ret;
}

static int hvsock_shutdown(struct socket *sock, int mode)
{
	struct sock *sk;

	if (mode < SHUT_RD || mode > SHUT_RDWR)
		return -EINVAL;
	/* This maps:
	 * SHUT_RD   (0) -> RCV_SHUTDOWN  (1)
	 * SHUT_WR   (1) -> SEND_SHUTDOWN (2)
	 * SHUT_RDWR (2) -> SHUTDOWN_MASK (3)
	 */
	++mode;

	if (sock->state == SS_UNCONNECTED)
		return -ENOTCONN;

	sock->state = SS_DISCONNECTING;

	sk = sock->sk;

	lock_sock(sk);

	sk->sk_shutdown |= mode;
	sk->sk_state_change(sk);

	/* TODO: how to send a FIN if we haven't done that? */
	if (mode & SEND_SHUTDOWN)
		;

	release_sock(sk);

	return 0;
}

static void get_ringbuffer_rw_status(struct vmbus_channel *channel,
				     bool *can_read, bool *can_write)
{
	u32 avl_read_bytes, avl_write_bytes, dummy;

	if (can_read) {
		hv_get_ringbuffer_availbytes(&channel->inbound,
					     &avl_read_bytes,
					     &dummy);
		*can_read = avl_read_bytes >= HVSOCK_MIN_PKT_LEN;
	}

	/* We write into the ringbuffer only when we're able to write a
	 * a payload of 4096 bytes (the actual written payload's length may be
	 * less than 4096).
	 */
	if (can_write) {
		hv_get_ringbuffer_availbytes(&channel->outbound,
					     &dummy,
					     &avl_write_bytes);
		*can_write = avl_write_bytes > HVSOCK_PKT_LEN(PAGE_SIZE);
	}
}

static unsigned int hvsock_poll(struct file *file, struct socket *sock,
				poll_table *wait)
{
	struct vmbus_channel *channel;
	bool can_read, can_write;
	struct hvsock_sock *hvsk;
	struct sock *sk;
	unsigned int mask;

	sk = sock->sk;
	hvsk = sk_to_hvsock(sk);

	poll_wait(file, sk_sleep(sk), wait);
	mask = 0;

	if (sk->sk_err)
		/* Signify that there has been an error on this socket. */
		mask |= POLLERR;

	/* INET sockets treat local write shutdown and peer write shutdown as a
	 * case of POLLHUP set.
	 */
	if ((sk->sk_shutdown == SHUTDOWN_MASK) ||
	    ((sk->sk_shutdown & SEND_SHUTDOWN) &&
	     (hvsk->peer_shutdown & SEND_SHUTDOWN))) {
		mask |= POLLHUP;
	}

	if (sk->sk_shutdown & RCV_SHUTDOWN ||
	    hvsk->peer_shutdown & SEND_SHUTDOWN) {
		mask |= POLLRDHUP;
	}

	lock_sock(sk);

	/* Listening sockets that have connections in their accept
	 * queue can be read.
	 */
	if (sk->sk_state == SS_LISTEN && !hvsock_is_accept_queue_empty(sk))
		mask |= POLLIN | POLLRDNORM;

	/* The mutex is to against hvsock_open_connection() */
	mutex_lock(&hvsock_mutex);

	channel = hvsk->channel;
	if (channel) {
		/* If there is something in the queue then we can read */
		get_ringbuffer_rw_status(channel, &can_read, &can_write);

		if (!can_read && hvsk->recv.data_len > 0)
			can_read = true;

		if (!(sk->sk_shutdown & RCV_SHUTDOWN) && can_read)
			mask |= POLLIN | POLLRDNORM;
	} else {
		can_read = false;
		can_write = false;
	}

	mutex_unlock(&hvsock_mutex);

	/* Sockets whose connections have been closed terminated should
	 * also be considered read, and we check the shutdown flag for that.
	 */
	if (sk->sk_shutdown & RCV_SHUTDOWN ||
	    hvsk->peer_shutdown & SEND_SHUTDOWN) {
		mask |= POLLIN | POLLRDNORM;
	}

	/* Connected sockets that can produce data can be written. */
	if (sk->sk_state == SS_CONNECTED && can_write &&
	    !(sk->sk_shutdown & SEND_SHUTDOWN)) {
		/* Remove POLLWRBAND since INET sockets are not setting it.
		 */
		mask |= POLLOUT | POLLWRNORM;
	}

	/* Simulate INET socket poll behaviors, which sets
	 * POLLOUT|POLLWRNORM when peer is closed and nothing to read,
	 * but local send is not shutdown.
	 */
	if (sk->sk_state == SS_UNCONNECTED &&
	    !(sk->sk_shutdown & SEND_SHUTDOWN))
		mask |= POLLOUT | POLLWRNORM;

	release_sock(sk);

	return mask;
}

/* This function runs in the tasklet context of process_chn_event() */
static void hvsock_on_channel_cb(void *ctx)
{
	struct sock *sk = (struct sock *)ctx;
	struct hvsock_sock *hvsk = sk_to_hvsock(sk);
	struct vmbus_channel *channel = hvsk->channel;
	bool can_read, can_write;

	if (!channel) {
		WARN_ONCE(1, "NULL channel! There is a programming bug.\n");
		return;
	}

	get_ringbuffer_rw_status(channel, &can_read, &can_write);

	if (can_read)
		sk->sk_data_ready(sk, 0);

	if (can_write)
		sk->sk_write_space(sk);
}

static void hvsock_close_connection(struct vmbus_channel *channel)
{
	struct hvsock_sock *hvsk;
	struct sock *sk;

	mutex_lock(&hvsock_mutex);

	sk = __hvsock_find_connected_socket_by_channel(channel);

	/* The guest has already closed the connection? */
	if (!sk)
		goto out;

	sk->sk_socket->state = SS_UNCONNECTED;
	sk->sk_state = SS_UNCONNECTED;
	sock_set_flag(sk, SOCK_DONE);

	hvsk = sk_to_hvsock(sk);
	hvsk->peer_shutdown |= SEND_SHUTDOWN | RCV_SHUTDOWN;

	sk->sk_state_change(sk);
out:
	mutex_unlock(&hvsock_mutex);
}

static int hvsock_open_connection(struct vmbus_channel *channel)
{
	struct hvsock_sock *hvsk, *new_hvsk;
	struct sockaddr_hv hv_addr;
	struct sock *sk, *new_sk;

	uuid_le *instance, *service_id;
	int ret;

	instance = &channel->offermsg.offer.if_instance;
	service_id = &channel->offermsg.offer.if_type;

	hvsock_addr_init(&hv_addr, *instance);

	mutex_lock(&hvsock_mutex);

	sk = __hvsock_find_bound_socket(&hv_addr);

	if (sk) {
		/* It is from the guest client's connect() */
		if (sk->sk_state != SS_CONNECTING) {
			ret = -ENXIO;
			goto out;
		}

		hvsk = sk_to_hvsock(sk);
		hvsk->channel = channel;
		set_channel_read_mode(channel, HV_CALL_DIRECT);
		vmbus_set_chn_rescind_callback(channel,
					       hvsock_close_connection);
		ret = vmbus_open(channel, VMBUS_RINGBUFFER_SIZE_HVSOCK_SEND,
				 VMBUS_RINGBUFFER_SIZE_HVSOCK_RECV, NULL, 0,
				 hvsock_on_channel_cb, sk);
		if (ret != 0) {
			hvsk->channel = NULL;
			goto out;
		}

		set_channel_pending_send_size(channel,
					      HVSOCK_PKT_LEN(PAGE_SIZE));
		sk->sk_state = SS_CONNECTED;
		sk->sk_socket->state = SS_CONNECTED;
		hvsock_insert_connected(hvsk);
		sk->sk_state_change(sk);
		goto out;
	}

	/* Now we suppose it is from a host client's connect() */
	hvsock_addr_init(&hv_addr, *service_id);
	sk = __hvsock_find_bound_socket(&hv_addr);

	/* No guest server listening? Well, let's ignore the offer */
	if (!sk || sk->sk_state != SS_LISTEN) {
		ret = -ENXIO;
		goto out;
	}

	if (sk->sk_ack_backlog >= sk->sk_max_ack_backlog) {
		ret = -EMFILE;
		goto out;
	}

	new_sk = __hvsock_create(sock_net(sk), NULL, GFP_KERNEL, sk->sk_type);
	if (!new_sk) {
		ret = -ENOMEM;
		goto out;
	}

	new_hvsk = sk_to_hvsock(new_sk);
	new_sk->sk_state = SS_CONNECTING;
	hvsock_addr_init(&new_hvsk->local_addr, *service_id);
	hvsock_addr_init(&new_hvsk->remote_addr, *instance);

	set_channel_read_mode(channel, HV_CALL_DIRECT);
	new_hvsk->channel = channel;
	vmbus_set_chn_rescind_callback(channel, hvsock_close_connection);
	ret = vmbus_open(channel, VMBUS_RINGBUFFER_SIZE_HVSOCK_SEND,
			 VMBUS_RINGBUFFER_SIZE_HVSOCK_RECV, NULL, 0,
			 hvsock_on_channel_cb, new_sk);
	if (ret != 0) {
		new_hvsk->channel = NULL;
		sock_put(new_sk);
		goto out;
	}
	set_channel_pending_send_size(channel, HVSOCK_PKT_LEN(PAGE_SIZE));

	new_sk->sk_state = SS_CONNECTED;
	hvsock_insert_connected(new_hvsk);
	hvsock_enqueue_accept(sk, new_sk);
	sk->sk_state_change(sk);
out:
	mutex_unlock(&hvsock_mutex);
	return ret;
}

static void hvsock_connect_timeout(struct work_struct *work)
{
	struct hvsock_sock *hvsk;
	struct sock *sk;

	hvsk = container_of(work, struct hvsock_sock, dwork.work);
	sk = hvsock_to_sk(hvsk);

	lock_sock(sk);
	if ((sk->sk_state == SS_CONNECTING) &&
	    (sk->sk_shutdown != SHUTDOWN_MASK)) {
		sk->sk_state = SS_UNCONNECTED;
		sk->sk_err = ETIMEDOUT;
		sk->sk_error_report(sk);
	}
	release_sock(sk);

	sock_put(sk);
}

static int hvsock_connect(struct socket *sock, struct sockaddr *addr,
			  int addr_len, int flags)
{
	struct sockaddr_hv *remote_addr;
	struct hvsock_sock *hvsk;
	struct sock *sk;

	DEFINE_WAIT(wait);
	long timeout;

	int ret = 0;

	sk = sock->sk;
	hvsk = sk_to_hvsock(sk);

	lock_sock(sk);

	switch (sock->state) {
	case SS_CONNECTED:
		ret = -EISCONN;
		goto out;
	case SS_DISCONNECTING:
		ret = -EINVAL;
		goto out;
	case SS_CONNECTING:
		/* This continues on so we can move sock into the SS_CONNECTED
		 * state once the connection has completed (at which point err
		 * will be set to zero also).  Otherwise, we will either wait
		 * for the connection or return -EALREADY should this be a
		 * non-blocking call.
		 */
		ret = -EALREADY;
		break;
	default:
		if ((sk->sk_state == SS_LISTEN) ||
		    hvsock_addr_cast(addr, addr_len, &remote_addr) != 0) {
			ret = -EINVAL;
			goto out;
		}

		/* Set the remote address that we are connecting to. */
		memcpy(&hvsk->remote_addr, remote_addr,
		       sizeof(hvsk->remote_addr));

		ret = hvsock_auto_bind(hvsk);
		if (ret)
			goto out;

		sk->sk_state = SS_CONNECTING;

		ret = vmbus_send_tl_connect_request(
					&hvsk->local_addr.shv_service_id,
					&hvsk->remote_addr.shv_service_id);
		if (ret < 0)
			goto out;

		/* Mark sock as connecting and set the error code to in
		 * progress in case this is a non-blocking connect.
		 */
		sock->state = SS_CONNECTING;
		ret = -EINPROGRESS;
	}

	/* The receive path will handle all communication until we are able to
	 * enter the connected state.  Here we wait for the connection to be
	 * completed or a notification of an error.
	 */
	timeout = 30 * HZ;
	prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);

	while (sk->sk_state != SS_CONNECTED && sk->sk_err == 0) {
		if (flags & O_NONBLOCK) {
			/* If we're not going to block, we schedule a timeout
			 * function to generate a timeout on the connection
			 * attempt, in case the peer doesn't respond in a
			 * timely manner. We hold on to the socket until the
			 * timeout fires.
			 */
			sock_hold(sk);
			INIT_DELAYED_WORK(&hvsk->dwork,
					  hvsock_connect_timeout);
			schedule_delayed_work(&hvsk->dwork, timeout);

			/* Skip ahead to preserve error code set above. */
			goto out_wait;
		}

		release_sock(sk);
		timeout = schedule_timeout(timeout);
		lock_sock(sk);

		if (signal_pending(current)) {
			ret = sock_intr_errno(timeout);
			goto out_wait_error;
		} else if (timeout == 0) {
			ret = -ETIMEDOUT;
			goto out_wait_error;
		}

		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
	}

	ret = sk->sk_err ? -sk->sk_err : 0;

out_wait_error:
	if (ret < 0) {
		sk->sk_state = SS_UNCONNECTED;
		sock->state = SS_UNCONNECTED;
	}
out_wait:
	finish_wait(sk_sleep(sk), &wait);
out:
	release_sock(sk);
	return ret;
}

static
int hvsock_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct hvsock_sock *hvconnected;
	struct sock *connected;
	struct sock *listener;

	DEFINE_WAIT(wait);
	long timeout;

	int ret = 0;

	listener = sock->sk;

	lock_sock(listener);

	if (sock->type != SOCK_STREAM) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (listener->sk_state != SS_LISTEN) {
		ret = -EINVAL;
		goto out;
	}

	/* Wait for children sockets to appear; these are the new sockets
	 * created upon connection establishment.
	 */
	timeout = sock_sndtimeo(listener, flags & O_NONBLOCK);
	prepare_to_wait(sk_sleep(listener), &wait, TASK_INTERRUPTIBLE);

	while ((connected = hvsock_dequeue_accept(listener)) == NULL &&
	       listener->sk_err == 0) {
		release_sock(listener);
		timeout = schedule_timeout(timeout);
		lock_sock(listener);

		if (signal_pending(current)) {
			ret = sock_intr_errno(timeout);
			goto out_wait;
		} else if (timeout == 0) {
			ret = -EAGAIN;
			goto out_wait;
		}

		prepare_to_wait(sk_sleep(listener), &wait, TASK_INTERRUPTIBLE);
	}

	if (listener->sk_err)
		ret = -listener->sk_err;

	if (connected) {
		lock_sock(connected);
		hvconnected = sk_to_hvsock(connected);

		/* If the listener socket has received an error, then we should
		 * reject this socket and return.  Note that we simply mark the
		 * socket rejected, drop our reference, and let the cleanup
		 * function handle the cleanup; the fact that we found it in
		 * the listener's accept queue guarantees that the cleanup
		 * function hasn't run yet.
		 */
		if (ret) {
			release_sock(connected);
			sock_put(connected);
			goto out_wait;
		}

		newsock->state = SS_CONNECTED;
		sock_graft(connected, newsock);
		release_sock(connected);
		sock_put(connected);
	}

out_wait:
	finish_wait(sk_sleep(listener), &wait);
out:
	release_sock(listener);
	return ret;
}

static int hvsock_listen(struct socket *sock, int backlog)
{
	struct hvsock_sock *hvsk;
	struct sock *sk;
	int ret = 0;

	sk = sock->sk;
	lock_sock(sk);

	if (sock->type != SOCK_STREAM) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (sock->state != SS_UNCONNECTED) {
		ret = -EINVAL;
		goto out;
	}

	if (backlog <= 0) {
		ret = -EINVAL;
		goto out;
	}
	/* This is an artificial limit */
	if (backlog > 128)
		backlog = 128;

	hvsk = sk_to_hvsock(sk);
	if (!hvsock_addr_bound(&hvsk->local_addr)) {
		ret = -EINVAL;
		goto out;
	}

	sk->sk_ack_backlog = 0;
	sk->sk_max_ack_backlog = backlog;
	sk->sk_state = SS_LISTEN;
out:
	release_sock(sk);
	return ret;
}

static int hvsock_setsockopt(struct socket *sock,
			     int level,
			     int optname,
			     char __user *optval, unsigned int optlen)
{
	return -ENOPROTOOPT;
}

static int hvsock_getsockopt(struct socket *sock,
			     int level,
			     int optname,
			     char __user *optval, int __user *optlen)
{
	return -ENOPROTOOPT;
}

static int hvsock_send_data(struct vmbus_channel *channel,
			    struct hvsock_sock *hvsk,
			    size_t to_write)
{
	hvsk->send.hdr.pkt_type = 1;
	hvsk->send.hdr.data_size = to_write;
	return vmbus_sendpacket(channel, &hvsk->send.hdr,
				sizeof(hvsk->send.hdr) + to_write,
				0, VM_PKT_DATA_INBAND, 0);
}

static int hvsock_sendmsg(struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len)
{
	struct vmbus_channel *channel;
	struct hvsock_sock *hvsk;
	struct sock *sk;

	size_t total_to_write = len;
	size_t total_written = 0;

	bool can_write;
	long timeout;
	int ret = 0;

	DEFINE_WAIT(wait);

	if (len == 0)
		return -EINVAL;

	if (msg->msg_flags & ~MSG_DONTWAIT) {
		pr_err("hvsock_sendmsg: unsupported flags=0x%x\n",
		       msg->msg_flags);
		return -EOPNOTSUPP;
	}

	sk = sock->sk;
	hvsk = sk_to_hvsock(sk);
	channel = hvsk->channel;

	lock_sock(sk);

	/* Callers should not provide a destination with stream sockets. */
	if (msg->msg_namelen) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* Send data only if both sides are not shutdown in the direction. */
	if (sk->sk_shutdown & SEND_SHUTDOWN ||
	    hvsk->peer_shutdown & RCV_SHUTDOWN) {
		ret = -EPIPE;
		goto out;
	}

	if (sk->sk_state != SS_CONNECTED ||
	    !hvsock_addr_bound(&hvsk->local_addr)) {
		ret = -ENOTCONN;
		goto out;
	}

	if (!hvsock_addr_bound(&hvsk->remote_addr)) {
		ret = -EDESTADDRREQ;
		goto out;
	}

	timeout = sock_sndtimeo(sk, msg->msg_flags & MSG_DONTWAIT);

	prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);

	while (total_to_write > 0) {
		size_t to_write;

		while (1) {
			get_ringbuffer_rw_status(channel, NULL, &can_write);

			if (can_write || sk->sk_err != 0 ||
			    (sk->sk_shutdown & SEND_SHUTDOWN) ||
			    (hvsk->peer_shutdown & RCV_SHUTDOWN))
				break;

			/* Don't wait for non-blocking sockets. */
			if (timeout == 0) {
				ret = -EAGAIN;
				goto out_wait;
			}

			release_sock(sk);

			timeout = schedule_timeout(timeout);

			lock_sock(sk);
			if (signal_pending(current)) {
				ret = sock_intr_errno(timeout);
				goto out_wait;
			} else if (timeout == 0) {
				ret = -EAGAIN;
				goto out_wait;
			}

			prepare_to_wait(sk_sleep(sk), &wait,
					TASK_INTERRUPTIBLE);
		}

		/* These checks occur both as part of and after the loop
		 * conditional since we need to check before and after
		 * sleeping.
		 */
		if (sk->sk_err) {
			ret = -sk->sk_err;
			goto out_wait;
		} else if ((sk->sk_shutdown & SEND_SHUTDOWN) ||
			   (hvsk->peer_shutdown & RCV_SHUTDOWN)) {
			ret = -EPIPE;
			goto out_wait;
		}

		/* Note: that write will only write as many bytes as possible
		 * in the ringbuffer. It is the caller's responsibility to
		 * check how many bytes we actually wrote.
		 */
		do {
			to_write = min_t(size_t, HVSOCK_SND_BUF_SZ,
					 total_to_write);
			ret = memcpy_from_msg(hvsk->send.buf, msg, to_write);
			if (ret != 0)
				goto out_wait;

			ret = hvsock_send_data(channel, hvsk, to_write);
			if (ret != 0)
				goto out_wait;

			total_written += to_write;
			total_to_write -= to_write;
		} while (total_to_write > 0);
	}
out_wait:
	if (total_written > 0)
		ret = total_written;

	finish_wait(sk_sleep(sk), &wait);
out:
	release_sock(sk);

	/* ret is a bigger-than-0 total_written or a negative err code. */
	if (ret == 0) {
		WARN(1, "unexpected return value of 0\n");
		ret = -EIO;
	}

	return ret;
}

static int hvsock_recv_data(struct vmbus_channel *channel,
			    struct hvsock_sock *hvsk,
			    size_t *payload_len)
{
	u32 buffer_actual_len;
	u64 dummy_req_id;
	int ret;

	ret = vmbus_recvpacket(channel, &hvsk->recv.hdr,
			       sizeof(hvsk->recv.hdr) + sizeof(hvsk->recv.buf),
			       &buffer_actual_len, &dummy_req_id);
	if (ret != 0 || buffer_actual_len <= sizeof(hvsk->recv.hdr))
		*payload_len = 0;
	else
		*payload_len = hvsk->recv.hdr.data_size;

	return ret;
}

static int hvsock_recvmsg(struct kiocb *iocb, struct socket *sock, struct msghdr *msg,
			  size_t len, int flags)
{
	struct vmbus_channel *channel;
	struct hvsock_sock *hvsk;
	struct sock *sk;

	size_t total_to_read = len;
	size_t copied;

	bool can_read;
	long timeout;

	int ret = 0;

	DEFINE_WAIT(wait);

	sk = sock->sk;
	hvsk = sk_to_hvsock(sk);
	channel = hvsk->channel;

	lock_sock(sk);

	if (sk->sk_state != SS_CONNECTED) {
		/* Recvmsg is supposed to return 0 if a peer performs an
		 * orderly shutdown. Differentiate between that case and when a
		 * peer has not connected or a local shutdown occurred with the
		 * SOCK_DONE flag.
		 */
		if (sock_flag(sk, SOCK_DONE))
			ret = 0;
		else
			ret = -ENOTCONN;

		goto out;
	}

	/* We ignore msg->addr_name/len. */
	if (flags & ~MSG_DONTWAIT) {
		pr_err("hvsock_recvmsg: unsupported flags=0x%x\n", flags);
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* We don't check peer_shutdown flag here since peer may actually shut
	 * down, but there can be data in the queue that a local socket can
	 * receive.
	 */
	if (sk->sk_shutdown & RCV_SHUTDOWN) {
		ret = 0;
		goto out;
	}

	/* It is valid on Linux to pass in a zero-length receive buffer.  This
	 * is not an error.  We may as well bail out now.
	 */
	if (!len) {
		ret = 0;
		goto out;
	}

	timeout = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
	copied = 0;

	prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);

	while (1) {
		bool need_refill = hvsk->recv.data_len == 0;

		if (need_refill)
			get_ringbuffer_rw_status(channel, &can_read, NULL);
		else
			can_read = true;

		if (can_read) {
			size_t payload_len;

			if (need_refill) {
				ret = hvsock_recv_data(channel, hvsk,
						       &payload_len);
				if (ret != 0 || payload_len == 0 ||
				    payload_len > HVSOCK_RCV_BUF_SZ) {
					ret = -EIO;
					goto out_wait;
				}

				hvsk->recv.data_len = payload_len;
				hvsk->recv.data_offset = 0;
			}

			if (hvsk->recv.data_len <= total_to_read) {
				ret = memcpy_to_msg(msg, hvsk->recv.buf +
						    hvsk->recv.data_offset,
						    hvsk->recv.data_len);
				if (ret != 0)
					break;

				copied += hvsk->recv.data_len;
				total_to_read -= hvsk->recv.data_len;
				hvsk->recv.data_len = 0;
				hvsk->recv.data_offset = 0;

				if (total_to_read == 0)
					break;
			} else {
				ret = memcpy_to_msg(msg, hvsk->recv.buf +
						    hvsk->recv.data_offset,
						    total_to_read);
				if (ret != 0)
					break;

				copied += total_to_read;
				hvsk->recv.data_len -= total_to_read;
				hvsk->recv.data_offset += total_to_read;
				total_to_read = 0;
				break;
			}
		} else {
			if (sk->sk_err || (sk->sk_shutdown & RCV_SHUTDOWN) ||
			    (hvsk->peer_shutdown & SEND_SHUTDOWN))
				break;

			/* Don't wait for non-blocking sockets. */
			if (timeout == 0) {
				ret = -EAGAIN;
				break;
			}

			if (copied > 0)
				break;

			release_sock(sk);
			timeout = schedule_timeout(timeout);
			lock_sock(sk);

			if (signal_pending(current)) {
				ret = sock_intr_errno(timeout);
				break;
			} else if (timeout == 0) {
				ret = -EAGAIN;
				break;
			}

			prepare_to_wait(sk_sleep(sk), &wait,
					TASK_INTERRUPTIBLE);
		}
	}

	if (sk->sk_err)
		ret = -sk->sk_err;
	else if (sk->sk_shutdown & RCV_SHUTDOWN)
		ret = 0;

	if (copied > 0) {
		ret = copied;

		/* If the other side has shutdown for sending and there
		 * is nothing more to read, then we modify the socket
		 * state.
		 */
		if ((hvsk->peer_shutdown & SEND_SHUTDOWN) &&
		    hvsk->recv.data_len == 0) {
			get_ringbuffer_rw_status(channel, &can_read, NULL);
			if (!can_read) {
				sk->sk_state = SS_UNCONNECTED;
				sock_set_flag(sk, SOCK_DONE);
				sk->sk_state_change(sk);
			}
		}
	}
out_wait:
	finish_wait(sk_sleep(sk), &wait);
out:
	release_sock(sk);
	return ret;
}

static const struct proto_ops hvsock_ops = {
	.family = PF_HYPERV,
	.owner = THIS_MODULE,
	.release = hvsock_release,
	.bind = hvsock_bind,
	.connect = hvsock_connect,
	.socketpair = sock_no_socketpair,
	.accept = hvsock_accept,
	.getname = hvsock_getname,
	.poll = hvsock_poll,
	.ioctl = sock_no_ioctl,
	.listen = hvsock_listen,
	.shutdown = hvsock_shutdown,
	.setsockopt = hvsock_setsockopt,
	.getsockopt = hvsock_getsockopt,
	.sendmsg = hvsock_sendmsg,
	.recvmsg = hvsock_recvmsg,
	.mmap = sock_no_mmap,
	.sendpage = sock_no_sendpage,
};

static int hvsock_create(struct net *net, struct socket *sock,
			 int protocol, int kern)
{
	if (!capable(CAP_SYS_ADMIN) && !capable(CAP_NET_ADMIN))
		return -EPERM;

	if (protocol != 0 && protocol != SHV_PROTO_RAW)
		return -EPROTONOSUPPORT;

	switch (sock->type) {
	case SOCK_STREAM:
		sock->ops = &hvsock_ops;
		break;
	default:
		return -ESOCKTNOSUPPORT;
	}

	sock->state = SS_UNCONNECTED;

	return __hvsock_create(net, sock, GFP_KERNEL, 0) ? 0 : -ENOMEM;
}

static const struct net_proto_family hvsock_family_ops = {
	.family = AF_HYPERV,
	.create = hvsock_create,
	.owner = THIS_MODULE,
};

static int hvsock_probe(struct hv_device *hdev,
			const struct hv_vmbus_device_id *dev_id)
{
	struct vmbus_channel *channel = hdev->channel;

	/* We ignore the error return code to suppress the unnecessary
	 * error message in vmbus_probe(): on error the host will rescind
	 * the offer in 30 seconds and we can do cleanup at that time.
	 */
	(void)hvsock_open_connection(channel);

	return 0;
}

static int hvsock_remove(struct hv_device *hdev)
{
	struct vmbus_channel *channel = hdev->channel;

	vmbus_close(channel);

	return 0;
}

/* It's not really used. See vmbus_match() and vmbus_probe(). */
static const struct hv_vmbus_device_id id_table[] = {
	{},
};

static struct hv_driver hvsock_drv = {
	.name		= "hv_sock",
	.hvsock		= true,
	.id_table	= id_table,
	.probe		= hvsock_probe,
	.remove		= hvsock_remove,
};

static int __init hvsock_init(void)
{
	int ret;

	/* Hyper-V socket requires at least VMBus 4.0 */
	if ((vmbus_proto_version >> 16) < 4) {
		pr_err("failed to load: VMBus 4 or later is required\n");
		return -ENODEV;
	}

	ret = vmbus_driver_register(&hvsock_drv);
	if (ret) {
		pr_err("failed to register hv_sock driver\n");
		return ret;
	}

	ret = proto_register(&hvsock_proto, 0);
	if (ret) {
		pr_err("failed to register protocol\n");
		goto unreg_hvsock_drv;
	}

	ret = sock_register(&hvsock_family_ops);
	if (ret) {
		pr_err("failed to register address family\n");
		goto unreg_proto;
	}

	return 0;

unreg_proto:
	proto_unregister(&hvsock_proto);
unreg_hvsock_drv:
	vmbus_driver_unregister(&hvsock_drv);
	return ret;
}

static void __exit hvsock_exit(void)
{
	sock_unregister(AF_HYPERV);
	proto_unregister(&hvsock_proto);
	vmbus_driver_unregister(&hvsock_drv);
}

module_init(hvsock_init);
module_exit(hvsock_exit);

MODULE_DESCRIPTION("Hyper-V Sockets");
MODULE_LICENSE("Dual BSD/GPL");
