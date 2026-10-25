/*
 * The media extension: audio and video through FFmpeg, sound through
 * ALSA.
 *
 * Each player has a decoding thread (demux, decode, convert frames to
 * RGBA and sound to 16-bit stereo) and, with sound, an output thread
 * feeding ALSA. Bytes come from the host's request function by ranges
 * of 1 MB, so a long video is never all in memory; a server without
 * ranges gives the whole file. The clock is the sound card's when
 * there is sound, the system's otherwise; the host's thread takes
 * the frame due at that time.
 *
 * A player can also be fed by a script (Media Source Extensions):
 * each SourceBuffer has a parsing thread that demuxes what is appended
 * into coded frames, kept in memory by time like browsers do; the
 * decoding thread takes them in decode order from the video and audio
 * tracks, and the clock waits where nothing is buffered.
 *
 * SOIE_AUDIO=none plays without sound (tests, machines without a
 * sound card), SOIE_AUDIO=device picks the ALSA device ("default").
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include <soie/ext.h>

#define CHUNK (1 << 20)		/* bytes per range request */
#define NFRAMES 4		/* decoded frames waiting */
#define MAX_PIXELS (1280 * 720)	/* frames are scaled down to this */
#define PCM_FRAMES 32768	/* sound waiting, in stereo frames */
/* Media Source Extensions */
#define MAX_BUFFERS 4		/* SourceBuffers per player */
#define VIDEO_QUOTA (24 << 20)	/* coded bytes kept per SourceBuffer */
#define AUDIO_QUOTA (4 << 20)
#define INIT_MAX (1 << 20)	/* an initialization segment kept */
#define MAX_RANGES 64
#define GAP 150000		/* us: frames closer are one range */
#define KEEP 3000000		/* us before the position never evicted */

/* Bytes, by ranges */
struct source {
	soie_request_fn request;
	void *rdata;
	char *url;
	unsigned char *buf;	/* the current chunk, or the whole file */
	int64_t buf_pos;
	size_t buf_len;
	int whole;		/* buf is the whole file */
	int64_t size;		/* -1: unknown */
	int64_t pos;
};

struct frame {
	unsigned char *rgba;
	int w, h;
	double pts;
};

/* Media Source Extensions: a track's codec, from an initialization
 * segment, shared by its coded frames */
struct init {
	int refs;
	AVCodecParameters *par;
};

struct cframe {
	AVPacket *pkt;		/* times in microseconds, offset added */
	int64_t pts, end;	/* what it shows: [pts, end) */
	struct init *init;
	unsigned run;		/* the append it came with */
};

struct track {
	struct cframe *f;	/* in decode order */
	int n, cap;
	size_t bytes;
	int64_t ranges[2 * MAX_RANGES];	/* buffered [start, end) pairs */
	int nranges;
	int64_t oldmax;		/* end of the frames of earlier appends */
	unsigned oldrun;
	int64_t cur;		/* the decoder's: next frame has dts > cur */
	int seek;		/* waits for the frames at the seek's time */
};

struct player;

/* A SourceBuffer */
struct sbuf {
	struct player *p;
	int used;
	AVInputFormat *ifmt;
	pthread_t thread;
	/* bytes appended, not parsed yet */
	unsigned char *fifo;
	size_t len, off, cap;
	unsigned char *initseg;	/* the last initialization segment */
	size_t initlen, replay;	/* replay: its bytes to read first */
	int idle;		/* the parser waits for bytes */
	int consumed;		/* the demuxer read bytes since it opened */
	int reset;		/* a new initialization segment: reopen */
	int error, quit;
	int64_t offset, wstart, wend;	/* us */
	unsigned run;
	int stream[2];		/* the demuxer's video and audio streams */
	struct init *init[2];
	struct track t[2];	/* video, audio */
	size_t quota;
};

struct player {
	struct source src;
	pthread_t thread, athread;
	int has_athread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	/* shared state, under lock */
	int playing, quit, ended, error, eof, opened;
	int seek_req;
	double seek_to;
	double duration;
	int width, height;
	int changes;
	double base;		/* position when the clock started */
	double started;		/* monotonic time it started, or 0 */
	struct frame ring[NFRAMES];
	int head, count;
	unsigned char *shown;	/* the frame on screen */
	int shown_w, shown_h;
	double shown_pts;
	int quarter;		/* time in quarters, for repaints */
	int seeks, decoded, dropped;
	double skip;		/* frames ending before are not shown */
	/* sound */
	int16_t *pcm;
	int pcm_head, pcm_count, rate;
	int flush;		/* the output drops what it has */
	int64_t played;		/* frames given to ALSA since base */
	double audio_clock;	/* -1 while the sound has not started */
	double audio_at;	/* the monotonic time it was read */
	double volume;
	int muted;
	snd_pcm_t *dev;
	/* the decoding thread's own */
	AVFormatContext *fmt;
	AVCodecContext *vdec, *adec;
	int vs, as;
	struct SwsContext *sws;
	SwrContext *swr;
	AVRational vtb, atb;	/* the decoders' time bases */
	/* fed by a script */
	int source;
	struct sbuf sb[MAX_BUFFERS];
	int eos, waiting;
	int64_t seek_us;
	int64_t ranges[2 * MAX_RANGES];	/* what plays, all tracks */
	int nranges;
	struct init *vinit, *ainit;	/* the decoders' */
};

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Sources */

static int fetch_range(struct source *s, int64_t pos)
{
	soie_request rq;
	soie_response res;
	char headers[96], *cr;

	snprintf(headers, sizeof(headers), "Range: bytes=%lld-%lld\r\n",
		(long long)pos, (long long)(pos + CHUNK - 1));
	memset(&rq, 0, sizeof(rq));
	rq.url = s->url;
	rq.headers = headers;
	if (s->request(s->rdata, &rq, &res))
		return -1;
	if (res.status == 206) {
		long long from = -1;

		/* the range must start where we asked */
		cr = soie_response_header(&res, "content-range");
		if (!cr || sscanf(cr, "bytes %lld-", &from) != 1 ||
		    from != pos) {
			free(cr);
			soie_response_free(&res);
			return -1;
		}
		if (strchr(cr, '/') && strchr(cr, '/')[1] != '*')
			s->size = atoll(strchr(cr, '/') + 1);
		free(cr);
		free(s->buf);
		s->buf = (unsigned char *)res.body;
		s->buf_pos = pos;
		s->buf_len = res.len;
		res.body = NULL;
	} else if (res.status == 200) {
		/* no ranges: the whole file */
		free(s->buf);
		s->buf = (unsigned char *)res.body;
		s->buf_pos = 0;
		s->buf_len = res.len;
		s->size = (int64_t)res.len;
		s->whole = 1;
		res.body = NULL;
	} else {
		soie_response_free(&res);
		return -1;
	}
	soie_response_free(&res);
	return 0;
}

static int src_read(void *opaque, uint8_t *out, int size)
{
	struct source *s = opaque;
	size_t off, n;

	if (s->size >= 0 && s->pos >= s->size)
		return AVERROR_EOF;
	if (!s->buf || s->pos < s->buf_pos ||
	    s->pos >= s->buf_pos + (int64_t)s->buf_len) {
		if (s->whole)
			return AVERROR_EOF;
		if (fetch_range(s, s->pos) || !s->buf_len)
			return AVERROR_EOF;
		if (s->pos < s->buf_pos ||
		    s->pos >= s->buf_pos + (int64_t)s->buf_len)
			return AVERROR_EOF;
	}
	off = (size_t)(s->pos - s->buf_pos);
	n = s->buf_len - off;
	if (n > (size_t)size)
		n = (size_t)size;
	memcpy(out, s->buf + off, n);
	s->pos += (int64_t)n;
	return (int)n;
}

static int64_t src_seek(void *opaque, int64_t offset, int whence)
{
	struct source *s = opaque;

	switch (whence & ~AVSEEK_FORCE) {
	case AVSEEK_SIZE:
		return s->size;
	case SEEK_SET:
		s->pos = offset;
		break;
	case SEEK_CUR:
		s->pos += offset;
		break;
	case SEEK_END:
		if (s->size < 0)
			return -1;
		s->pos = s->size + offset;
		break;
	default:
		return -1;
	}
	return s->pos;
}

/* The clock, under lock */

static double clock_now(struct player *p)
{
	if (!p->started)
		return p->base;
	if (p->audio_clock >= 0) {
		/* read at each write to the card (every 80 ms or so):
		 * the system's clock goes on in between, a little */
		double d = now() - p->audio_at;

		return p->audio_clock + (d < 0 ? 0 : d > 0.2 ? 0.2 : d);
	}
	return p->base + now() - p->started;
}

static void wait_or_go(struct player *p);

/* Take the frame due now; count changes for repaints. */
static void advance(struct player *p)
{
	double t;
	struct frame *f;
	unsigned char *swap;
	int q, sw, sh, taken = 0;

	if (p->source)
		wait_or_go(p);
	t = clock_now(p);
	while (p->count) {
		f = &p->ring[p->head];
		if (f->pts > t + 0.01 && p->shown)
			break;
		/* the next one is due too: this one is late, skip it */
		if (taken++)
			p->dropped++;
		swap = p->shown;
		sw = p->shown_w;
		sh = p->shown_h;
		p->shown = f->rgba;
		p->shown_w = f->w;
		p->shown_h = f->h;
		p->shown_pts = f->pts;
		f->rgba = swap;
		f->w = sw;
		f->h = sh;
		p->head = (p->head + 1) % NFRAMES;
		p->count--;
		p->changes++;
		pthread_cond_broadcast(&p->cond);
	}
	if (p->shown && (p->shown_w != p->width || p->shown_h != p->height)) {
		p->width = p->shown_w;	/* a stream of another size */
		p->height = p->shown_h;
		p->changes++;
	}
	q = (int)(t * 4);
	if (q != p->quarter) {
		p->quarter = q;
		p->changes++;
	}
	/* the end: everything decoded and shown, the sound played */
	if (p->eof && !p->count && !p->pcm_count && p->playing &&
	    (p->duration <= 0 || t >= p->duration - 0.05)) {
		p->base = p->duration > 0 ? p->duration : t;
		p->started = 0;
		p->playing = 0;
		p->ended = 1;
		p->changes++;
	}
}

/* The decoding thread */

/* The size frames are shown at: at most MAX_PIXELS, even */
static void fit(int *w, int *h)
{
	double scale;

	if (*w > 0 && *h > 0 && (long long)*w * *h > MAX_PIXELS) {
		scale = sqrt((double)MAX_PIXELS / ((double)*w * *h));
		*w = (int)(*w * scale) & ~1;
		*h = (int)(*h * scale) & ~1;
	}
	if (*w <= 0 || *h <= 0)
		*w = *h = 0;
}

static void push_video(struct player *p, AVFrame *fr)
{
	int64_t ts = fr->best_effort_timestamp;
	double pts = ts == AV_NOPTS_VALUE ? 0 : (double)ts * av_q2d(p->vtb);
	struct frame *f;
	uint8_t *dst[4];
	int linesize[4], w = fr->width, h = fr->height;

	/* what a seek lands past is not shown */
	if (ts != AV_NOPTS_VALUE && fr->pkt_duration > 0 &&
	    pts + (double)fr->pkt_duration * av_q2d(p->vtb) <= p->skip)
		return;
	fit(&w, &h);
	if (!w)
		return;
	p->sws = sws_getCachedContext(p->sws, fr->width, fr->height,
		fr->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL,
		NULL);
	if (!p->sws)
		return;
	pthread_mutex_lock(&p->lock);
	while (p->count == NFRAMES && !p->quit && !p->seek_req)
		pthread_cond_wait(&p->cond, &p->lock);
	if (p->quit || p->seek_req) {
		pthread_mutex_unlock(&p->lock);
		return;
	}
	f = &p->ring[(p->head + p->count) % NFRAMES];
	pthread_mutex_unlock(&p->lock);
	if (f->rgba && (f->w != w || f->h != h)) {
		free(f->rgba);
		f->rgba = NULL;
	}
	if (!f->rgba)
		f->rgba = malloc((size_t)w * (size_t)h * 4);
	if (!f->rgba)
		return;
	f->w = w;
	f->h = h;
	dst[0] = f->rgba;
	dst[1] = dst[2] = dst[3] = NULL;
	linesize[0] = w * 4;
	linesize[1] = linesize[2] = linesize[3] = 0;
	sws_scale(p->sws, (const uint8_t *const *)fr->data, fr->linesize, 0,
		fr->height, dst, linesize);
	f->pts = pts;
	pthread_mutex_lock(&p->lock);
	p->count++;
	p->decoded++;
	pthread_mutex_unlock(&p->lock);
}

static void push_audio(struct player *p, AVFrame *fr)
{
	int16_t tmp[2 * 4096];
	uint8_t *out = (uint8_t *)tmp;
	int64_t ts = fr->best_effort_timestamp;
	int n, i, k;

	if (ts != AV_NOPTS_VALUE && fr->pkt_duration > 0 &&
	    (double)(ts + fr->pkt_duration) * av_q2d(p->atb) <= p->skip)
		return;
	n = swr_convert(p->swr, &out, 4096,
		(const uint8_t **)fr->extended_data, fr->nb_samples);
	while (n > 0) {
		pthread_mutex_lock(&p->lock);
		for (i = 0; i < n; ) {
			while (p->pcm_count == PCM_FRAMES && !p->quit &&
			       !p->seek_req)
				pthread_cond_wait(&p->cond, &p->lock);
			if (p->quit || p->seek_req)
				break;
			for (; i < n && p->pcm_count < PCM_FRAMES; i++) {
				k = (p->pcm_head + p->pcm_count) % PCM_FRAMES;
				p->pcm[2 * k] = tmp[2 * i];
				p->pcm[2 * k + 1] = tmp[2 * i + 1];
				p->pcm_count++;
			}
			pthread_cond_broadcast(&p->cond);
		}
		pthread_mutex_unlock(&p->lock);
		if (i < n)
			return;
		/* what the resampler kept back */
		n = swr_convert(p->swr, &out, 4096, NULL, 0);
	}
}

static void drain(struct player *p, AVCodecContext *dec, AVFrame *fr,
	int video)
{
	while (avcodec_receive_frame(dec, fr) == 0) {
		if (video)
			push_video(p, fr);
		else
			push_audio(p, fr);
		av_frame_unref(fr);
	}
}

static AVCodecContext *open_codec(const AVCodecParameters *par,
	AVRational tb)
{
	AVCodec *codec = avcodec_find_decoder(par->codec_id);
	AVCodecContext *c;

	if (!codec || !(c = avcodec_alloc_context3(codec)))
		return NULL;
	c->thread_count = 1;
	c->pkt_timebase = tb;
	if (avcodec_parameters_to_context(c, par) < 0 ||
	    avcodec_open2(c, codec, NULL) < 0) {
		avcodec_free_context(&c);
		return NULL;
	}
	return c;
}

static AVCodecContext *open_decoder(AVFormatContext *fmt, int i)
{
	return open_codec(fmt->streams[i]->codecpar, fmt->streams[i]->time_base);
}

/* Sound conversion to 16-bit stereo at p->rate (the first stream's) */
static int open_resampler(struct player *p)
{
	int64_t layout = p->adec->channel_layout ?
		(int64_t)p->adec->channel_layout :
		av_get_default_channel_layout(p->adec->channels);

	if (!p->rate)
		p->rate = p->adec->sample_rate > 0 &&
			p->adec->sample_rate <= 96000 ? p->adec->sample_rate :
			48000;
	swr_free(&p->swr);
	p->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO,
		AV_SAMPLE_FMT_S16, p->rate, layout, p->adec->sample_fmt,
		p->adec->sample_rate, 0, NULL);
	if (!p->swr || swr_init(p->swr) < 0) {
		swr_free(&p->swr);
		return -1;
	}
	return 0;
}

static void start_audio(struct player *p);

static int open_media(struct player *p)
{
	AVIOContext *io;
	unsigned char *iobuf = av_malloc(65536);
	int w, h;

	p->fmt = avformat_alloc_context();
	if (!iobuf || !p->fmt)
		return -1;
	io = avio_alloc_context(iobuf, 65536, 0, &p->src, src_read, NULL,
		src_seek);
	if (!io)
		return -1;
	p->fmt->pb = io;
	p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	if (avformat_open_input(&p->fmt, p->src.url, NULL, NULL) < 0) {
		p->fmt = NULL;	/* freed by the failure */
		av_freep(&io->buffer);
		avio_context_free(&io);
		return -1;
	}
	if (avformat_find_stream_info(p->fmt, NULL) < 0)
		return -1;
	p->vs = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	p->as = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (p->vs >= 0 && !(p->vdec = open_decoder(p->fmt, p->vs)))
		p->vs = -1;
	if (p->as >= 0 && !(p->adec = open_decoder(p->fmt, p->as)))
		p->as = -1;
	if (p->vs < 0 && p->as < 0)
		return -1;
	if (p->vs >= 0)
		p->vtb = p->fmt->streams[p->vs]->time_base;
	if (p->as >= 0) {
		p->atb = p->fmt->streams[p->as]->time_base;
		if (open_resampler(p)) {
			avcodec_free_context(&p->adec);
			p->as = -1;
		}
	}
	pthread_mutex_lock(&p->lock);
	if (p->vs >= 0) {
		w = p->vdec->width;
		h = p->vdec->height;
		fit(&w, &h);
		p->width = w;
		p->height = h;
		if (!p->width || !p->height)
			p->vs = -1;
	}
	if (p->fmt->duration > 0)
		p->duration = (double)p->fmt->duration / AV_TIME_BASE;
	pthread_mutex_unlock(&p->lock);
	start_audio(p);
	pthread_mutex_lock(&p->lock);
	if (!p->has_athread && p->as >= 0) {
		swr_free(&p->swr);
		avcodec_free_context(&p->adec);
		p->as = -1;
	}
	p->opened = 1;
	p->changes++;
	pthread_mutex_unlock(&p->lock);
	return 0;
}

static void do_seek(struct player *p, double to)
{
	int i;

	av_seek_frame(p->fmt, -1, (int64_t)(to * AV_TIME_BASE),
		AVSEEK_FLAG_BACKWARD);
	if (p->vdec)
		avcodec_flush_buffers(p->vdec);
	if (p->adec)
		avcodec_flush_buffers(p->adec);
	pthread_mutex_lock(&p->lock);
	p->count = 0;
	p->pcm_count = 0;
	p->flush = 1;
	p->eof = 0;
	p->ended = 0;
	p->base = to;
	p->started = p->playing ? now() : 0;
	p->audio_clock = -1;
	p->played = 0;
	for (i = 0; i < NFRAMES; i++)
		p->ring[i].pts = 0;
	p->shown_pts = to;
	p->skip = to;
	p->changes++;
	p->seeks++;
	pthread_mutex_unlock(&p->lock);
}

static void *decode_thread(void *arg)
{
	struct player *p = arg;
	AVPacket pkt;
	AVFrame *fr = av_frame_alloc();
	int r;

	if (!fr || open_media(p)) {
		pthread_mutex_lock(&p->lock);
		p->error = 1;
		p->changes++;
		pthread_mutex_unlock(&p->lock);
		av_frame_free(&fr);
		return NULL;
	}
	for (;;) {
		pthread_mutex_lock(&p->lock);
		while (!p->quit && !p->seek_req && (!p->playing || p->eof) &&
		       !(p->count == 0 && p->vs >= 0 && !p->eof))
			pthread_cond_wait(&p->cond, &p->lock);
		if (p->quit) {
			pthread_mutex_unlock(&p->lock);
			break;
		}
		if (p->seek_req) {
			double to = p->seek_to;

			p->seek_req = 0;
			pthread_mutex_unlock(&p->lock);
			do_seek(p, to);
			continue;
		}
		pthread_mutex_unlock(&p->lock);
		r = av_read_frame(p->fmt, &pkt);
		if (r < 0) {
			/* the end: what the decoders still hold */
			if (p->vdec) {
				avcodec_send_packet(p->vdec, NULL);
				drain(p, p->vdec, fr, 1);
			}
			if (p->adec) {
				avcodec_send_packet(p->adec, NULL);
				drain(p, p->adec, fr, 0);
			}
			pthread_mutex_lock(&p->lock);
			p->eof = 1;
			pthread_cond_broadcast(&p->cond);
			pthread_mutex_unlock(&p->lock);
			continue;
		}
		if (pkt.stream_index == p->vs &&
		    avcodec_send_packet(p->vdec, &pkt) == 0)
			drain(p, p->vdec, fr, 1);
		else if (pkt.stream_index == p->as &&
			 avcodec_send_packet(p->adec, &pkt) == 0)
			drain(p, p->adec, fr, 0);
		av_packet_unref(&pkt);
	}
	av_frame_free(&fr);
	return NULL;
}

/* The sound output thread */

static void *audio_thread(void *arg)
{
	struct player *p = arg;
	int16_t buf[2 * 1024];
	snd_pcm_sframes_t delay, w;
	int n, i, k;
	double vol;

	for (;;) {
		pthread_mutex_lock(&p->lock);
		while (!p->quit && (!p->playing || !p->pcm_count ||
		       p->waiting) && !p->flush)
			pthread_cond_wait(&p->cond, &p->lock);
		if (p->quit) {
			pthread_mutex_unlock(&p->lock);
			break;
		}
		if (p->flush) {
			p->flush = 0;
			pthread_mutex_unlock(&p->lock);
			snd_pcm_drop(p->dev);
			snd_pcm_prepare(p->dev);
			continue;
		}
		n = p->pcm_count < 1024 ? p->pcm_count : 1024;
		for (i = 0; i < n; i++) {
			k = (p->pcm_head + i) % PCM_FRAMES;
			buf[2 * i] = p->pcm[2 * k];
			buf[2 * i + 1] = p->pcm[2 * k + 1];
		}
		p->pcm_head = (p->pcm_head + n) % PCM_FRAMES;
		p->pcm_count -= n;
		vol = p->muted ? 0 : p->volume;
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->lock);
		if (vol < 1)
			for (i = 0; i < 2 * n; i++)
				buf[i] = (int16_t)(buf[i] * vol);
		w = snd_pcm_writei(p->dev, buf, (snd_pcm_uframes_t)n);
		if (w < 0)
			snd_pcm_recover(p->dev, (int)w, 1);
		pthread_mutex_lock(&p->lock);
		if (w > 0)
			p->played += w;
		if (snd_pcm_delay(p->dev, &delay) < 0)
			delay = 0;
		if (p->rate) {
			p->audio_clock = p->base + (double)(p->played - delay) /
				p->rate;
			p->audio_at = now();
		}
		pthread_mutex_unlock(&p->lock);
	}
	return NULL;
}

/* Sound output, from the decoding thread once the stream is known.
 * Without it, sound is not decoded at all. */
static void start_audio(struct player *p)
{
	const char *env = getenv("SOIE_AUDIO");

	if (p->has_athread || p->as < 0 || (env && !strcmp(env, "none")))
		return;
	if (snd_pcm_open(&p->dev, env && *env ? env : "default",
	    SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		p->dev = NULL;
		return;
	}
	if (snd_pcm_set_params(p->dev, SND_PCM_FORMAT_S16,
	    SND_PCM_ACCESS_RW_INTERLEAVED, 2, (unsigned)p->rate, 1, 100000) < 0 ||
	    pthread_create(&p->athread, NULL, audio_thread, p)) {
		snd_pcm_close(p->dev);
		p->dev = NULL;
		return;
	}
	p->has_athread = 1;
}

/*
 * Media Source Extensions
 */

static void init_unref(struct init *in)
{
	if (in && !--in->refs) {
		avcodec_parameters_free(&in->par);
		free(in);
	}
}

/* Is t (us) in ranges? */
static int in_ranges(const int64_t *r, int n, int64_t t)
{
	int i;

	for (i = 0; i < n; i++)
		if (t >= r[2 * i] && t < r[2 * i + 1])
			return 1;
	return 0;
}

static int cmp_i64(const void *a, const void *b)
{
	const int64_t *x = a, *y = b;

	return *x < *y ? -1 : *x > *y;
}

/* A track's buffered ranges: its frames by time, merged across GAP */
static void track_ranges(struct track *t)
{
	int64_t *v;
	int i, n = 0;

	t->nranges = 0;
	if (!t->n || !(v = malloc((size_t)t->n * 2 * sizeof(*v))))
		return;
	for (i = 0; i < t->n; i++) {
		v[2 * i] = t->f[i].pts;
		v[2 * i + 1] = t->f[i].end;
	}
	qsort(v, (size_t)t->n, 2 * sizeof(*v), cmp_i64);
	for (i = 0; i < t->n; i++) {
		if (n && v[2 * i] <= t->ranges[2 * n - 1] + GAP) {
			if (v[2 * i + 1] > t->ranges[2 * n - 1])
				t->ranges[2 * n - 1] = v[2 * i + 1];
			continue;
		}
		if (n == MAX_RANGES)
			break;
		t->ranges[2 * n] = v[2 * i];
		t->ranges[2 * n + 1] = v[2 * i + 1];
		n++;
	}
	t->nranges = n;
	free(v);
}

/* out = a and b (out may be a) */
static int intersect(const int64_t *a, int na, const int64_t *b, int nb,
	int64_t *out)
{
	int64_t r[2 * MAX_RANGES];
	int i = 0, j = 0, n = 0;

	while (i < na && j < nb && n < MAX_RANGES) {
		int64_t s = a[2 * i] > b[2 * j] ? a[2 * i] : b[2 * j];
		int64_t e = a[2 * i + 1] < b[2 * j + 1] ? a[2 * i + 1] :
			b[2 * j + 1];

		if (s < e) {
			r[2 * n] = s;
			r[2 * n + 1] = e;
			n++;
		}
		if (a[2 * i + 1] < b[2 * j + 1])
			i++;
		else
			j++;
	}
	memcpy(out, r, (size_t)n * 2 * sizeof(*r));
	return n;
}

/* A buffer's ranges: those of its tracks together */
static int buffer_ranges(const struct sbuf *b, int64_t *out)
{
	int n = -1, k;

	for (k = 0; k < 2; k++) {
		if (!b->init[k] && !b->t[k].n)
			continue;
		if (n < 0) {
			n = b->t[k].nranges;
			memcpy(out, b->t[k].ranges, (size_t)n * 2 * sizeof(*out));
		} else {
			n = intersect(out, n, b->t[k].ranges, b->t[k].nranges, out);
		}
	}
	return n < 0 ? 0 : n;
}

/* The tracks that play: the first video one, the first audio one */
static struct track *track_of(struct player *p, int k)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++)
		if (p->sb[i].used && p->sb[i].init[k])
			return &p->sb[i].t[k];
	return NULL;
}

/* What plays, under lock */
static void player_ranges(struct player *p)
{
	struct track *v = track_of(p, 0), *a = track_of(p, 1);

	if (v && a)
		p->nranges = intersect(v->ranges, v->nranges, a->ranges,
			a->nranges, p->ranges);
	else if (v || a) {
		p->nranges = (v ? v : a)->nranges;
		memcpy(p->ranges, (v ? v : a)->ranges,
			(size_t)p->nranges * 2 * sizeof(*p->ranges));
	} else
		p->nranges = 0;
}

static void drop_frame(struct track *t, int i)
{
	t->bytes -= (size_t)t->f[i].pkt->size;
	av_packet_free(&t->f[i].pkt);
	init_unref(t->f[i].init);
}

/* Drop the frames of runs other than run starting in [start, end),
 * and those decoded after them up to the next key frame (they need
 * them). run 0: of any append. */
static void remove_frames(struct track *t, int64_t start, int64_t end,
	unsigned run)
{
	int i, w = 0, dropping = 0;

	for (i = 0; i < t->n; i++) {
		struct cframe *c = &t->f[i];
		int old = c->run != run;

		if (old && (c->pkt->flags & AV_PKT_FLAG_KEY))
			dropping = 0;
		if (old && (dropping || (c->pts >= start && c->pts < end))) {
			drop_frame(t, i);
			dropping = 1;
			continue;
		}
		t->f[w++] = *c;
	}
	t->n = w;
}

/* A coded frame into its track, in decode order; under lock */
static void insert_frame(struct track *t, struct cframe *f)
{
	int lo = 0, hi;

	if (f->run != t->oldrun) {
		/* a new append: what was there before may be replaced */
		int i;

		t->oldrun = f->run;
		t->oldmax = INT64_MIN;
		for (i = 0; i < t->n; i++)
			if (t->f[i].end > t->oldmax)
				t->oldmax = t->f[i].end;
	}
	if (f->pts < t->oldmax)
		remove_frames(t, f->pts, f->end, f->run);
	if (t->n == t->cap) {
		struct cframe *v;

		t->cap = t->cap ? t->cap * 2 : 256;
		v = realloc(t->f, (size_t)t->cap * sizeof(*v));
		if (!v)
			abort();
		t->f = v;
	}
	hi = t->n;
	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (t->f[mid].pkt->dts <= f->pkt->dts)
			lo = mid + 1;
		else
			hi = mid;
	}
	memmove(&t->f[lo + 1], &t->f[lo], (size_t)(t->n - lo) * sizeof(*f));
	t->f[lo] = *f;
	t->n++;
	t->bytes += (size_t)f->pkt->size;
}

/* The parser's bytes: what was appended, after the initialization
 * segment to replay; it waits for more and reads the end of the
 * stream when the demuxer must open again. */
static int fifo_read(void *opaque, uint8_t *out, int size)
{
	struct sbuf *b = opaque;
	struct player *p = b->p;
	size_t n;

	pthread_mutex_lock(&p->lock);
	while (!b->quit && !(b->reset && b->consumed) && b->off == b->len) {
		if (!b->idle) {
			b->idle = 1;	/* the append is parsed */
			pthread_cond_broadcast(&p->cond);
		}
		pthread_cond_wait(&p->cond, &p->lock);
	}
	if (b->quit || (b->reset && b->consumed)) {
		pthread_mutex_unlock(&p->lock);
		return AVERROR_EOF;
	}
	if (b->replay) {
		n = b->replay < (size_t)size ? b->replay : (size_t)size;
		memcpy(out, b->initseg + b->initlen - b->replay, n);
		b->replay -= n;
	} else {
		n = b->len - b->off < (size_t)size ? b->len - b->off :
			(size_t)size;
		memcpy(out, b->fifo + b->off, n);
		b->off += n;
		if (b->off == b->len)
			b->off = b->len = 0;
	}
	b->consumed = 1;
	pthread_mutex_unlock(&p->lock);
	return (int)n;
}

/* The demuxer opened: its first video and audio streams */
static void new_streams(struct sbuf *b, AVFormatContext *fmt)
{
	struct player *p = b->p;
	int k, i;

	pthread_mutex_lock(&p->lock);
	for (k = 0; k < 2; k++) {
		i = av_find_best_stream(fmt, k ? AVMEDIA_TYPE_AUDIO :
			AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
		b->stream[k] = i;
		if (i < 0)
			continue;
		init_unref(b->init[k]);
		b->init[k] = calloc(1, sizeof(struct init));
		if (!b->init[k] || !(b->init[k]->par = avcodec_parameters_alloc()))
			abort();
		b->init[k]->refs = 1;
		avcodec_parameters_copy(b->init[k]->par,
			fmt->streams[i]->codecpar);
		if (k == 0 && !p->width) {
			int w = b->init[k]->par->width, h = b->init[k]->par->height;

			fit(&w, &h);
			p->width = w;
			p->height = h;
		}
	}
	if (fmt->duration > 0 && p->duration <= 0)
		p->duration = (double)fmt->duration / AV_TIME_BASE;
	p->changes++;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);
}

static void new_packet(struct sbuf *b, AVFormatContext *fmt, AVPacket *pkt)
{
	struct player *p = b->p;
	AVRational tb, us = AV_TIME_BASE_Q;
	struct cframe f;
	int64_t pts, dts, dur;
	int k;

	k = pkt->stream_index == b->stream[0] ? 0 :
		pkt->stream_index == b->stream[1] ? 1 : -1;
	if (k < 0 || !b->init[k])
		return;
	tb = fmt->streams[pkt->stream_index]->time_base;
	pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
	if (pts == AV_NOPTS_VALUE)
		return;
	dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pts;
	pts = av_rescale_q(pts, tb, us) + b->offset;
	dts = av_rescale_q(dts, tb, us) + b->offset;
	dur = av_rescale_q(pkt->duration, tb, us);
	if (dur <= 0)
		dur = k ? 20000 : 33333;
	/* the append window */
	if (pts < b->wstart || pts + dur > b->wend)
		return;
	if (!(f.pkt = av_packet_clone(pkt)))
		return;
	f.pkt->pts = pts;
	f.pkt->dts = dts;
	f.pkt->duration = dur;
	f.pkt->stream_index = 0;
	f.pts = pts;
	f.end = pts + dur;
	f.run = b->run;
	pthread_mutex_lock(&p->lock);
	f.init = b->init[k];
	f.init->refs++;
	insert_frame(&b->t[k], &f);
	pthread_mutex_unlock(&p->lock);
}

static void *parse_thread(void *arg)
{
	struct sbuf *b = arg;
	struct player *p = b->p;
	AVPacket *pkt = av_packet_alloc();
	AVFormatContext *fmt;
	AVIOContext *io;
	unsigned char *iobuf;
	int quit, reset;

	for (;;) {
		pthread_mutex_lock(&p->lock);
		b->consumed = 0;
		b->reset = 0;
		pthread_mutex_unlock(&p->lock);
		fmt = avformat_alloc_context();
		iobuf = av_malloc(65536);
		io = iobuf ? avio_alloc_context(iobuf, 65536, 0, b, fifo_read,
			NULL, NULL) : NULL;
		if (!fmt || !io || !pkt)
			abort();
		io->seekable = 0;
		fmt->pb = io;
		fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
		if (avformat_open_input(&fmt, "", b->ifmt, NULL) == 0) {
			new_streams(b, fmt);
			while (av_read_frame(fmt, pkt) >= 0) {
				new_packet(b, fmt, pkt);
				av_packet_unref(pkt);
			}
			avformat_close_input(&fmt);
		}
		av_freep(&io->buffer);
		avio_context_free(&io);
		pthread_mutex_lock(&p->lock);
		quit = b->quit;
		reset = b->reset && b->consumed;
		if (!quit && !reset) {
			/* not media: what is left goes, a new segment may come */
			b->error = 1;
			b->off = b->len = 0;
			b->replay = b->initlen;
			b->idle = 1;
			pthread_cond_broadcast(&p->cond);
		}
		pthread_mutex_unlock(&p->lock);
		if (quit)
			break;
	}
	av_packet_free(&pkt);
	return NULL;
}

/* The length of the initialization segment bytes start with, 0 when
 * they start with a media segment */
static size_t init_length(const unsigned char *d, size_t n)
{
	static const unsigned char ebml[4] = { 0x1a, 0x45, 0xdf, 0xa3 };
	static const unsigned char cluster[4] = { 0x1f, 0x43, 0xb6, 0x75 };
	size_t i = 0;

	if (n >= 4 && !memcmp(d, ebml, 4)) {
		for (i = 4; i + 4 <= n; i++)
			if (!memcmp(d + i, cluster, 4))
				return i;
		return n;
	}
	if (n < 8 || (memcmp(d + 4, "ftyp", 4) && memcmp(d + 4, "moov", 4)))
		return 0;
	while (i + 8 <= n) {
		uint64_t size = (uint64_t)d[i] << 24 | (uint64_t)d[i + 1] << 16 |
			(uint64_t)d[i + 2] << 8 | d[i + 3];

		if (!memcmp(d + i + 4, "moof", 4) ||
		    !memcmp(d + i + 4, "mdat", 4) ||
		    !memcmp(d + i + 4, "styp", 4) ||
		    !memcmp(d + i + 4, "sidx", 4))
			return i;
		if (size == 1 && i + 16 <= n) {
			int j;

			for (size = 0, j = 8; j < 16; j++)
				size = size << 8 | d[i + j];
		}
		if (size < 8 || size > n - i)
			return n;
		i += size;
	}
	return n;
}

/* Frames up to the last key frame before limit go from t (eviction) */
static void evict(struct track *t, int64_t limit)
{
	int i, k = -1;

	for (i = 0; i < t->n; i++)
		if ((t->f[i].pkt->flags & AV_PKT_FLAG_KEY) &&
		    t->f[i].pts <= limit)
			k = i;
	if (k <= 0)
		return;
	for (i = 0; i < k; i++)
		drop_frame(t, i);
	memmove(t->f, t->f + k, (size_t)(t->n - k) * sizeof(*t->f));
	t->n -= k;
}

static void ranges_changed(struct sbuf *b)
{
	track_ranges(&b->t[0]);
	track_ranges(&b->t[1]);
	player_ranges(b->p);
	b->p->changes++;
	pthread_cond_broadcast(&b->p->cond);
}

/* The next frame of t to decode, under lock; after a seek, the key
 * frame at or before its time, once it is buffered */
static struct cframe *next_frame(struct player *p, struct track *t)
{
	int lo = 0, hi = t->n, i;

	if (t->seek) {
		int k = -1;

		if (!in_ranges(t->ranges, t->nranges, p->seek_us))
			return NULL;
		for (i = 0; i < t->n; i++)
			if ((t->f[i].pkt->flags & AV_PKT_FLAG_KEY) &&
			    t->f[i].pts <= p->seek_us)
				k = i;
		if (k < 0)
			return NULL;
		t->seek = 0;
		t->cur = t->f[k].pkt->dts - 1;	/* from there on */
		if (t == track_of(p, 0) || !track_of(p, 0))
			p->seeks++;
		return &t->f[k];
	}
	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (t->f[mid].pkt->dts <= t->cur)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < t->n ? &t->f[lo] : NULL;
}

/* A decoder for in (video or audio), the one open closes */
static void set_decoder(struct player *p, int audio, struct init *in)
{
	AVCodecContext **dec = audio ? &p->adec : &p->vdec;
	struct init **cur = audio ? &p->ainit : &p->vinit;

	avcodec_free_context(dec);
	/* the parsers count references too */
	pthread_mutex_lock(&p->lock);
	init_unref(*cur);
	*cur = in;
	in->refs++;
	pthread_mutex_unlock(&p->lock);
	*dec = open_codec(in->par, AV_TIME_BASE_Q);
	if (audio) {
		p->atb = AV_TIME_BASE_Q;
		if (*dec && open_resampler(p))
			avcodec_free_context(dec);
		if (*dec) {
			p->as = 0;
			start_audio(p);
		}
		/* without a sound card, sound is not decoded */
		if (!p->has_athread)
			avcodec_free_context(dec);
	} else {
		p->vtb = AV_TIME_BASE_Q;
	}
}

static void source_seek(struct player *p, double to)
{
	int i, k;

	if (p->vdec)
		avcodec_flush_buffers(p->vdec);
	if (p->adec)
		avcodec_flush_buffers(p->adec);
	pthread_mutex_lock(&p->lock);
	p->count = 0;
	p->pcm_count = 0;
	p->flush = 1;
	p->eof = 0;
	p->ended = 0;
	p->waiting = 0;
	p->base = to;
	p->started = p->playing ? now() : 0;
	p->audio_clock = -1;
	p->played = 0;
	p->shown_pts = to;
	p->skip = to;
	p->seek_us = (int64_t)(to * 1e6);
	for (i = 0; i < MAX_BUFFERS; i++)
		for (k = 0; k < 2; k++)
			p->sb[i].t[k].seek = 1;
	if (!track_of(p, 0) && !track_of(p, 1))
		p->seeks++;
	p->changes++;
	pthread_mutex_unlock(&p->lock);
}

static void *source_thread(void *arg)
{
	struct player *p = arg;
	AVFrame *fr = av_frame_alloc();
	struct track *v, *a;
	struct cframe *fv, *fa, *f;
	AVPacket *pkt;
	struct init *in;
	int audio;

	if (!fr)
		return NULL;
	pthread_mutex_lock(&p->lock);
	for (;;) {
		if (p->quit)
			break;
		if (p->seek_req) {
			double to = p->seek_to;

			p->seek_req = 0;
			pthread_mutex_unlock(&p->lock);
			source_seek(p, to);
			pthread_mutex_lock(&p->lock);
			continue;
		}
		v = track_of(p, 0);
		a = track_of(p, 1);
		fv = v ? next_frame(p, v) : NULL;
		fa = a ? next_frame(p, a) : NULL;
		if (p->eof && (fv || fa)) {
			/* more came after the end */
			p->eof = 0;
			pthread_mutex_unlock(&p->lock);
			if (p->vdec)
				avcodec_flush_buffers(p->vdec);
			if (p->adec)
				avcodec_flush_buffers(p->adec);
			pthread_mutex_lock(&p->lock);
			continue;
		}
		/* paused, one frame ahead is enough */
		if (!(p->playing || (v && !p->count)) || (!fv && !fa)) {
			if (!fv && !fa && p->eos && !p->eof &&
			    !(v && v->seek) && !(a && a->seek)) {
				/* the end: what the decoders still hold */
				pthread_mutex_unlock(&p->lock);
				if (p->vdec) {
					avcodec_send_packet(p->vdec, NULL);
					drain(p, p->vdec, fr, 1);
				}
				if (p->adec) {
					avcodec_send_packet(p->adec, NULL);
					drain(p, p->adec, fr, 0);
				}
				pthread_mutex_lock(&p->lock);
				p->eof = 1;
				p->changes++;
				pthread_cond_broadcast(&p->cond);
				continue;
			}
			pthread_cond_wait(&p->cond, &p->lock);
			continue;
		}
		/*
		 * Decode order, unless what one track gives cannot go: the
		 * frames wait to be shown, or the sound to be played. With
		 * sound the clock is the card's: video frames ahead of the
		 * sound decoded (B-frames come late out of the decoder) would
		 * fill the frames and stop the decoding, the sound would run
		 * out and the clock stop with it.
		 */
		if (fv && fa && p->count == NFRAMES)
			fv = NULL;
		else if (fv && fa && p->adec &&
			 p->pcm_count > PCM_FRAMES - 8192)
			fa = NULL;
		audio = !fv || (fa && fa->pkt->dts < fv->pkt->dts);
		f = audio ? fa : fv;
		(audio ? a : v)->cur = f->pkt->dts;
		pkt = av_packet_clone(f->pkt);
		in = f->init;
		in->refs++;
		pthread_mutex_unlock(&p->lock);
		if (in != (audio ? p->ainit : p->vinit)) {
			/* a new stream: the old decoder gives what it holds */
			AVCodecContext *old = audio ? p->adec : p->vdec;

			if (old) {
				avcodec_send_packet(old, NULL);
				drain(p, old, fr, !audio);
			}
			set_decoder(p, audio, in);
		}
		if (pkt && (audio ? p->adec : p->vdec) &&
		    avcodec_send_packet(audio ? p->adec : p->vdec, pkt) == 0)
			drain(p, audio ? p->adec : p->vdec, fr, !audio);
		av_packet_free(&pkt);
		pthread_mutex_lock(&p->lock);
		init_unref(in);
	}
	pthread_mutex_unlock(&p->lock);
	av_frame_free(&fr);
	return NULL;
}

/* The clock waits where nothing is buffered, and goes on once there
 * is half a second ahead, or the end; small gaps are jumped. Under
 * lock, from advance(). */
static void wait_or_go(struct player *p)
{
	double t = clock_now(p);
	int64_t us = (int64_t)(t * 1e6), last;
	int i, end;

	if (!p->playing)
		return;
	last = p->nranges ? p->ranges[2 * p->nranges - 1] : 0;
	end = p->eos && us >= last - 100000;
	if (!p->waiting) {
		if (end || in_ranges(p->ranges, p->nranges, us))
			return;
		p->base = t;
		p->started = 0;
		p->audio_clock = -1;
		p->waiting = 1;
		p->changes++;
		return;
	}
	for (i = 0; i < p->nranges; i++) {
		int64_t s = p->ranges[2 * i], e = p->ranges[2 * i + 1];

		if (us < s && s - us < 500000) {
			us = s;	/* a gap */
			p->base = (double)s / 1e6;
		}
		if (end || (us >= s && us < e && (e - us >= 500000 ||
		    (p->eos && i == p->nranges - 1)))) {
			p->waiting = 0;
			p->started = now();
			p->audio_clock = -1;
			p->played = 0;
			p->changes++;
			pthread_cond_broadcast(&p->cond);
			return;
		}
	}
}

/* Types: MIME type, codecs, and what a small machine plays well */
static int codec_ok(const char *c, int video)
{
	static const struct {
		const char *prefix;
		enum AVCodecID id;
		int video;
	} codecs[] = {
		{ "avc1", AV_CODEC_ID_H264, 1 }, { "avc3", AV_CODEC_ID_H264, 1 },
		{ "vp8", AV_CODEC_ID_VP8, 1 },
		{ "mp4a.40", AV_CODEC_ID_AAC, 0 }, { "mp4a.67", AV_CODEC_ID_AAC, 0 },
		{ "mp4a.69", AV_CODEC_ID_MP3, 0 }, { "mp4a.6b", AV_CODEC_ID_MP3, 0 },
		{ "opus", AV_CODEC_ID_OPUS, 0 }, { "vorbis", AV_CODEC_ID_VORBIS, 0 },
		{ "flac", AV_CODEC_ID_FLAC, 0 }, { "ac-3", AV_CODEC_ID_AC3, 0 },
		{ "ec-3", AV_CODEC_ID_EAC3, 0 }
	};
	size_t i, n;

	for (i = 0; i < sizeof(codecs) / sizeof(*codecs); i++) {
		n = strlen(codecs[i].prefix);
		if (!strncmp(c, codecs[i].prefix, n) &&
		    (!c[n] || c[n] == '.') && (video || !codecs[i].video))
			return avcodec_find_decoder(codecs[i].id) != NULL;
	}
	/* VP9, AV1, HEVC: too heavy without hardware, H.264 is offered */
	return 0;
}

/* A parameter of a MIME type ("codecs", "width"...), lower case,
 * unquoted; NULL when absent */
static char *type_param(const char *type, const char *name, char *out,
	size_t size)
{
	const char *s = strchr(type, ';');
	size_t n = strlen(name), i;

	while (s) {
		s++;
		while (*s == ' ' || *s == '\t')
			s++;
		if (!strncasecmp(s, name, n) && s[n] == '=') {
			s += n + 1;
			if (*s == '"')
				s++;
			for (i = 0; *s && *s != '"' && *s != ';' &&
			     i < size - 1; s++)
				out[i++] = (char)tolower((unsigned char)*s);
			out[i] = 0;
			return out;
		}
		s = strchr(s, ';');
	}
	return NULL;
}

/* The demuxer for a SourceBuffer of type, or NULL */
static AVInputFormat *source_format(const char *type)
{
	char codecs[256], num[32], *c, *next;
	int video = !strncasecmp(type, "video/", 6);
	double w, h;
	AVInputFormat *f;

	if (!strncasecmp(type, "video/mp4", 9) ||
	    !strncasecmp(type, "audio/mp4", 9))
		f = av_find_input_format("mp4");
	else if (!strncasecmp(type, "video/webm", 10) ||
		 !strncasecmp(type, "audio/webm", 10))
		f = av_find_input_format("webm");
	else
		return NULL;
	if (type_param(type, "codecs", codecs, sizeof(codecs)))
		for (c = codecs; c; c = next) {
			if ((next = strchr(c, ',')))
				*next++ = 0;
			while (*c == ' ')
				c++;
			if (!codec_ok(c, video))
				return NULL;
		}
	/* decoded to be scaled down: wasted (a side of 1280 at most, and
	 * the pixels of 1280x720, portrait videos too) */
	w = type_param(type, "width", num, sizeof(num)) ? atof(num) : 0;
	h = type_param(type, "height", num, sizeof(num)) ? atof(num) : 0;
	if (w > 1280 || h > 1280 || w * h > MAX_PIXELS)
		return NULL;
	if (type_param(type, "framerate", num, sizeof(num)) &&
	    atof(num) > 31)
		return NULL;
	return f;
}

/* The hooks */

static int can_play(void *data, const char *type)
{
	char low[64];
	size_t i;

	(void)data;
	if (!type || !*type)
		return 1;
	for (i = 0; type[i] && i < sizeof(low) - 1; i++)
		low[i] = (char)tolower((unsigned char)type[i]);
	low[i] = 0;
	if (strstr(low, "mpegurl") || strstr(low, "dash"))
		return 0;	/* streaming playlists: not ours */
	return !strncasecmp(type, "video/", 6) ||
		!strncasecmp(type, "audio/", 6);
}

static void *open_player(void *data, const char *url,
	soie_request_fn request, void *rdata)
{
	struct player *p = calloc(1, sizeof(*p));

	(void)data;
	if (!p)
		return NULL;
	p->src.request = request;
	p->src.rdata = rdata;
	p->src.url = strdup(url);
	p->src.size = -1;
	p->pcm = malloc(PCM_FRAMES * 2 * sizeof(*p->pcm));
	p->vs = p->as = -1;
	p->audio_clock = -1;
	p->volume = 1;
	if (!p->src.url || !p->pcm) {
		free(p->src.url);
		free(p->pcm);
		free(p);
		return NULL;
	}
	pthread_mutex_init(&p->lock, NULL);
	pthread_cond_init(&p->cond, NULL);
	if (pthread_create(&p->thread, NULL, decode_thread, p)) {
		free(p->src.url);
		free(p->pcm);
		free(p);
		return NULL;
	}
	return p;
}

static void close_player(void *h)
{
	struct player *p = h;
	int i, k, j;

	pthread_mutex_lock(&p->lock);
	p->quit = 1;
	for (i = 0; i < MAX_BUFFERS; i++)
		p->sb[i].quit = 1;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);
	pthread_join(p->thread, NULL);
	for (i = 0; i < MAX_BUFFERS; i++) {
		struct sbuf *b = &p->sb[i];

		if (!b->used)
			continue;
		pthread_join(b->thread, NULL);
		for (k = 0; k < 2; k++) {
			for (j = 0; j < b->t[k].n; j++)
				drop_frame(&b->t[k], j);
			free(b->t[k].f);
			init_unref(b->init[k]);
		}
		free(b->fifo);
		free(b->initseg);
	}
	init_unref(p->vinit);
	init_unref(p->ainit);
	if (p->has_athread) {
		pthread_join(p->athread, NULL);
		snd_pcm_close(p->dev);
	}
	if (p->fmt) {
		AVIOContext *io = p->fmt->pb;

		avformat_close_input(&p->fmt);
		if (io) {
			av_freep(&io->buffer);
			avio_context_free(&io);
		}
	}
	avcodec_free_context(&p->vdec);
	avcodec_free_context(&p->adec);
	sws_freeContext(p->sws);
	swr_free(&p->swr);
	for (i = 0; i < NFRAMES; i++)
		free(p->ring[i].rgba);
	free(p->shown);
	free(p->pcm);
	free(p->src.buf);
	free(p->src.url);
	pthread_mutex_destroy(&p->lock);
	pthread_cond_destroy(&p->cond);
	free(p);
}

static void play(void *h)
{
	struct player *p = h;

	pthread_mutex_lock(&p->lock);
	if (p->ended) {
		p->seek_req = 1;
		p->seek_to = 0;
		p->base = 0;
	}
	if (!p->playing) {
		p->playing = 1;
		p->started = now();
		p->audio_clock = -1;
		p->played = 0;
		p->ended = 0;
		p->waiting = 0;	/* the clock looks again */
		p->changes++;
	}
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);
}

static void pause_player(void *h)
{
	struct player *p = h;

	pthread_mutex_lock(&p->lock);
	if (p->playing) {
		p->base = clock_now(p);
		p->started = 0;
		p->audio_clock = -1;
		p->played = 0;
		p->playing = 0;
		p->flush = 1;
		p->changes++;
		pthread_cond_broadcast(&p->cond);
	}
	pthread_mutex_unlock(&p->lock);
}

static void seek(void *h, double t)
{
	struct player *p = h;

	pthread_mutex_lock(&p->lock);
	if (t < 0)
		t = 0;
	if (p->duration > 0 && t > p->duration)
		t = p->duration;
	p->seek_req = 1;
	p->seek_to = t;
	p->base = t;
	p->started = p->playing ? now() : 0;
	p->audio_clock = -1;
	p->changes++;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);
}

static void volume(void *h, double v, int muted)
{
	struct player *p = h;

	pthread_mutex_lock(&p->lock);
	p->volume = v < 0 ? 0 : v > 1 ? 1 : v;
	p->muted = muted;
	pthread_mutex_unlock(&p->lock);
}

static void state(void *h, soie_media_state *st)
{
	struct player *p = h;

	pthread_mutex_lock(&p->lock);
	advance(p);
	st->duration = p->duration;
	if (p->source) {
		/* known once every SourceBuffer had its initialization */
		int i, ready = 0;

		for (i = 0; i < MAX_BUFFERS; i++)
			if (p->sb[i].used)
				ready = p->sb[i].init[0] || p->sb[i].init[1] ?
					ready | 1 : ready | 2;
		if (ready != 1)
			st->duration = 0;
		else if (p->duration <= 0)
			st->duration = p->nranges ?
				(double)p->ranges[2 * p->nranges - 1] / 1e6 : 1e-6;
	}
	st->time = clock_now(p);
	if (p->duration > 0 && st->time > p->duration)
		st->time = p->duration;
	st->paused = !p->playing;
	st->ended = p->ended;
	st->width = p->width;
	st->height = p->height;
	st->error = p->error;
	st->changes = p->changes;
	st->waiting = p->waiting;
	st->seeks = p->seeks;
	st->decoded = p->decoded;
	st->dropped = p->dropped;
	pthread_mutex_unlock(&p->lock);
}

static const unsigned char *frame(void *h, int *w, int *hh)
{
	struct player *p = h;
	const unsigned char *f;

	pthread_mutex_lock(&p->lock);
	advance(p);
	f = p->shown;
	*w = p->shown_w;
	*hh = p->shown_h;
	pthread_mutex_unlock(&p->lock);
	return f;
}

/* Media Source Extensions hooks */

static int can_source(void *data, const char *type)
{
	(void)data;
	return type && source_format(type) != NULL;
}

static void *open_source(void *data)
{
	struct player *p = calloc(1, sizeof(*p));

	(void)data;
	if (!p || !(p->pcm = malloc(PCM_FRAMES * 2 * sizeof(*p->pcm)))) {
		free(p);
		return NULL;
	}
	p->source = 1;
	p->vs = p->as = -1;
	p->audio_clock = -1;
	p->volume = 1;
	p->opened = 1;
	pthread_mutex_init(&p->lock, NULL);
	pthread_cond_init(&p->cond, NULL);
	if (pthread_create(&p->thread, NULL, source_thread, p)) {
		free(p->pcm);
		free(p);
		return NULL;
	}
	return p;
}

static int add_buffer(void *h, const char *type)
{
	struct player *p = h;
	AVInputFormat *f = source_format(type);
	struct sbuf *b;
	int i;

	if (!f)
		return -1;
	pthread_mutex_lock(&p->lock);
	for (i = 0; i < MAX_BUFFERS && p->sb[i].used; i++)
		;
	if (i == MAX_BUFFERS) {
		pthread_mutex_unlock(&p->lock);
		return -1;
	}
	b = &p->sb[i];
	memset(b, 0, sizeof(*b));
	b->p = p;
	b->ifmt = f;
	b->stream[0] = b->stream[1] = -1;
	b->wend = INT64_MAX;
	b->wstart = INT64_MIN;
	b->t[0].cur = b->t[1].cur = INT64_MIN;
	b->quota = !strncasecmp(type, "audio/", 6) ? AUDIO_QUOTA : VIDEO_QUOTA;
	if (pthread_create(&b->thread, NULL, parse_thread, b)) {
		pthread_mutex_unlock(&p->lock);
		return -1;
	}
	b->used = 1;
	pthread_mutex_unlock(&p->lock);
	return i;
}

static struct sbuf *buffer_of(struct player *p, int id)
{
	return id >= 0 && id < MAX_BUFFERS && p->sb[id].used ? &p->sb[id] :
		NULL;
}

static int append(void *h, int id, const void *bytes, size_t len,
	double offset, double start, double end)
{
	struct player *p = h;
	struct sbuf *b;
	size_t n;
	int r = 0;

	pthread_mutex_lock(&p->lock);
	if (!(b = buffer_of(p, id))) {
		pthread_mutex_unlock(&p->lock);
		return SOIE_SOURCE_ERROR;
	}
	if (b->t[0].bytes + b->t[1].bytes + len > b->quota) {
		/* what was played goes first */
		int64_t limit = (int64_t)(clock_now(p) * 1e6) - KEEP;

		evict(&b->t[0], limit);
		evict(&b->t[1], limit);
		ranges_changed(b);
		if (b->t[0].bytes + b->t[1].bytes + len > b->quota) {
			pthread_mutex_unlock(&p->lock);
			return SOIE_SOURCE_FULL;
		}
	}
	if (b->len + len > b->cap) {
		unsigned char *f = realloc(b->fifo, b->len + len);

		if (!f) {
			pthread_mutex_unlock(&p->lock);
			return SOIE_SOURCE_FULL;
		}
		b->fifo = f;
		b->cap = b->len + len;
	}
	n = init_length(bytes, len);
	if (!n && !b->initlen) {
		/* the first segment must initialize */
		pthread_mutex_unlock(&p->lock);
		return SOIE_SOURCE_ERROR;
	}
	if (n) {
		/* kept to open the demuxer again (abort(), errors) */
		if (n <= INIT_MAX) {
			unsigned char *in = malloc(n);

			if (in) {
				memcpy(in, bytes, n);
				free(b->initseg);
				b->initseg = in;
				b->initlen = n;
			}
		}
		if (b->consumed)
			b->reset = 1;	/* the demuxer opens again */
		b->replay = 0;
	}
	memcpy(b->fifo + b->len, bytes, len);
	b->len += len;
	b->offset = (int64_t)(offset * 1e6);
	b->wstart = start <= -1e15 ? INT64_MIN : (int64_t)(start * 1e6);
	b->wend = end >= 1e15 ? INT64_MAX : (int64_t)(end * 1e6);
	b->run++;
	b->idle = 0;
	b->error = 0;
	pthread_cond_broadcast(&p->cond);
	/* parsing is quick: wait for it */
	while (!b->idle && !b->quit)
		pthread_cond_wait(&p->cond, &p->lock);
	if (b->error)
		r = SOIE_SOURCE_ERROR;
	ranges_changed(b);
	pthread_mutex_unlock(&p->lock);
	return r;
}

static int buffered(void *h, int id, double *out, int max)
{
	struct player *p = h;
	int64_t r[2 * MAX_RANGES];
	struct sbuf *b;
	int i, n = 0;

	pthread_mutex_lock(&p->lock);
	if (id < 0) {
		n = p->nranges;
		memcpy(r, p->ranges, (size_t)n * 2 * sizeof(*r));
	} else if ((b = buffer_of(p, id))) {
		n = buffer_ranges(b, r);
	}
	pthread_mutex_unlock(&p->lock);
	if (n > max)
		n = max;
	for (i = 0; i < 2 * n; i++)
		out[i] = (double)r[i] / 1e6;
	return n;
}

static void source(void *h, int op, int id, double a, double z)
{
	struct player *p = h;
	struct sbuf *b;
	int k, j;

	pthread_mutex_lock(&p->lock);
	b = buffer_of(p, id);
	switch (op) {
	case SOIE_SOURCE_REMOVE:
		if (!b)
			break;
		for (k = 0; k < 2; k++)
			remove_frames(&b->t[k], (int64_t)(a * 1e6),
				z >= 1e15 ? INT64_MAX : (int64_t)(z * 1e6), 0);
		ranges_changed(b);
		break;
	case SOIE_SOURCE_ABORT:
		if (!b)
			break;
		b->off = b->len = 0;
		b->reset = 1;
		b->replay = b->initlen;
		break;
	case SOIE_SOURCE_DROP:
		if (!b)
			break;
		b->quit = 1;
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->lock);
		pthread_join(b->thread, NULL);
		pthread_mutex_lock(&p->lock);
		for (k = 0; k < 2; k++) {
			for (j = 0; j < b->t[k].n; j++)
				drop_frame(&b->t[k], j);
			free(b->t[k].f);
			init_unref(b->init[k]);
		}
		free(b->fifo);
		free(b->initseg);
		memset(b, 0, sizeof(*b));
		player_ranges(p);
		p->changes++;
		pthread_cond_broadcast(&p->cond);
		break;
	case SOIE_SOURCE_DURATION:
		p->duration = a > 0 ? a : 0;
		p->changes++;
		break;
	case SOIE_SOURCE_END:
		p->eos = a != 0;
		p->changes++;
		pthread_cond_broadcast(&p->cond);
		break;
	}
	pthread_mutex_unlock(&p->lock);
}

static struct soie_ext_media hooks = {
	{ SOIE_EXT_ABI, sizeof(hooks), SOIE_EXT_MEDIA, "ffmpeg", NULL },
	can_play, open_player, close_player, play, pause_player, seek,
	volume, state, frame, can_source, open_source, add_buffer, append,
	buffered, source
};

int soie_ext_init(void)
{
	av_log_set_level(AV_LOG_QUIET);
	return soie_ext_register(&hooks.base);
}
