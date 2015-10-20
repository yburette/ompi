/*
 * Copyright (c) 2015 Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#include "opal/mca/base/base.h"
#include "opal/util/output.h"
#include "opal/mca/backtrace/backtrace.h"
#include "opal/mca/event/event.h"

#if OPAL_ENABLE_FT_CR == 1
#include "orte/mca/rml/rml.h"
#include "orte/mca/state/state.h"
#endif
#include "orte/mca/rml/base/base.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "rml_ofi.h"

static orte_rml_module_t* rml_ofi_init(int* priority);
static int rml_ofi_open(void);
static int rml_ofi_close(void);

/**
 * component definition
 */
orte_rml_component_t mca_rml_ofi_component = {
      /* First, the mca_base_component_t struct containing meta
         information about the component itself */

    .rml_version = {
        ORTE_RML_BASE_VERSION_2_0_0,

        .mca_component_name = "ofi",
        MCA_BASE_MAKE_VERSION(component, ORTE_MAJOR_VERSION, ORTE_MINOR_VERSION,
                              ORTE_RELEASE_VERSION),
        .mca_open_component = rml_ofi_open,
        .mca_close_component = rml_ofi_close,
    },
    .rml_data = {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
    .rml_init = rml_ofi_init,
};

orte_rml_ofi_module_t orte_rml_ofi_module = {
    {
        orte_rml_ofi_init,
        orte_rml_ofi_fini,

        orte_rml_ofi_get_uri,
        orte_rml_ofi_set_uri,

        orte_rml_ofi_ping,

        orte_rml_ofi_send_nb,
        orte_rml_ofi_send_buffer_nb,

        orte_rml_ofi_recv_nb,
        orte_rml_ofi_recv_buffer_nb,

        orte_rml_ofi_recv_cancel,

        orte_rml_ofi_add_exception,
        orte_rml_ofi_del_exception,
        orte_rml_ofi_ft_event,
        orte_rml_ofi_purge,

        orte_rml_ofi_open_channel,
        orte_rml_ofi_send_channel_nb,
        orte_rml_ofi_send_buffer_channel_nb,
        orte_rml_ofi_close_channel
    }
};

/* Local variables */
static bool init_done = false;

static int
rml_ofi_open(void)
{
    orte_rml_ofi.domain =  NULL;
    orte_rml_ofi.av     =  NULL;
    orte_rml_ofi.cq     =  NULL;
    orte_rml_ofi.ep     =  NULL;
    return ORTE_SUCCESS;
}


static int
rml_ofi_close(void)
{
    return ORTE_SUCCESS;
}

static orte_rml_module_t*
rml_ofi_init(int* priority)
{
    int ret, fi_version;
    struct fi_info *hints;
    struct fi_info *providers = NULL, *prov = NULL;
    struct fi_cq_attr cq_attr = {0};
    struct fi_av_attr av_attr = {0};
    char ep_name[FI_NAME_MAX] = {0};
    size_t namelen;

    if (init_done) {
        *priority = 1;
        return &orte_rml_ofi.super;
    }

    *priority = 1;

    /**
     * Hints to filter providers
     * See man fi_getinfo for a list of all filters
     * mode:  Select capabilities MTL is prepared to support.
     *        In this case, MTL will pass in context into communication calls
     * ep_type:  reliable datagram operation
     * caps:     Capabilities required from the provider.
     *           Tag matching is specified to implement MPI semantics.
     * msg_order: Guarantee that messages with same tag are ordered.
     */
    hints = fi_allocinfo();
    if (!hints) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: Could not allocate fi_info\n",
                            __FILE__, __LINE__);
        goto error;
    }
    hints->mode               = FI_CONTEXT;
    hints->ep_attr->type      = FI_EP_RDM;      /* Reliable datagram         */
    hints->caps               = FI_TAGGED;      /* Tag matching interface    */
    hints->tx_attr->msg_order = FI_ORDER_SAS;
    hints->rx_attr->msg_order = FI_ORDER_SAS;

    /**
     * Refine filter for additional capabilities
     * threading:  Disable locking
     * control_progress:  enable async progress
     */
    hints->domain_attr->threading        = FI_THREAD_ENDPOINT;
    hints->domain_attr->control_progress = FI_PROGRESS_AUTO;

    /**
     * FI_VERSION provides binary backward and forward compatibility support
     * Specify the version of OFI is coded to, the provider will select struct
     * layouts that are compatible with this version.
     */
    fi_version = FI_VERSION(1, 0);

    /**
     * fi_getinfo:  returns information about fabric  services for reaching a
     * remote node or service.  this does not necessarily allocate resources.
     * Pass NULL for name/service because we want a list of providers supported.
     */
    ret = fi_getinfo(fi_version,    /* OFI version requested                    */
                     NULL,          /* Optional name or fabric to resolve       */
                     NULL,          /* Optional service name or port to request */
                     0ULL,          /* Optional flag                            */
                     hints,        /* In: Hints to filter providers            */
                     &providers);   /* Out: List of matching providers          */
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_getinfo failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Select a provider from the list returned by fi_getinfo().
     */
    prov = select_ofi_provider(providers);
    if (!prov) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: select_ofi_provider: no provider found\n",
                            __FILE__, __LINE__);
        goto error;
    }


    /**
     * Open fabric
     * The getinfo struct returns a fabric attribute struct that can be used to
     * instantiate the virtual or physical network. This opens a "fabric
     * provider". See man fi_fabric for details.
     */
    ret = fi_fabric(prov->fabric_attr,    /* In:  Fabric attributes             */
                    &orte_rml_ofi.fabric, /* Out: Fabric handle                 */
                    NULL);                /* Optional context for fabric events */
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_fabric failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Create the access domain, which is the physical or virtual network or
     * hardware port/collection of ports.  Returns a domain object that can be
     * used to create endpoints.  See man fi_domain for details.
     */
    ret = fi_domain(orte_rml_ofi.fabric,  /* In:  Fabric object                 */
                    prov,                 /* In:  Provider                      */
                    &orte_rml_ofi.domain, /* Out: Domain oject                  */
                    NULL);                /* Optional context for domain events */
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_domain failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Create a transport level communication endpoint.  To use the endpoint,
     * it must be bound to completion counters or event queues and enabled,
     * and the resources consumed by it, such as address vectors, counters,
     * completion queues, etc.
     * see man fi_endpoint for more details.
     */
    ret = fi_endpoint(orte_rml_ofi.domain, /* In:  Domain object   */
                      prov,                /* In:  Provider        */
                      &orte_rml_ofi.ep,    /* Out: Endpoint object */
                      NULL);               /* Optional context     */
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_endpoint failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Save the maximum inject size.
     */
    orte_rml_ofi.max_inject_size = prov->tx_attr->inject_size;

    /**
     * Create the objects that will be bound to the endpoint.
     * The objects include:
     *     - completion queue for events
     *     - address vector of other endpoint addresses
     *     - dynamic memory-spanning memory region
     */
    cq_attr.format = FI_CQ_FORMAT_TAGGED;
    ret = fi_cq_open(orte_rml_ofi.domain, &cq_attr, &orte_rml_ofi.cq, NULL);
    if (ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_cq_open failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * The remote fi_addr will be stored in the ofi_endpoint struct.
     * So, we use the AV in "map" mode.
     */
    av_attr.type = FI_AV_MAP;
    ret = fi_av_open(orte_rml_ofi.domain, &av_attr, &orte_rml_ofi.av, NULL);
    if (ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_av_open failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Bind the CQ and AV to the endpoint object.
     */
    ret = fi_ep_bind(orte_rml_ofi.ep,
                     (fid_t)orte_rml_ofi.cq,
                     FI_SEND | FI_RECV);
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_bind CQ-EP failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    ret = fi_ep_bind(orte_rml_ofi.ep,
                     (fid_t)orte_rml_ofi.av,
                     0);
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_bind AV-EP failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Enable the endpoint for communication
     * This commits the bind operations.
     */
    ret = fi_enable(orte_rml_ofi.ep);
    if (0 != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_enable failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Free providers info since it's not needed anymore.
     */
    fi_freeinfo(hints);
    hints = NULL;
    fi_freeinfo(providers);
    providers = NULL;

    /**
     * Get our address.
     */
    orte_rml_ofi.ep_namelen = sizeof(orte_rml_ofi.ep_name);
    ret = fi_getname((fid_t)orte_rml_ofi.ep,
                     &orte_rml_ofi.ep_name[0],
                     &orte_rml_ofi.ep_namelen);
    if (ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: fi_getname failed: %s\n",
                            __FILE__, __LINE__, fi_strerror(-ret));
        goto error;
    }

    /**
     * Set the ANY_SRC address.
     */
    orte_rml_ofi.any_addr = FI_ADDR_UNSPEC;

    /**
     * Activate progress callback.
     */
    ret = opal_progress_register(orte_rml_ofi_progress_no_inline);
    if (OMPI_SUCCESS != ret) {
        opal_output_verbose(1, ompi_mtl_base_framework.framework_output,
                            "%s:%d: opal_progress_register failed: %d\n",
                            __FILE__, __LINE__, ret);
        goto error;
    }

    return &orte_rml_ofi.base;

error:
    if (providers) {
        (void) fi_freeinfo(providers);
    }
    if (hints) {
        (void) fi_freeinfo(hints);
    }
    if (orte_rml_ofi.av) {
        (void) fi_close((fid_t)orte_rml_ofi.av);
    }
    if (orte_rml_ofi.cq) {
        (void) fi_close((fid_t)orte_rml_ofi.cq);
    }
    if (orte_rml_ofi.ep) {
        (void) fi_close((fid_t)orte_rml_ofi.ep);
    }
    if (orte_rml_ofi.domain) {
        (void) fi_close((fid_t)orte_rml_ofi.domain);
    }
    if (orte_rml_ofi.fabric) {
        (void) fi_close((fid_t)orte_rml_ofi.fabric);
    }
    return NULL;

    OBJ_CONSTRUCT(&orte_rml_ofi.exceptions, opal_list_t);

    init_done = true;
    return &orte_rml_ofi.super;
}

int
orte_rml_ofi_init(void)
{
    /* enable the base receive to get updates on contact info */
    orte_rml_base_comm_start();

    return ORTE_SUCCESS;
}


int
orte_rml_ofi_fini(void)
{
    opal_list_item_t *item;

    while (NULL !=
           (item = opal_list_remove_first(&orte_rml_ofi.exceptions))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&orte_rml_ofi.exceptions);

    /* clear the base receive */
    orte_rml_base_comm_stop();

    return ORTE_SUCCESS;
}

#if OPAL_ENABLE_FT_CR == 1
int
orte_rml_ofi_ft_event(int state) {
    int exit_status = ORTE_SUCCESS;
    int ret;

    if(OPAL_CRS_CHECKPOINT == state) {
        ORTE_ACTIVATE_JOB_STATE(NULL, ORTE_JOB_STATE_FT_CHECKPOINT);
    }
    else if(OPAL_CRS_CONTINUE == state) {
        ORTE_ACTIVATE_JOB_STATE(NULL, ORTE_JOB_STATE_FT_CONTINUE);
    }
    else if(OPAL_CRS_RESTART == state) {
        ORTE_ACTIVATE_JOB_STATE(NULL, ORTE_JOB_STATE_FT_RESTART);
    }
    else if(OPAL_CRS_TERM == state ) {
        ;
    }
    else {
        ;
    }


    if(OPAL_CRS_CHECKPOINT == state) {
        ;
    }
    else if(OPAL_CRS_CONTINUE == state) {
        ;
    }
    else if(OPAL_CRS_RESTART == state) {
        (void) mca_base_framework_close(&orte_ofi_base_framework);

        if (ORTE_SUCCESS != (ret = mca_base_framework_open(&orte_ofi_base_framework, 0))) {
            ORTE_ERROR_LOG(ret);
            exit_status = ret;
            goto cleanup;
        }

        if( ORTE_SUCCESS != (ret = orte_ofi_base_select())) {
            ORTE_ERROR_LOG(ret);
            exit_status = ret;
            goto cleanup;
        }
    }
    else if(OPAL_CRS_TERM == state ) {
        ;
    }
    else {
        ;
    }

 cleanup:
    return exit_status;
}
#else
int
orte_rml_ofi_ft_event(int state) {
    return ORTE_SUCCESS;
}
#endif
