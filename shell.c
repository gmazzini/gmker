// Gianluca Mazzini @2026- Version 2.02
#include "gmker.h"

#define GM_LINE_MAX 512

static char gm_line[GM_LINE_MAX];
static uint16_t gm_line_len;

static char *gm_word(char **ps) {
  char *s;
  char *start;

  s=*ps;
  while (*s==' ' || *s=='\t') s++;
  if (!*s) {
    *ps=s;
    return 0;
  }
  start=s;
  while (*s && *s!=' ' && *s!='\t') s++;
  if (*s) *s++=0;
  *ps=s;
  return start;
}

static char *gm_rest(char *s) {
  while (*s==' ' || *s=='\t') s++;
  return *s?s:0;
}

static void gm_result(int ok) {
  gm_write(ok?"ok\n":"error\n");
}

static void gm_help(void) {
  gm_write("help                 commands\n");
  gm_write("version              kernel version\n");
  gm_write("uptime               timer ticks\n");
  gm_write("mem                  free physical memory\n");
  gm_write("net                  network status\n");
  gm_write("arp                  ARP table\n");
  gm_write("ping IP              ICMP echo\n");
  gm_write("tcp                  TCP client status\n");
  gm_write("programs             GMSTORE gm programs\n");
  gm_write("apps                 application slots\n");
  gm_write("resources            resource ownership/accounting\n");
  gm_write("run NAME [ARGS]      load and run GMSTORE program\n");
  gm_write("store status         GMSTORE status\n");
  gm_write("store connect        connect GMSTORE\n");
  gm_write("store ping           ping GMSTORE\n");
  gm_write("store ls             list files\n");
  gm_write("store stat PATH      file size\n");
  gm_write("store cat PATH       print file\n");
  gm_write("store write PATH TXT replace file\n");
  gm_write("store append PATH TXT append file\n");
  gm_write("store rm PATH        delete file\n");
  gm_write("shutdown             exit QEMU\n");
}

static void gm_store_command(char *args) {
  char *op;
  char *path;
  char *text;

  op=gm_word(&args);
  if (!op || gm_streq(op,"status")) {
    gm_store_status();
    return;
  }
  if (gm_streq(op,"connect")) {
    gm_result(gm_store_connect());
    return;
  }
  if (gm_streq(op,"ping")) {
    gm_result(gm_store_ping());
    return;
  }
  if (gm_streq(op,"ls")) {
    gm_result(gm_store_list());
    return;
  }
  path=gm_word(&args);
  if (!path) {
    gm_write("usage: store stat|cat|write|append|rm PATH\n");
    return;
  }
  if (gm_streq(op,"stat")) {
    gm_result(gm_store_stat(path));
    return;
  }
  if (gm_streq(op,"cat")) {
    if (!gm_store_cat(path)) gm_write("error\n");
    return;
  }
  if (gm_streq(op,"rm")) {
    gm_result(gm_store_delete(path));
    return;
  }
  text=gm_rest(args);
  if (gm_streq(op,"write")) {
    if (!text) text="";
    gm_result(gm_store_write(path,(const uint8_t *)text,(uint16_t)gm_strlen(text),0));
    return;
  }
  if (gm_streq(op,"append")) {
    if (!text) text="";
    gm_result(gm_store_write(path,(const uint8_t *)text,(uint16_t)gm_strlen(text),1));
    return;
  }
  gm_write("unknown store command\n");
}

static void gm_command(char *line) {
  char *args;
  char *cmd;
  char *value;
  uint8_t ip[4];
  const char *end;

  args=line;
  cmd=gm_word(&args);
  if (!cmd) return;
  if (gm_streq(cmd,"help")) gm_help();
  else if (gm_streq(cmd,"version")) gm_write("gmker " GM_VERSION "\n");
  else if (gm_streq(cmd,"uptime")) {
    gm_print_u64(gm_ticks());
    gm_write(" ticks\n");
  } else if (gm_streq(cmd,"mem")) {
    gm_print_u64(gm_memory_free()/1024ULL);
    gm_write(" KiB free\n");
  } else if (gm_streq(cmd,"net")) gm_net_status();
  else if (gm_streq(cmd,"arp")) gm_arp_status();
  else if (gm_streq(cmd,"tcp")) gm_tcp_status();
  else if (gm_streq(cmd,"store")) gm_store_command(args);
  else if (gm_streq(cmd,"programs")) gm_programs_list();
  else if (gm_streq(cmd,"apps")) gm_programs_status();
  else if (gm_streq(cmd,"resources")) gm_resources_status();
  else if (gm_streq(cmd,"run")) {
    value=gm_word(&args);
    if (!value) gm_write("usage: run NAME [ARGS]\n");
    else if (!gm_program_run(value,gm_rest(args))) gm_write("run error\n");
  } else if (gm_streq(cmd,"ping")) {
    value=gm_word(&args);
    if (!value || !gm_parse_ipv4(value,ip,&end) || *end) gm_write("usage: ping IP\n");
    else gm_result(gm_ping(ip,300U));
  } else if (gm_streq(cmd,"shutdown")) gm_shutdown();
  else gm_write("unknown command\n");
}

void gm_shell_init(void) {
  gm_line_len=0;
  gm_write("type help\n> ");
}

void gm_shell_poll(void) {
  int c;

  for (;;) {
    c=gm_serial_getc();
    if (c<0) return;
    if (c=='\n') {
      gm_putc('\n');
      gm_line[gm_line_len]=0;
      gm_command(gm_line);
      gm_line_len=0;
      gm_write("> ");
    } else if (c==8 || c==127) {
      if (gm_line_len) {
        gm_line_len--;
        gm_write("\b \b");
      }
    } else if (c>=32 && c<=126 && gm_line_len+1U<GM_LINE_MAX) {
      gm_line[gm_line_len++]=(char)c;
      gm_putc((char)c);
    }
  }
}
