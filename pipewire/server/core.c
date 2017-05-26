/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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
#include <time.h>

#include <pipewire/client/pipewire.h>
#include <pipewire/client/interfaces.h>
#include <pipewire/server/core.h>
#include <pipewire/server/data-loop.h>
#include <pipewire/server/client-node.h>
#include <spa/lib/debug.h>
#include <spa/format-utils.h>

struct global_impl {
	struct pw_global this;
	pw_bind_func_t bind;
};

struct impl {
	struct pw_core this;

	struct spa_support support[4];
};

#define ACCESS_VIEW_GLOBAL(client,global) (client->core->access == NULL || \
					   client->core->access->view_global (client->core->access, \
									      client, global) == SPA_RESULT_OK)

static void registry_bind(void *object, uint32_t id, uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_core *core = resource->core;
	struct pw_global *global;

	spa_list_for_each(global, &core->global_list, link)
	if (global->id == id)
		break;

	if (&global->link == &core->global_list)
		goto no_id;

	if (!ACCESS_VIEW_GLOBAL(client, global))
		 goto no_id;

	pw_log_debug("global %p: bind object id %d to %d", global, id, new_id);
	pw_global_bind(global, client, 0, new_id);

	return;

      no_id:
	pw_log_debug("registry %p: no global with id %u to bind to %u", resource, id, new_id);
	/* unmark the new_id the map, the client does not yet know about the failed
	 * bind and will choose the next id, which we would refuse when we don't mark
	 * new_id as 'used and freed' */
	pw_map_insert_at(&client->objects, new_id, NULL);
	pw_core_notify_remove_id(client->core_resource, new_id);
	return;
}

static struct pw_registry_methods registry_methods = {
	&registry_bind
};

static void destroy_registry_resource(void *object)
{
	struct pw_resource *resource = object;
	spa_list_remove(&resource->link);
}

static void core_client_update(void *object, const struct spa_dict *props)
{
	struct pw_resource *resource = object;

	pw_client_update_properties(resource->client, props);
}

static void core_sync(void *object, uint32_t seq)
{
	struct pw_resource *resource = object;

	pw_core_notify_done(resource, seq);
}

static void core_get_registry(void *object, uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_core *this = resource->core;
	struct pw_global *global;
	struct pw_resource *registry_resource;

	registry_resource = pw_resource_new(client,
					    new_id,
					    this->type.registry, this, destroy_registry_resource);
	if (registry_resource == NULL)
		goto no_mem;

	registry_resource->implementation = &registry_methods;

	spa_list_insert(this->registry_resource_list.prev, &registry_resource->link);

	spa_list_for_each(global, &this->global_list, link) {
		if (ACCESS_VIEW_GLOBAL(client, global))
			 pw_registry_notify_global(registry_resource,
						   global->id,
						   spa_type_map_get_type(this->type.map,
									 global->type));
	}

	return;

      no_mem:
	pw_log_error("can't create registry resource");
	pw_core_notify_error(client->core_resource,
			     resource->id, SPA_RESULT_NO_MEMORY, "no memory");
}

static void
core_create_node(void *object,
		 const char *factory_name,
		 const char *name, const struct spa_dict *props, uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;

	pw_core_notify_error(client->core_resource,
			     resource->id, SPA_RESULT_NOT_IMPLEMENTED, "not implemented");
}

struct access_create_client_node {
	struct pw_access_data data;
	char *name;
	struct pw_properties *properties;
	uint32_t new_id;
	bool async;
};

static void *async_create_client_node_copy(struct pw_access_data *data, size_t size)
{
	struct access_create_client_node *d;

	d = calloc(1, sizeof(struct access_create_client_node) + size);
	memcpy(d, data, sizeof(struct access_create_client_node));
	d->name = strdup(d->name);
	d->async = true;
	d->data.user_data = SPA_MEMBER(d, sizeof(struct access_create_client_node), void);
	return d;
}

static void async_create_client_node_free(struct pw_access_data *data)
{
	struct access_create_client_node *d = (struct access_create_client_node *) data;

	if (d->async) {
		if (d->data.free_cb)
			d->data.free_cb(&d->data);
		free(d->name);
		free(d);
	}
}

static void async_create_client_node_complete(struct pw_access_data *data)
{
	struct access_create_client_node *d = (struct access_create_client_node *) data;
	struct pw_resource *resource = d->data.resource;
	struct pw_client *client = resource->client;
	struct pw_client_node *node;
	int res;
	int readfd, writefd;

	if (data->res != SPA_RESULT_OK)
		goto denied;

	node = pw_client_node_new(client, d->new_id, d->name, d->properties);
	if (node == NULL)
		goto no_mem;

	if ((res = pw_client_node_get_fds(node, &readfd, &writefd)) < 0) {
		pw_core_notify_error(client->core_resource,
				     resource->id, SPA_RESULT_ERROR, "can't get data fds");
		return;
	}

	pw_client_node_notify_done(node->resource, readfd, writefd);
	goto done;

      no_mem:
	pw_log_error("can't create client node");
	pw_core_notify_error(client->core_resource,
			     resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	goto done;
      denied:
	pw_log_error("create client node refused");
	pw_core_notify_error(client->core_resource,
			     resource->id, SPA_RESULT_NO_PERMISSION, "operation not allowed");
      done:
	async_create_client_node_free(&d->data);
	return;
}

static void
core_create_client_node(void *object,
			const char *name, const struct spa_dict *props, uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	int i;
	struct pw_properties *properties;
	struct access_create_client_node access_data;
	int res;

	properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	for (i = 0; i < props->n_items; i++) {
		pw_properties_set(properties, props->items[i].key, props->items[i].value);
	}

	access_data.data.resource = resource;
	access_data.data.async_copy = async_create_client_node_copy;
	access_data.data.complete_cb = async_create_client_node_complete;
	access_data.data.free_cb = NULL;
	access_data.name = (char *) name;
	access_data.properties = properties;
	access_data.new_id = new_id;
	access_data.async = false;

	if (client->core->access) {
		access_data.data.res = SPA_RESULT_NO_PERMISSION;
		res = client->core->access->create_client_node(client->core->access,
							       &access_data.data, name, properties);
	} else {
		res = access_data.data.res = SPA_RESULT_OK;
	}
	if (!SPA_RESULT_IS_ASYNC(res))
		async_create_client_node_complete(&access_data.data);
	return;

      no_mem:
	pw_log_error("can't create client node");
	pw_core_notify_error(client->core_resource,
			     resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	return;
}

static void core_update_types(void *object, uint32_t first_id, uint32_t n_types, const char **types)
{
	struct pw_resource *resource = object;
	struct pw_core *this = resource->core;
	struct pw_client *client = resource->client;
	int i;

	for (i = 0; i < n_types; i++, first_id++) {
		uint32_t this_id = spa_type_map_get_id(this->type.map, types[i]);
		if (!pw_map_insert_at(&client->types, first_id, PW_MAP_ID_TO_PTR(this_id)))
			pw_log_error("can't add type for client");
	}
}

static struct pw_core_methods core_methods = {
	&core_client_update,
	&core_sync,
	&core_get_registry,
	&core_create_node,
	&core_create_client_node,
	&core_update_types
};

static void core_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	resource->client->core_resource = NULL;
	spa_list_remove(&resource->link);
}

static int
core_bind_func(struct pw_global *global, struct pw_client *client, uint32_t version, uint32_t id)
{
	struct pw_core *this = global->object;
	struct pw_resource *resource;

	resource = pw_resource_new(client, id, global->type, global->object, core_unbind_func);
	if (resource == NULL)
		goto no_mem;

	resource->implementation = &core_methods;

	spa_list_insert(this->resource_list.prev, &resource->link);
	client->core_resource = resource;

	pw_log_debug("core %p: bound to %d", global->object, resource->id);

	this->info.change_mask = PW_CORE_CHANGE_MASK_ALL;
	pw_core_notify_info(resource, &this->info);

	return SPA_RESULT_OK;

      no_mem:
	pw_log_error("can't create core resource");
	return SPA_RESULT_NO_MEMORY;
}

struct pw_core *pw_core_new(struct pw_main_loop *main_loop, struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_core *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->data_loop = pw_data_loop_new();
	if (this->data_loop == NULL)
		goto no_data_loop;

	this->main_loop = main_loop;
	this->properties = properties;

	pw_type_init(&this->type);
	pw_map_init(&this->objects, 128, 32);

	impl->support[0].type = SPA_TYPE__TypeMap;
	impl->support[0].data = this->type.map;
	impl->support[1].type = SPA_TYPE__Log;
	impl->support[1].data = pw_log_get();
	impl->support[2].type = SPA_TYPE_LOOP__DataLoop;
	impl->support[2].data = this->data_loop->loop->loop;
	impl->support[3].type = SPA_TYPE_LOOP__MainLoop;
	impl->support[3].data = this->main_loop->loop->loop;
	this->support = impl->support;
	this->n_support = 4;

	pw_data_loop_start(this->data_loop);

	spa_list_init(&this->resource_list);
	spa_list_init(&this->registry_resource_list);
	spa_list_init(&this->global_list);
	spa_list_init(&this->client_list);
	spa_list_init(&this->node_list);
	spa_list_init(&this->node_factory_list);
	spa_list_init(&this->link_list);
	pw_signal_init(&this->destroy_signal);
	pw_signal_init(&this->global_added);
	pw_signal_init(&this->global_removed);

	pw_core_add_global(this, NULL, this->type.core, 0, this, core_bind_func, &this->global);

	this->info.id = this->global->id;
	this->info.change_mask = 0;
	this->info.user_name = pw_get_user_name();
	this->info.host_name = pw_get_host_name();
	this->info.version = "0";
	this->info.name = "pipewire-0";
	srandom(time(NULL));
	this->info.cookie = random();
	this->info.props = this->properties ? &this->properties->dict : NULL;

	return this;

      no_data_loop:
	free(impl);
	return NULL;
}

void pw_core_destroy(struct pw_core *core)
{
	struct impl *impl = SPA_CONTAINER_OF(core, struct impl, this);

	pw_log_debug("core %p: destroy", core);
	pw_signal_emit(&core->destroy_signal, core);

	pw_data_loop_destroy(core->data_loop);

	pw_map_clear(&core->objects);

	pw_log_debug("core %p: free", core);
	free(impl);
}

bool
pw_core_add_global(struct pw_core *core,
		   struct pw_client *owner,
		   uint32_t type,
		   uint32_t version, void *object, pw_bind_func_t bind, struct pw_global **global)
{
	struct global_impl *impl;
	struct pw_global *this;
	struct pw_resource *registry;
	const char *type_name;

	impl = calloc(1, sizeof(struct global_impl));
	if (impl == NULL)
		return false;

	this = &impl->this;
	impl->bind = bind;

	this->core = core;
	this->owner = owner;
	this->type = type;
	this->version = version;
	this->object = object;
	*global = this;

	pw_signal_init(&this->destroy_signal);

	this->id = pw_map_insert_new(&core->objects, this);

	spa_list_insert(core->global_list.prev, &this->link);
	pw_signal_emit(&core->global_added, core, this);

	type_name = spa_type_map_get_type(core->type.map, this->type);
	pw_log_debug("global %p: new %u %s, owner %p", this, this->id, type_name, owner);

	spa_list_for_each(registry, &core->registry_resource_list, link)
	    if (ACCESS_VIEW_GLOBAL(registry->client, this))
		pw_registry_notify_global(registry, this->id, type_name);

	return true;
}

int
pw_global_bind(struct pw_global *global, struct pw_client *client, uint32_t version, uint32_t id)
{
	int res;
	struct global_impl *impl = SPA_CONTAINER_OF(global, struct global_impl, this);

	if (impl->bind) {
		res = impl->bind(global, client, version, id);
	} else {
		res = SPA_RESULT_NOT_IMPLEMENTED;
		pw_core_notify_error(client->core_resource,
				     client->core_resource->id, res, "can't bind object id %d", id);
	}
	return res;
}

void pw_global_destroy(struct pw_global *global)
{
	struct pw_core *core = global->core;
	struct pw_resource *registry;

	pw_log_debug("global %p: destroy %u", global, global->id);
	pw_signal_emit(&global->destroy_signal, global);

	spa_list_for_each(registry, &core->registry_resource_list, link)
	    if (ACCESS_VIEW_GLOBAL(registry->client, global))
		 pw_registry_notify_global_remove(registry, global->id);

	pw_map_remove(&core->objects, global->id);

	spa_list_remove(&global->link);
	pw_signal_emit(&core->global_removed, core, global);

	pw_log_debug("global %p: free", global);
	free(global);
}

void pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict)
{
	struct pw_resource *resource;

	if (core->properties == NULL) {
		if (dict)
			core->properties = pw_properties_new_dict(dict);
	} else if (dict != &core->properties->dict) {
		uint32_t i;

		for (i = 0; i < dict->n_items; i++)
			pw_properties_set(core->properties,
					  dict->items[i].key, dict->items[i].value);
	}

	core->info.change_mask = PW_CORE_CHANGE_MASK_PROPS;
	core->info.props = core->properties ? &core->properties->dict : NULL;

	spa_list_for_each(resource, &core->resource_list, link) {
		pw_core_notify_info(resource, &core->info);
	}
}

struct pw_port *pw_core_find_port(struct pw_core *core,
				  struct pw_port *other_port,
				  uint32_t id,
				  struct pw_properties *props,
				  uint32_t n_format_filters,
				  struct spa_format **format_filters, char **error)
{
	struct pw_port *best = NULL;
	bool have_id;
	struct pw_node *n;

	have_id = id != SPA_ID_INVALID;

	pw_log_debug("id \"%u\", %d", id, have_id);

	spa_list_for_each(n, &core->node_list, link) {
		if (n->global == NULL)
			continue;

		pw_log_debug("node id \"%d\"", n->global->id);

		if (have_id) {
			if (n->global->id == id) {
				pw_log_debug("id \"%u\" matches node %p", id, n);

				best =
				    pw_node_get_free_port(n,
							  pw_direction_reverse(other_port->
									       direction));
				if (best)
					break;
			}
		} else {
			struct pw_port *p, *pin, *pout;

			p = pw_node_get_free_port(n, pw_direction_reverse(other_port->direction));
			if (p == NULL)
				continue;

			if (p->direction == PW_DIRECTION_OUTPUT) {
				pin = other_port;
				pout = p;
			} else {
				pin = p;
				pout = other_port;
			}

			if (pw_core_find_format(core,
						pout,
						pin,
						props,
						n_format_filters, format_filters, error) == NULL)
				continue;

			best = p;
		}
	}
	if (best == NULL) {
		asprintf(error, "No matching Node found");
	}
	return best;
}

struct spa_format *pw_core_find_format(struct pw_core *core,
				       struct pw_port *output,
				       struct pw_port *input,
				       struct pw_properties *props,
				       uint32_t n_format_filters,
				       struct spa_format **format_filterss, char **error)
{
	uint32_t out_state, in_state;
	int res;
	struct spa_format *filter = NULL, *format;
	uint32_t iidx = 0, oidx = 0;

	out_state = output->state;
	in_state = input->state;

	pw_log_debug("core %p: finding best format %d %d", core, out_state, in_state);

	if (out_state > PW_PORT_STATE_CONFIGURE && output->node->state == PW_NODE_STATE_IDLE)
		out_state = PW_PORT_STATE_CONFIGURE;
	if (in_state > PW_PORT_STATE_CONFIGURE && input->node->state == PW_NODE_STATE_IDLE)
		in_state = PW_PORT_STATE_CONFIGURE;

	if (in_state == PW_PORT_STATE_CONFIGURE && out_state > PW_PORT_STATE_CONFIGURE) {
		/* only input needs format */
		if ((res = spa_node_port_get_format(output->node->node,
						    SPA_DIRECTION_OUTPUT,
						    output->port_id,
						    (const struct spa_format **) &format)) < 0) {
			asprintf(error, "error get output format: %d", res);
			goto error;
		}
	} else if (out_state == PW_PORT_STATE_CONFIGURE && in_state > PW_PORT_STATE_CONFIGURE) {
		/* only output needs format */
		if ((res = spa_node_port_get_format(input->node->node,
						    SPA_DIRECTION_INPUT,
						    input->port_id,
						    (const struct spa_format **) &format)) < 0) {
			asprintf(error, "error get input format: %d", res);
			goto error;
		}
	} else if (in_state == PW_PORT_STATE_CONFIGURE && out_state == PW_PORT_STATE_CONFIGURE) {
	      again:
		/* both ports need a format */
		pw_log_debug("core %p: finding best format", core);
		if ((res = spa_node_port_enum_formats(input->node->node,
						      SPA_DIRECTION_INPUT,
						      input->port_id, &filter, NULL, iidx)) < 0) {
			if (res == SPA_RESULT_ENUM_END && iidx != 0) {
				asprintf(error, "error input enum formats: %d", res);
				goto error;
			}
		}
		pw_log_debug("Try filter: %p", filter);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(filter, core->type.map);

		if ((res = spa_node_port_enum_formats(output->node->node,
						      SPA_DIRECTION_OUTPUT,
						      output->port_id,
						      &format, filter, oidx)) < 0) {
			if (res == SPA_RESULT_ENUM_END) {
				oidx = 0;
				iidx++;
				goto again;
			}
			asprintf(error, "error output enum formats: %d", res);
			goto error;
		}
		pw_log_debug("Got filtered:");
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(format, core->type.map);

		spa_format_fixate(format);
	} else {
		asprintf(error, "error node state");
		goto error;
	}
	if (format == NULL) {
		asprintf(error, "error get format");
		goto error;
	}
	return format;

      error:
	return NULL;
}

struct pw_node_factory *pw_core_find_node_factory(struct pw_core *core, const char *name)
{
	struct pw_node_factory *factory;

	spa_list_for_each(factory, &core->node_factory_list, link) {
		if (strcmp(factory->name, name) == 0)
			return factory;
	}
	return NULL;
}
