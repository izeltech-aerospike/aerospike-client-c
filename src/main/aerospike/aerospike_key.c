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
#include <aerospike/aerospike.h>
#include <aerospike/aerospike_key.h>

#include <aerospike/as_bin.h>
#include <aerospike/as_buffer.h>
#include <aerospike/as_error.h>
#include <aerospike/as_key.h>
#include <aerospike/as_list.h>
#include <aerospike/as_log.h>
#include <aerospike/as_operations.h>
#include <aerospike/as_policy.h>
#include <aerospike/as_record.h>
#include <aerospike/as_status.h>

#include <aerospike/as_msgpack.h>
#include <aerospike/as_serializer.h>

#include <citrusleaf/citrusleaf.h>
#include <citrusleaf/cl_object.h>
#include <citrusleaf/cl_write.h>
#include <citrusleaf/cf_proto.h>

#include "_shim.h"

#include "../citrusleaf/internal.h"

/******************************************************************************
 * FUNCTIONS
 *****************************************************************************/

/**
 *	Look up a record by key, then return all bins.
 *	
 *	@param as			The aerospike instance to use for this operation.
 *	@param err			The as_error to be populated if an error occurs.
 *	@param policy		The policy to use for this operation. If NULL, then the default policy will be used.
 *	@param key			The key of the record.
 *	@param rec 			The record to be populated with the data from request.
 *
 *	@return AEROSPIKE_OK if successful. Otherwise an error.
 */
as_status aerospike_key_get(
	aerospike * as, as_error * err, const as_policy_read * policy, 
	const as_key * key, 
	as_record ** rec) 
{
	// we want to reset the error so, we have a clean state
	as_error_reset(err);
	
	if (! policy) {
		policy = &as->config.policies.read;
	}

	uint32_t    timeout = policy->timeout == UINT32_MAX ? 0 : policy->timeout;
	uint32_t    gen = 0;
	uint32_t 	ttl = 0;
	int         nvalues = 0;
	cl_bin *    values = NULL;

	cl_rv rc = AEROSPIKE_OK;

	int consistency_level = 0;
	switch ( policy->consistency_level ) {
		case AS_POLICY_CONSISTENCY_LEVEL_ONE:
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B0;
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B1;
			break;
		case AS_POLICY_CONSISTENCY_LEVEL_ALL:
			consistency_level |= CL_MSG_INFO1_CONSISTENCY_LEVEL_B0;
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B1;
			break;
		default: {
			// ERROR CASE
			break;
		}
	}

	switch ( policy->key ) {
		case AS_POLICY_KEY_DIGEST: {
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_get_all(as->cluster, key->ns, key->set, NULL, (cf_digest*)digest->value,
					&values, &nvalues, timeout, &gen, &ttl, consistency_level, policy->replica);
			break;
		}
		case AS_POLICY_KEY_SEND: {
			cl_object okey;
			asval_to_clobject((as_val *) key->valuep, &okey);
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_get_all(as->cluster, key->ns, key->set, &okey, (cf_digest*)digest->value,
					&values, &nvalues, timeout, &gen, &ttl, consistency_level, policy->replica);
			break;
		}
		default: {
			// ERROR CASE
			break;
		}
	}

	if ( rc == AEROSPIKE_OK && rec != NULL ) {
		as_record * r = *rec;
		if ( r == NULL ) {
			r = as_record_new(0);
		}
		if ( r->bins.entries == NULL ) {
			r->bins.capacity = nvalues;
			r->bins.size = 0;
			r->bins.entries = malloc(sizeof(as_bin) * nvalues);
			r->bins._free = true;
		}
		clbins_to_asrecord(values, nvalues, r);
		r->gen = (uint16_t) gen;
		r->ttl = ttl;
		*rec = r;
	}

	if ( values != NULL ) {
		// We are freeing the bins' objects, as opposed to bins themselves.
		citrusleaf_bins_free(values, nvalues);
		free(values);
	}

	return as_error_fromrc(err,rc);
}

/**
 *	Lookup a record by key, then return specified bins.
 *	
 *	@param as			The aerospike instance to use for this operation.
 *	@param err			The as_error to be populated if an error occurs.
 *	@param policy		The policy to use for this operation. If NULL, then the default policy will be used.
 *	@param key			The key of the record.
 *	@param bins			The bins to select. A NULL terminated array of NULL terminated strings.
 *	@param rec 			The record to be populated with the data from request.
 *
 *	@return AEROSPIKE_OK if successful. Otherwise an error.
 */
as_status aerospike_key_select(
	aerospike * as, as_error * err, const as_policy_read * policy, 
	const as_key * key, const char * bins[], 
	as_record ** rec) 
{
	// we want to reset the error so, we have a clean state
	as_error_reset(err);
	
	if (! policy) {
		policy = &as->config.policies.read;
	}

	uint32_t    timeout = policy->timeout == UINT32_MAX ? 0 : policy->timeout;
	uint32_t    gen = 0;
	uint32_t 	ttl = 0;
	int         nvalues = 0;
	cl_bin *    values = NULL;

	for (nvalues = 0; bins[nvalues] != NULL && bins[nvalues][0] != '\0'; nvalues++)
		;

	values = (cl_bin *) alloca(sizeof(cl_bin) * nvalues);
	for ( int i = 0; i < nvalues; i++ ) {
		if ( strlen(bins[i]) > AS_BIN_NAME_MAX_LEN ) {
			return as_error_update(err, AEROSPIKE_ERR_PARAM, "bin name too long: %s", bins[i]);
		}

		strcpy(values[i].bin_name, bins[i]);
		citrusleaf_object_init(&values[i].object);
	}

	cl_rv rc = AEROSPIKE_OK;

	int consistency_level = 0;
	switch ( policy->consistency_level ) {
		case AS_POLICY_CONSISTENCY_LEVEL_ONE:
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B0;
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B1;
			break;
		case AS_POLICY_CONSISTENCY_LEVEL_ALL:
			consistency_level |= CL_MSG_INFO1_CONSISTENCY_LEVEL_B0;
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B1;
			break;
		default: {
			// ERROR CASE
			break;
		}
	}

	switch ( policy->key ) {
		case AS_POLICY_KEY_DIGEST: {
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_get(as->cluster, key->ns, key->set, NULL, (cf_digest*)digest->value,
					values, nvalues, timeout, &gen, &ttl, consistency_level, policy->replica);
			break;
		}
		case AS_POLICY_KEY_SEND: {
			cl_object okey;
			asval_to_clobject((as_val *) key->valuep, &okey);
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_get(as->cluster, key->ns, key->set, &okey, (cf_digest*)digest->value,
					values, nvalues, timeout, &gen, &ttl, consistency_level, policy->replica);
			break;
		}
		default: {
			// ERROR CASE
			break;
		}
	}

	if ( rc == AEROSPIKE_OK && rec != NULL ) {
		as_record * r = *rec;
		if ( r == NULL ) {
			r = as_record_new(0);
		}
		if ( r->bins.entries == NULL ) {
			r->bins.capacity = nvalues;
			r->bins.size = 0;
			r->bins.entries = malloc(sizeof(as_bin) * nvalues);
			r->bins._free = true;
		}
		clbins_to_asrecord(values, nvalues, r);
		r->gen = (uint16_t) gen;
		r->ttl = ttl;
		*rec = r;
	}

	if ( values != NULL ) {
		// We are freeing the bins' objects, as opposed to bins themselves.
		citrusleaf_bins_free(values, nvalues);
	}

	return as_error_fromrc(err,rc);
}

/**
 *	Check if a record exists in the cluster via its key.
 *	
 *	@param as			The aerospike instance to use for this operation.
 *	@param err			The as_error to be populated if an error occurs.
 *	@param policy		The policy to use for this operation. If NULL, then the default policy will be used.
 *	@param key			The key of the record.
 *	@param record       The record to populated with metadata if record exists, otherwise NULL
 *
 *	@return AEROSPIKE_OK if successful. Otherwise an error.
 */
as_status aerospike_key_exists(
	aerospike * as, as_error * err, const as_policy_read * policy, 
	const as_key * key, 
	as_record ** rec) 
{
	// we want to reset the error so, we have a clean state
	as_error_reset(err);
	
	if (! policy) {
		policy = &as->config.policies.read;
	}

	uint32_t    timeout = policy->timeout == UINT32_MAX ? 0 : policy->timeout;
	uint32_t	gen = 0;
	uint32_t	ttl = 0; // TODO - a version of 'exists' that returns all metadata
	int     	nvalues = 0;
	cl_bin *	values = NULL;
	
	cl_rv rc = AEROSPIKE_OK;

	int consistency_level = 0;
	switch ( policy->consistency_level ) {
		case AS_POLICY_CONSISTENCY_LEVEL_ONE:
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B0;
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B1;
			break;
		case AS_POLICY_CONSISTENCY_LEVEL_ALL:
			consistency_level |= CL_MSG_INFO1_CONSISTENCY_LEVEL_B0;
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B1;
			break;
		default: {
			// ERROR CASE
			break;
		}
	}

	switch ( policy->key ) {
		case AS_POLICY_KEY_DIGEST: {
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_exists_key(as->cluster, key->ns, key->set, NULL, (cf_digest*)digest->value,
					values, nvalues, timeout, &gen, &ttl, consistency_level, policy->replica);
			break;
		}
		case AS_POLICY_KEY_SEND: {
			cl_object okey;
			asval_to_clobject((as_val *) key->valuep, &okey);
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_exists_key(as->cluster, key->ns, key->set, &okey, (cf_digest*)digest->value,
					values, nvalues, timeout, &gen, &ttl, consistency_level, policy->replica);
			break;
		}
		default: {
			// ERROR CASE
			break;
		}
	}

	if ( values != NULL ) {
		// We are freeing the bins' objects, as opposed to bins themselves.
		citrusleaf_bins_free(values, nvalues);
		free(values);
	}

	switch(rc) {
		case AEROSPIKE_OK: 
			{
				as_record * r = *rec;
				if ( r == NULL ) { 
					r = as_record_new(0);
				}   
				r->gen = (uint16_t) gen;
				r->ttl = ttl;
				*rec = r;
				break;
			}

		default:
			*rec = NULL;
			break;
	}

	return as_error_fromrc(err,rc);
}

/**
 *	Store a record in the cluster.  Note that the TTL (time to live) value
 *	is specified inside of the rec (as_record) object.
 *	
 *	@param as			The aerospike instance to use for this operation.
 *	@param err			The as_error to be populated if an error occurs.
 *	@param policy		The policy to use for this operation. If NULL, then the default policy will be used.
 *	@param key			The key of the record.
 *	@param rec 			The record containing the data to be written.
 *
 *	@return AEROSPIKE_OK if successful. Otherwise an error.
 */
as_status aerospike_key_put(
	aerospike * as, as_error * err, const as_policy_write * policy, 
	const as_key * key, as_record * rec) 
{
	// we want to reset the error so, we have a clean state
	as_error_reset(err);
	
	if (! policy) {
		policy = &as->config.policies.write;
	}

	cl_write_parameters wp;
	aspolicywrite_to_clwriteparameters(policy, rec, &wp);

	int			nvalues	= rec->bins.size;
	cl_bin *	values	= (cl_bin *) alloca(sizeof(cl_bin) * nvalues);

	asrecord_to_clbins(rec, values, nvalues);

	cl_rv rc = AEROSPIKE_OK;

	int commit_level = 0;
	switch ( policy->commit_level ) {
		case AS_POLICY_COMMIT_LEVEL_ALL:
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B0;
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B1;
			break;
		case AS_POLICY_COMMIT_LEVEL_MASTER:
			commit_level |= CL_MSG_INFO3_COMMIT_LEVEL_B0;
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B1;
			break;
		default: {
			// ERROR CASE
			break;
		}
	}

	switch ( policy->key ) {
		case AS_POLICY_KEY_DIGEST: {
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_put(as->cluster, key->ns, key->set, NULL, (cf_digest*)digest->value, values, nvalues, &wp, commit_level);
			break;
		}
		case AS_POLICY_KEY_SEND: {
			cl_object okey;
			asval_to_clobject((as_val *) key->valuep, &okey);
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_put(as->cluster, key->ns, key->set, &okey, (cf_digest*)digest->value, values, nvalues, &wp, commit_level);
			break;
		}
		default: {
			// ERROR CASE
			break;
		}
	}

	// We are freeing the bins' objects, as opposed to bins themselves.
	citrusleaf_bins_free(values, nvalues);

	return as_error_fromrc(err,rc); 
}

/**
 *	Remove a record from the cluster.
 *
 *	~~~~~~~~~~{.c}
 *		as_key key;
 *		as_key_init(&key, "ns", "set", "key");
 *
 *		if ( aerospike_key_remove(&as, &err, NULL, &key) != AEROSPIKE_OK ) {
 *			fprintf(stderr, "error(%d) %s at [%s:%d]", err.code, err.message, err.file, err.line);
 *		}
 *	~~~~~~~~~~
 *
 *	@param as			The aerospike instance to use for this operation.
 *	@param err			The as_error to be populated if an error occurs.
 *	@param policy		The policy to use for this operation. If NULL, then the default policy will be used.
 *	@param key			The key of the record.
 *
 *	@return AEROSPIKE_OK if successful. Otherwise an error.
 */
as_status aerospike_key_remove(
	aerospike * as, as_error * err, const as_policy_remove * policy, 
	const as_key * key) 
{
	// we want to reset the error so, we have a clean state
	as_error_reset(err);
	
	if (! policy) {
		policy = &as->config.policies.remove;
	}

	cl_write_parameters wp;
	aspolicyremove_to_clwriteparameters(policy, &wp);

	cl_rv rc = AEROSPIKE_OK;

	int commit_level = 0;
	switch ( policy->commit_level ) {
		case AS_POLICY_COMMIT_LEVEL_ALL:
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B0;
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B1;
			break;
		case AS_POLICY_COMMIT_LEVEL_MASTER:
			commit_level |= CL_MSG_INFO3_COMMIT_LEVEL_B0;
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B1;
			break;
		default: {
			// ERROR CASE
			break;
		}
	}

	switch ( policy->key ) {
		case AS_POLICY_KEY_DIGEST: {
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_delete(as->cluster, key->ns, key->set, NULL, (cf_digest*)digest->value, &wp, commit_level);
			break;
		}
		case AS_POLICY_KEY_SEND: {
			cl_object okey;
			asval_to_clobject((as_val *) key->valuep, &okey);
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_delete(as->cluster, key->ns, key->set, &okey, (cf_digest*)digest->value, &wp, commit_level);
			break;
		}
		default: {
			// ERROR CASE
			break;
		}
	}

	return as_error_fromrc(err,rc);
}

/**
 *	Lookup a record by key, then perform specified operations.
 *
 *	~~~~~~~~~~{.c}
 *		as_key key;
 *		as_key_init(&key, "ns", "set", "key");
 *
 *		as_operations ops;
 *		as_operations_inita(&ops,2);
 *		as_operations_append_int64(&ops, AS_OPERATOR_INCR, "bin1", 456);
 *		as_operations_append_str(&ops, AS_OPERATOR_APPEND, "bin1", "def");
 *
 *		if ( aerospike_key_remove(&as, &err, NULL, &key, &ops) != AEROSPIKE_OK ) {
 *			fprintf(stderr, "error(%d) %s at [%s:%d]", err.code, err.message, err.file, err.line);
 *		}
 *	~~~~~~~~~~
 *	
 *	@param as			The aerospike instance to use for this operation.
 *	@param err			The as_error to be populated if an error occurs.
 *	@param policy		The policy to use for this operation. If NULL, then the default policy will be used.
 *	@param key			The key of the record.
 *	@param ops			The operations to perform on the record.
 *	@param rec			The record to be populated with the data from AS_OPERATOR_READ operations.
 *
 *	@return AEROSPIKE_OK if successful. Otherwise an error.
 */
as_status aerospike_key_operate(
	aerospike * as, as_error * err, const as_policy_operate * policy, 
	const as_key * key, const as_operations * ops,
	as_record ** rec)
{
	// we want to reset the error so, we have a clean state
	as_error_reset(err);
	
	if (! policy) {
		policy = &as->config.policies.operate;
	}

	cl_write_parameters wp;
	aspolicyoperate_to_clwriteparameters(policy, ops, &wp);

	uint32_t 		gen = 0;
	uint32_t 		ttl = 0;
	int 			n_operations = ops->binops.size;
	cl_operation * 	operations = (cl_operation *) alloca(sizeof(cl_operation) * n_operations);
	int				n_read_ops = 0;

	for (int i = 0; i < n_operations; i++) {
		cl_operation * clop = &operations[i];
		as_binop * op = &ops->binops.entries[i];

		// Length check already done on as_bin name length. For performance we
		// we'll leave out this down-size check since this is a temporary shim
		// and we know the CL and AS limits are the same...
//		if ( strlen(op->bin.name) > CL_BINNAME_SIZE - 1 ) {
//			return as_error_update(err, AEROSPIKE_ERR_PARAM, "bin name too long: %s", op->bin.name);
//		}

		strcpy(clop->bin.bin_name, op->bin.name);
		clop->op = (cl_operator)op->op;

		// Collect bin names that are read.
		if (op->op == AS_OPERATOR_READ) {
			n_read_ops++;
		}

		asbinvalue_to_clobject(op->bin.valuep, &clop->bin.object);
	}

	int consistency_level = 0;
	switch ( policy->consistency_level ) {
		case AS_POLICY_CONSISTENCY_LEVEL_ONE:
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B0;
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B1;
			break;
		case AS_POLICY_CONSISTENCY_LEVEL_ALL:
			consistency_level |= CL_MSG_INFO1_CONSISTENCY_LEVEL_B0;
			consistency_level &= ~CL_MSG_INFO1_CONSISTENCY_LEVEL_B1;
			break;
		default: {
			// ERROR CASE
			break;
		}
	}

	int commit_level = 0;
	switch ( policy->commit_level ) {
		case AS_POLICY_COMMIT_LEVEL_ALL:
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B0;
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B1;
			break;
		case AS_POLICY_COMMIT_LEVEL_MASTER:
			commit_level |= CL_MSG_INFO3_COMMIT_LEVEL_B0;
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B1;
			break;
		default: {
			// ERROR CASE
			break;
		}
	}

	cl_rv rc = AEROSPIKE_OK;
	cl_bin *result_bins = NULL;

	switch ( policy->key ) {
		case AS_POLICY_KEY_DIGEST: {
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_operate(as->cluster, key->ns, key->set, NULL, (cf_digest*)digest->value,
					&result_bins, operations, &n_operations, &wp, &gen, &ttl, consistency_level, commit_level, policy->replica);
			break;
		}
		case AS_POLICY_KEY_SEND: {
			cl_object okey;
			asval_to_clobject((as_val *) key->valuep, &okey);
			as_digest * digest = as_key_digest((as_key *) key);
			rc = citrusleaf_operate(as->cluster, key->ns, key->set, &okey, (cf_digest*)digest->value,
					&result_bins, operations, &n_operations, &wp, &gen, &ttl, consistency_level, commit_level, policy->replica);
			break;
		}
		default: {
			// ERROR CASE
			break;
		}
	}

	for (int i = 0; i < n_operations; i++) {
		citrusleaf_object_free(&operations[i].bin.object);
	}

    if (n_read_ops != n_operations) {
		if (result_bins) {
			citrusleaf_bins_free(result_bins, n_operations);
			free(result_bins);
		}

		return as_error_update(err, AEROSPIKE_ERR, "expected %d bins, got %d", n_read_ops, n_operations);
	}

	if ( n_read_ops != 0 && rc == AEROSPIKE_OK && rec != NULL ) {
		as_record * r = *rec;
		if ( r == NULL ) {
			r = as_record_new(0);
		}
		if ( r->bins.entries == NULL ) {
			r->bins.capacity = n_operations;
			r->bins.size = 0;
			r->bins.entries = malloc(sizeof(as_bin) * n_operations);
			r->bins._free = true;
		}
		clbins_to_asrecord(result_bins, n_operations, r);
		r->gen = (uint16_t) gen;
		r->ttl = ttl;

		*rec = r;
	}

	if (result_bins) {
		citrusleaf_bins_free(result_bins, n_operations);
		free(result_bins);
	}

	return as_error_fromrc(err,rc);
}

/**
 *	Lookup a record by key, then apply the UDF.
 *
 *	~~~~~~~~~~{.c}
 *		as_key key;
 *		as_key_init(&key, "ns", "set", "key");
 *
 *		as_arraylist args;
 *		as_arraylist_init(&args, 2, 0);
 *		as_arraylist_append_int64(&args, 1);
 *		as_arraylist_append_int64(&args, 2);
 *		
 *		as_val * res = NULL;
 *		
 *		if ( aerospike_key_apply(&as, &err, NULL, &key, "math", "add", &args, &res) != AEROSPIKE_OK ) {
 *			fprintf(stderr, "error(%d) %s at [%s:%d]", err.code, err.message, err.file, err.line);
 *		}
 *		else {
 *			as_val_destroy(res);
 *		}
 *		
 *		as_arraylist_destroy(&args);
 *	~~~~~~~~~~
 *
 *
 *	@param as			The aerospike instance to use for this operation.
 *	@param err			The as_error to be populated if an error occurs.
 *	@param policy		The policy to use for this operation. If NULL, then the default policy will be used.
 *	@param key			The key of the record.
 *	@param module		The module containing the function to execute.
 *	@param function 	The function to execute.
 *	@param arglist 		The arguments for the function.
 *	@param result 		The return value from the function.
 *
 *	@return AEROSPIKE_OK if successful. Otherwise an error.
 */
as_status aerospike_key_apply(
	aerospike * as, as_error * err, const as_policy_apply * policy, 
	const as_key * key, 
	const char * module, const char * function, as_list * arglist, 
	as_val ** result) 
{
	// we want to reset the error so, we have a clean state
	as_error_reset(err);
	
	if (! policy) {
		policy = &as->config.policies.apply;
	}

	cl_write_parameters wp;
	cl_write_parameters_set_default(&wp);
	wp.timeout_ms = policy->timeout == UINT32_MAX ? 0 : policy->timeout;

	cl_object okey;
	asval_to_clobject((as_val *) key->valuep, &okey);

	as_serializer ser;
	as_msgpack_init(&ser);

	as_string file;
	as_string_init(&file, (char *) module, true /*ismalloc*/);

	as_string func;
	as_string_init(&func, (char *) function, true /*ismalloc*/);
	
	as_buffer args;
	as_buffer_init(&args);

	as_serializer_serialize(&ser, (as_val *) arglist, &args);

	as_call call = {
		.file = &file,
		.func = &func,
		.args = &args
	};

	uint64_t trid = 0;
	cl_bin * bins = 0;
	int n_bins = 0;

	cl_rv rc = AEROSPIKE_OK;

	int commit_level = 0;
	switch ( policy->commit_level ) {
		case AS_POLICY_COMMIT_LEVEL_ALL:
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B0;
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B1;
			break;
		case AS_POLICY_COMMIT_LEVEL_MASTER:
			commit_level |= CL_MSG_INFO3_COMMIT_LEVEL_B0;
			commit_level &= ~CL_MSG_INFO3_COMMIT_LEVEL_B1;
			break;
		default: {
			// ERROR CASE
			break;
		}
	}

	switch ( policy->key ) {
		case AS_POLICY_KEY_DIGEST: {
			as_digest * digest = as_key_digest((as_key *) key);
			rc = do_the_full_monte( 
				as->cluster, 0, CL_MSG_INFO2_WRITE, commit_level,
				key->ns, key->set, NULL, (cf_digest*)digest->value, &bins, CL_OP_WRITE, 0, &n_bins,
				NULL, &wp, &trid, NULL, &call, NULL, -1
			);
			break;
		}
		case AS_POLICY_KEY_SEND: {
			cl_object okey;
			asval_to_clobject((as_val *) key->valuep, &okey);
			as_digest * digest = as_key_digest((as_key *) key);
			rc = do_the_full_monte( 
				as->cluster, 0, CL_MSG_INFO2_WRITE, commit_level,
				key->ns, key->set, &okey, (cf_digest*)digest->value, &bins, CL_OP_WRITE, 0, &n_bins,
				NULL, &wp, &trid, NULL, &call, NULL, -1
			);
			break;
		}
		default: {
			// ERROR CASE
			break;
		}
	}

	as_buffer_destroy(&args);

	if (! (rc == AEROSPIKE_OK || rc == AEROSPIKE_ERR_UDF)) {
		as_error_fromrc(err, rc);
	}
	else {

		// Begin processing the data returned from the server,
		// IFF `result` argument is not NULL.
		// The reason is if `result` is NULL, then it implies the end user
		// does not care about the data returned from the server.

		if ( n_bins == 1  ) {
			cl_bin * bin = &bins[0];

			if ( strcmp(bin->bin_name,"SUCCESS") == 0 ) {
				if ( result ) {
					as_val * val = NULL;
					clbin_to_asval(bin, &ser, &val);
					*result = val;
				}
			}
			else if ( strcmp(bin->bin_name,"FAILURE") == 0 ) {
				as_val * val = NULL;
				clbin_to_asval(bin, &ser, &val);
				if ( val->type == AS_STRING ) {
					as_string * s = as_string_fromval(val);
					as_error_update(err, AEROSPIKE_ERR_UDF, as_string_tostring(s));
				}
				else {
					as_error_update(err, AEROSPIKE_ERR_SERVER, "unexpected failure bin type");
				}
				as_val_destroy(val);
			}
			else {
				as_error_update(err, AEROSPIKE_ERR_SERVER, "unexpected bin name");
			}
		}
		else {
			as_error_update(err, AEROSPIKE_ERR_SERVER, "unexpected number of bins");
		}
	}

	if ( bins ) {
		citrusleaf_bins_free(bins, n_bins);
		free(bins);
	}

	as_serializer_destroy(&ser);
	
	return err->code;
}
