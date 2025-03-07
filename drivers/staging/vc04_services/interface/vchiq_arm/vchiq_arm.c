// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (c) 2014 Raspberry Pi (Trading) Ltd. All rights reserved.
 * Copyright (c) 2010-2012 Broadcom. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/bug.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/compat.h>
#include <linux/dma-mapping.h>
#include <linux/rcupdate.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#include "vchiq_core.h"
#include "vchiq_ioctl.h"
#include "vchiq_arm.h"
#include "vchiq_debugfs.h"

#define DEVICE_NAME "vchiq"

/* Override the default prefix, which would be vchiq_arm (from the filename) */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX DEVICE_NAME "."

#define KEEPALIVE_VER 1
#define KEEPALIVE_VER_MIN KEEPALIVE_VER

/* Run time control of log level, based on KERN_XXX level. */
int vchiq_arm_log_level = VCHIQ_LOG_DEFAULT;
int vchiq_susp_log_level = VCHIQ_LOG_ERROR;

DEFINE_SPINLOCK(msg_queue_spinlock);
struct vchiq_state g_state;

static struct platform_device *bcm2835_camera;
static struct platform_device *bcm2835_audio;

static struct vchiq_drvdata bcm2835_drvdata = {
	.cache_line_size = 32,
};

static struct vchiq_drvdata bcm2836_drvdata = {
	.cache_line_size = 64,
};

static enum vchiq_status
vchiq_blocking_bulk_transfer(unsigned int handle, void *data,
	unsigned int size, enum vchiq_bulk_dir dir);

#define VCHIQ_INIT_RETRIES 10
int vchiq_initialise(struct vchiq_instance **instance_out)
{
	struct vchiq_state *state;
	struct vchiq_instance *instance = NULL;
	int i, ret;

	/*
	 * VideoCore may not be ready due to boot up timing.
	 * It may never be ready if kernel and firmware are mismatched,so don't
	 * block forever.
	 */
	for (i = 0; i < VCHIQ_INIT_RETRIES; i++) {
		state = vchiq_get_state();
		if (state)
			break;
		usleep_range(500, 600);
	}
	if (i == VCHIQ_INIT_RETRIES) {
		vchiq_log_error(vchiq_core_log_level,
			"%s: videocore not initialized\n", __func__);
		ret = -ENOTCONN;
		goto failed;
	} else if (i > 0) {
		vchiq_log_warning(vchiq_core_log_level,
			"%s: videocore initialized after %d retries\n",
			__func__, i);
	}

	instance = kzalloc(sizeof(*instance), GFP_KERNEL);
	if (!instance) {
		vchiq_log_error(vchiq_core_log_level,
			"%s: error allocating vchiq instance\n", __func__);
		ret = -ENOMEM;
		goto failed;
	}

	instance->connected = 0;
	instance->state = state;
	mutex_init(&instance->bulk_waiter_list_mutex);
	INIT_LIST_HEAD(&instance->bulk_waiter_list);

	*instance_out = instance;

	ret = 0;

failed:
	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, ret);

	return ret;
}
EXPORT_SYMBOL(vchiq_initialise);

void free_bulk_waiter(struct vchiq_instance *instance)
{
	struct bulk_waiter_node *waiter, *next;

	list_for_each_entry_safe(waiter, next,
				 &instance->bulk_waiter_list, list) {
		list_del(&waiter->list);
		vchiq_log_info(vchiq_arm_log_level,
				"bulk_waiter - cleaned up %pK for pid %d",
				waiter, waiter->pid);
		kfree(waiter);
	}
}

enum vchiq_status vchiq_shutdown(struct vchiq_instance *instance)
{
	enum vchiq_status status = VCHIQ_SUCCESS;
	struct vchiq_state *state = instance->state;

	if (mutex_lock_killable(&state->mutex))
		return VCHIQ_RETRY;

	/* Remove all services */
	vchiq_shutdown_internal(state, instance);

	mutex_unlock(&state->mutex);

	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	free_bulk_waiter(instance);
	kfree(instance);

	return status;
}
EXPORT_SYMBOL(vchiq_shutdown);

static int vchiq_is_connected(struct vchiq_instance *instance)
{
	return instance->connected;
}

enum vchiq_status vchiq_connect(struct vchiq_instance *instance)
{
	enum vchiq_status status;
	struct vchiq_state *state = instance->state;

	if (mutex_lock_killable(&state->mutex)) {
		vchiq_log_trace(vchiq_core_log_level,
			"%s: call to mutex_lock failed", __func__);
		status = VCHIQ_RETRY;
		goto failed;
	}
	status = vchiq_connect_internal(state, instance);

	if (status == VCHIQ_SUCCESS)
		instance->connected = 1;

	mutex_unlock(&state->mutex);

failed:
	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	return status;
}
EXPORT_SYMBOL(vchiq_connect);

static enum vchiq_status
vchiq_add_service(struct vchiq_instance *instance,
		  const struct vchiq_service_params_kernel *params,
		  unsigned int *phandle)
{
	enum vchiq_status status;
	struct vchiq_state *state = instance->state;
	struct vchiq_service *service = NULL;
	int srvstate;

	*phandle = VCHIQ_SERVICE_HANDLE_INVALID;

	srvstate = vchiq_is_connected(instance)
		? VCHIQ_SRVSTATE_LISTENING
		: VCHIQ_SRVSTATE_HIDDEN;

	service = vchiq_add_service_internal(
		state,
		params,
		srvstate,
		instance,
		NULL);

	if (service) {
		*phandle = service->handle;
		status = VCHIQ_SUCCESS;
	} else {
		status = VCHIQ_ERROR;
	}

	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	return status;
}

enum vchiq_status
vchiq_open_service(struct vchiq_instance *instance,
		   const struct vchiq_service_params_kernel *params,
		   unsigned int *phandle)
{
	enum vchiq_status   status = VCHIQ_ERROR;
	struct vchiq_state   *state = instance->state;
	struct vchiq_service *service = NULL;

	*phandle = VCHIQ_SERVICE_HANDLE_INVALID;

	if (!vchiq_is_connected(instance))
		goto failed;

	service = vchiq_add_service_internal(state,
		params,
		VCHIQ_SRVSTATE_OPENING,
		instance,
		NULL);

	if (service) {
		*phandle = service->handle;
		status = vchiq_open_service_internal(service, current->pid);
		if (status != VCHIQ_SUCCESS) {
			vchiq_remove_service(service->handle);
			*phandle = VCHIQ_SERVICE_HANDLE_INVALID;
		}
	}

failed:
	vchiq_log_trace(vchiq_core_log_level,
		"%s(%p): returning %d", __func__, instance, status);

	return status;
}
EXPORT_SYMBOL(vchiq_open_service);

enum vchiq_status
vchiq_bulk_transmit(unsigned int handle, const void *data, unsigned int size,
		    void *userdata, enum vchiq_bulk_mode mode)
{
	enum vchiq_status status;

	while (1) {
		switch (mode) {
		case VCHIQ_BULK_MODE_NOCALLBACK:
		case VCHIQ_BULK_MODE_CALLBACK:
			status = vchiq_bulk_transfer(handle,
						     (void *)data, NULL,
						     size, userdata, mode,
						     VCHIQ_BULK_TRANSMIT);
			break;
		case VCHIQ_BULK_MODE_BLOCKING:
			status = vchiq_blocking_bulk_transfer(handle,
				(void *)data, size, VCHIQ_BULK_TRANSMIT);
			break;
		default:
			return VCHIQ_ERROR;
		}

		/*
		 * vchiq_*_bulk_transfer() may return VCHIQ_RETRY, so we need
		 * to implement a retry mechanism since this function is
		 * supposed to block until queued
		 */
		if (status != VCHIQ_RETRY)
			break;

		msleep(1);
	}

	return status;
}
EXPORT_SYMBOL(vchiq_bulk_transmit);

enum vchiq_status vchiq_bulk_receive(unsigned int handle, void *data,
				     unsigned int size, void *userdata,
				     enum vchiq_bulk_mode mode)
{
	enum vchiq_status status;

	while (1) {
		switch (mode) {
		case VCHIQ_BULK_MODE_NOCALLBACK:
		case VCHIQ_BULK_MODE_CALLBACK:
			status = vchiq_bulk_transfer(handle, data, NULL,
						     size, userdata,
						     mode, VCHIQ_BULK_RECEIVE);
			break;
		case VCHIQ_BULK_MODE_BLOCKING:
			status = vchiq_blocking_bulk_transfer(handle,
				(void *)data, size, VCHIQ_BULK_RECEIVE);
			break;
		default:
			return VCHIQ_ERROR;
		}

		/*
		 * vchiq_*_bulk_transfer() may return VCHIQ_RETRY, so we need
		 * to implement a retry mechanism since this function is
		 * supposed to block until queued
		 */
		if (status != VCHIQ_RETRY)
			break;

		msleep(1);
	}

	return status;
}
EXPORT_SYMBOL(vchiq_bulk_receive);

static enum vchiq_status
vchiq_blocking_bulk_transfer(unsigned int handle, void *data, unsigned int size,
			     enum vchiq_bulk_dir dir)
{
	struct vchiq_instance *instance;
	struct vchiq_service *service;
	enum vchiq_status status;
	struct bulk_waiter_node *waiter = NULL;
	bool found = false;

	service = find_service_by_handle(handle);
	if (!service)
		return VCHIQ_ERROR;

	instance = service->instance;

	vchiq_service_put(service);

	mutex_lock(&instance->bulk_waiter_list_mutex);
	list_for_each_entry(waiter, &instance->bulk_waiter_list, list) {
		if (waiter->pid == current->pid) {
			list_del(&waiter->list);
			found = true;
			break;
		}
	}
	mutex_unlock(&instance->bulk_waiter_list_mutex);

	if (found) {
		struct vchiq_bulk *bulk = waiter->bulk_waiter.bulk;

		if (bulk) {
			/* This thread has an outstanding bulk transfer. */
			/* FIXME: why compare a dma address to a pointer? */
			if ((bulk->data != (dma_addr_t)(uintptr_t)data) ||
				(bulk->size != size)) {
				/*
				 * This is not a retry of the previous one.
				 * Cancel the signal when the transfer completes.
				 */
				spin_lock(&bulk_waiter_spinlock);
				bulk->userdata = NULL;
				spin_unlock(&bulk_waiter_spinlock);
			}
		}
	} else {
		waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
		if (!waiter) {
			vchiq_log_error(vchiq_core_log_level,
				"%s - out of memory", __func__);
			return VCHIQ_ERROR;
		}
	}

	status = vchiq_bulk_transfer(handle, data, NULL, size,
				     &waiter->bulk_waiter,
				     VCHIQ_BULK_MODE_BLOCKING, dir);
	if ((status != VCHIQ_RETRY) || fatal_signal_pending(current) ||
		!waiter->bulk_waiter.bulk) {
		struct vchiq_bulk *bulk = waiter->bulk_waiter.bulk;

		if (bulk) {
			/* Cancel the signal when the transfer completes. */
			spin_lock(&bulk_waiter_spinlock);
			bulk->userdata = NULL;
			spin_unlock(&bulk_waiter_spinlock);
		}
		kfree(waiter);
	} else {
		waiter->pid = current->pid;
		mutex_lock(&instance->bulk_waiter_list_mutex);
		list_add(&waiter->list, &instance->bulk_waiter_list);
		mutex_unlock(&instance->bulk_waiter_list_mutex);
		vchiq_log_info(vchiq_arm_log_level,
				"saved bulk_waiter %pK for pid %d",
				waiter, current->pid);
	}

	return status;
}

static enum vchiq_status
add_completion(struct vchiq_instance *instance, enum vchiq_reason reason,
	       struct vchiq_header *header, struct user_service *user_service,
	       void *bulk_userdata)
{
	struct vchiq_completion_data_kernel *completion;
	int insert;

	DEBUG_INITIALISE(g_state.local)

	insert = instance->completion_insert;
	while ((insert - instance->completion_remove) >= MAX_COMPLETIONS) {
		/* Out of space - wait for the client */
		DEBUG_TRACE(SERVICE_CALLBACK_LINE);
		vchiq_log_trace(vchiq_arm_log_level,
			"%s - completion queue full", __func__);
		DEBUG_COUNT(COMPLETION_QUEUE_FULL_COUNT);
		if (wait_for_completion_interruptible(
					&instance->remove_event)) {
			vchiq_log_info(vchiq_arm_log_level,
				"service_callback interrupted");
			return VCHIQ_RETRY;
		} else if (instance->closing) {
			vchiq_log_info(vchiq_arm_log_level,
				"service_callback closing");
			return VCHIQ_SUCCESS;
		}
		DEBUG_TRACE(SERVICE_CALLBACK_LINE);
	}

	completion = &instance->completions[insert & (MAX_COMPLETIONS - 1)];

	completion->header = header;
	completion->reason = reason;
	/* N.B. service_userdata is updated while processing AWAIT_COMPLETION */
	completion->service_userdata = user_service->service;
	completion->bulk_userdata = bulk_userdata;

	if (reason == VCHIQ_SERVICE_CLOSED) {
		/*
		 * Take an extra reference, to be held until
		 * this CLOSED notification is delivered.
		 */
		vchiq_service_get(user_service->service);
		if (instance->use_close_delivered)
			user_service->close_pending = 1;
	}

	/*
	 * A write barrier is needed here to ensure that the entire completion
	 * record is written out before the insert point.
	 */
	wmb();

	if (reason == VCHIQ_MESSAGE_AVAILABLE)
		user_service->message_available_pos = insert;

	insert++;
	instance->completion_insert = insert;

	complete(&instance->insert_event);

	return VCHIQ_SUCCESS;
}

enum vchiq_status
service_callback(enum vchiq_reason reason, struct vchiq_header *header,
		 unsigned int handle, void *bulk_userdata)
{
	/*
	 * How do we ensure the callback goes to the right client?
	 * The service_user data points to a user_service record
	 * containing the original callback and the user state structure, which
	 * contains a circular buffer for completion records.
	 */
	struct user_service *user_service;
	struct vchiq_service *service;
	struct vchiq_instance *instance;
	bool skip_completion = false;

	DEBUG_INITIALISE(g_state.local)

	DEBUG_TRACE(SERVICE_CALLBACK_LINE);

	service = handle_to_service(handle);
	if (WARN_ON(!service))
		return VCHIQ_SUCCESS;

	user_service = (struct user_service *)service->base.userdata;
	instance = user_service->instance;

	if (!instance || instance->closing)
		return VCHIQ_SUCCESS;

	vchiq_log_trace(vchiq_arm_log_level,
		"%s - service %lx(%d,%p), reason %d, header %lx, instance %lx, bulk_userdata %lx",
		__func__, (unsigned long)user_service,
		service->localport, user_service->userdata,
		reason, (unsigned long)header,
		(unsigned long)instance, (unsigned long)bulk_userdata);

	if (header && user_service->is_vchi) {
		spin_lock(&msg_queue_spinlock);
		while (user_service->msg_insert ==
			(user_service->msg_remove + MSG_QUEUE_SIZE)) {
			spin_unlock(&msg_queue_spinlock);
			DEBUG_TRACE(SERVICE_CALLBACK_LINE);
			DEBUG_COUNT(MSG_QUEUE_FULL_COUNT);
			vchiq_log_trace(vchiq_arm_log_level,
				"service_callback - msg queue full");
			/*
			 * If there is no MESSAGE_AVAILABLE in the completion
			 * queue, add one
			 */
			if ((user_service->message_available_pos -
				instance->completion_remove) < 0) {
				enum vchiq_status status;

				vchiq_log_info(vchiq_arm_log_level,
					"Inserting extra MESSAGE_AVAILABLE");
				DEBUG_TRACE(SERVICE_CALLBACK_LINE);
				status = add_completion(instance, reason,
					NULL, user_service, bulk_userdata);
				if (status != VCHIQ_SUCCESS) {
					DEBUG_TRACE(SERVICE_CALLBACK_LINE);
					return status;
				}
			}

			DEBUG_TRACE(SERVICE_CALLBACK_LINE);
			if (wait_for_completion_interruptible(
						&user_service->remove_event)) {
				vchiq_log_info(vchiq_arm_log_level,
					"%s interrupted", __func__);
				DEBUG_TRACE(SERVICE_CALLBACK_LINE);
				return VCHIQ_RETRY;
			} else if (instance->closing) {
				vchiq_log_info(vchiq_arm_log_level,
					"%s closing", __func__);
				DEBUG_TRACE(SERVICE_CALLBACK_LINE);
				return VCHIQ_ERROR;
			}
			DEBUG_TRACE(SERVICE_CALLBACK_LINE);
			spin_lock(&msg_queue_spinlock);
		}

		user_service->msg_queue[user_service->msg_insert &
			(MSG_QUEUE_SIZE - 1)] = header;
		user_service->msg_insert++;

		/*
		 * If there is a thread waiting in DEQUEUE_MESSAGE, or if
		 * there is a MESSAGE_AVAILABLE in the completion queue then
		 * bypass the completion queue.
		 */
		if (((user_service->message_available_pos -
			instance->completion_remove) >= 0) ||
			user_service->dequeue_pending) {
			user_service->dequeue_pending = 0;
			skip_completion = true;
		}

		spin_unlock(&msg_queue_spinlock);
		complete(&user_service->insert_event);

		header = NULL;
	}
	DEBUG_TRACE(SERVICE_CALLBACK_LINE);

	if (skip_completion)
		return VCHIQ_SUCCESS;

	return add_completion(instance, reason, header, user_service,
		bulk_userdata);
}

int vchiq_dump(void *dump_context, const char *str, int len)
{
	struct dump_context *context = (struct dump_context *)dump_context;
	int copy_bytes;

	if (context->actual >= context->space)
		return 0;

	if (context->offset > 0) {
		int skip_bytes = min_t(int, len, context->offset);

		str += skip_bytes;
		len -= skip_bytes;
		context->offset -= skip_bytes;
		if (context->offset > 0)
			return 0;
	}
	copy_bytes = min_t(int, len, context->space - context->actual);
	if (copy_bytes == 0)
		return 0;
	if (copy_to_user(context->buf + context->actual, str,
			 copy_bytes))
		return -EFAULT;
	context->actual += copy_bytes;
	len -= copy_bytes;

	/*
	 * If the terminating NUL is included in the length, then it
	 * marks the end of a line and should be replaced with a
	 * carriage return.
	 */
	if ((len == 0) && (str[copy_bytes - 1] == '\0')) {
		char cr = '\n';

		if (copy_to_user(context->buf + context->actual - 1,
				 &cr, 1))
			return -EFAULT;
	}
	return 0;
}

int vchiq_dump_platform_instances(void *dump_context)
{
	struct vchiq_state *state = vchiq_get_state();
	char buf[80];
	int len;
	int i;

	if (!state)
		return -ENOTCONN;

	/*
	 * There is no list of instances, so instead scan all services,
	 * marking those that have been dumped.
	 */

	rcu_read_lock();
	for (i = 0; i < state->unused_service; i++) {
		struct vchiq_service *service;
		struct vchiq_instance *instance;

		service = rcu_dereference(state->services[i]);
		if (!service || service->base.callback != service_callback)
			continue;

		instance = service->instance;
		if (instance)
			instance->mark = 0;
	}
	rcu_read_unlock();

	for (i = 0; i < state->unused_service; i++) {
		struct vchiq_service *service;
		struct vchiq_instance *instance;
		int err;

		rcu_read_lock();
		service = rcu_dereference(state->services[i]);
		if (!service || service->base.callback != service_callback) {
			rcu_read_unlock();
			continue;
		}

		instance = service->instance;
		if (!instance || instance->mark) {
			rcu_read_unlock();
			continue;
		}
		rcu_read_unlock();

		len = snprintf(buf, sizeof(buf),
			       "Instance %pK: pid %d,%s completions %d/%d",
			       instance, instance->pid,
			       instance->connected ? " connected, " :
			       "",
			       instance->completion_insert -
			       instance->completion_remove,
			       MAX_COMPLETIONS);
		err = vchiq_dump(dump_context, buf, len + 1);
		if (err)
			return err;
		instance->mark = 1;
	}
	return 0;
}

int vchiq_dump_platform_service_state(void *dump_context,
				      struct vchiq_service *service)
{
	struct user_service *user_service =
			(struct user_service *)service->base.userdata;
	char buf[80];
	int len;

	len = scnprintf(buf, sizeof(buf), "  instance %pK", service->instance);

	if ((service->base.callback == service_callback) &&
		user_service->is_vchi) {
		len += scnprintf(buf + len, sizeof(buf) - len,
			", %d/%d messages",
			user_service->msg_insert - user_service->msg_remove,
			MSG_QUEUE_SIZE);

		if (user_service->dequeue_pending)
			len += scnprintf(buf + len, sizeof(buf) - len,
				" (dequeue pending)");
	}

	return vchiq_dump(dump_context, buf, len + 1);
}

struct vchiq_state *
vchiq_get_state(void)
{

	if (!g_state.remote)
		pr_err("%s: g_state.remote == NULL\n", __func__);
	else if (g_state.remote->initialised != 1)
		pr_notice("%s: g_state.remote->initialised != 1 (%d)\n",
			  __func__, g_state.remote->initialised);

	return (g_state.remote &&
		(g_state.remote->initialised == 1)) ? &g_state : NULL;
}

/*
 * Autosuspend related functionality
 */

static enum vchiq_status
vchiq_keepalive_vchiq_callback(enum vchiq_reason reason,
			       struct vchiq_header *header,
			       unsigned int service_user, void *bulk_user)
{
	vchiq_log_error(vchiq_susp_log_level,
		"%s callback reason %d", __func__, reason);
	return 0;
}

static int
vchiq_keepalive_thread_func(void *v)
{
	struct vchiq_state *state = (struct vchiq_state *)v;
	struct vchiq_arm_state *arm_state = vchiq_platform_get_arm_state(state);

	enum vchiq_status status;
	struct vchiq_instance *instance;
	unsigned int ka_handle;
	int ret;

	struct vchiq_service_params_kernel params = {
		.fourcc      = VCHIQ_MAKE_FOURCC('K', 'E', 'E', 'P'),
		.callback    = vchiq_keepalive_vchiq_callback,
		.version     = KEEPALIVE_VER,
		.version_min = KEEPALIVE_VER_MIN
	};

	ret = vchiq_initialise(&instance);
	if (ret) {
		vchiq_log_error(vchiq_susp_log_level,
			"%s vchiq_initialise failed %d", __func__, ret);
		goto exit;
	}

	status = vchiq_connect(instance);
	if (status != VCHIQ_SUCCESS) {
		vchiq_log_error(vchiq_susp_log_level,
			"%s vchiq_connect failed %d", __func__, status);
		goto shutdown;
	}

	status = vchiq_add_service(instance, &params, &ka_handle);
	if (status != VCHIQ_SUCCESS) {
		vchiq_log_error(vchiq_susp_log_level,
			"%s vchiq_open_service failed %d", __func__, status);
		goto shutdown;
	}

	while (1) {
		long rc = 0, uc = 0;

		if (wait_for_completion_interruptible(&arm_state->ka_evt)) {
			vchiq_log_error(vchiq_susp_log_level,
				"%s interrupted", __func__);
			flush_signals(current);
			continue;
		}

		/*
		 * read and clear counters.  Do release_count then use_count to
		 * prevent getting more releases than uses
		 */
		rc = atomic_xchg(&arm_state->ka_release_count, 0);
		uc = atomic_xchg(&arm_state->ka_use_count, 0);

		/*
		 * Call use/release service the requisite number of times.
		 * Process use before release so use counts don't go negative
		 */
		while (uc--) {
			atomic_inc(&arm_state->ka_use_ack_count);
			status = vchiq_use_service(ka_handle);
			if (status != VCHIQ_SUCCESS) {
				vchiq_log_error(vchiq_susp_log_level,
					"%s vchiq_use_service error %d",
					__func__, status);
			}
		}
		while (rc--) {
			status = vchiq_release_service(ka_handle);
			if (status != VCHIQ_SUCCESS) {
				vchiq_log_error(vchiq_susp_log_level,
					"%s vchiq_release_service error %d",
					__func__, status);
			}
		}
	}

shutdown:
	vchiq_shutdown(instance);
exit:
	return 0;
}

void
vchiq_arm_init_state(struct vchiq_state *state,
		     struct vchiq_arm_state *arm_state)
{
	if (arm_state) {
		rwlock_init(&arm_state->susp_res_lock);

		init_completion(&arm_state->ka_evt);
		atomic_set(&arm_state->ka_use_count, 0);
		atomic_set(&arm_state->ka_use_ack_count, 0);
		atomic_set(&arm_state->ka_release_count, 0);

		arm_state->state = state;
		arm_state->first_connect = 0;

	}
}

int
vchiq_use_internal(struct vchiq_state *state, struct vchiq_service *service,
		   enum USE_TYPE_E use_type)
{
	struct vchiq_arm_state *arm_state = vchiq_platform_get_arm_state(state);
	int ret = 0;
	char entity[16];
	int *entity_uc;
	int local_uc;

	if (!arm_state) {
		ret = -EINVAL;
		goto out;
	}

	if (use_type == USE_TYPE_VCHIQ) {
		sprintf(entity, "VCHIQ:   ");
		entity_uc = &arm_state->peer_use_count;
	} else if (service) {
		sprintf(entity, "%c%c%c%c:%03d",
			VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc),
			service->client_id);
		entity_uc = &service->service_use_count;
	} else {
		vchiq_log_error(vchiq_susp_log_level, "%s null service ptr", __func__);
		ret = -EINVAL;
		goto out;
	}

	write_lock_bh(&arm_state->susp_res_lock);
	local_uc = ++arm_state->videocore_use_count;
	++(*entity_uc);

	vchiq_log_trace(vchiq_susp_log_level,
		"%s %s count %d, state count %d",
		__func__, entity, *entity_uc, local_uc);

	write_unlock_bh(&arm_state->susp_res_lock);

	if (!ret) {
		enum vchiq_status status = VCHIQ_SUCCESS;
		long ack_cnt = atomic_xchg(&arm_state->ka_use_ack_count, 0);

		while (ack_cnt && (status == VCHIQ_SUCCESS)) {
			/* Send the use notify to videocore */
			status = vchiq_send_remote_use_active(state);
			if (status == VCHIQ_SUCCESS)
				ack_cnt--;
			else
				atomic_add(ack_cnt,
					&arm_state->ka_use_ack_count);
		}
	}

out:
	vchiq_log_trace(vchiq_susp_log_level, "%s exit %d", __func__, ret);
	return ret;
}

int
vchiq_release_internal(struct vchiq_state *state, struct vchiq_service *service)
{
	struct vchiq_arm_state *arm_state = vchiq_platform_get_arm_state(state);
	int ret = 0;
	char entity[16];
	int *entity_uc;

	if (!arm_state) {
		ret = -EINVAL;
		goto out;
	}

	if (service) {
		sprintf(entity, "%c%c%c%c:%03d",
			VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc),
			service->client_id);
		entity_uc = &service->service_use_count;
	} else {
		sprintf(entity, "PEER:   ");
		entity_uc = &arm_state->peer_use_count;
	}

	write_lock_bh(&arm_state->susp_res_lock);
	if (!arm_state->videocore_use_count || !(*entity_uc)) {
		/* Don't use BUG_ON - don't allow user thread to crash kernel */
		WARN_ON(!arm_state->videocore_use_count);
		WARN_ON(!(*entity_uc));
		ret = -EINVAL;
		goto unlock;
	}
	--arm_state->videocore_use_count;
	--(*entity_uc);

	vchiq_log_trace(vchiq_susp_log_level,
		"%s %s count %d, state count %d",
		__func__, entity, *entity_uc,
		arm_state->videocore_use_count);

unlock:
	write_unlock_bh(&arm_state->susp_res_lock);

out:
	vchiq_log_trace(vchiq_susp_log_level, "%s exit %d", __func__, ret);
	return ret;
}

void
vchiq_on_remote_use(struct vchiq_state *state)
{
	struct vchiq_arm_state *arm_state = vchiq_platform_get_arm_state(state);

	atomic_inc(&arm_state->ka_use_count);
	complete(&arm_state->ka_evt);
}

void
vchiq_on_remote_release(struct vchiq_state *state)
{
	struct vchiq_arm_state *arm_state = vchiq_platform_get_arm_state(state);

	atomic_inc(&arm_state->ka_release_count);
	complete(&arm_state->ka_evt);
}

int
vchiq_use_service_internal(struct vchiq_service *service)
{
	return vchiq_use_internal(service->state, service, USE_TYPE_SERVICE);
}

int
vchiq_release_service_internal(struct vchiq_service *service)
{
	return vchiq_release_internal(service->state, service);
}

struct vchiq_debugfs_node *
vchiq_instance_get_debugfs_node(struct vchiq_instance *instance)
{
	return &instance->debugfs_node;
}

int
vchiq_instance_get_use_count(struct vchiq_instance *instance)
{
	struct vchiq_service *service;
	int use_count = 0, i;

	i = 0;
	rcu_read_lock();
	while ((service = __next_service_by_instance(instance->state,
						     instance, &i)))
		use_count += service->service_use_count;
	rcu_read_unlock();
	return use_count;
}

int
vchiq_instance_get_pid(struct vchiq_instance *instance)
{
	return instance->pid;
}

int
vchiq_instance_get_trace(struct vchiq_instance *instance)
{
	return instance->trace;
}

void
vchiq_instance_set_trace(struct vchiq_instance *instance, int trace)
{
	struct vchiq_service *service;
	int i;

	i = 0;
	rcu_read_lock();
	while ((service = __next_service_by_instance(instance->state,
						     instance, &i)))
		service->trace = trace;
	rcu_read_unlock();
	instance->trace = (trace != 0);
}

enum vchiq_status
vchiq_use_service(unsigned int handle)
{
	enum vchiq_status ret = VCHIQ_ERROR;
	struct vchiq_service *service = find_service_by_handle(handle);

	if (service) {
		ret = vchiq_use_internal(service->state, service,
				USE_TYPE_SERVICE);
		vchiq_service_put(service);
	}
	return ret;
}
EXPORT_SYMBOL(vchiq_use_service);

enum vchiq_status
vchiq_release_service(unsigned int handle)
{
	enum vchiq_status ret = VCHIQ_ERROR;
	struct vchiq_service *service = find_service_by_handle(handle);

	if (service) {
		ret = vchiq_release_internal(service->state, service);
		vchiq_service_put(service);
	}
	return ret;
}
EXPORT_SYMBOL(vchiq_release_service);

struct service_data_struct {
	int fourcc;
	int clientid;
	int use_count;
};

void
vchiq_dump_service_use_state(struct vchiq_state *state)
{
	struct vchiq_arm_state *arm_state = vchiq_platform_get_arm_state(state);
	struct service_data_struct *service_data;
	int i, found = 0;
	/*
	 * If there's more than 64 services, only dump ones with
	 * non-zero counts
	 */
	int only_nonzero = 0;
	static const char *nz = "<-- preventing suspend";

	int peer_count;
	int vc_use_count;
	int active_services;

	if (!arm_state)
		return;

	service_data = kmalloc_array(MAX_SERVICES, sizeof(*service_data),
				     GFP_KERNEL);
	if (!service_data)
		return;

	read_lock_bh(&arm_state->susp_res_lock);
	peer_count = arm_state->peer_use_count;
	vc_use_count = arm_state->videocore_use_count;
	active_services = state->unused_service;
	if (active_services > MAX_SERVICES)
		only_nonzero = 1;

	rcu_read_lock();
	for (i = 0; i < active_services; i++) {
		struct vchiq_service *service_ptr =
			rcu_dereference(state->services[i]);

		if (!service_ptr)
			continue;

		if (only_nonzero && !service_ptr->service_use_count)
			continue;

		if (service_ptr->srvstate == VCHIQ_SRVSTATE_FREE)
			continue;

		service_data[found].fourcc = service_ptr->base.fourcc;
		service_data[found].clientid = service_ptr->client_id;
		service_data[found].use_count = service_ptr->service_use_count;
		found++;
		if (found >= MAX_SERVICES)
			break;
	}
	rcu_read_unlock();

	read_unlock_bh(&arm_state->susp_res_lock);

	if (only_nonzero)
		vchiq_log_warning(vchiq_susp_log_level, "Too many active "
			"services (%d).  Only dumping up to first %d services "
			"with non-zero use-count", active_services, found);

	for (i = 0; i < found; i++) {
		vchiq_log_warning(vchiq_susp_log_level,
			"----- %c%c%c%c:%d service count %d %s",
			VCHIQ_FOURCC_AS_4CHARS(service_data[i].fourcc),
			service_data[i].clientid,
			service_data[i].use_count,
			service_data[i].use_count ? nz : "");
	}
	vchiq_log_warning(vchiq_susp_log_level,
		"----- VCHIQ use count count %d", peer_count);
	vchiq_log_warning(vchiq_susp_log_level,
		"--- Overall vchiq instance use count %d", vc_use_count);

	kfree(service_data);
}

enum vchiq_status
vchiq_check_service(struct vchiq_service *service)
{
	struct vchiq_arm_state *arm_state;
	enum vchiq_status ret = VCHIQ_ERROR;

	if (!service || !service->state)
		goto out;

	arm_state = vchiq_platform_get_arm_state(service->state);

	read_lock_bh(&arm_state->susp_res_lock);
	if (service->service_use_count)
		ret = VCHIQ_SUCCESS;
	read_unlock_bh(&arm_state->susp_res_lock);

	if (ret == VCHIQ_ERROR) {
		vchiq_log_error(vchiq_susp_log_level,
			"%s ERROR - %c%c%c%c:%d service count %d, state count %d", __func__,
			VCHIQ_FOURCC_AS_4CHARS(service->base.fourcc),
			service->client_id, service->service_use_count,
			arm_state->videocore_use_count);
		vchiq_dump_service_use_state(service->state);
	}
out:
	return ret;
}

void vchiq_platform_conn_state_changed(struct vchiq_state *state,
				       enum vchiq_connstate oldstate,
				       enum vchiq_connstate newstate)
{
	struct vchiq_arm_state *arm_state = vchiq_platform_get_arm_state(state);
	char threadname[16];

	vchiq_log_info(vchiq_susp_log_level, "%d: %s->%s", state->id,
		get_conn_state_name(oldstate), get_conn_state_name(newstate));
	if (state->conn_state != VCHIQ_CONNSTATE_CONNECTED)
		return;

	write_lock_bh(&arm_state->susp_res_lock);
	if (arm_state->first_connect) {
		write_unlock_bh(&arm_state->susp_res_lock);
		return;
	}

	arm_state->first_connect = 1;
	write_unlock_bh(&arm_state->susp_res_lock);
	snprintf(threadname, sizeof(threadname), "vchiq-keep/%d",
		 state->id);
	arm_state->ka_thread = kthread_create(&vchiq_keepalive_thread_func,
					      (void *)state,
					      threadname);
	if (IS_ERR(arm_state->ka_thread)) {
		vchiq_log_error(vchiq_susp_log_level,
				"vchiq: FATAL: couldn't create thread %s",
				threadname);
	} else {
		wake_up_process(arm_state->ka_thread);
	}
}

static const struct of_device_id vchiq_of_match[] = {
	{ .compatible = "brcm,bcm2835-vchiq", .data = &bcm2835_drvdata },
	{ .compatible = "brcm,bcm2836-vchiq", .data = &bcm2836_drvdata },
	{},
};
MODULE_DEVICE_TABLE(of, vchiq_of_match);

static struct platform_device *
vchiq_register_child(struct platform_device *pdev, const char *name)
{
	struct platform_device_info pdevinfo;
	struct platform_device *child;

	memset(&pdevinfo, 0, sizeof(pdevinfo));

	pdevinfo.parent = &pdev->dev;
	pdevinfo.name = name;
	pdevinfo.id = PLATFORM_DEVID_NONE;
	pdevinfo.dma_mask = DMA_BIT_MASK(32);

	child = platform_device_register_full(&pdevinfo);
	if (IS_ERR(child)) {
		dev_warn(&pdev->dev, "%s not registered\n", name);
		child = NULL;
	}

	return child;
}

static int vchiq_probe(struct platform_device *pdev)
{
	struct device_node *fw_node;
	const struct of_device_id *of_id;
	struct vchiq_drvdata *drvdata;
	int err;

	of_id = of_match_node(vchiq_of_match, pdev->dev.of_node);
	drvdata = (struct vchiq_drvdata *)of_id->data;
	if (!drvdata)
		return -EINVAL;

	fw_node = of_find_compatible_node(NULL, NULL,
					  "raspberrypi,bcm2835-firmware");
	if (!fw_node) {
		dev_err(&pdev->dev, "Missing firmware node\n");
		return -ENOENT;
	}

	drvdata->fw = devm_rpi_firmware_get(&pdev->dev, fw_node);
	of_node_put(fw_node);
	if (!drvdata->fw)
		return -EPROBE_DEFER;

	platform_set_drvdata(pdev, drvdata);

	err = vchiq_platform_init(pdev, &g_state);
	if (err)
		goto failed_platform_init;

	vchiq_debugfs_init();

	vchiq_log_info(vchiq_arm_log_level,
		       "vchiq: platform initialised - version %d (min %d)",
		       VCHIQ_VERSION, VCHIQ_VERSION_MIN);

	/*
	 * Simply exit on error since the function handles cleanup in
	 * cases of failure.
	 */
	err = vchiq_register_chrdev(&pdev->dev);
	if (err) {
		vchiq_log_warning(vchiq_arm_log_level,
				  "Failed to initialize vchiq cdev");
		goto error_exit;
	}

	bcm2835_camera = vchiq_register_child(pdev, "bcm2835-camera");
	bcm2835_audio = vchiq_register_child(pdev, "bcm2835_audio");

	return 0;

failed_platform_init:
	vchiq_log_warning(vchiq_arm_log_level, "could not initialize vchiq platform");
error_exit:
	return err;
}

static int vchiq_remove(struct platform_device *pdev)
{
	platform_device_unregister(bcm2835_audio);
	platform_device_unregister(bcm2835_camera);
	vchiq_debugfs_deinit();
	vchiq_deregister_chrdev();

	return 0;
}

static struct platform_driver vchiq_driver = {
	.driver = {
		.name = "bcm2835_vchiq",
		.of_match_table = vchiq_of_match,
	},
	.probe = vchiq_probe,
	.remove = vchiq_remove,
};

static int __init vchiq_driver_init(void)
{
	int ret;

	ret = platform_driver_register(&vchiq_driver);
	if (ret)
		pr_err("Failed to register vchiq driver\n");

	return ret;
}
module_init(vchiq_driver_init);

static void __exit vchiq_driver_exit(void)
{
	platform_driver_unregister(&vchiq_driver);
}
module_exit(vchiq_driver_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Videocore VCHIQ driver");
MODULE_AUTHOR("Broadcom Corporation");
