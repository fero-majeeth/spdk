/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include <sys/eventfd.h>

#include "spdk/string.h"
#include "spdk/config.h"
#include "spdk/util.h"
#include "spdk/barrier.h"
#include "spdk_internal/virtio.h"
#include "spdk_internal/vhost_user.h"
#include "virtio_user_dev.h"
#include "vhost.h"


static inline void
spdk_write16_relaxed(uint16_t value, volatile void *addr)
{
        *(volatile uint16_t *)addr = value;
}

static inline void
spdk_write32_relaxed(uint32_t value, volatile void *addr)
{
        *(volatile uint32_t *)addr = value;
}


static inline void
spdk_write16(uint16_t value, volatile void *addr)
{
        spdk_wmb();
        spdk_write16_relaxed(value, addr);
}



static inline void
spdk_write32(uint32_t value, volatile void *addr)
{
        spdk_wmb();
        spdk_write32_relaxed(value, addr);
}


static int
virtio_user_dev_set_status(struct virtio_user_dev *dev, uint8_t status)
{
        int ret;

        ret = dev->ops->set_status(dev, status);
        if (ret && ret != -ENOTSUP)
                SPDK_ERRLOG("(%s) Failed to set backend status", dev->path);

        return ret;
}

static int
virtio_user_dev_update_status(struct virtio_user_dev *dev)
{
        int ret;
        uint8_t status;

        ret = dev->ops->get_status(dev, &status);
        if (!ret) {
                dev->status = status;
        } else if (ret != -ENOTSUP) {
                SPDK_ERRLOG("(%s) Failed to get backend status\n", dev->path);
        }

        return ret;
}

static int
virtio_user_create_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Of all per virtqueue MSGs, make sure VHOST_SET_VRING_CALL come
	 * firstly because vhost depends on this msg to allocate virtqueue
	 * pair.
	 */
	struct vhost_vring_file file;

	file.index = queue_sel;
	file.fd = dev->callfds[queue_sel];

	return dev->ops->set_vring_call(dev, &file);
}

static int
virtio_user_set_vring_addr_split(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vring *vring = &dev->vrings.split[queue_sel];
	struct vhost_vring_addr addr = {
		.index = queue_sel,
		.desc_user_addr = (uint64_t)(uintptr_t)vring->desc,
		.avail_user_addr = (uint64_t)(uintptr_t)vring->avail,
		.used_user_addr = (uint64_t)(uintptr_t)vring->used,
		.log_guest_addr = 0,
		.flags = 0, /* disable log */
	};

	return dev->ops->set_vring_addr(dev, &addr);
}

static int
virtio_user_set_vring_addr_packed(struct virtio_dev *vdev, uint32_t queue_sel)
{
	uint64_t desc_addr, avail_addr, used_addr;
        struct virtio_user_dev *dev = vdev->ctx;
        struct vring_packed *vring = &dev->vrings.packed[queue_sel];


	desc_addr = vring->desc_iova;
	avail_addr = desc_addr + vring->num * sizeof(struct vring_packed_desc);
        used_addr =  SPDK_ALIGN_CEIL(avail_addr + sizeof(struct vring_packed_desc_event),
                                            VIRTIO_PCI_VRING_ALIGN);

        struct vhost_vring_addr addr = {
                .index = queue_sel,
                .desc_user_addr = desc_addr,
                .avail_user_addr = avail_addr,
                .used_user_addr = used_addr,
                .log_guest_addr = 0,
                .flags = 0, /* disable log */
        };

        return dev->ops->set_vring_addr(dev, &addr);
}

static int
virtio_user_kick_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_file file;
	struct vhost_vring_state state;
	struct vring *vring = &dev->vrings.split[queue_sel];
	struct vring_packed *pq_vring = &dev->vrings.packed[queue_sel];
	int rc;

	state.index = queue_sel;

	if (virtio_with_packed_queue(vdev))
	{
		state.num = pq_vring->num;
	}
	else
	{
		state.num = vring->num;
	}

	rc = dev->ops->set_vring_num(dev, &state);
	if (rc < 0) {
		return rc;
	}

	state.index = queue_sel;
	state.num = 0; /* no reservation */

	if ((virtio_with_packed_queue(vdev)))
	{
		state.num |= (1 << 15);
	}

	rc = dev->ops->set_vring_base(dev, &state);
	if (rc < 0) {
		SPDK_ERRLOG("Failed set_vring_base\n");
		return rc;
	}

        if (virtio_with_packed_queue(vdev))
        {
		rc = virtio_user_set_vring_addr_packed(vdev, queue_sel);
		if (rc < 0) {
			SPDK_ERRLOG("Failed set_vring_addr\n");
			return rc;
		}
	}
	else
	{
		rc = virtio_user_set_vring_addr_split(vdev, queue_sel);
		if (rc < 0) {
			SPDK_ERRLOG("Failed set_vring_addr\n");
			return rc;
		}
	}

	/* Of all per virtqueue MSGs, make sure VHOST_USER_SET_VRING_KICK comes
	 * lastly because vhost depends on this msg to judge if
	 * virtio is ready.
	 */
	file.index = queue_sel;
	file.fd = dev->kickfds[queue_sel];

	rc = dev->ops->set_vring_kick(dev, &file);
	if (rc < 0) {
		SPDK_ERRLOG("Failed set_vring_kick\n");
		return rc;
	}

	return 0;
}

static int
virtio_user_stop_queue(struct virtio_dev *vdev, uint32_t queue_sel)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;

	state.index = queue_sel;
	state.num = 0;

	return dev->ops->get_vring_base(dev, &state);
}

static int
virtio_user_queue_setup(struct virtio_dev *vdev,
			int (*fn)(struct virtio_dev *, uint32_t))
{
	uint32_t i;
	int rc;

	for (i = 0; i < vdev->max_queues; ++i) {
		rc = fn(vdev, i);
		if (rc < 0) {
			SPDK_ERRLOG("setup tx vq fails: %"PRIu32".\n", i);
			return rc;
		}
	}

	return 0;
}

static int
virtio_user_map_notify(void *cb_ctx, struct spdk_mem_map *map,
		       enum spdk_mem_map_notify_action action,
		       void *vaddr, size_t size)
{
	struct virtio_dev *vdev = cb_ctx;
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t features;
	int ret;
	size_t page = getpagesize();
	uint8_t *cur = vaddr;
	uint8_t *end = cur + size;
	int rc = 0;
	/* We do not support dynamic memory allocation with virtio-user.  If this is the
	 * initial notification when the device is started, dev->mem_map will be NULL.  If
	 * this is the final notification when the device is stopped, dev->is_stopping will
	 * be true.  All other cases are unsupported.
	 */
	if (dev->mem_map != NULL && !dev->is_stopping) {
		assert(false);
		SPDK_ERRLOG("Memory map change with active virtio_user_devs not allowed.\n");
		SPDK_ERRLOG("Pre-allocate memory for application using -s (mem_size) option.\n");
		return -1;
	}


	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {
		uint64_t run_iova = 0;
		size_t run_len = 0;
		uint8_t *run_vaddr = NULL;

		while (cur < end) {
			size_t phys_len = end - cur;
			uint64_t iova = spdk_vtophys(cur, &phys_len);

			if (iova == SPDK_VTOPHYS_ERROR) {
				SPDK_ERRLOG("vDPA: spdk_vtophys failed for %p\n", cur);
				rc = -EFAULT;
				break;
			}

			/* Align both sides to page boundaries */
			uintptr_t uaddr_aligned = (uintptr_t)cur & ~(page - 1);
			uint64_t iova_aligned = iova & ~(uint64_t)(page - 1);
			size_t head_offset = (uintptr_t)cur - uaddr_aligned;
			size_t map_len = phys_len + head_offset;
			map_len = (map_len + page - 1) & ~(page - 1); /* round up to full page */

			/* Detect if contiguous with previous chunk */
			if (run_len > 0 &&
					run_vaddr + run_len == (uint8_t *)uaddr_aligned &&
					run_iova + run_len == iova_aligned) {
				/* Extend current coalesced region */
				run_len += map_len;
			} else {
				/* Flush previous run before starting new one */
				if (run_len > 0) {
					if (action == SPDK_MEM_MAP_NOTIFY_REGISTER)
						rc = dev->ops->dma_map(dev, (void *)run_vaddr, run_iova, run_len);
					else
						rc = dev->ops->dma_unmap(dev, (void *)run_vaddr, run_iova, run_len);

					if (rc < 0)
					{
						break;
					}
				}

				/* Start new contiguous run */
				run_vaddr = (uint8_t *)uaddr_aligned;
				run_iova = iova_aligned;
				run_len = map_len;
			}

			cur += phys_len;
		}

		/* Flush final run */
		if (rc == 0 && run_len > 0) {
			if (action == SPDK_MEM_MAP_NOTIFY_REGISTER)
				rc = dev->ops->dma_map(dev, (void *)run_vaddr, run_iova, run_len);
			else
				rc = dev->ops->dma_unmap(dev, (void *)run_vaddr, run_iova, run_len);
		}

		return rc;
	}


	/* We have to resend all mappings anyway, so don't bother with any
	 * page tracking.
	 */
	ret = dev->ops->set_memory_table(dev);
	if (ret < 0) {
		return ret;
	}

	/* Since we might want to use that mapping straight away, we have to
	 * make sure the guest has already processed our SET_MEM_TABLE message.
	 * F_REPLY_ACK is just a feature and the host is not obliged to
	 * support it, so we send a simple message that always has a response
	 * and we wait for that response. Messages are always processed in order.
	 */

	return dev->ops->get_features(dev, &features);
}

static int
virtio_user_register_mem(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	const struct spdk_mem_map_ops virtio_user_map_ops = {
		.notify_cb = virtio_user_map_notify,
		.are_contiguous = NULL
	};
	int ret;

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {

		ret = dev->ops->iotlb_batch_begin(dev);
                if (ret < 0) {
                        SPDK_ERRLOG("Error iotlb_batch_begin\n");
                        return ret;
                }

		ret = dev->ops->dma_unmap(dev, NULL, 0, SIZE_MAX);
		if (ret < 0) {
			SPDK_ERRLOG("Error dma_unmap\n");
			return ret;
		}
	}

	dev->mem_map = spdk_mem_map_alloc(0, &virtio_user_map_ops, vdev);
	if (dev->mem_map == NULL) {
		dev->ops->iotlb_batch_end(dev);
		SPDK_ERRLOG("spdk_mem_map_alloc() failed\n");
		return -1;
	}

        if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {
                ret = dev->ops->iotlb_batch_end(dev);
                if (ret < 0) {
                        SPDK_ERRLOG("Error iotlb_batch_end\n");
                        return ret;
                }
        }

	return 0;
}

static void
virtio_user_unregister_mem(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
        int ret;

	dev->is_stopping = true;

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {

		ret = dev->ops->iotlb_batch_begin(dev);
		if (ret < 0) {
			SPDK_ERRLOG("Error iotlb_batch_begin\n");
			return;
		}

	}

	spdk_mem_map_free(&dev->mem_map);

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {
		ret = dev->ops->iotlb_batch_end(dev);
		if (ret < 0) {
			SPDK_ERRLOG("Error iotlb_batch_end\n");
			return; 
		}
	}

}

static int
virtio_user_start_device(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int ret;


	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_USER) { 
		/* negotiate the number of I/O queues. */
		ret = dev->ops->get_queue_num(vdev);
		if (ret < 0) {
			return ret;
		}
	}

	/* tell vhost to create queues */
	ret = virtio_user_queue_setup(vdev, virtio_user_create_queue);
	if (ret < 0) {
		return ret;
	}

	ret = virtio_user_register_mem(vdev);
	if (ret < 0) {
		return ret;
	}

        ret = virtio_user_queue_setup(vdev, virtio_user_kick_queue);
        if (ret < 0) {
                return ret;
        }

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {
		for (int i = 0; i < vdev->max_queues; i++) {
			ret = dev->ops->enable_qp(dev, i, 1);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

static int
virtio_user_stop_device(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int ret;
	uint32_t i;

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {
		for (i = 0; i < vdev->max_queues; ++i) {
			ret = dev->ops->enable_qp(dev, i, 0);
			if (ret < 0)
				return ret;
		}
	}
	ret = virtio_user_queue_setup(vdev, virtio_user_stop_queue);
	/* a queue might fail to stop for various reasons, e.g. socket
	 * connection going down, but this mustn't prevent us from freeing
	 * the mem map.
	 */
	virtio_user_unregister_mem(vdev);
	return ret;
}

static int
virtio_user_dev_setup(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint16_t i;

	dev->vhostfd = -1;

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {
		dev->ops = &virtio_ops_vdpa;
		vdev->is_hw = 1;
	}
	else
	{
		dev->ops = &virtio_ops_user;
		vdev->is_hw = 0;
	}

	for (i = 0; i < SPDK_VIRTIO_MAX_VIRTQUEUES; ++i) {
		dev->callfds[i] = -1;
		dev->kickfds[i] = -1;
	}

	return dev->ops->setup(dev);
}

static int
virtio_user_read_dev_config(struct virtio_dev *vdev, size_t offset,
			    void *dst, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int rc;

	rc = dev->ops->get_config(dev, dst, offset, length);
	if (rc < 0) {
		SPDK_ERRLOG("get_config failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

static int
virtio_user_write_dev_config(struct virtio_dev *vdev, size_t offset,
			     const void *src, int length)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int rc;

	rc = dev->ops->set_config(dev, src, offset, length);
	if (rc < 0) {
		SPDK_ERRLOG("set_config failed: %s\n", spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

static void
virtio_user_set_status(struct virtio_dev *vdev, uint8_t status)
{
	struct virtio_user_dev *dev = vdev->ctx;
	int rc = 0;

	if ((dev->status & VIRTIO_CONFIG_S_NEEDS_RESET) &&
			status != VIRTIO_CONFIG_S_RESET) {
		rc = -1;
	} else if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
		rc = virtio_user_start_device(vdev);

	} else if (status == VIRTIO_CONFIG_S_RESET &&
			(dev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		rc = virtio_user_stop_device(vdev);
	}

	if (rc != 0) {
		dev->status |= VIRTIO_CONFIG_S_NEEDS_RESET;
	} else {
		dev->status = status;
	}

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {
		virtio_user_dev_set_status(dev, dev->status);
	}
}

static uint8_t
virtio_user_get_status(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_VDPA) {
		virtio_user_dev_update_status(dev);
	}

	return dev->status;
}

static uint64_t
virtio_user_get_features(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t features;
	int rc;

	rc = dev->ops->get_features(dev, &features);
	if (rc < 0) {
		SPDK_ERRLOG("get_features failed: %s\n", spdk_strerror(-rc));
		return 0;
	}

	return features;
}

static int
virtio_user_set_features(struct virtio_dev *vdev, uint64_t features)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t protocol_features;
	int ret;

	ret = dev->ops->set_features(dev, &features);
	if (ret < 0) {
		return ret;
	}

	vdev->negotiated_features = features;
	vdev->modern = virtio_dev_has_feature(vdev, VIRTIO_F_VERSION_1);


	ret = dev->ops->get_protocol_features(vdev, &protocol_features);
	if (ret < 0) {
		return ret;
	}

	ret = dev->ops->set_protocol_features(dev, &protocol_features);
	if (ret < 0) {
		return ret;
	}

	dev->protocol_features = protocol_features;
	return 0;
}

static uint16_t
virtio_user_get_queue_size(struct virtio_dev *vdev, uint16_t queue_id)
{
	struct virtio_user_dev *dev = vdev->ctx;

	/* Currently each queue has same queue size */
	return dev->queue_size;
}

static void
virtio_user_setup_queue_packed(struct virtqueue *vq,
                               struct virtio_user_dev *dev)
{
        uint16_t queue_idx = vq->vq_queue_index;
        struct vring_packed *vring;
        uint64_t desc_addr;
        uint64_t avail_addr;
        uint64_t used_addr;
        uint16_t i;

        vring  = &dev->vrings.packed[queue_idx];
        desc_addr = (uintptr_t)vq->vq_ring_virt_mem;
        avail_addr = desc_addr + vq->vq_nentries *
                sizeof(struct vring_packed_desc);
        used_addr = SPDK_ALIGN_CEIL(avail_addr +
                           sizeof(struct vring_packed_desc_event),
                           VIRTIO_PCI_VRING_ALIGN);
        vring->num = vq->vq_nentries;
        vring->desc_iova = vq->vq_ring_mem;
        vring->desc = (void *)(uintptr_t)desc_addr;
        vring->driver = (void *)(uintptr_t)avail_addr;
        vring->device = (void *)(uintptr_t)used_addr;
        dev->packed_queues[queue_idx].avail_wrap_counter = true;
        dev->packed_queues[queue_idx].used_wrap_counter = true;
        dev->packed_queues[queue_idx].used_idx = 0;

        for (i = 0; i < vring->num; i++)
                vring->desc[i].flags = 0;
}

static void
virtio_user_setup_queue_split(struct virtqueue *vq, struct virtio_user_dev *dev)
{
        uint16_t queue_idx = vq->vq_queue_index;
        uint64_t desc_addr, avail_addr, used_addr;

        desc_addr = (uintptr_t)vq->vq_ring_virt_mem;
        avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
        used_addr = SPDK_ALIGN_CEIL(avail_addr + offsetof(struct vring_avail,
                                                         ring[vq->vq_nentries]),
                                   VIRTIO_PCI_VRING_ALIGN);

        dev->vrings.split[queue_idx].num = vq->vq_nentries;
        dev->vrings.split[queue_idx].desc = (void *)(uintptr_t)desc_addr;
        dev->vrings.split[queue_idx].avail = (void *)(uintptr_t)avail_addr;
        dev->vrings.split[queue_idx].used = (void *)(uintptr_t)used_addr;
}

static int
virtio_user_setup_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	struct virtio_user_dev *dev = vdev->ctx;
	struct vhost_vring_state state;
	uint16_t queue_idx = vq->vq_queue_index;
	void *queue_mem;
	int callfd, kickfd,rc;
	uint64_t phys_addr, phys_len = vq->vq_ring_size;


	if (dev->callfds[queue_idx] != -1 || dev->kickfds[queue_idx] != -1) {
		SPDK_ERRLOG("queue %"PRIu16" already exists\n", queue_idx);
		return -EEXIST;
	}

	/* May use invalid flag, but some backend uses kickfd and
	 * callfd as criteria to judge if dev is alive. so finally we
	 * use real event_fd.
	 */
	callfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (callfd < 0) {
		SPDK_ERRLOG("callfd error, %s\n", spdk_strerror(errno));
		return -errno;
	}

	kickfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (kickfd < 0) {
		SPDK_ERRLOG("kickfd error, %s\n", spdk_strerror(errno));
		close(callfd);
		return -errno;
	}

	queue_mem = spdk_zmalloc(vq->vq_ring_size, VIRTIO_PCI_VRING_ALIGN, NULL,
			SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (queue_mem == NULL) {
		close(kickfd);
		close(callfd);
		return -ENOMEM;
	}

	if (vdev->is_hw) {
		phys_addr = spdk_vtophys(queue_mem, &phys_len);
		if (phys_addr == SPDK_VTOPHYS_ERROR || phys_len < vq->vq_ring_size) {
			SPDK_ERRLOG("vDPA: failed to translate ring to physical address, phys_len:%ld, vq->vq_ring_size:%d\n",phys_len, vq->vq_ring_size);
			close(kickfd);
			close(callfd);
			spdk_free(queue_mem);
			return -EFAULT;
		}
		vq->vq_ring_mem = phys_addr;
	}
	else
	{
		vq->vq_ring_mem = (uintptr_t) queue_mem;
	}

	vq->vq_ring_virt_mem = queue_mem;

	if (dev->backend_type == VIRTIO_USER_BACKEND_VHOST_USER) {
		state.index = vq->vq_queue_index;
		state.num = 1;

		if (virtio_dev_has_feature(vdev, VHOST_USER_F_PROTOCOL_FEATURES)) {
			rc =  dev->ops->set_vring_enable(dev, &state);
			if (rc < 0) {
				SPDK_ERRLOG("failed to send VHOST_USER_SET_VRING_ENABLE: %s\n",
						spdk_strerror(-rc));
				close(kickfd);
				close(callfd);
				spdk_free(queue_mem);
				return -rc;
			}
		}
	}

	dev->callfds[queue_idx] = callfd;
	dev->kickfds[queue_idx] = kickfd;

	if (virtio_with_packed_queue(vdev))
		virtio_user_setup_queue_packed(vq, dev);
	else
		virtio_user_setup_queue_split(vq, dev);

	if (dev->notify_area)
	{
		vq->notify_addr = dev->notify_area[vq->vq_queue_index];
	}

	return 0;
}

static void
virtio_user_del_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	/* For legacy devices, write 0 to VIRTIO_PCI_QUEUE_PFN port, QEMU
	 * correspondingly stops the ioeventfds, and reset the status of
	 * the device.
	 * For modern devices, set queue desc, avail, used in PCI bar to 0,
	 * not see any more behavior in QEMU.
	 *
	 * Here we just care about what information to deliver to vhost-user.
	 * So we just close ioeventfd for now.
	 */
	struct virtio_user_dev *dev = vdev->ctx;

	close(dev->callfds[vq->vq_queue_index]);
	close(dev->kickfds[vq->vq_queue_index]);
	dev->callfds[vq->vq_queue_index] = -1;
	dev->kickfds[vq->vq_queue_index] = -1;

	spdk_free(vq->vq_ring_virt_mem);
}

static void
virtio_user_notify_queue(struct virtio_dev *vdev, struct virtqueue *vq)
{
	uint64_t notify_data = 1;
	struct virtio_user_dev *dev = vdev->ctx;

	if (!dev->notify_area) {
		if (write(dev->kickfds[vq->vq_queue_index], &notify_data, sizeof(notify_data)) < 0) {
			SPDK_ERRLOG("failed to kick backend: %s.\n", spdk_strerror(errno));
		}
		return;
        } else if (!virtio_dev_has_feature(vdev, VIRTIO_F_NOTIFICATION_DATA)) {
		spdk_write16(vq->vq_queue_index, vq->notify_addr);
                return;
        }

        if (virtio_with_packed_queue(vdev)) {
                /* Bit[0:15]: vq queue index
                 * Bit[16:30]: avail index
                 * Bit[31]: avail wrap counter
                 */
                notify_data = ((uint32_t)(!!(vq->vq_packed.cached_flags &
                                SPDK_VRING_PACKED_DESC_F_AVAIL)) << 31) |
                                ((uint32_t)vq->vq_avail_idx << 16) |
                                vq->vq_queue_index;
        } else {
                /* Bit[0:15]: vq queue index
                 * Bit[16:31]: avail index
                 */
                notify_data = ((uint32_t)vq->vq_avail_idx << 16) |
                                vq->vq_queue_index;
        }

	spdk_write32(notify_data, vq->notify_addr);
}

static void
virtio_user_destroy(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;

	if (dev) {
		close(dev->vhostfd);
		free(dev);
	}
}

static void
virtio_user_dump_json_info(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_named_string(w, "type", "user");
	spdk_json_write_named_string(w, "socket", dev->path);
}

static void
virtio_user_write_json_config(struct virtio_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct virtio_user_dev *dev = vdev->ctx;

	spdk_json_write_named_string(w, "trtype", "user");
	spdk_json_write_named_string(w, "traddr", dev->path);
	spdk_json_write_named_uint32(w, "vq_count", vdev->max_queues - vdev->fixed_queues_num);
	spdk_json_write_named_uint32(w, "vq_size", virtio_dev_backend_ops(vdev)->get_queue_size(vdev, 0));
}

static int
virtio_user_init_notify_queue (struct virtio_dev *vdev, uint16_t max_queues)
{
	struct virtio_user_dev *dev = vdev->ctx;

        if (vdev->negotiated_features & (1ULL << VIRTIO_F_NOTIFICATION_DATA))
	{
            if (dev->ops->map_notification_area &&
                                dev->ops->map_notification_area(dev, max_queues))
	     {
		     SPDK_ERRLOG("Failed map_notification_area\n");
                        return -1;
	     }
	}
        return 0;
}

static void
virtio_user_uninit_notify_queue (struct virtio_dev *vdev, uint16_t max_queues)
{
      struct virtio_user_dev *dev = vdev->ctx;

      if (dev->ops->unmap_notification_area && dev->notify_area)
		dev->ops->unmap_notification_area(dev, max_queues);

}

static const struct virtio_dev_ops virtio_user_ops = {
	.read_dev_cfg	= virtio_user_read_dev_config,
	.write_dev_cfg	= virtio_user_write_dev_config,
	.get_status	= virtio_user_get_status,
	.set_status	= virtio_user_set_status,
	.get_features	= virtio_user_get_features,
	.set_features	= virtio_user_set_features,
	.destruct_dev	= virtio_user_destroy,
	.get_queue_size	= virtio_user_get_queue_size,
	.setup_queue	= virtio_user_setup_queue,
	.del_queue	= virtio_user_del_queue,
	.notify_queue	= virtio_user_notify_queue,
	.dump_json_info = virtio_user_dump_json_info,
	.write_json_config = virtio_user_write_json_config,
	.init_notify_queue = virtio_user_init_notify_queue,
	.uninit_notify_queue = virtio_user_uninit_notify_queue,
};

int
virtio_user_dev_init(struct virtio_dev *vdev, const char *name, const char *path,
		     uint32_t queue_size)
{
	struct virtio_user_dev *dev;
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("No name given for controller: %s\n", path);
		return -EINVAL;
	}

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		return -ENOMEM;
	}

	rc = virtio_dev_construct(vdev, name, &virtio_user_ops, dev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to init device: %s\n", path);
		free(dev);
		return rc;
	}

	snprintf(dev->path, PATH_MAX, "%s", path);

	if (strstr(dev->path, "/dev/vhost-vdpa") != NULL) {
              dev->backend_type = VIRTIO_USER_BACKEND_VHOST_VDPA;
	}
	else
	{
               dev->backend_type = VIRTIO_USER_BACKEND_VHOST_USER;
	}

	dev->queue_size = queue_size;

	rc = virtio_user_dev_setup(vdev);
	if (rc < 0) {
		SPDK_ERRLOG("backend set up fails\n");
		goto err;
	}

	rc = dev->ops->set_owner(dev);
	if (rc < 0) {
		SPDK_ERRLOG("set_owner fails: %s\n", spdk_strerror(-rc));
		goto err;
	}

	return 0;

err:
	virtio_dev_destruct(vdev);
	return rc;
}
SPDK_LOG_REGISTER_COMPONENT(virtio_user)
