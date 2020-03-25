/*
 * Copyright (c) 2014 Cesanta Software Limited
 * All rights reserved
 */

#include "stdio.h"
#include "cJSON/cJSON.h"
#include "mongoose/mongoose.h"

/* reserved buffer to save memory */
static char g_cmd_buf[1024];
static char g_cmd_output[1024];

/**
 * CGW API Handler
**/
struct bs_cgw_api_handler {
  char * api;
  mg_event_handler_t fn;
  struct mg_connection * nc;
};

//-----------------------------------------------------------------
// HMI Message  
//-----------------------------------------------------------------
#define CHECK_NEW_PACKAGE   1
#define START_UPGRADE       2
#define UPGRADE_PROGRESS    3

#define REQUEST         "1"
#define RESPONSE        "2"

/**
 * Upgrading pkg info of single ECU 
**/
struct bs_pkg_info {
  // “yes” or “no”
  char door_module[8];
  char dev_id[32];
  char soft_id[32];
  char release_notes[256];
};

/**
 * Upgrading status of single ECU
**/
struct bs_ecu_upgrade_stat {
  char dev_id[32];
  char soft_id[32];
  char esti_time[64];
  char start_time[32];
  char time_stamp[32];
  // "yes" or "no"
  char door_module[8];
  // "pending", "in progress", "failed", "success"
  char status[15];
  // raw percentage data, e.g., 0, 55, or 100
  float progress_percent; 
};

/**
 * core state machine
**/
#define STAT_INVALID  0xFF
#define STAT_IDLE  0x00
#define DLC_PKG_NEW 0x01
#define DLC_PKG_DOWNLOADING 0x02
#define DLC_PKG_READY 0x03
#define DLC_PKG_BAD 0x04

#define ORCH_CON_ERR 0x10
#define ORCH_PKG_DOWNLOADING 0x20
#define ORCH_PKG_READY 0x30
#define ORCH_PKG_BAD 0x40
#define ORCH_TDR_RUN 0x50
#define ORCH_TDR_FAIL 0x60
#define ORCH_TDR_SUCC 0x70
static unsigned char g_stat = 0;
static unsigned char g_stat_lock = 0;
struct bs_context {
  struct mg_connection * dmc;
  struct mg_connection * hmi;
  unsigned char cgw_thread_exit;
  unsigned char downloader_thread_exit;
  unsigned char hmi_thread_exit;
  struct bs_cgw_api_handler cgw_api_pkg_new;
  struct bs_cgw_api_handler cgw_api_pkg_stat;
  struct bs_cgw_api_handler cgw_api_tdr_run;
  struct bs_cgw_api_handler cgw_api_tdr_stat;
  struct bs_cgw_api_handler cgw_api_pkg_upload;
  struct bs_pkg_info * hmi_pkg_info;
  struct bs_ecu_upgrade_stat * hmi_upgrade_stat;
  char * tftp_server;
  char * downloader;
  char * pkg_cdn_url;
  char * cmd_buf;
  char * cmd_output;
  
  void * data;
};
struct bs_context g_ctx;
static void core_state_handler(unsigned char);

//---------------------------------------------------------------------------
// Communication with HMI
// 
//--------------------------------------------------------------------------- 
static void init_hmi_objs(struct bs_context * p_ctx) {

  // TODO: take values from p_ctx to init these values

  // To make private
  static struct bs_pkg_info _hmi_pkg_info;
  static struct bs_ecu_upgrade_stat _hmi_upgrade_stat;

  p_ctx->hmi_pkg_info = &_hmi_pkg_info;
  p_ctx->hmi_upgrade_stat = &_hmi_upgrade_stat;

  strcpy(_hmi_pkg_info.door_module, "no");
  strcpy(_hmi_pkg_info.dev_id, "FP_001_FID1");
  strcpy(_hmi_pkg_info.soft_id, "wpc.1.0.0.0");
  strcpy(_hmi_pkg_info.release_notes, "N/A");

  strcpy(_hmi_upgrade_stat.dev_id, "FP_001_FID1");
  strcpy(_hmi_upgrade_stat.soft_id, "wpc.1.0.0.0");
  strcpy(_hmi_upgrade_stat.esti_time, "00:00:00 01-01-1900");
  strcpy(_hmi_upgrade_stat.start_time, "00:00:00 01-01-1900");
  strcpy(_hmi_upgrade_stat.time_stamp, "00:00:00 01-01-1900");
  // "yes" or "no"
  strcpy(_hmi_upgrade_stat.door_module, "no");
  // "pending", "in progress", "failed", "success"
  strcpy(_hmi_upgrade_stat.status, "in progress");
  // raw percentage data, e.g., 0, 55, or 100
  _hmi_upgrade_stat.progress_percent = 0; 
}

static unsigned int hmi_resp_check_new_pkg(struct bs_context *p_ctx, char *msg) {
  unsigned int pc = 0;
  char buf[512];
  static char *resp_header = "\"func-id\":1,\"category\":2,\"response\":{";
  // first 4 bytes for length
  pc += 4;

  // start {
  msg[pc] = '{';
  pc += 1;

  // header
  strcpy(msg + pc, resp_header);
  pc += strlen(resp_header);

  // door_module
  sprintf(buf, "\"door_module\":\"%s\",", p_ctx->hmi_pkg_info->door_module);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // dev_id
  sprintf(buf, "\"dev_id\":\"%s\",", p_ctx->hmi_pkg_info->dev_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // soft_id
  sprintf(buf, "\"soft_id\":\"%s\",", p_ctx->hmi_pkg_info->soft_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // release_notes
  sprintf(buf, "\"release_notes\":\"%s\",", p_ctx->hmi_pkg_info->release_notes);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}' for pkg info
  msg[pc] = '}';
  pc += 1;

  // end '}'
  msg[pc] = '}';
  pc += 1;


  // add string end to keep safe
  msg[++pc] = 0;

  // the value of pc is the length
  msg[0] = (char) (pc<< 24);
  msg[1] = (char) (pc<< 16);
  msg[2] = (char) (pc<< 8); 
  msg[3] = (char) (pc);

  // debug
  printf("res:-----> %s\n", msg+4);

  return pc;
}

static unsigned int hmi_resp_start_upgrade(struct bs_context *p_ctx, char *msg) {
  unsigned int pc = 0;
  char buf[128];
  static char *resp_header = "\"func-id\":2,\"category\":2,\"response\":{";
  // first 4 bytes for length
  pc += 4;

  // start {
  msg[pc] = '{';
  pc += 1;

  // header
  strcpy(msg + pc, resp_header);
  pc += strlen(resp_header);

  // dev_id
  sprintf(buf, "\"dev_id\":\"%s\",", p_ctx->hmi_pkg_info->dev_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // soft_id
  sprintf(buf, "\"soft_id\":\"%s\",", p_ctx->hmi_pkg_info->soft_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}' for pkg info
  msg[pc] = '}';
  pc += 1;

  // end '}'
  msg[pc] = '}';
  pc += 1;

  // add string end to keep safe
  msg[++pc] = 0;

  // the value of pc is the length
  msg[0] = (char) (pc<< 24);
  msg[1] = (char) (pc<< 16);
  msg[2] = (char) (pc<< 8);
  msg[3] = (char) (pc);

  // debug
  printf("res:-----> %s\n", msg+4);

  return pc;
}

static unsigned int hmi_resp_upgrade_stat(struct bs_context *p_ctx, char *msg) {
  unsigned int pc = 0; 
  static char *resp_header = "\"func-id\":3,\"category\":2,\"response\":{";
  char buf[128];
 
  // first 4 bytes for length
  pc += 4;

  // start {
  msg[pc] = '{';
  pc += 1;

  // header
  strcpy(msg + pc, resp_header);
  pc += strlen(resp_header);

  // dev_id
  sprintf(buf, "\"dev_id\":\"%s\",", p_ctx->hmi_upgrade_stat->dev_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // soft_id
  sprintf(buf, "\"soft_id\":\"%s\",", p_ctx->hmi_upgrade_stat->soft_id);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // esti_time
  sprintf(buf, "\"esti_time\":\"%s\",", p_ctx->hmi_upgrade_stat->esti_time);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // start_time
  sprintf(buf, "\"start_time\":\"%s\",", p_ctx->hmi_upgrade_stat->start_time);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // time_stamp
  sprintf(buf, "\"time_stamp\":\"%s\",", p_ctx->hmi_upgrade_stat->time_stamp);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // door_module
  sprintf(buf, "\"door_module\":\"%s\",", p_ctx->hmi_upgrade_stat->door_module);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // status
  sprintf(buf, "\"status\":\"%s\",", p_ctx->hmi_upgrade_stat->status);
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // progress
  sprintf(buf, "\"progress_percent\":%d", (int)(p_ctx->hmi_upgrade_stat->progress_percent));
  strcpy(msg + pc, buf);
  pc += strlen(buf);

  // end '}' for pkg info
  msg[pc] = '}';
  pc += 1;

  // end '}'
  msg[pc] = '}';
  pc += 1;

  // add string end to keep safe
  msg[++pc] = 0;

  // the value of pc is the length
  msg[0] = (char) (pc<< 24);
  msg[1] = (char) (pc<< 16);
  msg[2] = (char) (pc<< 8); 
  msg[3] = (char) (pc);

  // debug
  printf("res:-----> %s\n", msg+4);

  return pc;  
}

// parse JSON format payload
static int hmi_payload_parser(struct bs_context *p_ctx, char* payload, unsigned int len) { 
  int result = 1;
  int resp_len = 0;
  static char response[1024];
  struct cJSON * root = NULL;
  struct cJSON * func_id = NULL;
  int func_id_code = 0;

  (void) len;

  root = cJSON_Parse(payload);

  if (root == NULL)
      return 0;

  // TODO: validate whole JSON 
  func_id = root->child;
  if (!cJSON_IsNumber(func_id)) {
    goto last_step;
  }
 
  func_id_code = func_id->valueint;

  // TODO: synthesize response acoording to coming in value
  switch(func_id_code) {
    case START_UPGRADE:
      resp_len = hmi_resp_start_upgrade(p_ctx, response);
      mg_send(p_ctx->hmi, response, resp_len);
      break;
    case UPGRADE_PROGRESS:
      resp_len = hmi_resp_upgrade_stat(p_ctx, response);
      mg_send(p_ctx->hmi, response, resp_len);
      break;
    case CHECK_NEW_PACKAGE:
      resp_len = hmi_resp_check_new_pkg(p_ctx, response);
      mg_send(p_ctx->hmi, response, resp_len);
      break;
    default:
      // unknown item
      result = 0;
      goto last_step;
      break;
  }

last_step:
  // release memory
  cJSON_Delete(root);
  return result; 
}

static void hmi_msg_handler(struct mg_connection *nc, int ev, void *p) {
  unsigned int len = 0;
  struct mbuf *io = &nc->recv_mbuf;
  (void) p;

  switch (ev) {
    case MG_EV_ACCEPT:
      g_ctx.hmi = nc;
      printf("HMI: socket connected\n");
      break;
    case MG_EV_RECV:
      // first 4 bytes for length
      len = io->buf[3] + (io->buf[2] << 8) + (io->buf[1] << 16) + (io->buf[0] << 24);
      hmi_payload_parser(&g_ctx, io->buf+4, len);
      break;
    default:
      break;
  }
}

static void * hmi_thread(void * param) {
  struct mg_mgr mgr;
  struct bs_context * p_ctx = (struct bs_context *) param;

  mg_mgr_init(&mgr, NULL);

  printf("==Start socket server for HMI ==\n");
    // by default, listen to 3001
    mg_bind(&mgr, "3001", hmi_msg_handler);
    printf("Listen on port 3001 for HMI\n");

  while (!p_ctx->hmi_thread_exit) {
//    if (g_stat_lock) {
//      sleep(0.1);
//      continue;
//    }

//    g_stat_lock = 1;
    if (g_ctx.hmi != NULL) {
      if (g_stat >= ORCH_PKG_READY) {
      // TODO: report status if downloading stated
      ;
      }
    }

    mg_mgr_poll(&mgr, 1000);
//    g_stat_lock = 0;
  }
  mg_mgr_free(&mgr);

  return 0;
}

static int hmi_thread_run(struct bs_context * p_ctx) {
  mg_start_thread(hmi_thread, (void *) p_ctx);
  return 1;
}

//---------------------------------------------------------------------------
// Communication with DMC
//--------------------------------------------------------------------------- 
static int dmc_tftp_run(struct bs_context * p_ctx) {
  // TODO: 1. format message according convention with SocketWrapper
  // TODO: 2. need root right to listen port 69?
  char * succ = "{ \"DLC pkg new\": \"Downloader start to run\"}";
  char * fail = "{ \"DLC pkg new\": \"Donwloader failed to run\"}";

  FILE *fp;

  if ((fp = popen(p_ctx->cmd_buf, "r")) != NULL) {
    mg_send(p_ctx->dmc, succ, strlen(succ));
  } else {
    mg_send(p_ctx->dmc, fail, strlen(fail));
  }

  //TODO:  handle the cases of tftp failed and monitoring tftp server

  return 1;
}

static int dmc_downloader_stat(struct bs_context * p_ctx) {
  (void) p_ctx;
  return g_stat;
}

static void * dmc_downloader_thread(void * param) {
  // TODO: format message according convention with SocketWrapper
  char * succ = "{ \"DLC pkg new\": \"Downloader start to run\"}";
  char * fail = "{ \"DLC pkg new\": \"Donwloader failed to run\"}";

  FILE *fp;
  struct bs_context * p_ctx = (struct bs_context *) param;

  // TODO: memeset g_cmd_buf to zero and check the length of url and dlc_path
  strcpy(p_ctx->cmd_buf, p_ctx->downloader);
  strcat(p_ctx->cmd_buf, " ");
  strcat(p_ctx->cmd_buf, p_ctx->pkg_cdn_url);    

  if ((fp = popen(p_ctx->cmd_buf, "r")) != NULL) {
    mg_send(p_ctx->dmc, succ, strlen(succ));
  } else {
    mg_send(p_ctx->dmc, fail, strlen(fail));
    return NULL;
  }

  printf("--> curl start downling!\n");
  while (!p_ctx->downloader_thread_exit &&
         fgets(p_ctx->cmd_output, 1024, fp) != NULL) {
    // block until has lock
    while (g_stat_lock);

    g_stat_lock = 1;
    // TODO: need better way to check successful downloading  
    g_stat_lock = 0;
  }
  printf("--> curl finished downloading!\n");

  while (g_stat_lock);
  g_stat_lock = 1;
  if (pclose(fp) == 0) {
    core_state_handler(DLC_PKG_READY);
  }
  g_stat_lock = 0;

  return NULL;
}

static int dmc_downloader_run(struct bs_context * p_ctx) {
  mg_start_thread(dmc_downloader_thread, (void *) p_ctx);
  return 1;
}

static void dmc_msg_handler(struct mg_connection *nc, int ev, void *p) {
  struct mbuf *io = &nc->recv_mbuf;
  unsigned int len = 0;
  (void) p;

  switch (ev) {
    case MG_EV_ACCEPT:
      g_ctx.dmc = nc;
      dmc_tftp_run(&g_ctx);
      hmi_thread_run(&g_ctx);
      core_state_handler(DLC_PKG_NEW);//??test
      break;
    case MG_EV_RECV:
      // first 4 bytes for length
      len = io->buf[3] + (io->buf[2] << 8) + (io->buf[1] << 16) + (io->buf[0] << 24);   
      //TODO: parse JSON
      (void) len;
      core_state_handler(STAT_INVALID);
      mbuf_remove(io, io->len);       // Discard message from recv buffer
      break;
    default:
      break;
  }
}

//---------------------------------------------------------------------------
// Communication with Orchestrator on CGW
//--------------------------------------------------------------------------- 
static void * cgw_pkg_upload_thread(void *param) {
  struct bs_context *p_ctx = (struct bs_context *) param;
  FILE *fp;
  static char cmd[128];
  char *curl = "curl -F 'data=@./wpc.1.0.0' ";
  int i;

  (void) param;
 
  for(i = 0; i < 128; i++) {
    cmd[i] = 0;
  }
  // TODO: obtain the package name from g_ctx
  strcpy(cmd, curl);
  strcat(cmd, p_ctx->cgw_api_pkg_upload.api);

  if ((fp = popen(cmd, "r")) != NULL) {
    printf("curl start to upload");
  } else {
    printf("curl failed to upload");
  }

  printf("--> curl start to upload!\n");
  while (!p_ctx->downloader_thread_exit &&
         fgets(p_ctx->cmd_output, 1024, fp) != NULL) {
    // block until has lock
    while (g_stat_lock);

    g_stat_lock = 1;
    g_stat = ORCH_PKG_READY;//HMI will triger orchestrator to do upgrading  
    g_stat_lock = 0;
  }
  printf("--> curl uploading finished!\n");

  return NULL;
}

static void cgw_handler_pkg_new(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  (void) hm;

  switch (ev) {
    case MG_EV_CONNECT:
      if (*(int *) ev_data != 0) {
        printf("Orchestrator pkg new: connect failed\n");
        g_stat = ORCH_CON_ERR;
        g_ctx.cgw_thread_exit = 1;
      } else {
        g_ctx.cgw_api_pkg_new.nc = nc;
        g_stat = ORCH_PKG_DOWNLOADING;
        printf("Orchestrator pkg new: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      g_stat = ORCH_PKG_DOWNLOADING;
      mg_start_thread(cgw_pkg_upload_thread, (void *) &g_ctx);
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_ctx.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_pkg_new.nc = NULL;
      if (g_ctx.cgw_thread_exit == 0) {
        printf("Orchestrator pkg new: connection closed\n");
        g_ctx.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_pkg_stat(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;

  switch (ev) {
    case MG_EV_CONNECT:
      if (*(int *) ev_data != 0) {
        printf("Orchestrator pkg status: connect failed\n");
        g_stat = ORCH_PKG_BAD;
        g_ctx.cgw_thread_exit = 1;
      } else {
        g_ctx.cgw_api_pkg_stat.nc = nc;
        g_stat = ORCH_PKG_DOWNLOADING;
        printf("Orchestrator pkg status: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      fwrite(hm->body.p, 1, hm->body.len, stdout);
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      //g_stat = ORCH_PKG_DOWNLOADING;
      if (strstr(hm->body.p, "succ") != NULL) {
        g_stat = ORCH_PKG_READY;
        printf("Orchestrator pkg status: pkg ready\n");
      } else {
        g_stat = ORCH_PKG_DOWNLOADING;
        printf("Orchestrator pkg status: pkg downloading\n");
      }

      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_ctx.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_pkg_stat.nc = NULL;
      if (g_ctx.cgw_thread_exit == 0) {
        printf("Orchestrator pkg status: closed connection\n");
        g_ctx.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_tdr_run(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  (void) hm;

  switch (ev) {
    case MG_EV_CONNECT:
      if (*(int *) ev_data != 0) {
        printf("Orchestrator tdr run: connect failed\n");
        g_stat = ORCH_CON_ERR;
        g_ctx.cgw_thread_exit = 1;
      } else {
        g_ctx.cgw_api_tdr_run.nc = nc;
        g_stat = ORCH_TDR_RUN;
        printf("Orchestrator tdr run: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_NET_ERR;
      // g_stat = ORCH_TDR_FAIL;
      g_stat = ORCH_TDR_RUN;
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_ctx.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_tdr_run.nc = NULL;
      if (g_ctx.cgw_thread_exit == 0) {
        printf("Orchestrator tdr run: closed connection\n");
        g_ctx.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static void cgw_handler_tdr_stat(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;
  (void)hm;

  switch (ev) {
    case MG_EV_CONNECT:
      g_ctx.cgw_api_tdr_stat.nc = nc;
      if (*(int *) ev_data != 0) {
        printf("Orchestrator tdr status: connect failed\n");
        g_stat = ORCH_CON_ERR;
        g_ctx.cgw_thread_exit = 1;
      } else {
        g_ctx.cgw_api_tdr_stat.nc = nc;
        g_stat = ORCH_TDR_RUN;
        printf("Orchestrator tdr status: connected to send msg\n");
      }
      break;
    case MG_EV_HTTP_REPLY:
      //TODO: parse JSON, if error then g_stat = ORCH_TDR_FAIL;
      // g_stat = ORCH_TDR_SUCC;
      g_stat = ORCH_TDR_RUN;
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      g_ctx.cgw_thread_exit = 1;
      break;
    case MG_EV_CLOSE:
      g_ctx.cgw_api_tdr_stat.nc = NULL;
      if (g_ctx.cgw_thread_exit == 0) {
        printf("Orchestrator tdr status: closed connection\n");
        g_ctx.cgw_thread_exit = 1;
      }
      break;
    default:
      break;
  }
}

static void * cgw_msg_thread(void *param) {
  struct mg_mgr mgr;
  struct bs_cgw_api_handler * handler = (struct bs_cgw_api_handler *) param;

  mg_mgr_init(&mgr, NULL);
  mg_connect_http(&mgr, handler->fn, handler->api, NULL, NULL);

  while (g_ctx.cgw_thread_exit == 0) {
    if (g_stat_lock)
      continue;

    // run until one api request end
    g_stat_lock = 1;
    mg_mgr_poll(&mgr, 300);
    g_stat_lock = 0;
  }

  mg_mgr_free(&mgr);
  return NULL;
}

//---------------------------------------------------------------------------
// Core state machine
// Set g_stat_lock=1 before invoking this function
//--------------------------------------------------------------------------- 
static void core_state_handler(unsigned char reset) {

  // force to reset
  if (reset != STAT_INVALID) { 
    g_stat = reset;
  }

  switch (g_stat) {
    case STAT_IDLE:
      break;
    case DLC_PKG_NEW:
      // TODO: parse received JSON

      dmc_downloader_run(&g_ctx);
      g_stat = DLC_PKG_DOWNLOADING;
      break;
    case DLC_PKG_DOWNLOADING:
      // TODO: verify checksum of downloaded pkg
      break;
    case DLC_PKG_READY:
      printf("---> pgk ready\n");
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_pkg_new)); 
      break;
    case ORCH_CON_ERR:
      break;
    case ORCH_PKG_DOWNLOADING:
      if (g_ctx.cgw_api_pkg_stat.nc == NULL)
        mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_pkg_stat)); 
      break;
    case ORCH_PKG_BAD:
      break;
    case ORCH_PKG_READY:
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_tdr_run)); 
      break;
    case ORCH_TDR_RUN:
      mg_start_thread(cgw_msg_thread, (void *) &(g_ctx.cgw_api_tdr_stat)); 
      break;
    case ORCH_TDR_FAIL:
      break;
    case ORCH_TDR_SUCC:
      mg_send(g_ctx.dmc, "{\"result\":\"TDR run succesful\n\"}", 31);
      break;
    default:
      break;
  } 
}
//---------------------------------------------------------------------------
// Main
//---------------------------------------------------------------------------
static void init_context() {
  g_ctx.dmc = NULL;
  g_ctx.hmi = NULL;

  g_ctx.cgw_thread_exit = 0;
  g_ctx.hmi_thread_exit = 0;
  g_ctx.downloader_thread_exit = 0;

  init_hmi_objs(&g_ctx);

  // CGW http API
  g_ctx.cgw_api_pkg_new.api = "http://127.0.0.1:8018/pkg/new";
  g_ctx.cgw_api_pkg_new.fn = cgw_handler_pkg_new;
  g_ctx.cgw_api_pkg_new.nc = NULL;
  g_ctx.cgw_api_pkg_stat.api = "http://127.0.0.1:8018/pkg/sta";
  g_ctx.cgw_api_pkg_stat.fn = cgw_handler_pkg_stat;
  g_ctx.cgw_api_pkg_stat.nc = NULL;
  g_ctx.cgw_api_tdr_run.api = "http://127.0.0.1:8018/tdr/run";
  g_ctx.cgw_api_tdr_run.fn = cgw_handler_tdr_run;
  g_ctx.cgw_api_tdr_run.nc = NULL;
  g_ctx.cgw_api_tdr_stat.api = "http://127.0.0.1:8018/tdr/stat";
  g_ctx.cgw_api_tdr_stat.fn = cgw_handler_tdr_stat;
  g_ctx.cgw_api_tdr_stat.nc = NULL;
  // TODO: so far use curl to upload
  g_ctx.cgw_api_pkg_upload.api = "http://127.0.0.1:8018/upload";
  g_ctx.cgw_api_pkg_upload.fn = NULL;
  g_ctx.cgw_api_pkg_upload.nc = NULL;

  g_ctx.cmd_buf = g_cmd_buf;
  g_ctx.cmd_output = g_cmd_output;

  g_ctx.pkg_cdn_url = "ftp://speedtest.tele2.net/1KB.zip";
  
  g_ctx.downloader = "curl --output wpc.1.0.0"; 
//  g_ctx.downloader = "/data/duc/test_interface/dlc";

  // Change to FTPS
  g_ctx.tftp_server = "sudo ./tftpserver"; 
}

int main(int argc, char *argv[]) {
  struct mg_mgr mgr;

  init_context();

  mg_mgr_init(&mgr, NULL);

  printf("==Start socket server==\n");
  if (argc >= 2 && strcmp(argv[1], "-o") == 0) {
    // Listen to specidied port
    mg_bind(&mgr, argv[2], dmc_msg_handler);
    printf("Listen on port %s\n", argv[2]);
  } else {
    // by default, listen to 3000
    mg_bind(&mgr, "3000", dmc_msg_handler);
    printf("Listen on port 3000\n");
  }

  for (;;) {
    if (g_stat_lock)
      continue;

    g_stat_lock = 1;

    mg_mgr_poll(&mgr, 1000);
    g_stat_lock = 0;
  }
  mg_mgr_free(&mgr);

  return 0;
}
