in3_ret_t in3_register_eth_nano(in3_t *);
in3_ret_t in3_register_eth_basic(in3_t *);
in3_ret_t in3_register_eth_full(in3_t *);
in3_ret_t in3_register_ipfs(in3_t *);
in3_ret_t in3_register_btc(in3_t *);
in3_ret_t eth_register_pk_signer(in3_t *);
in3_ret_t in3_register_zksync(in3_t *);
in3_ret_t in3_register_eth_api(in3_t *);
in3_ret_t in3_register_core_api(in3_t *);
in3_ret_t in3_register_nodeselect_def(in3_t *);

static void auto_init() {
  in3_register_default(in3_register_core_api);
  in3_register_default(in3_register_eth_nano);
  in3_register_default(in3_register_eth_basic);
  in3_register_default(in3_register_eth_full);
  in3_register_default(in3_register_ipfs);
  in3_register_default(in3_register_btc);
  in3_register_default(eth_register_pk_signer);
  in3_register_default(in3_register_zksync);
  in3_register_default(in3_register_eth_api);
  in3_register_default(in3_register_nodeselect_def);
}
