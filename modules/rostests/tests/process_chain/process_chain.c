/* Dependency-free process/file/map stress mimicking the cygwin gcc/ld pipeline. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PROCESS_CHAIN_SMALL_OBJECT
#define DATA_SIZE 1024
#define STRING_TABLE_OFFSET 512
#define STRING_TABLE_SIZE 512
#else
#define DATA_SIZE (64 * 1024)
#define STRING_TABLE_OFFSET 4096
#define STRING_TABLE_SIZE 4096
#endif
#define ITERATIONS 12
#define READERS 4
#define MAP_PASSES 16
#define PRIVATE_CHURN_SIZE (8 * 1024 * 1024)

static LONG image_nonce;

static unsigned char byte_at(DWORD seed, DWORD offset)
{
    if (offset >= STRING_TABLE_OFFSET && offset < STRING_TABLE_OFFSET + 4)
        return (unsigned char)(STRING_TABLE_SIZE >> (8 * (offset - STRING_TABLE_OFFSET)));
    return (unsigned char)((offset * 37u + seed * 101u) >> 3);
}

static void fill(unsigned char *p, DWORD seed)
{
    DWORD i;
    for (i = 0; i < DATA_SIZE; ++i)
        p[i] = byte_at(seed, i);
}

/* The object has the same COFF areas that ld traverses: file/section headers,
 * raw section data, a symbol table, and a post-symbol string table. */
static void make_coff(unsigned char *p)
{
    IMAGE_FILE_HEADER *file = (IMAGE_FILE_HEADER *)p;
    IMAGE_SECTION_HEADER *section = (IMAGE_SECTION_HEADER *)(p + sizeof(*file));
    IMAGE_SYMBOL *symbols = (IMAGE_SYMBOL *)(p + STRING_TABLE_OFFSET - 2 * IMAGE_SIZEOF_SYMBOL);
    DWORD *string_size = (DWORD *)(p + STRING_TABLE_OFFSET);

    ZeroMemory(file, sizeof(*file));
    file->Machine = IMAGE_FILE_MACHINE_AMD64;
    file->NumberOfSections = 1;
    file->PointerToSymbolTable = STRING_TABLE_OFFSET - 2 * IMAGE_SIZEOF_SYMBOL;
    file->NumberOfSymbols = 2;
    ZeroMemory(section, sizeof(*section));
    memcpy(section->Name, ".text", 5);
    section->SizeOfRawData = 32;
    section->PointerToRawData = 0x80;
    section->Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE |
                               IMAGE_SCN_MEM_READ;
    ZeroMemory(symbols, 2 * IMAGE_SIZEOF_SYMBOL);
    memcpy(symbols[0].N.ShortName, "main", 4);
    symbols[0].SectionNumber = 1;
    symbols[0].Type = IMAGE_SYM_DTYPE_FUNCTION;
    symbols[0].StorageClass = IMAGE_SYM_CLASS_EXTERNAL;
    memcpy(symbols[1].N.ShortName, "__main", 6);
    symbols[1].StorageClass = IMAGE_SYM_CLASS_EXTERNAL;
    *string_size = STRING_TABLE_SIZE;
}

static int make_expected(unsigned char **expected, DWORD seed)
{
    *expected = HeapAlloc(GetProcessHeap(), 0, DATA_SIZE);
    if (!*expected) return 0;
    fill(*expected, seed);
    make_coff(*expected);
    return 1;
}

static int private_churn(DWORD seed)
{
    volatile unsigned char *memory;
    SIZE_T offset;

    memory = HeapAlloc(GetProcessHeap(), 0, PRIVATE_CHURN_SIZE);
    if (!memory) return 1;
    for (offset = 0; offset < PRIVATE_CHURN_SIZE; offset += 4096)
        memory[offset] = (unsigned char)(seed ^ offset);
    for (offset = 0; offset < PRIVATE_CHURN_SIZE; offset += 4096)
        if (memory[offset] != (unsigned char)(seed ^ offset))
        {
            HeapFree(GetProcessHeap(), 0, (void *)memory);
            return 2;
        }
    HeapFree(GetProcessHeap(), 0, (void *)memory);
    return 0;
}

static int writer(const char *path, DWORD seed)
{
    FILE *file;
    DWORD written;
    unsigned char *data = HeapAlloc(GetProcessHeap(), 0, DATA_SIZE);
    if (!data || private_churn(seed)) return 2;
    fill(data, seed);
    make_coff(data);
    file = fopen(path, "wb");
    if (!file)
    {
        HeapFree(GetProcessHeap(), 0, data);
        return 3;
    }
    written = (DWORD)fwrite(data, 1, DATA_SIZE, file);
    if (written != DATA_SIZE || fclose(file) != 0)
    {
        HeapFree(GetProcessHeap(), 0, data);
        return 4;
    }
    HeapFree(GetProcessHeap(), 0, data);
    return 0;
}

static int mapped_reader(const char *path, DWORD seed)
{
    DWORD pass;
    unsigned char *expected;

    if (!make_expected(&expected, seed))
        return 4;

    for (pass = 0; pass < MAP_PASSES; ++pass)
    {
        HANDLE file, mapping;
        unsigned char *view;
        DWORD offset;

        file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            HeapFree(GetProcessHeap(), 0, expected);
            return 5;
        }
        mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!mapping)
        {
            CloseHandle(file);
            HeapFree(GetProcessHeap(), 0, expected);
            return 6;
        }
        view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, DATA_SIZE);
        if (!view)
        {
            CloseHandle(mapping);
            CloseHandle(file);
            HeapFree(GetProcessHeap(), 0, expected);
            return 7;
        }
        offset = memcmp(view, expected, DATA_SIZE);
        if (offset)
        {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            CloseHandle(file);
            HeapFree(GetProcessHeap(), 0, expected);
            return 8;
        }
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        CloseHandle(file);
    }
    HeapFree(GetProcessHeap(), 0, expected);
    return 0;
}

static int reader(const char *path, DWORD seed)
{
    FILE *file;
    unsigned char *data;
    DWORD got;
    DWORD offset = 0;
    DWORD string_size;
    unsigned char *strings;
    {
        int status = mapped_reader(path, seed);
        if (status)
            return 20 + status;
    }
    file = fopen(path, "rb");
    if (!file) return 5;
    if (private_churn(seed))
    {
        fclose(file);
        return 6;
    }
    {
        IMAGE_FILE_HEADER coff;
        IMAGE_SECTION_HEADER section;
        IMAGE_SYMBOL symbols[2];
        if (fread(&coff, 1, sizeof(coff), file) != sizeof(coff) ||
            coff.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            coff.NumberOfSections != 1 || coff.NumberOfSymbols != 2 ||
            fread(&section, 1, sizeof(section), file) != sizeof(section) ||
            memcmp(section.Name, ".text", 5) || section.PointerToRawData != 0x80 ||
            fseek(file, coff.PointerToSymbolTable, SEEK_SET) != 0 ||
            fread(symbols, 1, sizeof(symbols), file) != sizeof(symbols) ||
            memcmp(symbols[0].N.ShortName, "main", 4))
        {
            fclose(file);
            return 6;
        }
    }
    if (fseek(file, STRING_TABLE_OFFSET, SEEK_SET) != 0 ||
        fread(&string_size, 1, sizeof(string_size), file) != sizeof(string_size) ||
        string_size != STRING_TABLE_SIZE)
    {
        fclose(file);
        return 6;
    }
    strings = malloc(string_size + 1);
    if (!strings)
    {
        fclose(file);
        return 7;
    }
    memset(strings, 0, 4);
    if (fread(strings + 4, 1, string_size - 4, file) != string_size - 4)
    {
        free(strings);
        fclose(file);
        return 8;
    }
    strings[string_size] = 0;
    free(strings);
    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 9;
    }
    data = HeapAlloc(GetProcessHeap(), 0, 64 * 1024);
    if (!data) { fclose(file); return 10; }
    while (offset != DATA_SIZE)
    {
        DWORD want = DATA_SIZE - offset;
        if (want > 64 * 1024) want = 64 * 1024;
        got = (DWORD)fread(data, 1, want, file);
        if (got != want)
        {
            HeapFree(GetProcessHeap(), 0, data);
            fclose(file);
            return 11;
        }
        offset += got;
    }
    {
        unsigned char *expected = NULL;
        if (!make_expected(&expected, seed) || memcmp(data, expected, DATA_SIZE))
        {
            if (expected) HeapFree(GetProcessHeap(), 0, expected);
            HeapFree(GetProcessHeap(), 0, data);
            fclose(file);
            return 12;
        }
        HeapFree(GetProcessHeap(), 0, expected);
    }
    HeapFree(GetProcessHeap(), 0, data);
    fclose(file);
    return 0;
}

static int start(const char *image, const char *mode, const char *path, DWORD seed,
                 HANDLE stdin_handle, HANDLE stdout_handle, PROCESS_INFORMATION *pi)
{
    char command[MAX_PATH * 2];
    STARTUPINFOA si = { sizeof(si) };
    if (path)
        sprintf(command, "\"%s\" %s \"%s\" %lu", image, mode, path, seed);
    else
        sprintf(command, "\"%s\" %s %lu", image, mode, seed);
    if (stdin_handle || stdout_handle)
    {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdin_handle ? stdin_handle : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = stdout_handle ? stdout_handle : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    if (!CreateProcessA(image, command, NULL, NULL, stdin_handle || stdout_handle,
                        0, NULL, NULL, &si, pi))
        return 11;
    return 0;
}

static int wait_process(PROCESS_INFORMATION *pi)
{
    DWORD code;
    if (WaitForSingleObject(pi->hProcess, 30000) != WAIT_OBJECT_0)
    {
        TerminateProcess(pi->hProcess, 12);
        CloseHandle(pi->hThread);
        CloseHandle(pi->hProcess);
        return 12;
    }
    GetExitCodeProcess(pi->hProcess, &code);
    CloseHandle(pi->hThread);
    CloseHandle(pi->hProcess);
    return (int)code;
}

/*
 * Model the actual gcc handoff more closely than a parent launching a set of
 * workers from one already-loaded image.  Each role has a freshly-created PE
 * image section, and the gcc role owns cc1 -> as; a distinct collect2 image
 * then owns the handoff to the ld readers.
 */
static int collect2(const char *ld, const char *path, DWORD seed)
{
    DWORD reader;
    PROCESS_INFORMATION reader_pi[READERS];
    int status;

    for (reader = 0; reader < READERS; ++reader)
    {
        if (start(ld, "reader", path, seed, NULL, NULL, &reader_pi[reader]))
        {
            DWORD previous;
            for (previous = 0; previous < reader; ++previous)
                wait_process(&reader_pi[previous]);
            return 12;
        }
    }
    for (reader = 0; reader < READERS; ++reader)
    {
        status = wait_process(&reader_pi[reader]);
        if (status)
            return 100 + status;
    }

    return 0;
}

static int chain(const char *cc1, const char *as, const char *collect2_image,
                 const char *ld, const char *path, DWORD seed)
{
    PROCESS_INFORMATION pi;
    int status;

    status = start(cc1, "churn", NULL, seed, NULL, NULL, &pi);
    if (!status) status = wait_process(&pi);
    if (status) return status;

    status = start(as, "writer", path, seed, NULL, NULL, &pi);
    if (!status) status = wait_process(&pi);
    if (status) return status;

    /* collect2 needs the fresh object path as well as the linker image. */
    {
        char command[MAX_PATH * 3];
        STARTUPINFOA si = { sizeof(si) };
        sprintf(command, "\"%s\" collect2 \"%s\" \"%s\" %lu",
                collect2_image, ld, path, seed);
        if (!CreateProcessA(collect2_image, command, NULL, NULL, FALSE, 0,
                            NULL, NULL, &si, &pi))
            return 12;
    }
    return wait_process(&pi);
}

static int start_chain(const char *gcc, const char *cc1, const char *as,
                       const char *collect2_image, const char *ld,
                       const char *path, DWORD seed,
                       PROCESS_INFORMATION *pi)
{
    char command[MAX_PATH * 6];
    STARTUPINFOA si = { sizeof(si) };

    sprintf(command, "\"%s\" chain \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" %lu",
            gcc, cc1, as, collect2_image, ld, path, seed);
    if (!CreateProcessA(gcc, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi))
        return 11;
    return 0;
}

static int make_role_image(const char *self, const char *temp, const char *role,
                           DWORD iteration, char *path)
{
    LONG nonce = InterlockedIncrement(&image_nonce);
    sprintf(path, "%spchain_%08lx_%08lx_%08lx_%08lx_%s.exe", temp,
            GetCurrentProcessId(), GetTickCount(), iteration, nonce, role);
    return CopyFileA(self, path, FALSE) ? 0 : 1;
}

int main(int argc, char **argv)
{
    char self[MAX_PATH], image_dir[MAX_PATH], temp[MAX_PATH], path[MAX_PATH];
    char gcc[MAX_PATH], cc1[MAX_PATH], as[MAX_PATH], collect2_image[MAX_PATH], ld[MAX_PATH];
    DWORD i, iterations = ITERATIONS;
    if (argc == 4 && !strcmp(argv[1], "writer")) return writer(argv[2], strtoul(argv[3], NULL, 0));
    if (argc == 4 && !strcmp(argv[1], "reader")) return reader(argv[2], strtoul(argv[3], NULL, 0));
    if (argc == 3 && !strcmp(argv[1], "churn")) return private_churn(strtoul(argv[2], NULL, 0));
    if (argc == 5 && !strcmp(argv[1], "collect2"))
        return collect2(argv[2], argv[3], strtoul(argv[4], NULL, 0));
    if (argc == 8 && !strcmp(argv[1], "chain"))
        return chain(argv[2], argv[3], argv[4], argv[5], argv[6], strtoul(argv[7], NULL, 0));
    if (argc == 3 && !strcmp(argv[1], "stress"))
    {
        iterations = strtoul(argv[2], NULL, 0);
        if (!iterations)
            return 11;
    }
    if (!GetModuleFileNameA(NULL, self, sizeof(self)) || !GetTempPathA(sizeof(temp), temp)) return 11;
    strcpy(image_dir, self);
    {
        char *separator = strrchr(image_dir, '\\');
        if (!separator) return 11;
        separator[1] = 0;
    }
    for (i = 1; i <= iterations; ++i)
    {
        HANDLE reserved;
        DWORD attempt;
        PROCESS_INFORMATION writer_pi;
        DWORD writer;
        reserved = INVALID_HANDLE_VALUE;
        for (attempt = 0; attempt != 64; ++attempt)
        {
            sprintf(path, "%scc%08lx.o", temp,
                    GetTickCount() ^ GetCurrentProcessId() ^ (i << 16) ^ attempt);
            reserved = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (reserved != INVALID_HANDLE_VALUE)
                break;
        }
        if (reserved == INVALID_HANDLE_VALUE)
        {
            if (reserved != INVALID_HANDLE_VALUE) CloseHandle(reserved);
            printf("PROCESS_CHAIN: temp setup failed at iteration %lu\n", i);
            DeleteFileA(path);
            return 1;
        }
        CloseHandle(reserved);
        if (make_role_image(self, image_dir, "gcc", i, gcc))
        {
            printf("PROCESS_CHAIN: gcc image setup failed at iteration %lu (%lu)\n", i, GetLastError());
            DeleteFileA(path);
            return 1;
        }
        if (make_role_image(self, image_dir, "cc1", i, cc1) ||
            make_role_image(self, image_dir, "as", i, as) ||
            make_role_image(self, image_dir, "collect2", i, collect2_image) ||
            make_role_image(self, image_dir, "ld", i, ld))
        {
            printf("PROCESS_CHAIN: child image setup failed at iteration %lu (%lu)\n", i, GetLastError());
            DeleteFileA(path);
            DeleteFileA(gcc);
            DeleteFileA(cc1);
            DeleteFileA(as);
            DeleteFileA(collect2_image);
            DeleteFileA(ld);
            return 1;
        }
        writer = start_chain(gcc, cc1, as, collect2_image, ld, path, i, &writer_pi);
        if (!writer) writer = wait_process(&writer_pi);
        if (writer)
        {
            printf("PROCESS_CHAIN: chain failed at iteration %lu (%lu)\n", i, writer);
            DeleteFileA(path);
            DeleteFileA(gcc);
            DeleteFileA(cc1);
            DeleteFileA(as);
            DeleteFileA(ld);
            return 1;
        }
        DeleteFileA(path);
        DeleteFileA(gcc);
        DeleteFileA(cc1);
        DeleteFileA(as);
        DeleteFileA(collect2_image);
        DeleteFileA(ld);
    }
    printf("PROCESS_CHAIN: no mismatch in %lu iterations\n", iterations);
    return 0;
}
