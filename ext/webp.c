/*
 * WebP images, decoded by libwebp. Animated WebP needs the demuxer
 * library and is left out: the decoder refuses it.
 */
#include <stdlib.h>
#include <string.h>
#include <decode.h>	/* pkg-config gives .../include/webp */

#include <soie/ext.h>

static unsigned char *decode(void *data, const unsigned char *bytes,
	size_t len, int *w, int *h)
{
	uint8_t *px, *out;
	size_t size;

	(void)data;
	if (len < 16 || memcmp(bytes, "RIFF", 4) ||
	    memcmp(bytes + 8, "WEBP", 4))
		return NULL;
	px = WebPDecodeRGBA(bytes, len, w, h);
	if (!px)
		return NULL;
	/* libwebp's buffer must go back through WebPFree() */
	size = (size_t)*w * (size_t)*h * 4;
	out = malloc(size);
	if (out)
		memcpy(out, px, size);
	WebPFree(px);
	return out;
}

static struct soie_ext_image hooks = {
	{ SOIE_EXT_ABI, sizeof(hooks), SOIE_EXT_IMAGE, "webp", NULL },
	decode
};

int soie_ext_init(void)
{
	return soie_ext_register(&hooks.base);
}
