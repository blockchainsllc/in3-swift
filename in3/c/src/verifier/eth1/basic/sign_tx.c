/*******************************************************************************
 * This file is part of the Incubed project.
 * Sources: https://github.com/blockchainsllc/in3
 * 
 * Copyright (C) 2018-2020 slock.it GmbH, Blockchains LLC
 * 
 * 
 * COMMERCIAL LICENSE USAGE
 * 
 * Licensees holding a valid commercial license may use this file in accordance 
 * with the commercial license agreement provided with the Software or, alternatively, 
 * in accordance with the terms contained in a written agreement between you and 
 * slock.it GmbH/Blockchains LLC. For licensing terms and conditions or further 
 * information please contact slock.it at in3@slock.it.
 * 	
 * Alternatively, this file may be used under the AGPL license as follows:
 *    
 * AGPL LICENSE USAGE
 * 
 * This program is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free Software 
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A 
 * PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.
 * [Permissions of this strong copyleft license are conditioned on making available 
 * complete source code of licensed works and modifications, which include larger 
 * works using a licensed work, under the same license. Copyright and license notices 
 * must be preserved. Contributors provide an express grant of patent rights.]
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *******************************************************************************/

#include "../../../core/client/keys.h"
#include "../../../core/client/request_internal.h"
#include "../../../core/util/data.h"
#include "../../../core/util/mem.h"
#include "../../../core/util/utils.h"
#include "../../../third-party/crypto/ecdsa.h"
#include "../../../third-party/crypto/secp256k1.h"
#include "../../../verifier/eth1/basic/filter.h"
#include "../../../verifier/eth1/nano/eth_nano.h"
#include "../../../verifier/eth1/nano/merkle.h"
#include "../../../verifier/eth1/nano/rlp.h"
#include "../../../verifier/eth1/nano/serialize.h"
#include "eth_basic.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/** helper to get a key and convert it to bytes*/
static inline bytes_t get(d_token_t* t, uint16_t key) {
  return d_to_bytes(d_get(t, key));
}
/** helper to get a key and convert it to bytes with a specified length*/
static inline bytes_t getl(d_token_t* t, uint16_t key, size_t l) {
  return d_to_bytes(d_getl(t, key, l));
}

/**  return data from the client.*/
static in3_ret_t get_from_nodes(in3_req_t* parent, char* method, char* params, bytes_t* dst) {
  // check if the method is already existing
  in3_req_t* ctx = req_find_required(parent, method, NULL);
  if (ctx) {
    // found one - so we check if it is useable.
    switch (in3_req_state(ctx)) {
      // in case of an error, we report it back to the parent context
      case REQ_ERROR:
        return req_set_error(parent, ctx->error, IN3_EUNKNOWN);
      // if we are still waiting, we stop here and report it.
      case REQ_WAITING_FOR_RESPONSE:
      case REQ_WAITING_TO_SEND:
        return IN3_WAITING;

      // if it is useable, we can now handle the result.
      case REQ_SUCCESS: {
        d_token_t* r = d_get(ctx->responses[0], K_RESULT);
        if (r) {
          // we have a result, so write it back to the dst
          *dst = d_to_bytes(r);
          return IN3_OK;
        }
        else
          // or check the error and report it
          return req_check_response_error(ctx, 0);
      }
    }
  }

  // no required context found yet, so we create one:

  // since this is a subrequest it will be freed when the parent is freed.
  // allocate memory for the request-string
  char* req = _malloc(strlen(method) + strlen(params) + 200);
  // create it
  sprintf(req, "{\"method\":\"%s\",\"jsonrpc\":\"2.0\",\"params\":%s}", method, params);
  // and add the request context to the parent.
  return req_add_required(parent, req_new(parent->client, req));
}

/** gets the from-fied from the tx or ask the signer */
static in3_ret_t get_from_address(d_token_t* tx, in3_req_t* ctx, address_t res) {
  d_token_t* t = d_get(tx, K_FROM);
  if (t) {
    // we only accept valid from addresses which need to be 20 bytes
    if (d_type(t) != T_BYTES || d_len(t) != 20) return req_set_error(ctx, "invalid from address in tx", IN3_EINVAL);
    memcpy(res, d_bytes(t)->data, 20);
    return IN3_OK;
  }

  // if it is not specified, we rely on the from-address of the signer.
  if (!in3_plugin_is_registered(ctx->client, PLGN_ACT_SIGN_ACCOUNT)) return req_set_error(ctx, "missing from address in tx", IN3_EINVAL);

  in3_sign_account_ctx_t actx = {.req = ctx, .accounts = NULL, .accounts_len = 0};
  TRY(in3_plugin_execute_first(ctx, PLGN_ACT_SIGN_ACCOUNT, &actx))
  if (!actx.accounts) return req_set_error(ctx, "no from address found", IN3_EINVAL);
  memcpy(res, actx.accounts, 20);
  _free(actx.accounts);
  return IN3_OK;
}

/** checks if the nonce and gas is set  or fetches it from the nodes */
static in3_ret_t get_nonce_and_gasprice(bytes_t* nonce, bytes_t* gas_price, in3_req_t* ctx, address_t from) {
  in3_ret_t ret = IN3_OK;
  if (!nonce->data) {
    bytes_t from_bytes = bytes(from, 20);
    sb_t*   sb         = sb_new("[");
    sb_add_bytes(sb, "", &from_bytes, 1, false);
    sb_add_chars(sb, ",\"latest\"]");
    ret = get_from_nodes(ctx, "eth_getTransactionCount", sb->data, nonce);
    sb_free(sb);
  }
  if (!gas_price->data) {
    in3_ret_t res = get_from_nodes(ctx, "eth_gasPrice", "[]", gas_price);
    if (res == IN3_WAITING)
      ret = (ret == IN3_WAITING || ret == IN3_OK) ? IN3_WAITING : ret;
    else if (res != IN3_OK)
      ret = res;
  }

  return ret;
}

/** gets the v-value from the chain_id */
static inline uint64_t get_v(chain_id_t chain) {
  uint64_t v = chain;
  if (v > 0xFF && v != 1337) v = 0; // this is only valid for ethereum chains.
  return v;
}

/**
 * prepares a transaction and writes the data to the dst-bytes. In case of success, you MUST free only the data-pointer of the dst. 
 */
in3_ret_t eth_prepare_unsigned_tx(d_token_t* tx, in3_req_t* ctx, bytes_t* dst) {
  address_t from;

  // read the values
  bytes_t gas_limit = d_get(tx, K_GAS) ? get(tx, K_GAS) : (d_get(tx, K_GAS_LIMIT) ? get(tx, K_GAS_LIMIT) : bytes((uint8_t*) "\x52\x08", 2)),
          to        = getl(tx, K_TO, 20),
          value     = get(tx, K_VALUE),
          data      = get(tx, K_DATA),
          nonce     = get(tx, K_NONCE),
          gas_price = get(tx, K_GAS_PRICE);

  // make sure, we have the correct chain_id
  chain_id_t chain_id = ctx->client->chain.chain_id;
  if (chain_id == CHAIN_ID_LOCAL) {
    d_token_t* r = NULL;
    TRY(req_send_sub_request(ctx, "eth_chainId", "", NULL, &r, NULL))
    chain_id = d_long(r);
  }
  TRY(get_from_address(tx, ctx, from))
  TRY(get_nonce_and_gasprice(&nonce, &gas_price, ctx, from))

  // create raw without signature
  bytes_t* raw = serialize_tx_raw(nonce, gas_price, gas_limit, to, value, data, get_v(chain_id), bytes(NULL, 0), bytes(NULL, 0));
  *dst         = *raw;
  _free(raw);

  // do we need to change it?
  if (in3_plugin_is_registered(ctx->client, PLGN_ACT_SIGN_PREPARE)) {
    in3_sign_prepare_ctx_t pctx = {.req = ctx, .old_tx = *dst, .new_tx = {0}};
    memcpy(pctx.account, from, 20);
    in3_ret_t prep_res = in3_plugin_execute_first(ctx, PLGN_ACT_SIGN_PREPARE, &pctx);

    if (prep_res) {
      if (dst->data) _free(dst->data);
      if (pctx.new_tx.data) _free(pctx.new_tx.data);
      return prep_res;
    }
    else if (pctx.new_tx.data) {
      if (dst->data) _free(dst->data);
      *dst = pctx.new_tx;
    }
  }

  // cleanup subcontexts
  TRY(req_remove_required(ctx, req_find_required(ctx, "eth_getTransactionCount", NULL), false))
  TRY(req_remove_required(ctx, req_find_required(ctx, "eth_gasPrice", NULL), false))

  return IN3_OK;
}

/**
 * signs a unsigned raw transaction and writes the raw data to the dst-bytes. In case of success, you MUST free only the data-pointer of the dst. 
 */
in3_ret_t eth_sign_raw_tx(bytes_t raw_tx, in3_req_t* ctx, address_t from, bytes_t* dst) {
  bytes_t signature;

  // make sure, we have the correct chain_id
  chain_id_t chain_id = ctx->client->chain.chain_id;
  if (chain_id == CHAIN_ID_LOCAL) {
    d_token_t* r = NULL;
    TRY(req_send_sub_request(ctx, "eth_chainId", "", NULL, &r, NULL))
    chain_id = d_long(r);
  }

  TRY(req_require_signature(ctx, SIGN_EC_HASH, &signature, raw_tx, bytes(from, 20)));
  if (signature.len != 65) return req_set_error(ctx, "Transaction must be signed by a ECDSA-Signature!", IN3_EINVAL);

  // get the signature from required

  // if we reached that point we have a valid signature in sig
  // create raw transaction with signature
  bytes_t  data, last;
  uint32_t v = 27 + signature.data[64] + (get_v(chain_id) ? (get_v(chain_id) * 2 + 8) : 0);
  EXPECT_EQ(rlp_decode(&raw_tx, 0, &data), 2)                           // the raw data must be a list(2)
  EXPECT_EQ(rlp_decode(&data, 5, &last), 1)                             // the last element (data) must be an item (1)
  bytes_builder_t* rlp = bb_newl(raw_tx.len + 68);                      // we try to make sure, we don't have to reallocate
  bb_write_raw_bytes(rlp, data.data, last.data + last.len - data.data); // copy the existing data without signature

  // add v
  uint8_t vdata[sizeof(v)];
  data = bytes(vdata, sizeof(vdata));
  int_to_bytes(v, vdata);
  b_optimize_len(&data);
  rlp_encode_item(rlp, &data);

  // add r
  data = bytes(signature.data, 32);
  b_optimize_len(&data);
  rlp_encode_item(rlp, &data);

  // add s
  data = bytes(signature.data + 32, 32);
  b_optimize_len(&data);
  rlp_encode_item(rlp, &data);

  // finish up
  rlp_encode_to_list(rlp);
  *dst = rlp->b;

  _free(rlp);
  return IN3_OK;
}

/** handle the sendTransaction internally */
in3_ret_t handle_eth_sendTransaction(in3_req_t* ctx, d_token_t* req) {
  // get the transaction-object
  d_token_t* tx_params   = d_get(req, K_PARAMS);
  bytes_t    unsigned_tx = bytes(NULL, 0), signed_tx = bytes(NULL, 0);
  address_t  from;
  if (!tx_params || d_type(tx_params + 1) != T_OBJECT) return req_set_error(ctx, "invalid params", IN3_EINVAL);

  TRY(get_from_address(tx_params + 1, ctx, from));

  // is there a pending signature?
  // we get the raw transaction from this request
  in3_req_t* sig_ctx = req_find_required(ctx, "sign_ec_hash", NULL);
  if (sig_ctx) {
    bytes_t raw = *d_get_bytes_at(d_get(sig_ctx->requests[0], K_PARAMS), 0);
    unsigned_tx = bytes(_malloc(raw.len), raw.len);
    memcpy(unsigned_tx.data, raw.data, raw.len);
  }
  else
    TRY(eth_prepare_unsigned_tx(tx_params + 1, ctx, &unsigned_tx));
  TRY_FINAL(eth_sign_raw_tx(unsigned_tx, ctx, from, &signed_tx),
            if (unsigned_tx.data) _free(unsigned_tx.data);)

  // build the RPC-request
  sb_t sb = {0};
  sb_add_rawbytes(&sb, "{ \"jsonrpc\":\"2.0\", \"method\":\"eth_sendRawTransaction\", \"params\":[\"0x", signed_tx, 0);
  sb_add_chars(&sb, "\"]");
  sb_add_chars(&sb, "}");

  // now that we included the signature in the rpc-request, we can free it + the old rpc-request.
  _free(signed_tx.data);
  json_free(ctx->request_context);

  // set the new RPC-Request.
  ctx->request_context                           = parse_json(sb.data);
  ctx->requests[0]                               = ctx->request_context->result;
  in3_cache_add_ptr(&ctx->cache, sb.data)->props = CACHE_PROP_MUST_FREE | CACHE_PROP_ONLY_EXTERNAL; // we add the request-string to the cache, to make sure the request-string will be cleaned afterwards
  return IN3_OK;
}

/** minimum signer for the wallet, returns the signed message which needs to be freed **/
char* eth_wallet_sign(const char* key, const char* data) {
  int     data_l = strlen(data) / 2 - 1;
  uint8_t key_bytes[32], *data_bytes = alloca(data_l + 1), dst[65];

  hex_to_bytes((char*) key + 2, -1, key_bytes, 32);
  data_l    = hex_to_bytes((char*) data + 2, -1, data_bytes, data_l + 1);
  char* res = _malloc(133);

  if (ecdsa_sign(&secp256k1, HASHER_SHA3K, key_bytes, data_bytes, data_l, dst, dst + 64, NULL) >= 0) {
    bytes_to_hex(dst, 65, res + 2);
    res[0] = '0';
    res[1] = 'x';
  }

  return res;
}
