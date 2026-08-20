// Gianluca Mazzini @2026- Version 2.02
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GMSTORE_BLOCK 1024
#define GMSTORE_LINE 512
#define GMSTORE_PATH 512
#define GMSTORE_INPUT 4096
#define GMSTORE_OUTPUT 16384
#define GMSTORE_CLIENTS 16
#define GMSTORE_BACKLOG 16

#define GM_PENDING_NONE 0
#define GM_PENDING_WRITE 1
#define GM_PENDING_APPEND 2

struct gm_client {
  int fd;
  unsigned char input[GMSTORE_INPUT];
  size_t input_len;
  unsigned char output[GMSTORE_OUTPUT];
  size_t output_pos;
  size_t output_len;
  int pending;
  size_t pending_count;
  char pending_path[GMSTORE_PATH];
};

static int gm_make_dirs(char *path) {
  char *p;

  if (!path || !*path) return 0;
  for (p=path+1;*p;p++) {
    if (*p!='/') continue;
    *p=0;
    if (mkdir(path,0755)<0 && errno!=EEXIST) {
      *p='/';
      return 0;
    }
    *p='/';
  }
  if (mkdir(path,0755)<0 && errno!=EEXIST) return 0;
  return 1;
}

static int gm_text2(char *out,size_t cap,const char *a,const char *b) {
  size_t na;
  size_t nb;

  na=strlen(a);
  nb=strlen(b);
  if (na+nb+1>cap) return 0;
  memcpy(out,a,na);
  memcpy(out+na,b,nb+1);
  return 1;
}

static int gm_name_valid(const char *name) {
  const unsigned char *p;
  const unsigned char *start;
  size_t len;

  if (!name || name[0]!='/' || !name[1]) return 0;
  p=(const unsigned char *)name+1;
  start=p;
  for (;;) {
    if (!*p || *p=='/') {
      len=(size_t)(p-start);
      if (!len || (len==1 && start[0]=='.') ||
          (len==2 && start[0]=='.' && start[1]=='.')) return 0;
      if (!*p) return 1;
      start=p+1;
    } else if (*p<33 || *p>126) return 0;
    p++;
  }
}

static int gm_open_parent(int rootfd,const char *name,int create,char *leaf,size_t cap) {
  char path[GMSTORE_PATH];
  char *part;
  char *slash;
  int current;
  int next;
  size_t len;

  if (!gm_name_valid(name) || strlen(name)>=sizeof(path)) return -1;
  strcpy(path,name+1);
  current=dup(rootfd);
  if (current<0) return -1;
  part=path;
  for (;;) {
    slash=strchr(part,'/');
    if (!slash) break;
    *slash=0;
    if (create && mkdirat(current,part,0755)<0 && errno!=EEXIST) {
      close(current);
      return -1;
    }
    next=openat(current,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (next<0) {
      close(current);
      return -1;
    }
    close(current);
    current=next;
    part=slash+1;
  }
  len=strlen(part);
  if (!len || len+1>cap) {
    close(current);
    return -1;
  }
  memcpy(leaf,part,len+1);
  return current;
}

static int gm_number(const char *text,unsigned long *value) {
  char *end;
  unsigned long n;

  if (!text || !*text) return 0;
  errno=0;
  n=strtoul(text,&end,10);
  if (errno || *end) return 0;
  *value=n;
  return 1;
}

static void gm_client_reset(struct gm_client *client) {
  client->fd=-1;
  client->input_len=0;
  client->output_pos=0;
  client->output_len=0;
  client->pending=GM_PENDING_NONE;
  client->pending_count=0;
  client->pending_path[0]=0;
}

static void gm_client_close(struct gm_client *client) {
  if (client->fd>=0) close(client->fd);
  gm_client_reset(client);
}

static int gm_nonblock(int fd) {
  int flags;

  flags=fcntl(fd,F_GETFL,0);
  if (flags<0) return 0;
  return fcntl(fd,F_SETFL,flags|O_NONBLOCK)>=0;
}

static int gm_output_add(struct gm_client *client,const void *data,size_t len) {
  if (client->output_len+len>sizeof(client->output)) return 0;
  memcpy(client->output+client->output_len,data,len);
  client->output_len+=len;
  return 1;
}

static int gm_output_line(struct gm_client *client,const char *text) {
  size_t len;

  len=strlen(text);
  if (!gm_output_add(client,text,len)) return 0;
  return gm_output_add(client,"\n",1);
}

static void gm_output_error(struct gm_client *client,const char *text) {
  client->output_pos=0;
  client->output_len=0;
  if (!gm_output_line(client,text)) gm_client_close(client);
}

static int gm_list_collect(struct gm_client *client,int dirfd,const char *rel) {
  char childrel[GMSTORE_PATH];
  int scanfd;
  int childfd;
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  char item[GMSTORE_PATH+8];

  scanfd=dup(dirfd);
  if (scanfd<0) return 0;
  dir=fdopendir(scanfd);
  if (!dir) {
    close(scanfd);
    return 0;
  }
  for (;;) {
    entry=readdir(dir);
    if (!entry) break;
    if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
    if (fstatat(dirfd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)<0 || S_ISLNK(st.st_mode)) continue;
    if (!gm_text2(childrel,sizeof(childrel),rel,"/")) continue;
    if (strlen(childrel)+strlen(entry->d_name)+1>sizeof(childrel)) continue;
    strcat(childrel,entry->d_name);
    if (S_ISDIR(st.st_mode)) {
      childfd=openat(dirfd,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
      if (childfd<0 || !gm_list_collect(client,childfd,childrel)) {
        if (childfd>=0) close(childfd);
        closedir(dir);
        return 0;
      }
      close(childfd);
    } else if (S_ISREG(st.st_mode)) {
      if (!gm_text2(item,sizeof(item),"ITEM ",childrel) || !gm_output_line(client,item)) {
        closedir(dir);
        return 0;
      }
    }
  }
  closedir(dir);
  return 1;
}

static void gm_input_consume(struct gm_client *client,size_t len) {
  if (len>=client->input_len) {
    client->input_len=0;
    return;
  }
  memmove(client->input,client->input+len,client->input_len-len);
  client->input_len-=len;
}

static int gm_pending_write(struct gm_client *client,int rootfd) {
  char leaf[GMSTORE_PATH];
  int dirfd;
  int fd;
  int flags;
  ssize_t n;
  size_t done;

  if (client->pending==GM_PENDING_NONE) return 1;
  if (client->input_len<client->pending_count) return 1;
  dirfd=gm_open_parent(rootfd,client->pending_path,1,leaf,sizeof(leaf));
  if (dirfd<0) {
    gm_output_error(client,"ERR path");
    client->pending=GM_PENDING_NONE;
    return client->fd>=0;
  }
  flags=O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC;
  if (client->pending==GM_PENDING_APPEND) flags|=O_APPEND;
  else flags|=O_TRUNC;
  fd=openat(dirfd,leaf,flags,0644);
  close(dirfd);
  if (fd<0) {
    gm_output_error(client,"ERR write");
    client->pending=GM_PENDING_NONE;
    return client->fd>=0;
  }
  done=0;
  while (done<client->pending_count) {
    n=write(fd,client->input+done,client->pending_count-done);
    if (n<0 && errno==EINTR) continue;
    if (n<=0) break;
    done+=(size_t)n;
  }
  if (close(fd)<0 || done!=client->pending_count) {
    gm_output_error(client,"ERR write");
    client->pending=GM_PENDING_NONE;
    return client->fd>=0;
  }
  gm_input_consume(client,client->pending_count);
  client->pending=GM_PENDING_NONE;
  client->pending_count=0;
  client->pending_path[0]=0;
  if (!gm_output_line(client,"OK")) gm_client_close(client);
  return client->fd>=0;
}

static int gm_open_regular(int rootfd,const char *name,int *fd,struct stat *st) {
  char leaf[GMSTORE_PATH];
  int dirfd;
  int file;
  struct stat local;

  dirfd=gm_open_parent(rootfd,name,0,leaf,sizeof(leaf));
  if (dirfd<0) return 0;
  file=openat(dirfd,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
  close(dirfd);
  if (file<0) return 0;
  if (fstat(file,&local)<0 || !S_ISREG(local.st_mode)) {
    close(file);
    return 0;
  }
  if (st) *st=local;
  *fd=file;
  return 1;
}

static int gm_command(struct gm_client *client,int rootfd,char *line) {
  char *part[4];
  char leaf[GMSTORE_PATH];
  char reply[64];
  char *token;
  unsigned char data[GMSTORE_BLOCK];
  struct stat st;
  unsigned long offset;
  unsigned long count;
  ssize_t got;
  size_t parts;
  int dirfd;
  int fd;

  parts=0;
  token=strtok(line," \t");
  while (token && parts<4) {
    part[parts++]=token;
    token=strtok(0," \t");
  }
  if (token) return gm_output_line(client,"ERR syntax");
  if (!parts) return 1;
  if (!strcmp(part[0],"PING") && parts==1) return gm_output_line(client,"OK PONG");
  if (!strcmp(part[0],"LIST") && parts==1) {
    client->output_pos=0;
    client->output_len=0;
    if (!gm_list_collect(client,rootfd,"")) {
      gm_output_error(client,"ERR list");
      return client->fd>=0;
    }
    if (!gm_output_line(client,"OK END")) {
      gm_output_error(client,"ERR list");
      return client->fd>=0;
    }
    return 1;
  }
  if (parts<2 || !gm_name_valid(part[1])) return gm_output_line(client,"ERR path");
  if (!strcmp(part[0],"STAT") && parts==2) {
    if (!gm_open_regular(rootfd,part[1],&fd,&st)) return gm_output_line(client,"ERR missing");
    close(fd);
    sprintf(reply,"OK %llu",(unsigned long long)st.st_size);
    return gm_output_line(client,reply);
  }
  if (!strcmp(part[0],"READ") && parts==4) {
    if (!gm_number(part[2],&offset) || !gm_number(part[3],&count) ||
        count>GMSTORE_BLOCK || offset>LONG_MAX) return gm_output_line(client,"ERR read");
    if (!gm_open_regular(rootfd,part[1],&fd,0)) return gm_output_line(client,"ERR read");
    if (lseek(fd,(off_t)offset,SEEK_SET)<0) {
      close(fd);
      return gm_output_line(client,"ERR read");
    }
    do {
      got=read(fd,data,(size_t)count);
    } while (got<0 && errno==EINTR);
    close(fd);
    if (got<0) return gm_output_line(client,"ERR read");
    sprintf(reply,"OK %lu",(unsigned long)got);
    if (!gm_output_line(client,reply)) return 0;
    return !got || gm_output_add(client,data,(size_t)got);
  }
  if ((!strcmp(part[0],"WRITE") || !strcmp(part[0],"APPEND")) && parts==3) {
    if (!gm_number(part[2],&count) || count>GMSTORE_BLOCK) return gm_output_line(client,"ERR size");
    if (strlen(part[1])+1>sizeof(client->pending_path)) return gm_output_line(client,"ERR path");
    strcpy(client->pending_path,part[1]);
    client->pending=!strcmp(part[0],"APPEND")?GM_PENDING_APPEND:GM_PENDING_WRITE;
    client->pending_count=(size_t)count;
    return gm_pending_write(client,rootfd);
  }
  if (!strcmp(part[0],"DEL") && parts==2) {
    dirfd=gm_open_parent(rootfd,part[1],0,leaf,sizeof(leaf));
    if (dirfd<0) return gm_output_line(client,"ERR missing");
    if (fstatat(dirfd,leaf,&st,AT_SYMLINK_NOFOLLOW)<0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
      close(dirfd);
      return gm_output_line(client,"ERR missing");
    }
    if (unlinkat(dirfd,leaf,0)<0) {
      close(dirfd);
      return gm_output_line(client,"ERR delete");
    }
    close(dirfd);
    return gm_output_line(client,"OK");
  }
  return gm_output_line(client,"ERR syntax");
}

static int gm_process_input(struct gm_client *client,int rootfd) {
  char line[GMSTORE_LINE];
  unsigned char *nl;
  size_t len;
  size_t i;

  if (client->output_len!=client->output_pos) return 1;
  if (client->pending!=GM_PENDING_NONE) return gm_pending_write(client,rootfd);
  nl=(unsigned char *)memchr(client->input,'\n',client->input_len);
  if (!nl) {
    if (client->input_len==sizeof(client->input)) {
      client->input_len=0;
      gm_output_error(client,"ERR syntax");
    }
    return client->fd>=0;
  }
  len=(size_t)(nl-client->input);
  if (len && client->input[len-1]=='\r') len--;
  if (len+1>sizeof(line)) {
    gm_input_consume(client,(size_t)(nl-client->input)+1);
    gm_output_error(client,"ERR syntax");
    return client->fd>=0;
  }
  for (i=0;i<len;i++) {
    if (client->input[i]<32 || client->input[i]>126) {
      gm_input_consume(client,(size_t)(nl-client->input)+1);
      gm_output_error(client,"ERR syntax");
      return client->fd>=0;
    }
    line[i]=(char)client->input[i];
  }
  line[len]=0;
  gm_input_consume(client,(size_t)(nl-client->input)+1);
  if (!gm_command(client,rootfd,line)) {
    gm_client_close(client);
    return 0;
  }
  return client->fd>=0;
}

static int gm_client_read(struct gm_client *client,int rootfd) {
  ssize_t n;

  if (client->input_len==sizeof(client->input)) return gm_process_input(client,rootfd);
  for (;;) {
    n=recv(client->fd,client->input+client->input_len,
           sizeof(client->input)-client->input_len,0);
    if (n<0 && errno==EINTR) continue;
    if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) break;
    if (n<=0) return 0;
    client->input_len+=(size_t)n;
    break;
  }
  return gm_process_input(client,rootfd);
}

static int gm_client_write(struct gm_client *client,int rootfd) {
  ssize_t n;

  while (client->output_pos<client->output_len) {
    n=send(client->fd,client->output+client->output_pos,
           client->output_len-client->output_pos,0);
    if (n<0 && errno==EINTR) continue;
    if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 1;
    if (n<=0) return 0;
    client->output_pos+=(size_t)n;
  }
  client->output_pos=0;
  client->output_len=0;
  return gm_process_input(client,rootfd);
}

static int gm_client_add(struct gm_client clients[],int fd) {
  int i;

  for (i=0;i<GMSTORE_CLIENTS;i++) {
    if (clients[i].fd>=0) continue;
    gm_client_reset(&clients[i]);
    clients[i].fd=fd;
    if (!gm_output_line(&clients[i],"GMSTORE 2.0")) {
      gm_client_close(&clients[i]);
      return 0;
    }
    return 1;
  }
  return 0;
}

static void gm_accept_clients(int server,struct gm_client clients[]) {
  struct sockaddr_in peer;
  socklen_t peer_len;
  int client;

  for (;;) {
    peer_len=sizeof(peer);
    client=accept(server,(struct sockaddr *)&peer,&peer_len);
    if (client<0 && errno==EINTR) continue;
    if (client<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return;
    if (client<0) return;
    if (!gm_nonblock(client) || !gm_client_add(clients,client)) {
      close(client);
      continue;
    }
    printf("connect %s:%u\n",inet_ntoa(peer.sin_addr),(unsigned int)ntohs(peer.sin_port));
    fflush(stdout);
  }
}

static void gm_usage(const char *name) {
  fprintf(stderr,"usage: %s [--root DIR] [--host IPv4] [--port PORT]\n",name);
}

int main(int argc,char **argv) {
  char root[GMSTORE_PATH];
  char resolved[GMSTORE_PATH];
  const char *root_arg;
  const char *host;
  struct sockaddr_in addr;
  struct gm_client clients[GMSTORE_CLIENTS];
  struct pollfd fds[GMSTORE_CLIENTS+1];
  unsigned long port;
  int rootfd;
  int server;
  int yes;
  int i;
  int rc;

  root_arg="store";
  host="0.0.0.0";
  port=7070;
  for (i=1;i<argc;i++) {
    if (!strcmp(argv[i],"--root") && i+1<argc) root_arg=argv[++i];
    else if (!strcmp(argv[i],"--host") && i+1<argc) host=argv[++i];
    else if (!strcmp(argv[i],"--port") && i+1<argc) {
      if (!gm_number(argv[++i],&port) || !port || port>65535) {
        gm_usage(argv[0]);
        return 1;
      }
    } else {
      gm_usage(argv[0]);
      return 1;
    }
  }
  if (strlen(root_arg)+1>sizeof(root)) {
    fprintf(stderr,"gmstored: root path too long\n");
    return 1;
  }
  strcpy(root,root_arg);
  if (!gm_make_dirs(root)) {
    perror("gmstored: root");
    return 1;
  }
  if (!realpath(root,resolved)) {
    perror("gmstored: realpath");
    return 1;
  }
  if (strlen(resolved)+1>sizeof(root)) {
    fprintf(stderr,"gmstored: resolved root path too long\n");
    return 1;
  }
  strcpy(root,resolved);
  rootfd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
  if (rootfd<0) {
    perror("gmstored: open root");
    return 1;
  }
  signal(SIGPIPE,SIG_IGN);
  for (i=0;i<GMSTORE_CLIENTS;i++) gm_client_reset(&clients[i]);
  server=socket(AF_INET,SOCK_STREAM,0);
  if (server<0) {
    perror("gmstored: socket");
    close(rootfd);
    return 1;
  }
  yes=1;
  if (setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes))<0 || !gm_nonblock(server)) {
    perror("gmstored: socket setup");
    close(server);
    close(rootfd);
    return 1;
  }
  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_port=htons((uint16_t)port);
  if (inet_pton(AF_INET,host,&addr.sin_addr)!=1) {
    fprintf(stderr,"gmstored: invalid IPv4 address\n");
    close(server);
    close(rootfd);
    return 1;
  }
  if (bind(server,(struct sockaddr *)&addr,sizeof(addr))<0) {
    perror("gmstored: bind");
    close(server);
    close(rootfd);
    return 1;
  }
  if (listen(server,GMSTORE_BACKLOG)<0) {
    perror("gmstored: listen");
    close(server);
    close(rootfd);
    return 1;
  }
  printf("GMSTORE 2.0 %s:%lu root=%s clients=%d\n",host,port,root,GMSTORE_CLIENTS);
  fflush(stdout);
  for (;;) {
    fds[0].fd=server;
    fds[0].events=POLLIN;
    fds[0].revents=0;
    for (i=0;i<GMSTORE_CLIENTS;i++) {
      fds[i+1].fd=clients[i].fd;
      fds[i+1].events=0;
      fds[i+1].revents=0;
      if (clients[i].fd<0) continue;
      if (clients[i].output_pos<clients[i].output_len) fds[i+1].events|=POLLOUT;
      else fds[i+1].events|=POLLIN;
    }
    rc=poll(fds,GMSTORE_CLIENTS+1,-1);
    if (rc<0 && errno==EINTR) continue;
    if (rc<0) {
      perror("gmstored: poll");
      break;
    }
    if (fds[0].revents&POLLIN) gm_accept_clients(server,clients);
    for (i=0;i<GMSTORE_CLIENTS;i++) {
      if (clients[i].fd<0) continue;
      if (fds[i+1].revents&(POLLERR|POLLHUP|POLLNVAL)) {
        gm_client_close(&clients[i]);
        printf("disconnect\n");
        fflush(stdout);
        continue;
      }
      if ((fds[i+1].revents&POLLOUT) && !gm_client_write(&clients[i],rootfd)) {
        gm_client_close(&clients[i]);
        printf("disconnect\n");
        fflush(stdout);
        continue;
      }
      if (clients[i].fd>=0 && (fds[i+1].revents&POLLIN) && !gm_client_read(&clients[i],rootfd)) {
        gm_client_close(&clients[i]);
        printf("disconnect\n");
        fflush(stdout);
      }
    }
  }
  for (i=0;i<GMSTORE_CLIENTS;i++) gm_client_close(&clients[i]);
  close(server);
  close(rootfd);
  return 1;
}
