/*
 * Copyright 2008-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <aerospike/as_cluster.h>
#include <aerospike/as_lookup.h>
#include <aerospike/as_partition.h>

#include <citrusleaf/cl_types.h>
#include <citrusleaf/cl_object.h>
#include <citrusleaf/cl_write.h>
#include <citrusleaf/cl_info.h>
#include <citrusleaf/cl_kv.h>
#include <citrusleaf/cl_object.h>
#include <citrusleaf/cl_scan.h>
#include <citrusleaf/cl_batch.h>

/******************************************************************************
 * FUNCTIONS
 ******************************************************************************/

/**
 * All citrusleaf functions return an integer. This integer is 0 if the
 * call has succeeded, and a negative number if it has failed.
 * All returns of pointers and objects are done through the parameters.
 * (When in C++, use & parameters for return, but we're not there yet)
 *
 * 'void' return functions are only used for functions that are syntactically
 * unable to fail.
 */

#define INFO_TIMEOUT_MS 500

/**
 * Call this init function sometime early, create our mutexes and a few other things.
 * We'd prefer if this is only called once
 */
int citrusleaf_init(void);

void citrusleaf_change_tend_speed(int secs);

/**
 * If you wish to free up resources used by the citrusleaf client in your process,
 * call this - all cl_conn will be invalid, and you'll have to call citrusleaf_init
 * again to do anything
 */
void citrusleaf_shutdown(void);

/**
 * This call will print stats to stderr
 */
void citrusleaf_print_stats(void);

struct cl_cluster_compression_stat_s;
typedef struct cl_cluster_compression_stat_s cl_cluster_compression_stat;

extern void citrusleaf_cluster_put_compression_stat(as_cluster *asc, uint64_t actual_sz, uint64_t compressed_sz);
extern void citrusleaf_cluster_get_compression_stat(as_cluster *asc, uint64_t *actual_sz, uint64_t *compressed_sz);

/*
 * Set minimum size of packet, above which packet will be compressed before sending on wire,
 * provided compression is enabled.
 */
int
citrusleaf_cluster_change_compression_threshold(as_cluster *asc, int size_in_bytes);

/**
 * This call allows the caller to specify the operation - read, write, add, etc.  Multiple operations
 * can be specified in a single call.
 */

cl_rv citrusleaf_operate(as_cluster *asc, const char *ns, const char *set, const cl_object *key, cf_digest *d, cl_bin **values, cl_operation *operations, int *n_values, const cl_write_parameters *cl_w_p, uint32_t *generation, uint32_t* ttl, int consistency_level, int commit_level, as_policy_replica replica);

/**
 * This debugging call can be useful for tracking down errors and coordinating with server failures
 * gets the digest for a particular set and key
 */
int citrusleaf_calculate_digest(const char *set, const cl_object *key, cf_digest *digest);

#ifdef __cplusplus
} // end extern "C"
#endif

