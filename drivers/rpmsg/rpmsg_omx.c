/*
 * OMX offloading remote processor driver
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/skbuff.h>
#include <linux/sched.h>
#include <linux/rpmsg.h>
#include <linux/rpmsg_omx.h>
#include <linux/completion.h>
#include <linux/remoteproc.h>
#include <linux/fdtable.h>

#ifdef CONFIG_DMA_SHARED_BUFFER
#include <linux/dma-buf.h>
#endif

/* maximum OMX devices this driver can handle */
#define MAX_OMX_DEVICES		8

enum rpc_omx_map_info_type {
	RPC_OMX_MAP_INFO_NONE          = 0,
	RPC_OMX_MAP_INFO_ONE_BUF       = 1,
	RPC_OMX_MAP_INFO_TWO_BUF       = 2,
	RPC_OMX_MAP_INFO_THREE_BUF     = 3,
	RPC_OMX_MAP_INFO_MAX           = 0x7FFFFFFF
};

struct rpmsg_omx_service {
	struct cdev cdev;
	struct device *dev;
	struct rpmsg_channel *rpdev;
	int minor;
	struct list_head list;
	struct mutex lock;
	struct completion comp;
};

struct rpmsg_omx_instance {
	struct list_head next;
	struct rpmsg_omx_service *omxserv;
	struct sk_buff_head queue;
	struct mutex lock;
	wait_queue_head_t readq;
	struct completion reply_arrived;
	struct rpmsg_endpoint *ept;
	u32 dst;
	int state;
#ifdef CONFIG_DMA_SHARED_BUFFER
	struct idr dma_idr;
#endif
};

#ifdef CONFIG_DMA_SHARED_BUFFER
struct rpmsg_omx_dma_info {
	struct dma_buf *dbuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
};
#endif

static struct class *rpmsg_omx_class;
static dev_t rpmsg_omx_dev;

/* store all remote omx connection services (usually one per remoteproc) */
static DEFINE_IDR(rpmsg_omx_services);
static DEFINE_SPINLOCK(rpmsg_omx_services_lock);

static int _rpmsg_pa_to_da(struct rpmsg_omx_instance *omx, u32 pa, u32 *da)
{
	int ret = 0;
	struct rproc *rproc;
	u64 temp_da;

	mutex_lock(&omx->lock);
	if (omx->state == OMX_FAIL)
		ret = -ENXIO;
	else
		rproc = vdev_to_rproc(omx->omxserv->rpdev->vrp->vdev);
	mutex_unlock(&omx->lock);
	if (ret)
		return ret;

	ret = rproc_pa_to_da(rproc, (phys_addr_t) pa, &temp_da);
	if (ret)
		pr_err("error with pa to da from rproc %d\n", ret);
	else
		/* we know it is a 32 bit address */
		*da = (u32)temp_da;

	return ret;
}

#ifdef CONFIG_DMA_SHARED_BUFFER
/*
 * Proudly inspired by the OMAP RPC code (omaprpc_dmabuf.c) by Hervé Fache
 */
static int rpmsg_omx_pin_buffer(struct rpmsg_omx_instance *omx, int fd)
{
	struct rpmsg_omx_service *omxserv = omx->omxserv;
	struct rpmsg_omx_dma_info *dma = kmalloc(sizeof *dma, GFP_KERNEL);
	int id;
	int ret;

	if (!dma)
		return -ENOMEM;

	dma->dbuf = dma_buf_get(fd);
	dev_dbg(omxserv->dev, "pining with fd=%u/dbuf=%p\n", fd, dma->dbuf);
	if (IS_ERR(dma->dbuf)) {
		ret = PTR_ERR(dma->dbuf);
		goto err;
	}

	dma->attach = dma_buf_attach(dma->dbuf, omxserv->dev);
	if (IS_ERR(dma->attach)) {
		ret = PTR_ERR(dma->attach);
		goto err_buf_put;
	}

	dma->sgt = dma_buf_map_attachment(dma->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(dma->sgt)) {
		ret = PTR_ERR(dma->sgt);
		goto err_detach;
	}

	if (!idr_pre_get(&omx->dma_idr, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	mutex_lock(&omx->lock);
	ret = idr_get_new_above(&omx->dma_idr, dma, fd, &id);
	mutex_unlock(&omx->lock);
	if (ret)
		goto err_unmap;

	if (fd != id) {
		ret = -EINVAL;
		goto err_rem_idr;
	}

	return 0;

err_rem_idr:
	mutex_lock(&omx->lock);
	idr_remove(&omx->dma_idr, id);
	mutex_unlock(&omx->lock);
err_unmap:
	dma_buf_unmap_attachment(dma->attach, dma->sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dma->dbuf, dma->attach);
err_buf_put:
	dma_buf_put(dma->dbuf);
err:
	kfree(dma);
	dev_err(omxserv->dev, "error pining buffer %d\n", ret);

	return ret;
}

static struct rpmsg_omx_dma_info *
rpmsg_omx_dma_find(struct rpmsg_omx_instance *omx, int fd, phys_addr_t *pa)
{
	struct device *dev = omx->omxserv->dev;
	struct rpmsg_omx_dma_info *dma;
	struct dma_buf *dbuf = dma_buf_get(fd);

	dev_dbg(dev, "looking for fd=%u/dbuf=%p\n", fd, dbuf);

	mutex_lock(&omx->lock);
	dma = idr_find(&omx->dma_idr, fd);
	mutex_unlock(&omx->lock);
	if (dma)
		*pa = sg_dma_address(dma->sgt->sgl) + dma->sgt->sgl->offset;

	return dma;
}

static int rpmsg_omx_remove_dma_buffer(struct rpmsg_omx_instance *omx,
				struct rpmsg_omx_dma_info *dma)
{
	struct device *dev = omx->omxserv->dev;

	dev_dbg(dev, "unpining with dbuf=%p\n", dma->dbuf);

	dma_buf_unmap_attachment(dma->attach, dma->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(dma->dbuf, dma->attach);
	dma_buf_put(dma->dbuf);
	kfree(dma);

	return 0;
}

static int rpmsg_omx_unpin_buffer(struct rpmsg_omx_instance *omx, int fd)
{
	struct rpmsg_omx_dma_info *dma;
	phys_addr_t pa;

	dma = rpmsg_omx_dma_find(omx, fd, &pa);
	if (!dma)
		return -EINVAL;

	idr_remove(&omx->dma_idr, fd);

	return rpmsg_omx_remove_dma_buffer(omx, dma);
}
#endif

/*
 * This takes a buffer or map and return one or two 'device addresses' (wrongly
 * called va and va2, should be called da and da2), meaning the address that the
 * device considers 'physical', but is in fact mapped by the IOMMU (remoteproc
 * takes care of that.)
 */
static int _rpmsg_omx_buffer_get(struct rpmsg_omx_instance *omx,
					long buffer, phys_addr_t *da)
{
	struct device *dev = omx->omxserv->dev;
	int ret = -EIO;
	phys_addr_t pa;

#ifdef CONFIG_DMA_SHARED_BUFFER
	{
		int fd = buffer;
		/* find the base of the dam buf from the list */
		if (rpmsg_omx_dma_find(omx, fd, &pa))
			ret = 0;
		else
			dev_err(dev, "error getting fd %d\n", fd);
	}
#endif
	if (!ret)
		return _rpmsg_pa_to_da(omx, pa, da);

	dev_err(dev, "buffer lookup failed %x\n", ret);

	return ret;
}

static int
_rpmsg_omx_map_buf(struct rpmsg_omx_instance *omx, struct omx_packet *packet)
{
	void *data = packet->data;
	enum rpc_omx_map_info_type maptype =
					*(enum rpc_omx_map_info_type *)data;
	int offset = *(int *)(data + sizeof(maptype));
	u32 *buffer = (u32 *)(data + offset);

	/*
	 * maptype has the good idea to count for 0 = none to 3 = three buffers
	 */
	if (maptype < RPC_OMX_MAP_INFO_NONE ||
					maptype > RPC_OMX_MAP_INFO_THREE_BUF)
		return -EINVAL;

	while (maptype--) {
		/* Replace given data by device address in message */
		int ret = _rpmsg_omx_buffer_get(omx, *buffer, buffer);
		if (ret)
			return ret;
		++buffer;
	}

	return 0;
}

static void rpmsg_omx_cb(struct rpmsg_channel *rpdev, void *data, int len,
							void *priv, u32 src)
{
	struct omx_msg_hdr *hdr = data;
	struct rpmsg_omx_instance *omx = priv;
	struct omx_conn_rsp *rsp;
	struct sk_buff *skb;
	char *skbdata;

	if (len < sizeof(*hdr) || hdr->len < len - sizeof(*hdr)) {
		dev_warn(&rpdev->dev, "%s: truncated message\n", __func__);
		return;
	}

	dev_dbg(&rpdev->dev, "%s: incoming msg src 0x%x type %d len %d\n",
					__func__, src, hdr->type, hdr->len);
#if 0
	print_hex_dump(KERN_DEBUG, "rpmsg_omx RX: ", DUMP_PREFIX_NONE, 16, 1,
		       data, len,  true);
#endif
	switch (hdr->type) {
	case OMX_CONN_RSP:
		if (hdr->len < sizeof(*rsp)) {
			dev_warn(&rpdev->dev, "incoming empty response msg\n");
			break;
		}
		rsp = (struct omx_conn_rsp *) hdr->data;
		dev_dbg(&rpdev->dev, "conn rsp: status %d addr %d\n",
			       rsp->status, rsp->addr);
		omx->dst = rsp->addr;
		mutex_lock(&omx->lock);
		if (rsp->status)
			omx->state = OMX_FAIL;
		else if (omx->state != OMX_FAIL)
			omx->state = OMX_CONNECTED;
		mutex_unlock(&omx->lock);

		complete(&omx->reply_arrived);
		break;
	case OMX_RAW_MSG:
		skb = alloc_skb(hdr->len, GFP_KERNEL);
		if (!skb) {
			dev_err(&rpdev->dev, "alloc_skb err: %u\n", hdr->len);
			break;
		}
		skbdata = skb_put(skb, hdr->len);
		memcpy(skbdata, hdr->data, hdr->len);

		mutex_lock(&omx->lock);
		skb_queue_tail(&omx->queue, skb);
		mutex_unlock(&omx->lock);
		/* wake up any blocking processes, waiting for new data */
		wake_up_interruptible(&omx->readq);
		break;
	default:
		dev_warn(&rpdev->dev, "unexpected msg type: %d\n", hdr->type);
		break;
	}
}

static int rpmsg_omx_connect(struct rpmsg_omx_instance *omx, char *omxname)
{
	struct omx_msg_hdr *hdr;
	struct omx_conn_req *payload;
	struct rpmsg_omx_service *omxserv = omx->omxserv;
	char connect_msg[sizeof(*hdr) + sizeof(*payload)] = { 0 };
	int ret;

	if (omx->state == OMX_CONNECTED) {
		dev_dbg(omxserv->dev, "endpoint already connected\n");
		return -EISCONN;
	}

	hdr = (struct omx_msg_hdr *)connect_msg;
	hdr->type = OMX_CONN_REQ;
	hdr->flags = 0;
	hdr->len = strlen(omxname) + 1;
	payload = (struct omx_conn_req *)hdr->data;
	strcpy(payload->name, omxname);

	/* send a conn req to the remote OMX connection service. use
	 * the new local address that was just allocated by ->open */
	mutex_lock(&omx->lock);
	if (omx->state == OMX_FAIL) {
		ret = -ENXIO;
	} else {
		ret = rpmsg_send_offchannel(omxserv->rpdev, omx->ept->addr,
			omxserv->rpdev->dst, connect_msg, sizeof(connect_msg));
	}
	mutex_unlock(&omx->lock);
	if (ret) {
		dev_err(omxserv->dev, "rpmsg_send failed: %d\n", ret);
		return ret;
	}

	/* wait until a connection reply arrives or 5 seconds elapse */
	ret = wait_for_completion_interruptible_timeout(&omx->reply_arrived,
						msecs_to_jiffies(5000));

	mutex_lock(&omx->lock);
	if (omx->state == OMX_FAIL) {
		ret = -ENXIO;
	} else if (omx->state == OMX_UNCONNECTED) {
		if (ret)
			dev_err(omxserv->dev, "premature wakeup: %d\n", ret);
		else
			ret = -ETIMEDOUT;
	}
	mutex_unlock(&omx->lock);

	return ret;
}

static
long rpmsg_omx_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct rpmsg_omx_instance *omx = filp->private_data;
	struct rpmsg_omx_service *omxserv = omx->omxserv;
	char buf[48];
	int ret = 0;

	dev_dbg(omxserv->dev, "%s: cmd %d, arg 0x%lx\n", __func__, cmd, arg);

	if (_IOC_TYPE(cmd) != OMX_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > OMX_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case OMX_IOCCONNECT:
		ret = copy_from_user(buf, (char __user *) arg, sizeof(buf));
		if (ret) {
			dev_err(omxserv->dev,
				"%s: %d: copy_from_user fail: %d\n", __func__,
				_IOC_NR(cmd), ret);
			ret = -EFAULT;
			break;
		}
		/* make sure user input is null terminated */
		buf[sizeof(buf) - 1] = '\0';
		ret = rpmsg_omx_connect(omx, buf);
		break;
#ifdef CONFIG_DMA_SHARED_BUFFER
	case OMX_IOCBUFREGISTER:
		ret = rpmsg_omx_pin_buffer(omx, (int)arg);
		break;
	case OMX_IOCBUFUNREGISTER:
		ret = rpmsg_omx_unpin_buffer(omx, (int)arg);
		break;
#endif
	default:
		dev_warn(omxserv->dev, "unhandled ioctl cmd: %d\n", cmd);
		break;
	}

	return ret;
}

static int rpmsg_omx_open(struct inode *inode, struct file *filp)
{
	struct rpmsg_omx_service *omxserv;
	struct rpmsg_omx_instance *omx;
	int ret;

	omxserv = container_of(inode->i_cdev, struct rpmsg_omx_service, cdev);

	omx = kzalloc(sizeof(*omx), GFP_KERNEL);
	if (!omx)
		return -ENOMEM;

	mutex_init(&omx->lock);
	skb_queue_head_init(&omx->queue);
	init_waitqueue_head(&omx->readq);
	omx->omxserv = omxserv;
	omx->state = OMX_UNCONNECTED;

	mutex_lock(&omxserv->lock);
	if (!omxserv->rpdev && filp->f_flags & O_NONBLOCK) {
		mutex_unlock(&omxserv->lock);
		ret = -EBUSY;
		goto err;
	}

	/*
	 * if there is no rpdev that means it was destroyed due to a rproc
	 * crash, so wait until the rpdev is created again
	 */
	while (!omxserv->rpdev) {
		mutex_unlock(&omxserv->lock);
		ret = wait_for_completion_interruptible(&omxserv->comp);
		if (ret)
			goto err;
		mutex_lock(&omxserv->lock);
	}

	/* assign a new, unique, local address and associate omx with it */
	omx->ept = rpmsg_create_ept(omxserv->rpdev, rpmsg_omx_cb, omx,
							RPMSG_ADDR_ANY);
	if (!omx->ept) {
		mutex_unlock(&omxserv->lock);
		dev_err(omxserv->dev, "create ept failed\n");
		ret = -ENOMEM;
		goto err;
	}
	list_add(&omx->next, &omxserv->list);
	mutex_unlock(&omxserv->lock);

#ifdef CONFIG_DMA_SHARED_BUFFER
	idr_init(&omx->dma_idr);
#endif

	init_completion(&omx->reply_arrived);

	/* associate filp with the new omx instance */
	filp->private_data = omx;

	dev_dbg(omxserv->dev, "local addr assigned: 0x%x\n", omx->ept->addr);

	return 0;
err:
	kfree(omx);
	return ret;
}

#ifdef CONFIG_DMA_SHARED_BUFFER
int rpmsg_omx_idr_unpin_buffer(int id, void *node, void *omx)
{
	return rpmsg_omx_remove_dma_buffer(omx, node);
}
#endif

static int rpmsg_omx_release(struct inode *inode, struct file *filp)
{
	struct rpmsg_omx_instance *omx = filp->private_data;
	struct rpmsg_omx_service *omxserv = omx->omxserv;
	char kbuf[512];
	struct omx_msg_hdr *hdr = (struct omx_msg_hdr *) kbuf;
	struct omx_disc_req *disc_req = (struct omx_disc_req *)hdr->data;
	int use, ret = 0;

	/* todo: release resources here */

	/* send a disconnect msg with the OMX instance addr
	 * only if connected otherwise just destroy
	 */
	if (omx->state == OMX_CONNECTED) {
		hdr->type = OMX_DISCONNECT;
		hdr->flags = 0;
		hdr->len = sizeof(struct omx_disc_req);
		disc_req->addr = omx->dst;
		use = sizeof(*hdr) + hdr->len;

		dev_dbg(omxserv->dev, "Disconnecting from OMX service at %d\n",
			 omx->dst);

		mutex_lock(&omx->lock);
		/*
		 * If state == fail, remote processor crashed, so don't send it
		 * any message.
		 */
		if (omx->state != OMX_FAIL)
			/* send the msg to the remote OMX connection service */
			ret = rpmsg_send_offchannel(omxserv->rpdev,
				omx->ept->addr, omxserv->rpdev->dst, kbuf, use);
		mutex_unlock(&omx->lock);
		if (ret)
			dev_err(omxserv->dev, "rpmsg_send failed: %d\n", ret);
	}

#ifdef CONFIG_DMA_SHARED_BUFFER
	idr_for_each(&omx->dma_idr, rpmsg_omx_idr_unpin_buffer, omx);
	idr_remove_all(&omx->dma_idr);
	idr_destroy(&omx->dma_idr);
#endif
	mutex_lock(&omxserv->lock);
	list_del(&omx->next);
	/*
	 * only destroy ept if there is a valid rpdev. Otherwise, it is not
	 * needed because it was already destroyed by rpmsg_omx_remove function
	 */
	if (omxserv->rpdev)
		rpmsg_destroy_ept(omx->ept);
	mutex_unlock(&omxserv->lock);
	kfree(omx);

	return 0;
}

static ssize_t rpmsg_omx_read(struct file *filp, char __user *buf,
						size_t len, loff_t *offp)
{
	struct rpmsg_omx_instance *omx = filp->private_data;
	struct sk_buff *skb;
	int use;


	if (omx->state == OMX_UNCONNECTED)
		return -ENOTCONN;

	mutex_lock(&omx->lock);
	/* nothing to read ? */
	if (skb_queue_empty(&omx->queue)) {
		mutex_unlock(&omx->lock);
		/* non-blocking requested ? return now */
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/* otherwise block, and wait for data */
		if (wait_event_interruptible(omx->readq,
				(!skb_queue_empty(&omx->queue) ||
				omx->state == OMX_FAIL)))
			return -ERESTARTSYS;
		mutex_lock(&omx->lock);
	}

	if (omx->state == OMX_FAIL) {
		mutex_unlock(&omx->lock);
		return -ENXIO;
	}

	skb = skb_dequeue(&omx->queue);
	mutex_unlock(&omx->lock);
	if (!skb) {
		dev_err(omx->omxserv->dev, "err is rmpsg_omx racy ?\n");
		return -EIO;
	}

	use = min(len, skb->len);

	if (copy_to_user(buf, skb->data, use)) {
		dev_err(omx->omxserv->dev, "%s: copy_to_user fail\n", __func__);
		use = -EFAULT;
	}

	kfree_skb(skb);
	return use;
}

static ssize_t rpmsg_omx_write(struct file *filp, const char __user *ubuf,
						size_t len, loff_t *offp)
{
	struct rpmsg_omx_instance *omx = filp->private_data;
	struct rpmsg_omx_service *omxserv = omx->omxserv;
	char kbuf[512];
	struct omx_msg_hdr *hdr = (struct omx_msg_hdr *) kbuf;
	int use, ret;

	if (omx->state == OMX_UNCONNECTED)
		return -ENOTCONN;

	/*
	 * for now, limit msg size to 512 bytes (incl. header).
	 * (note: rpmsg's limit is even tighter. this whole thing needs fixing)
	 */
	use = min(sizeof(kbuf) - sizeof(*hdr), len);

	/*
	 * copy the data. Later, number of copies can be optimized if found to
	 * be significant in real use cases
	 */
	if (copy_from_user(hdr->data, ubuf, use))
		return -EFAULT;

	ret = _rpmsg_omx_map_buf(omx, (void *)hdr->data);
	if (ret < 0)
		return ret;

	hdr->type = OMX_RAW_MSG;
	hdr->flags = 0;
	hdr->len = use;

	mutex_lock(&omx->lock);
	if (omx->state == OMX_FAIL)
		ret = -ENXIO;
	else
		ret = rpmsg_send_offchannel(omxserv->rpdev, omx->ept->addr,
					omx->dst, kbuf, use + sizeof(*hdr));
	mutex_unlock(&omx->lock);
	if (ret) {
		dev_err(omxserv->dev, "rpmsg_send failed: %d\n", ret);
		return ret;
	}

	return use;
}

static
unsigned int rpmsg_omx_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct rpmsg_omx_instance *omx = filp->private_data;
	unsigned int mask = 0;

	poll_wait(filp, &omx->readq, wait);

	if (!skb_queue_empty(&omx->queue))
		mask |= POLLIN | POLLRDNORM;

	/* implement missing rpmsg virtio functionality here */
	if (true)
		mask |= POLLOUT | POLLWRNORM;

	if (omx->state == OMX_FAIL)
		mask = POLLERR;

	return mask;
}

static const struct file_operations rpmsg_omx_fops = {
	.open		= rpmsg_omx_open,
	.release	= rpmsg_omx_release,
	.unlocked_ioctl	= rpmsg_omx_ioctl,
	.read		= rpmsg_omx_read,
	.write		= rpmsg_omx_write,
	.poll		= rpmsg_omx_poll,
	.owner		= THIS_MODULE,
};

static int _match_omx_service(int id, void *p, void *data)
{
	struct rpmsg_omx_service *omxserv = p;

	return strcmp(dev_name(omxserv->dev), data) ? 0 : (int)p;
}

static int rpmsg_omx_probe(struct rpmsg_channel *rpdev)
{
	int ret, major, minor;
	struct rpmsg_omx_service *omxserv;

	/* look for an already created omx service */
	spin_lock(&rpmsg_omx_services_lock);
	omxserv = (struct rpmsg_omx_service *)idr_for_each(&rpmsg_omx_services,
					_match_omx_service, rpdev->id.name);
	spin_unlock(&rpmsg_omx_services_lock);
	if (omxserv) {
		omxserv->rpdev = rpdev;
		dev_set_drvdata(&rpdev->dev, omxserv);
		goto serv_up;
	}

	if (!idr_pre_get(&rpmsg_omx_services, GFP_KERNEL)) {
		dev_err(&rpdev->dev, "idr_pre_get failes\n");
		return -ENOMEM;
	}

	omxserv = kzalloc(sizeof(*omxserv), GFP_KERNEL);
	if (!omxserv) {
		dev_err(&rpdev->dev, "kzalloc failed\n");
		return -ENOMEM;
	}

	/* dynamically assign a new minor number */
	spin_lock(&rpmsg_omx_services_lock);
	ret = idr_get_new(&rpmsg_omx_services, omxserv, &minor);
	spin_unlock(&rpmsg_omx_services_lock);
	if (ret) {
		dev_err(&rpdev->dev, "failed to idr_get_new: %d\n", ret);
		goto free_omx;
	}

	INIT_LIST_HEAD(&omxserv->list);
	mutex_init(&omxserv->lock);
	init_completion(&omxserv->comp);

	omxserv->minor = minor;
	omxserv->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, omxserv);

	major = MAJOR(rpmsg_omx_dev);

	cdev_init(&omxserv->cdev, &rpmsg_omx_fops);
	omxserv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&omxserv->cdev, MKDEV(major, minor), 1);
	if (ret) {
		dev_err(&rpdev->dev, "cdev_add failed: %d\n", ret);
		goto rem_idr;
	}

	omxserv->dev = device_create(rpmsg_omx_class, &rpdev->dev,
			MKDEV(major, minor), NULL,
			rpdev->id.name);
	if (IS_ERR(omxserv->dev)) {
		ret = PTR_ERR(omxserv->dev);
		dev_err(&rpdev->dev, "device_create failed: %d\n", ret);
		goto clean_cdev;
	}
serv_up:
	complete_all(&omxserv->comp);

	dev_info(omxserv->dev, "new OMX connection srv channel: %u -> %u!\n",
						rpdev->src, rpdev->dst);
	return 0;

clean_cdev:
	cdev_del(&omxserv->cdev);
rem_idr:
	spin_lock(&rpmsg_omx_services_lock);
	idr_remove(&rpmsg_omx_services, minor);
	spin_unlock(&rpmsg_omx_services_lock);
free_omx:
	kfree(omxserv);
	return ret;
}

static void __devexit rpmsg_omx_remove(struct rpmsg_channel *rpdev)
{
	struct rpmsg_omx_service *omxserv = dev_get_drvdata(&rpdev->dev);
	int major = MAJOR(rpmsg_omx_dev);
	struct rpmsg_omx_instance *omx;
	struct rproc *rproc = vdev_to_rproc(rpdev->vrp->vdev);

	dev_info(omxserv->dev, "rpmsg omx driver is removed\n");

	if (rproc->state != RPROC_CRASHED) {
		device_destroy(rpmsg_omx_class, MKDEV(major, omxserv->minor));
		cdev_del(&omxserv->cdev);
		spin_lock(&rpmsg_omx_services_lock);
		idr_remove(&rpmsg_omx_services, omxserv->minor);
		spin_unlock(&rpmsg_omx_services_lock);
		kfree(omxserv);
		return;
	}

	/* If it is a recovery, don't clean the omxserv */
	init_completion(&omxserv->comp);
	mutex_lock(&omxserv->lock);
	list_for_each_entry(omx, &omxserv->list, next) {
		mutex_lock(&omx->lock);
		/* set omx instance to fail state */
		omx->state = OMX_FAIL;
		mutex_unlock(&omx->lock);
		/* unblock any pending omx thread */
		complete_all(&omx->reply_arrived);
		wake_up_interruptible(&omx->readq);
		rpmsg_destroy_ept(omx->ept);
	}
	omxserv->rpdev = NULL;
	mutex_unlock(&omxserv->lock);
}

static void rpmsg_omx_driver_cb(struct rpmsg_channel *rpdev, void *data,
						int len, void *priv, u32 src)
{
	dev_warn(&rpdev->dev, "uhm, unexpected message\n");

#if 0
	print_hex_dump(KERN_DEBUG, __func__, DUMP_PREFIX_NONE, 16, 1,
		       data, len,  true);
#endif
}

static struct rpmsg_device_id rpmsg_omx_id_table[] = {
	{ .name =	"rpmsg-omx0" }, /* ipu_c0 */
	{ .name =	"rpmsg-omx1" }, /* ipu_c1 */
	{ .name =	"rpmsg-omx2" }, /* dsp */
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_omx_id_table);

static struct rpmsg_driver rpmsg_omx_driver = {
	.drv.name	= KBUILD_MODNAME,
	.drv.owner	= THIS_MODULE,
	.id_table	= rpmsg_omx_id_table,
	.probe		= rpmsg_omx_probe,
	.callback	= rpmsg_omx_driver_cb,
	.remove		= __devexit_p(rpmsg_omx_remove),
};

static int __init init(void)
{
	int ret;

	ret = alloc_chrdev_region(&rpmsg_omx_dev, 0, MAX_OMX_DEVICES,
							KBUILD_MODNAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		goto out;
	}

	rpmsg_omx_class = class_create(THIS_MODULE, KBUILD_MODNAME);
	if (IS_ERR(rpmsg_omx_class)) {
		ret = PTR_ERR(rpmsg_omx_class);
		pr_err("class_create failed: %d\n", ret);
		goto unreg_region;
	}

	return register_rpmsg_driver(&rpmsg_omx_driver);

unreg_region:
	unregister_chrdev_region(rpmsg_omx_dev, MAX_OMX_DEVICES);
out:
	return ret;
}
module_init(init);

static void __exit fini(void)
{
	unregister_rpmsg_driver(&rpmsg_omx_driver);
	class_destroy(rpmsg_omx_class);
	unregister_chrdev_region(rpmsg_omx_dev, MAX_OMX_DEVICES);
}
module_exit(fini);

MODULE_DESCRIPTION("OMX offloading rpmsg driver");
MODULE_LICENSE("GPL v2");
