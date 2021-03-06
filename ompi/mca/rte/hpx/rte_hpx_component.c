/*
 * Copyright (c) 2012      Los Alamos National Security, LLC.  All rights reserved. 
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "ompi/mca/rte/rte.h"
#include "ompi/mca/rte/hpx/rte_hpx_db.h"

/*
 * Public string showing the component version number
 */
const char *ompi_rte_hpx_component_version_string =
    "OMPI hpx rte MCA component version " OMPI_VERSION;

/*
 * Local function
 */
static int rte_hpx_open(void);
static int rte_hpx_close(void);
static int rte_hpx_register(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

ompi_rte_hpx_component_t mca_rte_hpx_component = {
    {
        /* First, the mca_component_t struct containing meta information
           about the component itself */

        {
            OMPI_RTE_BASE_VERSION_1_0_0,

            /* Component name and version */
            "hpx",
            OMPI_MAJOR_VERSION,
            OMPI_MINOR_VERSION,
            OMPI_RELEASE_VERSION,

            /* Component open and close functions */
            rte_hpx_open,
            rte_hpx_close,
            NULL,
            rte_hpx_register
        },
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int rte_hpx_open(void)
{
    OBJ_CONSTRUCT(&mca_rte_hpx_component.lock, opal_mutex_t);
    OBJ_CONSTRUCT(&mca_rte_hpx_component.modx_reqs, opal_list_t);

    return OMPI_SUCCESS;
}

static int rte_hpx_close(void)
{
    opal_mutex_lock(&mca_rte_hpx_component.lock);
    OPAL_LIST_DESTRUCT(&mca_rte_hpx_component.modx_reqs);
    OBJ_DESTRUCT(&mca_rte_hpx_component.lock);

    return OMPI_SUCCESS;
}

static int rte_hpx_register(void)
{
    mca_rte_hpx_component.direct_modex = false;
    (void) mca_base_component_var_register (&mca_rte_hpx_component.super.base_version,
                                            "direct_modex", "Enable direct modex (default: false)",
                                            MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                            OPAL_INFO_LVL_9,
                                            MCA_BASE_VAR_SCOPE_READONLY, &mca_rte_hpx_component.direct_modex);
    return OMPI_SUCCESS;
}

static void con(ompi_hpx_tracker_t *p)
{
    p->active = true;
    OBJ_CONSTRUCT(&p->lock, opal_mutex_t);
    OBJ_CONSTRUCT(&p->cond, opal_condition_t);
}
static void des(ompi_hpx_tracker_t *p)
{
    OBJ_DESTRUCT(&p->lock);
    OBJ_DESTRUCT(&p->cond);
}
OBJ_CLASS_INSTANCE(ompi_hpx_tracker_t,
                   opal_list_item_t,
                   con, des);


int printf(const char *restrict format, ...)
{
  char     out[1024];
  va_list  args;
        
  va_start ( args, format );
  vsnprintf ( out, 1024, format, args );
  va_end   ( args );

  rte_hpx_cpp_printf(out);
  return (strlen(out));
}

