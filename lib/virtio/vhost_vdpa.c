/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Marvell
 */

#include "spdk/stdinc.h"

#include <sys/eventfd.h>

#include "spdk/string.h"
#include "spdk/config.h"
#include "spdk/util.h"

#include "spdk_internal/virtio.h"
#include "spdk_internal/vhost_user.h"

#include "vhost.h"
#include "virtio_user_dev.h"

struct vhost_vdpa_data {
        int vhostfd;
        uint64_t protocol_features;
};


#define VIRTIO_ID_BLOCK    0x02

#define VHOST_VDPA_SUPPORTED_BACKEND_FEATURES           \
        (1ULL << VHOST_BACKEND_F_IOTLB_MSG_V2   |       \
        1ULL << VHOST_BACKEND_F_IOTLB_BATCH)



static int
vhost_vdpa_ioctl(int fd, uint64_t request, void *arg)
{
        int ret;

        ret = ioctl(fd, request, arg);
        if (ret) {
                SPDK_ERRLOG("Vhost-vDPA ioctl %"PRIu64" failed (%s)\n",
                                request, strerror(errno));
                return -1;
        }

        return 0;
}

static int
vhost_vdpa_get_status(struct virtio_user_dev *dev, uint8_t *status)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_VDPA_GET_STATUS, status);
}

static int
vhost_vdpa_set_status(struct virtio_user_dev *dev, uint8_t status)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_VDPA_SET_STATUS, &status);
}

static int
vhost_vdpa_set_owner(struct virtio_user_dev *dev)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_SET_OWNER, NULL);
}


static int
vhost_vdpa_get_protocol_features(struct virtio_dev *vdev, uint64_t *features)
{
	 struct virtio_user_dev *dev = vdev->ctx;

        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_GET_BACKEND_FEATURES, features);
}

static int
vhost_vdpa_set_protocol_features(struct virtio_user_dev *dev, uint64_t *features)
{
	*features &= VHOST_VDPA_SUPPORTED_BACKEND_FEATURES;
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_SET_BACKEND_FEATURES, features);
}

static int
vhost_vdpa_set_features(struct virtio_user_dev *dev, uint64_t *features)
{
        /* WORKAROUND */
        *features |= 1ULL << VIRTIO_F_IOMMU_PLATFORM;

        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_SET_FEATURES, features);
}


static int
vhost_vdpa_get_features(struct virtio_user_dev *dev, uint64_t *features)
{

        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_GET_FEATURES, features);
}


static int
vhost_vdpa_iotlb_batch_begin(struct virtio_user_dev *dev)
{
        struct vhost_msg_v2 msg = {};

        if (!(dev->protocol_features & (1ULL << VHOST_BACKEND_F_IOTLB_BATCH)))
	{
		SPDK_ERRLOG( "Return 0\n");
                return 0;
	}

        if (!(dev->protocol_features & (1ULL << VHOST_BACKEND_F_IOTLB_MSG_V2))) {
                SPDK_ERRLOG( "IOTLB_MSG_V2 not supported by the backend.\n");
                return -1;
        }

        msg.type = VHOST_IOTLB_MSG_V2;
        msg.iotlb.type = VHOST_IOTLB_BATCH_BEGIN;

        if (write(dev->vhostfd, &msg, sizeof(msg)) != sizeof(msg)) {
                SPDK_ERRLOG( "Failed to send IOTLB batch begin (%s)\n",
                                strerror(errno));
                return -1;
        }

        return 0;
}

static int
vhost_vdpa_iotlb_batch_end(struct virtio_user_dev *dev)
{
        struct vhost_msg_v2 msg = {};

        if (!(dev->protocol_features & (1ULL << VHOST_BACKEND_F_IOTLB_BATCH)))
	{
		SPDK_ERRLOG( "Return 0\n");
                return 0;
	}

        if (!(dev->protocol_features & (1ULL << VHOST_BACKEND_F_IOTLB_MSG_V2))) {
                SPDK_ERRLOG("IOTLB_MSG_V2 not supported by the backend.\n");
                return -1;
        }

        msg.type = VHOST_IOTLB_MSG_V2;
        msg.iotlb.type = VHOST_IOTLB_BATCH_END;

        if (write(dev->vhostfd, &msg, sizeof(msg)) != sizeof(msg)) {
                SPDK_ERRLOG( "Failed to send IOTLB batch end (%s)\n",
                                strerror(errno));
                return -1;
        }

        return 0;
}

static int
vhost_vdpa_dma_map(struct virtio_user_dev *dev, void *addr,
                                  uint64_t iova, size_t len)
{
        struct vhost_msg_v2 msg = {};

        if (!(dev->protocol_features & (1ULL << VHOST_BACKEND_F_IOTLB_MSG_V2))) {
                SPDK_ERRLOG("IOTLB_MSG_V2 not supported by the backend.\n");
                return -1;
        }

	/* Basic validation */
	size_t page = getpagesize();
	if ((iova & (page - 1)) || (((uintptr_t)addr) & (page - 1)) || (len & (page - 1))) {
		SPDK_ERRLOG( "vDPA: iova/vaddr/len must be page aligned: iova=0x%"PRIx64" vaddr=%p len=0x%zx\n",
				iova, addr, len);
		return -1;
	}

	if (iova + len < iova) {
		SPDK_ERRLOG( "vDPA: iova + len overflow\n");
		return -1;
	}

	msg.type = VHOST_IOTLB_MSG_V2;
	msg.iotlb.type = VHOST_IOTLB_UPDATE;
	msg.iotlb.iova = iova;
	msg.iotlb.uaddr = (uint64_t)(uintptr_t)addr;
        msg.iotlb.size = len;
        msg.iotlb.perm = VHOST_ACCESS_RW;

        if (write(dev->vhostfd, &msg, sizeof(msg)) != sizeof(msg)) {
                SPDK_ERRLOG("Failed to send IOTLB update (%s)\n",
                                strerror(errno));
                return -1;
        }

        return 0;
}

static int
vhost_vdpa_dma_unmap(struct virtio_user_dev *dev, void *addr,
                                  uint64_t iova, size_t len)
{
        struct vhost_msg_v2 msg = {};

        if (!(dev->protocol_features & (1ULL << VHOST_BACKEND_F_IOTLB_MSG_V2))) {
                SPDK_ERRLOG("IOTLB_MSG_V2 not supported by the backend.\n");
                return -1;
        }

        msg.type = VHOST_IOTLB_MSG_V2;
        msg.iotlb.type = VHOST_IOTLB_INVALIDATE;
        msg.iotlb.iova = iova;
        msg.iotlb.size = len;

        if (write(dev->vhostfd, &msg, sizeof(msg)) != sizeof(msg)) {
                SPDK_ERRLOG("Failed to send IOTLB invalidate (%s)\n",
                                strerror(errno));
                return -1;
        }

        return 0;
}

static int
vhost_vdpa_dma_map_batch(struct virtio_user_dev *dev, void *addr,
                                  uint64_t iova, size_t len)
{
        int ret;

	ret = vhost_vdpa_dma_map(dev, addr, iova, len);
	if (ret < 0) {
		SPDK_ERRLOG("vDPA: dma map failed\n");
		return ret;
	}

        return 0;
}

static int
vhost_vdpa_dma_unmap_batch(struct virtio_user_dev *dev, void *addr,
                                  uint64_t iova, size_t len)
{
        int ret;

        ret = vhost_vdpa_dma_unmap(dev, addr, iova, len);
        if (ret < 0) {
                SPDK_ERRLOG("vDPA: dma unmap failed\n");
                vhost_vdpa_iotlb_batch_end(dev);
                return -1;
        }

        return 0;
}


static int
vhost_vdpa_unmap_notification_area(struct virtio_user_dev *dev, uint16_t nr_vrings)
{
        int i;

        for (i = 0; i < nr_vrings; i++) {
                if (dev->notify_area[i])
                        munmap(dev->notify_area[i], getpagesize());
        }
        free(dev->notify_area);
        dev->notify_area = NULL;

        return 0;
}

static int
vhost_vdpa_map_notification_area(struct virtio_user_dev *dev, uint16_t nr_vrings)
{

        int i, page_size = getpagesize();
        uint16_t **notify_area;

        notify_area = malloc(nr_vrings * sizeof(*notify_area));
        if (!notify_area) {
                SPDK_ERRLOG("(%s) Failed to allocate notify area array\n", dev->path);
                return -1;
        }

        for (i = 0; i < nr_vrings; i++) {
                notify_area[i] = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED | MAP_FILE,
                                        dev->vhostfd, i * page_size);
                if (notify_area[i] == MAP_FAILED) {
                        SPDK_ERRLOG("(%s) Map failed for notify address of queue %d\n",
                                        dev->path, i);
                        i--;
                        goto map_err;
                }
        }
        dev->notify_area = notify_area;

        return 0;

map_err:
        for (; i >= 0; i--)
                munmap(notify_area[i], page_size);
        free(notify_area);

        return -1;
}

static int
vhost_vdpa_set_vring_enable(struct virtio_user_dev *dev, struct vhost_vring_state *state)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_VDPA_SET_VRING_ENABLE, state);
}

static int
vhost_vdpa_set_vring_num(struct virtio_user_dev *dev, struct vhost_vring_state *state)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_SET_VRING_NUM, state);
}

static int
vhost_vdpa_set_vring_base(struct virtio_user_dev *dev, struct vhost_vring_state *state)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_SET_VRING_BASE, state);
}

static int
vhost_vdpa_get_vring_base(struct virtio_user_dev *dev, struct vhost_vring_state *state)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_GET_VRING_BASE, state);
}

static int
vhost_vdpa_set_vring_call(struct virtio_user_dev *dev, struct vhost_vring_file *file)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_SET_VRING_CALL, file);
}

static int
vhost_vdpa_set_vring_kick(struct virtio_user_dev *dev, struct vhost_vring_file *file)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_SET_VRING_KICK, file);
}

static int
vhost_vdpa_set_vring_addr(struct virtio_user_dev *dev, struct vhost_vring_addr *addr)
{
        return vhost_vdpa_ioctl(dev->vhostfd, VHOST_SET_VRING_ADDR, addr);
}

static int
vhost_vdpa_get_config(struct virtio_user_dev *dev, uint8_t *data, size_t off, int len)
{
        struct vhost_vdpa_config *config;
        int ret = 0;

        config = malloc(sizeof(*config) + len);
        if (!config) {
                SPDK_ERRLOG("Failed to allocate vDPA config data\n");
                return -1;
        }

        config->off = off;
        config->len = len;

        ret = vhost_vdpa_ioctl(dev->vhostfd, VHOST_VDPA_GET_CONFIG, config);
        if (ret) {
                SPDK_ERRLOG("Failed to get vDPA config (offset 0x%lx, len 0x%x)\n", off, len);
                ret = -1;
                goto out;
        }

        memcpy(data, config->buf, len);
out:
        free(config);

        return ret;
}

static int
vhost_vdpa_set_config(struct virtio_user_dev *dev, const uint8_t *data, size_t off, int len)
{
        struct vhost_vdpa_config *config;
        int ret = 0;

        config = malloc(sizeof(*config) + len);
        if (!config) {
                SPDK_ERRLOG("Failed to allocate vDPA config data\n");
                return -1;
        }

        config->off = off;
        config->len = len;

        memcpy(config->buf, data, len);

        ret = vhost_vdpa_ioctl(dev->vhostfd, VHOST_VDPA_SET_CONFIG, config);
        if (ret) {
                SPDK_ERRLOG("Failed to set vDPA config (offset 0x%lx, len 0x%x)\n", off, len);
                ret = -1;
        }

        free(config);

        return ret;
}

/**
 * Set up environment to talk with a vhost vdpa backend.
 *
 * @return
 *   - (-1) if fail to set up;
 *   - (>=0) if successful.
 */
static int
vhost_vdpa_setup(struct virtio_user_dev *dev)
{
        uint32_t did = (uint32_t)-1;

        dev->vhostfd = open(dev->path, O_RDWR);
        if (dev->vhostfd < 0) {
                SPDK_ERRLOG("Failed to open %s: %s",
                                dev->path, strerror(errno));
                return -1;
        }

        if (ioctl(dev->vhostfd, VHOST_VDPA_GET_DEVICE_ID, &did) < 0 ||
                        did != VIRTIO_ID_BLOCK) {
                SPDK_ERRLOG("Invalid vdpa device ID: %u", did);
                close(dev->vhostfd);
                return -1;
        }

        return 0;
}

static int
vhost_vdpa_enable_queue_pair(struct virtio_user_dev *dev,
                                uint16_t pair_idx,
                                int enable)
{
        struct vhost_vring_state state = {
                .index = pair_idx,
                .num   = enable,
        };

        if (dev->qp_enabled[pair_idx] == enable)
                return 0;

        if (vhost_vdpa_set_vring_enable(dev, &state))
                return -1;

        dev->qp_enabled[pair_idx] = enable;
        return 0;
}

struct virtio_user_backend_ops virtio_ops_vdpa = {
        .setup = vhost_vdpa_setup,
	.set_owner = vhost_vdpa_set_owner,
	.get_features = vhost_vdpa_get_features,
	.set_features = vhost_vdpa_set_features,
	.set_vring_enable = vhost_vdpa_set_vring_enable,
	.set_vring_num = vhost_vdpa_set_vring_num,
	.set_vring_base = vhost_vdpa_set_vring_base,
	.get_vring_base = vhost_vdpa_get_vring_base,
	.set_vring_call = vhost_vdpa_set_vring_call,
	.set_vring_kick = vhost_vdpa_set_vring_kick,
	.set_vring_addr = vhost_vdpa_set_vring_addr,
        .get_config = vhost_vdpa_get_config,
        .set_config = vhost_vdpa_set_config,
        .dma_map = vhost_vdpa_dma_map_batch,
        .dma_unmap = vhost_vdpa_dma_unmap_batch,
        .map_notification_area = vhost_vdpa_map_notification_area,
        .unmap_notification_area = vhost_vdpa_unmap_notification_area,
	.set_protocol_features = vhost_vdpa_set_protocol_features,
	.get_protocol_features = vhost_vdpa_get_protocol_features,
	.iotlb_batch_begin = vhost_vdpa_iotlb_batch_begin,
	.iotlb_batch_end = vhost_vdpa_iotlb_batch_end,
        .get_status = vhost_vdpa_get_status,
        .set_status = vhost_vdpa_set_status,
	.enable_qp = vhost_vdpa_enable_queue_pair,
};
