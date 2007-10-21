/**
 * \file src/stream/gl_play.c
 * \brief OpenGL playback
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007
 */

/* gl_play.c -- OpenGL stuff
 * Copyright (C) 2007 Pyry Haulos
 * For conditions of distribution and use, see copyright notice in glc.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>
#include <unistd.h>
#include <packetstream.h>
#include <pthread.h>
#include <errno.h>

#include "../common/glc.h"
#include "../common/util.h"
#include "../common/thread.h"
#include "gl_play.h"

/**
 * \addtogroup stream
 *  \{
 */

/**
 * \defgroup gl_play OpenGL playback
 *  \{
 */

struct gl_play_private_s {
	glc_t *glc;
	glc_thread_t play_thread;

	glc_ctx_i ctx_i;
	GLenum format;
	unsigned int w, h;
	glc_utime_t last, fps;

	Display *dpy;
	GLXDrawable drawable;
	GLXContext ctx;
	char name[100];
	int created;
	GLuint texture;

	Atom delete_atom, wm_proto_atom;

	int cancel;
	sem_t *finished;
};

int gl_play_read_callback(glc_thread_state_t *state);
void gl_play_finish_callback(void *ptr, int err);

int gl_play_create_ctx(struct gl_play_private_s *gl_play);
int gl_play_update_ctx(struct gl_play_private_s *gl_play);
int gl_play_update_viewport(struct gl_play_private_s *gl_play, unsigned int w, unsigned int h);

int gl_play_draw_picture(struct gl_play_private_s *gl_play, char *from);

int gl_play_handle_xevents(struct gl_play_private_s *gl_play, glc_thread_state_t *state);

int gl_play_init(glc_t *glc, ps_buffer_t *from, glc_ctx_i ctx, sem_t *finished)
{
	struct gl_play_private_s *gl_play = (struct gl_play_private_s *) malloc(sizeof(struct gl_play_private_s));
	memset(gl_play, 0, sizeof(struct gl_play_private_s));

	gl_play->glc = glc;
	gl_play->ctx_i = ctx;
	gl_play->finished = finished;

	gl_play->play_thread.flags = GLC_THREAD_READ;
	gl_play->play_thread.ptr = gl_play;
	gl_play->play_thread.read_callback = &gl_play_read_callback;
	gl_play->play_thread.finish_callback = &gl_play_finish_callback;
	gl_play->play_thread.threads = 1;

	gl_play->dpy = XOpenDisplay(NULL);

	if (!gl_play->dpy) {
		fprintf(stderr, "can't open display\n");
		return 1;
	}

	return glc_thread_create(glc, &gl_play->play_thread, from, NULL);
}

void gl_play_finish_callback(void *ptr, int err)
{
	struct gl_play_private_s *gl_play = (struct gl_play_private_s *) ptr;

	if (err)
		fprintf(stderr, "gl_play failed: %s (%d)\n", strerror(err), err);

	if (gl_play->created) {
		if (gl_play->texture)
			glDeleteTextures(1, &gl_play->texture);

		glXDestroyContext(gl_play->dpy, gl_play->ctx);
		XDestroyWindow(gl_play->dpy, gl_play->drawable);
	}

	XCloseDisplay(gl_play->dpy);

	sem_post(gl_play->finished);
	free(gl_play);
}

int gl_play_draw_picture(struct gl_play_private_s *gl_play, char *from)
{
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, gl_play->texture);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, gl_play->w, gl_play->h, 0, GL_BGR,
		     GL_UNSIGNED_BYTE, from);

	glBegin(GL_QUADS);
	glTexCoord2i(0, 0); glVertex2i(0, 0);
	glTexCoord2i(1, 0); glVertex2i(1, 0);
	glTexCoord2i(1, 1); glVertex2i(1, 1);
	glTexCoord2i(0, 1); glVertex2i(0, 1);
	glEnd();

	return 0;
}

int gl_play_create_ctx(struct gl_play_private_s *gl_play)
{
	int attribs[] = { GLX_RGBA,
			  GLX_RED_SIZE, 1,
			  GLX_GREEN_SIZE, 1,
			  GLX_BLUE_SIZE, 1,
			  GLX_DOUBLEBUFFER,
			  GLX_DEPTH_SIZE, 1,
			  None };
	XVisualInfo *visinfo;
	XSetWindowAttributes winattr;

	visinfo = glXChooseVisual(gl_play->dpy, DefaultScreen(gl_play->dpy), attribs);

	winattr.background_pixel = 0;
	winattr.border_pixel = 0;
	winattr.colormap = XCreateColormap(gl_play->dpy, RootWindow(gl_play->dpy, DefaultScreen(gl_play->dpy)),
	                                   visinfo->visual, AllocNone);
	winattr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask | KeyReleaseMask;
	winattr.override_redirect = 0;
	gl_play->drawable = XCreateWindow(gl_play->dpy, RootWindow(gl_play->dpy, DefaultScreen(gl_play->dpy)),
	                              0, 0, gl_play->w, gl_play->h, 0, visinfo->depth, InputOutput,
	                              visinfo->visual, CWBackPixel | CWBorderPixel |
	                              CWColormap | CWEventMask | CWOverrideRedirect, &winattr);

	gl_play->ctx = glXCreateContext(gl_play->dpy, visinfo, NULL, True);
	if (gl_play->ctx == NULL)
		return EAGAIN;

	gl_play->created = 1;

	XFree(visinfo);

	gl_play->delete_atom = XInternAtom(gl_play->dpy, "WM_DELETE_WINDOW", False);
	gl_play->wm_proto_atom = XInternAtom(gl_play->dpy, "WM_PROTOCOLS", True);
	XSetWMProtocols(gl_play->dpy, gl_play->drawable, &gl_play->delete_atom, 1);

	return gl_play_update_ctx(gl_play);
}

int gl_play_update_ctx(struct gl_play_private_s *gl_play)
{
	XSizeHints sizehints;

	if (!gl_play->created)
		return EINVAL;

	snprintf(gl_play->name, sizeof(gl_play->name) - 1, "glc-play (ctx %d)", gl_play->ctx_i);

	XUnmapWindow(gl_play->dpy, gl_play->drawable);

	sizehints.x = 0;
	sizehints.y = 0;
	sizehints.width = gl_play->w;
	sizehints.height = gl_play->h;
	sizehints.min_aspect.x = gl_play->w;
	sizehints.min_aspect.y = gl_play->h;
	sizehints.max_aspect.x = gl_play->w;
	sizehints.max_aspect.y = gl_play->h;
	sizehints.flags = USSize | USPosition | PAspect;
	XSetNormalHints(gl_play->dpy, gl_play->drawable, &sizehints);
	XSetStandardProperties(gl_play->dpy, gl_play->drawable, gl_play->name, gl_play->name, None,
	                       (char **)NULL, 0, &sizehints);
	XResizeWindow(gl_play->dpy, gl_play->drawable, gl_play->w, gl_play->h);

	XMapWindow(gl_play->dpy, gl_play->drawable);

	glXMakeCurrent(gl_play->dpy, gl_play->drawable, gl_play->ctx);

	return gl_play_update_viewport(gl_play, gl_play->w, gl_play->h);
}

int gl_play_update_viewport(struct gl_play_private_s *gl_play, unsigned int w, unsigned int h)
{
	glViewport(0, 0, (GLsizei) w, (GLsizei) h);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	if (!gl_play->texture) {
		glGenTextures(1, &gl_play->texture);

		glBindTexture(GL_TEXTURE_2D, gl_play->texture);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	}

	return 0;
}

int gl_handle_xevents(struct gl_play_private_s *gl_play, glc_thread_state_t *state)
{
	XEvent event;
	XConfigureEvent *ce;
	int code;

	while (XPending(gl_play->dpy) > 0) {
		XNextEvent(gl_play->dpy, &event);

		switch (event.type) {
		case KeyPress:
			code = XLookupKeysym(&event.xkey, 0);

			if (code == XK_Right)
				util_timediff(gl_play->glc, -100000);
			break;
		case KeyRelease:
			code = XLookupKeysym(&event.xkey, 0);

			if (code == XK_Escape)
				gl_play->glc->flags |= GLC_CANCEL;
			break;
		case DestroyNotify:
			state->flags |= GLC_THREAD_STOP;
			break;
		case ClientMessage:
			if (event.xclient.message_type == gl_play->wm_proto_atom) {
				if ((Atom) event.xclient.data.l[0] == gl_play->delete_atom)
					state->flags |= GLC_THREAD_STOP;
			}
			break;
		case ConfigureNotify:
			ce = (XConfigureEvent *) &event;
			gl_play_update_viewport(gl_play, ce->width, ce->height);

			break;
		}
	}

	return 0;
}

int gl_play_read_callback(glc_thread_state_t *state)
{
	struct gl_play_private_s *gl_play = (struct gl_play_private_s *) state->ptr;

	glc_ctx_message_t *ctx_msg;
	glc_picture_header_t *pic_hdr;
	glc_utime_t time;

	gl_handle_xevents(gl_play, state);

	if (state->flags & GLC_THREAD_STOP)
		return 0;

	if (state->header.type == GLC_MESSAGE_CTX) {
		ctx_msg = (glc_ctx_message_t *) state->read_data;
		if (ctx_msg->ctx != gl_play->ctx_i)
			return 0; /* just ignore it */

		gl_play->w = ctx_msg->w;
		gl_play->h = ctx_msg->h;

		if ((ctx_msg->flags & GLC_CTX_BGR) && (ctx_msg->flags & GLC_CTX_CREATE))
			gl_play_create_ctx(gl_play);
		else if ((ctx_msg->flags & GLC_CTX_BGR) && (ctx_msg->flags & GLC_CTX_UPDATE)) {
			if (gl_play_update_ctx(gl_play)) {
				fprintf(stderr, "broken ctx %d\n", ctx_msg->ctx);
				return EINVAL;
			}
		} else {
			fprintf(stderr, "ctx %d is in unsupported format\n", ctx_msg->ctx);
			return EINVAL;
		}
	} else if (state->header.type == GLC_MESSAGE_PICTURE) {
		pic_hdr = (glc_picture_header_t *) state->read_data;

		if (pic_hdr->ctx != gl_play->ctx_i)
			return 0;

		if (!gl_play->created) {
			fprintf(stderr, "picture refers to uninitalized ctx %d\n", pic_hdr->ctx);
			return EINVAL;
		}

		/* draw first, measure and sleep after */
		gl_play_draw_picture(gl_play, &state->read_data[GLC_PICTURE_HEADER_SIZE]);

		time = util_timestamp(gl_play->glc);

		if (pic_hdr->timestamp > time)
			usleep(pic_hdr->timestamp - time);
		else if (time > pic_hdr->timestamp + gl_play->fps)
			return 0;

		glXSwapBuffers(gl_play->dpy, gl_play->drawable);
	}

	return 0;
}

/**  \} */
/**  \} */