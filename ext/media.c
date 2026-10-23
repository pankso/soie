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
 * SOIE_AUDIO=none plays without sound (tests, machines without a
 * sound card), SOIE_AUDIO=device picks the ALSA device ("default").
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
	double pts;
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
	double shown_pts;
	int quarter;		/* time in quarters, for repaints */
	/* sound */
	int16_t *pcm;
	int pcm_head, pcm_count, rate;
	int flush;		/* the output drops what it has */
	int64_t played;		/* frames given to ALSA since base */
	double audio_clock;	/* -1 while the sound has not started */
	double volume;
	int muted;
	snd_pcm_t *dev;
	/* the decoding thread's own */
	AVFormatContext *fmt;
	AVCodecContext *vdec, *adec;
	int vs, as;
	struct SwsContext *sws;
	SwrContext *swr;
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
	if (p->audio_clock >= 0)
		return p->audio_clock;
	return p->base + now() - p->started;
}

/* Take the frame due now; count changes for repaints. */
static void advance(struct player *p)
{
	double t = clock_now(p);
	struct frame *f;
	unsigned char *swap;
	int q;

	while (p->count) {
		f = &p->ring[p->head];
		if (f->pts > t + 0.01 && p->shown)
			break;
		/* the next one is due too: this one is late, skip it */
		swap = p->shown;
		p->shown = f->rgba;
		p->shown_pts = f->pts;
		f->rgba = swap;
		p->head = (p->head + 1) % NFRAMES;
		p->count--;
		p->changes++;
		pthread_cond_broadcast(&p->cond);
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

static void push_video(struct player *p, AVFrame *fr)
{
	AVRational tb = p->fmt->streams[p->vs]->time_base;
	int64_t ts = fr->best_effort_timestamp;
	struct frame *f;
	uint8_t *dst[4];
	int linesize[4], w = p->width, h = p->height;

	if (!p->sws)
		p->sws = sws_getContext(fr->width, fr->height, fr->format, w, h,
			AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
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
	if (!f->rgba)
		f->rgba = malloc((size_t)w * (size_t)h * 4);
	if (!f->rgba)
		return;
	dst[0] = f->rgba;
	dst[1] = dst[2] = dst[3] = NULL;
	linesize[0] = w * 4;
	linesize[1] = linesize[2] = linesize[3] = 0;
	sws_scale(p->sws, (const uint8_t *const *)fr->data, fr->linesize, 0,
		fr->height, dst, linesize);
	f->pts = ts == AV_NOPTS_VALUE ? 0 : (double)ts * av_q2d(tb);
	pthread_mutex_lock(&p->lock);
	p->count++;
	pthread_mutex_unlock(&p->lock);
}

static void push_audio(struct player *p, AVFrame *fr)
{
	int16_t tmp[2 * 4096];
	uint8_t *out = (uint8_t *)tmp;
	int n, i, k;

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

static AVCodecContext *open_decoder(AVFormatContext *fmt, int i)
{
	AVCodecParameters *par = fmt->streams[i]->codecpar;
	AVCodec *codec = avcodec_find_decoder(par->codec_id);
	AVCodecContext *c;

	if (!codec || !(c = avcodec_alloc_context3(codec)))
		return NULL;
	c->thread_count = 1;
	if (avcodec_parameters_to_context(c, par) < 0 ||
	    avcodec_open2(c, codec, NULL) < 0) {
		avcodec_free_context(&c);
		return NULL;
	}
	return c;
}

static void start_audio(struct player *p);

static int open_media(struct player *p)
{
	AVIOContext *io;
	unsigned char *iobuf = av_malloc(65536);
	double scale;
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
	if (p->as >= 0) {
		int64_t layout = p->adec->channel_layout ?
			(int64_t)p->adec->channel_layout :
			av_get_default_channel_layout(p->adec->channels);

		p->rate = p->adec->sample_rate > 0 &&
			p->adec->sample_rate <= 96000 ? p->adec->sample_rate :
			48000;
		p->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO,
			AV_SAMPLE_FMT_S16, p->rate, layout, p->adec->sample_fmt,
			p->adec->sample_rate, 0, NULL);
		if (!p->swr || swr_init(p->swr) < 0) {
			swr_free(&p->swr);
			avcodec_free_context(&p->adec);
			p->as = -1;
		}
	}
	pthread_mutex_lock(&p->lock);
	if (p->vs >= 0) {
		w = p->vdec->width;
		h = p->vdec->height;
		if (w > 0 && h > 0 && (long long)w * h > MAX_PIXELS) {
			scale = sqrt((double)MAX_PIXELS / ((double)w * h));
			w = (int)(w * scale) & ~1;
			h = (int)(h * scale) & ~1;
		}
		p->width = w > 0 ? w : 0;
		p->height = h > 0 ? h : 0;
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
	p->changes++;
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
		while (!p->quit && (!p->playing || !p->pcm_count) && !p->flush)
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
		if (p->rate)
			p->audio_clock = p->base + (double)(p->played - delay) /
				p->rate;
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
	int i;

	pthread_mutex_lock(&p->lock);
	p->quit = 1;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);
	pthread_join(p->thread, NULL);
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
	st->time = clock_now(p);
	if (p->duration > 0 && st->time > p->duration)
		st->time = p->duration;
	st->paused = !p->playing;
	st->ended = p->ended;
	st->width = p->width;
	st->height = p->height;
	st->error = p->error;
	st->changes = p->changes;
	pthread_mutex_unlock(&p->lock);
}

static const unsigned char *frame(void *h, int *w, int *hh)
{
	struct player *p = h;
	const unsigned char *f;

	pthread_mutex_lock(&p->lock);
	advance(p);
	f = p->shown;
	*w = p->width;
	*hh = p->height;
	pthread_mutex_unlock(&p->lock);
	return f;
}

static struct soie_ext_media hooks = {
	{ SOIE_EXT_ABI, sizeof(hooks), SOIE_EXT_MEDIA, "ffmpeg", NULL },
	can_play, open_player, close_player, play, pause_player, seek,
	volume, state, frame
};

int soie_ext_init(void)
{
	av_log_set_level(AV_LOG_QUIET);
	return soie_ext_register(&hooks.base);
}
