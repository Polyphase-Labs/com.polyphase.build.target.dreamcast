
#ifndef _ELF_H
#define _ELF_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>

#define	EI_CLASS	4
#define	ELFCLASS32	1

#define EM_SH   42  /* SuperH */

#define PF_R    0x4
#define PF_W    0x2
#define PF_X    0x1

//
// Segment Types
//
// Unused segment
#define PT_NULL 0
// Loadable segment 
#define PT_LOAD 1
// Dynamic linking tables
#define PT_DYNAMIC 2
// Program interpreter path name
#define PT_INTERP 3 
// Note Section
#define PT_NOTE 4
// Reserved
#define PT_SHLIB 5
// Program Header Table
#define PT_PHDR 6
// Processor Specific Start
#define PT_LOPROC 0x70000000
// Processor Specific End
#define PT_HIPROC 0x7fffffff

//
// Section Types
//

#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

#define Elf32_Addr  uint32_t
#define Elf32_Half  uint16_t
#define Elf32_Off   uint32_t
#define Elf32_Sword uint32_t
#define Elf32_Word  uint32_t

#define EI_NIDENT 16

/*  ELF file header */
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half  e_type;
    Elf32_Half  e_machine;
    Elf32_Word  e_version;
    Elf32_Addr  e_entry;
    Elf32_Off   e_phoff;
    Elf32_Off   e_shoff;
    Elf32_Word  e_flags;
    Elf32_Half  e_ehsize;
    Elf32_Half  e_phentsize;
    Elf32_Half  e_phnum;
    Elf32_Half  e_shentsize;
    Elf32_Half  e_shnum;
    Elf32_Half  e_shstrndx;
} Elf32_Ehdr;

/* Program Header */
typedef struct {
    Elf32_Word  p_type;
    Elf32_Off   p_offset;
    Elf32_Addr  p_vaddr;
    Elf32_Addr  p_paddr;
    Elf32_Word  p_filesz;
    Elf32_Word  p_memsz;
    Elf32_Word  p_flags;
    Elf32_Word  p_align;
} Elf32_Phdr;

/* ELF Section header */
typedef struct {
    Elf32_Word  sh_name;
    Elf32_Word  sh_type;
    Elf32_Word  sh_flags;
    Elf32_Addr  sh_addr;
    Elf32_Off   sh_offset;
    Elf32_Word  sh_size;
    Elf32_Word  sh_link;
    Elf32_Word  sh_info;
    Elf32_Word  sh_addralign;
    Elf32_Word  sh_entsize;
} Elf32_Shdr;

/* Symbol table entry */
typedef struct {
    Elf32_Word      st_name;
    Elf32_Addr      st_value;
    Elf32_Word      st_size;
    unsigned char   st_info;
    unsigned char   st_other;
    Elf32_Half      st_shndx;
} Elf32_Sym;

#if defined(__cplusplus)
}
#endif

#endif