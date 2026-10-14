/**
 * @file symbols.cpp
 *
 * @author Martin Olivier <martin.olivier@live.fr>
 * @copyright (c) 2025 Martin Olivier
 *
 * This library is released under MIT license
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

std::string demangle_symbol(const char *symbol);

enum internal_symbol_type : std::uint8_t {
    C,
    CPP,
};

struct internal_symbol_info {
    std::string name;
    std::string demangled_name;
    internal_symbol_type type;
    bool loadable;
};

static void add_symbol(std::vector<internal_symbol_info> &result, const char *symbol,
                       bool loadable) {
    internal_symbol_type type = internal_symbol_type::C;
    std::string demangled;

    if (!symbol || strcmp(symbol, "") == 0)
        return;

    demangled = demangle_symbol(symbol);
    if (demangled.empty())
        demangled = symbol;
    else
        type = internal_symbol_type::CPP;

    /*
     * In case of duplicate symbols, for example when loading a FAT binary,
     * avoid duplicates and override loadable if the previous symbol was not loadable.
     */
    for (auto &sym : result) {
        if (sym.name == symbol) {
            if (!sym.loadable)
                sym.loadable = loadable;
            return;
        }
    }

    result.push_back({symbol, demangled, type, loadable});
}

/************************   Windows   ************************/
#ifdef _WIN32

#include <windows.h>
#include <tchar.h>

std::vector<internal_symbol_info> get_symbols(HMODULE handle, int fd) {
    std::vector<internal_symbol_info> symbols_list;
    PIMAGE_EXPORT_DIRECTORY pExportDir;
    PIMAGE_DOS_HEADER pDosHeader;
    PIMAGE_NT_HEADERS pNTHeaders;
    DWORD exportDirRVA;
    DWORD *pNames;

    // Get the DOS header
    pDosHeader = (PIMAGE_DOS_HEADER)handle;
    if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        throw std::runtime_error("Invalid DOS header");

    // Get the NT headers
    pNTHeaders = (PIMAGE_NT_HEADERS)((BYTE *)handle + pDosHeader->e_lfanew);
    if (pNTHeaders->Signature != IMAGE_NT_SIGNATURE)
        throw std::runtime_error("Invalid NT headers");

    // Get the export directory
    exportDirRVA =
        pNTHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (exportDirRVA == 0)
        throw std::runtime_error("No export directory found");

    pExportDir = (PIMAGE_EXPORT_DIRECTORY)((BYTE *)handle + exportDirRVA);

    // Get the list of exported function names
    pNames = (DWORD *)((BYTE *)handle + pExportDir->AddressOfNames);

    for (DWORD i = 0; i < pExportDir->NumberOfNames; ++i) {
        const char *name = (const char *)((BYTE *)handle + pNames[i]);

        add_symbol(symbols_list, name, !!GetProcAddress(handle, name));
    }

    return symbols_list;
}

/************************   Mac OS   ************************/
#elif defined(__APPLE__)

#include <dlfcn.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <unistd.h>
#include <utility>

#if INTPTR_MAX == INT32_MAX
using mach_header_arch = mach_header;
using nlist_arch = nlist;
#define DYLIB_MH_MAGIC MH_MAGIC
#define DYLIB_MH_CIGAM MH_CIGAM
#elif INTPTR_MAX == INT64_MAX
using mach_header_arch = mach_header_64;
using nlist_arch = nlist_64;
#define DYLIB_MH_MAGIC MH_MAGIC_64
#define DYLIB_MH_CIGAM MH_CIGAM_64
#else
#error "Environment not 32 or 64-bit."
#endif

static void get_symbols_at_off(std::vector<internal_symbol_info> &symbols_list, void *handle,
                               int fd, off_t offset) {
    mach_header_arch mh;
    uint32_t ncmds;

    lseek(fd, offset, SEEK_SET);

    read(fd, &mh, sizeof(mh));

    ncmds = mh.ncmds;

    lseek(fd, offset + sizeof(mach_header_arch), SEEK_SET);

    for (uint32_t i = 0; i < ncmds; i++) {
        struct load_command lc;
        off_t cmd_offset;

        read(fd, &lc, sizeof(lc));

        cmd_offset = lseek(fd, 0, SEEK_CUR);

        if (lc.cmd == LC_SYMTAB) {
            std::vector<nlist_arch> symbols;
            std::vector<char> strtab;
            struct symtab_command symtab;

            lseek(fd, cmd_offset - sizeof(lc), SEEK_SET);
            read(fd, &symtab, sizeof(symtab));

            symbols.resize(symtab.nsyms);

            lseek(fd, offset + symtab.symoff, SEEK_SET);
            read(fd, symbols.data(), symtab.nsyms * sizeof(nlist_arch));

            strtab.resize(symtab.strsize);

            lseek(fd, offset + symtab.stroff, SEEK_SET);
            read(fd, strtab.data(), symtab.strsize);

            for (uint32_t j = 0; j < symtab.nsyms; j++) {
                uint32_t strx;
                char *name;

                strx = symbols[j].n_un.n_strx;
                name = &strtab[strx];

                if (name[0] == '_')
                    name++;

                add_symbol(symbols_list, name, !!dlsym(handle, name));
            }
        }

        lseek(fd, cmd_offset + lc.cmdsize - sizeof(lc), SEEK_SET);
    }
}

std::vector<internal_symbol_info> get_symbols(void *handle, int fd) {
    std::vector<internal_symbol_info> symbols_list;
    uint32_t magic;

    lseek(fd, 0, SEEK_SET);
    read(fd, &magic, sizeof(magic));
    lseek(fd, 0, SEEK_SET);

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        std::vector<struct fat_arch> fat_arches;
        struct fat_header fat_header;

        read(fd, &fat_header, sizeof(fat_header));

        fat_arches.resize(ntohl(fat_header.nfat_arch));

        read(fd, fat_arches.data(), sizeof(struct fat_arch) * ntohl(fat_header.nfat_arch));

        for (uint32_t i = 0; i < ntohl(fat_header.nfat_arch); i++) {
            off_t off = ntohl(fat_arches[i].offset);

            get_symbols_at_off(symbols_list, handle, fd, off);
        }
    } else if (magic == DYLIB_MH_MAGIC || magic == DYLIB_MH_CIGAM) {
        get_symbols_at_off(symbols_list, handle, fd, 0);
    } else {
        throw std::runtime_error("Unsupported file format");
    }

    return symbols_list;
}

#else /************************   Linux   ************************/

#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#if INTPTR_MAX == INT32_MAX
using ElfSym = Elf32_Sym;
using ElfDyn = Elf32_Dyn;
#define DYLIB_ELF_ST_TYPE ELF32_ST_TYPE
#elif INTPTR_MAX == INT64_MAX
using ElfSym = Elf64_Sym;
using ElfDyn = Elf64_Dyn;
#define DYLIB_ELF_ST_TYPE ELF64_ST_TYPE
#else
#error "Environment not 32 or 64-bit."
#endif

// Number of entries in the dynamic symbol table, from DT_HASH or DT_GNU_HASH.
// The table's size isn't stored anywhere else.
static size_t dynsym_count(const uint32_t *hash, const uint32_t *gnu_hash) {
    // DT_HASH: nbucket, nchain, ..., and nchain is the symbol count
    if (hash)
        return hash[1];
    if (!gnu_hash)
        return 0;

    // DT_GNU_HASH: nbuckets, symoffset, bloom_size, bloom_shift, then the
    // bloom filter (pointer-sized words), the buckets and the chains. The
    // count is one past the end of the chain of the highest bucket.
    const uint32_t nbuckets = gnu_hash[0];
    const uint32_t symoffset = gnu_hash[1];
    const uint32_t bloom_size = gnu_hash[2];
    const auto *buckets =
        reinterpret_cast<const uint32_t *>(reinterpret_cast<const uintptr_t *>(gnu_hash + 4) + bloom_size);
    const uint32_t *chains = buckets + nbuckets;

    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; ++i)
        last = std::max(last, buckets[i]);
    if (last < symoffset)
        return symoffset;
    // The last entry of a chain has its lowest bit set
    while (!(chains[last - symoffset] & 1))
        ++last;
    return last + 1;
}

std::vector<internal_symbol_info> get_symbols(void *handle, int fd) {
    std::vector<internal_symbol_info> symbols_list;
    struct link_map *map = nullptr;
    ElfSym *symtab = nullptr;
    char *strtab = nullptr;
    unsigned long symentries = 0;
    unsigned long strsize = 0;
    const uint32_t *hash = nullptr;
    const uint32_t *gnu_hash = nullptr;

    if (dlinfo(handle, RTLD_DI_LINKMAP, static_cast<void *>(&map)) != 0) {
        const char *error = dlerror();
        throw std::runtime_error("dlinfo failed: " +
                                 std::string(error ? error : "Unknown error (dlerror failed)"));
    }

    // glibc relocates the dynamic section's pointers in place, while the
    // BSDs' rtld and musl leave them relative to the load address
    const uintptr_t base = (uintptr_t)map->l_addr;
    const auto to_address = [base](uintptr_t ptr) { return ptr < base ? base + ptr : ptr; };

    // l_ld is ElfW(Dyn) * in glibc but const void * on the BSDs
    for (auto *section = static_cast<const ElfDyn *>(map->l_ld); section->d_tag != DT_NULL; ++section) {
        if (section->d_tag == DT_SYMTAB)
            symtab = (ElfSym *)to_address(section->d_un.d_ptr);
        else if (section->d_tag == DT_STRTAB)
            strtab = (char *)to_address(section->d_un.d_ptr);
        else if (section->d_tag == DT_SYMENT)
            symentries = section->d_un.d_val;
        else if (section->d_tag == DT_STRSZ)
            strsize = section->d_un.d_val;
        else if (section->d_tag == DT_HASH)
            hash = (const uint32_t *)to_address(section->d_un.d_ptr);
        else if (section->d_tag == DT_GNU_HASH)
            gnu_hash = (const uint32_t *)to_address(section->d_un.d_ptr);
    }

    if (!symtab || !strtab || symentries == 0)
        return symbols_list;

    size_t count = dynsym_count(hash, gnu_hash);
    // Without a hash table, assume .dynstr directly follows .dynsym, as GNU
    // ld lays them out (lld doesn't, but always emits a hash table)
    if (count == 0 && strtab > (char *)symtab)
        count = (strtab - (char *)symtab) / symentries;

    for (size_t i = 0; i < count; ++i) {
        const ElfSym *sym = &symtab[i];

        if (strsize != 0 && sym->st_name >= strsize)
            continue;

        if (DYLIB_ELF_ST_TYPE(sym->st_info) == STT_FUNC) {
            const char *name = &strtab[sym->st_name];

            add_symbol(symbols_list, name, !!dlsym(handle, name));
        }
    }

    return symbols_list;
}

#endif
