/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Marvell
 */

#ifndef _VIRTIO_USER_DEV_H
#define _VIRTIO_USER_DEV_H

enum virtio_user_backend_type {
        VIRTIO_USER_BACKEND_UNKNOWN,
        VIRTIO_USER_BACKEND_VHOST_USER,
        VIRTIO_USER_BACKEND_VHOST_VDPA,
};

struct virtio_user_queue {
        uint16_t used_idx;
        bool avail_wrap_counter;
        bool used_wrap_counter;
};

struct virtio_user_dev {
	enum virtio_user_backend_type backend_type;
        int             vhostfd;

        int             callfds[SPDK_VIRTIO_MAX_VIRTQUEUES];
        int             kickfds[SPDK_VIRTIO_MAX_VIRTQUEUES];
        uint32_t        queue_size;

        uint8_t         status;
        bool            is_stopping;
        char            path[PATH_MAX];
        uint64_t        protocol_features;

        union {
                struct vring    split[SPDK_VIRTIO_MAX_VIRTQUEUES];
                struct vring_packed packed[SPDK_VIRTIO_MAX_VIRTQUEUES];
        } vrings;

        struct virtio_user_queue packed_queues[SPDK_VIRTIO_MAX_VIRTQUEUES];

	struct virtio_user_backend_ops *ops;
        struct spdk_mem_map *mem_map;
	uint16_t **notify_area;
	bool *notify_area_is_dma;
	bool  qp_enabled[SPDK_VIRTIO_MAX_VIRTQUEUES];
};

#endif
