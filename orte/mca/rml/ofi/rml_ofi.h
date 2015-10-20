/*
 * Copyright (c) 2015      Intel, Inc. All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_RML_OFI_RML_OFI_H
#define MCA_RML_OFI_RML_OFI_H

#include "orte_config.h"

#include "opal/dss/dss_types.h"
#include "opal/mca/event/event.h"

#include "orte/mca/rml/base/base.h"

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_tagged.h>

BEGIN_C_DECLS

typedef struct {
    struct orte_rml_module_t super;

    /** Fabric Domain handle */
    struct fid_fabric *fabric;

    /** Access Domain handle */
    struct fid_domain *domain;

    /** Address vector handle */
    struct fid_av *av;

    /** Completion queue handle */
    struct fid_cq *cq;

    /** Endpoint to communicate on */
    struct fid_ep *ep;

    /** Endpoint name */
    char ep_name[FI_NAME_MAX];

    /** Endpoint name length */
    size_t epnamelen;

    /** "Any source" address */
    fi_addr_t any_addr;

    opal_list_t              exceptions;
    opal_list_t              queued_routing_messages;
    opal_event_t            *timer_event;
    struct timeval           timeout;
} orte_rml_ofi_module_t;

ORTE_MODULE_DECLSPEC extern orte_rml_component_t mca_rml_ofi_component;
extern orte_rml_ofi_module_t orte_rml_ofi;

int orte_rml_ofi_init(void);
int orte_rml_ofi_fini(void);
int orte_rml_ofi_ft_event(int state);

int orte_rml_ofi_send_nb(orte_process_name_t* peer,
                         struct iovec* msg,
                         int count,
                         orte_rml_tag_t tag,
                         orte_rml_callback_fn_t cbfunc,
                         void* cbdata);

int orte_rml_ofi_send_buffer_nb(orte_process_name_t* peer,
                                opal_buffer_t* buffer,
                                orte_rml_tag_t tag,
                                orte_rml_buffer_callback_fn_t cbfunc,
                                void* cbdata);

void orte_rml_ofi_recv_nb(orte_process_name_t* peer,
                          orte_rml_tag_t tag,
                          bool persistent,
                          orte_rml_callback_fn_t cbfunc,
                          void* cbdata);

void orte_rml_ofi_recv_buffer_nb(orte_process_name_t* peer,
                                 orte_rml_tag_t tag,
                                 bool persistent,
                                 orte_rml_buffer_callback_fn_t cbfunc,
                                 void* cbdata);

void orte_rml_ofi_recv_cancel(orte_process_name_t* peer,
                              orte_rml_tag_t tag);

int orte_rml_ofi_open_channel(orte_process_name_t * peer,
                              opal_list_t * qos_attributes,
                              orte_rml_channel_callback_fn_t cbfunc,
                              void *cbdata);

int orte_rml_ofi_send_channel_nb(orte_rml_channel_num_t channel,
                                 struct iovec* msg,
                                 int count,
                                 orte_rml_tag_t tag,
                                 orte_rml_send_channel_callback_fn_t cbfunc,
                                 void* cbdata);

int orte_rml_ofi_send_buffer_channel_nb(orte_rml_channel_num_t channel,
                                        opal_buffer_t *buffer,
                                        orte_rml_tag_t tag,
                                        orte_rml_send_buffer_channel_callback_fn_t cbfunc,
                                        void* cbdata);

int orte_rml_ofi_close_channel(orte_rml_channel_num_t channel,
                               orte_rml_channel_callback_fn_t cbfunc,
                               void* cbdata);

int orte_rml_ofi_ping(const char* uri,
                      const struct timeval* tv);

char* orte_rml_ofi_get_uri(void);
void orte_rml_ofi_set_uri(const char*);

int orte_rml_ofi_add_exception(orte_rml_exception_callback_t cbfunc);
int orte_rml_ofi_del_exception(orte_rml_exception_callback_t cbfunc);
void orte_rml_ofi_exception_callback(orte_process_name_t *peer,
                                     orte_rml_exception_t exception);


void orte_rml_ofi_purge(orte_process_name_t *peer);

END_C_DECLS

#endif
