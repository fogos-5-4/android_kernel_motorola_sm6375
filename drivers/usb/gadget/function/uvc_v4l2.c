// SPDX-License-Identifier: GPL-2.0+
/*
 *	uvc_v4l2.c  --  USB Video Class Gadget driver
 *
 *	Copyright (C) 2009-2010
 *	    Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/usb/g_uvc.h>
#include <linux/usb/video.h>
#include <linux/videodev2.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "f_uvc.h"
#include "uvc.h"
#include "uvc_queue.h"
#include "uvc_video.h"
#include "uvc_v4l2.h"

/* --------------------------------------------------------------------------
 * Requests handling
 */

static int
uvc_send_response(struct uvc_device *uvc, struct uvc_request_data *data)
{
	struct usb_composite_dev *cdev = uvc->func.config->cdev;
	struct usb_request *req = uvc->control_req;

	if (data->length < 0)
		return usb_ep_set_halt(cdev->gadget->ep0);

	req->length = min_t(unsigned int, uvc->event_length, data->length);
	req->zero = data->length < uvc->event_length;

	memcpy(req->buf, data->data, req->length);

	return usb_ep_queue(cdev->gadget->ep0, req, GFP_KERNEL);
}

/* --------------------------------------------------------------------------
 * V4L2 ioctls
 */

struct uvc_format {
	u8 bpp;
	u32 fcc;
};

static struct uvc_format uvc_formats[] = {
	{ 16, V4L2_PIX_FMT_YUYV  },
	{ 0,  V4L2_PIX_FMT_MJPEG },
	{ 0,  V4L2_PIX_FMT_H264 },
	{ 12, V4L2_PIX_FMT_YUV420 },
	{ 8, V4L2_PIX_FMT_GREY },
};

static int
uvc_v4l2_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct usb_composite_dev *cdev = uvc->func.config->cdev;

	strlcpy(cap->driver, "g_uvc", sizeof(cap->driver));
	strlcpy(cap->card, cdev->gadget->name, sizeof(cap->card));
	strlcpy(cap->bus_info, dev_name(&cdev->gadget->dev),
		sizeof(cap->bus_info));
	return 0;
}

static int
uvc_v4l2_get_format(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_video *video = &uvc->video;

	fmt->fmt.pix.pixelformat = video->fcc;
	fmt->fmt.pix.width = video->width;
	fmt->fmt.pix.height = video->height;
	fmt->fmt.pix.field = V4L2_FIELD_NONE;
	fmt->fmt.pix.bytesperline = video->bpp * video->width / 8;
	fmt->fmt.pix.sizeimage = video->imagesize;
	fmt->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	fmt->fmt.pix.priv = 0;

	return 0;
}

static const struct uvc_descriptor_header * const *
uvc_v4l2_get_streaming_desc(struct uvc_device *uvc)
{
	struct usb_composite_dev *cdev = uvc->func.config ? uvc->func.config->cdev : NULL;
	enum usb_device_speed speed = cdev && cdev->gadget ? cdev->gadget->speed : USB_SPEED_UNKNOWN;

	if ((speed == USB_SPEED_SUPER || speed == USB_SPEED_SUPER_PLUS) && uvc->desc.ss_streaming)
		return uvc->desc.ss_streaming;
	if (speed == USB_SPEED_HIGH && uvc->desc.hs_streaming)
		return uvc->desc.hs_streaming;
	if (speed == USB_SPEED_FULL && uvc->desc.fs_streaming)
		return uvc->desc.fs_streaming;

	if (uvc->desc.hs_streaming)
		return uvc->desc.hs_streaming;
	if (uvc->desc.ss_streaming)
		return uvc->desc.ss_streaming;
	if (uvc->desc.fs_streaming)
		return uvc->desc.fs_streaming;

	return NULL;
}

static u32 uvc_v4l2_sub_type_to_fcc(u8 subtype)
{
	switch (subtype) {
	case UVC_VS_FORMAT_UNCOMPRESSED:
		return V4L2_PIX_FMT_YUYV;
	case UVC_VS_FORMAT_MJPEG:
		return V4L2_PIX_FMT_MJPEG;
	case UVC_VS_FORMAT_FRAME_BASED:
		return V4L2_PIX_FMT_H264;
	default:
		return 0;
	}
}

static bool uvc_v4l2_is_format(u8 subtype)
{
	return subtype == UVC_VS_FORMAT_UNCOMPRESSED ||
	       subtype == UVC_VS_FORMAT_MJPEG ||
	       subtype == UVC_VS_FORMAT_FRAME_BASED;
}

static bool uvc_v4l2_is_frame(u8 subtype)
{
	return subtype == UVC_VS_FRAME_UNCOMPRESSED ||
	       subtype == UVC_VS_FRAME_MJPEG ||
	       subtype == UVC_VS_FRAME_FRAME_BASED;
}

static void uvc_v4l2_get_frame_info(const struct uvc_descriptor_header *h,
				    u16 *width, u16 *height,
				    u8 *interval_type, const __le32 **intervals)
{
	if (h->bDescriptorSubType == UVC_VS_FRAME_FRAME_BASED) {
		const struct uvc_frame_framebased *frm =
			(const struct uvc_frame_framebased *)h;
		*width = le16_to_cpu(frm->wWidth);
		*height = le16_to_cpu(frm->wHeight);
		*interval_type = frm->bFrameIntervalType;
		*intervals = (const __le32 *)frm->dwFrameInterval;
	} else {
		const struct uvc_frame_uncompressed *frm =
			(const struct uvc_frame_uncompressed *)h;
		*width = le16_to_cpu(frm->wWidth);
		*height = le16_to_cpu(frm->wHeight);
		*interval_type = frm->bFrameIntervalType;
		*intervals = frm->dwFrameInterval;
	}
}

static int
uvc_v4l2_enum_format(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	const struct uvc_descriptor_header * const *cls;
	unsigned int index = 0;

	if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	cls = uvc_v4l2_get_streaming_desc(uvc);
	if (cls) {
		for (; *cls; ++cls) {
			const struct uvc_descriptor_header *h = *cls;

			if (!uvc_v4l2_is_format(h->bDescriptorSubType))
				continue;

			if (index == f->index) {
				f->pixelformat = uvc_v4l2_sub_type_to_fcc(h->bDescriptorSubType);
				if (h->bDescriptorSubType == UVC_VS_FORMAT_UNCOMPRESSED) {
					strlcpy(f->description, "YUV 4:2:2 (YUYV)", sizeof(f->description));
					f->flags = 0;
				} else if (h->bDescriptorSubType == UVC_VS_FORMAT_MJPEG) {
					strlcpy(f->description, "Motion-JPEG", sizeof(f->description));
					f->flags = V4L2_FMT_FLAG_COMPRESSED;
				} else {
					strlcpy(f->description, "H.264", sizeof(f->description));
					f->flags = V4L2_FMT_FLAG_COMPRESSED;
				}
				return 0;
			}
			index++;
		}
		return -EINVAL;
	}

	/* Fallback if descriptors are not available */
	if (f->index >= ARRAY_SIZE(uvc_formats))
		return -EINVAL;

	f->pixelformat = uvc_formats[f->index].fcc;
	if (f->pixelformat == V4L2_PIX_FMT_MJPEG || f->pixelformat == V4L2_PIX_FMT_H264)
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
	else
		f->flags = 0;
	strlcpy(f->description, "Video Format", sizeof(f->description));
	return 0;
}

static int
uvc_v4l2_enum_framesizes(struct file *file, void *fh,
			 struct v4l2_frmsizeenum *fsize)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	const struct uvc_descriptor_header * const *cls;
	bool in_matching_format = false;
	unsigned int frame_index = 0;

	cls = uvc_v4l2_get_streaming_desc(uvc);
	if (cls) {
		for (; *cls; ++cls) {
			const struct uvc_descriptor_header *h = *cls;

			if (uvc_v4l2_is_format(h->bDescriptorSubType)) {
				if (in_matching_format)
					break;
				if (uvc_v4l2_sub_type_to_fcc(h->bDescriptorSubType) == fsize->pixel_format)
					in_matching_format = true;
				continue;
			}

			if (in_matching_format && uvc_v4l2_is_frame(h->bDescriptorSubType)) {
				if (frame_index == fsize->index) {
					u16 width, height;
					u8 interval_type;
					const __le32 *intervals;

					uvc_v4l2_get_frame_info(h, &width, &height,
								&interval_type, &intervals);
					fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
					fsize->discrete.width = width;
					fsize->discrete.height = height;
					return 0;
				}
				frame_index++;
			}
		}
		return -EINVAL;
	}

	/* Fallback if descriptors are not available */
	if (fsize->index == 0) {
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = 640;
		fsize->discrete.height = 360;
		return 0;
	} else if (fsize->index == 1) {
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = 1280;
		fsize->discrete.height = 720;
		return 0;
	} else if (fsize->index == 2 && fsize->pixel_format == V4L2_PIX_FMT_MJPEG) {
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = 1920;
		fsize->discrete.height = 1080;
		return 0;
	}
	return -EINVAL;
}

static int
uvc_v4l2_enum_frameintervals(struct file *file, void *fh,
			     struct v4l2_frmivalenum *fival)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	const struct uvc_descriptor_header * const *cls;
	bool in_matching_format = false;

	cls = uvc_v4l2_get_streaming_desc(uvc);
	if (cls) {
		for (; *cls; ++cls) {
			const struct uvc_descriptor_header *h = *cls;

			if (uvc_v4l2_is_format(h->bDescriptorSubType)) {
				if (in_matching_format)
					break;
				if (uvc_v4l2_sub_type_to_fcc(h->bDescriptorSubType) == fival->pixel_format)
					in_matching_format = true;
				continue;
			}

			if (in_matching_format && uvc_v4l2_is_frame(h->bDescriptorSubType)) {
				u16 width, height;
				u8 interval_type;
				const __le32 *intervals;

				uvc_v4l2_get_frame_info(h, &width, &height,
							&interval_type, &intervals);
				if (width == fival->width && height == fival->height) {
					if (interval_type == 0) {
						fival->type = V4L2_FRMIVAL_TYPE_STEPWISE;
						fival->stepwise.min.numerator = le32_to_cpu(intervals[0]);
						fival->stepwise.min.denominator = 10000000;
						fival->stepwise.max.numerator = le32_to_cpu(intervals[1]);
						fival->stepwise.max.denominator = 10000000;
						fival->stepwise.step.numerator = le32_to_cpu(intervals[2]);
						fival->stepwise.step.denominator = 10000000;
						return 0;
					}
					if (fival->index >= interval_type)
						return -EINVAL;
					fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
					fival->discrete.numerator = le32_to_cpu(intervals[fival->index]);
					fival->discrete.denominator = 10000000;
					return 0;
				}
			}
		}
		return -EINVAL;
	}

	/* Fallback */
	if (fival->index == 0) {
		fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		fival->discrete.numerator = 333333;
		fival->discrete.denominator = 10000000;
		return 0;
	} else if (fival->index == 1) {
		fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		fival->discrete.numerator = 666666;
		fival->discrete.denominator = 10000000;
		return 0;
	}
	return -EINVAL;
}

static int
uvc_v4l2_set_format(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_video *video = &uvc->video;
	struct uvc_format *format;
	unsigned int imagesize;
	unsigned int bpl;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(uvc_formats); ++i) {
		format = &uvc_formats[i];
		if (format->fcc == fmt->fmt.pix.pixelformat)
			break;
	}

	if (i == ARRAY_SIZE(uvc_formats)) {
		uvcg_info(&uvc->func, "Unsupported format 0x%08x.\n",
			  fmt->fmt.pix.pixelformat);
		return -EINVAL;
	}

	bpl = format->bpp * fmt->fmt.pix.width / 8;
	imagesize = bpl ? bpl * fmt->fmt.pix.height : fmt->fmt.pix.sizeimage;

	video->fcc = format->fcc;
	video->bpp = format->bpp;
	video->width = fmt->fmt.pix.width;
	video->height = fmt->fmt.pix.height;
	video->imagesize = imagesize;

	fmt->fmt.pix.field = V4L2_FIELD_NONE;
	fmt->fmt.pix.bytesperline = bpl;
	fmt->fmt.pix.sizeimage = imagesize;
	fmt->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	fmt->fmt.pix.priv = 0;

	return 0;
}

static int
uvc_v4l2_reqbufs(struct file *file, void *fh, struct v4l2_requestbuffers *b)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_video *video = &uvc->video;

	if (b->type != video->queue.queue.type)
		return -EINVAL;

	return uvcg_alloc_buffers(&video->queue, b);
}

static int
uvc_v4l2_querybuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_video *video = &uvc->video;

	return uvcg_query_buffer(&video->queue, b);
}

static int
uvc_v4l2_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_video *video = &uvc->video;
	int ret;

	ret = uvcg_queue_buffer(&video->queue, b);
	if (ret < 0)
		return ret;

	return uvcg_video_pump(video);
}

static int
uvc_v4l2_dqbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_video *video = &uvc->video;

	return uvcg_dequeue_buffer(&video->queue, b, file->f_flags & O_NONBLOCK);
}

static int
uvc_v4l2_streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_video *video = &uvc->video;
	int ret;

	if (type != video->queue.queue.type)
		return -EINVAL;

	/* Enable UVC video. */
	ret = uvcg_video_enable(video, 1);
	if (ret < 0)
		return ret;

	/*
	 * Complete the alternate setting selection setup phase now that
	 * userspace is ready to provide video frames.
	 */
	uvc_function_setup_continue(uvc);
	uvc->state = UVC_STATE_STREAMING;

	return 0;
}

static int
uvc_v4l2_streamoff(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_video *video = &uvc->video;

	if (type != video->queue.queue.type)
		return -EINVAL;

	return uvcg_video_enable(video, 0);
}

static int
uvc_v4l2_subscribe_event(struct v4l2_fh *fh,
			 const struct v4l2_event_subscription *sub)
{
	if (sub->type < UVC_EVENT_FIRST || sub->type > UVC_EVENT_LAST)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 2, NULL);
}

static int
uvc_v4l2_unsubscribe_event(struct v4l2_fh *fh,
			   const struct v4l2_event_subscription *sub)
{
	return v4l2_event_unsubscribe(fh, sub);
}

static long
uvc_v4l2_ioctl_default(struct file *file, void *fh, bool valid_prio,
		       unsigned int cmd, void *arg)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);

	switch (cmd) {
	case UVCIOC_SEND_RESPONSE:
		return uvc_send_response(uvc, arg);

	default:
		return -ENOIOCTLCMD;
	}
}

const struct v4l2_ioctl_ops uvc_v4l2_ioctl_ops = {
	.vidioc_querycap = uvc_v4l2_querycap,
	.vidioc_g_fmt_vid_out = uvc_v4l2_get_format,
	.vidioc_s_fmt_vid_out = uvc_v4l2_set_format,
	.vidioc_enum_fmt_vid_out = uvc_v4l2_enum_format,
	.vidioc_enum_framesizes = uvc_v4l2_enum_framesizes,
	.vidioc_enum_frameintervals = uvc_v4l2_enum_frameintervals,
	.vidioc_reqbufs = uvc_v4l2_reqbufs,
	.vidioc_querybuf = uvc_v4l2_querybuf,
	.vidioc_qbuf = uvc_v4l2_qbuf,
	.vidioc_dqbuf = uvc_v4l2_dqbuf,
	.vidioc_streamon = uvc_v4l2_streamon,
	.vidioc_streamoff = uvc_v4l2_streamoff,
	.vidioc_subscribe_event = uvc_v4l2_subscribe_event,
	.vidioc_unsubscribe_event = uvc_v4l2_unsubscribe_event,
	.vidioc_default = uvc_v4l2_ioctl_default,
};

/* --------------------------------------------------------------------------
 * V4L2
 */

static int
uvc_v4l2_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_file_handle *handle;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (handle == NULL)
		return -ENOMEM;

	v4l2_fh_init(&handle->vfh, vdev);
	v4l2_fh_add(&handle->vfh);

	handle->device = &uvc->video;
	file->private_data = &handle->vfh;

	uvc_function_connect(uvc);
	return 0;
}

static int
uvc_v4l2_release(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);
	struct uvc_file_handle *handle = to_uvc_file_handle(file->private_data);
	struct uvc_video *video = handle->device;

	uvc_function_disconnect(uvc);

	mutex_lock(&video->mutex);
	uvcg_video_enable(video, 0);
	uvcg_free_buffers(&video->queue);
	mutex_unlock(&video->mutex);

	file->private_data = NULL;
	v4l2_fh_del(&handle->vfh);
	v4l2_fh_exit(&handle->vfh);
	kfree(handle);

	return 0;
}

static int
uvc_v4l2_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);

	return uvcg_queue_mmap(&uvc->video.queue, vma);
}

static __poll_t
uvc_v4l2_poll(struct file *file, poll_table *wait)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);

	return uvcg_queue_poll(&uvc->video.queue, file, wait);
}

#ifndef CONFIG_MMU
static unsigned long uvcg_v4l2_get_unmapped_area(struct file *file,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long flags)
{
	struct video_device *vdev = video_devdata(file);
	struct uvc_device *uvc = video_get_drvdata(vdev);

	return uvcg_queue_get_unmapped_area(&uvc->video.queue, pgoff);
}
#endif

const struct v4l2_file_operations uvc_v4l2_fops = {
	.owner		= THIS_MODULE,
	.open		= uvc_v4l2_open,
	.release	= uvc_v4l2_release,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= uvc_v4l2_mmap,
	.poll		= uvc_v4l2_poll,
#ifndef CONFIG_MMU
	.get_unmapped_area = uvcg_v4l2_get_unmapped_area,
#endif
};

