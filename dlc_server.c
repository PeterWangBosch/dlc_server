/*
 * Copyright (c) 2014 Cesanta Software Limited
 * All rights reserved
 */

#include "stdio.h"
#include "src/mongoose.h"

static const char *s_http_port = "8018";
static char *s_dlc_path = "/data/duc/test_interface";
static struct mg_serve_http_opts s_http_server_opts;
static char s_tdr_stat[512] = { 0 };
static unsigned int i_tdr_stat_len = 0;

/*keep this for testing*/
static void handle_sum_call(struct mg_connection *nc, struct http_message *hm) {
  char n1[100], n2[100];
  double result;

  /* Get form variables */
  mg_get_http_var(&hm->body, "n1", n1, sizeof(n1));
  mg_get_http_var(&hm->body, "n2", n2, sizeof(n2));

  /* Send headers */
  mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");

  /* Compute the result and send it back as a JSON object */
  result = strtod(n1, NULL) + strtod(n2, NULL);
  mg_printf_http_chunk(nc, "{ \"result\": %lf }", result);
  mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
}

static void check_tdr_status(struct mg_connection *nc) {
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "{ \"TDR status\": \"%s\"}", s_tdr_stat);
    mg_send_http_chunk(nc, "", 0);
    return;
}

static void socket_check_tdr_status(struct mg_connection *nc) {
    mg_send(nc, s_tdr_stat, i_tdr_stat_len);
    return;
}

static void run_tdr_command(struct mg_connection *nc) {
    FILE *fp;
    char cmd[256] = {0};
    char output[300];

    strcpy(cmd, "ls -la ");
    if ((fp = popen(cmd, "r")) != NULL) {
        mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        mg_printf_http_chunk(nc, "{ \"TDR status\": \"Going to run\"}");
        mg_send_http_chunk(nc, "", 0);
    } else {
        mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        mg_printf_http_chunk(nc, "{ \"TDR status\": \"Failed to run\"}");
        mg_send_http_chunk(nc, "", 0);
        return;
    }

    while (fgets(output, sizeof(output)-1, fp) != NULL){
        if ((i_tdr_stat_len + sizeof(output)) >= 512) {
          sprintf(s_tdr_stat, "%s", output);
          i_tdr_stat_len = strlen(s_tdr_stat);
        } else {
          sprintf(s_tdr_stat, "%s\n %s", s_tdr_stat, output);
          i_tdr_stat_len = strlen(s_tdr_stat);
        }
    }

    pclose(fp);
}

static void socket_run_dlc_command(struct mg_connection *nc, char* url, char* dlc_path) {
    FILE *fp;
    char cmd[256] = {0};
    char output[300];

    // DLC command
    sprintf(cmd, "echo \"%s 2000000\" >> %s", url, dlc_path );

    if ((fp = popen(cmd, "r")) != NULL) {
        mg_send(nc, "{ \"DLC status\": \"Going to run\"}", 31);
    } else {
        mg_send(nc, "{ \"DLC status\": \"Failed to run\"}", 32);
        return;
    }

    while (fgets(output, sizeof(output)-1, fp) != NULL){
        if ((i_tdr_stat_len + sizeof(output)) >= 512) {
          sprintf(s_tdr_stat, "%s", output);
          i_tdr_stat_len = strlen(s_tdr_stat);
        } else {
          sprintf(s_tdr_stat, "%s\n %s", s_tdr_stat, output);
          i_tdr_stat_len = strlen(s_tdr_stat);
        }
    }

    pclose(fp);
}


static void socket_ev_handler(struct mg_connection *nc, int ev, void *p) {
  struct mbuf *io = &nc->recv_mbuf;
  (void) p;

  switch (ev) {
    case MG_EV_ACCEPT:
      mg_send(nc, "/test/live\n", 10);
      break;
    case MG_EV_RECV:
      if (strcmp((char*)io->buf, "/test/live") == 0) {
        printf("==> test live\n");
        mg_send(nc, "{\"result\":\"0.000000\"}", 21);
      } else if (strcmp((char*)io->buf, "/dlc/run") == 0) {
        printf("==> run dlc\n");
        socket_run_dlc_command(nc, (char*)(io->buf+8), s_dlc_path);
      } else if (strcmp((char*)io->buf, "/dlc/res") == 0) {
        printf("==> check result of tdr\n");
        socket_check_tdr_status(nc);
      }
      mbuf_remove(io, io->len);       // Discard message from recv buffer
      break;
    default:
      break;
  }
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;

  switch (ev) {
    case MG_EV_HTTP_REQUEST:
      if (mg_vcmp(&hm->uri, "/test/live") == 0) {
        handle_sum_call(nc, hm); /* Handle RESTful call */
      } else if (mg_vcmp(&hm->uri, "/tdr/run") == 0) {
        run_tdr_command(nc);
      } else if (mg_vcmp(&hm->uri, "/tdr/res") == 0) {
        check_tdr_status(nc);
      } else {
        //mg_serve_http(nc, hm, s_http_server_opts); /* Serve static content */
      }
      break;
    default:
      break;
  }
}

int main(int argc, char *argv[]) {
  struct mg_mgr mgr;
  struct mg_connection *nc;
  struct mg_bind_opts bind_opts;
  int i;
  char *cp;
  const char *err_str;
#if MG_ENABLE_SSL
  const char *ssl_cert = NULL;
#endif

  mg_mgr_init(&mgr, NULL);

  if (argc == 2 && strcmp(argv[1], "-o") == 0) {
    printf("Start socket server\n");
    mg_bind(&mgr, "8018", socket_ev_handler);
    printf("Listen on port 8018\n");
    for (;;) {
      mg_mgr_poll(&mgr, 1000);
    }
    mg_mgr_free(&mgr);
    return 1;
  }

  /* Use current binary directory as document root */
  if (argc > 0 && ((cp = strrchr(argv[0], DIRSEP)) != NULL)) {
    *cp = '\0';
    s_http_server_opts.document_root = argv[0];
  }

  /* Process command line options to customize HTTP server */
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
      mgr.hexdump_file = argv[++i];
    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      s_http_server_opts.document_root = argv[++i];
    } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      s_http_port = argv[++i];
    } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
      s_http_server_opts.auth_domain = argv[++i];
    } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
      s_http_server_opts.global_auth_file = argv[++i];
    } else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
      s_http_server_opts.per_directory_auth_file = argv[++i];
    } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
      s_http_server_opts.url_rewrites = argv[++i];
#if MG_ENABLE_HTTP_CGI
    } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      s_http_server_opts.cgi_interpreter = argv[++i];
#endif
#if MG_ENABLE_SSL
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      ssl_cert = argv[++i];
#endif
    } else {
      fprintf(stderr, "Unknown option: [%s]\n", argv[i]);
      exit(1);
    }
  }

  /* Set HTTP server options */
  memset(&bind_opts, 0, sizeof(bind_opts));
  bind_opts.error_string = &err_str;
#if MG_ENABLE_SSL
  if (ssl_cert != NULL) {
    bind_opts.ssl_cert = ssl_cert;
  }
#endif
  nc = mg_bind_opt(&mgr, s_http_port, ev_handler, bind_opts);
  if (nc == NULL) {
    fprintf(stderr, "Error starting server on port %s: %s\n", s_http_port,
            *bind_opts.error_string);
    exit(1);
  }

  mg_set_protocol_http_websocket(nc);
  s_http_server_opts.enable_directory_listing = "yes";

  printf("Starting RESTful server on port %s, serving %s\n", s_http_port,
         s_http_server_opts.document_root);
  for (;;) {
    mg_mgr_poll(&mgr, 1000);
  }
  mg_mgr_free(&mgr);

  return 0;
}
