// Gianluca Mazzini @2026- Version 2.02
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gmabi.h"


static uint32_t gm_checksum(const unsigned char *data,uint32_t len) {
  uint32_t hash;
  uint32_t i;

  hash=2166136261U;
  for (i=0;i<len;i++) {
    hash^=data[i];
    hash*=16777619U;
  }
  return hash;
}

static void gm_put32(unsigned char *p,uint32_t value) {
  p[0]=(unsigned char)value;
  p[1]=(unsigned char)(value>>8);
  p[2]=(unsigned char)(value>>16);
  p[3]=(unsigned char)(value>>24);
}

static int gm_read(FILE *file,void *data,size_t len,long offset) {
  if (fseek(file,offset,SEEK_SET)) return 0;
  return fread(data,1,len,file)==len;
}

static int gm_arch(uint16_t machine,uint32_t *arch,const char **name) {
  if (machine==EM_X86_64) {
    *arch=GM_ARCH_X86_64;
    *name="x86_64";
    return 1;
  }
  if (machine==EM_AARCH64) {
    *arch=GM_ARCH_AARCH64;
    *name="aarch64";
    return 1;
  }
  if (machine==EM_RISCV) {
    *arch=GM_ARCH_RISCV64;
    *name="riscv64";
    return 1;
  }
  return 0;
}

int main(int argc,char **argv) {
  Elf64_Ehdr eh;
  Elf64_Phdr ph;
  unsigned char image[GM_PROGRAM_IMAGE_MAX];
  unsigned char header[GM_PROGRAM_HEADER_SIZE];
  const char *arch_name;
  FILE *in;
  FILE *out;
  uint64_t end;
  uint64_t seg_end;
  uint64_t entry;
  uint64_t off;
  uint32_t arch;
  uint32_t image_size;
  uint32_t checksum;
  uint16_t i;
  int found;
  int entry_exec;

  if (argc!=3) {
    fprintf(stderr,"usage: %s program.elf program.gm\n",argv[0]);
    return 1;
  }
  in=fopen(argv[1],"rb");
  if (!in) {
    perror("gmpack: input");
    return 1;
  }
  if (!gm_read(in,&eh,sizeof(eh),0) || memcmp(eh.e_ident,ELFMAG,SELFMAG) ||
      eh.e_ident[EI_CLASS]!=ELFCLASS64 || eh.e_ident[EI_DATA]!=ELFDATA2LSB ||
      eh.e_ident[EI_VERSION]!=EV_CURRENT || eh.e_type!=ET_EXEC || eh.e_version!=EV_CURRENT ||
      !eh.e_phnum || eh.e_phentsize!=sizeof(Elf64_Phdr) ||
      !gm_arch(eh.e_machine,&arch,&arch_name)) {
    fprintf(stderr,"gmpack: invalid ELF64 executable\n");
    fclose(in);
    return 1;
  }
  if (eh.e_entry<GM_PROGRAM_BASE || eh.e_entry>=GM_PROGRAM_BASE+GM_PROGRAM_IMAGE_MAX) {
    fprintf(stderr,"gmpack: entry outside gm arena\n");
    fclose(in);
    return 1;
  }
  memset(image,0,sizeof(image));
  end=GM_PROGRAM_BASE;
  found=0;
  entry_exec=0;
  for (i=0;i<eh.e_phnum;i++) {
    if (!gm_read(in,&ph,sizeof(ph),(long)(eh.e_phoff+(uint64_t)i*eh.e_phentsize))) {
      fprintf(stderr,"gmpack: program header read error\n");
      fclose(in);
      return 1;
    }
    if (ph.p_type!=PT_LOAD || !ph.p_memsz) continue;
    if (ph.p_vaddr<GM_PROGRAM_BASE || ph.p_vaddr>GM_PROGRAM_BASE+GM_PROGRAM_IMAGE_MAX || ph.p_memsz>GM_PROGRAM_IMAGE_MAX ||
        ph.p_vaddr+ph.p_memsz<ph.p_vaddr || ph.p_vaddr+ph.p_memsz>GM_PROGRAM_BASE+GM_PROGRAM_IMAGE_MAX ||
        ph.p_filesz>ph.p_memsz) {
      fprintf(stderr,"gmpack: segment outside gm arena\n");
      fclose(in);
      return 1;
    }
    off=ph.p_vaddr-GM_PROGRAM_BASE;
    if (ph.p_filesz && !gm_read(in,image+off,(size_t)ph.p_filesz,(long)ph.p_offset)) {
      fprintf(stderr,"gmpack: segment read error\n");
      fclose(in);
      return 1;
    }
    seg_end=ph.p_vaddr+ph.p_memsz;
    if ((ph.p_flags&PF_X) && eh.e_entry>=ph.p_vaddr && eh.e_entry<seg_end) entry_exec=1;
    if (seg_end>end) end=seg_end;
    found=1;
  }
  fclose(in);
  if (!found || !entry_exec || end<=GM_PROGRAM_BASE) {
    fprintf(stderr,"gmpack: no executable entry image\n");
    return 1;
  }
  image_size=(uint32_t)(end-GM_PROGRAM_BASE);
  entry=eh.e_entry-GM_PROGRAM_BASE;
  if (entry>=image_size) {
    fprintf(stderr,"gmpack: invalid entry\n");
    return 1;
  }
  checksum=gm_checksum(image,image_size);
  memset(header,0,sizeof(header));
  header[0]='G';
  header[1]='M';
  header[2]='0';
  header[3]='1';
  gm_put32(header+4,GM_API_VERSION);
  gm_put32(header+8,arch);
  gm_put32(header+12,image_size);
  gm_put32(header+16,(uint32_t)entry);
  gm_put32(header+20,checksum);
  out=fopen(argv[2],"wb");
  if (!out) {
    perror("gmpack: output");
    return 1;
  }
  if (fwrite(header,1,sizeof(header),out)!=sizeof(header) ||
      fwrite(image,1,image_size,out)!=image_size || fclose(out)) {
    fprintf(stderr,"gmpack: write error\n");
    return 1;
  }
  printf("GM01 api=%u arch=%s image=%u entry=%llu checksum=%08x\n",
         GM_API_VERSION,arch_name,image_size,(unsigned long long)entry,checksum);
  return 0;
}
