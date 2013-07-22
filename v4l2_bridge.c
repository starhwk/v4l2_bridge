/*
 * Bridge application which transfers buffers between V4L2 pipelines
 *
 * Copyright (C) 2013 Xilinx, Inc. All rights reserved.
 * Author: hyun woo kwon <hyunk@xilinx.com>
 *
 * Description:
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>

/*
 * OVERALL STRUCTURES
 *
 * manager	-> stream	-> buffers
 *				-> common config
 *				-> device(in)
 *				-> device(out)
 *		-> streams,,,
 *
 */

/* video device */
struct device {
	char devname[32];		/* device name */
	int fd;				/* device node fd */
	unsigned int type;		/* device type */

	unsigned int buf_type;		/* type of buffer */
	unsigned int mem_type;		/* type of memory */

	bool export;			/* flag to export using dmabuf */
};

/* common config for stream */
struct config {
	unsigned int fourcc;		/* fourcc */
	struct v4l2_pix_format format;	/* v4l2 pixel format */
	unsigned int num_buffers;	/* num of buffers */
	int fps;			/* fps */
	unsigned int frame_us;		/* us per frame(1 sec / fps) */
};

/* buffer */
struct buffer {
	unsigned int index;		/* buffer index */
	int dbuf_fd;			/* dmabuf fd */
};

/* manager stream between 2 pipelines */
struct stream {
	struct device in;		/* input device */
	struct device out;		/* output device */
	struct buffer *buffers;		/* buffers */
	struct config config;		/* common config */
	pthread_t thread;		/* thread */
};

/* bridge stream  manager */
struct manager {
	struct stream *streams;		/* streams */
	int num_streams;		/* number of streams */
};

#define ERRSTR strerror(errno)

#define ASSERT(cond, ...) 					\
	do {							\
		if (cond) {					\
			int errsv = errno;			\
			fprintf(stderr, "ERROR(%s:%d) : ",	\
					__FILE__, __LINE__);	\
			errno = errsv;				\
			fprintf(stderr,  __VA_ARGS__);		\
			abort();				\
		}						\
	} while(0)

static inline int warn(const char *file, int line, const char *fmt, ...)
{
	int errsv = errno;
	va_list va;
	va_start(va, fmt);
	fprintf(stderr, "WARN(%s:%d): ", file, line);
	vfprintf(stderr, fmt, va);
	va_end(va);
	errno = errsv;
	return 1;
}

#define WARN_ON(cond, ...) \
	((cond) ? warn(__FILE__, __LINE__, __VA_ARGS__) : 0)

static void usage(char *name)
{
#define HELP(...) fprintf(stderr, __VA_ARGS__);
	HELP("usage: %s [-nh]\n", name);

	HELP(" -n\tnumber of streams\t<stream count>\n");
	HELP(" -S\tstream config\t\t<in:out@expdev@fps:num_buf:w,h:fourcc>\n");
	HELP(" \t\t\t\tin = input video device node\n");
	HELP(" \t\t\t\tout = output video device node\n");
	HELP(" \t\t\t\texpdev = device to export(i or o)\n");
	HELP(" \t\t\t\tfps = fps using usleep(-1 for free run)\n");
	HELP(" \t\t\t\tnum_buf = number of buffer\n");
	HELP(" \t\t\t\tw,h = width,height\n");
	HELP(" \t\t\t\tfourcc = pixel format fourcc\n");
	HELP(" \t\t\t\t(ex, /dev/video0:/dev/video1@o@5:4:640,480:YUYV)\n");
	HELP(" -h\tshow this help\n");
#undef HELP
}

/*
 * video device operations
 */

/* queue buffer */
static void device_queue_buffer(struct device *d, struct buffer *b)
{
	struct v4l2_buffer vb;
	int ret;

	memset(&vb, 0, sizeof vb);
	vb.type = d->buf_type;
	vb.memory = d->mem_type;
	vb.index = b->index;
	vb.m.fd = b->dbuf_fd;

	ret = ioctl(d->fd, VIDIOC_QBUF, &vb);
	ASSERT(ret, "VIDIOC_QBUF(index = %d) failed: %s\n", b->index, ERRSTR);
}

/* dequeue buffer */
static struct buffer *device_dequeue_buffer(struct device *d, struct buffer *bs)
{
	struct v4l2_buffer vb;
	int ret;

	memset(&vb, 0, sizeof vb);

	vb.type = d->type;
	vb.memory = d->mem_type;
	ret = ioctl(d->fd, VIDIOC_DQBUF, &vb);
	ASSERT(ret, "VIDIOC_DQBUF failed: %s\n", ERRSTR);

	return &bs[vb.index];
}

/* prepare buffer */
static void device_prepare_buffer(struct device *d, struct buffer *b)
{
	struct v4l2_exportbuffer eb;
	int res;

	/* export buffer */
	if (d->export) {
		memset(&eb, 0, sizeof(eb));
		eb.type = d->buf_type;;
		res = ioctl(d->fd, VIDIOC_EXPBUF, &eb);
		ASSERT(res < 0, "VIDIOC_EXPBUF failed: %s\n", ERRSTR);
		b->dbuf_fd = eb.fd;
	}

	return;
}

/* turn off video device */
static void device_off(struct device *d)
{
	int res;
	res = ioctl(d->fd, VIDIOC_STREAMOFF, &d->buf_type);
	ASSERT(res < 0, "STREAMOFF failed: %s\n", ERRSTR);
	return;
}

/* turn on video device */
static void device_on(struct device *d)
{
	int res;
	res = ioctl(d->fd, VIDIOC_STREAMON, &d->buf_type);
	ASSERT(res < 0, "STREAMON failed: %s\n", ERRSTR);
	return;
}

/* initialize device */
static void device_init(struct device *d, struct config *c, unsigned int type)
{
	struct v4l2_capability caps;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers rqbufs;
	int ret;

	d->fd = open(d->devname, O_RDWR);
	ASSERT(d->fd < 0, "failed to open %s: %s\n", d->devname, ERRSTR);

	/* query caps */
	memset(&caps, 0, sizeof caps);

	ret = ioctl(d->fd, VIDIOC_QUERYCAP, &caps);
	ASSERT(ret, "VIDIOC_QUERYCAP failed: %s\n", ERRSTR);

	ASSERT(~caps.capabilities & type,
		"video: output or capture is not supported(%d, %d)\n",
		caps.capabilities, type);
	d->type = type;
	d->buf_type = (d->type == V4L2_CAP_VIDEO_CAPTURE) ?
		V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
	d->mem_type = d->export ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = d->buf_type;

	/* set format(g_fmt->s_fmt->g_fmt) */
	ret = ioctl(d->fd, VIDIOC_G_FMT, &fmt);
	ASSERT(ret < 0, "VIDIOC_G_FMT failed: %s\n", ERRSTR);
	printf("G_FMT(start): width = %u, height = %u, 4cc = %.4s\n",
		fmt.fmt.pix.width, fmt.fmt.pix.height,
		(char*)&fmt.fmt.pix.pixelformat);

	c->format.pixelformat = c->fourcc;
	fmt.fmt.pix = c->format;

	ret = ioctl(d->fd, VIDIOC_S_FMT, &fmt);
	ASSERT(ret < 0, "VIDIOC_S_FMT failed: %s\n", ERRSTR);

	ret = ioctl(d->fd, VIDIOC_G_FMT, &fmt);
	ASSERT(ret < 0, "VIDIOC_G_FMT failed: %s\n", ERRSTR);
	printf("G_FMT(final): width = %u, height = %u, 4cc = %.4s\n",
		fmt.fmt.pix.width, fmt.fmt.pix.height,
		(char*)&fmt.fmt.pix.pixelformat);

	/* request buffers */
	memset(&rqbufs, 0, sizeof(rqbufs));
	rqbufs.count = c->num_buffers;
	rqbufs.type = d->buf_type;
	rqbufs.memory = d->mem_type;

	ret = ioctl(d->fd, VIDIOC_REQBUFS, &rqbufs);
	ASSERT(ret < 0, "VIDIOC_REQBUFS failed: %s\n", ERRSTR);
	ASSERT(rqbufs.count < c->num_buffers, "video node allocated only "
		"%u of %u buffers\n", rqbufs.count, c->num_buffers);

	c->format = fmt.fmt.pix;

	return;
}

/*
 * stream operations
 */

/* dump stream config */
static void stream_dump_config(struct stream *s)
{
	char fourcc[5];
#define DUMP(...) fprintf(stderr, __VA_ARGS__);
	DUMP("input device name:%s(exp: %d)\n", s->in.devname, s->in.export);
	DUMP("output device name:%s(exp: %d)\n", s->out.devname, s->out.export);
	DUMP("width: %d\n", s->config.format.width);
	DUMP("height: %d\n", s->config.format.height);
	DUMP("buffer count:%d\n", s->config.num_buffers);
	DUMP("fps:%d\n", s->config.fps);
	fourcc[0] = (char)(s->config.fourcc);
	fourcc[1] = (char)(s->config.fourcc >> 8);
	fourcc[2] = (char)(s->config.fourcc >> 16);
	fourcc[3] = (char)(s->config.fourcc >> 24);
	fourcc[4] = '\0';
	DUMP("fourcc %s\n", fourcc);
#undef DUMP
}

#define min(a, b)		((a) < (b) ? (a):(b))

#define NEXT_ARG(s, e, x)		\
	do {				\
		e = strchr(s, x);	\
		if (!e) {		\
			ret = -1;	\
			goto err_out;	\
		}			\
	} while(0);

/* parse stream args */
/* ex: in_dev:out_dev@device_to_exp(o/i)@fps:num_buf:width,height:fourcc */
static int stream_parse_args(struct stream *s, const char *arg)
{
	const char *startp;
	char *endp;
	unsigned int len;
	int ret;

	/* input device name */
	startp = arg;
	NEXT_ARG(startp, endp, ':');
	len = min(sizeof(s->in) - 1, endp - startp);
	strncpy(s->in.devname, startp, len);
	s->in.devname[len] = '\0';

	/* output device name */
	startp = endp + 1;
	NEXT_ARG(startp, endp, '@');
	len = min(sizeof(s->out) - 1, endp - startp);
	strncpy(s->out.devname, startp, len);
	s->out.devname[len] = '\0';

	/* device to export */
	startp = endp + 1;
	NEXT_ARG(startp, endp, '@');
	if (*startp == 'o') {
		s->out.export = true;
		s->in.export = false;
	} else if (*startp == 'i') {
		s->out.export = false;
		s->in.export = true;
	} else {
		ret = -1;
		goto err_out;
	}

	/* fps */
	startp = endp + 1;
	NEXT_ARG(startp, endp, ':');
	s->config.fps = strtoul(startp, &endp, 10);

	/* num of buffers */
	startp = endp + 1;
	NEXT_ARG(startp, endp, ':');
	s->config.num_buffers = strtoul(startp, &endp, 10);

	/* size(width, height) */
	startp = endp + 1;
	NEXT_ARG(startp, endp, ':');
	ret = sscanf(startp, "%u,%u",
			&s->config.format.width, &s->config.format.height);
	if (ret < 0)
		goto err_out;

	/* fourcc */
	startp = endp + 1;
	s->config.fourcc = ((unsigned)startp[0] << 0) |
		((unsigned)startp[1] << 8) |
		((unsigned)startp[2] << 16) |
		((unsigned)startp[3] << 24);

	return 0;

err_out:
	return ret;
}

/* turn off stream */
static void stream_off(void *data)
{
	struct stream *s = data;
	/* turn off devices */
	device_off(&s->in);
	device_off(&s->out);
	return;
}

/* turn on stream */
static void *stream_on(void *data)
{
	struct stream *s = data;
	struct buffer *b;
	struct pollfd fds[] = {
		{.fd = s->in.fd, .events = POLLIN},
		{.fd = s->out.fd, .events = POLLOUT},
	};
	struct timeval now;
	unsigned int curr = 0;
	unsigned int prev = 0;
	unsigned int delay = 0;
	int res;

	/* push cleanup handler */
	pthread_cleanup_push(stream_off, s);

	/* turn on devices */
	device_on(&s->in);
	device_on(&s->out);

	/* poll and pass buffers */
	while ((res = poll(fds, 2, 5000)) > 0) {

		if (fds[0].revents & POLLIN) {
			/* sleep for specified fps if needed */
			if (s->config.frame_us > 0) {
				gettimeofday(&now, NULL);
				curr = now.tv_sec * 1000000 + now.tv_usec;
				delay = curr - prev;
				if (delay < s->config.frame_us) {
					usleep((s->config.frame_us - delay));
				}
				gettimeofday(&now, NULL);
				prev = now.tv_sec * 1000000 + now.tv_usec;
			}

			b = device_dequeue_buffer(&s->in, s->buffers);
			device_queue_buffer(&s->out, b);
		}

		if (fds[1].revents & POLLOUT) {
			b = device_dequeue_buffer(&s->out, s->buffers);
			device_queue_buffer(&s->in, b);
		}
	}

	/* pop cleanup handler */
	pthread_cleanup_pop(s);

	return NULL;
}

/* initialize stream */
static void stream_init(struct stream *s)
{
	struct buffer *b;
	int i;

	/* initialize devices */
	device_init(&s->in, &s->config, V4L2_CAP_VIDEO_CAPTURE);
	device_init(&s->out, &s->config, V4L2_CAP_VIDEO_OUTPUT);

	s->buffers = calloc(sizeof(*b), s->config.num_buffers);
	for (i = 0; i < s->config.num_buffers; i++) {
		s->buffers[i].index = i;
		/* prepare/export buffer */
		device_prepare_buffer(&s->in, &s->buffers[i]);
		device_prepare_buffer(&s->out, &s->buffers[i]);
	}

	for (i = 0; i < s->config.num_buffers; i++) {
		/* queue buffer to input */
		device_queue_buffer(&s->in, &s->buffers[i]);
	}

	if (s->config.fps > 0)
		s->config.frame_us = (10000000 / s->config.fps) / 10;
	else
		s->config.frame_us = -1;

	return;
}

/*
 * stream manager operations
 */

/* parse args */
static int manager_parse_args(struct manager *m, int argc, char *argv[])
{
	int c;
	int idx = 0;;
	int ret;

	if (argc <= 1) {
		usage(argv[0]);
		ret = -1;
		goto err_out;
	}

	while ((c = getopt(argc, argv, "hn:S:")) != -1) {
		switch (c) {
		case 'h':
			usage(argv[0]);
			ret = -1;
			goto err_out;
		case 'n':
			ret = sscanf(optarg, "%u", &m->num_streams);
			if (WARN_ON(ret != 1, "incorrect stream count\n"))
				goto err_out;
			m->streams =
				malloc(sizeof(*m->streams) * m->num_streams);
			break;
		case 'S':
			ret = stream_parse_args(&m->streams[idx], optarg);
			if (WARN_ON(ret < 0, "invalid stream args\n")) {
				stream_dump_config(&m->streams[idx]);
				goto err_out;
			}
			if (WARN_ON(++idx > m->num_streams, "num streams\n")) {
				goto err_out;
			}
			break;
		default:
			usage(argv[0]);
			ret = -1;
			goto err_out;
		}
	}

	return 0;

err_out:
	return ret;
}

/* turn on manager */
static void manager_on(struct manager *m)
{
	int i;
	for (i = 0; i < m->num_streams; i++) {
		/* create a thread for each stream */
		pthread_create(&m->streams[i].thread, NULL, stream_on,
				&m->streams[i]);
	}
	return;
}

/* turn off manager */
static void manager_off(struct manager *m)
{
	int i;
	for (i = 0; i < m->num_streams; i++) {
		/* cancel a stream thread */
		if (m->streams[i].thread)
			pthread_cancel(m->streams[i].thread);
	}
	return;
}

/* exit manager */
static void manager_exit(struct manager *m)
{
	int i;
	/* wait for threads to terminate */
	for (i = 0; i < m->num_streams; i++) {
		pthread_join(m->streams[i].thread, NULL);
	}
	return;
}

/* initialize manager */
static void manager_init(struct manager *m)
{
	int i;
	for (i = 0; i < m->num_streams; i++) {
		stream_init(&m->streams[i]);
	}
	return;
}

/*
 * main
 */

static struct manager *gb;

static void sigint_action(int sig, siginfo_t *siginfo, void *data)
{
	manager_off(gb);
	return;
}

int main(int argc, char *argv[])
{
	struct manager *m;
	struct sigaction sa;
	int ret;

	m = malloc(sizeof(*m));
	ret = manager_parse_args(m, argc, argv);
	ASSERT(ret, "failed to parse arguments\n");

	/* set up signal handler for sigint */
	gb = m;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = sigint_action;
	sigaction(SIGINT, &sa, NULL);

	manager_init(m);
	manager_on(m);
	manager_exit(m);

	return 0;
}
