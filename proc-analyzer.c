/*
 * Colin Powell
 * 18/2/15
 * Process Analyzer: main 
 */

#include "helpers.h"

#define _GNU_SOURCE             //enable process_vm_readv
#define _LARGEFILE64_SOURCE     //enable 64-bit seeking for pagemap

#include <pthread.h>
#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/uio.h>

#define TRUE 1
#define FALSE 0

#define MAX_NAME 1024
#define MAX_MAPS 2048
#define MAX_PATH 1024

#define LINELEN 32 

typedef struct {
    char name[MAX_NAME];
    char cmdline[MAX_NAME];
    pid_t pid;
} p_info;


typedef struct {
    char name[MAX_NAME]; 
    pthread_t tgid;
    pthread_t tid;
} t_info;

typedef struct {
    // start, end, perms, name
    unsigned long vm_start;
    unsigned long vm_end;
    char perms[8];
    char path[MAX_NAME];
} map_reg;


/* USER INPUT */

void print_main_menu() ;
char get_user_choice(char *prompt, char *choices);
int get_numeric_input();
unsigned long long get_hex_input();

/* PROCESS FUNCTIONS */

int read_proc_info( char *path, p_info *pi);
int read_thread_info(char *path, t_info *ti);
int read_mapped_regions(int pid, map_reg *map);
int read_memory(int pid, unsigned long long start_addr);

int is_shared_obj(char *path);
int is_executable(map_reg *map);
int read_bit(unsigned long long tgt, int bnum);

int list_processes();
int list_threads(int pid);
void list_shared_objects(int pid);
int list_executable_pages(int pid);

void print_pageinfo(unsigned long addr, unsigned long long pmap_entry, char *path); 

/*
 * main(): driver for process analyzer functions
 */

int main(int argc, char **argv) {

    char user_choice;
    char choices[] = "12345q";
    int quit = FALSE;
    int pid = 0;
    unsigned long long start_addr;

    printf("*****WELCOME TO PROCESS ANALYZER*****\n");

    while(!quit) {

        print_main_menu();
        user_choice = get_user_choice("Enter a menu option", choices);

        switch (user_choice){

            case '1':   list_processes();
                        break;

            case '2':   pid = get_numeric_input(); 
                        list_threads(pid);
                        break;

            case '3':   pid = get_numeric_input();
                        list_shared_objects(pid);
                        break;

            case '4':   pid = get_numeric_input();
                        list_executable_pages(pid);
                        break;

            case '5':   pid = get_numeric_input();
                        start_addr = get_hex_input();
                        read_memory(pid, start_addr);
                        break;

            default:    quit = TRUE; 
        }

    }

    return 0;

}

/*
 * list_processes(): enumerates all processes.
 * -- returns TRUE if no read errors occured, FALSE otherwise.
 */

int list_processes() {

    int success = FALSE;
    p_info pi;

    struct dirent *entry = NULL;
    struct stat dir_stat;

    char temp[MAX_PATH];
    const char name[] = "/proc";
    
    DIR *dptr = opendir(name);
    if(dptr == NULL) {
        fprintf(stderr, "ERROR: could not open %s directory", name);
        return FALSE;
    }

    printf("\n----PROCESSES----\n");
    printf("%-8s%-32s%-64s\n", "PID", "NAME", "CMDLINE");
    
    while((entry = readdir(dptr)) != NULL) {

        // stat file
        stat(entry->d_name, &dir_stat);

        if(is_number(entry->d_name) && S_ISDIR(dir_stat.st_mode)){

            snprintf(temp, sizeof(temp), "%s/%s", name, entry->d_name);

            pi.pid = atoi(entry->d_name);
            success = read_proc_info(temp, &pi);
            printf("%-8d%-32s%-64s\n", pi.pid, pi.name, pi.cmdline);
        } 
    } 

    closedir(dptr);
    dptr = NULL;

    return success;

}

/*
 * list_threads(): output information about all threads associated with a particular pid
 */

int list_threads(int pid) {

    t_info ti;

    int success = FALSE;
    
    struct dirent *entry = NULL;
    struct stat dir_stat;

    char path[MAX_PATH] = {'\0'};
    sprintf(path, "/proc/%d/task", pid);

    char tmp[MAX_PATH] = {'\0'};

    DIR *dptr = opendir(path);
    if(dptr == NULL) {
        fprintf(stderr, "ERROR: could not open %s. Did you enter a valid PID?\n", path);
        return FALSE;
    }

    printf("\n----THREADS FOR PID: %d----\n", pid);
    printf("%-8s%-8s%-64s\n", "TID", "TGID", "NAME");
    while((entry = readdir(dptr)) != NULL) {
        stat(entry->d_name, &dir_stat);

        if(is_number(entry->d_name) && S_ISDIR(dir_stat.st_mode)) {
            snprintf(tmp, sizeof(tmp), "%s/%s", path, entry->d_name);

            ti.tid = atoi(entry->d_name);
            success = read_thread_info(tmp, &ti);
            printf("%-8lu%-8lu%-64s\n", ti.tid, ti.tgid, ti.name);
        }
    }

    return success;
}

/*
 * read_mapped_regions(): retrive information on all mapped virtual memory regions
 * in the process passed as an argument. 
 * Returns number of mapped regions read from /proc/pid/maps
 */

int read_mapped_regions(int pid, map_reg *map) {
   
    char path[MAX_PATH] = {'\0'};
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    int i = 0;  // map array counter 

    FILE *fp = fopen(path, "r"); 
    if(fp == NULL) {
        return -1;
    }
    while( (read = (getline(&line, &len, fp)) != -1)) {
        // process map entry
        // FORMAT: address perms offset dev inode pathname --> ignore offset dev inode
        sscanf(line, "%lX-%lX %s %*s %*s %*d %s", &map[i].vm_start, &map[i].vm_end, map[i].perms, map[i].path);
        i++;
    }
    free(line);
    fclose(fp);
    return i;

}

/*
 * list_shared_objects(): outputs a list of shared objects 
 */

void list_shared_objects(int pid) {

    map_reg maps[MAX_MAPS]; // list of all mapped regions
    memset(maps, '\0', sizeof(maps));

    // grab all mapped regions (will filter below)
    int n_maps = read_mapped_regions(pid, maps);

    char sh_objs[n_maps][MAX_PATH];
    memset(sh_objs, '\0', sizeof(sh_objs));

    int k;
    int j = 0;
    int n_shared = 0;   // num of shared objects

    // extract unique .so files from mapped regions
    for(k = 0; k < n_maps; k++) {
        j = 0;
        // if this path is not already in sh_objs, stop iterating
        while(j < n_shared) {
            if(strcmp(maps[k].path, sh_objs[j]) == 0) {
                break;
            }
            j++;
        }

        // if no duplicate and a .so file, place in sh_objs directory
        if(j >= n_shared && is_shared_obj(maps[k].path) ){ 
            strcpy(sh_objs[j], maps[k].path);
            n_shared++;
        }
    }

    printf("\n----SHARED OBJECTS----\n");
    for(k = 0; k < n_shared; k++) {
        printf("%s\n", sh_objs[k]);
    }

}

/*
 * is_shared_obj(): returns TRUE if string passed as an argument matches shared object file naming 
 * convention, and FALSE otherwise.
 * CONVENTION: starts with "lib", contains ".so" file extension, possibly followed by version number.
 */

int is_shared_obj(char *path) {

    int is_so = FALSE;
    
    // find filename in path (by finding last / char)
    if(strstr(path, ".so") != 0) {
        is_so = TRUE;
    }

    return is_so;
}

/*
 * list_executable_pages(): list information for all executable pages in a process
 * NOTE: useof 64-bit types and logic for determining offset adapted from:
 * eqware.net/Articles/CapturingProcessMemoryUsageUnderLinux/page-collect.c 
 */

int list_executable_pages(int pid) {

    int i = 0;  
//    int j = 0;  // counters

    map_reg maps[MAX_MAPS];
    memset(maps, '\0', sizeof(maps));

    char path[MAX_PATH] = {'\0'};
    snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
    
    unsigned long long pmap_entry;
    unsigned long addr;
    int num_pages;
    off64_t offset;
    ssize_t sz;

    // get index of pagemap entry from pos. in virtual memory
    // will be located at 64bits * num of pages to get to vm_start
    long idx;
  
    int n_maps = read_mapped_regions(pid, maps);

    FILE *fp = fopen(path, "rb");
    if(fp == NULL || n_maps <= 0) {
        fprintf(stderr, "ERROR: unable to read information for pid %d. Did you enter a valid PID?\n", pid);    
        return FALSE;
    } 
    
    int fnum = fileno(fp);
            
    printf("\n%-24s%-16s%-16s%-64s\n", "VIRT. ADDRESS", "PAGEFRAME NO.", "EXCL. MAPPED", "PATH");

    for(i = 0; i < n_maps; i++) {
   
        // if mapped region is executable, read page entries for that mapped region 
        if(is_executable(&maps[i])) {

            // number of pages is (end addr - start addr) / size of a page
            num_pages = (maps[i].vm_end - maps[i].vm_start) / sysconf(_SC_PAGE_SIZE);
            addr = maps[i].vm_start;

            // starting index into pagemap
            idx = maps[i].vm_start / sysconf(_SC_PAGE_SIZE) * sizeof(unsigned long long);
            while( num_pages > 0) {
           
                offset = lseek64(fnum, idx, SEEK_SET);

                sz = read(fnum, &pmap_entry, sizeof(pmap_entry));
                if( sz < 0 ) {
                    fprintf(stderr, "ERROR: could not read pagemap entry at offset %llu", (unsigned long long)offset);
                }
    
                // if present (bit 63 set)
               if(read_bit(pmap_entry, 63)) {
                    print_pageinfo(addr, pmap_entry, maps[i].path);
               }
            
                // add 64 bits to index
                idx += sizeof(unsigned long long);
    
                // add PAGE_SIZE to base address of mapped region
                addr += sysconf(_SC_PAGE_SIZE); 
                num_pages--;        // and decrease num. of pages left
            }
        }
    }    

    fclose(fp);
    return TRUE;
}

/*
 * is_executable(): returns TRUE if mapped region is executable, FALSE if not.
 */

int is_executable(map_reg *map) {

    if(strchr(map->perms, 'x') != NULL) return TRUE;
    return FALSE;

}

/*
 * read_memory(): reads through process memory, one page a time, starting at specified address
 * NOTE: requires glibc version 2.15 or greater, Linux kernel version 3.2 or greater.
 */

int read_memory(int pid, unsigned long long start_addr) {

    ssize_t nread;
    int i;

    int keep_reading = TRUE;

    int buflen = sysconf(_SC_PAGE_SIZE);
    unsigned char buf[buflen];
    memset(buf, '\0', buflen);

    struct iovec local;
    struct iovec remote;

    local.iov_base = buf;
    local.iov_len = buflen;

    remote.iov_base = (void *) start_addr;
    remote.iov_len = buflen;
    
    unsigned long long line_addr = start_addr;

    // do initial read
    if ((nread = process_vm_readv(pid, &local, 1, &remote, 1, 0)) < 0) {
        perror("Could not read process memory");
        memset(buf, '\0', buflen);
        return FALSE;
    }

    while(nread >= 0 && keep_reading) {

        // print a chunk of memory
        printf("%llX:    ", line_addr);
        for(i = 0; i < (int)nread; i++) {
            if((i > 0) && (i % (LINELEN) == 0)) {
                printf("\n");
                line_addr += LINELEN; // add line-length bytes to line address
                printf("%llX:    ", line_addr);
            }
            printf("%02X ", (unsigned int)buf[i]);
        }
        printf("\n");

        // update starting address for next read
        start_addr += nread;
        line_addr = start_addr;

        remote.iov_base = (void *) start_addr;

        // try to read next section
        nread = process_vm_readv(pid, &local, 1, &remote, 1, 0);

        // if there was memory to read...
        if(nread > 0) {
            if( get_user_choice("Continue reading (y/n)?", "yn") == 'n') {
               keep_reading = FALSE;
            }
        }
        else if(errno == EFAULT) {
            printf("\nReached end of accessible address space\n");
        }
        else {
            perror("Could not read process memory:");
            return FALSE;
        }
    }

    return TRUE;
    
}

/*
 * print_pageinfo(): prints content of a specific pagemap entry
 */

void print_pageinfo(unsigned long addr, unsigned long long pmap_entry, char *path) {

    // mask to show bits 0-54, which contain pagemap number 
    unsigned long long pfn = pmap_entry & 0x7FFFFFFFFFFFFF;
    
    // print 16 hex chars, as long-long (64-bit) entry
    printf("%-24lX", addr);                     // virtual address
    printf("%-16llu", pfn);                     // phys pageframe number
    printf("%-16d", read_bit(pmap_entry, 56));  // exclusively mapped
    printf("%-64s", path);                      // shared lib / path to executable
    printf("\n");

}

/*
 * read_proc_info(): gets information about the process at the specified path, returns in a struct p_info.
 * NOTE: file reading logic adapted from getline() man pages, linux.die.net/man/3/getline
 */

int read_proc_info(char *path, p_info *pi) {

    char info_path[MAX_PATH] = {'\0'};
    strcpy(info_path, path);
    strcat(info_path, "/cmdline");

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    // read command line info

    FILE *fp = fopen(info_path, "r");
    if(fp == NULL) {
        fprintf(stderr, "ERROR: could not open file %s\n", info_path);
        return FALSE;
    }
  
    // read command 
    while( (read = getline(&line, &len, fp)) != -1) {
        strcpy(pi->cmdline, line);
    }
    fclose(fp);

    strcpy(info_path, path);
    strcat(info_path, "/status");

    // open /proc/pid/status status for reading
    fp = fopen(info_path, "r");

    if(fp == NULL) {
        fprintf(stderr, "ERROR: could not open file %s\n", info_path);
        return FALSE;
    } 

    while( (read = getline(&line, &len, fp)) != -1) {

        // if we have matched name field
        if(strncmp("Name:", line, strlen("Name:")) == 0) {
            strcpy(pi->name, get_field_val(line)); 
            break;
        }
    }
    
    fclose(fp);

    // free read line per getline() specs
    free(line);
    return TRUE;

}

/*
 * read_thread_info(): reads /proc/[pid]/task/[tid], returns information about 
 * a process thread in a t_info struct
 * NOTE: file reading logic adapted from getline() man pages, linux.die.net/man/3/getline
 */

int read_thread_info(char *path, t_info *ti) {

    char info_path[MAX_PATH] = {'\0'};
    strcpy(info_path, path);
    strcat(info_path, "/status");

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    FILE *fp = fopen(info_path, "r");
    if(fp == NULL) {
        fprintf(stderr, "ERROR: could not open file %s\n", info_path);
        return FALSE;
    }

    while( (read = getline(&line, &len, fp)) != -1) {
    
        // if we have matched name field
        if(strncmp("Name:", line, strlen("Name:")) == 0) {
            strcpy(ti->name, get_field_val(line));
        }

        // if we have matched thread group id field
        if(strncmp("Tgid:", line, strlen("Tgid:")) == 0) {
            ti->tgid =  atoi(get_field_val(line));
        }
    }

    fclose(fp);
    free(line);

    return TRUE;

}


/*
 * print_main_menu(): prints the program's main menu, incl. a list of user choices
 */

void print_main_menu() {

    printf("\nMAIN MENU:\n");
    printf("[1]: Enumerate all processes\n");
    printf("[2]: Enumerate all threads for a process\n");
    printf("[3]: Enumerate all shared objects (libraries) loaded for a process\n");
    printf("[4]: Show information for all executable pages of a process\n");
    printf("[5]: Read through memory of a process\n");
    printf("\nTo QUIT, enter \'q\'\n\n");
}

/*
 * get_user_choice(): gets a character from user, returns it. 
 * -- parameters: choices, a string containing all valid character choices
 * NOTE: getline() usage follows Linux man pages, man7.org/linux/man-pages/man-3/getline.3.hmtl
 */

char get_user_choice(char *prompt, char *choices) {

    char user_choice;

    char *input = NULL;
    size_t len = 0;
    ssize_t nread; 
    int good_input = FALSE;

    while(!good_input) {
        printf("%s: ", prompt);
        nread = getline(&input, &len, stdin);

        // if char + newline was read, and char matches valid choice
        if(nread == 2 && strchr(choices, *input )) {
            good_input = TRUE;
        }
        else {
            printf("Invalid choice. Try again.\n");
        }
    } 

    user_choice = *input;
    free(input);

    return user_choice; 

}


/*
 * get_numeric_input(): gets an integral number from the user, validates.
 */

int get_numeric_input() {

    int num;
    long cand;
    char *input = NULL;
    size_t len = 0;
    int good_input = FALSE;
    int nread = 0;

    while(!good_input) {
        printf("Enter a PID: ");
        nread = getline(&input, &len, stdin);

        // verify that user entered more than '\n', and input is a pos. number within INT_MAX
        if(nread > 1 && is_number(trim(input))){
            
            errno = 0;  // reset and previous ERANGE errors

            // convert to long int, compare with int limits:
            cand = strtol(input, NULL, 10);

            if(cand > INT_MAX || errno == ERANGE){
                printf("PID out of range. Try again.\n");
            }
            else {
                good_input = TRUE;
            }

        } 
        else {
            printf("PID must be a number. Try again.\n");
        }
    }

    num = (int) cand;   // don't necessarily need to cast, but good to be explicit

    free(input);
    return num;
}

/*
 * get_hex_input(): gets a hexadecimal value from the user
 */

unsigned long long get_hex_input() {

    unsigned long long hex;
    char *input = NULL;
    size_t len = 0;
    int good_input = FALSE;
    int nread = 0;

    while(!good_input) {

        printf("Enter a hexadecimal address (ex: 123ABC): ");
        nread = getline(&input, &len, stdin);

        // if user entered more than '\n', and input is a number
        if(nread > 1 && is_hex(trim(input))){
            
            errno = 0;  // reset and previous ERANGE errors
            hex = strtoull(input, NULL, 16);    //convert to unsigned long long

            // if conversion fails (too big), inform user
            if(hex == ULLONG_MAX && errno == ERANGE) {
                printf("Address is out of range. Try again.\n");
            }
            else {
                good_input = TRUE;
            }

        } 
        else {
            printf("Address must be non-negative hexadecimal number. Try again.\n");
        }

    }

    free(input);
    return hex;

}


