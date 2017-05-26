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

#ifndef __PIPEWIRE_TRANSPORT_H__
#define __PIPEWIRE_TRANSPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>

#include <spa/defs.h>
#include <spa/node.h>

#include <pipewire/client/mem.h>
#include <pipewire/client/sig.h>

struct pw_transport_info {
	int memfd;
	uint32_t offset;
	uint32_t size;
};

/**
 * struct pw_transport_area:
 *
 * Shared structure between client and server
 */
struct pw_transport_area {
	uint32_t max_inputs;
	uint32_t n_inputs;
	uint32_t max_outputs;
	uint32_t n_outputs;
};

struct pw_transport {
	PW_SIGNAL(destroy_signal, (struct pw_listener * listener, struct pw_transport * trans));

	struct pw_transport_area *area;
	struct spa_port_io *inputs;
	struct spa_port_io *outputs;
	void *input_data;
	struct spa_ringbuffer *input_buffer;
	void *output_data;
	struct spa_ringbuffer *output_buffer;
};

struct pw_transport *
pw_transport_new(uint32_t max_inputs, uint32_t max_outputs);

struct pw_transport *
pw_transport_new_from_info(struct pw_transport_info *info);

void
pw_transport_destroy(struct pw_transport *trans);

int
pw_transport_get_info(struct pw_transport *trans, struct pw_transport_info *info);

int
pw_transport_add_event(struct pw_transport *trans, struct spa_event *event);

int
pw_transport_next_event(struct pw_transport *trans, struct spa_event *event);

int
pw_transport_parse_event(struct pw_transport *trans, void *event);

#define PIPEWIRE_TYPE_EVENT__Transport            SPA_TYPE_EVENT_BASE "Transport"
#define PIPEWIRE_TYPE_EVENT_TRANSPORT_BASE        PIPEWIRE_TYPE_EVENT__Transport ":"

#define PIPEWIRE_TYPE_EVENT_TRANSPORT__HaveOutput       PIPEWIRE_TYPE_EVENT_TRANSPORT_BASE "HaveOutput"
#define PIPEWIRE_TYPE_EVENT_TRANSPORT__NeedInput        PIPEWIRE_TYPE_EVENT_TRANSPORT_BASE "NeedInput"
#define PIPEWIRE_TYPE_EVENT_TRANSPORT__ReuseBuffer      PIPEWIRE_TYPE_EVENT_TRANSPORT_BASE "ReuseBuffer"

struct pw_type_event_transport {
	uint32_t HaveOutput;
	uint32_t NeedInput;
	uint32_t ReuseBuffer;
};

static inline void
pw_type_event_transport_map(struct spa_type_map *map, struct pw_type_event_transport *type)
{
	if (type->HaveOutput == 0) {
		type->HaveOutput = spa_type_map_get_id(map, PIPEWIRE_TYPE_EVENT_TRANSPORT__HaveOutput);
		type->NeedInput = spa_type_map_get_id(map, PIPEWIRE_TYPE_EVENT_TRANSPORT__NeedInput);
		type->ReuseBuffer = spa_type_map_get_id(map, PIPEWIRE_TYPE_EVENT_TRANSPORT__ReuseBuffer);
	}
}

struct pw_event_transport_reuse_buffer_body {
	struct spa_pod_object_body body;
	struct spa_pod_int port_id;
	struct spa_pod_int buffer_id;
};

struct pw_event_transport_reuse_buffer {
	struct spa_pod pod;
	struct pw_event_transport_reuse_buffer_body body;
};

#define PW_EVENT_TRANSPORT_REUSE_BUFFER_INIT(type,port_id,buffer_id)		\
	SPA_EVENT_INIT_COMPLEX(struct pw_event_transport_reuse_buffer,		\
		sizeof(struct pw_event_transport_reuse_buffer_body), type,	\
		SPA_POD_INT_INIT(port_id),					\
		SPA_POD_INT_INIT(buffer_id))


#ifdef __cplusplus
}				/* extern "C" */
#endif

#endif /* __PIPEWIRE_TRANSPORT_H__ */
