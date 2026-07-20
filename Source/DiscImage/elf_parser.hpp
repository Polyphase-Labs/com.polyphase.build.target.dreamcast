// MIT License

// Copyright (c) 2018 finixbit
// Copyright (c) 2022-2023 Colton Pawielski

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Recommended reading material:
// Tool Interface Stands (TIS) Portable Formats Specificaion
// Version 1.1 can be found here: https://www.cs.cmu.edu/afs/cs/academic/class/15213-f00/docs/elf.pdf

#ifndef H_ELF_PARSER
#define H_ELF_PARSER

#include <cstdio>
#include <cstdlib>
#include <fcntl.h> /* O_RDONLY */
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string.h>
#include <string>
#include <vector>
#include "elf.h"

namespace elfparser {

class Segment {
protected:
    uint32_t m_Type, m_Offset, m_VirtAddr, m_PhysAddr, 
        m_FileSize, m_MemSize, m_Flags, m_Align;
    std::vector<char> m_Data;
public:
    Segment(const Elf32_Phdr *phdr, const std::vector<char>& elf_data) 
        : m_Type(phdr->p_type)
        , m_Offset(phdr->p_offset)
        , m_VirtAddr(phdr->p_vaddr)
        , m_PhysAddr(phdr->p_paddr)
        , m_FileSize(phdr->p_filesz)
        , m_MemSize(phdr->p_memsz)
        , m_Flags(phdr->p_flags)
        , m_Align(phdr->p_align)
        , m_Data()
        {
            auto start = &elf_data[0] + phdr->p_offset;
            m_Data = std::vector<char>(start, start + phdr->p_filesz);
    }

    uint32_t get_offset() const { return m_Offset; }
    uint32_t get_filesize() const { return m_FileSize; }
    uint32_t get_virtaddr() const { return m_VirtAddr; }
    uint32_t get_physaddr() const { return m_PhysAddr; }
    uint32_t get_memsize() const { return m_MemSize; }
    uint32_t get_align() const { return m_Align; }
    void write_data_to(char * dest, size_t len) const { memcpy(dest, &m_Data[0], len); }
    uint32_t get_type() const { return m_Type; }

};

enum SectionType : uint32_t{
    SHT_NULL = 0, // Inactive
    SHT_PROGBITS = 1, // Program data
    SHT_SYMTAB = 2, // Symbol table
    SHT_STRTAB = 3, // String table
    SHT_RELA = 4, // Relocation entries with addends
    SHT_HASH = 5, // Symbol hash table
    SHT_DYNAMIC = 6, // Dynamic linking information
    SHT_NOTE = 7, // Notes
    SHT_NOBITS = 8, // Program space with no data (bss)
    SHT_REL = 9, // Relocation entries, no addends
    SHT_SHLIB = 10, // Reserved
    SHT_DYNSYM = 11, // Dynamic linker symbol table
    SHT_LOPROC = 0x70000000, // Processor-specific Begin
    SHT_HIPROC = 0x7fffffff, // Processor-specific End
    SHT_LOUSER = 0x80000000, // User-specific Begin
    SHT_HIUSER = 0xffffffff, // User-specific End
};

class Section {
protected:
    uint32_t m_Type, m_Flags, m_Index, m_Size, m_EntSize, 
             m_AddrAlign, m_Offset, m_Address;
    std::string m_Name;
    std::vector<char> m_Data;

public:
    Section( const Elf32_Shdr* shdr, const std::vector<char> elf_data, std::string name)
        : m_Type(shdr->sh_type)
        , m_Flags(shdr->sh_flags)
        , m_Index(0)
        , m_Size(shdr->sh_size)
        , m_EntSize(shdr->sh_entsize)
        , m_AddrAlign(shdr->sh_addralign)
        , m_Offset(shdr->sh_offset)
        , m_Address(shdr->sh_addr)
        , m_Name(std::move(name))
        , m_Data()
        {
            auto start = elf_data.begin() + m_Offset;
            switch(m_Type) {
                case SHT_PROGBITS:
                    m_Data = std::vector<char>(start, start + m_Size);
                    break;
                case SHT_NOBITS:
                    m_Data = std::vector<char>(m_Size);
                    break;
            }
    }

    std::string get_name() const { return m_Name; }
    uint32_t get_flags() const { return m_Flags; }
    uint32_t get_addr() const { return m_Address; }
    uint32_t get_offset() const { return m_Offset; }
    uint32_t get_size() const { return m_Size; }
    uint32_t get_entsize() const { return m_EntSize; }
    uint32_t get_alignment() const { return m_AddrAlign; }
    void write_data_to(char * dest, size_t len) const { memcpy(dest, &m_Data[0], len); }
    SectionType get_type() const { return (SectionType) m_Type;}
};

class Parser {
private:
    static const uint32_t DC_RAM_START_ADDR = 0x8c010000;

    std::vector<Segment> m_Segments;
    std::vector<Section> m_Sections;

public:
    // Load ELF from std::vector<char>
    static std::optional<std::shared_ptr<Parser>> Load(const std::vector<char>& elfData);

    // Load ELF from Path
    static std::optional<std::shared_ptr<Parser>> Load(const std::filesystem::path& elfData);

    bool fill_bin(std::vector<char>& bin_data);

protected:
    inline Parser(size_t elf_size, std::vector<Segment> segments, std::vector<Section> sections)
        : m_Segments(std::move(segments)), m_Sections(std::move(sections)) {};
};

} // namespace elfparser
#endif