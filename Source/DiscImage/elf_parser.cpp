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

#include "elf_parser.hpp"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>

namespace elfparser {

/**
 *  Selects the segments from an elf which should
 *  be loaded into memory and place them into a
 *  vector of bytes starting at 0x8c010000.
 */
bool Parser::fill_bin(std::vector<char>& bin_data)
{
    // remove non-loadable segments and ones that are not in the loadable region
    auto end_valid = std::remove_if(m_Segments.begin(), m_Segments.end(), 
        [](const Segment& segment) 
        { 
            // Can't load a segment before RAM start, some ELFs 
            // (often dcload & libronin) have a segment ".stack" that 
            // is before RAM start address which is not what we want
            bool before_ram = segment.get_physaddr() < DC_RAM_START_ADDR;
            // type of PT_LOAD signifies this should be loaded into memory
            bool is_loadable = segment.get_type() == PT_LOAD;
            return before_ram || !is_loadable; 
        }
    );
    auto seg_count = m_Segments.size();
    m_Segments.erase(end_valid, m_Segments.end());
    std::cout << "Removed " << seg_count - m_Segments.size() << " segments" << std::endl;


    // Make sure at least one loadable segment exists or binary will be empty
    if (m_Segments.size() == 0) {
        std::cerr << "No loadable segments found!" << std::endl;
        return false;
    }

    // get begining and end section to determine bin size
    auto min_max = std::minmax_element(m_Segments.begin(), m_Segments.end(), 
        [](const Segment& left, const Segment& right){
            return left.get_physaddr() < right.get_physaddr();
        }
    );

    size_t start = min_max.first->get_physaddr();
    size_t end = min_max.second->get_physaddr() + min_max.second->get_memsize();

    // Binary should always start at DC_RAM_START_ADDR on Dreamcast
    if (start != DC_RAM_START_ADDR) {
        std::cerr << "Unexpected binary start at 0x" << std::hex << start << ", expected " << DC_RAM_START_ADDR << std::endl;
        return false;
    }
    
    std::cout << "Binary start: 0x" << std::hex << start << '\t';
    std::cout << "Binary end:   0x" << std::hex << end << '\t';
    std::cout << "Binary size:  0x" << std::hex << (end - start) << std::endl;

    if ( !bin_data.empty()) {
        std::cerr << "Failed to empty binary vector!" << std::endl;
        return false;
    }
    // Vector should have enough space to hold all segments
    bin_data.resize(end - start);

    for (const Segment& segment : m_Segments) {
        // Skip `LOAD` segment for `.bss` (this happens occasionally)
        if (segment.get_filesize() == 0)
            continue;
        // Offset from begining of binary
        auto offset = segment.get_physaddr() - start;
        // Get data from segment and copy it into the binary    
        segment.write_data_to(&bin_data[offset], segment.get_filesize());
    }
    return true;
}

std::optional<std::shared_ptr<Parser>> Parser::Load(const std::vector<char>& elf_data)
{
    const char * elf_start = &elf_data[0];
    // Really make sure this is an elf
    assert(elf_data[0] == 0x7F);
    assert(elf_data[1] == 'E');
    assert(elf_data[2] == 'L');
    assert(elf_data[3] == 'F');

    // ELF header starts at the begining of the file
    const Elf32_Ehdr* elf_header = (const Elf32_Ehdr*)&elf_data[0];

    // Check if ELF is for SuperH
    if( elf_header->e_machine != EM_SH ) {
        std::cout << "Unsupported Architecture (0x" << std::hex << elf_header->e_machine << ")" << std::endl;
        return {};
    }

    // Check if ELF is 32bit
    if( elf_header->e_ident[EI_CLASS] != ELFCLASS32) {
        std::cout << "ELF is not 32bit" << std::endl; 
        return {};
    }

    // Holds all the segments and sections after parsing
    std::vector<Segment> segments;
    std::vector<Section> sections;

    // Program Header Table can be found using e_phoff from the begining of the elf
    const char * program_header_table = elf_start + elf_header->e_phoff;
    // Each entry in the program header table is e_phentsize bytes long
    auto segment_entry_size = elf_header->e_phentsize;
    // The number of entries in the program header table is e_phnum
    auto segment_entry_count = elf_header->e_phnum;

    // Section Header Table can be found using e_shoff from the begining of the elf
    const char * section_header_table = elf_start + elf_header->e_shoff;
    // Each entry in the section header table is e_shentsize bytes long
    auto section_entry_size = elf_header->e_shentsize;
    // Each entry in the section header table is e_shnum bytes long
    auto section_entry_count = elf_header->e_shnum;
    
    // String look up table can be found using section header table with an index of e_shstrndx
    const Elf32_Shdr* string_names_section = (const Elf32_Shdr*) (section_header_table + (elf_header->e_shstrndx * section_entry_size));
    // Get pointer to the string names data using the offset from the Section Header
    const char * string_names_data = elf_start + string_names_section->sh_offset;
    // Make sure the string names section is a string table
    if (string_names_section->sh_type != SHT_STRTAB) {
        std::cerr << "Expected section header selected by e_shstrndx to be a string table (%d)" << std::hex << string_names_section->sh_type << std::endl;
        return {};
    }
    
    //
    // Parse Segments
    //      For every Program Header (one per segment) in the ELF
    for( int index = 0; index < segment_entry_count; index++ ) {
        // Pointer to Program Header at index
        auto program_header = (const Elf32_Phdr*)(program_header_table + (index * segment_entry_size));
        // Add segment to list of segments
        segments.push_back(Segment(program_header, elf_data));
    }

    //
    // Parse Sections
    //      For every Section Header in the ELF
    for( int index = 0; index < section_entry_count; index++ ) {
        // Pointer to Section Header at index
        auto section_header = (const Elf32_Shdr*)(section_header_table + (index * section_entry_size));
        // Get the name of the section from the string names table
        const char * name = string_names_data + section_header->sh_name;
        // Add segment to list of segments
        sections.push_back(Section(section_header, elf_data, name));
    }
    
    // Return a shared pointer to the parser
    return std::shared_ptr<Parser>(new Parser(elf_data.size(), segments, sections));
}

std::optional<std::shared_ptr<Parser>> Parser::Load(const std::filesystem::path& elfPath)
{
    auto size = static_cast<int32_t>(std::filesystem::file_size(elfPath));
    std::ifstream stream;
    stream.open(elfPath, std::ios::binary);
    if (!stream.is_open()) {
        std::cerr << "Failed to open elf at " << elfPath << std::endl;
        return {};
    }
    std::vector<char> elf_data(size);
    stream.read(&elf_data[0], size);
    std::cout << "Loaded " << elf_data.size() << " bytes from ELF" << std::endl;
    return Load(elf_data);
}

}
