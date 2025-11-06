/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Marvell
 */

#ifndef _VIRTIO_VHOST_H
#define _VIRTIO_VHOST_H


/* The version of the protocol we support */
#define VHOST_USER_VERSION    0x1

#define VIRTIO_USER_SUPPORTED_PROTOCOL_FEATURES \
        ((1ULL << VHOST_USER_PROTOCOL_F_MQ) | \
        (1ULL << VHOST_USER_PROTOCOL_F_CONFIG))

struct virtio_user_dev;

struct virtio_user_backend_ops {
        int (*setup)(struct virtio_user_dev *dev);
        int (*destroy)(struct virtio_user_dev *dev);
        int (*get_backend_features)(uint64_t *features);
        int (*set_owner)(struct virtio_user_dev *dev);
        int (*get_features)(struct virtio_user_dev *dev, uint64_t *features);
        int (*set_features)(struct virtio_user_dev *dev, uint64_t *features);
        int (*set_memory_table)(struct virtio_user_dev *dev);
        int (*set_vring_num)(struct virtio_user_dev *dev, struct vhost_vring_state *state);
        int (*set_vring_enable)(struct virtio_user_dev *dev, struct vhost_vring_state *state);
        int (*set_vring_base)(struct virtio_user_dev *dev, struct vhost_vring_state *state);
        int (*get_vring_base)(struct virtio_user_dev *dev, struct vhost_vring_state *state);
        int (*set_vring_call)(struct virtio_user_dev *dev, struct vhost_vring_file *file);
        int (*set_vring_kick)(struct virtio_user_dev *dev, struct vhost_vring_file *file);
        int (*set_vring_addr)(struct virtio_user_dev *dev, struct vhost_vring_addr *addr);
        int (*get_status)(struct virtio_user_dev *dev, uint8_t *status);
        int (*set_status)(struct virtio_user_dev *dev, uint8_t status);
        int (*get_config)(struct virtio_user_dev *dev, uint8_t *data, size_t off, int len);
        int (*set_config)(struct virtio_user_dev *dev, const uint8_t *data, size_t off,
                        int len);
        int (*enable_qp)(struct virtio_user_dev *dev, uint16_t pair_idx, int enable);
        int (*dma_map)(struct virtio_user_dev *dev, void *addr, uint64_t iova, size_t len);
        int (*dma_unmap)(struct virtio_user_dev *dev, void *addr, uint64_t iova, size_t len);
        int (*get_intr_fd)(struct virtio_user_dev *dev);
        int (*map_notification_area)(struct virtio_user_dev *dev, uint16_t max_queue);
        int (*unmap_notification_area)(struct virtio_user_dev *dev, uint16_t max_queue);
	int (*get_protocol_features)(struct virtio_dev *vdev, uint64_t *protocol_features);
	int (*set_protocol_features)(struct virtio_user_dev *dev, uint64_t *protocol_features);
	int (*get_queue_num)(struct virtio_dev *vdev);
	int (*iotlb_batch_begin)(struct virtio_user_dev *dev);
	int (*iotlb_batch_end)(struct virtio_user_dev *dev);
};


extern struct virtio_user_backend_ops virtio_ops_user;
extern struct virtio_user_backend_ops virtio_ops_vdpa;

#endif
