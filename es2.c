/*
 * Greybus "AP" USB driver for "ES2" controller chips
 *
 * Copyright 2014-2015 Google Inc.
 * Copyright 2014-2015 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */
#include <linux/kthread.h>
#include <linux/sizes.h>
#include <linux/usb.h>
#include <linux/kfifo.h>
#include <linux/debugfs.h>
#include <asm/unaligned.h>

#include "greybus.h"
#include "kernel_ver.h"
#include "connection.h"
#include "greybus_trace.h"

/* Memory sizes for the buffers sent to/from the ES1 controller */
#define ES1_GBUF_MSG_SIZE_MAX	2048

static const struct usb_device_id id_table[] = {
	/* Made up numbers for the SVC USB Bridge in ES2 */
	{ USB_DEVICE(0xffff, 0x0002) },
	{ },
};
MODULE_DEVICE_TABLE(usb, id_table);

#define APB1_LOG_SIZE		SZ_16K
static struct dentry *apb1_log_dentry;
static struct dentry *apb1_log_enable_dentry;
static struct task_struct *apb1_log_task;
static DEFINE_KFIFO(apb1_log_fifo, char, APB1_LOG_SIZE);

/* Number of bulk in and bulk out couple */
#define NUM_BULKS		7

/*
 * Number of CPort IN urbs in flight at any point in time.
 * Adjust if we are having stalls in the USB buffer due to not enough urbs in
 * flight.
 */
#define NUM_CPORT_IN_URB	4

/* Number of CPort OUT urbs in flight at any point in time.
 * Adjust if we get messages saying we are out of urbs in the system log.
 */
#define NUM_CPORT_OUT_URB	(8 * NUM_BULKS)

/* vendor request APB1 log */
#define REQUEST_LOG		0x02

/* vendor request to map a cport to bulk in and bulk out endpoints */
#define REQUEST_EP_MAPPING	0x03

/* vendor request to get the number of cports available */
#define REQUEST_CPORT_COUNT	0x04

/*
 * @endpoint: bulk in endpoint for CPort data
 * @urb: array of urbs for the CPort in messages
 * @buffer: array of buffers for the @cport_in_urb urbs
 */
struct es1_cport_in {
	__u8 endpoint;
	struct urb *urb[NUM_CPORT_IN_URB];
	u8 *buffer[NUM_CPORT_IN_URB];
};

/*
 * @endpoint: bulk out endpoint for CPort data
 */
struct es1_cport_out {
	__u8 endpoint;
};

/**
 * es1_ap_dev - ES1 USB Bridge to AP structure
 * @usb_dev: pointer to the USB device we are.
 * @usb_intf: pointer to the USB interface we are bound to.
 * @hd: pointer to our greybus_host_device structure

 * @cport_in: endpoint, urbs and buffer for cport in messages
 * @cport_out: endpoint for for cport out messages
 * @cport_out_urb: array of urbs for the CPort out messages
 * @cport_out_urb_busy: array of flags to see if the @cport_out_urb is busy or
 *			not.
 * @cport_out_urb_cancelled: array of flags indicating whether the
 *			corresponding @cport_out_urb is being cancelled
 * @cport_out_urb_lock: locks the @cport_out_urb_busy "list"
 */
struct es1_ap_dev {
	struct usb_device *usb_dev;
	struct usb_interface *usb_intf;
	struct greybus_host_device *hd;

	struct es1_cport_in cport_in[NUM_BULKS];
	struct es1_cport_out cport_out[NUM_BULKS];
	struct urb *cport_out_urb[NUM_CPORT_OUT_URB];
	bool cport_out_urb_busy[NUM_CPORT_OUT_URB];
	bool cport_out_urb_cancelled[NUM_CPORT_OUT_URB];
	spinlock_t cport_out_urb_lock;

	int *cport_to_ep;
};

/**
 * cport_to_ep - information about cport to endpoints mapping
 * @cport_id: the id of cport to map to endpoints
 * @endpoint_in: the endpoint number to use for in transfer
 * @endpoint_out: he endpoint number to use for out transfer
 */
struct cport_to_ep {
	__le16 cport_id;
	__u8 endpoint_in;
	__u8 endpoint_out;
};

static inline struct es1_ap_dev *hd_to_es1(struct greybus_host_device *hd)
{
	return (struct es1_ap_dev *)&hd->hd_priv;
}

static void cport_out_callback(struct urb *urb);
static void usb_log_enable(struct es1_ap_dev *es1);
static void usb_log_disable(struct es1_ap_dev *es1);

/* Get the endpoints pair mapped to the cport */
static int cport_to_ep_pair(struct es1_ap_dev *es1, u16 cport_id)
{
	if (cport_id >= es1->hd->num_cports)
		return 0;
	return es1->cport_to_ep[cport_id];
}

#define ES1_TIMEOUT	500	/* 500 ms for the SVC to do something */

/* Disable for now until we work all of this out to keep a warning-free build */
#if 0
/* Test if the endpoints pair is already mapped to a cport */
static int ep_pair_in_use(struct es1_ap_dev *es1, int ep_pair)
{
	int i;

	for (i = 0; i < es1->hd->num_cports; i++) {
		if (es1->cport_to_ep[i] == ep_pair)
			return 1;
	}
	return 0;
}

/* Configure the endpoint mapping and send the request to APBridge */
static int map_cport_to_ep(struct es1_ap_dev *es1,
				u16 cport_id, int ep_pair)
{
	int retval;
	struct cport_to_ep *cport_to_ep;

	if (ep_pair < 0 || ep_pair >= NUM_BULKS)
		return -EINVAL;
	if (cport_id >= es1->hd->num_cports)
		return -EINVAL;
	if (ep_pair && ep_pair_in_use(es1, ep_pair))
		return -EINVAL;

	cport_to_ep = kmalloc(sizeof(*cport_to_ep), GFP_KERNEL);
	if (!cport_to_ep)
		return -ENOMEM;

	es1->cport_to_ep[cport_id] = ep_pair;
	cport_to_ep->cport_id = cpu_to_le16(cport_id);
	cport_to_ep->endpoint_in = es1->cport_in[ep_pair].endpoint;
	cport_to_ep->endpoint_out = es1->cport_out[ep_pair].endpoint;

	retval = usb_control_msg(es1->usb_dev,
				 usb_sndctrlpipe(es1->usb_dev, 0),
				 REQUEST_EP_MAPPING,
				 USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
				 0x00, 0x00,
				 (char *)cport_to_ep,
				 sizeof(*cport_to_ep),
				 ES1_TIMEOUT);
	if (retval == sizeof(*cport_to_ep))
		retval = 0;
	kfree(cport_to_ep);

	return retval;
}

/* Unmap a cport: use the muxed endpoints pair */
static int unmap_cport(struct es1_ap_dev *es1, u16 cport_id)
{
	return map_cport_to_ep(es1, cport_id, 0);
}
#endif

static struct urb *next_free_urb(struct es1_ap_dev *es1, gfp_t gfp_mask)
{
	struct urb *urb = NULL;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&es1->cport_out_urb_lock, flags);

	/* Look in our pool of allocated urbs first, as that's the "fastest" */
	for (i = 0; i < NUM_CPORT_OUT_URB; ++i) {
		if (es1->cport_out_urb_busy[i] == false &&
				es1->cport_out_urb_cancelled[i] == false) {
			es1->cport_out_urb_busy[i] = true;
			urb = es1->cport_out_urb[i];
			break;
		}
	}
	spin_unlock_irqrestore(&es1->cport_out_urb_lock, flags);
	if (urb)
		return urb;

	/*
	 * Crap, pool is empty, complain to the syslog and go allocate one
	 * dynamically as we have to succeed.
	 */
	dev_err(&es1->usb_dev->dev,
		"No free CPort OUT urbs, having to dynamically allocate one!\n");
	return usb_alloc_urb(0, gfp_mask);
}

static void free_urb(struct es1_ap_dev *es1, struct urb *urb)
{
	unsigned long flags;
	int i;
	/*
	 * See if this was an urb in our pool, if so mark it "free", otherwise
	 * we need to free it ourselves.
	 */
	spin_lock_irqsave(&es1->cport_out_urb_lock, flags);
	for (i = 0; i < NUM_CPORT_OUT_URB; ++i) {
		if (urb == es1->cport_out_urb[i]) {
			es1->cport_out_urb_busy[i] = false;
			urb = NULL;
			break;
		}
	}
	spin_unlock_irqrestore(&es1->cport_out_urb_lock, flags);

	/* If urb is not NULL, then we need to free this urb */
	usb_free_urb(urb);
}

/*
 * We (ab)use the operation-message header pad bytes to transfer the
 * cport id in order to minimise overhead.
 */
static void
gb_message_cport_pack(struct gb_operation_msg_hdr *header, u16 cport_id)
{
	header->pad[0] = cport_id;
}

/* Clear the pad bytes used for the CPort id */
static void gb_message_cport_clear(struct gb_operation_msg_hdr *header)
{
	header->pad[0] = 0;
}

/* Extract the CPort id packed into the header, and clear it */
static u16 gb_message_cport_unpack(struct gb_operation_msg_hdr *header)
{
	u16 cport_id = header->pad[0];

	gb_message_cport_clear(header);

	return cport_id;
}

/*
 * Returns zero if the message was successfully queued, or a negative errno
 * otherwise.
 */
static int message_send(struct greybus_host_device *hd, u16 cport_id,
			struct gb_message *message, gfp_t gfp_mask)
{
	struct es1_ap_dev *es1 = hd_to_es1(hd);
	struct usb_device *udev = es1->usb_dev;
	size_t buffer_size;
	int retval;
	struct urb *urb;
	int ep_pair;
	unsigned long flags;

	/*
	 * The data actually transferred will include an indication
	 * of where the data should be sent.  Do one last check of
	 * the target CPort id before filling it in.
	 */
	if (!cport_id_valid(hd, cport_id)) {
		pr_err("invalid destination cport 0x%02x\n", cport_id);
		return -EINVAL;
	}

	/* Find a free urb */
	urb = next_free_urb(es1, gfp_mask);
	if (!urb)
		return -ENOMEM;

	spin_lock_irqsave(&es1->cport_out_urb_lock, flags);
	message->hcpriv = urb;
	spin_unlock_irqrestore(&es1->cport_out_urb_lock, flags);

	/* Pack the cport id into the message header */
	gb_message_cport_pack(message->header, cport_id);

	buffer_size = sizeof(*message->header) + message->payload_size;

	ep_pair = cport_to_ep_pair(es1, cport_id);
	usb_fill_bulk_urb(urb, udev,
			  usb_sndbulkpipe(udev,
					  es1->cport_out[ep_pair].endpoint),
			  message->buffer, buffer_size,
			  cport_out_callback, message);
	urb->transfer_flags |= URB_ZERO_PACKET;
	trace_gb_host_device_send(hd, cport_id, buffer_size);
	retval = usb_submit_urb(urb, gfp_mask);
	if (retval) {
		pr_err("error %d submitting URB\n", retval);

		spin_lock_irqsave(&es1->cport_out_urb_lock, flags);
		message->hcpriv = NULL;
		spin_unlock_irqrestore(&es1->cport_out_urb_lock, flags);

		free_urb(es1, urb);
		gb_message_cport_clear(message->header);

		return retval;
	}

	return 0;
}

/*
 * Can not be called in atomic context.
 */
static void message_cancel(struct gb_message *message)
{
	struct greybus_host_device *hd = message->operation->connection->hd;
	struct es1_ap_dev *es1 = hd_to_es1(hd);
	struct urb *urb;
	int i;

	might_sleep();

	spin_lock_irq(&es1->cport_out_urb_lock);
	urb = message->hcpriv;

	/* Prevent dynamically allocated urb from being deallocated. */
	usb_get_urb(urb);

	/* Prevent pre-allocated urb from being reused. */
	for (i = 0; i < NUM_CPORT_OUT_URB; ++i) {
		if (urb == es1->cport_out_urb[i]) {
			es1->cport_out_urb_cancelled[i] = true;
			break;
		}
	}
	spin_unlock_irq(&es1->cport_out_urb_lock);

	usb_kill_urb(urb);

	if (i < NUM_CPORT_OUT_URB) {
		spin_lock_irq(&es1->cport_out_urb_lock);
		es1->cport_out_urb_cancelled[i] = false;
		spin_unlock_irq(&es1->cport_out_urb_lock);
	}

	usb_free_urb(urb);
}

static struct greybus_host_driver es1_driver = {
	.hd_priv_size		= sizeof(struct es1_ap_dev),
	.message_send		= message_send,
	.message_cancel		= message_cancel,
};

/* Common function to report consistent warnings based on URB status */
static int check_urb_status(struct urb *urb)
{
	struct device *dev = &urb->dev->dev;
	int status = urb->status;

	switch (status) {
	case 0:
		return 0;

	case -EOVERFLOW:
		dev_err(dev, "%s: overflow actual length is %d\n",
			__func__, urb->actual_length);
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -EILSEQ:
	case -EPROTO:
		/* device is gone, stop sending */
		return status;
	}
	dev_err(dev, "%s: unknown status %d\n", __func__, status);

	return -EAGAIN;
}

static void ap_disconnect(struct usb_interface *interface)
{
	struct es1_ap_dev *es1;
	struct usb_device *udev;
	int bulk_in;
	int i;

	es1 = usb_get_intfdata(interface);
	if (!es1)
		return;

	usb_log_disable(es1);

	/* Tear down everything! */
	for (i = 0; i < NUM_CPORT_OUT_URB; ++i) {
		struct urb *urb = es1->cport_out_urb[i];

		if (!urb)
			break;
		usb_kill_urb(urb);
		usb_free_urb(urb);
		es1->cport_out_urb[i] = NULL;
		es1->cport_out_urb_busy[i] = false;	/* just to be anal */
	}

	for (bulk_in = 0; bulk_in < NUM_BULKS; bulk_in++) {
		struct es1_cport_in *cport_in = &es1->cport_in[bulk_in];
		for (i = 0; i < NUM_CPORT_IN_URB; ++i) {
			struct urb *urb = cport_in->urb[i];

			if (!urb)
				break;
			usb_kill_urb(urb);
			usb_free_urb(urb);
			kfree(cport_in->buffer[i]);
			cport_in->buffer[i] = NULL;
		}
	}

	usb_set_intfdata(interface, NULL);
	udev = es1->usb_dev;
	greybus_remove_hd(es1->hd);
	kfree(es1->cport_to_ep);

	usb_put_dev(udev);
}

static void cport_in_callback(struct urb *urb)
{
	struct greybus_host_device *hd = urb->context;
	struct device *dev = &urb->dev->dev;
	struct gb_operation_msg_hdr *header;
	int status = check_urb_status(urb);
	int retval;
	u16 cport_id;

	if (status) {
		if ((status == -EAGAIN) || (status == -EPROTO))
			goto exit;
		dev_err(dev, "urb cport in error %d (dropped)\n", status);
		return;
	}

	if (urb->actual_length < sizeof(*header)) {
		dev_err(dev, "%s: short message received\n", __func__);
		goto exit;
	}

	/* Extract the CPort id, which is packed in the message header */
	header = urb->transfer_buffer;
	cport_id = gb_message_cport_unpack(header);

	if (cport_id_valid(hd, cport_id)) {
		trace_gb_host_device_recv(hd, cport_id, urb->actual_length);
		greybus_data_rcvd(hd, cport_id, urb->transfer_buffer,
							urb->actual_length);
	} else {
		dev_err(dev, "%s: invalid cport id 0x%02x received\n",
				__func__, cport_id);
	}
exit:
	/* put our urb back in the request pool */
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(dev, "%s: error %d in submitting urb.\n",
			__func__, retval);
}

static void cport_out_callback(struct urb *urb)
{
	struct gb_message *message = urb->context;
	struct greybus_host_device *hd = message->operation->connection->hd;
	struct es1_ap_dev *es1 = hd_to_es1(hd);
	int status = check_urb_status(urb);
	unsigned long flags;

	gb_message_cport_clear(message->header);

	spin_lock_irqsave(&es1->cport_out_urb_lock, flags);
	message->hcpriv = NULL;
	spin_unlock_irqrestore(&es1->cport_out_urb_lock, flags);

	/*
	 * Tell the submitter that the message send (attempt) is
	 * complete, and report the status.
	 */
	greybus_message_sent(hd, message, status);

	free_urb(es1, urb);
}

#define APB1_LOG_MSG_SIZE	64
static void apb1_log_get(struct es1_ap_dev *es1, char *buf)
{
	int retval;

	/* SVC messages go down our control pipe */
	do {
		retval = usb_control_msg(es1->usb_dev,
					usb_rcvctrlpipe(es1->usb_dev, 0),
					REQUEST_LOG,
					USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
					0x00, 0x00,
					buf,
					APB1_LOG_MSG_SIZE,
					ES1_TIMEOUT);
		if (retval > 0)
			kfifo_in(&apb1_log_fifo, buf, retval);
	} while (retval > 0);
}

static int apb1_log_poll(void *data)
{
	struct es1_ap_dev *es1 = data;
	char *buf;

	buf = kmalloc(APB1_LOG_MSG_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		msleep(1000);
		apb1_log_get(es1, buf);
	}

	kfree(buf);

	return 0;
}

static ssize_t apb1_log_read(struct file *f, char __user *buf,
				size_t count, loff_t *ppos)
{
	ssize_t ret;
	size_t copied;
	char *tmp_buf;

	if (count > APB1_LOG_SIZE)
		count = APB1_LOG_SIZE;

	tmp_buf = kmalloc(count, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

	copied = kfifo_out(&apb1_log_fifo, tmp_buf, count);
	ret = simple_read_from_buffer(buf, count, ppos, tmp_buf, copied);

	kfree(tmp_buf);

	return ret;
}

static const struct file_operations apb1_log_fops = {
	.read	= apb1_log_read,
};

static void usb_log_enable(struct es1_ap_dev *es1)
{
	if (!IS_ERR_OR_NULL(apb1_log_task))
		return;

	/* get log from APB1 */
	apb1_log_task = kthread_run(apb1_log_poll, es1, "apb1_log");
	if (IS_ERR(apb1_log_task))
		return;
	apb1_log_dentry = debugfs_create_file("apb1_log", S_IRUGO,
						gb_debugfs_get(), NULL,
						&apb1_log_fops);
}

static void usb_log_disable(struct es1_ap_dev *es1)
{
	if (IS_ERR_OR_NULL(apb1_log_task))
		return;

	debugfs_remove(apb1_log_dentry);
	apb1_log_dentry = NULL;

	kthread_stop(apb1_log_task);
	apb1_log_task = NULL;
}

static ssize_t apb1_log_enable_read(struct file *f, char __user *buf,
				size_t count, loff_t *ppos)
{
	char tmp_buf[3];
	int enable = !IS_ERR_OR_NULL(apb1_log_task);

	sprintf(tmp_buf, "%d\n", enable);
	return simple_read_from_buffer(buf, count, ppos, tmp_buf, 3);
}

static ssize_t apb1_log_enable_write(struct file *f, const char __user *buf,
				size_t count, loff_t *ppos)
{
	int enable;
	ssize_t retval;
	struct es1_ap_dev *es1 = (struct es1_ap_dev *)f->f_inode->i_private;

	retval = kstrtoint_from_user(buf, count, 10, &enable);
	if (retval)
		return retval;

	if (enable)
		usb_log_enable(es1);
	else
		usb_log_disable(es1);

	return count;
}

static const struct file_operations apb1_log_enable_fops = {
	.read	= apb1_log_enable_read,
	.write	= apb1_log_enable_write,
};

static int apb1_get_cport_count(struct usb_device *udev)
{
	int retval;
	__le16 *cport_count;

	cport_count = kmalloc(sizeof(*cport_count), GFP_KERNEL);
	if (!cport_count)
		return -ENOMEM;

	retval = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
				 REQUEST_CPORT_COUNT,
				 USB_DIR_IN | USB_TYPE_VENDOR |
				 USB_RECIP_INTERFACE, 0, 0, cport_count,
				 sizeof(*cport_count), ES1_TIMEOUT);
	if (retval < 0) {
		dev_err(&udev->dev, "Cannot retrieve CPort count: %d\n",
			retval);
		goto out;
	}

	retval = le16_to_cpu(*cport_count);

	/* We need to fit a CPort ID in one byte of a message header */
	if (retval > U8_MAX) {
		retval = U8_MAX;
		dev_warn(&udev->dev, "Limiting number of CPorts to U8_MAX\n");
	}

out:
	kfree(cport_count);
	return retval;
}

/*
 * The ES1 USB Bridge device contains 4 endpoints
 * 1 Control - usual USB stuff + AP -> SVC messages
 * 1 Interrupt IN - SVC -> AP messages
 * 1 Bulk IN - CPort data in
 * 1 Bulk OUT - CPort data out
 */
static int ap_probe(struct usb_interface *interface,
		    const struct usb_device_id *id)
{
	struct es1_ap_dev *es1;
	struct greybus_host_device *hd;
	struct usb_device *udev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	int bulk_in = 0;
	int bulk_out = 0;
	int retval = -ENOMEM;
	int i;
	int num_cports;

	udev = usb_get_dev(interface_to_usbdev(interface));

	num_cports = apb1_get_cport_count(udev);
	if (num_cports < 0) {
		usb_put_dev(udev);
		dev_err(&udev->dev, "Cannot retrieve CPort count: %d\n",
			num_cports);
		return num_cports;
	}

	hd = greybus_create_hd(&es1_driver, &udev->dev, ES1_GBUF_MSG_SIZE_MAX,
			       num_cports);
	if (IS_ERR(hd)) {
		usb_put_dev(udev);
		return PTR_ERR(hd);
	}

	es1 = hd_to_es1(hd);
	es1->hd = hd;
	es1->usb_intf = interface;
	es1->usb_dev = udev;
	spin_lock_init(&es1->cport_out_urb_lock);
	usb_set_intfdata(interface, es1);

	es1->cport_to_ep = kcalloc(hd->num_cports, sizeof(*es1->cport_to_ep),
				   GFP_KERNEL);
	if (!es1->cport_to_ep) {
		retval = -ENOMEM;
		goto error;
	}

	/* find all 3 of our endpoints */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(endpoint)) {
			es1->cport_in[bulk_in++].endpoint =
				endpoint->bEndpointAddress;
		} else if (usb_endpoint_is_bulk_out(endpoint)) {
			es1->cport_out[bulk_out++].endpoint =
				endpoint->bEndpointAddress;
		} else {
			dev_err(&udev->dev,
				"Unknown endpoint type found, address %x\n",
				endpoint->bEndpointAddress);
		}
	}
	if ((bulk_in == 0) ||
	    (bulk_out == 0)) {
		dev_err(&udev->dev, "Not enough endpoints found in device, aborting!\n");
		goto error;
	}

	/* Allocate buffers for our cport in messages and start them up */
	for (bulk_in = 0; bulk_in < NUM_BULKS; bulk_in++) {
		struct es1_cport_in *cport_in = &es1->cport_in[bulk_in];
		for (i = 0; i < NUM_CPORT_IN_URB; ++i) {
			struct urb *urb;
			u8 *buffer;

			urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!urb)
				goto error;
			buffer = kmalloc(ES1_GBUF_MSG_SIZE_MAX, GFP_KERNEL);
			if (!buffer)
				goto error;

			usb_fill_bulk_urb(urb, udev,
					  usb_rcvbulkpipe(udev,
							  cport_in->endpoint),
					  buffer, ES1_GBUF_MSG_SIZE_MAX,
					  cport_in_callback, hd);
			cport_in->urb[i] = urb;
			cport_in->buffer[i] = buffer;
			retval = usb_submit_urb(urb, GFP_KERNEL);
			if (retval)
				goto error;
		}
	}

	/* Allocate urbs for our CPort OUT messages */
	for (i = 0; i < NUM_CPORT_OUT_URB; ++i) {
		struct urb *urb;

		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb)
			goto error;

		es1->cport_out_urb[i] = urb;
		es1->cport_out_urb_busy[i] = false;	/* just to be anal */
	}

	apb1_log_enable_dentry = debugfs_create_file("apb1_log_enable",
							(S_IWUSR | S_IRUGO),
							gb_debugfs_get(), es1,
							&apb1_log_enable_fops);
	return 0;
error:
	ap_disconnect(interface);

	return retval;
}

static struct usb_driver es1_ap_driver = {
	.name =		"es2_ap_driver",
	.probe =	ap_probe,
	.disconnect =	ap_disconnect,
	.id_table =	id_table,
};

module_usb_driver(es1_ap_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Greg Kroah-Hartman <gregkh@linuxfoundation.org>");
