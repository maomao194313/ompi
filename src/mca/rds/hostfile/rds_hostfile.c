/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#include "orte_config.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>

#include "include/orte_constants.h"
#include "class/ompi_list.h"
#include "util/output.h"
#include "util/sys_info.h"
#include "mca/mca.h"
#include "mca/base/base.h"
#include "mca/ns/ns.h"
#include "mca/ras/base/ras_base_node.h"
#include "mca/errmgr/errmgr.h"
#include "rds_hostfile.h"
#include "rds_hostfile_lex.h"

#include "runtime/runtime_types.h"

/*
 * Local global variable
 */
static bool localhost_found;

static void orte_rds_hostfile_parse_error(void)
{
    ompi_output(0, "Error reading hostfile at line %d: %s\n",
        orte_rds_hostfile_line, orte_rds_hostfile_value.sval);
}


static int orte_rds_hostfile_parse_int(void)
{
    if (ORTE_RDS_HOSTFILE_EQUAL != orte_rds_hostfile_lex()) 
        return -1;
    if (ORTE_RDS_HOSTFILE_INT != orte_rds_hostfile_lex()) 
        return -1;
    return orte_rds_hostfile_value.ival;
}


static orte_ras_base_node_t* orte_rds_hostfile_lookup(ompi_list_t* nodes, const char* name)
{
    ompi_list_item_t* item;
    for(item =  ompi_list_get_first(nodes);
        item != ompi_list_get_end(nodes);
        item =  ompi_list_get_next(item)) {
        orte_ras_base_node_t* node = (orte_ras_base_node_t*)item;
        if(strcmp(node->node_name, name) == 0) {
            ompi_list_remove_item(nodes, item);
            return node;
        }
    }
    return NULL;
}

static int orte_rds_hostfile_parse_line(int token, ompi_list_t* existing, ompi_list_t* updates)
{
    int rc;
    orte_ras_base_node_t* node;
    bool update = false;
    bool got_count = false;

    if (ORTE_RDS_HOSTFILE_STRING == token) {
        char* node_name = orte_rds_hostfile_value.sval;

        /* If a line for "localhost" was included, we do NOT allow
         * any other hosts to be specified in the file. This is due to the
         * vaguery of the "nodename" parameter returned by Linux system calls.
         * See the man page for uname for a detailed explanation
         */
        if (0 == strcmp(node_name, "localhost")) {

            /* If the size of the updates list == 1 and it only
               contains localhost, or if the size of the updates list
               == 0, we're ok.  Otherwise, this is an error.  The
               positive logic test was a little clearer than a
               negative logic check, so even though this results in
               potentially re-setting localhost_found=true multiple
               times (if "localhost" is included multiple times in the
               file), the code is clearer this way. */

            if (0 == ompi_list_get_size(updates) ||
                (1 == ompi_list_get_size(updates) && localhost_found)) {
                localhost_found = true;
            } else {
                ORTE_ERROR_LOG(ORTE_ERR_VALUE_OUT_OF_BOUNDS);
                return ORTE_ERR_VALUE_OUT_OF_BOUNDS;
            }
        } else if (localhost_found) {
            ORTE_ERROR_LOG(ORTE_ERR_VALUE_OUT_OF_BOUNDS);
            return ORTE_ERR_VALUE_OUT_OF_BOUNDS;
        }

        /* Do we need to make a new node object?  First check to see
           if it's in the existing list. */
        
        if (NULL == (node = orte_rds_hostfile_lookup(existing, node_name))) {

            /* If it wasn't, see if it's already in the updates list */

            if (NULL == (node = orte_rds_hostfile_lookup(updates, 
                                                         node_name))) {
                node = OBJ_NEW(orte_ras_base_node_t);
                node->node_name = strdup(node_name);
                node->node_slots = 0;
            }

            /* Note that we need to set update to true regardless of
               whether the node was found on the updates list or not.
               If it was found, we just removed it (in
               orte_rds_hostfile_lookup()), so the update puts it back
               (potentially after updating it, of course).  If it was
               not found, then we have a new node instance that needs
               to be added to the updates list. */

            update = true;
        }
    } else {
        orte_rds_hostfile_parse_error();
        return OMPI_ERROR;
    }

    got_count = false;
    while (!orte_rds_hostfile_done) {
        token = orte_rds_hostfile_lex();
        switch (token) {
        case ORTE_RDS_HOSTFILE_DONE:
            goto done;

        case ORTE_RDS_HOSTFILE_NEWLINE:
            goto done;

        case ORTE_RDS_HOSTFILE_COUNT:
        case ORTE_RDS_HOSTFILE_CPU:
        case ORTE_RDS_HOSTFILE_SLOTS:
            rc = orte_rds_hostfile_parse_int();
            if (rc < 0) {
                OBJ_RELEASE(node);
                return OMPI_ERROR;
            }
            node->node_slots += rc;
            update = true;
            got_count = true;

            /* Ensure that node_slots_max >= node_slots */
            if (node->node_slots_max != 0 && node->node_slots_max < node->node_slots) {
                node->node_slots_max = node->node_slots;
            }
            break;

        case ORTE_RDS_HOSTFILE_SLOTS_MAX:
            rc = orte_rds_hostfile_parse_int();
            if (rc < 0) {
                OBJ_RELEASE(node);
                return OMPI_ERROR;
            }
            /* Only take this update if it puts us >= node_slots */
            if (((size_t) rc) >= node->node_slots) {
                if (node->node_slots_max != (size_t)rc) {
                    node->node_slots_max = rc;
                    update = true;
                }
            } else {
                ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
                OBJ_RELEASE(node);
                return OMPI_ERROR;
            }
            break;

        default:
            orte_rds_hostfile_parse_error();
            OBJ_RELEASE(node);
            return OMPI_ERROR;
        }
    }

done:
    if (update) {
        if (!got_count) {
            ++node->node_slots;
        }
        ompi_list_append(updates, &node->super);
    } else {
        OBJ_RELEASE(node);
    }
    return OMPI_SUCCESS;
}


/**
 * Parse the specified file into a node list.
 */

static int orte_rds_hostfile_parse(const char *hostfile, ompi_list_t* existing, ompi_list_t* updates)
{
    int token;
    int rc = ORTE_SUCCESS;

    OMPI_LOCK(&mca_rds_hostfile_component.lock);

    orte_rds_hostfile_done = false;
    orte_rds_hostfile_in = fopen(hostfile, "r");
    if (NULL == orte_rds_hostfile_in) {
        rc = ORTE_ERR_NOT_FOUND;
        goto unlock;
    }

    while (!orte_rds_hostfile_done) {
        token = orte_rds_hostfile_lex();
        switch (token) {
        case ORTE_RDS_HOSTFILE_DONE:
            orte_rds_hostfile_done = true;
            break;

        case ORTE_RDS_HOSTFILE_NEWLINE:
            break;

        case ORTE_RDS_HOSTFILE_STRING:
            rc = orte_rds_hostfile_parse_line(token, existing, updates);
            if (ORTE_SUCCESS != rc) {
                goto unlock;
            }
            break;

        default:
            orte_rds_hostfile_parse_error();
            goto unlock;
        }
    }
    fclose(orte_rds_hostfile_in);
    orte_rds_hostfile_in = NULL;

unlock:
    OMPI_UNLOCK(&mca_rds_hostfile_component.lock);
    return rc;
}


/**
 * Parse the default file as specified by the MCA parameter,
 * rds_hostfile_path, and add the nodes to the registry.
 */

static int orte_rds_hostfile_query(void)
{
    ompi_list_t existing;
    ompi_list_t updates;
    ompi_list_item_t *item;
    int rc;
    
    /* initialize the localhost_found flag */
    localhost_found = false;

    OBJ_CONSTRUCT(&existing, ompi_list_t);
    OBJ_CONSTRUCT(&updates, ompi_list_t);
    rc = orte_ras_base_node_query(&existing);
    if(ORTE_SUCCESS != rc) {
        goto cleanup;
    }

    rc = mca_base_param_find("rds", "hostfile", "path");
    mca_base_param_lookup_string(rc, &mca_rds_hostfile_component.path);
    rc = orte_rds_hostfile_parse(mca_rds_hostfile_component.path, &existing, &updates);
    if (ORTE_ERR_NOT_FOUND == rc) {
        if(mca_rds_hostfile_component.default_hostfile) {
            rc = ORTE_SUCCESS;
        } else {
            ompi_output(0, "orte_rds_hostfile: could not open %s\n", mca_rds_hostfile_component.path);
        }
        goto cleanup;
    } else if (ORTE_SUCCESS != rc) {
        goto cleanup;
    }
    if(ompi_list_get_size(&updates)) {
        rc = orte_ras_base_node_insert(&updates);
    }
   
cleanup:
    if (NULL != mca_rds_hostfile_component.path) {
        free(mca_rds_hostfile_component.path);
        mca_rds_hostfile_component.path = NULL;
    }
    
    while(NULL != (item = ompi_list_remove_first(&existing))) {
        OBJ_RELEASE(item);
    }

    while(NULL != (item = ompi_list_remove_first(&updates))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&existing);
    OBJ_DESTRUCT(&updates);
    return rc;
}


static int orte_rds_hostfile_finalize(void)
{
    return ORTE_SUCCESS;
}


orte_rds_base_module_t orte_rds_hostfile_module = {
    orte_rds_hostfile_query,
    orte_rds_hostfile_finalize
};

