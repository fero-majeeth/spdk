/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
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

static int
vhost_user_write(int fd, void *buf, int len, int *fds, int fd_num)
{
        int r;
        struct msghdr msgh;
        struct iovec iov;
        size_t fd_size = fd_num * sizeof(int);
        char control[CMSG_SPACE(fd_size)];
        struct cmsghdr *cmsg;

        memset(&msgh, 0, sizeof(msgh));
        memset(control, 0, sizeof(control));

        iov.iov_base = (uint8_t *)buf;
        iov.iov_len = len;

        msgh.msg_iov = &iov;
        msgh.msg_iovlen = 1;

        if (fds && fd_num > 0) {
                msgh.msg_control = control;
                msgh.msg_controllen = sizeof(control);
                cmsg = CMSG_FIRSTHDR(&msgh);
                if (!cmsg) {
                        SPDK_WARNLOG("First HDR is NULL\n");
                        return -EIO;
                }
                cmsg->cmsg_len = CMSG_LEN(fd_size);
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_RIGHTS;
                memcpy(CMSG_DATA(cmsg), fds, fd_size);
        } else {
                msgh.msg_control = NULL;
                msgh.msg_controllen = 0;
        }

        do {
                r = sendmsg(fd, &msgh, 0);
        } while (r < 0 && errno == EINTR);

        if (r == -1) {
                return -errno;
        }

        return 0;
}


static int
vhost_user_read(int fd, struct vhost_user_msg *msg)
{
        uint32_t valid_flags = VHOST_USER_REPLY_MASK | VHOST_USER_VERSION;
        ssize_t ret;
        size_t sz_hdr = VHOST_USER_HDR_SIZE, sz_payload;

        ret = recv(fd, (void *)msg, sz_hdr, 0);
        if ((size_t)ret != sz_hdr) {
                SPDK_WARNLOG("Failed to recv msg hdr: %zd instead of %zu.\n",
                             ret, sz_hdr);
                if (ret == -1) {
                        return -errno;
                } else {
                        return -EBUSY;
                }
        }

        /* validate msg flags */
        if (msg->flags != (valid_flags)) {
                SPDK_WARNLOG("Failed to recv msg: flags %"PRIx32" instead of %"PRIx32".\n",
                             msg->flags, valid_flags);
                return -EIO;
        }

        sz_payload = msg->size;

        if (sz_payload > VHOST_USER_PAYLOAD_SIZE) {
                SPDK_WARNLOG("Received oversized msg: payload size %zu > available space %zu\n",
                             sz_payload, VHOST_USER_PAYLOAD_SIZE);
                return -EIO;
        }

        if (sz_payload) {
                ret = recv(fd, (void *)((char *)msg + sz_hdr), sz_payload, 0);
                if ((size_t)ret != sz_payload) {
                        SPDK_WARNLOG("Failed to recv msg payload: %zd instead of %"PRIu32".\n",
                                     ret, msg->size);
                        if (ret == -1) {
                                return -errno;
                        } else {
                                return -EBUSY;
                        }
                }
        }

        return 0;
}

struct hugepage_file_info {
        uint64_t addr;            /**< virtual addr */
        size_t   size;            /**< the file size */
        char     path[PATH_MAX];  /**< path to backing file */
};

/* Two possible options:
 * 1. Match HUGEPAGE_INFO_FMT to find the file storing struct hugepage_file
 * array. This is simple but cannot be used in secondary process because
 * secondary process will close and munmap that file.
 * 2. Match HUGEFILE_FMT to find hugepage files directly.
 *
 * We choose option 2.
 */
static int
get_hugepage_file_info(struct hugepage_file_info hugepages[], int max)
{
        int idx, rc;
        FILE *f;
        char buf[BUFSIZ], *tmp, *tail;
        char *str_underline, *str_start;
        int huge_index;
        uint64_t v_start, v_end;

        f = fopen("/proc/self/maps", "r");
        if (!f) {
                SPDK_ERRLOG("cannot open /proc/self/maps\n");
                rc = -errno;
                assert(rc < 0); /* scan-build hack */
                return rc;
        }

        idx = 0;
        while (fgets(buf, sizeof(buf), f) != NULL) {
                if (sscanf(buf, "%" PRIx64 "-%" PRIx64, &v_start, &v_end) < 2) {
                        SPDK_ERRLOG("Failed to parse address\n");
                        rc = -EIO;
                        goto out;
                }

                tmp = strchr(buf, ' ') + 1; /** skip address */
                tmp = strchr(tmp, ' ') + 1; /** skip perm */
                tmp = strchr(tmp, ' ') + 1; /** skip offset */
                tmp = strchr(tmp, ' ') + 1; /** skip dev */
                tmp = strchr(tmp, ' ') + 1; /** skip inode */
                while (*tmp == ' ') {       /** skip spaces */
                        tmp++;
                }
                tail = strrchr(tmp, '\n');  /** remove newline if exists */
                if (tail) {
                        *tail = '\0';
                }

                /* Match HUGEFILE_FMT, aka "%s/%smap_%d",
                 * which is defined in eal_filesystem.h
                 */
                str_underline = strrchr(tmp, '_');
                if (!str_underline) {
                        continue;
                }

                str_start = str_underline - strlen("map");
                if (str_start < tmp) {
                        continue;
                }

                if (sscanf(str_start, "map_%d", &huge_index) != 1) {
                        continue;
                }

                if (idx >= max) {
                        SPDK_ERRLOG("Exceed maximum of %d\n", max);
                        rc = -ENOSPC;
                        goto out;
                }

                if (idx > 0 &&
                    strncmp(tmp, hugepages[idx - 1].path, PATH_MAX) == 0 &&
                    v_start == hugepages[idx - 1].addr + hugepages[idx - 1].size) {
                        hugepages[idx - 1].size += (v_end - v_start);
                        continue;
                }

                hugepages[idx].addr = v_start;
                hugepages[idx].size = v_end - v_start;
                snprintf(hugepages[idx].path, PATH_MAX, "%s", tmp);
                idx++;
        }

        rc = idx;
out:
        fclose(f);
        return rc;
}

static int
prepare_vhost_memory_user(struct vhost_user_msg *msg, int fds[])
{
        int i, num;
        struct hugepage_file_info hugepages[VHOST_USER_MEMORY_MAX_NREGIONS];

        num = get_hugepage_file_info(hugepages, VHOST_USER_MEMORY_MAX_NREGIONS);
        if (num < 0) {
                SPDK_ERRLOG("Failed to prepare memory for vhost-user\n");
                return num;
        }

        for (i = 0; i < num; ++i) {
                /* the memory regions are unaligned */
                msg->payload.memory.regions[i].guest_phys_addr = hugepages[i].addr; /* use vaddr! */
                msg->payload.memory.regions[i].userspace_addr = hugepages[i].addr;
                msg->payload.memory.regions[i].memory_size = hugepages[i].size;
                msg->payload.memory.regions[i].flags_padding = 0;
                fds[i] = open(hugepages[i].path, O_RDWR);
        }

        msg->payload.memory.nregions = num;
        msg->payload.memory.padding = 0;

        return 0;
}

static const char *const vhost_msg_strings[VHOST_USER_MAX] = {
        [VHOST_USER_SET_OWNER] = "VHOST_SET_OWNER",
        [VHOST_USER_RESET_OWNER] = "VHOST_RESET_OWNER",
        [VHOST_USER_SET_FEATURES] = "VHOST_SET_FEATURES",
        [VHOST_USER_GET_FEATURES] = "VHOST_GET_FEATURES",
        [VHOST_USER_SET_VRING_CALL] = "VHOST_SET_VRING_CALL",
        [VHOST_USER_GET_PROTOCOL_FEATURES] = "VHOST_USER_GET_PROTOCOL_FEATURES",
        [VHOST_USER_SET_PROTOCOL_FEATURES] = "VHOST_USER_SET_PROTOCOL_FEATURES",
        [VHOST_USER_SET_VRING_NUM] = "VHOST_SET_VRING_NUM",
        [VHOST_USER_SET_VRING_BASE] = "VHOST_SET_VRING_BASE",
        [VHOST_USER_GET_VRING_BASE] = "VHOST_GET_VRING_BASE",
        [VHOST_USER_SET_VRING_ADDR] = "VHOST_SET_VRING_ADDR",
        [VHOST_USER_SET_VRING_KICK] = "VHOST_SET_VRING_KICK",
        [VHOST_USER_SET_MEM_TABLE] = "VHOST_SET_MEM_TABLE",
        [VHOST_USER_SET_VRING_ENABLE] = "VHOST_SET_VRING_ENABLE",
        [VHOST_USER_GET_QUEUE_NUM] = "VHOST_USER_GET_QUEUE_NUM",
        [VHOST_USER_GET_CONFIG] = "VHOST_USER_GET_CONFIG",
        [VHOST_USER_SET_CONFIG] = "VHOST_USER_SET_CONFIG",
};


static int
vhost_user_sock(struct virtio_user_dev *dev,
                enum vhost_user_request req,
                void *arg)
{
        struct vhost_user_msg msg;
        struct vhost_vring_file *file = 0;
        int need_reply = 0;
        int fds[VHOST_USER_MEMORY_MAX_NREGIONS];
        int fd_num = 0;
        int i, len, rc;
        int vhostfd = dev->vhostfd;

        SPDK_DEBUGLOG(virtio_user, "sent message %d = %s\n", req, vhost_msg_strings[req]);

        msg.request = req;
        msg.flags = VHOST_USER_VERSION;
        msg.size = 0;

        switch (req) {
        case VHOST_USER_GET_FEATURES:
        case VHOST_USER_GET_PROTOCOL_FEATURES:
        case VHOST_USER_GET_QUEUE_NUM:
                need_reply = 1;
                break;

        case VHOST_USER_SET_FEATURES:
        case VHOST_USER_SET_LOG_BASE:
        case VHOST_USER_SET_PROTOCOL_FEATURES:
                msg.payload.u64 = *((__u64 *)arg);
                msg.size = sizeof(msg.payload.u64);
                break;

        case VHOST_USER_SET_OWNER:
        case VHOST_USER_RESET_OWNER:
                break;

        case VHOST_USER_SET_MEM_TABLE:
                rc = prepare_vhost_memory_user(&msg, fds);
                if (rc < 0) {
                        return rc;
                }
                fd_num = msg.payload.memory.nregions;
                msg.size = sizeof(msg.payload.memory.nregions);
                msg.size += sizeof(msg.payload.memory.padding);
                msg.size += fd_num * sizeof(struct vhost_memory_region);
                break;

        case VHOST_USER_SET_LOG_FD:
                fds[fd_num++] = *((int *)arg);
                break;

        case VHOST_USER_SET_VRING_NUM:
        case VHOST_USER_SET_VRING_BASE:
        case VHOST_USER_SET_VRING_ENABLE:
                memcpy(&msg.payload.state, arg, sizeof(msg.payload.state));
                msg.size = sizeof(msg.payload.state);
                break;

        case VHOST_USER_GET_VRING_BASE:
                memcpy(&msg.payload.state, arg, sizeof(msg.payload.state));
                msg.size = sizeof(msg.payload.state);
                need_reply = 1;
                break;

        case VHOST_USER_SET_VRING_ADDR:
                memcpy(&msg.payload.addr, arg, sizeof(msg.payload.addr));
                msg.size = sizeof(msg.payload.addr);
                break;

        case VHOST_USER_SET_VRING_KICK:
        case VHOST_USER_SET_VRING_CALL:
        case VHOST_USER_SET_VRING_ERR:
                file = arg;
                msg.payload.u64 = file->index & VHOST_USER_VRING_IDX_MASK;
                msg.size = sizeof(msg.payload.u64);
                if (file->fd > 0) {
                        fds[fd_num++] = file->fd;
                } else {
                        msg.payload.u64 |= VHOST_USER_VRING_NOFD_MASK;
                }
                break;


        case VHOST_USER_GET_CONFIG:
                memcpy(&msg.payload.cfg, arg, sizeof(msg.payload.cfg));
                msg.size = sizeof(msg.payload.cfg);
                need_reply = 1;
                break;

        case VHOST_USER_SET_CONFIG:
                memcpy(&msg.payload.cfg, arg, sizeof(msg.payload.cfg));
                msg.size = sizeof(msg.payload.cfg);
                break;

        default:
                SPDK_ERRLOG("trying to send unknown msg\n");
                return -EINVAL;
        }

        len = VHOST_USER_HDR_SIZE + msg.size;
        rc = vhost_user_write(vhostfd, &msg, len, fds, fd_num);
        if (rc < 0) {
                SPDK_ERRLOG("%s failed: %s\n",
                            vhost_msg_strings[req], spdk_strerror(-rc));
                return rc;
        }

        if (req == VHOST_USER_SET_MEM_TABLE)
                for (i = 0; i < fd_num; ++i) {
                        close(fds[i]);
                }

        if (need_reply) {
                rc = vhost_user_read(vhostfd, &msg);
                if (rc < 0) {
                        SPDK_WARNLOG("Received msg failed: %s\n", spdk_strerror(-rc));
                        return rc;
                }

                if (req != msg.request) {
                        SPDK_WARNLOG("Received unexpected msg type\n");
                        return -EIO;
                }

                switch (req) {
                case VHOST_USER_GET_FEATURES:
                case VHOST_USER_GET_PROTOCOL_FEATURES:
                case VHOST_USER_GET_QUEUE_NUM:
                        if (msg.size != sizeof(msg.payload.u64)) {
                                SPDK_WARNLOG("Received bad msg size\n");
                                return -EIO;
                        }
                        *((__u64 *)arg) = msg.payload.u64;
                        break;
                case VHOST_USER_GET_VRING_BASE:
                        if (msg.size != sizeof(msg.payload.state)) {
                                SPDK_WARNLOG("Received bad msg size\n");
                                return -EIO;
                        }
                        memcpy(arg, &msg.payload.state,
                               sizeof(struct vhost_vring_state));
                        break;
                case VHOST_USER_GET_CONFIG:
                        if (msg.size != sizeof(msg.payload.cfg)) {
                                SPDK_WARNLOG("Received bad msg size\n");
                                return -EIO;
                        }
                        memcpy(arg, &msg.payload.cfg, sizeof(msg.payload.cfg));
                        break;
                default:
                        SPDK_WARNLOG("Received unexpected msg type\n");
                        return -EBADMSG;
                }
        }

        return 0;
}

/**
 * Set up environment to talk with a vhost user backend.
 *
 * @return
 *   - (-1) if fail;
 *   - (0) if succeed.
 */
static int
vhost_user_setup(struct virtio_user_dev *dev)
{
        int fd;
        int flag;
        struct sockaddr_un un;
        ssize_t rc;

        SPDK_NOTICELOG("==== %s ====\n",__func__);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
                SPDK_ERRLOG("socket() error, %s\n", spdk_strerror(errno));
                return -errno;
        }

        flag = fcntl(fd, F_GETFD);
        if (fcntl(fd, F_SETFD, flag | FD_CLOEXEC) < 0) {
                SPDK_ERRLOG("fcntl failed, %s\n", spdk_strerror(errno));
        }

        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        rc = snprintf(un.sun_path, sizeof(un.sun_path), "%s", dev->path);
        if (rc < 0 || (size_t)rc >= sizeof(un.sun_path)) {
                SPDK_ERRLOG("socket path too long\n");
                close(fd);
                if (rc < 0) {
                        return -errno;
                } else {
                        return -EINVAL;
                }
        }
        if (connect(fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
                SPDK_ERRLOG("connect error, %s\n", spdk_strerror(errno));
                close(fd);
                return -errno;
        }

        dev->vhostfd = fd;
        return 0;
}

static int
vhost_user_set_vring_call(struct virtio_user_dev *dev, struct vhost_vring_file *file)
{
        return vhost_user_sock(dev, VHOST_USER_SET_VRING_CALL, file);
}

static int
vhost_user_set_vring_addr(struct virtio_user_dev *dev, struct vhost_vring_addr *addr)
{

	return vhost_user_sock(dev, VHOST_USER_SET_VRING_ADDR, addr);
}

static int
vhost_user_set_vring_num(struct virtio_user_dev *dev, struct vhost_vring_state *state)
{
        return vhost_user_sock(dev, VHOST_USER_SET_VRING_NUM, state);
}

static int
vhost_user_set_vring_base(struct virtio_user_dev *dev, struct vhost_vring_state *state)
{
        return vhost_user_sock(dev, VHOST_USER_SET_VRING_BASE, state);
}

static int
vhost_user_set_vring_kick(struct virtio_user_dev *dev, struct vhost_vring_file *file)
{
        return vhost_user_sock(dev, VHOST_USER_SET_VRING_KICK, file);
}


static int
vhost_user_get_vring_base(struct virtio_user_dev *dev, struct vhost_vring_state *state)
{
        return vhost_user_sock(dev, VHOST_USER_GET_VRING_BASE, state);
}

static int
vhost_user_set_memory_table(struct virtio_user_dev *dev)
{
   return vhost_user_sock(dev, VHOST_USER_SET_MEM_TABLE, NULL);
}


static int
vhost_user_get_features(struct virtio_user_dev *dev, uint64_t *features)
{

   return vhost_user_sock(dev, VHOST_USER_GET_FEATURES, features);
}

static int
vhost_user_set_features(struct virtio_user_dev *dev, uint64_t *features)
{

   return vhost_user_sock(dev, VHOST_USER_SET_FEATURES, features);
}

static int
vhost_user_get_queue_num(struct virtio_dev *vdev)
{
	struct virtio_user_dev *dev = vdev->ctx;
	uint64_t host_max_queues;
	int ret;

	if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_MQ)) == 0 &&
			vdev->max_queues > 1 + vdev->fixed_queues_num) {
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues, but the "
				"host doesn't support VHOST_USER_PROTOCOL_F_MQ. "
				"Only one request queue will be used.\n",
				vdev->name, vdev->max_queues - vdev->fixed_queues_num);
		vdev->max_queues = 1 + vdev->fixed_queues_num;
	}


	ret = vhost_user_sock(dev, VHOST_USER_GET_QUEUE_NUM, &host_max_queues);

	if (ret < 0) {
		return ret;
	}

	if (vdev->max_queues > host_max_queues + vdev->fixed_queues_num) {
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues"
				"but only %"PRIu64" available\n",
				vdev->name, vdev->max_queues - vdev->fixed_queues_num,
				host_max_queues);
		vdev->max_queues = host_max_queues;
	}

	return 0;
}

static int
vhost_user_set_config(struct virtio_user_dev *dev, const uint8_t *src, size_t offset, int length)
{
   struct vhost_user_config cfg = {0};

           if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0) {
                return -ENOTSUP;
        }

    cfg.offset = offset;
    cfg.size = length;
    memcpy(cfg.region, src, length);

   return vhost_user_sock(dev, VHOST_USER_SET_CONFIG, &cfg);
}

static int
vhost_user_get_config(struct virtio_user_dev *dev, uint8_t *dst, size_t offset, int length)
{
	struct vhost_user_config cfg = {0};
	int rc;

        if ((dev->protocol_features & (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) == 0) {
                return -ENOTSUP;
        }

	cfg.offset = 0;
	cfg.size = VHOST_USER_MAX_CONFIG_SIZE;


	rc = vhost_user_sock(dev, VHOST_USER_GET_CONFIG, &cfg);
	if (rc < 0) {
	     return rc;
	}

	memcpy(dst, cfg.region + offset, length);
	return 0;
}

static int
vhost_user_get_proto_feature(struct virtio_dev *vdev, uint64_t *protocol_features)
{

	struct virtio_user_dev *dev = vdev->ctx;

        if (!virtio_dev_has_feature(vdev, VHOST_USER_F_PROTOCOL_FEATURES)) {
                /* nothing else to do */
                SPDK_ERRLOG("nothing else to do\n");
                return 0;
        }

	return vhost_user_sock(dev, VHOST_USER_GET_PROTOCOL_FEATURES, protocol_features);
}

static int
vhost_user_set_proto_feature(struct virtio_user_dev *dev, uint64_t *protocol_features)
{

   *protocol_features &= VIRTIO_USER_SUPPORTED_PROTOCOL_FEATURES;
   return vhost_user_sock(dev, VHOST_USER_SET_PROTOCOL_FEATURES, protocol_features);
}

static int
vhost_user_set_vring_enable(struct virtio_user_dev *dev, struct vhost_vring_state *state)
{
        return vhost_user_sock(dev, VHOST_USER_SET_VRING_ENABLE, state);
}

static int
vhost_user_set_owner(struct virtio_user_dev *dev)
{
     return vhost_user_sock(dev, VHOST_USER_SET_OWNER, NULL);
}

struct virtio_user_backend_ops virtio_ops_user = {
        .setup = vhost_user_setup,
        .set_owner = vhost_user_set_owner,
        .get_features = vhost_user_get_features,
        .set_features = vhost_user_set_features,
        .set_memory_table = vhost_user_set_memory_table,
        .set_vring_num = vhost_user_set_vring_num,
	.set_vring_enable = vhost_user_set_vring_enable,
        .set_vring_base = vhost_user_set_vring_base,
        .get_vring_base = vhost_user_get_vring_base,
        .set_vring_call = vhost_user_set_vring_call,
        .set_vring_kick = vhost_user_set_vring_kick,
        .set_vring_addr = vhost_user_set_vring_addr,
	.get_config = vhost_user_get_config,
	.set_config = vhost_user_set_config,
        .get_protocol_features = vhost_user_get_proto_feature,
        .set_protocol_features = vhost_user_set_proto_feature,
	.get_queue_num = vhost_user_get_queue_num,
};

