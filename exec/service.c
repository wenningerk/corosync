/*
 * Copyright (c) 2006 MontaVista Software, Inc.
 * Copyright (c) 2006-2009 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <corosync/lcr/lcr_ifact.h>
#include <corosync/swab.h>
#include <corosync/totem/totem.h>

#include <corosync/corotypes.h>
#include <corosync/coroipc_types.h>
#include "mainconfig.h"
#include "util.h"
#include <corosync/engine/logsys.h>

#include "timer.h"
#include <corosync/totem/totempg.h>
#include <corosync/totem/totemip.h>
#include "main.h"
#include <corosync/engine/coroapi.h>
#include "service.h"

LOGSYS_DECLARE_SUBSYS ("SERV");

struct default_service {
	const char *name;
	int ver;
};

static struct default_service default_services[] = {
	{
		.name			 = "corosync_evs",
		.ver			 = 0,
	},
	{
		.name			 = "corosync_cfg",
		.ver			 = 0,
	},
	{
		.name			 = "corosync_cpg",
		.ver			 = 0,
	},
	{
		.name			 = "corosync_confdb",
		.ver			 = 0,
	},
	{
		.name			 = "corosync_pload",
		.ver			 = 0,
	},
	{
		.name			 = "corosync_quorum",
		.ver			 = 0,
	}
};

struct corosync_service_engine *ais_service[SERVICE_HANDLER_MAXIMUM_COUNT];

hdb_handle_t service_stats_handle[SERVICE_HANDLER_MAXIMUM_COUNT][64];

static hdb_handle_t object_internal_configuration_handle;
static hdb_handle_t object_stats_services_handle;

static unsigned int default_services_requested (struct corosync_api_v1 *corosync_api)
{
	hdb_handle_t object_service_handle;
	hdb_handle_t object_find_handle;
	char *value;

	/*
	 * Don't link default services if they have been disabled
	 */
	corosync_api->object_find_create (
		OBJECT_PARENT_HANDLE,
		"aisexec",
		strlen ("aisexec"),
		&object_find_handle);

	if (corosync_api->object_find_next (
		object_find_handle,
		&object_service_handle) == 0) {

		if ( ! corosync_api->object_key_get (object_service_handle,
			"defaultservices",
			strlen ("defaultservices"),
			(void *)&value,
			NULL)) {

			if (value && strcmp (value, "no") == 0) {
				return 0;
			}
		}
	}

	corosync_api->object_find_destroy (object_find_handle);

	return (-1);
}

unsigned int corosync_service_link_and_init (
	struct corosync_api_v1 *corosync_api,
	const char *service_name,
	unsigned int service_ver)
{
	struct corosync_service_engine_iface_ver0 *iface_ver0;
	void *iface_ver0_p;
	hdb_handle_t handle;
	struct corosync_service_engine *service;
	unsigned int res;
	hdb_handle_t object_service_handle;
	hdb_handle_t object_stats_handle;
	int fn;
	char object_name[32];
	char *name_sufix;
	uint64_t zero_64 = 0;

	/*
	 * reference the service interface
	 */
	iface_ver0_p = NULL;
	lcr_ifact_reference (
		&handle,
		service_name,
		service_ver,
		&iface_ver0_p,
		(void *)0);

	iface_ver0 = (struct corosync_service_engine_iface_ver0 *)iface_ver0_p;

	if (iface_ver0 == 0) {
		log_printf(LOGSYS_LEVEL_ERROR, "Service failed to load '%s'.\n", service_name);
		return (-1);
	}


	/*
	 * Initialize service
	 */
	service = iface_ver0->corosync_get_service_engine_ver0();

	ais_service[service->id] = service;
	if (service->config_init_fn) {
		res = service->config_init_fn (corosync_api);
	}

	if (service->exec_init_fn) {
		res = service->exec_init_fn (corosync_api);
	}

	/*
	 * Store service in object database
	 */
	corosync_api->object_create (object_internal_configuration_handle,
		&object_service_handle,
		"service",
		strlen ("service"));

	corosync_api->object_key_create_typed (object_service_handle,
		"name",
		service_name,
		strlen (service_name) + 1, OBJDB_VALUETYPE_STRING);

	corosync_api->object_key_create_typed (object_service_handle,
		"ver",
		&service_ver,
		sizeof (service_ver), OBJDB_VALUETYPE_UINT32);

	res = corosync_api->object_key_create_typed (object_service_handle,
		"handle",
		&handle,
		sizeof (handle), OBJDB_VALUETYPE_UINT64);

	corosync_api->object_key_create_typed (object_service_handle,
		"service_id",
		&service->id,
		sizeof (service->id), OBJDB_VALUETYPE_UINT16);

	name_sufix = strrchr (service_name, '_');
	if (name_sufix)
		name_sufix++;
	else
		name_sufix = (char*)service_name;

	corosync_api->object_create (object_stats_services_handle,
								 &object_stats_handle,
								 name_sufix, strlen (name_sufix));

	corosync_api->object_key_create_typed (object_stats_handle,
										 "service_id",
										 &service->id, sizeof (service->id),
										 OBJDB_VALUETYPE_INT16);

	for (fn = 0; fn < service->exec_engine_count; fn++) {

		snprintf (object_name, 32, "%d", fn);
		corosync_api->object_create (object_stats_handle,
									 &service_stats_handle[service->id][fn],
									 object_name, strlen (object_name));

		corosync_api->object_key_create_typed (service_stats_handle[service->id][fn],
											 "tx",
											 &zero_64, sizeof (zero_64),
											 OBJDB_VALUETYPE_UINT64);

		corosync_api->object_key_create_typed (service_stats_handle[service->id][fn],
											 "rx",
											 &zero_64, sizeof (zero_64),
											 OBJDB_VALUETYPE_UINT64);
	}

	log_printf (LOGSYS_LEVEL_NOTICE, "Service initialized '%s'\n", service->name);
	return (res);
}

static int service_priority_max(void)
{
    int lpc = 0, max = 0;
    for(; lpc < SERVICE_HANDLER_MAXIMUM_COUNT; lpc++) {
	if(ais_service[lpc] != NULL && ais_service[lpc]->priority > max) {
	    max = ais_service[lpc]->priority;
	}
    }
    return max;
}

extern unsigned int corosync_service_unlink_priority (struct corosync_api_v1 *corosync_api, int priority)
{
	char *service_name;
	unsigned int *service_ver;
	unsigned short *service_id;
	hdb_handle_t object_service_handle;
	hdb_handle_t object_find_handle;
	int p = service_priority_max();
	int lpc = 0;

	if(priority == 0) {
	    log_printf(LOGSYS_LEVEL_NOTICE, "Unloading all corosync components\n");
	} else {
	    log_printf(LOGSYS_LEVEL_NOTICE, "Unloading corosync components up to (and including) priority %d\n", priority);
	}

	for( ; p >= priority; p--) {
	    for(lpc = 0; lpc < SERVICE_HANDLER_MAXIMUM_COUNT; lpc++) {
		if(ais_service[lpc] == NULL || ais_service[lpc]->priority != p) {
		    continue;
		}

		/* unload
		 *
		 * If we had a pointer to the objdb entry, we'd not need to go looking again...
		 */
 		corosync_api->object_find_create (
		    object_internal_configuration_handle,
		    "service", strlen ("service"), &object_find_handle);

		while(corosync_api->object_find_next (
			  object_find_handle, &object_service_handle) == 0) {

		    int res = corosync_api->object_key_get (
			object_service_handle,
			"service_id", strlen ("service_id"), (void *)&service_id, NULL);

		    service_name = NULL;
		    if(res == 0 && *service_id == ais_service[lpc]->id) {
			hdb_handle_t *found_service_handle;
			corosync_api->object_key_get (
			    object_service_handle,
			    "name", strlen ("name"), (void *)&service_name, NULL);

			corosync_api->object_key_get (
			    object_service_handle,
			    "ver", strlen ("ver"), (void *)&service_ver, NULL);

			res = corosync_api->object_key_get (
			    object_service_handle,
			    "handle", strlen ("handle"), (void *)&found_service_handle, NULL);

			res = corosync_api->object_key_get (
			    object_service_handle,
			    "service_id", strlen ("service_id"), (void *)&service_id, NULL);

			log_printf(LOGSYS_LEVEL_NOTICE, "Unloading corosync component: %s v%u\n",
				   service_name, *service_ver);

			if (ais_service[*service_id]->exec_exit_fn) {
			    ais_service[*service_id]->exec_exit_fn ();
			}
			ais_service[*service_id] = NULL;

			lcr_ifact_release (*found_service_handle);

			corosync_api->object_destroy (object_service_handle);
			break;
		    }
		}

		corosync_api->object_find_destroy (object_find_handle);
	    }
	}
	return 0;
}

extern unsigned int corosync_service_unlink_and_exit (
	struct corosync_api_v1 *corosync_api,
	const char *service_name,
	unsigned int service_ver)
{
	hdb_handle_t object_service_handle;
	char *found_service_name;
	unsigned short *service_id;
	unsigned int *found_service_ver;
	hdb_handle_t object_find_handle;
	char *name_sufix;

	name_sufix = strrchr (service_name, '_');
	if (name_sufix)
		name_sufix++;
	else
		name_sufix = (char*)service_name;

	corosync_api->object_find_create (
		object_stats_services_handle,
		name_sufix, strlen (name_sufix),
		&object_find_handle);

	if (corosync_api->object_find_next (
			object_find_handle,
			&object_service_handle) == 0) {

		corosync_api->object_destroy (object_service_handle);

	}
	corosync_api->object_find_destroy (object_find_handle);


	corosync_api->object_find_create (
		object_internal_configuration_handle,
		"service",
		strlen ("service"),
		&object_find_handle);

	while (corosync_api->object_find_next (
		object_find_handle,
		&object_service_handle) == 0) {

		corosync_api->object_key_get (object_service_handle,
			"name",
			strlen ("name"),
			(void *)&found_service_name,
			NULL);

		if (strcmp (service_name, found_service_name) != 0) {
		    continue;
		}

		corosync_api->object_key_get (object_service_handle,
			"ver",
			strlen ("ver"),
			(void *)&found_service_ver,
			NULL);

		/*
		 * If service found and linked exit it
		 */
		if (service_ver != *found_service_ver) {
		    continue;
		}

		corosync_api->object_key_get (
		    object_service_handle,
		    "service_id", strlen ("service_id"),
		    (void *)&service_id, NULL);

		if(service_id != NULL
		   && *service_id > 0
		   && *service_id < SERVICE_HANDLER_MAXIMUM_COUNT
		   && ais_service[*service_id] != NULL) {

		    corosync_api->object_find_destroy (object_find_handle);
		    return corosync_service_unlink_priority (corosync_api, ais_service[*service_id]->priority);
		}
	}

	corosync_api->object_find_destroy (object_find_handle);

	return (-1);
}

extern unsigned int corosync_service_unlink_all (
	struct corosync_api_v1 *corosync_api)
{
    return corosync_service_unlink_priority (corosync_api, 0);
}

/*
 * Links default services into the executive
 */
unsigned int corosync_service_defaults_link_and_init (struct corosync_api_v1 *corosync_api)
{
	unsigned int i;

	hdb_handle_t object_service_handle;
	char *found_service_name;
	char *found_service_ver;
	unsigned int found_service_ver_atoi;
	hdb_handle_t object_find_handle;
	hdb_handle_t object_find2_handle;
	hdb_handle_t object_runtime_handle;

	corosync_api->object_find_create (
		OBJECT_PARENT_HANDLE,
		"runtime",
		strlen ("runtime"),
		&object_find2_handle);

	if (corosync_api->object_find_next (
			object_find2_handle,
			&object_runtime_handle) == 0) {

		corosync_api->object_create (object_runtime_handle,
									 &object_stats_services_handle,
									 "services", strlen ("services"));
	}
	corosync_api->object_create (OBJECT_PARENT_HANDLE,
		&object_internal_configuration_handle,
		"internal_configuration",
		strlen ("internal_configuration"));

	corosync_api->object_find_create (
		OBJECT_PARENT_HANDLE,
		"service",
		strlen ("service"),
		&object_find_handle);

	while (corosync_api->object_find_next (
		object_find_handle,
		&object_service_handle) == 0) {

		corosync_api->object_key_get (object_service_handle,
			"name",
			strlen ("name"),
			(void *)&found_service_name,
			NULL);

		found_service_ver = NULL;

		corosync_api->object_key_get (object_service_handle,
			"ver",
			strlen ("ver"),
			(void *)&found_service_ver,
			NULL);

		found_service_ver_atoi = (found_service_ver ? atoi (found_service_ver) : 0);

		corosync_service_link_and_init (
			corosync_api,
			found_service_name,
			found_service_ver_atoi);
	}

	corosync_api->object_find_destroy (object_find_handle);

 	if (default_services_requested (corosync_api) == 0) {
 		return (0);
 	}

	for (i = 0;
		i < sizeof (default_services) / sizeof (struct default_service); i++) {

		corosync_service_link_and_init (
			corosync_api,
			default_services[i].name,
			default_services[i].ver);
	}

	return (0);
}
