/**
 * \file src/hook/alsa.c
 * \brief alsa wrapper
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007
 * For conditions of distribution and use, see copyright notice in glc.h
 */

/**
 * \addtogroup hook
 *  \{
 * \defgroup alsa alsa wrapper
 *  \{
 */

#include <dlfcn.h>
#include <elfhacks.h>
#include <alsa/asoundlib.h>

#include "../common/util.h"
#include "lib.h"
#include "../capture/audio_hook.h"
#include "../capture/audio_capture.h"

struct alsa_capture_stream_s {
	void *capture;
	char *device;
	unsigned int channels;
	unsigned int rate;

	struct alsa_capture_stream_s *next;
};

struct alsa_private_s {
	glc_t *glc;
	
	int started;
	int capture;

	void *audio_hook;

	struct alsa_capture_stream_s *capture_stream;

	void *libasound_handle;
	int (*snd_pcm_open)(snd_pcm_t **, const char *, snd_pcm_stream_t, int);
	snd_pcm_sframes_t (*snd_pcm_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
	snd_pcm_sframes_t (*snd_pcm_writen)(snd_pcm_t *, void **, snd_pcm_uframes_t);
	int (*snd_pcm_mmap_begin)(snd_pcm_t *, const snd_pcm_channel_area_t **,
				  snd_pcm_uframes_t *, snd_pcm_uframes_t *);
	snd_pcm_sframes_t (*snd_pcm_mmap_commit)(snd_pcm_t *, snd_pcm_uframes_t, snd_pcm_uframes_t);
};

__PRIVATE struct alsa_private_s alsa;
__PRIVATE int alsa_loaded = 0;

__PRIVATE void get_real_alsa();

__PRIVATE int alsa_parse_capture_cfg(const char *cfg);

int alsa_init(glc_t *glc)
{
	alsa.glc = glc;
	alsa.started = 0;
	alsa.capture_stream = NULL;

	util_log(alsa.glc, GLC_DEBUG, "alsa", "initializing");

	if (getenv("GLC_AUDIO"))
		alsa.capture = atoi(getenv("GLC_AUDIO"));
	else
		alsa.capture = 1;

	if (getenv("GLC_AUDIO_SKIP")) {
		if (atoi(getenv("GLC_AUDIO_SKIP")))
			alsa.glc->flags |= GLC_AUDIO_ALLOW_SKIP;
	}

	if (getenv("GLC_AUDIO_RECORD"))
		alsa_parse_capture_cfg(getenv("GLC_AUDIO_RECORD"));

	get_real_alsa();
	return 0;
}

int alsa_parse_capture_cfg(const char *cfg)
{
	struct alsa_capture_stream_s *stream;
	const char *args, *next, *device = cfg;
	unsigned int channels, rate;
	size_t len;

	while (device != NULL) {
		while (*device == ';')
			device++;
		if (*device == '\0')
			break;

		channels = 1;
		rate = 44100;

		/* check if some args have been given */
		if ((args = strstr(device, ",")))
			sscanf(args, ",%u,%u", &rate, &channels);
		next = strstr(device, ";");

		stream = malloc(sizeof(struct alsa_capture_stream_s));
		memset(stream, 0, sizeof(struct alsa_capture_stream_s));

		if (args)
			len = args - device;
		else if (next)
			len = next - device;
		else
			len = strlen(device);

		stream->device = (char *) malloc(sizeof(char) * len);
		memcpy(stream->device, device, len);
		stream->device[len] = '\0';

		stream->channels = channels;
		stream->rate = rate;
		stream->next = alsa.capture_stream;
		alsa.capture_stream = stream;

		device = next;
	}

	return 0;
}

int alsa_start(ps_buffer_t *buffer)
{
	struct alsa_capture_stream_s *stream = alsa.capture_stream;

	if (alsa.started)
		return EINVAL;

	/* make sure libasound.so does not call our hooked functions */
	alsa_unhook_so("*libasound.so*");

	if (alsa.capture) {
		if (!(alsa.audio_hook = audio_hook_init(alsa.glc, buffer)))
			return EAGAIN;
	}

	/* start capture streams */
	while (stream != NULL) {
		stream->capture = audio_capture_init(alsa.glc, buffer, stream->device,
						     stream->rate, stream->channels);
		stream = stream->next;
	}

	alsa.started = 1;
	return 0;
}

int alsa_close()
{
	struct alsa_capture_stream_s *del;

	if (!alsa.started)
		return 0;

	util_log(alsa.glc, GLC_DEBUG, "alsa", "closing");

	if (alsa.capture)
		audio_hook_close(alsa.audio_hook);

	while (alsa.capture_stream != NULL) {
		del = alsa.capture_stream;
		alsa.capture_stream = alsa.capture_stream->next;

		if (del->capture)
			audio_capture_close(del->capture);

		free(del->device);
		free(del);
	}

	return 0;
}

int alsa_pause()
{
	struct alsa_capture_stream_s *stream = alsa.capture_stream;

	while (stream != NULL) {
		if (stream->capture)
			audio_capture_pause(stream->capture);
		stream = stream->next;
	}

	return 0;
}

int alsa_resume()
{
	struct alsa_capture_stream_s *stream = alsa.capture_stream;

	while (stream != NULL) {
		if (stream->capture)
			audio_capture_resume(stream->capture);
		stream = stream->next;
	}

	return 0;
}

void get_real_alsa()
{
	if (!lib.dlopen)
		get_real_dlsym();

	if (alsa_loaded)
		return;

	alsa.libasound_handle = lib.dlopen("libasound.so", RTLD_LAZY);
	if (!alsa.libasound_handle)
		goto err;
	alsa.snd_pcm_open =
	  (int (*)(snd_pcm_t **, const char *, snd_pcm_stream_t, int))
	    lib.dlsym(alsa.libasound_handle, "snd_pcm_open");
	if (!alsa.snd_pcm_open)
		goto err;
	alsa.snd_pcm_writei =
	  (snd_pcm_sframes_t (*)(snd_pcm_t *, const void *, snd_pcm_uframes_t))
	    lib.dlsym(alsa.libasound_handle, "snd_pcm_writei");
	if (!alsa.snd_pcm_writei)
		goto err;
	alsa.snd_pcm_writen =
	  (snd_pcm_sframes_t (*)(snd_pcm_t *, void **, snd_pcm_uframes_t))
	    lib.dlsym(alsa.libasound_handle, "snd_pcm_writen");
	if (!alsa.snd_pcm_writen)
		goto err;
	alsa.snd_pcm_mmap_begin =
	  (int (*)(snd_pcm_t *, const snd_pcm_channel_area_t **, snd_pcm_uframes_t *, snd_pcm_uframes_t *))
	    lib.dlsym(alsa.libasound_handle, "snd_pcm_mmap_begin");
	if (!alsa.snd_pcm_mmap_begin)
		goto err;
	alsa.snd_pcm_mmap_commit =
	  (snd_pcm_sframes_t (*)(snd_pcm_t *, snd_pcm_uframes_t, snd_pcm_uframes_t))
	    lib.dlsym(alsa.libasound_handle, "snd_pcm_mmap_commit");
	if (!alsa.snd_pcm_mmap_commit)
		goto err;

	alsa_loaded = 1;
	return;
err:
	fprintf(stderr, "(glc:alsa) can't get real alsa");
	exit(1);
}

int alsa_unhook_so(const char *soname)
{
	int ret;
	eh_obj_t so;

	if (!alsa_loaded)
		get_real_alsa(); /* make sure we have real functions */

	if ((ret = eh_find_obj(&so, soname)))
		return ret;

	/* don't look at 'elfhacks'... contains some serious black magic */
	eh_set_rel(&so, "snd_pcm_writei", alsa.snd_pcm_writei);
	eh_set_rel(&so, "snd_pcm_writen", alsa.snd_pcm_writen);
	eh_set_rel(&so, "snd_pcm_mmap_begin", alsa.snd_pcm_mmap_begin);
	eh_set_rel(&so, "snd_pcm_mmap_commit", alsa.snd_pcm_mmap_commit);
	eh_set_rel(&so, "dlsym", lib.dlsym);
	eh_set_rel(&so, "dlvsym", lib.dlvsym);

	eh_destroy_obj(&so);

	return 0;
}

__PUBLIC int snd_pcm_open(snd_pcm_t **pcmp, const char *name, snd_pcm_stream_t stream, int mode)
{
	return __alsa_snd_pcm_open(pcmp, name, stream, mode);
}

int __alsa_snd_pcm_open(snd_pcm_t **pcmp, const char *name, snd_pcm_stream_t stream, int mode)
{
	/* it is not necessarily safe to call glc_init() from write funcs
	   especially async mode (initiated from signal) is troublesome */
	INIT_GLC
	return alsa.snd_pcm_open(pcmp, name, stream, mode);
}

__PUBLIC snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
	return __alsa_snd_pcm_writei(pcm, buffer, size);
}

snd_pcm_sframes_t __alsa_snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
	INIT_GLC
	snd_pcm_sframes_t ret = alsa.snd_pcm_writei(pcm, buffer, size);
	if ((alsa.capture) && (ret > 0) && (alsa.glc->flags & GLC_CAPTURE))
		audio_hook_alsa_i(alsa.audio_hook, pcm, buffer, ret);
	return ret;
}

__PUBLIC snd_pcm_sframes_t snd_pcm_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
	return __alsa_snd_pcm_writen(pcm, bufs, size);
}

snd_pcm_sframes_t __alsa_snd_pcm_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
	INIT_GLC
	snd_pcm_sframes_t ret = alsa.snd_pcm_writen(pcm, bufs, size);
	if ((alsa.capture) && (ret > 0) && (alsa.glc->flags & GLC_CAPTURE))
		audio_hook_alsa_n(alsa.audio_hook, pcm, bufs, ret);
	return ret;
}

__PUBLIC int snd_pcm_mmap_begin(snd_pcm_t *pcm, const snd_pcm_channel_area_t **areas, snd_pcm_uframes_t *offset, snd_pcm_uframes_t *frames)
{
	return __alsa_snd_pcm_mmap_begin(pcm, areas, offset, frames);
}

int __alsa_snd_pcm_mmap_begin(snd_pcm_t *pcm, const snd_pcm_channel_area_t **areas, snd_pcm_uframes_t *offset, snd_pcm_uframes_t *frames)
{
	INIT_GLC
	int ret = alsa.snd_pcm_mmap_begin(pcm, areas, offset, frames);
	if ((alsa.capture) && (ret >= 0) && (alsa.glc->flags & GLC_CAPTURE))
		audio_hook_alsa_mmap_begin(alsa.audio_hook, pcm, *areas, *offset, *frames);
	return ret;
}

__PUBLIC snd_pcm_sframes_t snd_pcm_mmap_commit(snd_pcm_t *pcm, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames)
{
	return __alsa_snd_pcm_mmap_commit(pcm, offset, frames);
}

snd_pcm_sframes_t __alsa_snd_pcm_mmap_commit(snd_pcm_t *pcm, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames)
{
	INIT_GLC
	snd_pcm_uframes_t ret;
	if (alsa.capture && (alsa.glc->flags & GLC_CAPTURE))
		audio_hook_alsa_mmap_commit(alsa.audio_hook, pcm, offset,  frames);

	ret = alsa.snd_pcm_mmap_commit(pcm, offset, frames);
	if (ret != frames)
		util_log(alsa.glc, GLC_WARNING, "alsa", "frames=%lu, ret=%ld", frames, ret);
	return ret;
}

/**  \} */
/**  \} */
