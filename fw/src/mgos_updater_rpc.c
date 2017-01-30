/*
 * Copyright (c) 2016 Cesanta Software Limited
 * All rights reserved
 */

#include "fw/src/mgos_updater_rpc.h"

#include "common/mg_rpc/mg_rpc.h"
#include "common/cs_dbg.h"
#include "common/mg_str.h"
#include "fw/src/mgos_rpc.h"
#include "fw/src/mgos_console.h"
#include "fw/src/mgos_mongoose.h"
#include "fw/src/mgos_updater_common.h"
#include "fw/src/mgos_updater_http.h"
#include "fw/src/mgos_utils.h"

#if MGOS_ENABLE_UPDATER_RPC && MGOS_ENABLE_RPC

static struct mg_rpc_request_info *s_update_req;

static void mg_rpc_updater_result(struct update_context *ctx) {
  if (s_update_req == NULL) return;
  mg_rpc_send_errorf(s_update_req, (ctx->result > 0 ? 0 : -1), ctx->status_msg);
  s_update_req = NULL;
}

static void handle_update_req(struct mg_rpc_request_info *ri, void *cb_arg,
                              struct mg_rpc_frame_info *fi,
                              struct mg_str args) {
  char *blob_url = NULL;
  struct json_token section_tok = JSON_INVALID_TOKEN;
  struct json_token blob_url_tok = JSON_INVALID_TOKEN;
  struct json_token blob_type_tok = JSON_INVALID_TOKEN;
  int commit_timeout = 0;
  struct update_context *ctx = NULL;

  LOG(LL_DEBUG, ("Update request received: %.*s", (int) args.len, args.p));

  const char *reply = "Malformed request";

  if (args.len == 0) {
    goto clean;
  }

  json_scanf(args.p, args.len,
             "{section: %T, blob_url: %T, blob_type: %T, commit_timeout: %d}",
             &section_tok, &blob_url_tok, &blob_type_tok, &commit_timeout);

  /*
   * TODO(alashkin): enable update for another files, not
   * firmware only
   */
  if (section_tok.len == 0 || section_tok.type != JSON_TYPE_STRING ||
      strncmp(section_tok.ptr, "firmware", section_tok.len) != 0 ||
      blob_url_tok.len == 0 || blob_url_tok.type != JSON_TYPE_STRING) {
    goto clean;
  }

  LOG(LL_DEBUG,
      ("Blob url: %.*s blob type: %.*s commit_timeout: %d", blob_url_tok.len,
       blob_url_tok.ptr, blob_type_tok.len, blob_type_tok.ptr, commit_timeout));

  /*
   * If user setup callback for updater, just call it.
   * User can start update with Sys.updater.start()
   */

  blob_url = calloc(1, blob_url_tok.len + 1);
  if (blob_url == NULL) {
    CONSOLE_LOG(LL_ERROR, ("Out of memory"));
    return;
  }

  memcpy(blob_url, blob_url_tok.ptr, blob_url_tok.len);

  ctx = updater_context_create();
  if (ctx == NULL) {
    reply = "Failed to init updater";
    goto clean;
  }
  ctx->fctx.commit_timeout = commit_timeout;
  ctx->result_cb = mg_rpc_updater_result;
  s_update_req = ri;

  mgos_updater_http_start(ctx, blob_url);
  free(blob_url);
  return;

clean:
  if (blob_url != NULL) free(blob_url);
  CONSOLE_LOG(LL_ERROR, ("Failed to start update: %s", reply));
  mg_rpc_send_errorf(ri, -1, reply);
  ri = NULL;
  (void) cb_arg;
  (void) fi;
}

static void handle_commit_req(struct mg_rpc_request_info *ri, void *cb_arg,
                              struct mg_rpc_frame_info *fi,
                              struct mg_str args) {
  mg_rpc_send_errorf(ri, mgos_upd_commit() ? 0 : -1, NULL);
  ri = NULL;
  (void) cb_arg;
  (void) fi;
  (void) args;
}

static void handle_revert_req(struct mg_rpc_request_info *ri, void *cb_arg,
                              struct mg_rpc_frame_info *fi,
                              struct mg_str args) {
  bool ok = mgos_upd_revert(false /* reboot */);
  mg_rpc_send_errorf(ri, ok ? 0 : -1, NULL);
  ri = NULL;
  if (ok) mgos_system_restart_after(100);
  (void) cb_arg;
  (void) fi;
  (void) args;
}

static void handle_create_snapshot_req(struct mg_rpc_request_info *ri,
                                       void *cb_arg,
                                       struct mg_rpc_frame_info *fi,
                                       struct mg_str args) {
  int ret = mgos_upd_create_snapshot();
  if (ret < 0) {
    mg_rpc_send_errorf(ri, ret, NULL);
  } else {
    mg_rpc_send_responsef(ri, "{slot: %d}", ret);
  }
  (void) cb_arg;
  (void) fi;
  (void) args;
}

static void handle_get_boot_state_req(struct mg_rpc_request_info *ri,
                                      void *cb_arg,
                                      struct mg_rpc_frame_info *fi,
                                      struct mg_str args) {
  struct mgos_upd_boot_state bs;
  if (!mgos_upd_boot_get_state(&bs)) {
    mg_rpc_send_errorf(ri, -1, NULL);
  } else {
    LOG(LL_INFO, ("is_committed", bs.is_committed));
    mg_rpc_send_responsef(ri,
                          "{active_slot: %d, is_committed: %B, revert_slot: "
                          "%d, commit_timeout: %d}",
                          bs.active_slot, bs.is_committed, bs.revert_slot,
                          mgos_upd_get_commit_timeout());
  }
  (void) cb_arg;
  (void) fi;
  (void) args;
}

static void handle_set_boot_state_req(struct mg_rpc_request_info *ri,
                                      void *cb_arg,
                                      struct mg_rpc_frame_info *fi,
                                      struct mg_str args) {
  int ret = 0;
  struct mgos_upd_boot_state bs;
  if (mgos_upd_boot_get_state(&bs)) {
    int commit_timeout = -1;
    if (json_scanf(args.p, args.len,
                   "{active_slot: %d, is_committed: %B, revert_slot: %d, "
                   "commit_timeout: %d}",
                   &bs.active_slot, &bs.is_committed, &bs.revert_slot,
                   &commit_timeout) > 0) {
      ret = mgos_upd_boot_set_state(&bs) ? 0 : -3;
      if (ret == 0 && commit_timeout >= 0) {
        ret = mgos_upd_set_commit_timeout(commit_timeout) ? 0 : -4;
      }
    } else {
      ret = -2;
    }
  } else {
    ret = -1;
  }
  mg_rpc_send_errorf(ri, ret, NULL);
  (void) cb_arg;
  (void) fi;
}

void mgos_updater_rpc_init(void) {
  struct mg_rpc *mg_rpc = mgos_rpc_get_global();
  if (mg_rpc == NULL) return;
  mg_rpc_add_handler(mg_rpc, mg_mk_str("OTA.Update"), handle_update_req, NULL);
  mg_rpc_add_handler(mg_rpc, mg_mk_str("OTA.Commit"), handle_commit_req, NULL);
  mg_rpc_add_handler(mg_rpc, mg_mk_str("OTA.Revert"), handle_revert_req, NULL);
  mg_rpc_add_handler(mg_rpc, mg_mk_str("OTA.CreateSnapshot"),
                     handle_create_snapshot_req, NULL);
  mg_rpc_add_handler(mg_rpc, mg_mk_str("OTA.GetBootState"),
                     handle_get_boot_state_req, NULL);
  mg_rpc_add_handler(mg_rpc, mg_mk_str("OTA.SetBootState"),
                     handle_set_boot_state_req, NULL);
}

#endif /* MGOS_ENABLE_UPDATER_RPC && MGOS_ENABLE_RPC */