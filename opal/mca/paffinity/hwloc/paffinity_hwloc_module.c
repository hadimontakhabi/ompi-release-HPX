/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC.
 *                         All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/*
 * 13 Mar 2012
 *
 * Per discussion on https://svn.open-mpi.org/trac/ompi/ticket/3051,
 * we're converting the hwloc paffinity module to have logical and
 * physical be equivalent.  Specifically, use the hwloc logical
 * mapping for all "logical" results.  And when asked to convert
 * paffinity logical to physical, just return the unity conversion
 * (i.e., physical 6 == logical 6).
 *
 * As a consequence, note that the logical/physical core IDs that
 * we're returning are NOT relative to the socket that they're on.
 * They are unique across all cores, which makes looking them up a
 * little easier (i.e., you don't have to find the socket first, then
 * look up the core -- you can just look up the core from the set of
 * all cores).
 *
 * This really only has relevance for the v1.5/v1.6 branch, as the
 * trunk/v1.7 has been revamped w.r.t. paffinity, and we use hwloc
 * objects for everything.
 */

#include "opal_config.h"

/* This component will only be compiled on Hwloc, where we are
   guaranteed to have <unistd.h> and friends */
#include <stdio.h>

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "opal/constants.h"
#include "opal/util/output.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/mca/paffinity/paffinity.h"
#include "opal/mca/paffinity/base/base.h"
#include "paffinity_hwloc.h"
#include "opal/mca/hwloc/hwloc.h"

/*
 * Local functions
 */
static int module_init(void);
static int module_set(opal_paffinity_base_cpu_set_t cpumask);
static int module_get(opal_paffinity_base_cpu_set_t *cpumask);
static int module_map_to_processor_id(int socket, int core, int *processor_id);
static int module_map_to_socket_core(int processor_id, int *socket, int *core);
static int module_get_processor_info(int *num_processors);
static int module_get_socket_info(int *num_sockets);
static int module_get_core_info(int socket, int *num_cores);
static int module_get_physical_processor_id(int logical_processor_id);
static int module_get_physical_socket_id(int logical_socket_id);
static int module_get_physical_core_id(int physical_socket_id, 
                                       int logical_core_id);

/*
 * Hwloc paffinity module
 */
static const opal_paffinity_base_module_1_1_0_t loc_module = {
    /* Initialization function */
    module_init,

    /* Module function pointers */
    module_set,
    module_get,
    module_map_to_processor_id,
    module_map_to_socket_core,
    module_get_processor_info,
    module_get_socket_info,
    module_get_core_info,
    module_get_physical_processor_id,
    module_get_physical_socket_id,
    module_get_physical_core_id,
    NULL
};

/*
 * Trivial DFS traversal recursion function
 */
static hwloc_obj_t dfs_find_nth_item(hwloc_obj_t root, 
                                     hwloc_obj_type_t type, 
                                     unsigned *current,
                                     unsigned n)
{
    unsigned i;
    hwloc_obj_t ret;

    if (root->type == type) {
        if (*current == n) {
            return root;
        }
        ++(*current);
    }
    for (i = 0; i < root->arity; ++i) {
        ret = dfs_find_nth_item(root->children[i], type, current, n);
        if (NULL != ret) {
            return ret;
        }
    }

    return NULL;
}

/*
 * Trivial DFS traversal recursion function
 */
static int dfs_count_type(hwloc_obj_t root, hwloc_obj_type_t type)
{
    unsigned i;
    int count = 0;
    if (root->type == type) {
        ++count;
    }
    for (i = 0; i < root->arity; ++i) {
        count += dfs_count_type(root->children[i], type);
    }

    return count;
}


int opal_paffinity_hwloc_component_query(mca_base_module_t **module, 
                                         int *priority)
{
    int param;

    param = mca_base_param_find("paffinity", "hwloc", "priority");
    mca_base_param_lookup_int(param, priority);

    *module = (mca_base_module_t *)&loc_module;

    return OPAL_SUCCESS;
}


static int module_init(void)
{
    /* Nothing to do */

    return OPAL_SUCCESS;
}


/*
 * Per comment in the beginning of this file, the input mask to this
 * function will be a set of logical core IDs.  We need to convert it
 * to a bitmap of physical PU IDs.  Specifically, for any (logical)
 * core ID in the output mask, set all physical PU IDs in are in that
 * core in the mask that we use to bind.
 */
static int module_set(opal_paffinity_base_cpu_set_t mask)
{
    int ret = OPAL_SUCCESS;
    hwloc_bitmap_t set = NULL, tmp = NULL, tmp2 = NULL;
    hwloc_obj_t core;

    /* bozo check */
    if (NULL == opal_hwloc_topology) {
        return OPAL_ERR_NOT_SUPPORTED;
    }

    set = hwloc_bitmap_alloc();
    if (NULL == set) {
        return OPAL_ERR_OUT_OF_RESOURCE;
    }
    hwloc_bitmap_zero(set);

    tmp = hwloc_bitmap_alloc();
    if (NULL == tmp) {
        ret = OPAL_ERR_OUT_OF_RESOURCE;
        goto out;
    }
    tmp2 = hwloc_bitmap_alloc();
    if (NULL == tmp2) {
        ret = OPAL_ERR_OUT_OF_RESOURCE;
        goto out;
    }

    /* Iterate through the cores */
    for (core = hwloc_get_obj_by_type(opal_hwloc_topology, HWLOC_OBJ_CORE, 0);
         core && core->logical_index < OPAL_PAFFINITY_BITMASK_CPU_MAX;
         core = core->next_cousin) {
        if (OPAL_PAFFINITY_CPU_ISSET(core->logical_index, mask)) {
            /* This is a core that's in the input mask.  Yay!  Get the
               actually-available PUs (i.e., (online & allowed)) */
            hwloc_bitmap_and(tmp, core->online_cpuset, core->allowed_cpuset);
            /* OR those PUs with the set of PUs that we already have */
            hwloc_bitmap_or(tmp2, set, tmp);
            /* Now copy that bitmap from the temp output back to the main set */
            hwloc_bitmap_copy(set, tmp2);
        }
    }

    if (0 != hwloc_set_cpubind(opal_hwloc_topology, set, 0)) {
        ret = OPAL_ERR_IN_ERRNO;
    }

 out:
    if (NULL != set) {
        hwloc_bitmap_free(set);
    }
    if (NULL != tmp) {
        hwloc_bitmap_free(tmp);
    }
    if (NULL != tmp2) {
        hwloc_bitmap_free(tmp2);
    }

    return ret;
}


/*
 * Per the comment at the top of this file, we need to return a bitmap
 * of *logical* *core* IDs.  So we have to get the binding from hwloc
 * (which returns a bitmap of *physical* PU IDs) and then convert it
 * to a bitmap of *logical* core IDs.
 *
 * Also see https://svn.open-mpi.org/trac/ompi/ticket/3085.
 */
static int module_get(opal_paffinity_base_cpu_set_t *mask)
{
    int ret = OPAL_SUCCESS;
    hwloc_bitmap_t set = NULL;
    hwloc_topology_t *t;
    hwloc_obj_t pu, core;

    /* bozo check */
    if (NULL == opal_hwloc_topology) {
        return OPAL_ERR_NOT_SUPPORTED;
    }
    t = &opal_hwloc_topology;

    if (NULL == mask) {
        return OPAL_ERR_BAD_PARAM;
    }

    set = hwloc_bitmap_alloc();
    if (NULL == set) {
        return OPAL_ERR_OUT_OF_RESOURCE;
    }

    /* Get the physical bitmap representing the binding */
    if (0 != hwloc_get_cpubind(*t, set, 0)) {
        ret = OPAL_ERR_IN_ERRNO;
        goto out;
    } 

    /* Now convert that bitmap of physical PU IDs:
       - to *logical* core IDs
       - to only include the first PU in any given core */
    OPAL_PAFFINITY_CPU_ZERO(*mask);
    for (pu = hwloc_get_obj_by_type(*t, HWLOC_OBJ_PU, 0);
         pu && pu->logical_index < 8 * sizeof(*mask);
         pu = pu->next_cousin) {
        if (hwloc_bitmap_isset(set, pu->os_index)) {
            /* This PU is set.  Let's see if it was the *first* PU
               to be set in this core. */
            core = pu->parent;
            while (NULL != core && HWLOC_OBJ_CORE != core->type) {
                core = core->parent;
            }
            
            if (NULL == core) {
                /* If hwloc didn't report the parent core, then give
                   up */
                ret = OPAL_ERR_NOT_FOUND;
                goto out;
            } else {
                /* Otherwise, save this core's logical index in the
                   output mask */
                OPAL_PAFFINITY_CPU_SET(core->logical_index, *mask);
            }
        }
    }

 out:
    if (NULL != set) {
        hwloc_bitmap_free(set);
    }

    return ret;
}

/*
 * Returns mapping of PHYSICAL socket:core -> PHYSICAL processor id.
 *
 * Since paffinity currently does not understand hardware threads,
 * return the processor ID of the first hardware thread in the target
 * core.
 */
static int module_map_to_processor_id(int socket, int core, int *processor_id)
{
    hwloc_topology_t *t;
    hwloc_obj_t core_obj;
    hwloc_bitmap_t good;

    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_map_to_processor_id: IN: socket=%d, core=%d", socket, core);
    /* bozo check */
    if (NULL == opal_hwloc_topology) {
        return OPAL_ERR_NOT_SUPPORTED;
    }
    t = &opal_hwloc_topology;

    /* The physical core IDs that we're returning from
       module_map_to_socket_core() and module_get_physical_core_id()
       correspond to hwloc's logical core ordering, which is unique
       across all sockets.  Since we know that value is unique across
       all sockets, we can just look up that core ID directly. */
    core_obj = hwloc_get_obj_by_type(*t, HWLOC_OBJ_CORE, core);
    if (NULL == core_obj) {
        opal_output_verbose(10, opal_paffinity_base_output,
                            "hwloc_module_map_to_processor_id: OUT: Didn't find core %d", core);
        return OPAL_ERR_NOT_FOUND;
    }

    /* Ok, we found the right core.  Get the cpuset and return the
       first PU (because hwloc understands hardware threads, of which
       there might be multiple on this core). */
    good = hwloc_bitmap_alloc();
    if (NULL == good) {
        return OPAL_ERR_OUT_OF_RESOURCE;
    }
    hwloc_bitmap_and(good, core_obj->online_cpuset,
                     core_obj->allowed_cpuset);
    {
        char str[1024];
        hwloc_bitmap_snprintf(str, sizeof(str), good);
        opal_output_verbose(10, opal_paffinity_base_output,
                            "hwloc_module_map_to_processor_id: OUT: map_to_processor_id: bitmap %s", str);
    }
    *processor_id = hwloc_bitmap_first(good);
    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_map_to_processor_id: OUT: socket=%d, core=%d, processor_id=%d", socket, core, *processor_id);
    hwloc_bitmap_free(good);
    return OPAL_SUCCESS;
}

/*
 * Provides mapping of PHYSICAL processor id -> PHYSICAL socket:core.
 */
static int module_map_to_socket_core(int processor_id, int *socket, int *core)
{
    int ret;
    hwloc_obj_t obj, prev;
    hwloc_topology_t *t;
    hwloc_bitmap_t desired = NULL, good = NULL;

    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_map_to_socket_core: IN: proc_id = %d", processor_id);

    /* bozo check */
    if (NULL == opal_hwloc_topology) {
        return OPAL_ERR_NOT_SUPPORTED;
    }
    t = &opal_hwloc_topology;

    /* Create a bitmap with the desired processor ID */
    desired = hwloc_bitmap_alloc();
    if (NULL == desired) {
        ret = OPAL_ERR_OUT_OF_RESOURCE;
        goto out;
    }
    hwloc_bitmap_set(desired, processor_id);

    good = hwloc_bitmap_alloc();
    if (NULL == good) {
        ret = OPAL_ERR_OUT_OF_RESOURCE;
        goto out;
    }

    /* Find the PU with that bitmap */
    prev = NULL;
    while (1) {
        obj = hwloc_get_next_obj_covering_cpuset_by_type(*t, desired, 
                                                         HWLOC_OBJ_PU, prev);
        if (NULL == obj) {
            ret = OPAL_ERR_NOT_FOUND;
            goto out;
        }
        prev = obj;

        /* Double check that the PU we found has that processor ID
           online and allowed */
        hwloc_bitmap_and(good, obj->online_cpuset, obj->allowed_cpuset);
        if (hwloc_bitmap_isset(good, processor_id)) {
            /* If it's good, break out of the loop to find the
               core/socket parents */
            break;
        }
    }

    /* If we get here, then we found a good PU.  Now find its core and
       socket parents */

    while (1) {
        obj = obj->parent;
        if (NULL == obj) {
            ret = OPAL_ERR_NOT_FOUND;
            goto out;
        }

        *core = -1;
        switch (obj->type) {
        case HWLOC_OBJ_CORE:
            *core = obj->logical_index;
            break;

        case HWLOC_OBJ_SOCKET:
            *socket = obj->logical_index;
            if (-1 != *core) {
                ret = OPAL_SUCCESS;
            } else {
                ret = OPAL_ERR_NOT_FOUND;
            }
            opal_output_verbose(10, opal_paffinity_base_output,
                                "hwloc_module_map_to_socket_core: OUT: FOUND proc_id = %d, socket=%d, core=%d", processor_id, *socket, *core);
            goto out;

        default:
            /* Silence compiler warning */
            break;
        }
    }

    /* Will never get here */

 out:
    if (NULL != desired) {
        hwloc_bitmap_free(desired);
    }
    if (NULL != good) {
        hwloc_bitmap_free(good);
    }
    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_map_to_socket_core: OUT: FAIL");

    return ret;
}

/*
 * Provides number of LOGICAL processors in a host.  Since paffinity
 * does not currently understand hardware threads, we interpret
 * "processors" to mean "cores".
 */
static int module_get_processor_info(int *num_processors)
{
    hwloc_topology_t *t;

    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_get_processor_info: IN");

    /* bozo check */
    if (NULL == opal_hwloc_topology) {
        return OPAL_ERR_NOT_SUPPORTED;
    }
    t = &opal_hwloc_topology;

    /* Try the simple hwloc_get_nbobjs_by_type() first.  If we get -1,
       go aggregate ourselves (because it means that there are cores
       are multiple levels in the topology). */
    *num_processors = (int) hwloc_get_nbobjs_by_type(*t, HWLOC_OBJ_CORE);
    if (-1 == *num_processors) {
        hwloc_obj_t obj;

        *num_processors = 0;
        for (obj = hwloc_get_next_obj_by_type(*t, HWLOC_OBJ_CORE, NULL);
             NULL != obj; 
             obj = hwloc_get_next_obj_by_type(*t, HWLOC_OBJ_CORE, obj)) {
            if (HWLOC_OBJ_CORE == obj->type) {
                ++*num_processors;
            }
        }
    }

    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_get_processor_info: OUT: returning %d processors (cores)", *num_processors);
    return OPAL_SUCCESS;
}

/*
 * Provides the number of LOGICAL sockets in a host.
 */
static int module_get_socket_info(int *num_sockets)
{
    hwloc_topology_t *t;

    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_get_socket_info: IN");

    /* bozo check */
    if (NULL == opal_hwloc_topology) {
        return OPAL_ERR_NOT_SUPPORTED;
    }
    t = &opal_hwloc_topology;

    /* Try the simple hwloc_get_nbobjs_by_type() first.  If we get -1,
       go aggregate ourselves (because it means that there are cores
       are multiple levels in the topology). */
    *num_sockets = (int) hwloc_get_nbobjs_by_type(*t, HWLOC_OBJ_SOCKET);
    if (-1 == *num_sockets) {
        hwloc_obj_t obj;

        *num_sockets = 0;
        for (obj = hwloc_get_next_obj_by_type(*t, HWLOC_OBJ_SOCKET, NULL);
             NULL != obj; 
             obj = hwloc_get_next_obj_by_type(*t, HWLOC_OBJ_SOCKET, obj)) {
            if (HWLOC_OBJ_CORE == obj->type) {
                ++*num_sockets;
            }
        }
    }

    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_get_socket_info: OUT: returning %d sockets", *num_sockets);
    return OPAL_SUCCESS;
}

/*
 * Provides the number of LOGICAL cores in a PHYSICAL socket. 
 */
static int module_get_core_info(int socket, int *num_cores)
{
    hwloc_obj_t obj;
    hwloc_topology_t *t;

    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_get_core_info: IN: socket=%d", socket);

    /* bozo check */
    if (NULL == opal_hwloc_topology) {
        return OPAL_ERR_NOT_SUPPORTED;
    }
    t = &opal_hwloc_topology;

    /* Find the socket */
    obj = hwloc_get_obj_by_type(*t, HWLOC_OBJ_SOCKET, socket);
    if (NULL == obj) {
        return OPAL_ERR_NOT_FOUND;
    }

    /* Ok, we found the right socket.  Browse its descendants looking
       for all cores. */
    *num_cores = dfs_count_type(obj, HWLOC_OBJ_CORE);
    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_get_core_info: OUT: socket=%d, num_cores=%d", socket, *num_cores);
    return OPAL_SUCCESS;
}

/*
 * Provide the PHYSICAL processor id that corresponds to the given
 * LOGICAL processor id.  Remember: paffinity does not understand
 * hardware threads, so "processor" here [usually] means "core" --
 * except that on some platforms, hwloc won't find any cores; it'll
 * only find PUs (!).  On such platforms, then do the same calculation
 * but with PUs instead of COREs.
 */
static int module_get_physical_processor_id(int logical_processor_id)
{
    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_get_physical_processor_id: INOUT: logical proc %d (unity)", logical_processor_id);
    return logical_processor_id;
}

/*
 * Provide the PHYSICAL socket id that corresponds to the given
 * LOGICAL socket id
 */
static int module_get_physical_socket_id(int logical_socket_id)
{
    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_get_physical_processor_id: INOUT: logical socket %d (unity)", logical_socket_id);
    return logical_socket_id;
}

/*
 * Provide the PHYSICAL core id that corresponds to the given LOGICAL
 * core id on the given PHYSICAL socket id
 */
static int module_get_physical_core_id(int physical_socket_id, 
                                       int logical_core_id)
{
    unsigned count = 0;
    hwloc_obj_t obj;
    hwloc_topology_t *t;

    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_get_physical_core_id: IN: phys socket=%d, logical core=%d",
                        physical_socket_id, logical_core_id);
    /* bozo check */
    if (NULL == opal_hwloc_topology) {
        return OPAL_ERR_NOT_SUPPORTED;
    }
    t = &opal_hwloc_topology;

    obj = hwloc_get_obj_by_type(*t, HWLOC_OBJ_SOCKET, physical_socket_id);
    if (NULL == obj) {
        return OPAL_ERR_NOT_FOUND;
    }

    /* Note that we can't look at hwloc's logical_index here -- hwloc
       counts logically across *all* cores.  We only want to find the
       Nth logical core under this particular socket. */
    obj = dfs_find_nth_item(obj, HWLOC_OBJ_CORE, &count, logical_core_id);
    if (NULL == obj) {
        return OPAL_ERR_NOT_FOUND;
    }
    opal_output_verbose(10, opal_paffinity_base_output,
                        "hwloc_module_get_physical_core_id: OUT: phys socket=%d, logical core=%d: return %d",
                        physical_socket_id, logical_core_id, obj->logical_index);
    return obj->logical_index;
}