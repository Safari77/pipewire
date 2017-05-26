/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/eventfd.h>

#include "spa/list.h"
#include "spa/ringbuffer.h"
#include "pipewire/client/log.h"
#include "pipewire/server/main-loop.h"

struct impl {
	struct pw_main_loop this;

	bool running;
};

/**
 * pw_main_loop_new:
 *
 * Create a new #struct pw_main_loop.
 *
 * Returns: a new #struct pw_main_loop
 */
struct pw_main_loop *pw_main_loop_new(void)
{
	struct impl *impl;
	struct pw_main_loop *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	pw_log_debug("main-loop %p: new", impl);
	this = &impl->this;

	this->loop = pw_loop_new();
	if (this->loop == NULL)
		goto no_loop;

	pw_signal_init(&this->destroy_signal);

	return this;

      no_loop:
	free(impl);
	return NULL;
}

void pw_main_loop_destroy(struct pw_main_loop *loop)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, this);

	pw_log_debug("main-loop %p: destroy", impl);
	pw_signal_emit(&loop->destroy_signal, loop);

	pw_loop_destroy(loop->loop);

	free(impl);
}

/**
 * pw_main_loop_quit:
 * @loop: a #struct pw_main_loop
 *
 * Stop the running @loop.
 */
void pw_main_loop_quit(struct pw_main_loop *loop)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, this);
	pw_log_debug("main-loop %p: quit", impl);
	impl->running = false;
}

/**
 * pw_main_loop_run:
 * @loop: a #struct pw_main_loop
 *
 * Start running @loop. This function blocks until pw_main_loop_quit()
 * has been called.
 */
void pw_main_loop_run(struct pw_main_loop *loop)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, this);

	pw_log_debug("main-loop %p: run", impl);

	impl->running = true;
	pw_loop_enter(loop->loop);
	while (impl->running) {
		pw_loop_iterate(loop->loop, -1);
	}
	pw_loop_leave(loop->loop);
}
