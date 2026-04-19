/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║              MPOS v6.1 — Advanced Kernel                    ║
 * ║                                                              ║
 * ║  Features:                                                   ║
 * ║  • Bump Memory Allocator (malloc/free)                       ║
 * ║  • Process Table (ps, kill, background tasks)                ║
 * ║  • Shell Command History (↑ up-arrow recall)                 ║
 * ║  • Pipeline Operator  cmd1 | cmd2                            ║
 * ║  • Environment Variables  set VAR=val  / $VAR expansion      ║
 * ║  • Extended RAM Filesystem (32 files × 4 KB, permissions)    ║
 * ║  • Timestamps via tick counter                               ║
 * ║  • C-Script v2 (vars, if/else, while, funcs, arithmetic)     ║
 * ║  • Tiny-Script v2 (registers, math, CMP/JEQ/JNE/JGT/JLT)    ║
 * ║  • Built-ins: nano, calc, ttt, hexdump, grep, wc, echo, env  ║
 * ║  • Color themes (theme command)                               ║
 * ║  • Boot splash + real CPUID/RTC hardware detection           ║
 * ║  • ACPI shutdown / reboot                                    ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  v6.1 Bug Fixes:                                             ║
 * ║  • run .c: strips #include/#define, handles int main() {}    ║
 * ║  • return; no longer corrupts the file (was writing '\0')    ║
 * ║  • return <expr>; now supported (not just bare return;)       ║
 * ║  • .c extension check fixed (was matching any trailing 'c')  ║
 * ║  • malloc: now handles exact-fit blocks; avoids zero-payload  ║
 * ║    header fragments with minimum split threshold             ║
 * ║  • itoa: undefined behaviour on INT_MIN fixed (unsigned cast) ║
 * ║  • cs_eval: two_char operator detection had wrong precedence  ║
 * ║  • cs_var_set: name buffer now always null-terminated        ║
 * ║  • proc_kill: wrong errno ENOENT→ESRCH for missing process   ║
 * ║  • cmd_ps: now shows per-process uptime (was UNUSED(now))    ║
 * ║  • cmd_mv: no longer deletes source when dest create fails   ║
 * ║  • input_string: term_col underflow guard (col 0 backspace)  ║
 * ║  • input_string: history paste now always null-terminates    ║
 * ║  • env_set: uses sizeof() instead of magic literal sizes     ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

/* ─────────────────────────────────────────────────────────────────
   1.  PRIMITIVE TYPES & MACROS
   ───────────────────────────────────────────────────────────────── */
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned int       size_t;
typedef int                bool;
typedef int                pid_t;

#define true   1
#define false  0
#define NULL   ((void*)0)
#define UNUSED(x) ((void)(x))

#define MAX_FILES     32
#define FILE_SIZE     4096
#define MAX_PROCS     16
#define MAX_ENV       32
#define HIST_SIZE     20
#define HIST_LEN      128
#define PIPE_BUF_SIZE 2048
#define MAX_ARGS      16

/* errno codes */
static int errno = 0;
#define ENOENT  2
#define EEXIST  17
#define ENOMEM  12
#define ENOSPC  28
#define EPERM   1
#define ESRCH   3   /* no such process */

/* ── User accounts ── */
typedef struct { char name[16]; char pass[32]; bool is_root; } UserAcct;
static UserAcct users[] = {
    {"root",  "toor", true },
    {"guest", "guest",false},
};
#define NUM_USERS 2
static int  logged_in_user = -1;   /* index into users[], -1 = nobody */
static char current_user[16] = ""; /* copy of users[logged_in_user].name */
static bool is_root = false;

/* ── System identity ── */
static char sys_hostname[32] = "mpos";
static char cwd[64]          = "/";   /* current working directory */

/* Forward-declared globals needed by VGA/keyboard before their definition site */
static char pipe_buf[PIPE_BUF_SIZE];
static int  pipe_len = 0;
static bool pipe_mode = false;
static bool gui_mode  = false;

/* ─────────────────────────────────────────────────────────────────
   2.  HARDWARE PORT I/O
   ───────────────────────────────────────────────────────────────── */
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port));
    return r;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile("inw %1,%0":"=a"(r):"Nd"(port));
    return r;
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port));
}
static inline void io_wait() { outb(0x80, 0); }

/* Read CPU timestamp counter */
static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ─────────────────────────────────────────────────────────────────
   3.  HARDWARE PROBE
   ───────────────────────────────────────────────────────────────── */
void get_cpu_vendor(char *buf) {
    uint32_t ebx, ecx, edx;
    __asm__ volatile("cpuid":"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(0));
    char *b=(char*)&ebx, *d=(char*)&edx, *c=(char*)&ecx;
    for(int i=0;i<4;i++) buf[i]=b[i];
    for(int i=0;i<4;i++) buf[4+i]=d[i];
    for(int i=0;i<4;i++) buf[8+i]=c[i];
    buf[12]='\0';
}

void get_cpu_brand(char *buf) {
    uint32_t regs[4];
    /* EAX = 0x80000002..0x80000004 gives brand string */
    for(int leaf=0;leaf<3;leaf++) {
        __asm__ volatile("cpuid":"=a"(regs[0]),"=b"(regs[1]),"=c"(regs[2]),"=d"(regs[3])
                         :"a"(0x80000002+leaf));
        for(int j=0;j<16;j++) buf[leaf*16+j]=((char*)regs)[j];
    }
    buf[48]='\0';
}

uint32_t get_cpu_max_leaf() {
    uint32_t eax;
    /* Must set EAX=0 before CPUID; without the input constraint EAX is
       whatever garbage the compiler left in the register — undefined behaviour. */
    __asm__ volatile("cpuid":"=a"(eax):"a"(0):"ebx","ecx","edx");
    return eax;
}

/* CMOS/RTC: read a byte from the real-time clock */
uint8_t rtc_read(uint8_t reg) {
    outb(0x70, reg); io_wait(); return inb(0x71);
}
/* BCD to binary */
#define BCD(x) (((x)>>4)*10+((x)&0xF))

typedef struct { uint8_t sec, min, hour, day, month; uint16_t year; } DateTime;
void rtc_get(DateTime *dt) {
    /* Wait until update is not in progress */
    while (rtc_read(0x0A) & 0x80);
    dt->sec   = BCD(rtc_read(0x00));
    dt->min   = BCD(rtc_read(0x02));
    dt->hour  = BCD(rtc_read(0x04));
    dt->day   = BCD(rtc_read(0x07));
    dt->month = BCD(rtc_read(0x08));
    dt->year  = BCD(rtc_read(0x09)) + 2000;
}


/* ─────────────────────────────────────────────────────────────────
   4.  VGA TEXT-MODE DRIVER  (80×25)
   ───────────────────────────────────────────────────────────────── */
volatile uint16_t *vga = (uint16_t*)0xB8000;
int term_col = 0, term_row = 0;
const int VGA_W = 80, VGA_H = 25;
uint8_t term_color = 0x0F;      /* default bright white on black */
uint8_t saved_color = 0x0F;

/* Active color theme (foreground colors) */
typedef struct { uint8_t normal, header, accent, error, success; } Theme;
static Theme themes[] = {
    {0x0F, 0x0B, 0x0A, 0x0C, 0x0A}, /* 0 Classic  */
    {0x07, 0x0E, 0x09, 0x0C, 0x02}, /* 1 Amber    */
    {0x0A, 0x0B, 0x0F, 0x0C, 0x0E}, /* 2 Matrix   */
    {0x0F, 0x0D, 0x0B, 0x0C, 0x09}, /* 3 Neon     */
    {0x07, 0x0F, 0x08, 0x04, 0x02}, /* 4 Mono     */
};
static int current_theme = 0;
#define T_NORMAL  (themes[current_theme].normal)
#define T_HEADER  (themes[current_theme].header)
#define T_ACCENT  (themes[current_theme].accent)
#define T_ERROR   (themes[current_theme].error)
#define T_SUCCESS (themes[current_theme].success)

void disable_cursor() { outb(0x3D4,0x0A); outb(0x3D5,0x20); }

void set_cursor_hw(int row, int col) {
    uint16_t pos = row * VGA_W + col;
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void scroll_up() {
    for(int y=0;y<VGA_H-1;y++)
        for(int x=0;x<VGA_W;x++)
            vga[y*VGA_W+x] = vga[(y+1)*VGA_W+x];
    for(int x=0;x<VGA_W;x++)
        vga[(VGA_H-1)*VGA_W+x] = (uint16_t)' ' | (uint16_t)(0x07<<8);
    if(term_row > 0) term_row--;
}

void term_putc(char c) {
    /* In GUI mode never touch VGA text memory — redirect to pipe_buf if active */
    if(gui_mode) {
        if(pipe_mode && c != '\r') {
            if(pipe_len < PIPE_BUF_SIZE-1) { pipe_buf[pipe_len++] = c; pipe_buf[pipe_len] = '\0'; }
        }
        return;
    }
    if(c == '\n') { term_col=0; term_row++; }
    else if(c == '\r') { term_col=0; }
    else if(c == '\t') { term_col = (term_col+8)&~7; }
    else {
        if(term_col >= VGA_W) { term_col=0; term_row++; }
        vga[term_row*VGA_W+term_col] = (uint16_t)(unsigned char)c | (uint16_t)(term_color<<8);
        term_col++;
    }
    while(term_row >= VGA_H) scroll_up();
}

void print(const char *s) { for(;*s;s++) term_putc(*s); }
void println(const char *s) { print(s); term_putc('\n'); }
void print_char(char c) { term_putc(c); }

void print_col(const char *s, uint8_t color) {
    uint8_t save = term_color; term_color = color;
    print(s); term_color = save;
}

void clear_screen() {
    if(gui_mode) return;  /* never wipe VGA text memory while in graphics mode */
    for(int i=0;i<VGA_W*VGA_H;i++)
        vga[i] = (uint16_t)' ' | (uint16_t)(0x07<<8);
    term_row=0; term_col=0;
}

/* Draw a horizontal rule */
void hline(char c, int width) {
    for(int i=0;i<width;i++) term_putc(c);
    term_putc('\n');
}

/* ─────────────────────────────────────────────────────────────────
   5.  STRING UTILITIES
   ───────────────────────────────────────────────────────────────── */
int strlen(const char *s) { int n=0; while(s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while(*a && *a==*b){a++;b++;} return *(unsigned char*)a-*(unsigned char*)b;
}
int strncmp(const char *a, const char *b, int n) {
    while(n>0&&*a&&*a==*b){a++;b++;n--;} return n?*(unsigned char*)a-*(unsigned char*)b:0;
}
void strcpy(char *d, const char *s) { while((*d++=*s++)); }
void strncpy(char *d, const char *s, int n) {
    int i=0; while(i<n-1&&s[i]){d[i]=s[i];i++;} d[i]='\0';
}
void strcat(char *d, const char *s) { while(*d) d++; while((*d++=*s++)); }
char *strchr(const char *s, char c) {
    while(*s){ if(*s==c) return (char*)s; s++; } return NULL;
}
char *strrchr(const char *s, char c) {
    const char *last=NULL;
    while(*s){ if(*s==c) last=s; s++; } return (char*)last;
}
void strncat(char *d, const char *s, int n) {
    while(*d) d++;
    while(n-->0&&*s) *d++=*s++;
    *d='\0';
}
/* Case-insensitive compare for short strings */
int strcasecmp(const char *a, const char *b) {
    while(*a&&*b){
        char ca=(*a>='A'&&*a<='Z')?*a+32:*a;
        char cb=(*b>='A'&&*b<='Z')?*b+32:*b;
        if(ca!=cb) return ca-cb; a++;b++;
    }
    return (unsigned char)*a-(unsigned char)*b;
}
/* Find first occurrence of needle in haystack */
char *strstr(const char *hay, const char *needle) {
    int nl=strlen(needle);
    while(*hay){ if(strncmp(hay,needle,nl)==0) return (char*)hay; hay++; }
    return NULL;
}
/* Trim leading/trailing whitespace (modifies string) */
char *strtrim(char *s) {
    while(*s==' '||*s=='\t') s++;
    int l=strlen(s);
    while(l>0&&(s[l-1]==' '||s[l-1]=='\t'||s[l-1]=='\n')) s[--l]='\0';
    return s;
}
int atoi(char *s) {
    int r=0,sign=1,i=0;
    if(s[0]=='-'){sign=-1;i++;}
    for(;s[i];i++) if(s[i]>='0'&&s[i]<='9') r=r*10+(s[i]-'0');
    return sign*r;
}
void itoa(int n, char *s) {
    int i = 0, sign = (n < 0);
    /* Cast to unsigned before negating to avoid UB on INT_MIN */
    unsigned int u = sign ? (unsigned int)(-(n + 1)) + 1u : (unsigned int)n;
    do { s[i++] = '0' + (int)(u % 10); } while ((u /= 10) > 0);
    if(sign) s[i++] = '-'; s[i] = '\0';
    for(int j=0, k=i-1; j<k; j++, k--) { char t=s[j]; s[j]=s[k]; s[k]=t; }
}
void itoh(uint32_t n, char *s) {
    /* Hex string, 8 digits */
    const char *hex="0123456789ABCDEF";
    for(int i=7;i>=0;i--){ s[i]=hex[n&0xF]; n>>=4; }
    s[8]='\0';
}
void memset(void *p, uint8_t v, size_t n) {
    uint8_t *b=(uint8_t*)p; while(n--) *b++=v;
}
void memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd=(uint8_t*)d; const uint8_t *ss=(const uint8_t*)s;
    while(n--) *dd++=*ss++;
}
/* split string by delimiter, returns token count, fills argv */
int split_args(char *input, char **argv, int max) {
    int argc=0;
    char *p=input;
    while(*p&&argc<max) {
        while(*p==' ') p++;
        if(!*p) break;
        argv[argc++]=p;
        while(*p&&*p!=' ') p++;
        if(*p) { *p='\0'; p++; }
    }
    return argc;
}

/* ─────────────────────────────────────────────────────────────────
   6.  BUMP MEMORY ALLOCATOR
   ───────────────────────────────────────────────────────────────── */
#define HEAP_SIZE (256*1024)  /* 256 KB heap */
static uint8_t _heap[HEAP_SIZE];
static size_t  _heap_ptr = 0;

typedef struct AllocBlock {
    size_t size;
    bool   free;
    struct AllocBlock *next;
} AllocBlock;

static AllocBlock *heap_head = NULL;

void mm_init() {
    memset(_heap, 0, HEAP_SIZE);
    heap_head = (AllocBlock*)_heap;
    heap_head->size = HEAP_SIZE - sizeof(AllocBlock);
    heap_head->free = true;
    heap_head->next = NULL;
    _heap_ptr = 0;
}

void *malloc(size_t size) {
    if(size == 0) return NULL;
    size = (size + 7) & ~7; /* align to 8 bytes */
    AllocBlock *cur = heap_head;
    while(cur) {
        if(cur->free && cur->size >= size) {
            /* Only split if the leftover is large enough to be a usable block;
               otherwise hand the entire block to the caller to avoid creating
               zero-payload header fragments. */
            if(cur->size >= size + sizeof(AllocBlock) + 8) {
                AllocBlock *next = (AllocBlock*)((uint8_t*)cur + sizeof(AllocBlock) + size);
                next->size = cur->size - size - sizeof(AllocBlock);
                next->free = true;
                next->next = cur->next;
                cur->size  = size;
                cur->next  = next;
            }
            cur->free = false;
            errno = 0;
            return (void*)((uint8_t*)cur + sizeof(AllocBlock));
        }
        cur = cur->next;
    }
    errno = ENOMEM;
    return NULL;
}

void free(void *ptr) {
    if(!ptr) return;
    AllocBlock *block = (AllocBlock*)((uint8_t*)ptr - sizeof(AllocBlock));
    block->free = true;
    /* Coalesce adjacent free blocks */
    AllocBlock *cur = heap_head;
    while(cur && cur->next) {
        if(cur->free && cur->next->free) {
            cur->size += sizeof(AllocBlock) + cur->next->size;
            cur->next  = cur->next->next;
        } else cur = cur->next;
    }
}

size_t mm_free_bytes() {
    size_t total=0;
    AllocBlock *c=heap_head;
    while(c){ if(c->free) total+=c->size; c=c->next; }
    return total;
}

/* ─────────────────────────────────────────────────────────────────
   7.  KEYBOARD DRIVER  (with shift & special keys)
   ───────────────────────────────────────────────────────────────── */
#define KEY_UP    0xF0
#define KEY_DOWN  0xF1
#define KEY_LEFT  0xF2
#define KEY_RIGHT 0xF3
#define KEY_DEL   0xF4
#define KEY_HOME  0xF5
#define KEY_END   0xF6
#define KEY_PGUP  0xF7
#define KEY_PGDN  0xF8

static const char keymap_lo[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,
    '\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
};
static const char keymap_hi[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',0,
    '|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',
};

static bool shift_held  = false;
static bool ctrl_held   = false;
static bool escape_next = false;

char get_key() {
    /* In GUI mode we must never block — the GUI has its own event loop */
    if(gui_mode) return '\n';
    while(1) {
        if(!(inb(0x64) & 1)) continue;
        uint8_t sc = inb(0x60);
        bool released = (sc & 0x80);
        uint8_t code  = sc & 0x7F;

        /* Extended scancode prefix */
        if(sc == 0xE0) { escape_next=true; continue; }

        if(escape_next) {
            escape_next=false;
            if(!released) {
                if(code==0x48) return KEY_UP;
                if(code==0x50) return KEY_DOWN;
                if(code==0x4B) return KEY_LEFT;
                if(code==0x4D) return KEY_RIGHT;
                if(code==0x53) return KEY_DEL;
                if(code==0x47) return KEY_HOME;
                if(code==0x4F) return KEY_END;
                if(code==0x49) return KEY_PGUP;
                if(code==0x51) return KEY_PGDN;
            }
            continue;
        }

        if(code==0x2A||code==0x36) { shift_held=!released; continue; }
        if(code==0x1D)             { ctrl_held =!released; continue; }

        if(!released && code < 128) {
            char c = shift_held ? keymap_hi[code] : keymap_lo[code];
            if(ctrl_held && c>='a'&&c<='z') return c-'a'+1; /* Ctrl+A=1 etc */
            if(c) return c;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────
   8.  SHELL HISTORY
   ───────────────────────────────────────────────────────────────── */
static char hist[HIST_SIZE][HIST_LEN];
static int hist_count = 0;
static int hist_idx   = 0;

void hist_push(const char *cmd) {
    if(cmd[0]=='\0') return;
    /* Avoid duplicates */
    if(hist_count>0 && strcmp(hist[(hist_count-1)%HIST_SIZE],cmd)==0) return;
    strcpy(hist[hist_count%HIST_SIZE], cmd);
    hist_count++;
}

const char *hist_prev(int *nav) {
    if(*nav >= hist_count) return NULL;
    (*nav)++;
    return hist[(hist_count - *nav) % HIST_SIZE];
}
const char *hist_next(int *nav) {
    if(*nav <= 0) return NULL;
    (*nav)--;
    if(*nav==0) return "";
    return hist[(hist_count - *nav) % HIST_SIZE];
}

/* ─────────────────────────────────────────────────────────────────
   9.  ENVIRONMENT VARIABLES
   ───────────────────────────────────────────────────────────────── */
typedef struct { char key[32]; char val[128]; } EnvVar;
static EnvVar env[MAX_ENV];
static int env_count = 0;

void env_set(const char *key, const char *val) {
    for(int i=0;i<env_count;i++) {
        if(strcmp(env[i].key,key)==0){
            strncpy(env[i].val, val, sizeof(env[i].val)-1); return;
        }
    }
    if(env_count<MAX_ENV) {
        strncpy(env[env_count].key, key, sizeof(env[env_count].key)-1);
        strncpy(env[env_count].val, val, sizeof(env[env_count].val)-1);
        env_count++;
    }
}

const char *env_get(const char *key) {
    for(int i=0;i<env_count;i++) if(strcmp(env[i].key,key)==0) return env[i].val;
    return NULL;
}

/* Expand $VAR in src into dst (dst must be large enough) */
void env_expand(const char *src, char *dst, int max) {
    int si=0,di=0;
    while(src[si]&&di<max-1) {
        if(src[si]=='$') {
            si++;
            char var[32]; int vi=0;
            while(src[si]&&src[si]!=' '&&src[si]!='/'&&vi<31) var[vi++]=src[si++];
            var[vi]='\0';
            const char *val=env_get(var);
            if(val) { while(*val&&di<max-1) dst[di++]=*val++; }
        } else { dst[di++]=src[si++]; }
    }
    dst[di]='\0';
}

/* ─────────────────────────────────────────────────────────────────
   10. PROCESS TABLE
   ───────────────────────────────────────────────────────────────── */
#define PROC_FREE    0
#define PROC_RUNNING 1
#define PROC_ZOMBIE  2

typedef struct {
    int    state;
    pid_t  pid;
    char   name[32];
    int    exit_code;
    uint64_t start_tick;
} Process;

static Process proc_table[MAX_PROCS];
static pid_t next_pid = 1;
static pid_t cur_pid  = 0;
static uint64_t boot_tick = 0;

void proc_init() {
    for(int i=0;i<MAX_PROCS;i++) proc_table[i].state=PROC_FREE;
    boot_tick = rdtsc();
    /* Create kernel process */
    proc_table[0].state = PROC_RUNNING;
    proc_table[0].pid   = 0;
    proc_table[0].exit_code = 0;
    proc_table[0].start_tick = boot_tick;
    strcpy(proc_table[0].name,"kernel");
    cur_pid = 0;
}

pid_t proc_spawn(const char *name) {
    for(int i=1;i<MAX_PROCS;i++) {
        if(proc_table[i].state==PROC_FREE) {
            proc_table[i].state = PROC_RUNNING;
            proc_table[i].pid   = next_pid++;
            proc_table[i].exit_code = 0;
            proc_table[i].start_tick = rdtsc();
            strncpy(proc_table[i].name, name, 32);
            return proc_table[i].pid;
        }
    }
    errno = ENOMEM;
    return -1;
}

void proc_kill(pid_t pid) {
    if(pid==0){ print("Cannot kill kernel.\n"); return; }
    for(int i=0;i<MAX_PROCS;i++) {
        if(proc_table[i].pid==pid && proc_table[i].state!=PROC_FREE) {
            proc_table[i].state=PROC_ZOMBIE;
            return;
        }
    }
    errno=ESRCH; print("No such process.\n");
}

void proc_exit(pid_t pid, int code) {
    for(int i=0;i<MAX_PROCS;i++) {
        if(proc_table[i].pid==pid) { proc_table[i].state=PROC_ZOMBIE; proc_table[i].exit_code=code; return; }
    }
}

void cmd_ps() {
    uint64_t now = rdtsc();
    print_col("\n PID  STATE    UPTIME(s)  NAME\n", T_HEADER);
    hline('-', 40);
    for(int i=0;i<MAX_PROCS;i++) {
        if(proc_table[i].state==PROC_FREE) continue;
        char buf[16];
        itoa(proc_table[i].pid, buf);
        int pl=strlen(buf); print(" ");
        for(int j=pl;j<4;j++) print(" ");
        print(buf); print("  ");
        if(proc_table[i].state==PROC_RUNNING)  print_col("running  ", T_SUCCESS);
        else                                    print_col("zombie   ", T_ERROR);
        /* Per-process uptime using same TSC shift trick as cmd_uptime */
        uint64_t delta = now - proc_table[i].start_tick;
        uint32_t hi = (uint32_t)(delta >> 32);
        uint32_t lo = (uint32_t)(delta);
        uint32_t secs = (hi << 1) | (lo >> 31);
        itoa(secs, buf);
        int sl=strlen(buf); for(int j=sl;j<10;j++) print(" ");
        print(buf); print("  ");
        println(proc_table[i].name);
    }
    print("\n");
}

/* ─────────────────────────────────────────────────────────────────
   11. PIPE BUFFER
   ───────────────────────────────────────────────────────────────── */
/* pipe_buf, pipe_len, pipe_mode, gui_mode declared at top of file */

void pipe_clear()  { pipe_buf[0]='\0'; pipe_len=0; }
void pipe_write(const char *s) {
    while(*s && pipe_len<PIPE_BUF_SIZE-1) { pipe_buf[pipe_len++]=*s++; }
    pipe_buf[pipe_len]='\0';
}

/* Redirect-aware print (used during pipe) */
void rprint(const char *s) {
    if(pipe_mode) pipe_write(s);
    else print(s);
}

/* ─────────────────────────────────────────────────────────────────
   12. FILE SYSTEM  (RAM disk, 32 files × 4 KB)
   ───────────────────────────────────────────────────────────────── */
#define PERM_R 0x01
#define PERM_W 0x02
#define PERM_X 0x04

typedef struct {
    char    name[32];
    char   *content;      /* allocated from heap */
    size_t  size;
    bool    active;
    uint8_t perms;
    DateTime mtime;
} File;

static File fs[MAX_FILES];
static int  fs_count = 0;

/* Forward declarations */
int   fs_find_idx(const char *name);
void  rprint_char(char c);

void fs_init() {
    for(int i=0;i<MAX_FILES;i++) { fs[i].active=false; fs[i].content=NULL; }
    fs_count=0;
}

/* Allocate file content from heap */
File *fs_create(const char *name, const char *initial) {
    if(fs_find_idx(name)!=-1){ errno=EEXIST; return NULL; }
    for(int i=0;i<MAX_FILES;i++) {
        if(!fs[i].active) {
            fs[i].content = (char*)malloc(FILE_SIZE);
            if(!fs[i].content){ errno=ENOMEM; return NULL; }
            memset(fs[i].content, 0, FILE_SIZE);
            strncpy(fs[i].name, name, 32);
            if(initial) strncpy(fs[i].content, initial, FILE_SIZE-1);
            fs[i].size   = initial ? strlen(initial) : 0;
            fs[i].active = true;
            fs[i].perms  = PERM_R|PERM_W;
            rtc_get(&fs[i].mtime);
            /* Make .c and .tiny files executable */
            int nl=strlen(name);
            if(nl>2 && name[nl-1]=='c' && name[nl-2]=='.') fs[i].perms|=PERM_X;
            if(nl>5 && strncmp(name+nl-5,".tiny",5)==0)     fs[i].perms|=PERM_X;
            fs_count++;
            return &fs[i];
        }
    }
    errno=ENOSPC; return NULL;
}

int fs_find_idx(const char *name) {
    for(int i=0;i<MAX_FILES;i++) if(fs[i].active&&strcmp(fs[i].name,name)==0) return i;
    return -1;
}

File *fs_find(const char *name) {
    int i=fs_find_idx(name); return i==-1?NULL:&fs[i];
}

void fs_rm(const char *name) {
    int i=fs_find_idx(name);
    if(i==-1){ errno=ENOENT; println("rm: file not found."); return; }
    if(!(fs[i].perms & PERM_W)){ errno=EPERM; println("rm: permission denied."); return; }
    free(fs[i].content);
    fs[i].content=NULL; fs[i].active=false; fs_count--; println("Deleted.");
}

void fs_write(File *f, const char *data) {
    if(!f||!f->content) return;
    strncpy(f->content, data, FILE_SIZE-1);
    f->size = strlen(data);
    rtc_get(&f->mtime);
}

void fs_append(File *f, const char *data) {
    if(!f||!f->content) return;
    int cur=strlen(f->content);
    strncpy(f->content+cur, data, FILE_SIZE-1-cur);
    f->size=strlen(f->content);
    rtc_get(&f->mtime);
}

/* Print file permissions string */
void fs_perm_str(uint8_t p, char *out) {
    out[0]=(p&PERM_R)?'r':'-';
    out[1]=(p&PERM_W)?'w':'-';
    out[2]=(p&PERM_X)?'x':'-';
    out[3]='\0';
}

void cmd_ls(bool long_fmt) {
    print_col("\n", T_HEADER);
    if(long_fmt) {
        print_col(" PERM  SIZE  DATE            NAME\n", T_HEADER);
        hline('-', 50);
    } else {
        print_col(" FILES:\n", T_HEADER);
    }
    for(int i=0;i<MAX_FILES;i++) {
        if(!fs[i].active) continue;
        if(long_fmt) {
            char perm[4]; fs_perm_str(fs[i].perms, perm);
            char sz[12]; itoa(fs[i].size, sz);
            char yr[8]; itoa(fs[i].mtime.year, yr);
            char mo[4]; itoa(fs[i].mtime.month, mo);
            char dy[4]; itoa(fs[i].mtime.day, dy);
            print(" "); print(perm); print("  ");
            int sl=strlen(sz); for(int j=sl;j<5;j++) print(" "); print(sz);
            print("  "); print(yr); print("-"); print(mo); print("-"); print(dy);
            print("  ");
        } else print("  ");
        print_col(fs[i].name, (fs[i].perms&PERM_X)?T_ACCENT:T_NORMAL);
        print("\n");
    }
    char total[12]; itoa(fs_count, total);
    print("\n"); print(total); print(" file(s)\n\n");
}

void cmd_cat(const char *name) {
    File *f=fs_find(name);
    if(!f){ errno=ENOENT; println("cat: file not found."); return; }
    if(!(f->perms&PERM_R)){ errno=EPERM; println("cat: permission denied."); return; }
    print("\n"); print(f->content); print("\n");
}

void cmd_wc(const char *name) {
    File *f=fs_find(name);
    if(!f){ println("wc: not found."); return; }
    int lines=0, words=0, bytes=strlen(f->content);
    bool in_word=false;
    for(int i=0;f->content[i];i++) {
        char c=f->content[i];
        if(c=='\n') lines++;
        if(c==' '||c=='\n'||c=='\t') { if(in_word){words++;in_word=false;} }
        else in_word=true;
    }
    if(in_word) words++;
    char buf[16];
    print(" lines:"); itoa(lines,buf); print(buf);
    print("  words:"); itoa(words,buf); print(buf);
    print("  bytes:"); itoa(bytes,buf); print(buf);
    print("\n");
}

void cmd_grep(const char *pattern, const char *name) {
    File *f=fs_find(name);
    if(!f){ println("grep: file not found."); return; }
    /* Line-by-line search */
    char *p=f->content;
    int line_num=1; bool found=false;
    while(*p) {
        /* Find line end */
        char *eol=p;
        while(*eol&&*eol!='\n') eol++;
        /* Temporarily null-terminate */
        char save=*eol; *eol='\0';
        if(strstr(p,pattern)) {
            char n[8]; itoa(line_num,n);
            print_col(n, T_ACCENT); print(": "); println(p);
            found=true;
        }
        *eol=save;
        if(*eol=='\n') eol++;
        p=eol; line_num++;
    }
    if(!found) println("(no match)");
}

void cmd_hexdump(const char *name) {
    File *f=fs_find(name);
    if(!f){ println("hexdump: not found."); return; }
    uint8_t *data=(uint8_t*)f->content;
    int sz=f->size; if(sz==0) sz=strlen(f->content);
    char addr[12], hb[4], asc[17];
    print_col("\n OFFSET   00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  ASCII\n", T_HEADER);
    hline('-', 72);
    for(int off=0;off<sz;off+=16) {
        itoh(off, addr); print(" "); print(addr); print("  ");
        memset(asc,0,17);
        for(int j=0;j<16;j++) {
            if(off+j<sz) {
                uint8_t b=data[off+j];
                hb[0]="0123456789ABCDEF"[b>>4];
                hb[1]="0123456789ABCDEF"[b&0xF];
                hb[2]=' '; hb[3]='\0';
                print(hb);
                asc[j]=(b>=32&&b<127)?(char)b:'.';
            } else { print("   "); asc[j]=' '; }
            if(j==7) print(" ");
        }
        print(" "); println(asc);
    }
    print("\n");
}

void cmd_chmod(const char *perm, const char *name) {
    File *f=fs_find(name);
    if(!f){ println("chmod: not found."); return; }
    uint8_t p=0;
    if(strchr(perm,'r')) p|=PERM_R;
    if(strchr(perm,'w')) p|=PERM_W;
    if(strchr(perm,'x')) p|=PERM_X;
    f->perms=p; println("Done.");
}

/* cp */
void cmd_cp(const char *src, const char *dst) {
    File *f=fs_find(src);
    if(!f){ println("cp: source not found."); return; }
    if(!fs_create(dst, f->content)){ println("cp: failed."); return; }
    println("Copied.");
}

/* mv */
void cmd_mv(const char *src, const char *dst) {
    File *f = fs_find(src);
    if(!f){ println("mv: source not found."); return; }
    if(!fs_create(dst, f->content)){ println("mv: failed (dest may already exist)."); return; }
    fs_rm(src);
}

/* ─────────────────────────────────────────────────────────────────
   13. ADVANCED LINE EDITOR (with history navigation & cursor)
   ───────────────────────────────────────────────────────────────── */
void redraw_line(const char *prompt, const char *buf, int len, int cur) {
    /* Move to beginning of line */
    /* We can't easily move cursor arbitrarily in text mode without more
       infrastructure, so just reprint line with a simple approach: */
    UNUSED(cur); UNUSED(len);
    print(prompt); print(buf);
}

void input_string(char *buffer, int max_len) {
    int i=0, nav=0;
    buffer[0]='\0';
    while(1) {
        char c=get_key();
        if(c=='\n') { buffer[i]='\0'; term_putc('\n'); return; }
        else if(c=='\b') {
            if(i>0){
                i--; buffer[i]='\0';
                if(term_col > 0) term_col--;
                term_putc(' ');
                if(term_col > 0) term_col--;
            }
        }
        else if(c==KEY_UP) {
            /* Erase current input */
            for(int j=0;j<i;j++){
                if(term_col > 0) term_col--;
                term_putc(' ');
                if(term_col > 0) term_col--;
            }
            const char *h=hist_prev(&nav);
            if(h){ strncpy(buffer,h,max_len-1); buffer[max_len-1]='\0'; i=strlen(buffer); print(buffer); }
            else { buffer[0]='\0'; i=0; }
        }
        else if(c==KEY_DOWN) {
            for(int j=0;j<i;j++){
                if(term_col > 0) term_col--;
                term_putc(' ');
                if(term_col > 0) term_col--;
            }
            const char *h=hist_next(&nav);
            if(h){ strncpy(buffer,h,max_len-1); buffer[max_len-1]='\0'; i=strlen(buffer); print(buffer); }
            else { buffer[0]='\0'; i=0; nav=0; }
        }
        else if(c=='\t') {
            tab_complete(buffer, &i);
            buffer[i]='\0';
        }
        else if(c>0&&c<0xF0) {
            if(i<max_len-1){ buffer[i++]=c; buffer[i]='\0'; term_putc(c); }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────
   14. C-SCRIPT v2 INTERPRETER
       Supports: variables (int), print(), if/else, while, arithmetic,
                 sysinfo(), color(), cls(), wait(), return, // comments
   ───────────────────────────────────────────────────────────────── */
#define CS_MAX_VARS 32
typedef struct { char name[16]; int val; } CVar;
static CVar cs_vars[CS_MAX_VARS];
static int  cs_var_count = 0;

int cs_var_get(const char *name) {
    for(int i=0;i<cs_var_count;i++) if(strcmp(cs_vars[i].name,name)==0) return cs_vars[i].val;
    return 0;
}
void cs_var_set(const char *name, int val) {
    for(int i=0;i<cs_var_count;i++) { if(strcmp(cs_vars[i].name,name)==0){cs_vars[i].val=val;return;} }
    if(cs_var_count<CS_MAX_VARS){
        strncpy(cs_vars[cs_var_count].name, name, sizeof(cs_vars[0].name)-1);
        cs_vars[cs_var_count].name[sizeof(cs_vars[0].name)-1] = '\0';
        cs_vars[cs_var_count++].val=val;
    }
}

/* Skip whitespace in interpreter cursor */
static void cs_skip_ws(char **p) { while(**p==' '||**p=='\t') (*p)++; }

/* Evaluate a simple integer expression: supports +,-,*,/,%,==,!=,<,>,<=,>= */
/* Handles: number, variable, (expr op expr) */
int cs_eval(char **p);

int cs_eval_primary(char **p) {
    cs_skip_ws(p);
    if(**p=='(') {
        (*p)++; int v=cs_eval(p); cs_skip_ws(p);
        if(**p==')') (*p)++; return v;
    }
    /* Negative number */
    int sign=1;
    if(**p=='-'){ sign=-1; (*p)++; }
    /* Number literal */
    if(**p>='0'&&**p<='9') {
        int v=0; while(**p>='0'&&**p<='9'){ v=v*10+(**p-'0'); (*p)++; }
        return sign*v;
    }
    /* Variable name */
    char vname[16]; int vi=0;
    while((**p>='a'&&**p<='z')||(**p>='A'&&**p<='Z')||**p=='_'||(**p>='0'&&**p<='9'))
        vname[vi++]=*(*p)++;
    vname[vi]='\0';
    return sign*cs_var_get(vname);
}

int cs_eval(char **p) {
    int left=cs_eval_primary(p);
    cs_skip_ws(p);
    while(**p=='+'||**p=='-'||**p=='*'||**p=='/'||**p=='%'||
          **p=='='||**p=='!'||**p=='<'||**p=='>') {
        char op1=**p; (*p)++; char op2=**p;
        /* two_char ops: ==, !=, <=, >= */
        bool two_char = (op2 == '=' && (op1=='='||op1=='!'||op1=='<'||op1=='>'));
        if(two_char) (*p)++;
        cs_skip_ws(p);
        int right=cs_eval_primary(p);
        if     (op1=='+'              ) left=left+right;
        else if(op1=='-'              ) left=left-right;
        else if(op1=='*'              ) left=left*right;
        else if(op1=='/'&&right!=0    ) left=left/right;
        else if(op1=='%'&&right!=0    ) left=left%right;
        else if(op1=='='&&op2=='='    ) left=(left==right);
        else if(op1=='!'&&op2=='='    ) left=(left!=right);
        else if(op1=='<'&&op2=='='    ) left=(left<=right);
        else if(op1=='>'&&op2=='='    ) left=(left>=right);
        else if(op1=='<'              ) left=(left<right);
        else if(op1=='>'              ) left=(left>right);
        cs_skip_ws(p);
    }
    return left;
}

/* Skip a block { ... } handling nesting */
void cs_skip_block(char **p) {
    int depth=0;
    while(**p) {
        if(**p=='{') depth++;
        else if(**p=='}') { depth--; if(depth<=0){(*p)++;return;} }
        (*p)++;
    }
}

/* Forward declaration */
/* CS return flag — set instead of writing '\0' into the file buffer */
static bool cs_returned = false;

void cs_run_block(char **p, bool execute);

void cs_run_stmt(char **p, bool execute) {
    cs_skip_ws(p);
    if(**p=='\0') return;

    /* Comment // */
    if(strncmp(*p,"//",2)==0) { while(**p&&**p!='\n') (*p)++; return; }

    /* Block { } */
    if(**p=='{') {
        (*p)++; cs_run_block(p, execute);
        if(**p=='}') (*p)++;
        return;
    }

    /* print("...") */
    if(strncmp(*p,"print(\"",7)==0) {
        *p+=7;
        if(execute) {
            while(**p!='"'&&**p) { rprint_char(**p); (*p)++; }
        } else {
            while(**p!='"'&&**p) (*p)++;
        }
        if(**p=='"') (*p)++;
        if(**p==')') (*p)++;
        if(**p==';') (*p)++;
        if(execute) rprint("\n");
        return;
    }

    /* printv(varname) — print variable value */
    if(strncmp(*p,"printv(",7)==0) {
        *p+=7;
        char vn[16]; int vi=0;
        while(**p!=')'&&**p&&vi<15) vn[vi++]=*(*p)++;
        vn[vi]='\0';
        if(**p==')') (*p)++;
        if(**p==';') (*p)++;
        if(execute){ char buf[16]; itoa(cs_var_get(vn),buf); rprint(buf); rprint("\n"); }
        return;
    }

    /* print_num(expr) — print integer without trailing newline */
    if(strncmp(*p,"print_num(",10)==0) {
        *p+=10;
        int val=0;
        if(execute) val=cs_eval(p); else cs_eval(p);
        if(**p==')') (*p)++;
        if(**p==';') (*p)++;
        if(execute){ char buf[16]; itoa(val,buf); rprint(buf); }
        return;
    }

    /* print_nl() — emit a single newline */
    if(strncmp(*p,"print_nl();",11)==0) {
        if(execute) rprint("\n");
        *p+=11; return;
    }

    /* cls(); */
    if(strncmp(*p,"cls();",6)==0) { if(execute) clear_screen(); *p+=6; return; }

    /* color(name); */
    if(strncmp(*p,"color(",6)==0) {
        *p+=6; char cn[16]; int ci=0;
        while(**p!=')'&&**p&&ci<15) cn[ci++]=*(*p)++;
        cn[ci]='\0';
        if(**p==')') (*p)++;
        if(**p==';') (*p)++;
        if(execute){
            if(strcmp(cn,"red")==0)    term_color=0x04;
            else if(strcmp(cn,"green")==0) term_color=0x0A;
            else if(strcmp(cn,"blue")==0)  term_color=0x09;
            else if(strcmp(cn,"yellow")==0)term_color=0x0E;
            else if(strcmp(cn,"cyan")==0)  term_color=0x0B;
            else if(strcmp(cn,"white")==0) term_color=0x0F;
            else if(strcmp(cn,"grey")==0)  term_color=0x07;
        }
        return;
    }

    /* wait(); */
    if(strncmp(*p,"wait();",7)==0) {
        if(execute){ print("[Press any key...]"); get_key(); print("\n"); }
        *p+=7; return;
    }

    /* sysinfo(); */
    if(strncmp(*p,"sysinfo();",10)==0) {
        if(execute){
            char v[13]; get_cpu_vendor(v);
            char b[49]; get_cpu_brand(b);
            rprint("CPU Vendor: "); rprint(v); rprint("\n");
            rprint("CPU Brand: ");  rprint(b); rprint("\n");
        }
        *p+=10; return;
    }

    /* return [expr]; — use a flag, never write into the file buffer */
    if(strncmp(*p,"return",6)==0 &&
       ((*p)[6]==';'||(*p)[6]==' '||(*p)[6]=='\n'||(*p)[6]=='\0')) {
        *p+=6; cs_skip_ws(p);
        if(**p!=';'&&**p!='\n'&&**p!='\0') cs_eval(p); /* consume optional value */
        if(**p==';') (*p)++;
        if(execute) cs_returned = true;
        return;
    }

    /* int varname = expr; */
    if(strncmp(*p,"int ",4)==0) {
        *p+=4; cs_skip_ws(p);
        char vn[16]; int vi=0;
        while((**p>='a'&&**p<='z')||(**p>='A'&&**p<='Z')||**p=='_') vn[vi++]=*(*p)++;
        vn[vi]='\0';
        cs_skip_ws(p);
        int val=0;
        if(**p=='='){ (*p)++; cs_skip_ws(p); val=cs_eval(p); }
        if(**p==';') (*p)++;
        if(execute) cs_var_set(vn, val);
        return;
    }

    /* varname = expr; (assignment) */
    {
        char *save=*p; char vn[16]; int vi=0;
        while((**p>='a'&&**p<='z')||(**p>='A'&&**p<='Z')||**p=='_'||(**p>='0'&&**p<='9'&&vi>0))
            vn[vi++]=*(*p)++;
        vn[vi]='\0';
        cs_skip_ws(p);
        if(**p=='='&&*(*p+1)!='=') {
            (*p)++;
            cs_skip_ws(p);
            int val=cs_eval(p);
            if(**p==';') (*p)++;
            if(execute) cs_var_set(vn, val);
            return;
        }
        *p=save; /* Not assignment, restore */
    }

    /* if(expr) { ... } [else { ... }] */
    if(strncmp(*p,"if(",3)==0) {
        *p+=3;
        int cond=cs_eval(p);
        cs_skip_ws(p);
        if(**p==')') (*p)++;
        cs_skip_ws(p);
        bool run_then = execute && (cond!=0);
        bool run_else = execute && (cond==0);
        if(**p=='{') { (*p)++; cs_run_block(p, run_then); if(**p=='}') (*p)++; }
        else cs_run_stmt(p, run_then);
        cs_skip_ws(p);
        if(strncmp(*p,"else",4)==0) {
            *p+=4; cs_skip_ws(p);
            if(**p=='{') { (*p)++; cs_run_block(p, run_else); if(**p=='}') (*p)++; }
            else cs_run_stmt(p, run_else);
        }
        return;
    }

    /* while(expr) { ... } */
    if(strncmp(*p,"while(",6)==0) {
        char *loop_start=*p;
        *p+=6;
        int max_iter=10000;
        while(max_iter-->0) {
            char *cp=*p;
            int cond=cs_eval(&cp);
            if(cp[0]==')') cp++;
            cs_skip_ws(&cp);
            if(!cond||!execute) {
                /* Skip body */
                if(*cp=='{') { cp++; cs_run_block(&cp, false); if(*cp=='}') cp++; }
                *p=cp; return;
            }
            if(*cp=='{') { cp++; cs_run_block(&cp, true); if(*cp=='}') cp++; }
            *p=loop_start+6;
        }
        return;
    }

    /* Unknown — skip to newline */
    while(**p&&**p!='\n'&&**p!=';') (*p)++;
    if(**p==';'||**p=='\n') (*p)++;
}

void cs_run_block(char **p, bool execute) {
    while(**p && **p!='}' && !cs_returned) {
        cs_skip_ws(p);
        if(**p=='\n'){ (*p)++; continue; }
        if(**p=='}') break;
        cs_run_stmt(p, execute);
    }
}

void rprint_char(char c) {
    if(pipe_mode) { if(pipe_len<PIPE_BUF_SIZE-1) pipe_buf[pipe_len++]=c; pipe_buf[pipe_len]='\0'; }
    else term_putc(c);
}

void run_c_script(char *code) {
    cs_var_count = 0;
    cs_returned  = false;

    /* Make a working copy so we never modify the file on disk */
    static char cs_work[FILE_SIZE];
    strncpy(cs_work, code, FILE_SIZE-1);
    cs_work[FILE_SIZE-1] = '\0';

    char *p = cs_work;

    /* Pre-pass: strip lines that real C has but C-Script can't handle.
       Replace each such line with spaces so pointer arithmetic stays valid. */
    char *scan = cs_work;
    while(*scan) {
        char *line_start = scan;
        /* Skip leading whitespace to check directive */
        while(*scan == ' ' || *scan == '\t') scan++;
        bool is_directive = (strncmp(scan,"#include",8)==0 ||
                             strncmp(scan,"#define",7)==0  ||
                             strncmp(scan,"#pragma",7)==0);
        /* Also skip lines that are only a function signature like:
           "int main()" / "void main()" — we handle entry below    */
        bool is_main_sig = (strncmp(scan,"int main",8)==0 ||
                            strncmp(scan,"void main",9)==0);
        /* Walk to end of line */
        while(*scan && *scan != '\n') scan++;
        if(is_directive) {
            /* Blank it out */
            for(char *c=line_start; c<scan; c++) *c=' ';
        }
        (void)is_main_sig; /* handled below */
        if(*scan == '\n') scan++;
    }

    /* If file contains a main() function, jump into its body */
    char *main_pos = strstr(cs_work, "main");
    if(main_pos) {
        /* Find the opening brace of main's body */
        char *brace = main_pos;
        while(*brace && *brace != '{') brace++;
        if(*brace == '{') {
            brace++; /* step past '{' */
            cs_run_block(&brace, true);
            goto done;
        }
    }

    /* No main() — treat entire file as a top-level block */
    cs_run_block(&p, true);

done:
    cs_returned = false;
    term_color = T_NORMAL;
}

static void run_c_script_named(File *f) {
    pid_t pid = proc_spawn("c-script");
    print_col("--- C-Script v2 ---\n", T_HEADER);
    run_c_script(f->content);
    print_col("--- Done ---\n", T_HEADER);
    proc_exit(pid, 0);
}

/* ─────────────────────────────────────────────────────────────────
   15. TINY-SCRIPT v2 INTERPRETER
       Registers: R0-R3, STACK (8 deep), CMPREG
       Ops: SET, ADD, SUB, MUL, DIV, MOD, PUSH, POP,
            CMP, JEQ, JNE, JGT, JLT, JGE, JLE, GOTO, CALL, RET,
            PRINT, PRINT_REG, INPUT, EXIT, CLS, COLOR, SYSINFO
   ───────────────────────────────────────────────────────────────── */
#define TS_REGS    4
#define TS_STACK   16
#define TS_LABELS  32

static int  ts_reg[TS_REGS];
static int  ts_stack[TS_STACK];
static int  ts_sp=0;
static int  ts_cmp=0;   /* compare result: -1, 0, 1 */
static char ts_buf[128];/* string input buffer */

typedef struct { char name[32]; char *addr; } TinyLabel;
static TinyLabel ts_labels[TS_LABELS];
static int ts_label_count=0;

/* Call stack for CALL/RET */
static char *ts_callstack[TS_STACK];
static int   ts_callsp=0;

/* Parse register index from "R0"-"R3" */
int ts_reg_idx(char **p) {
    if(**p=='R'||**p=='r') {
        (*p)++;
        int idx=**p-'0'; (*p)++;
        return (idx>=0&&idx<TS_REGS)?idx:0;
    }
    return 0;
}

/* Parse an integer value: number or register */
int ts_val(char **p) {
    while(**p==' ') (*p)++;
    if(**p=='R'||**p=='r') return ts_reg[ts_reg_idx(p)];
    /* Parse signed integer */
    int sign=1; if(**p=='-'){sign=-1;(*p)++;}
    int v=0;
    while(**p>='0'&&**p<='9'){ v=v*10+(**p-'0'); (*p)++; }
    return sign*v;
}

/* First pass: collect all labels */
void ts_collect_labels(char *code) {
    ts_label_count=0;
    char *p=code;
    while(*p) {
        while(*p==' '||*p=='\t') p++;
        if(*p=='\n'||*p=='\0') { if(*p) p++; continue; }
        /* Check for LABEL: */
        char *start=p;
        char tmp[32]; int ti=0;
        while(*p&&*p!=':'&&*p!='\n'&&*p!=' '&&ti<31) tmp[ti++]=*p++;
        tmp[ti]='\0';
        if(*p==':' && ti>0) {
            /* It's a label */
            if(ts_label_count<TS_LABELS) {
                strcpy(ts_labels[ts_label_count].name, tmp);
                ts_labels[ts_label_count].addr = p+1; /* after ':' */
                ts_label_count++;
            }
        }
        /* Skip to next line */
        while(*p&&*p!='\n') p++;
        if(*p=='\n') p++;
        UNUSED(start);
    }
}

char *ts_find_label(const char *name) {
    for(int i=0;i<ts_label_count;i++) if(strcmp(ts_labels[i].name,name)==0) return ts_labels[i].addr;
    return NULL;
}

void run_tiny_script(char *code) {
    ts_sp=0; ts_callsp=0; ts_cmp=0;
    for(int i=0;i<TS_REGS;i++) ts_reg[i]=0;
    ts_buf[0]='\0';
    ts_collect_labels(code);

    char *p=code;
    int max_steps=100000;
    pid_t pid=proc_spawn("tiny-script");
    print_col("--- Tiny-Script v2 ---\n", T_HEADER);

    while(*p && max_steps-->0) {
        /* Skip whitespace and blank lines */
        while(*p==' '||*p=='\t') p++;
        if(*p=='\n'||*p=='\r'){ p++; continue; }
        if(*p=='\0') break;

        /* Skip comments (# or ;) */
        if(*p=='#'||*p==';') { while(*p&&*p!='\n') p++; continue; }

        /* Read token */
        char tok[32]; int ti=0;
        while(*p&&*p!=' '&&*p!='\n'&&*p!='\t'&&ti<31) tok[ti++]=*p++;
        tok[ti]='\0';
        while(*p==' '||*p=='\t') p++;

        /* Labels end with : — skip */
        if(tok[ti-1]==':') { while(*p&&*p!='\n') p++; continue; }

        /* ---- INSTRUCTIONS ---- */
        if(strcmp(tok,"SET")==0) {
            int ri=ts_reg_idx(&p); while(*p==' '||*p==',') p++;
            ts_reg[ri]=ts_val(&p);
        }
        else if(strcmp(tok,"ADD")==0) {
            int ri=ts_reg_idx(&p); while(*p==' '||*p==',') p++;
            ts_reg[ri]+=ts_val(&p);
        }
        else if(strcmp(tok,"SUB")==0) {
            int ri=ts_reg_idx(&p); while(*p==' '||*p==',') p++;
            ts_reg[ri]-=ts_val(&p);
        }
        else if(strcmp(tok,"MUL")==0) {
            int ri=ts_reg_idx(&p); while(*p==' '||*p==',') p++;
            ts_reg[ri]*=ts_val(&p);
        }
        else if(strcmp(tok,"DIV")==0) {
            int ri=ts_reg_idx(&p); while(*p==' '||*p==',') p++;
            int v=ts_val(&p); if(v!=0) ts_reg[ri]/=v; else print("DIV/0\n");
        }
        else if(strcmp(tok,"MOD")==0) {
            int ri=ts_reg_idx(&p); while(*p==' '||*p==',') p++;
            int v=ts_val(&p); if(v!=0) ts_reg[ri]%=v; else print("MOD/0\n");
        }
        else if(strcmp(tok,"PUSH")==0) {
            if(ts_sp<TS_STACK) ts_stack[ts_sp++]=ts_val(&p); else print("Stack overflow\n");
        }
        else if(strcmp(tok,"POP")==0) {
            if(ts_sp>0){ int ri=ts_reg_idx(&p); ts_reg[ri]=ts_stack[--ts_sp]; } else print("Stack underflow\n");
        }
        else if(strcmp(tok,"CMP")==0) {
            int a=ts_val(&p); while(*p==' '||*p==',') (*p)++; int b=ts_val(&p);
            ts_cmp=(a<b)?-1:(a>b)?1:0;
        }
        else if(strcmp(tok,"GOTO")==0) {
            char lbl[32]; int li=0;
            while(*p&&*p!='\n'&&li<31) lbl[li++]=*p++;
            lbl[li]='\0';
            strtrim(lbl);
            char *target=ts_find_label(lbl);
            if(target) p=target; else { print("GOTO: label not found\n"); break; }
            continue;
        }
        else if(strcmp(tok,"JEQ")==0||strcmp(tok,"JNE")==0||strcmp(tok,"JGT")==0||
                strcmp(tok,"JLT")==0||strcmp(tok,"JGE")==0||strcmp(tok,"JLE")==0) {
            char lbl[32]; int li=0;
            while(*p&&*p!='\n'&&li<31) lbl[li++]=*p++;
            lbl[li]='\0'; strtrim(lbl);
            bool jump=false;
            if(strcmp(tok,"JEQ")==0) jump=(ts_cmp==0);
            else if(strcmp(tok,"JNE")==0) jump=(ts_cmp!=0);
            else if(strcmp(tok,"JGT")==0) jump=(ts_cmp>0);
            else if(strcmp(tok,"JLT")==0) jump=(ts_cmp<0);
            else if(strcmp(tok,"JGE")==0) jump=(ts_cmp>=0);
            else if(strcmp(tok,"JLE")==0) jump=(ts_cmp<=0);
            if(jump) {
                char *target=ts_find_label(lbl);
                if(target) { p=target; continue; }
                else { print("JMP: label not found\n"); break; }
            }
        }
        else if(strcmp(tok,"CALL")==0) {
            char lbl[32]; int li=0;
            while(*p&&*p!='\n'&&li<31) lbl[li++]=*p++;
            lbl[li]='\0'; strtrim(lbl);
            /* Skip newline and save return address */
            while(*p&&*p!='\n') p++;
            if(*p=='\n') p++;
            if(ts_callsp<TS_STACK) ts_callstack[ts_callsp++]=p;
            char *target=ts_find_label(lbl);
            if(target) { p=target; continue; }
            else { print("CALL: label not found\n"); break; }
        }
        else if(strcmp(tok,"RET")==0) {
            if(ts_callsp>0) { p=ts_callstack[--ts_callsp]; continue; }
            else { print("RET: empty call stack\n"); break; }
        }
        else if(strcmp(tok,"PRINT")==0) {
            if(*p=='"') {
                p++;
                while(*p&&*p!='"'){ rprint_char(*p); p++; }
                if(*p=='"') p++;
            } else {
                rprint_char('"'); /* should not happen */
            }
            rprint("\n");
        }
        else if(strcmp(tok,"PRINT_REG")==0) {
            int ri=ts_reg_idx(&p);
            char buf[16]; itoa(ts_reg[ri],buf);
            rprint(buf); rprint("\n");
        }
        else if(strcmp(tok,"PRINT_BUF")==0) { rprint(ts_buf); rprint("\n"); }
        else if(strcmp(tok,"INPUT")==0) { input_string(ts_buf, 128); }
        else if(strcmp(tok,"CLS")==0) { clear_screen(); }
        else if(strcmp(tok,"COLOR")==0) {
            char cn[16]; int ci=0;
            while(*p&&*p!='\n'&&ci<15) cn[ci++]=*p++;
            cn[ci]='\0'; strtrim(cn);
            if(strcmp(cn,"red")==0)    term_color=0x04;
            else if(strcmp(cn,"green")==0)  term_color=0x0A;
            else if(strcmp(cn,"cyan")==0)   term_color=0x0B;
            else if(strcmp(cn,"yellow")==0) term_color=0x0E;
            else if(strcmp(cn,"white")==0)  term_color=0x0F;
        }
        else if(strcmp(tok,"SYSINFO")==0) {
            char v[13]; get_cpu_vendor(v);
            char b[49]; get_cpu_brand(b);
            rprint("CPU: "); rprint(b); rprint("\n");
        }
        else if(strcmp(tok,"EXIT")==0) { print_col("--- Done ---\n", T_HEADER); proc_exit(pid,0); term_color=T_NORMAL; return; }

        /* Skip rest of line */
        while(*p&&*p!='\n') p++;
        if(*p=='\n') p++;
    }
    print_col("--- Done ---\n", T_HEADER);
    proc_exit(pid, 0);
    term_color=T_NORMAL;
}

/* Helper: workaround for CMP parsing above (p is passed by value in else-if chain) */
static inline void ts_advance(char **p) { while(**p==' '||**p==',') (*p)++; }

/* ─────────────────────────────────────────────────────────────────
   16. BUILT-IN APPS
   ───────────────────────────────────────────────────────────────── */
/* ---- NANO text editor ---- */
void app_nano(const char *filename) {
    File *f=fs_find(filename);
    char *buf;
    int  len=0;

    if(f) {
        if(!(f->perms&PERM_W)){ println("nano: read-only file."); return; }
        buf=f->content;
        len=strlen(buf);
    } else {
        f=fs_create(filename, "");
        if(!f){ println("nano: cannot create file."); return; }
        buf=f->content; len=0;
    }

    while(1) {
        clear_screen();
        /* Header bar */
        term_color=T_HEADER;
        print(" NANO: "); print(filename);
        int pad=60-strlen(filename); for(int i=0;i<pad;i++) print(" ");
        print("[TAB] Save  [ESC] Quit\n");
        term_color=0x08; hline('-', VGA_W);
        term_color=T_NORMAL;
        print(buf);
        /* Status bar at bottom */

        char c=get_key();
        if(c=='\t') {
            fs_write(f, buf);
            clear_screen();
            print_col("Saved.\n", T_SUCCESS);
            return;
        }
        else if(c==27) { clear_screen(); println("Quit without saving."); return; }
        else if(c=='\b') { if(len>0){ len--; buf[len]='\0'; } }
        else if(c>0 && c<0xF0 && len<FILE_SIZE-1) { buf[len++]=c; buf[len]='\0'; }
    }
}

/* ---- Calculator ---- */
void app_calc() {
    char a_s[32],b_s[32],op_s[8]; int a,b,res=0; char op;
    print_col("\n=== CALCULATOR ===\n", T_HEADER);
    print("Operand 1: "); input_string(a_s,32); a=atoi(a_s);
    print("Operand 2: "); input_string(b_s,32); b=atoi(b_s);
    print("Op [+/-/*//%/^]: "); input_string(op_s,8); op=op_s[0];
    bool ok=true;
    if(op=='+') res=a+b;
    else if(op=='-') res=a-b;
    else if(op=='*') res=a*b;
    else if(op=='/'){ if(b!=0) res=a/b; else{ print_col("Division by zero!\n",T_ERROR); ok=false; } }
    else if(op=='%'){ if(b!=0) res=a%b; else{ print_col("Division by zero!\n",T_ERROR); ok=false; } }
    else if(op=='^'){ res=1; for(int i=0;i<b;i++) res*=a; }
    else { print_col("Unknown op.\n",T_ERROR); ok=false; }
    if(ok){ char buf[32]; itoa(res,buf); print_col("= ", T_ACCENT); println(buf); }
}

/* ---- Tic Tac Toe ---- */
static char ttt[9];

void ttt_draw() {
    clear_screen();
    print_col(" TIC TAC TOE\n\n", T_HEADER);
    for(int r=0;r<3;r++){
        print("  ");
        for(int c=0;c<3;c++){
            char ch=ttt[r*3+c];
            uint8_t color=(ch=='X')?T_ACCENT:(ch=='O')?T_ERROR:T_NORMAL;
            term_color=color; print_char(ch); term_color=T_NORMAL;
            if(c<2) print(" | ");
        }
        print("\n");
        if(r<2) print("  --+---+--\n");
    }
    print("\n");
}

int ttt_win() {
    int lines[8][3]={{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    for(int i=0;i<8;i++) {
        char a=ttt[lines[i][0]], b=ttt[lines[i][1]], c=ttt[lines[i][2]];
        if(a==b&&b==c&&a!='.'&&a>='1') return 0;
        if(a==b&&b==c&&(a=='X'||a=='O')) return 1;
    }
    return 0;
}

void app_ttt() {
    for(int i=0;i<9;i++) ttt[i]='1'+i;
    char player='X'; int turns=0;
    while(turns<9) {
        ttt_draw();
        print("Player "); print_char(player); print(" > pick (1-9): ");
        char c=get_key();
        int idx=c-'1';
        if(idx>=0&&idx<9&&ttt[idx]!='X'&&ttt[idx]!='O') {
            ttt[idx]=player; turns++;
            if(ttt_win()){ ttt_draw(); print_col("Winner: ",T_SUCCESS); print_char(player); print("!\n"); return; }
            player=(player=='X')?'O':'X';
        }
    }
    ttt_draw(); print_col("Draw!\n",T_ACCENT);
}

/* ─────────────────────────────────────────────────────────────────
   17. SYSTEM INFO & POWER
   ───────────────────────────────────────────────────────────────── */
void cmd_sysinfo() {
    char v[13]; get_cpu_vendor(v);
    char b[49]; get_cpu_brand(b);
    DateTime dt; rtc_get(&dt);
    char buf[16];
    print_col("\n=== SYSTEM INFO ===\n", T_HEADER);
    print("CPU Vendor : "); println(v);
    print("CPU Brand  : "); println(b);
    print("Date       : ");
    itoa(dt.year,buf); print(buf); print("-");
    itoa(dt.month,buf); if(dt.month<10) print("0"); print(buf); print("-");
    itoa(dt.day,buf); if(dt.day<10) print("0"); print(buf); print("\n");
    print("Time       : ");
    itoa(dt.hour,buf); if(dt.hour<10) print("0"); print(buf); print(":");
    itoa(dt.min,buf); if(dt.min<10) print("0"); print(buf); print(":");
    itoa(dt.sec,buf); if(dt.sec<10) print("0"); print(buf); print("\n");
    size_t free_mem=mm_free_bytes()/1024;
    itoa(free_mem,buf); print("Free Heap  : "); print(buf); println(" KB");
    itoa(fs_count,buf); print("Files      : "); println(buf);
    itoa(current_theme,buf); print("Theme      : "); println(buf);
    print("\n");
}

void cmd_uptime() {
    /* Avoid 64-bit division (__udivdi3 unavailable in freestanding env).
       Use bit-shifts only: delta >> 31 gives approx seconds on a ~2 GHz CPU.
       delta = (hi<<32)|lo  =>  delta>>31 = (hi<<1)|(lo>>31)  — all 32-bit ops. */
    uint64_t now   = rdtsc();
    uint64_t delta = now - boot_tick;
    uint32_t hi    = (uint32_t)(delta >> 32);
    uint32_t lo    = (uint32_t)(delta);
    uint32_t secs  = (hi << 1) | (lo >> 31);   /* delta / 2^31 ~ seconds @2GHz */
    char buf[16];
    itoa(secs, buf);
    print("Uptime: ~"); print(buf); println(" seconds (TSC, ~2GHz basis)");
}

void cmd_shutdown() {
    /* ACPI S5 shutdown via QEMU/bochs port */
    clear_screen();
    print_col("System halting...\n", T_ERROR);
    /* Try ACPI via 0x604 (QEMU) */
    outw(0x604, 0x2000);
    /* Fallback: bochs/old ACPI via 0xB004 */
    outw(0xB004, 0x2000);
    /* If nothing works, halt CPU */
    __asm__ volatile("cli; hlt");
}

void cmd_reboot() {
    clear_screen();
    print_col("Rebooting...\n", T_ACCENT);
    /* PS/2 Controller reset line */
    uint8_t good=0x02;
    while(good&0x02) good=inb(0x64);
    outb(0x64, 0xFE);
    __asm__ volatile("cli; hlt");
}

/* ─────────────────────────────────────────────────────────────────
   18. THEME COMMAND
   ───────────────────────────────────────────────────────────────── */
static const char *theme_names[] = {"Classic","Amber","Matrix","Neon","Mono"};
void cmd_theme(const char *arg) {
    if(!arg||arg[0]=='\0') {
        /* List themes */
        print_col("\nThemes:\n", T_HEADER);
        for(int i=0;i<5;i++) {
            char n[4]; itoa(i,n);
            print(" "); print(n); print(": "); println(theme_names[i]);
        }
        print("Usage: theme <0-4>\n\n");
        return;
    }
    int t=atoi((char*)arg);
    if(t>=0&&t<=4){ current_theme=t; term_color=T_NORMAL;
        print_col("Theme set to ", T_SUCCESS); println(theme_names[t]);
    } else println("Invalid theme.");
}

/* ─────────────────────────────────────────────────────────────────
   19. HELP SCREEN
   ───────────────────────────────────────────────────────────────── */
/* cmd_help defined later in section 22b alongside all new commands */
void cmd_help(void);

/* ─────────────────────────────────────────────────────────────────
   20. BOOT SPLASH
   ───────────────────────────────────────────────────────────────── */
void draw_splash() {
    clear_screen();
    term_color = T_ACCENT;
    println("  ___  ___ ____   ___  ____");
    println(" |   \\/   |  _ \\ / _ \\/ ___|");
    println(" | |\\/| | | |_) | | | \\___ \\");
    println(" | |  | | |  __/| |_| |___) |");
    println(" |_|  |_|_|_|    \\___/|____/   v6.0");
    term_color = T_HEADER;
    println("\n  Multi-Purpose OS  —  Advanced Kernel");
    term_color = 0x08;
    hline('-', 45);
    term_color = T_NORMAL;

    /* Hardware detection */
    char vendor[13]; get_cpu_vendor(vendor);
    char brand[49];  get_cpu_brand(brand);
    print("  CPU: "); println(brand);
    print("  Heap: "); char hbuf[16]; itoa(HEAP_SIZE/1024,hbuf); print(hbuf); println(" KB");

    DateTime dt; rtc_get(&dt);
    char buf[8];
    print("  Date: ");
    itoa(dt.year,buf); print(buf); print("-");
    itoa(dt.month,buf); if(dt.month<10) print("0"); print(buf); print("-");
    itoa(dt.day,buf);   if(dt.day<10)   print("0"); println(buf);

    term_color=0x08; hline('-',45); term_color=T_NORMAL;
    print_col("  Type 'help' to get started.\n\n", T_SUCCESS);
}

/* ─────────────────────────────────────────────────────────────────
   21. DEMO FILES
   ───────────────────────────────────────────────────────────────── */
void load_demo_files() {
    /* C-Script demo */
    fs_create("demo.c",
        "// MPOS C-Script v2 demo\n"
        "color(cyan);\n"
        "print(\"Hello from C-Script v2!\");\n"
        "sysinfo();\n"
        "int x = 10;\n"
        "int y = 0;\n"
        "while(y < x) {\n"
        "  printv(y);\n"
        "  y = y + 1;\n"
        "}\n"
        "if(x == 10) {\n"
        "  print(\"x is ten!\");\n"
        "} else {\n"
        "  print(\"x is not ten.\");\n"
        "}\n"
        "color(white);\n"
        "wait();\n"
        "return;\n"
    );

    /* Tiny-Script demo */
    fs_create("demo.tiny",
        "# Tiny-Script v2 demo — FizzBuzz\n"
        "SET R0, 1\n"
        "SET R1, 20\n"
        "LOOP:\n"
        "  CMP R0, R1\n"
        "  JGT DONE\n"
        "  MOD R2, R0, 15   # Not valid single-op, just illustrate SET+MOD\n"
        "  SET R2, R0\n"
        "  MOD R2, 15\n"
        "  CMP R2, 0\n"
        "  JEQ FIZZBUZZ\n"
        "  SET R2, R0\n"
        "  MOD R2, 3\n"
        "  CMP R2, 0\n"
        "  JEQ FIZZ\n"
        "  SET R2, R0\n"
        "  MOD R2, 5\n"
        "  CMP R2, 0\n"
        "  JEQ BUZZ\n"
        "  PRINT_REG R0\n"
        "  ADD R0, 1\n"
        "  GOTO LOOP\n"
        "FIZZ:\n"
        "  PRINT \"Fizz\"\n"
        "  ADD R0, 1\n"
        "  GOTO LOOP\n"
        "BUZZ:\n"
        "  PRINT \"Buzz\"\n"
        "  ADD R0, 1\n"
        "  GOTO LOOP\n"
        "FIZZBUZZ:\n"
        "  PRINT \"FizzBuzz\"\n"
        "  ADD R0, 1\n"
        "  GOTO LOOP\n"
        "DONE:\n"
        "EXIT\n"
    );

    /* ── App Library ─────────────────────────────────────────── */

    fs_create("fibonacci.c",
        "color(cyan);\n"
        "print(\"=== Fibonacci Sequence ===\");\n"
        "int a = 0;\n"
        "int b = 1;\n"
        "int tmp = 0;\n"
        "int i = 0;\n"
        "while(i < 18) {\n"
        "  printv(a);\n"
        "  tmp = b;\n"
        "  b = a + b;\n"
        "  a = tmp;\n"
        "  i = i + 1;\n"
        "}\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("primes.c",
        "color(yellow);\n"
        "print(\"=== Primes up to 80 ===\");\n"
        "int n = 2;\n"
        "while(n <= 80) {\n"
        "  int is_prime = 1;\n"
        "  int d = 2;\n"
        "  while(d * d <= n) {\n"
        "    if(n % d == 0) {\n"
        "      is_prime = 0;\n"
        "    }\n"
        "    d = d + 1;\n"
        "  }\n"
        "  if(is_prime == 1) {\n"
        "    printv(n);\n"
        "  }\n"
        "  n = n + 1;\n"
        "}\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("fizzbuzz.c",
        "color(green);\n"
        "print(\"=== FizzBuzz 1-30 ===\");\n"
        "int i = 1;\n"
        "while(i <= 30) {\n"
        "  int m3 = i % 3;\n"
        "  int m5 = i % 5;\n"
        "  if(m3 == 0) {\n"
        "    if(m5 == 0) {\n"
        "      print(\"FizzBuzz\");\n"
        "    } else {\n"
        "      print(\"Fizz\");\n"
        "    }\n"
        "  } else {\n"
        "    if(m5 == 0) {\n"
        "      print(\"Buzz\");\n"
        "    } else {\n"
        "      printv(i);\n"
        "    }\n"
        "  }\n"
        "  i = i + 1;\n"
        "}\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("times.c",
        "color(cyan);\n"
        "print(\"=== Times Tables 1-10 ===\");\n"
        "int row = 1;\n"
        "while(row <= 10) {\n"
        "  printv(row * 1);\n"
        "  printv(row * 2);\n"
        "  printv(row * 3);\n"
        "  printv(row * 5);\n"
        "  printv(row * 10);\n"
        "  print(\"---\");\n"
        "  row = row + 1;\n"
        "}\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("countdown.c",
        "color(red);\n"
        "print(\"=== Countdown ===\");\n"
        "int n = 10;\n"
        "while(n > 0) {\n"
        "  printv(n);\n"
        "  n = n - 1;\n"
        "}\n"
        "color(green);\n"
        "print(\"LIFTOFF!\");\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("squares.c",
        "color(magenta);\n"
        "print(\"=== Perfect Squares ===\");\n"
        "int i = 1;\n"
        "while(i <= 15) {\n"
        "  printv(i * i);\n"
        "  i = i + 1;\n"
        "}\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("powers2.c",
        "color(blue);\n"
        "print(\"=== Powers of 2 ===\");\n"
        "int p = 1;\n"
        "int i = 0;\n"
        "while(i < 16) {\n"
        "  printv(p);\n"
        "  p = p * 2;\n"
        "  i = i + 1;\n"
        "}\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("collatz.c",
        "color(yellow);\n"
        "print(\"=== Collatz Sequence (n=27) ===\");\n"
        "int n = 27;\n"
        "int steps = 0;\n"
        "while(n != 1) {\n"
        "  printv(n);\n"
        "  if(n % 2 == 0) {\n"
        "    n = n / 2;\n"
        "  } else {\n"
        "    n = n * 3 + 1;\n"
        "  }\n"
        "  steps = steps + 1;\n"
        "}\n"
        "print(\"Reached 1 in steps:\");\n"
        "printv(steps);\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("sysinfo2.c",
        "color(green);\n"
        "print(\"=== System Information ===\");\n"
        "sysinfo();\n"
        "color(cyan);\n"
        "print(\"OS: MPOS v6.1\");\n"
        "print(\"Kernel: C freestanding\");\n"
        "print(\"Display: VGA 320x200\");\n"
        "print(\"FS: RAM 32x4KB\");\n"
        "print(\"Scripting: C-Script v2\");\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("rainbow.c",
        "print(\"=== Color Showcase ===\");\n"
        "color(red);\n"
        "print(\"Red   - danger zone\");\n"
        "color(yellow);\n"
        "print(\"Yellow - caution ahead\");\n"
        "color(green);\n"
        "print(\"Green  - all systems go\");\n"
        "color(cyan);\n"
        "print(\"Cyan   - cool and calm\");\n"
        "color(blue);\n"
        "print(\"Blue   - deep waters\");\n"
        "color(magenta);\n"
        "print(\"Magenta - electric dreams\");\n"
        "color(white);\n"
        "print(\"White  - pure light\");\n"
        "color(grey);\n"
        "print(\"Grey   - shadow mode\");\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("triangles.c",
        "color(cyan);\n"
        "print(\"=== Number Triangle ===\");\n"
        "int row = 1;\n"
        "while(row <= 9) {\n"
        "  int col = 1;\n"
        "  int line = 0;\n"
        "  while(col <= row) {\n"
        "    line = line * 10 + col;\n"
        "    col = col + 1;\n"
        "  }\n"
        "  printv(line);\n"
        "  row = row + 1;\n"
        "}\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("factors.c",
        "color(yellow);\n"
        "print(\"=== Factors of 360 ===\");\n"
        "int n = 360;\n"
        "int i = 1;\n"
        "int count = 0;\n"
        "while(i <= n) {\n"
        "  if(n % i == 0) {\n"
        "    printv(i);\n"
        "    count = count + 1;\n"
        "  }\n"
        "  i = i + 1;\n"
        "}\n"
        "print(\"Total factors:\");\n"
        "printv(count);\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("gcd.c",
        "color(green);\n"
        "print(\"=== GCD: 1071 and 462 ===\");\n"
        "int a = 1071;\n"
        "int b = 462;\n"
        "int tmp = 0;\n"
        "while(b != 0) {\n"
        "  tmp = b;\n"
        "  b = a % b;\n"
        "  a = tmp;\n"
        "}\n"
        "print(\"GCD result:\");\n"
        "printv(a);\n"
        "print(\"Verification: 1071/21 = 51, 462/21 = 22\");\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("guess.c",
        "color(cyan);\n"
        "print(\"=== Secret Number Game ===\");\n"
        "print(\"The secret number is...\");\n"
        "int secret = 42;\n"
        "int clue1 = secret / 6;\n"
        "int clue2 = secret - 40;\n"
        "print(\"Clue 1: secret / 6 =>\");\n"
        "printv(clue1);\n"
        "print(\"Clue 2: secret - 40 =>\");\n"
        "printv(clue2);\n"
        "print(\"Clue 3: it is divisible by 6 and 7\");\n"
        "print(\"Answer:\");\n"
        "printv(secret);\n"
        "color(white);\n"
        "wait();\n"
    );

    fs_create("bubblesort.c",
        "color(yellow);\n"
        "print(\"=== Bubble Sort Demo ===\");\n"
        "int a = 64;\n"
        "int b = 34;\n"
        "int c = 25;\n"
        "int d = 12;\n"
        "int e = 22;\n"
        "int f = 11;\n"
        "int g = 90;\n"
        "print(\"Before:\");\n"
        "printv(a); printv(b); printv(c); printv(d);\n"
        "printv(e); printv(f); printv(g);\n"
        "int tmp = 0;\n"
        "int pass = 0;\n"
        "while(pass < 6) {\n"
        "  if(a > b) { tmp=a; a=b; b=tmp; }\n"
        "  if(b > c) { tmp=b; b=c; c=tmp; }\n"
        "  if(c > d) { tmp=c; c=d; d=tmp; }\n"
        "  if(d > e) { tmp=d; d=e; e=tmp; }\n"
        "  if(e > f) { tmp=e; e=f; f=tmp; }\n"
        "  if(f > g) { tmp=f; f=g; g=tmp; }\n"
        "  pass = pass + 1;\n"
        "}\n"
        "print(\"After:\");\n"
        "printv(a); printv(b); printv(c); printv(d);\n"
        "printv(e); printv(f); printv(g);\n"
        "color(white);\n"
        "wait();\n"
    );

    /* Simple text file */
    fs_create("readme.txt",
        "Welcome to MPOS v6.0!\n\n"
        "This OS features:\n"
        "- RAM filesystem (32 files x 4KB)\n"
        "- C-Script v2 with variables, loops, if/else\n"
        "- Tiny-Script v2 with registers and jumps\n"
        "- Shell with history (up arrow)\n"
        "- Environment variables ($VAR)\n"
        "- Pipes: cmd1 | cmd2\n"
        "- Hex dump, grep, wc, chmod\n"
        "- Color themes (theme 0-4)\n"
        "- Real hardware detection (CPUID, RTC)\n"
    );
}

/* ─────────────────────────────────────────────────────────────────
   22. SHELL DISPATCHER
   ───────────────────────────────────────────────────────────────── */
int last_exit_code = 0;

void gui_run();
void open_apps();

/* Forward declarations for commands defined later in section 22b */
void tab_complete(char *buf, int *len);
void cmd_date(void); void cmd_df(void); void cmd_free(void); void cmd_top(void);
void cmd_whoami(void); void cmd_hostname(const char *arg); void cmd_touch(const char *name);
void cmd_head(const char *name, int n); void cmd_tail(const char *name, int n);
void cmd_find(const char *pat); void cmd_mkdir(const char *name);
void cmd_cd(const char *arg); void cmd_pwd(void); void cmd_passwd(void);
void cmd_motd(void); void cmd_man(const char *topic); void cmd_help(void);

void dispatch(char *raw_input) {
    /* Expand environment variables */
    char expanded[HIST_LEN];
    env_expand(raw_input, expanded, HIST_LEN);

    /* Check for pipe: split at | */
    char *pipe_pos = strchr(expanded, '|');
    if(pipe_pos) {
        *pipe_pos = '\0';
        char *cmd1 = strtrim(expanded);
        char *cmd2 = strtrim(pipe_pos+1);
        /* Run cmd1 capturing output in pipe_buf */
        pipe_clear(); pipe_mode=true;
        dispatch(cmd1);
        pipe_mode=false;
        /* Make pipe_buf available as a virtual file "__pipe__" */
        File *pf=fs_find("__pipe__");
        if(!pf) pf=fs_create("__pipe__",NULL);
        if(pf) fs_write(pf, pipe_buf);
        /* Replace any <stdin> token in cmd2 with __pipe__ */
        /* For simplicity, append __pipe__ as last argument to cmd2 if it has no filename arg */
        char cmd2_full[HIST_LEN*2];
        strcpy(cmd2_full, cmd2);
        strcat(cmd2_full, " __pipe__");
        dispatch(cmd2_full);
        fs_rm("__pipe__");
        return;
    }

    char *cmd = strtrim(expanded);
    if(!cmd||cmd[0]=='\0') return;

    /* Parse args */
    char cmd_copy[HIST_LEN]; strncpy(cmd_copy, cmd, HIST_LEN);
    char *argv[MAX_ARGS]; int argc=split_args(cmd_copy, argv, MAX_ARGS);
    if(argc==0) return;
    char *verb=argv[0];

    /* ---- BUILT-IN COMMANDS ---- */
    if(strcmp(verb,"help")==0)    { cmd_help(); last_exit_code=0; }
    else if(strcmp(verb,"cls")==0||strcmp(verb,"clear")==0) {
        clear_screen();
        print_col("MPOS v6.0\n", T_ACCENT);
        last_exit_code=0;
    }
    else if(strcmp(verb,"ls")==0) {
        bool lng=(argc>1&&strcmp(argv[1],"-l")==0);
        cmd_ls(lng); last_exit_code=0;
    }
    else if(strcmp(verb,"cat")==0) {
        if(argc<2){ println("Usage: cat <file>"); last_exit_code=1; }
        else { cmd_cat(argv[1]); last_exit_code=0; }
    }
    else if(strcmp(verb,"nano")==0) {
        if(argc<2){ println("Usage: nano <file>"); last_exit_code=1; }
        else { app_nano(argv[1]); last_exit_code=0; }
    }
    else if(strcmp(verb,"rm")==0) {
        if(argc<2){ println("Usage: rm <file>"); last_exit_code=1; }
        else { fs_rm(argv[1]); last_exit_code=0; }
    }
    else if(strcmp(verb,"cp")==0) {
        if(argc<3){ println("Usage: cp <src> <dst>"); last_exit_code=1; }
        else { cmd_cp(argv[1],argv[2]); last_exit_code=0; }
    }
    else if(strcmp(verb,"mv")==0) {
        if(argc<3){ println("Usage: mv <src> <dst>"); last_exit_code=1; }
        else { cmd_mv(argv[1],argv[2]); last_exit_code=0; }
    }
    else if(strcmp(verb,"chmod")==0) {
        if(argc<3){ println("Usage: chmod <perms> <file>"); last_exit_code=1; }
        else { cmd_chmod(argv[1],argv[2]); last_exit_code=0; }
    }
    else if(strcmp(verb,"wc")==0) {
        if(argc<2){ println("Usage: wc <file>"); last_exit_code=1; }
        else { cmd_wc(argv[1]); last_exit_code=0; }
    }
    else if(strcmp(verb,"grep")==0) {
        if(argc<3){ println("Usage: grep <pattern> <file>"); last_exit_code=1; }
        else { cmd_grep(argv[1],argv[2]); last_exit_code=0; }
    }
    else if(strcmp(verb,"hexdump")==0) {
        if(argc<2){ println("Usage: hexdump <file>"); last_exit_code=1; }
        else { cmd_hexdump(argv[1]); last_exit_code=0; }
    }
    else if(strcmp(verb,"ps")==0)       { cmd_ps(); last_exit_code=0; }
    else if(strcmp(verb,"kill")==0) {
        if(argc<2){ println("Usage: kill <pid>"); last_exit_code=1; }
        else { proc_kill(atoi(argv[1])); last_exit_code=0; }
    }
    else if(strcmp(verb,"echo")==0) {
        for(int i=1;i<argc;i++){ rprint(argv[i]); if(i<argc-1) rprint(" "); }
        rprint("\n"); last_exit_code=0;
    }
    else if(strcmp(verb,"env")==0) {
        print_col("\nEnvironment:\n", T_HEADER);
        for(int i=0;i<env_count;i++){ print("  "); print(env[i].key); print("="); println(env[i].val); }
        if(env_count==0) println("  (empty)");
        print("\n"); last_exit_code=0;
    }
    else if(strncmp(verb,"set",3)==0&&argc>=2) {
        /* set VAR=val */
        char *eq=strchr(argv[1],'=');
        if(eq){ *eq='\0'; env_set(argv[1],eq+1); last_exit_code=0; }
        else println("Usage: set VAR=value");
    }
    else if(strcmp(verb,"history")==0) {
        print_col("\nHistory:\n", T_HEADER);
        int start=hist_count>10?hist_count-10:0;
        for(int i=start;i<hist_count;i++){
            char n[8]; itoa(i+1,n);
            print("  "); print(n); print(": ");
            println(hist[i%HIST_SIZE]);
        }
        print("\n"); last_exit_code=0;
    }
    else if(strcmp(verb,"sysinfo")==0)  { cmd_sysinfo(); last_exit_code=0; }
    else if(strcmp(verb,"date")==0)     { cmd_date(); last_exit_code=0; }
    else if(strcmp(verb,"df")==0)       { cmd_df(); last_exit_code=0; }
    else if(strcmp(verb,"free")==0)     { cmd_free(); last_exit_code=0; }
    else if(strcmp(verb,"top")==0)      { cmd_top(); last_exit_code=0; }
    else if(strcmp(verb,"whoami")==0)   { cmd_whoami(); last_exit_code=0; }
    else if(strcmp(verb,"hostname")==0) { cmd_hostname(argc>1?argv[1]:NULL); last_exit_code=0; }
    else if(strcmp(verb,"touch")==0)    { if(argc>1)cmd_touch(argv[1]); last_exit_code=0; }
    else if(strcmp(verb,"head")==0)     { if(argc>1)cmd_head(argv[1],argc>2?atoi(argv[2]):10); last_exit_code=0; }
    else if(strcmp(verb,"tail")==0)     { if(argc>1)cmd_tail(argv[1],argc>2?atoi(argv[2]):10); last_exit_code=0; }
    else if(strcmp(verb,"find")==0)     { cmd_find(argc>1?argv[1]:NULL); last_exit_code=0; }
    else if(strcmp(verb,"mkdir")==0)    { if(argc>1)cmd_mkdir(argv[1]); last_exit_code=0; }
    else if(strcmp(verb,"cd")==0)       { cmd_cd(argc>1?argv[1]:NULL); last_exit_code=0; }
    else if(strcmp(verb,"pwd")==0)      { cmd_pwd(); last_exit_code=0; }
    else if(strcmp(verb,"passwd")==0)   { cmd_passwd(); last_exit_code=0; }
    else if(strcmp(verb,"motd")==0)     { cmd_motd(); last_exit_code=0; }
    else if(strcmp(verb,"man")==0)      { cmd_man(argc>1?argv[1]:NULL); last_exit_code=0; }
    else if(strcmp(verb,"uptime")==0)   { cmd_uptime(); last_exit_code=0; }
    else if(strcmp(verb,"theme")==0)    { cmd_theme(argc>1?argv[1]:NULL); last_exit_code=0; }
    else if(strcmp(verb,"calc")==0)     { app_calc(); last_exit_code=0; }
    else if(strcmp(verb,"ttt")==0)      { app_ttt(); last_exit_code=0; }
    else if(strcmp(verb,"shutdown")==0) { cmd_shutdown(); }
    else if(strcmp(verb,"reboot")==0)   { cmd_reboot(); }
    else if(strcmp(verb,"run")==0) {
        if(argc<2){ println("Usage: run <file>"); last_exit_code=1; return; }
        File *f=fs_find(argv[1]);
        if(!f){ println("run: file not found."); last_exit_code=1; return; }
        if(!(f->perms&PERM_X)){ println("run: not executable (chmod x)."); last_exit_code=1; return; }
        int nl=strlen(argv[1]);
        if(nl>=2 && argv[1][nl-2]=='.' && argv[1][nl-1]=='c')
            run_c_script_named(f);
        else if(nl>=5 && strncmp(argv[1]+nl-5,".tiny",5)==0)
            run_tiny_script(f->content);
        else println("run: unknown file type (expected .c or .tiny).");
        last_exit_code=0;
    }
    else if(strcmp(verb,"gui")==0) {
        /* Launch graphical desktop */
        gui_run();
        last_exit_code=0;
    }
    else {
        print_col("Unknown command: ", T_ERROR); println(verb);
        println("Type 'help' for commands.");
        last_exit_code=127;
    }
}

/* ─────────────────────────────────────────────────────────────────
   23. KERNEL ENTRY POINT
   ───────────────────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────
   22b. BOOT SEQUENCE, LOGIN, AND NEW COMMANDS
   ───────────────────────────────────────────────────────────────── */

/* Read a password — prints * for each character */
void input_password(char *buf, int max) {
    int i=0; buf[0]='\0';
    while(1){
        char c=get_key();
        if(c=='\n'){ buf[i]='\0'; term_putc('\n'); return; }
        if(c=='\b'&&i>0){ i--; buf[i]='\0'; term_col--; term_putc(' '); term_col--; }
        else if(c>=' '&&c<127&&i<max-1){ buf[i++]=c; buf[i]='\0'; term_putc('*'); }
    }
}

/* Tab completion helper — called from input_string on TAB key */
static const char *cmd_list[] = {
    "ls","cat","echo","rm","cp","mv","mkdir","cd","pwd","touch",
    "nano","run","grep","wc","hexdump","chmod","find","head","tail",
    "ps","kill","date","df","free","top","whoami","hostname","passwd",
    "man","history","env","set","sysinfo","uptime","calc","ttt",
    "theme","clear","cls","gui","reboot","shutdown","motd",NULL
};

void tab_complete(char *buf, int *len) {
    if(*len==0) return;
    /* Find start of last word */
    char *word=buf;
    for(int i=0;i<*len;i++) if(buf[i]==' ') word=buf+i+1;
    int wlen=(int)(buf+*len-word);
    if(wlen==0) return;
    bool after_space=(word!=buf);

    /* Collect matches */
    const char *matches[32]; int nm=0;
    if(!after_space){
        /* Complete command */
        for(int i=0;cmd_list[i]&&nm<32;i++)
            if(strncmp(cmd_list[i],word,wlen)==0) matches[nm++]=cmd_list[i];
    } else {
        /* Complete filename */
        for(int i=0;i<MAX_FILES&&nm<32;i++)
            if(fs[i].active&&strncmp(fs[i].name,word,wlen)==0) matches[nm++]=fs[i].name;
    }
    if(nm==0) return;
    if(nm==1){
        /* Unique match — complete it */
        const char *m=matches[0];
        int ml=strlen(m);
        for(int i=wlen;i<ml;i++){
            buf[*len]=m[i]; (*len)++; term_putc(m[i]);
        }
        buf[*len]=' '; (*len)++; term_putc(' ');
        buf[*len]='\0';
    } else {
        /* Show all matches on new line */
        term_putc('\n');
        for(int i=0;i<nm;i++){ print(matches[i]); print("  "); }
        term_putc('\n');
        /* Reprint prompt + current input */
        print_col(current_user, T_SUCCESS); print("@");
        print_col(sys_hostname, T_ACCENT); print(":");
        print_col(cwd, T_HEADER); print(is_root?"# ":"$ ");
        print(buf);
    }
}

/* ── Boot message sequence ── */
void boot_messages(void) {
    clear_screen();
    term_color=0x0A;
    println("MPOS v6.1 — Multi-Purpose Operating System");
    term_color=0x07;
    println("──────────────────────────────────────────");

    /* CPU detection */
    char vendor[13], brand[49];
    get_cpu_vendor(vendor); get_cpu_brand(brand);
    print_col("[  OK  ] ", 0x0A); print("CPU: "); print(vendor); print(" — "); println(brand);

    /* Memory */
    char mbuf[16]; itoa(HEAP_SIZE/1024, mbuf);
    print_col("[  OK  ] ", 0x0A); print("Memory: "); print(mbuf); println(" KB heap initialised");

    /* Filesystem */
    print_col("[  OK  ] ", 0x0A); println("RAM filesystem mounted (32 files × 4 KB)");

    /* RTC */
    DateTime dt; rtc_get(&dt);
    char yr[8],mo[4],dy[4],hr[4],mn[4],sc[4];
    itoa(dt.year,yr); itoa(dt.month,mo); itoa(dt.day,dy);
    itoa(dt.hour,hr); itoa(dt.min,mn);   itoa(dt.sec,sc);
    print_col("[  OK  ] ", 0x0A);
    print("RTC: "); print(yr); print("-");
    if(dt.month<10)print("0"); print(mo); print("-");
    if(dt.day<10)print("0");   print(dy); print("  ");
    if(dt.hour<10)print("0");  print(hr); print(":");
    if(dt.min<10)print("0");   print(mn); print(":");
    if(dt.sec<10)print("0");   println(sc);

    /* Keyboard */
    print_col("[  OK  ] ", 0x0A); println("PS/2 keyboard driver loaded");

    /* VGA */
    print_col("[  OK  ] ", 0x0A); println("VGA 80×25 text mode active");

    print_col("[  OK  ] ", 0x0A); println("All systems nominal");
    println("");
    term_color=0x0F;
    println("MPOS v6.1 (c) 2025  Type 'help' for commands, 'gui' for desktop");
    println("");
}

/* ── Login screen ── */
bool do_login(void) {
    char uname[16], upass[32];
    for(int tries=0;tries<3;tries++){
        term_color=0x0F; print("login: "); term_color=0x0A;
        input_string(uname,sizeof(uname));
        term_color=0x0F; print("password: ");
        input_password(upass,sizeof(upass));
        for(int i=0;i<NUM_USERS;i++){
            if(strcmp(users[i].name,uname)==0 && strcmp(users[i].pass,upass)==0){
                logged_in_user=i;
                strncpy(current_user,users[i].name,sizeof(current_user)-1);
                is_root=users[i].is_root;
                /* Set cwd */
                if(is_root) strncpy(cwd,"/root",sizeof(cwd)-1);
                else{ strncpy(cwd,"/home/",sizeof(cwd)-1); strcat(cwd,current_user); }
                env_set("USER",current_user);
                env_set("HOME",cwd);
                return true;
            }
        }
        term_color=0x0C; println("Login incorrect.\n"); term_color=0x0F;
    }
    return false;
}

/* ── New commands ── */

void cmd_date(void){
    DateTime dt; rtc_get(&dt);
    char y[8],mo[4],d[4],h[4],mn[4],s[4];
    itoa(dt.year,y);itoa(dt.month,mo);itoa(dt.day,d);
    itoa(dt.hour,h);itoa(dt.min,mn);itoa(dt.sec,s);
    print(y); print("-");
    if(dt.month<10)print("0"); print(mo); print("-");
    if(dt.day<10)print("0");   print(d);  print("  ");
    if(dt.hour<10)print("0");  print(h);  print(":");
    if(dt.min<10)print("0");   print(mn); print(":");
    if(dt.sec<10)print("0");   print(s);
    print("\n");
}

void cmd_df(void){
    print_col("\n Filesystem      Files    Used\n",T_HEADER);
    hline('-',34);
    char buf[12];
    int used=0; for(int i=0;i<MAX_FILES;i++) if(fs[i].active) used++;
    print(" ramfs        "); itoa(MAX_FILES,buf); print("   "); print(buf); print("    ");
    itoa(used,buf); print(buf); print("\n\n");
}

void cmd_free(void){
    size_t total=HEAP_SIZE/1024;
    size_t free_k=mm_free_bytes()/1024;
    size_t used_k=total-free_k;
    char b[16];
    print_col("\n              total    used    free\n",T_HEADER);
    hline('-',38);
    print(" Mem:      ");
    itoa(total,b); int l=strlen(b); for(int i=l;i<8;i++)print(" "); print(b); print(" KB  ");
    itoa(used_k,b); l=strlen(b); for(int i=l;i<6;i++)print(" "); print(b); print(" KB  ");
    itoa(free_k,b); print(b); println(" KB");
    print("\n");
}

void cmd_top(void){
    println("PROCESSES  (press any key to exit)");
    hline('-',40);
    uint64_t now=rdtsc();
    for(int i=0;i<MAX_PROCS;i++){
        if(proc_table[i].state==PROC_FREE) continue;
        char pid[8]; itoa(proc_table[i].pid,pid);
        uint64_t d=now-proc_table[i].start_tick;
        uint32_t hi=(uint32_t)(d>>32),lo=(uint32_t)d;
        uint32_t secs=(hi<<1)|(lo>>31);
        char st[8]; itoa(secs,st);
        int pl=strlen(pid); print(" "); for(int j=pl;j<4;j++)print(" ");
        print(pid); print("  ");
        print_col(proc_table[i].state==PROC_RUNNING?"RUN ":"ZMB ",
                  proc_table[i].state==PROC_RUNNING?T_SUCCESS:T_ERROR);
        print(st); print("s  ");
        println(proc_table[i].name);
    }
    get_key();
}

void cmd_whoami(void){ println(current_user); }

void cmd_hostname(const char *arg){
    if(!arg||!arg[0]){ println(sys_hostname); return; }
    if(!is_root){ println("hostname: permission denied (not root)"); return; }
    strncpy(sys_hostname,arg,sizeof(sys_hostname)-1);
    env_set("HOSTNAME",sys_hostname);
    println("Hostname updated.");
}

void cmd_touch(const char *name){
    if(!name||!name[0]){ println("Usage: touch <file>"); return; }
    if(fs_find(name)){ return; }   /* already exists — update would go here */
    if(!fs_create(name,NULL)) println("touch: cannot create file.");
}

void cmd_head(const char *name, int n){
    File *f=fs_find(name);
    if(!f){ println("head: file not found."); return; }
    char *p=f->content; int lines=0;
    while(*p&&lines<n){
        if(*p=='\n') lines++;
        term_putc(*p++);
    }
    if(*(p-1)!='\n') term_putc('\n');
}

void cmd_tail(const char *name, int n){
    File *f=fs_find(name);
    if(!f){ println("tail: file not found."); return; }
    int total=0; char *p=f->content;
    while(*p){ if(*p=='\n') total++; p++; }
    int skip=total-n; if(skip<0)skip=0;
    p=f->content; int skipped=0;
    while(*p&&skipped<skip){ if(*p=='\n')skipped++; p++; }
    print(p); if(p[strlen(p)-1]!='\n') print("\n");
}

void cmd_find(const char *pat){
    bool found=false;
    for(int i=0;i<MAX_FILES;i++){
        if(!fs[i].active) continue;
        if(!pat||pat[0]=='\0'||strstr(fs[i].name,pat)){
            print("  "); println(fs[i].name); found=true;
        }
    }
    if(!found) println("(no files found)");
}

void cmd_mkdir(const char *name){
    if(!name||!name[0]){println("Usage: mkdir <dir>");return;}
    char dname[36]; strncpy(dname,name,32); strcat(dname,"/");
    if(fs_find(dname)){ println("mkdir: already exists."); return; }
    File *f=fs_create(dname,"");
    if(f){ f->perms=PERM_R|PERM_X; println("Directory created."); }
    else println("mkdir: failed.");
}

void cmd_cd(const char *arg){
    if(!arg||!arg[0]||strcmp(arg,"~")==0||strcmp(arg,"--")==0){
        /* home */
        if(is_root) strncpy(cwd,"/root",sizeof(cwd)-1);
        else{ strncpy(cwd,"/home/",sizeof(cwd)-1); strcat(cwd,current_user); }
        return;
    }
    if(strcmp(arg,"/")==0){ strncpy(cwd,"/",sizeof(cwd)-1); return; }
    if(strcmp(arg,"..")==0){
        char *last=strrchr(cwd,'/');
        if(last&&last!=cwd) *last='\0';
        else strncpy(cwd,"/",sizeof(cwd)-1);
        return;
    }
    /* Check if directory marker exists */
    char dname[36]; strncpy(dname,arg,32); strcat(dname,"/");
    if(!fs_find(dname)&&strcmp(arg,"/")!=0){
        println("cd: no such directory."); return;
    }
    if(arg[0]=='/') strncpy(cwd,arg,sizeof(cwd)-1);
    else{ strcat(cwd,"/"); strncat(cwd,arg,sizeof(cwd)-strlen(cwd)-1); }
}

void cmd_pwd(void){ println(cwd); }

void cmd_passwd(void){
    if(logged_in_user<0) return;
    char p1[32],p2[32];
    print("New password: ");   input_password(p1,sizeof(p1));
    print("Retype password: "); input_password(p2,sizeof(p2));
    if(strcmp(p1,p2)!=0){ println("Passwords do not match."); return; }
    strncpy(users[logged_in_user].pass,p1,sizeof(users[0].pass)-1);
    println("Password updated.");
}

void cmd_motd(void){
    File *f=fs_find("motd");
    if(f) println(f->content);
}

/* ── man pages ── */
static const struct { const char *cmd; const char *text; } mandb[]={
    {"ls",    "ls [-l]\n  List files. -l shows size and permissions."},
    {"cat",   "cat <file>\n  Print file contents."},
    {"echo",  "echo <text> [> file]\n  Print text or write to file."},
    {"rm",    "rm <file>\n  Delete a file."},
    {"cp",    "cp <src> <dst>\n  Copy a file."},
    {"mv",    "mv <src> <dst>\n  Move/rename a file."},
    {"mkdir", "mkdir <name>\n  Create a directory."},
    {"cd",    "cd [dir]\n  Change directory. cd alone goes home."},
    {"pwd",   "pwd\n  Print current directory."},
    {"touch", "touch <file>\n  Create empty file."},
    {"find",  "find [pattern]\n  Search for files matching pattern."},
    {"head",  "head <file> [n]\n  Show first n lines (default 10)."},
    {"tail",  "tail <file> [n]\n  Show last n lines (default 10)."},
    {"grep",  "grep <pattern> <file>\n  Search for pattern in file."},
    {"wc",    "wc <file>\n  Count lines, words, bytes."},
    {"hexdump","hexdump <file>\n  Show file contents in hex."},
    {"chmod", "chmod <rwx> <file>\n  Change file permissions."},
    {"nano",  "nano [file]\n  Text editor. ESC=save, Ctrl+Q=quit."},
    {"run",   "run <file.c|file.tiny>\n  Execute a C-Script or Tiny-Script."},
    {"ps",    "ps\n  List processes with uptime."},
    {"kill",  "kill <pid>\n  Terminate a process."},
    {"date",  "date\n  Show current date and time."},
    {"df",    "df\n  Show filesystem usage."},
    {"free",  "free\n  Show memory usage."},
    {"top",   "top\n  Show live process list. Any key to exit."},
    {"whoami","whoami\n  Show current username."},
    {"hostname","hostname [name]\n  Show or set hostname (root only)."},
    {"passwd","passwd\n  Change your password."},
    {"uptime","uptime\n  Show system uptime."},
    {"sysinfo","sysinfo\n  CPU and system information."},
    {"history","history\n  Show command history."},
    {"env",   "env\n  Show environment variables."},
    {"set",   "set VAR=value\n  Set environment variable."},
    {"theme", "theme [0-4]\n  Change colour theme."},
    {"gui",   "gui\n  Launch graphical desktop (Mode 13h)."},
    {"motd",  "motd\n  Show message of the day."},
    {"reboot","reboot\n  Restart the system."},
    {"shutdown","shutdown\n  Power off the system."},
};
#define MANDB_SIZE ((int)(sizeof(mandb)/sizeof(mandb[0])))

void cmd_man(const char *topic){
    if(!topic||!topic[0]){
        println("Usage: man <command>");
        println("Available topics:");
        for(int i=0;i<MANDB_SIZE;i++){
            print("  "); print(mandb[i].cmd);
            if(i%4==3) print("\n"); else print("\t");
        }
        print("\n"); return;
    }
    for(int i=0;i<MANDB_SIZE;i++){
        if(strcmp(mandb[i].cmd,topic)==0){
            print_col(mandb[i].cmd,T_HEADER); print_col(" — ",T_HEADER);
            println(""); println(mandb[i].text); println(""); return;
        }
    }
    print("man: no manual entry for '"); print(topic); println("'.");
}

/* ── Updated help ── */
void cmd_help(void){
    print_col("\nMPOS v6.1 Command Reference\n",T_HEADER);
    hline('-',40);
    print_col("Files:     ",T_ACCENT);
    println("ls  cat  echo  rm  cp  mv  touch  find  head  tail  chmod  nano");
    print_col("Dirs:      ",T_ACCENT);
    println("cd  pwd  mkdir");
    print_col("View:      ",T_ACCENT);
    println("grep  wc  hexdump");
    print_col("System:    ",T_ACCENT);
    println("ps  kill  top  free  df  date  uptime  sysinfo  hostname  whoami");
    print_col("User:      ",T_ACCENT);
    println("passwd  motd  env  set  history");
    print_col("Scripts:   ",T_ACCENT);
    println("run <file.c>   run <file.tiny>   calc   ttt");
    print_col("Settings:  ",T_ACCENT);
    println("theme [0-4]");
    print_col("Desktop:   ",T_ACCENT);
    println("gui");
    print_col("Power:     ",T_ACCENT);
    println("reboot  shutdown");
    println("\nType 'man <command>' for detailed help on any command.");
    println("");
}

void kernel_main_text() {
    disable_cursor();
    mm_init();
    fs_init();
    proc_init();

    /* Default environment */
    env_set("OS",      "MPOS");
    env_set("VER",     "6.1");
    env_set("HOSTNAME",sys_hostname);

    load_demo_files();

    /* Create default directory structure */
    cmd_mkdir("home");
    cmd_mkdir("home/root");
    cmd_mkdir("home/guest");
    cmd_mkdir("etc");
    cmd_mkdir("tmp");

    /* /etc/motd */
    fs_create("motd",
        "Welcome to MPOS v6.1!\n"
        "Type 'help' for commands, 'man <cmd>' for details.\n"
        "Type 'gui' to launch the graphical desktop.\n"
    );

    /* Boot sequence */
    boot_messages();

    /* Login */
    while(!do_login()){
        term_color=0x0C;
        println("Maximum login attempts exceeded.");
        __asm__ volatile("cli; hlt");
    }

    /* Message of the day */
    term_color=T_SUCCESS;
    print("\nWelcome, "); print(current_user); println("!\n");
    term_color=T_NORMAL;
    cmd_motd();
    println("");

    char input[HIST_LEN];
    while(1) {
        /* Rich prompt: user@hostname:cwd$ */
        if(last_exit_code!=0){
            term_color=T_ERROR; print("[");
            char ec[8]; itoa(last_exit_code,ec); print(ec);
            print("] "); term_color=T_NORMAL;
        }
        print_col(current_user,T_SUCCESS);
        print("@"); print_col(sys_hostname,T_ACCENT);
        print(":"); print_col(cwd,T_HEADER);
        print(is_root?"# ":"$ ");
        term_color=T_NORMAL;

        input_string(input,HIST_LEN);
        if(input[0]!='\0') hist_push(input);
        dispatch(input);
    }
}


/* ─────────────────────────────────────────────────────────────────────────────
   GUI LAYER  (appended)
   ───────────────────────────────────────────────────────────────────────────── */

/* ═══════════════════════════════════════════════════════════════════════════
   MPOS GUI SUBSYSTEM  —  VGA Mode 13h  (320×200, 256 colours)
   ═══════════════════════════════════════════════════════════════════════════

   Sections:
     G1.  VGA Mode 13h driver + palette
     G2.  8×8 bitmap font (ASCII 32-127)
     G3.  Pixel/rect/text drawing primitives
     G4.  PS/2 mouse driver
     G5.  Window manager (8 windows, drag, z-order, close)
     G6.  Desktop renderer (wallpaper, taskbar, icons)
     G7.  Built-in app windows: Terminal, File Manager, Calculator, About
     G8.  GUI main loop
     G9.  'gui' shell command hook + kernel_main patch
*/

/* ───────────────────────────────────────────────────────────────────────────
   G1. VGA MODE 13h DRIVER
   ─────────────────────────────────────────────────────────────────────────── */
#define VGA_GFX_BASE  ((uint8_t*)0xA0000)
#define GFX_W  320
#define GFX_H  200

static uint8_t gui_backbuffer[64000]; // Our "hidden" screen



/* EGA/VGA register sets needed for mode-set */
static const uint8_t mode13_regs[] = {
    /* MISC */          0x63,
    /* SEQ  */          0x03,0x01,0x0F,0x00,0x0E,
    /* CRTC (25 regs)*/ 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
                        0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
                        0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF,
    /* GC   (9 regs) */ 0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF,
    /* AC   (21 regs)*/ 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
                        0x41,0x00,0x0F,0x00,0x00
};

void vga_write_regs() {
    const uint8_t *r = mode13_regs;
    /* Misc output */
    outb(0x3C2, *r++);
    /* Sequencer */
    for(uint8_t i=0;i<5;i++){ outb(0x3C4,i); outb(0x3C5,*r++); }
    /* CRTC unlock */
    outb(0x3D4,0x03); outb(0x3D5, inb(0x3D5)|0x80);
    outb(0x3D4,0x11); outb(0x3D5, inb(0x3D5)&~0x80);
    /* CRTC */
    for(uint8_t i=0;i<25;i++){ outb(0x3D4,i); outb(0x3D5,*r++); }
    /* Graphics controller */
    for(uint8_t i=0;i<9;i++){ outb(0x3CE,i); outb(0x3CF,*r++); }
    /* Attribute controller */
    inb(0x3DA);
    for(uint8_t i=0;i<21;i++){
        outb(0x3C0,i); outb(0x3C0,*r++);
    }
    outb(0x3C0,0x20);
}

/* ---- Standard 256-colour palette (first 16 = CGA, rest = 6-6-6 cube + greys) ---- */
typedef struct { uint8_t r,g,b; } RGB;

static const RGB cga16[16] = {
    {0,0,0},{0,0,170},{0,170,0},{0,170,170},
    {170,0,0},{170,0,170},{170,85,0},{170,170,170},
    {85,85,85},{85,85,255},{85,255,85},{85,255,255},
    {255,85,85},{255,85,255},{255,255,85},{255,255,255}
};

void vga_set_palette() {
    outb(0x3C8, 0);
    /* Set the standard 16 colors */
    for(int i=0; i<16; i++){
        outb(0x3C9, cga16[i].r >> 2);
        outb(0x3C9, cga16[i].g >> 2);
        outb(0x3C9, cga16[i].b >> 2);
    }
    /* 16-231: 6×6×6 RGB cube */
    for(int r=0;r<6;r++)
    for(int g=0;g<6;g++)
    for(int b=0;b<6;b++){
        outb(0x3C9,(r*51)>>2);
        outb(0x3C9,(g*51)>>2);
        outb(0x3C9,(b*51)>>2);
    }
    /* 232-255: 24 greyscale steps */
    for(int i=0;i<24;i++){
        uint8_t v=((i*10)+8)>>2;
        outb(0x3C9,v); outb(0x3C9,v); outb(0x3C9,v);
    }
}

/* Map approximate RGB to nearest palette index */
uint8_t rgb_to_pal(uint8_t r, uint8_t g, uint8_t b) {
    /* Use 6×6×6 cube (indices 16-231) */
    uint8_t ri=r/43, gi=g/43, bi=b/43;
    if(ri>5)ri=5; if(gi>5)gi=5; if(bi>5)bi=5;
    return (uint8_t)(16 + ri*36 + gi*6 + bi);
}

/* Predefined colour constants (palette indices) */
#define C_BLACK     0
#define C_DKBLUE    1
#define C_DKGREEN   2
#define C_DKCYAN    3
#define C_DKRED     4
#define C_DKMAGENTA 5
#define C_BROWN     6
#define C_LTGREY    7
#define C_DKGREY    8
#define C_BLUE      9
#define C_GREEN     10
#define C_CYAN      11
#define C_RED       12
#define C_MAGENTA   13
#define C_YELLOW    14
#define C_WHITE     15
/* Convenience palette shortcuts */
#define C_NAVY      rgb_to_pal(0,0,100)
#define C_TEAL      rgb_to_pal(0,120,120)
#define C_ORANGE    rgb_to_pal(255,140,0)
#define C_PINK      rgb_to_pal(255,100,180)
#define C_INDIGO    rgb_to_pal(60,0,180)
#define C_LIME      rgb_to_pal(100,220,0)
#define C_GOLD      rgb_to_pal(200,170,0)
#define C_SILVER    rgb_to_pal(180,180,180)
#define C_TITLEBAR  rgb_to_pal(30,60,140)
#define C_TITLEACT  rgb_to_pal(0,90,200)
#define C_WINBG     rgb_to_pal(220,220,220)
#define C_WINBDR    rgb_to_pal(80,80,80)
#define C_DESKTOP   rgb_to_pal(30,100,160)
#define C_TASKBAR   rgb_to_pal(20,20,60)
#define C_BTNFACE   rgb_to_pal(190,190,200)
#define C_BTNSHADOW rgb_to_pal(100,100,110)
#define C_BTNHI     rgb_to_pal(240,240,255)
#define C_TXTDARK   rgb_to_pal(20,20,20)
#define C_TXTLIGHT  C_WHITE
#define C_CURSOR    C_WHITE
#define C_SELBG     rgb_to_pal(0,80,200)

void gfx_enter_mode13() {
    vga_write_regs();
    vga_set_palette();
}

/* Return to VGA text mode 3 */
void gfx_exit_mode13() {
    /* INT 10h AX=0003 — we can't use BIOS interrupts from kernel, so
       reprogram VGA registers back to text mode 3 manually via BIOS data.
       Simplest reliable approach: triple-fault reboot and let BIOS reinit.
       For QEMU we can do a soft reset: */
    outb(0x64, 0xFE);   /* pulse reset line */
    while(1) __asm__("hlt");
}

/* ───────────────────────────────────────────────────────────────────────────
   G2. 8×8 BITMAP FONT  (printable ASCII 32-127)
       Each character is 8 bytes, one per row, MSB = leftmost pixel.
   ─────────────────────────────────────────────────────────────────────────── */
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 32 SPC */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 33 !   */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 34 "   */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 35 #   */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 36 $   */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 37 %   */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 38 &   */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 39 '   */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 40 (   */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 41 )   */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 42 *   */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 43 +   */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 44 ,   */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 45 -   */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 46 .   */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 47 /   */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 48 0   */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 49 1   */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 50 2   */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 51 3   */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 52 4   */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 53 5   */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 54 6   */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 55 7   */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 56 8   */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 57 9   */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 58 :   */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 59 ;   */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 60 <   */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 61 =   */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 62 >   */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 63 ?   */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 64 @   */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 65 A   */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 66 B   */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 67 C   */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 68 D   */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 69 E   */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 70 F   */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 71 G   */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 72 H   */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 73 I   */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 74 J   */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 75 K   */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 76 L   */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 77 M   */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 78 N   */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 79 O   */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 80 P   */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 81 Q   */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 82 R   */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 83 S   */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 84 T   */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 85 U   */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 86 V   */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 87 W   */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 88 X   */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 89 Y   */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 90 Z   */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 91 [   */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 92 \   */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 93 ]   */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 94 ^   */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 95 _   */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* 96 `   */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 97 a   */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 98 b   */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 99 c   */
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, /* 100 d  */
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, /* 101 e  */
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, /* 102 f  */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* 103 g  */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* 104 h  */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* 105 i  */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* 106 j  */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* 107 k  */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 108 l  */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* 109 m  */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* 110 n  */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* 111 o  */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* 112 p  */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* 113 q  */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* 114 r  */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* 115 s  */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* 116 t  */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* 117 u  */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 118 v  */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 119 w  */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* 120 x  */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* 121 y  */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* 122 z  */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* 123 { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 124 | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* 125 } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* 126 ~ */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* 127 DEL (solid block) */
};

/* ───────────────────────────────────────────────────────────────────────────
   G3. PIXEL / RECT / TEXT PRIMITIVES (FIXED)
   ─────────────────────────────────────────────────────────────────────────── */


/* 2. Modified gfx_pixel to write to the BUFFER, not the screen */
static inline void gfx_pixel(int x, int y, uint8_t c) {
    if((unsigned)x < GFX_W && (unsigned)y < GFX_H) {
        gui_backbuffer[y * GFX_W + x] = c;
    }
}

/* 3. This function pushes the buffer to the screen and fixes Mirroring */
void gfx_flip() {
    /* Wait for VSync to stop flashing */
    while ((inb(0x3DA) & 0x08));
    while (!(inb(0x3DA) & 0x08));

    uint8_t *vram = (uint8_t*)0xA0000;
    /* Use a simple linear copy. If your screen is STILL mirrored after 
       fixing gfx_char, only then change (i) to (y*320 + (319-x)) */
    for (int i = 0; i < 64000; i++) {
        vram[i] = gui_backbuffer[i];
    }
}
static inline uint8_t gfx_getpixel(int x, int y) {
    if((unsigned)x < GFX_W && (unsigned)y < GFX_H) return gui_backbuffer[y * GFX_W + x];
    return 0;
}

void gfx_hline(int x, int y, int w, uint8_t c) {
    for(int i=0; i<w; i++) gfx_pixel(x+i, y, c);
}

void gfx_vline(int x, int y, int h, uint8_t c) {
    for(int i=0; i<h; i++) gfx_pixel(x, y+i, c);
}

void gfx_rect(int x, int y, int w, int h, uint8_t c) {
    gfx_hline(x, y, w, c); gfx_hline(x, y+h-1, w, c);
    gfx_vline(x, y, h, c); gfx_vline(x+w-1, y, h, c);
}

void gfx_fill(int x, int y, int w, int h, uint8_t c) {
    for(int j=0; j<h; j++) {
        for(int i=0; i<w; i++) {
            gfx_pixel(x + i, y + j, c);
        }
    }
}

void gfx_rect3d(int x, int y, int w, int h, bool pressed) {
    uint8_t hi = pressed ? C_BTNSHADOW : C_BTNHI;
    uint8_t lo = pressed ? C_BTNHI     : C_BTNSHADOW;
    gfx_fill(x, y, w, h, C_BTNFACE);
    gfx_hline(x, y, w, hi);    gfx_vline(x, y, h, hi);
    gfx_hline(x, y+h-1, w, lo);gfx_vline(x+w-1, y, h, lo);
}

/* Fix the Mirroring: Use 0x80 >> col to read bits Left-to-Right */
void gfx_char(int x, int y, char ch, uint8_t fg, uint8_t bg) {
    if(ch < 32 || ch > 127) ch = 32;
    const uint8_t *bmp = font8x8[(int)(ch - 32)];
    for(int row = 0; row < 8; row++) {
        uint8_t bits = bmp[row];
        for(int col = 0; col < 8; col++) {
            /* 0x80 is the leftmost bit. We shift it right to draw left-to-right. */
            if(bits & (1 << col)) {
                gfx_pixel(x + col, y + row, fg);
            } else if(bg != 0xFF) {
                gfx_pixel(x + col, y + row, bg);
            }
        }
    }
}

void gfx_str(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    int start_x = x;
    while(*s) {
        if(*s == '\n') { 
            y += 9; 
            x = start_x; 
            s++; 
            continue; 
        }
        /* gfx_char (defined above) now handles bg == 0xFF correctly */
        gfx_char(x, y, *s, fg, bg);
        x += 8; 
        s++;
    }
}

void gfx_str_clip(int x, int y, int max_w, const char *s, uint8_t fg, uint8_t bg){
    int cx = x;
    while(*s && (cx + 8 <= x + max_w)) {
        gfx_char(cx, y, *s, fg, bg);
        cx += 8; s++;
    }
}

/* Integer to string and draw */
void gfx_int(int x,int y,int n,uint8_t fg,uint8_t bg){
    char buf[16]; itoa(n,buf); gfx_str(x,y,buf,fg,bg);
}

/* ───────────────────────────────────────────────────────────────────────────
   G4. PS/2 MOUSE DRIVER
   ─────────────────────────────────────────────────────────────────────────── */
static int  mouse_x=160, mouse_y=90;
static int  mouse_dx=0,  mouse_dy=0;
static bool mouse_lb=false, mouse_rb=false;
static bool mouse_clicked=false; /* edge-detect: button went down this frame */
static bool mouse_released=false;
static bool prev_lb=false;

/* Saved background under cursor (3×3) */
static uint8_t cursor_bg[16*16];
static int cursor_save_x=-1, cursor_save_y=-1;
static bool cursor_drawn=false;

/* 11×19 arrow cursor bitmap (1=fg,0=bg,2=outline) */
static const uint8_t CURSOR_W=11, CURSOR_H=19;
static const uint8_t cursor_bmp[19][11] = {
    {1,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0},
    {1,2,1,0,0,1,2,2,1,0,0},
    {1,1,0,0,0,0,1,2,2,1,0},
    {0,0,0,0,0,0,1,2,2,1,0},
    {0,0,0,0,0,0,0,1,2,1,0},
    {0,0,0,0,0,0,0,0,1,0,0},
};

void mouse_init() {
    /* Enable PS/2 auxiliary (mouse) device */
    /* Enable IRQ12 in PIC2 */
    outb(0xA1, inb(0xA1) & ~(1<<4));
    /* Tell PS/2 controller to enable aux port */
    outb(0x64,0xA8);
    /* Get current compaq status byte, set bit 1 (enable IRQ12), clear bit 5 */
    outb(0x64,0x20); uint8_t status=inb(0x60);
    status |=0x02; status &=~0x20;
    outb(0x64,0x60); outb(0x60,status);
    /* Send 0xF4 (enable) to mouse */
    outb(0x64,0xD4); outb(0x60,0xF4);
    inb(0x60); /* ACK */
}

/* Poll mouse (non-blocking — check if data ready) */
#define MOUSE_PORT 0x60
#define MOUSE_STAT 0x64

static uint8_t mouse_packet[3];
static int     mouse_pkt_idx=0;

void mouse_poll() {
    while(1) {
        uint8_t stat = inb(MOUSE_STAT);
        if(!(stat & 0x01)) break;        /* no data at all */
        if(!(stat & 0x20)) break;        /* data present but it's keyboard, not mouse — stop */
        uint8_t data = inb(MOUSE_PORT);
        /* Wait for sync byte: bit 3 must be set */
        if(mouse_pkt_idx==0 && !(data&0x08)) continue;
        mouse_packet[mouse_pkt_idx++]=data;
        if(mouse_pkt_idx==3) {
            mouse_pkt_idx=0;
            uint8_t flags=mouse_packet[0];
            int dx=(int)(int8_t)mouse_packet[1];
            int dy=-(int)(int8_t)mouse_packet[2]; /* Y is inverted */
            if(flags&0x40||flags&0x80) { dx=0; dy=0; } /* overflow */
            mouse_x+=dx; mouse_y+=dy;
            if(mouse_x<0)       mouse_x=0;
            if(mouse_x>=GFX_W)  mouse_x=GFX_W-1;
            if(mouse_y<0)       mouse_y=0;
            if(mouse_y>=GFX_H)  mouse_y=GFX_H-1;
            prev_lb=mouse_lb;
            mouse_lb=(flags&0x01)!=0;
            mouse_rb=(flags&0x02)!=0;
            mouse_clicked  = (mouse_lb && !prev_lb);
            mouse_released = (!mouse_lb && prev_lb);
        }
    }
}

void cursor_save(int x, int y) {
    for(int row=0;row<CURSOR_H;row++)
        for(int col=0;col<CURSOR_W;col++)
            cursor_bg[row*CURSOR_W+col]=gfx_getpixel(x+col,y+row);
    cursor_save_x=x; cursor_save_y=y;
}
void cursor_restore() {
    if(cursor_save_x<0) return;
    for(int row=0;row<CURSOR_H;row++)
        for(int col=0;col<CURSOR_W;col++)
            gfx_pixel(cursor_save_x+col,cursor_save_y+row,cursor_bg[row*CURSOR_W+col]);
    cursor_save_x=-1;
}
void cursor_draw(int x, int y) {
    for(int row = 0; row < CURSOR_H; row++) {
        for(int col = 0; col < CURSOR_W; col++) {
            uint8_t v = cursor_bmp[row][col];
            if(v == 1) gfx_pixel(x + col, y + row, C_WHITE);
            else if(v == 2) gfx_pixel(x + col, y + row, C_BLACK);
        }
    }
}

/* ───────────────────────────────────────────────────────────────────────────
   G5. WINDOW MANAGER
   ─────────────────────────────────────────────────────────────────────────── */
/* Forward declarations for app openers (defined later in G7) */
void open_terminal();
void open_editor_with(const char *filename);
#define MAX_WINS    8
#define TITLE_H     12  /* title bar height in pixels */
#define BORDER      2

typedef void (*WinDrawFn)(int wid);  /* callback: draw window contents */
typedef void (*WinClickFn)(int wid, int lx, int ly); /* local coords click */
typedef void (*WinKeyFn)(int wid, char key);

typedef struct {
    bool      active;
    int       x,y,w,h;
    char      title[32];
    WinDrawFn  on_draw;
    WinClickFn on_click;
    WinKeyFn   on_key;
    bool      focused;
    bool      dragging;
    int       drag_ox, drag_oy; /* offset within title bar */
    uint8_t   icon_color;
} Window;

static Window wins[MAX_WINS];
static int    win_focus = -1;
static bool   wm_needs_redraw = true;

int wm_open(const char *title, int x, int y, int w, int h,
            WinDrawFn df, WinClickFn cf, WinKeyFn kf, uint8_t ic) {
    for(int i=0;i<MAX_WINS;i++) {
        if(!wins[i].active) {
            wins[i].active=true;
            wins[i].x=x; wins[i].y=y;
            wins[i].w=w; wins[i].h=h;
            strncpy(wins[i].title,title,32);
            wins[i].on_draw=df; wins[i].on_click=cf; wins[i].on_key=kf;
            wins[i].focused=false;
            wins[i].dragging=false;
            wins[i].icon_color=ic;
            win_focus=i;
            wm_needs_redraw=true;
            return i;
        }
    }
    return -1;
}

void wm_close(int id) {
    if(id<0||id>=MAX_WINS||!wins[id].active) return;
    wins[id].active=false;
    win_focus=-1;
    /* Re-focus topmost remaining */
    for(int i=MAX_WINS-1;i>=0;i--) if(wins[i].active){ win_focus=i; break; }
    wm_needs_redraw=true;
}

void wm_draw_window(int id) {
    Window *w=&wins[id];
    bool focused=(id==win_focus);
    uint8_t tb_col = focused ? C_TITLEACT : C_TITLEBAR;

    /* Shadow */
    gfx_fill(w->x+3, w->y+3, w->w, w->h+TITLE_H, C_DKGREY);
    /* Border */
    gfx_fill(w->x, w->y, w->w, w->h+TITLE_H, C_WINBDR);
    /* Title bar */
    gfx_fill(w->x+BORDER, w->y+BORDER, w->w-BORDER*2, TITLE_H, tb_col);
    /* Title text */
    gfx_str_clip(w->x+BORDER+4, w->y+BORDER+2, w->w-30, w->title, C_WHITE, tb_col);
    /* Close button [X] */
    int cx=w->x+w->w-BORDER-12, cy=w->y+BORDER+1;
    gfx_fill(cx,cy,11,10,C_RED);
    gfx_char(cx+2,cy+1,'X',C_WHITE,C_RED);
    /* Client area */
    gfx_fill(w->x+BORDER, w->y+BORDER+TITLE_H, w->w-BORDER*2, w->h-TITLE_H, C_WINBG);
    /* Call draw callback */
    if(w->on_draw) w->on_draw(id);
}

void wm_draw_all() {
    for(int i=0;i<MAX_WINS;i++) if(wins[i].active) wm_draw_window(i);
}

/* Returns window id hit at (gx,gy), -1 if none.  Checks top-to-bottom. */
int wm_hit(int gx, int gy) {
    for(int i=MAX_WINS-1;i>=0;i--) {
        if(!wins[i].active) continue;
        Window *w=&wins[i];
        if(gx>=w->x && gx<w->x+w->w && gy>=w->y && gy<w->y+w->h+TITLE_H)
            return i;
    }
    return -1;
}

/* Check if click is on title bar */
bool wm_on_titlebar(int id, int gx, int gy) {
    Window *w=&wins[id];
    return (gx>=w->x+BORDER && gx<w->x+w->w-BORDER &&
            gy>=w->y+BORDER && gy<w->y+BORDER+TITLE_H);
}
bool wm_on_close(int id, int gx, int gy) {
    Window *w=&wins[id];
    int cx=w->x+w->w-BORDER-12, cy=w->y+BORDER+1;
    return (gx>=cx&&gx<cx+11&&gy>=cy&&gy<cy+10);
}

void wm_handle_mouse() {
    static bool was_dragging=false;
    static int  drag_win=-1;

    if(mouse_clicked) {
        /* Find hit window */
        int hit=wm_hit(mouse_x,mouse_y);
        if(hit>=0) {
            win_focus=hit;
            if(wm_on_close(hit,mouse_x,mouse_y)) {
                wm_close(hit);
                wm_needs_redraw=true;
                return;
            }
            if(wm_on_titlebar(hit,mouse_x,mouse_y)) {
                wins[hit].dragging=true;
                wins[hit].drag_ox=mouse_x-wins[hit].x;
                wins[hit].drag_oy=mouse_y-wins[hit].y;
                drag_win=hit;
                was_dragging=true;
            } else {
                /* Client area click */
                Window *w=&wins[hit];
                int lx=mouse_x-(w->x+BORDER);
                int ly=mouse_y-(w->y+BORDER+TITLE_H);
                if(w->on_click) w->on_click(hit,lx,ly);
            }
            wm_needs_redraw=true;
        }
    }

    if(mouse_lb && was_dragging && drag_win>=0) {
        Window *w=&wins[drag_win];
        int nx=mouse_x-w->drag_ox;
        int ny=mouse_y-w->drag_oy;
        /* Clamp */
        if(nx<0) nx=0;
        if(ny<0) ny=0;
        if(nx+w->w>GFX_W) nx=GFX_W-w->w;
        if(ny+w->h+TITLE_H>GFX_H-10) ny=GFX_H-10-w->h-TITLE_H;
        if(nx!=w->x||ny!=w->y){ w->x=nx; w->y=ny; wm_needs_redraw=true; }
    }

    if(mouse_released) {
        if(drag_win>=0) { wins[drag_win].dragging=false; drag_win=-1; }
        was_dragging=false;
    }
}

void wm_handle_key(char k) {
    if(win_focus>=0 && wins[win_focus].active && wins[win_focus].on_key)
        wins[win_focus].on_key(win_focus, k);
    wm_needs_redraw=true;
}

/* ───────────────────────────────────────────────────────────────────────────
   G6. DESKTOP  (wallpaper, taskbar, icons)
   ─────────────────────────────────────────────────────────────────────────── */
#define TASKBAR_H   10
#define TASKBAR_Y   (GFX_H-TASKBAR_H)
#define ICON_W      30
#define ICON_H      30
#define ICON_COLS   4

typedef struct {
    int x,y;
    char label[16];
    uint8_t color;
    void (*action)();
} DesktopIcon;

static DesktopIcon icons[10];
static int icon_count=0;

void icon_add(int x,int y,const char *label, uint8_t color, void(*action)()) {
    if(icon_count>=10) return;
    icons[icon_count].x=x; icons[icon_count].y=y;
    strncpy(icons[icon_count].label,label,16);
    icons[icon_count].color=color;
    icons[icon_count].action=action;
    icon_count++;
}

void draw_icon(DesktopIcon *ic) {
    /* Shadow */
    gfx_fill(ic->x+2,ic->y+2,ICON_W,ICON_H,C_DKGREY);
    /* Icon box */
    gfx_fill(ic->x,ic->y,ICON_W,ICON_H,ic->color);
    gfx_rect(ic->x,ic->y,ICON_W,ICON_H,C_WHITE);
    /* Label (centered below) */
    int lw=strlen(ic->label)*8;
    int lx=ic->x+(ICON_W-lw)/2;
    gfx_str(lx,ic->y+ICON_H+2,ic->label,C_WHITE,C_DESKTOP);
}

void draw_desktop() {
    /* Gradient-ish wallpaper: horizontal bands */
    for(int y=0;y<TASKBAR_Y;y++) {
        uint8_t col=rgb_to_pal(0, 60+(y*40/(TASKBAR_Y)), 100+(y*60/TASKBAR_Y));
        gfx_hline(0,y,GFX_W,col);
    }
    /* Subtle grid dots */
    for(int y=8;y<TASKBAR_Y;y+=16)
        for(int x=8;x<GFX_W;x+=16)
            gfx_pixel(x,y,rgb_to_pal(30,80,130));

    /* Draw icons */
    for(int i=0;i<icon_count;i++) draw_icon(&icons[i]);

    /* Taskbar */
    gfx_fill(0,TASKBAR_Y,GFX_W,TASKBAR_H,C_TASKBAR);
    gfx_hline(0,TASKBAR_Y,GFX_W,rgb_to_pal(80,80,140));

    /* Start label */
    gfx_str(2,TASKBAR_Y+1,"MPOS",rgb_to_pal(100,200,255),C_TASKBAR);

    /* Window buttons in taskbar */
    int tx=42;
    for(int i=0;i<MAX_WINS;i++) {
        if(!wins[i].active) continue;
        bool focused=(i==win_focus);
        uint8_t bc=focused?C_TITLEACT:rgb_to_pal(50,50,100);
        gfx_fill(tx,TASKBAR_Y+1,50,8,bc);
        gfx_str_clip(tx+2,TASKBAR_Y+2,46,wins[i].title,C_WHITE,bc);
        tx+=52;
    }

    /* Clock (live from RTC) */
    DateTime dt; rtc_get(&dt);
    char clock[12];
    char tmp[4];
    clock[0]='\0';
    if(dt.hour<10){clock[0]='0';clock[1]='0'+dt.hour;clock[2]='\0';}
    else itoa(dt.hour,clock);
    int cl=strlen(clock); clock[cl]=':'; clock[cl+1]='\0';
    if(dt.min<10){tmp[0]='0';tmp[1]='0'+dt.min;tmp[2]='\0';}
    else itoa(dt.min,tmp);
    strcat(clock,tmp);
    clock[strlen(clock)]=':'; clock[strlen(clock)+1]='\0';
    if(dt.sec<10){tmp[0]='0';tmp[1]='0'+dt.sec;tmp[2]='\0';}
    else itoa(dt.sec,tmp);
    strcat(clock,tmp);
    int cw=strlen(clock)*8;
    gfx_str(GFX_W-cw-2,TASKBAR_Y+1,clock,rgb_to_pal(200,220,255),C_TASKBAR);
}

/* Desktop icon click detection */
void desktop_click(int gx, int gy) {
    for(int i=0;i<icon_count;i++) {
        DesktopIcon *ic=&icons[i];
        if(gx>=ic->x&&gx<ic->x+ICON_W && gy>=ic->y&&gy<ic->y+ICON_H) {
            if(ic->action) ic->action();
        }
    }
}

/* ───────────────────────────────────────────────────────────────────────────
   G7. BUILT-IN APP WINDOWS
   ─────────────────────────────────────────────────────────────────────────── */

/* ===== TERMINAL WINDOW ===== */
#define TERM_ROWS  10
#define TERM_COLS  36
#define TERM_BUFSZ 512

static char  tw_lines[TERM_ROWS][TERM_COLS+1];
static int   tw_row=0, tw_col=0;
static char  tw_input[64];
static int   tw_input_len=0;
static bool  tw_input_active=false;
static int   tw_win=-1;

void tw_newline() {
    tw_col=0;
    tw_row++;
    if(tw_row>=TERM_ROWS-1) {
        /* Scroll up */
        for(int i=0;i<TERM_ROWS-2;i++) {
            for(int j=0;j<=TERM_COLS;j++) tw_lines[i][j]=tw_lines[i+1][j];
        }
        memset(tw_lines[TERM_ROWS-2],0,TERM_COLS+1);
        tw_row=TERM_ROWS-2;
    }
}

void tw_putc(char c) {
    if(c=='\n') { tw_newline(); return; }
    if(c=='\r') { tw_col=0; return; }
    if(tw_col>=TERM_COLS) tw_newline();
    tw_lines[tw_row][tw_col++]=c;
    tw_lines[tw_row][tw_col]='\0';
}

void tw_print(const char *s) { while(*s) tw_putc(*s++); }

void tw_draw(int id) {
    Window *w=&wins[id];
    int cx=w->x+BORDER, cy=w->y+BORDER+TITLE_H;
    int cw=w->w-BORDER*2, ch=w->h-TITLE_H;
    UNUSED(cw); UNUSED(ch);
    /* Black terminal background */
    gfx_fill(cx,cy,w->w-BORDER*2,w->h-TITLE_H,C_BLACK);

    /* Text rows */
    uint8_t fg=rgb_to_pal(0,220,0); /* green-on-black */
    for(int r=0;r<TERM_ROWS-1;r++)
        gfx_str(cx+2, cy+2+r*9, tw_lines[r], fg, C_BLACK);

    /* Input line */
    int iy=cy+2+(TERM_ROWS-1)*9;
    gfx_fill(cx,iy-1,w->w-BORDER*2,10,rgb_to_pal(0,30,0));
    gfx_str(cx+2,iy,">",rgb_to_pal(0,255,0),rgb_to_pal(0,30,0));

    /* Input buffer */
    char disp[TERM_COLS+2]; disp[0]='\0';
    strncpy(disp, tw_input, TERM_COLS-2); disp[TERM_COLS-2]='\0';
    gfx_str(cx+10,iy,disp,C_WHITE,rgb_to_pal(0,30,0));

    /* Cursor blink (always on for simplicity) */
    int cursor_px=cx+10+(tw_input_len*8);
    if(cursor_px < cx+w->w-BORDER*2-8)
        gfx_fill(cursor_px,iy,2,8,rgb_to_pal(0,255,0));
}

void tw_click(int id, int lx, int ly) {
    UNUSED(id); UNUSED(lx); UNUSED(ly);
    tw_input_active=true;
}

/* Pipe terminal output to tw_print */
static bool tw_capture=false;
void tw_flush_pipe() {
    /* pipe_buf was filled by dispatch — print it to terminal window */
    tw_print(pipe_buf);
}

void tw_key(int id, char k) {
    UNUSED(id);
    if(k == '\n') {
        tw_print("\n");
        if(tw_input_len > 0) {
            /* 1. Clear the output buffer */
            pipe_clear(); 
            
            /* 2. Tell the kernel to redirect 'print' into our buffer */
            pipe_mode = true; 
            
            /* 3. Execute the command */
            dispatch(tw_input);
            
            /* 4. Stop redirection */
            pipe_mode = false; 
            
            /* 5. Print the result into the GUI terminal window */
            if(pipe_len > 0) {
                tw_print(pipe_buf);
            } else {
                /* If command produced no output, at least show a newline */
                tw_print("\n");
            }
        }
        /* Reset input bar */
        tw_input[0] = '\0'; 
        tw_input_len = 0;
    } else if(k == '\b') {
        if(tw_input_len > 0) { 
            tw_input_len--; 
            tw_input[tw_input_len] = '\0'; 
        }
    } else if(k >= ' ' && k <= '~' && tw_input_len < 60) {
        tw_input[tw_input_len++] = k; 
        tw_input[tw_input_len] = '\0';
    }
    wm_needs_redraw = true;
}

static bool gui_running=false;

void open_terminal() {
    if(tw_win>=0 && wins[tw_win].active){ win_focus=tw_win; wm_needs_redraw=true; return; }
    for(int i=0;i<TERM_ROWS;i++){ tw_lines[i][0]='\0'; }
    tw_row=0; tw_col=0;
    tw_input[0]='\0'; tw_input_len=0;
    tw_win=wm_open("Terminal",45,10,240,115,tw_draw,tw_click,tw_key,C_DKGREEN);
}

/* ===== FILE MANAGER WINDOW ===== */
static int fm_win=-1;
static int fm_scroll=0;
static int fm_selected=-1;

void fm_draw(int id) {
    Window *w=&wins[id];
    int cx=w->x+BORDER, cy=w->y+BORDER+TITLE_H;
    gfx_fill(cx,cy,w->w-BORDER*2,w->h-TITLE_H,C_WINBG);
    /* Header */
    gfx_fill(cx,cy,w->w-BORDER*2,10,C_TITLEBAR);
    gfx_str(cx+2,cy+1,"NAME             SIZE  PERM",C_WHITE,C_TITLEBAR);
    int row=0;
    for(int i=fm_scroll;i<MAX_FILES&&row<12;i++) {
        if(!fs[i].active) continue;
        int ry=cy+11+row*9;
        uint8_t bg=(i==fm_selected)?C_SELBG:((row&1)?rgb_to_pal(210,210,220):C_WINBG);
        uint8_t fg=(i==fm_selected)?C_WHITE:C_TXTDARK;
        gfx_fill(cx,ry,w->w-BORDER*2,9,bg);
        gfx_str_clip(cx+2,ry+1,120,fs[i].name,fg,bg);
        char sz[8]; itoa(fs[i].size,sz);
        gfx_str(cx+122,ry+1,sz,fg,bg);
        char perm[4]; fs_perm_str(fs[i].perms,perm);
        gfx_str(cx+155,ry+1,perm,fg,bg);
        row++;
    }
    /* Buttons */
    int by=cy+w->h-TITLE_H-12;
    gfx_rect3d(cx+2,by,30,10,false);   gfx_str(cx+6,by+1,"Open",C_TXTDARK,C_BTNFACE);
    gfx_rect3d(cx+35,by,30,10,false);  gfx_str(cx+39,by+1,"Run ",C_TXTDARK,C_BTNFACE);
    gfx_rect3d(cx+68,by,30,10,false);  gfx_str(cx+72,by+1,"Del ",C_TXTDARK,C_BTNFACE);
}

void fm_click(int id, int lx, int ly) {
    Window *w=&wins[id];
    int ch=w->h-TITLE_H;
    /* Row click */
    if(ly>=11 && ly<ch-12) {
        int row=(ly-11)/9;
        int sel=0;
        for(int i=fm_scroll;i<MAX_FILES;i++) {
            if(!fs[i].active) continue;
            if(sel==row){ fm_selected=i; break; }
            sel++;
        }
    }
    /* Button clicks */
    int by=ch-12;
    if(ly>=by&&ly<by+10) {
        if(lx>=2&&lx<32 && fm_selected>=0) {
            /* Open: launch text editor with this file */
            open_editor_with(fs[fm_selected].name);
        }
        if(lx>=35&&lx<65 && fm_selected>=0) {
            if(fs[fm_selected].perms&PERM_X) {
                open_terminal();
                int nl=strlen(fs[fm_selected].name);
                pipe_clear(); pipe_mode=true;
                if(nl>=2 && fs[fm_selected].name[nl-2]=='.' && fs[fm_selected].name[nl-1]=='c')
                    run_c_script(fs[fm_selected].content);
                else run_tiny_script(fs[fm_selected].content);
                pipe_mode=false;
                tw_print(pipe_buf);
            } else {
                open_terminal();
                tw_print("Not executable. Use chmod.\n");
            }
        }
        if(lx>=68&&lx<98 && fm_selected>=0) {
            pipe_clear(); pipe_mode=true;
            fs_rm(fs[fm_selected].name);
            pipe_mode=false;
            fm_selected=-1;
        }
    }
    UNUSED(id);
    wm_needs_redraw=true;
}

void fm_key(int id, char k) { UNUSED(id); UNUSED(k); }

void open_filemanager() {
    if(fm_win>=0&&wins[fm_win].active){ win_focus=fm_win; wm_needs_redraw=true; return; }
    fm_selected=-1; fm_scroll=0;
    fm_win=wm_open("File Manager",50,15,185,135,fm_draw,fm_click,fm_key,C_DKBLUE);
}

/* ===== CALCULATOR WINDOW ===== */
static int   calc_win=-1;
static char  calc_display[24]="0";
static int   calc_disp_len=1;
static double calc_accum=0;
static char  calc_op=0;
static bool  calc_new_num=true;

/* Simple integer-only GUI calc */
static int calc_val=0;
static int calc_prev=0;

static const char calc_btns[4][4] = {
    {'7','8','9','/'},
    {'4','5','6','*'},
    {'1','2','3','-'},
    {'0','=','C','+'},
};

void calc_draw(int id) {
    Window *w=&wins[id];
    int cx=w->x+BORDER, cy=w->y+BORDER+TITLE_H;
    gfx_fill(cx,cy,w->w-BORDER*2,w->h-TITLE_H,rgb_to_pal(200,200,210));
    /* Display */
    gfx_fill(cx+2,cy+2,w->w-BORDER*2-4,12,C_BLACK);
    int dw=strlen(calc_display)*8;
    int dx=cx+w->w-BORDER*2-4-dw;
    gfx_str(dx>cx+2?dx:cx+4,cy+4,calc_display,rgb_to_pal(0,255,0),C_BLACK);
    /* Buttons 4×4, each 20×14 */
    for(int r=0;r<4;r++)
    for(int c=0;c<4;c++){
        int bx=cx+2+c*21, by=cy+16+r*15;
        gfx_rect3d(bx,by,20,14,false);
        char label[2]={calc_btns[r][c],'\0'};
        gfx_char(bx+6,by+3,calc_btns[r][c],C_TXTDARK,C_BTNFACE);
    }
}

void calc_press(char btn) {
    if(btn>='0'&&btn<='9') {
        if(calc_new_num||strcmp(calc_display,"0")==0){
            calc_display[0]=btn; calc_display[1]='\0'; calc_disp_len=1;
            calc_new_num=false;
        } else if(calc_disp_len<10){
            calc_display[calc_disp_len++]=btn; calc_display[calc_disp_len]='\0';
        }
    } else if(btn=='C') {
        calc_display[0]='0'; calc_display[1]='\0'; calc_disp_len=1;
        calc_val=0; calc_prev=0; calc_op=0; calc_new_num=true;
    } else if(btn=='='&&calc_op) {
        int cur=atoi(calc_display);
        int res=calc_prev;
        if(calc_op=='+') res=calc_prev+cur;
        else if(calc_op=='-') res=calc_prev-cur;
        else if(calc_op=='*') res=calc_prev*cur;
        else if(calc_op=='/'&&cur!=0) res=calc_prev/cur;
        itoa(res, calc_display); calc_disp_len=strlen(calc_display);
        calc_op=0; calc_new_num=true;
    } else if(btn=='+'||btn=='-'||btn=='*'||btn=='/') {
        calc_prev=atoi(calc_display);
        calc_op=btn; calc_new_num=true;
    }
}

void calc_click(int id, int lx, int ly) {
    UNUSED(id);
    /* Map click to button */
    int col=(lx-2)/21, row=(ly-16)/15;
    if(col>=0&&col<4&&row>=0&&row<4) calc_press(calc_btns[row][col]);
    wm_needs_redraw=true;
}
void calc_key(int id, char k) {
    UNUSED(id);
    if((k>='0'&&k<='9')||k=='+'||k=='-'||k=='*'||k=='/'||k=='='||k=='c'||k=='C')
        calc_press(k);
    wm_needs_redraw=true;
}

void open_calculator() {
    if(calc_win>=0&&wins[calc_win].active){ win_focus=calc_win; wm_needs_redraw=true; return; }
    calc_display[0]='0'; calc_display[1]='\0'; calc_disp_len=1;
    calc_op=0; calc_prev=0; calc_new_num=true;
    calc_win=wm_open("Calculator",200,30,90,80,calc_draw,calc_click,calc_key,C_BROWN);
}

/* ===== APPS LAUNCHER WINDOW ===== */

typedef struct {
    const char *display_name;
    const char *filename;
    const char *description;
    uint8_t     icon_color;
} AppEntry;

/* Catalogue — matches files created in load_demo_files */
static const AppEntry app_catalog[] = {
    { "Fibonacci",   "fibonacci.c",  "Fibonacci sequence",    C_CYAN      },
    { "Primes",      "primes.c",     "Prime numbers to 80",   C_YELLOW    },
    { "FizzBuzz",    "fizzbuzz.c",   "Classic FizzBuzz 1-30", C_GREEN     },
    { "Times Table", "times.c",      "Multiplication tables", C_DKCYAN    },
    { "Countdown",   "countdown.c",  "10..1 countdown!",      C_RED       },
    { "Squares",     "squares.c",    "Perfect squares",       C_MAGENTA   },
    { "Powers of 2", "powers2.c",    "Binary powers 0-15",    C_DKBLUE    },
    { "Collatz",     "collatz.c",    "Collatz: starting n=27",C_YELLOW    },
    { "Sys Info",    "sysinfo2.c",   "System information",    C_DKGREEN   },
    { "Rainbow",     "rainbow.c",    "Color showcase",        C_DKRED     },
    { "Triangles",   "triangles.c",  "Number triangle",       C_CYAN      },
    { "Factors",     "factors.c",    "Factors of 360",        C_GREEN     },
    { "GCD",         "gcd.c",        "Euclidean GCD algo",    C_DKGREEN   },
    { "Guess Game",  "guess.c",      "Secret number puzzle",  C_MAGENTA   },
    { "Bubble Sort", "bubblesort.c", "Sort 7 numbers",        C_YELLOW    },
};
#define APP_COUNT 15
#define APP_ROW_H  19
#define APP_VISIBLE 8

static int  apps_win      = -1;
static int  apps_scroll   =  0;
static int  apps_selected = -1;

void apps_draw(int id) {
    Window *w = &wins[id];
    int cx = w->x + BORDER;
    int cy = w->y + BORDER + TITLE_H;
    int cw = w->w - BORDER * 2;

    /* Panel background */
    gfx_fill(cx, cy, cw, w->h - TITLE_H, C_WINBG);

    /* Column headers */
    gfx_fill(cx, cy, cw, 11, C_TITLEACT);
    gfx_str(cx + 20, cy + 2, "App",         C_WHITE, C_TITLEACT);
    gfx_str(cx + 110, cy + 2, "Description", C_WHITE, C_TITLEACT);

    int ay = cy + 12;
    for(int i = apps_scroll; i < APP_COUNT && i < apps_scroll + APP_VISIBLE; i++) {
        bool sel = (i == apps_selected);
        uint8_t row_bg = sel
            ? C_SELBG
            : (i % 2 == 0 ? C_WINBG : rgb_to_pal(205, 205, 218));
        uint8_t fg     = sel ? C_WHITE : C_TXTDARK;
        uint8_t dfg    = sel ? rgb_to_pal(180,220,255) : rgb_to_pal(80,80,100);

        gfx_fill(cx, ay, cw, APP_ROW_H - 1, row_bg);

        /* Coloured icon square */
        gfx_fill(cx + 2, ay + 3, 12, 12, app_catalog[i].icon_color);
        gfx_rect(cx + 2, ay + 3, 12, 12, sel ? C_WHITE : C_DKGREY);

        /* App name */
        gfx_str(cx + 18, ay + 5, app_catalog[i].display_name, fg, row_bg);

        /* Description (clipped) */
        gfx_str_clip(cx + 108, ay + 5, cw - 110, app_catalog[i].description, dfg, row_bg);

        ay += APP_ROW_H;
    }

    /* ── Scroll bar area ── */
    int sb_x  = cx + cw - 14;     /* right edge strip */
    int sb_y0 = cy + 12;
    int sb_h  = APP_VISIBLE * APP_ROW_H;

    /* Track */
    gfx_fill(sb_x, sb_y0, 13, sb_h, rgb_to_pal(180,180,200));

    /* Up arrow button */
    bool can_up   = (apps_scroll > 0);
    uint8_t arr_bg = can_up ? rgb_to_pal(100,120,200) : rgb_to_pal(150,150,170);
    gfx_fill(sb_x, sb_y0, 13, 13, arr_bg);
    gfx_rect(sb_x, sb_y0, 13, 13, C_WHITE);
    gfx_str(sb_x + 3, sb_y0 + 3, can_up ? "^" : "-", C_WHITE, arr_bg);

    /* Down arrow button */
    bool can_dn   = (apps_scroll + APP_VISIBLE < APP_COUNT);
    arr_bg = can_dn ? rgb_to_pal(100,120,200) : rgb_to_pal(150,150,170);
    gfx_fill(sb_x, sb_y0 + sb_h - 13, 13, 13, arr_bg);
    gfx_rect(sb_x, sb_y0 + sb_h - 13, 13, 13, C_WHITE);
    gfx_str(sb_x + 3, sb_y0 + sb_h - 11, can_dn ? "v" : "-", C_WHITE, arr_bg);

    /* Thumb */
    if(APP_COUNT > APP_VISIBLE) {
        int track_h = sb_h - 26;
        int thumb_h = track_h * APP_VISIBLE / APP_COUNT;
        if(thumb_h < 6) thumb_h = 6;
        int thumb_y = sb_y0 + 13 + track_h * apps_scroll / APP_COUNT;
        gfx_fill(sb_x + 2, thumb_y, 9, thumb_h, rgb_to_pal(60,90,180));
    }

    /* ── Run button ── */
    int btn_y  = cy + 12 + APP_VISIBLE * APP_ROW_H + 3;
    bool can_run = (apps_selected >= 0);
    uint8_t btn_bg = can_run ? C_TITLEACT : rgb_to_pal(130,130,150);
    int btn_w = 64, btn_h = 13;
    int btn_x = cx + (cw - btn_w) / 2;
    gfx_fill(btn_x, btn_y, btn_w, btn_h, btn_bg);
    gfx_rect(btn_x, btn_y, btn_w, btn_h, C_WHITE);
    gfx_str(btn_x + 8, btn_y + 3,
            can_run ? "  RUN APP " : " SELECT.. ", C_WHITE, btn_bg);

    /* hint text */
    gfx_str(cx, btn_y + 3, can_run ? app_catalog[apps_selected].display_name : "",
            rgb_to_pal(80,80,100), C_WINBG);
}

static void apps_run_selected() {
    if(apps_selected < 0 || apps_selected >= APP_COUNT) return;
    File *f = fs_find(app_catalog[apps_selected].filename);
    open_terminal();
    /* Clear terminal output */
    for(int i = 0; i < TERM_ROWS; i++) memset(tw_lines[i], 0, TERM_COLS + 1);
    tw_row = 0; tw_col = 0;
    if(f) {
        tw_print(app_catalog[apps_selected].display_name);
        tw_print("\n");
        pipe_clear(); pipe_mode = true;
        run_c_script(f->content);
        pipe_mode = false;
        tw_print(pipe_buf);
    } else {
        tw_print("Error: file not found: ");
        tw_print(app_catalog[apps_selected].filename);
        tw_print("\n");
    }
    wm_needs_redraw = true;
}

void apps_click(int id, int lx, int ly) {
    Window *w = &wins[id];
    int cw = w->w - BORDER * 2;

    /* ── Scrollbar strip (rightmost 14px) ── */
    int sb_x  = cw - 14;
    int sb_y0 = 12;                        /* below header */
    int sb_h  = APP_VISIBLE * APP_ROW_H;

    if(lx >= sb_x) {
        /* Up arrow */
        if(ly >= sb_y0 && ly < sb_y0 + 13) {
            if(apps_scroll > 0) { apps_scroll--; wm_needs_redraw = true; }
        }
        /* Down arrow */
        else if(ly >= sb_y0 + sb_h - 13 && ly < sb_y0 + sb_h) {
            if(apps_scroll + APP_VISIBLE < APP_COUNT) { apps_scroll++; wm_needs_redraw = true; }
        }
        /* Thumb track — jump to proportional position */
        else if(ly >= sb_y0 + 13 && ly < sb_y0 + sb_h - 13) {
            int track_h = sb_h - 26;
            int new_scroll = (ly - sb_y0 - 13) * APP_COUNT / track_h - APP_VISIBLE / 2;
            if(new_scroll < 0) new_scroll = 0;
            if(new_scroll + APP_VISIBLE > APP_COUNT) new_scroll = APP_COUNT - APP_VISIBLE;
            apps_scroll = new_scroll;
            wm_needs_redraw = true;
        }
        return;
    }

    /* ── Row list ── */
    int list_rel = ly - 12;
    if(list_rel >= 0 && list_rel < APP_VISIBLE * APP_ROW_H) {
        int row = list_rel / APP_ROW_H + apps_scroll;
        if(row >= 0 && row < APP_COUNT) {
            if(apps_selected == row) {
                apps_run_selected();   /* second click on same row = run */
            } else {
                apps_selected = row;
                wm_needs_redraw = true;
            }
        }
        return;
    }

    /* ── Run button ── */
    int btn_y  = 12 + APP_VISIBLE * APP_ROW_H + 3;
    int btn_w  = 64, btn_h = 13;
    int btn_x0 = (cw - btn_w) / 2;
    if(ly >= btn_y && ly < btn_y + btn_h && lx >= btn_x0 && lx < btn_x0 + btn_w) {
        apps_run_selected();
    }
}

void apps_key(int id, char k) {
    UNUSED(id);
    uint8_t uk = (uint8_t)k;

    if(uk == KEY_UP) {
        if(apps_selected > 0) apps_selected--;
        else if(apps_scroll > 0) { apps_scroll--; apps_selected = apps_scroll; }
        /* Keep selection visible */
        if(apps_selected < apps_scroll) apps_scroll = apps_selected;
        wm_needs_redraw = true;

    } else if(uk == KEY_DOWN) {
        if(apps_selected < APP_COUNT - 1) apps_selected++;
        else apps_selected = APP_COUNT - 1;
        /* Keep selection visible */
        if(apps_selected >= apps_scroll + APP_VISIBLE)
            apps_scroll = apps_selected - APP_VISIBLE + 1;
        wm_needs_redraw = true;

    } else if(uk == KEY_PGUP) {
        apps_scroll -= APP_VISIBLE;
        if(apps_scroll < 0) apps_scroll = 0;
        apps_selected = apps_scroll;
        wm_needs_redraw = true;

    } else if(uk == KEY_PGDN) {
        apps_scroll += APP_VISIBLE;
        if(apps_scroll + APP_VISIBLE > APP_COUNT)
            apps_scroll = APP_COUNT - APP_VISIBLE;
        if(apps_scroll < 0) apps_scroll = 0;
        apps_selected = apps_scroll;
        wm_needs_redraw = true;

    } else if(uk == KEY_HOME) {
        apps_scroll = 0; apps_selected = 0;
        wm_needs_redraw = true;

    } else if(uk == KEY_END) {
        apps_selected = APP_COUNT - 1;
        apps_scroll = APP_COUNT - APP_VISIBLE;
        if(apps_scroll < 0) apps_scroll = 0;
        wm_needs_redraw = true;

    } else if(k == '\n' || k == ' ') {
        apps_run_selected();
    }
}

void open_apps() {
    if(apps_win >= 0 && wins[apps_win].active) {
        win_focus = apps_win;
        wm_needs_redraw = true;
        return;
    }
    apps_scroll = 0; apps_selected = -1;
    /* Window: 270 wide, header+8 rows+scroll+button+padding */
    int wh = 12 + APP_VISIBLE * APP_ROW_H + 10 + 14 + TITLE_H + BORDER * 2 + 4;
    apps_win = wm_open("App Launcher", 25, 12, 270, wh,
                       apps_draw, apps_click, apps_key,
                       rgb_to_pal(20, 50, 150));
}

/* ===== ABOUT WINDOW ===== */
static int about_win=-1;

void about_draw(int id) {
    Window *w=&wins[id];
    int cx=w->x+BORDER, cy=w->y+BORDER+TITLE_H;
    gfx_fill(cx,cy,w->w-BORDER*2,w->h-TITLE_H,C_WINBG);
    /* Logo */
    gfx_fill(cx+4,cy+4,w->w-BORDER*2-8,20,C_TITLEACT);
    gfx_str(cx+8,cy+10,"MPOS v7.0",C_WHITE,C_TITLEACT);
    /* Info text */
    uint8_t bg=C_WINBG; uint8_t fg=C_TXTDARK;
    gfx_str(cx+4,cy+28,"Multi-Purpose OS",fg,bg);
    gfx_str(cx+4,cy+38,"VGA Mode 13h GUI",fg,bg);
    gfx_str(cx+4,cy+48,"PS/2 Mouse + Keyboard",fg,bg);
    gfx_str(cx+4,cy+58,"Window Manager",fg,bg);
    gfx_str(cx+4,cy+68,"RAM Filesystem (32x4KB)",fg,bg);
    gfx_str(cx+4,cy+78,"C-Script & Tiny-Script",fg,bg);
    /* CPU info */
    char vendor[13]; get_cpu_vendor(vendor);
    gfx_str(cx+4,cy+90,"CPU:",fg,bg); gfx_str(cx+36,cy+90,vendor,fg,bg);
    /* Close button */
    gfx_rect3d(cx+w->w/2-BORDER*2-20,cy+w->h-TITLE_H-14,40,12,false);
    gfx_str(cx+w->w/2-BORDER*2-14,cy+w->h-TITLE_H-11,"Close",C_TXTDARK,C_BTNFACE);
}

void about_click(int id, int lx, int ly) {
    Window *w=&wins[id];
    int ch=w->h-TITLE_H;
    /* Close button */
    int bx=w->w/2-BORDER*2-20, by=ch-14;
    if(lx>=bx&&lx<bx+40&&ly>=by&&ly<by+12) wm_close(id);
    wm_needs_redraw=true;
}
void about_key(int id, char k){ UNUSED(id); UNUSED(k); }

void open_about() {
    if(about_win>=0&&wins[about_win].active){ win_focus=about_win; wm_needs_redraw=true; return; }
    about_win=wm_open("About MPOS",110,50,160,112,about_draw,about_click,about_key,C_MAGENTA);
}

/* ===== SYSTEM MONITOR WINDOW ===== */
static int sysmon_win=-1;

void sysmon_draw(int id) {
    Window *w=&wins[id];
    int cx=w->x+BORDER, cy=w->y+BORDER+TITLE_H;
    gfx_fill(cx,cy,w->w-BORDER*2,w->h-TITLE_H,C_BLACK);

    uint8_t fg=rgb_to_pal(0,220,100); uint8_t bg=C_BLACK;
    size_t free_kb=mm_free_bytes()/1024;
    size_t total_kb=HEAP_SIZE/1024;
    size_t used_kb=total_kb-free_kb;

    gfx_str(cx+2,cy+2, "=== SYSTEM MONITOR ===",fg,bg);

    /* Memory bar */
    gfx_str(cx+2,cy+14,"MEM:",fg,bg);
    char buf[16]; itoa(used_kb,buf); gfx_str(cx+34,cy+14,buf,C_WHITE,bg);
    gfx_str(cx+34+strlen(buf)*8,cy+14,"/",fg,bg);
    itoa(total_kb,buf); gfx_str(cx+34+(strlen(buf)+1)*8,cy+14,buf,C_WHITE,bg);
    gfx_str(cx+34+(strlen(buf)*2+2)*8,cy+14,"KB",fg,bg);

    int bar_w=w->w-BORDER*2-4;
    int used_w=(int)((long)used_kb*bar_w/total_kb);
    gfx_fill(cx+2,cy+24,bar_w,6,rgb_to_pal(0,50,0));
    gfx_fill(cx+2,cy+24,used_w,6,rgb_to_pal(0,200,80));
    gfx_rect(cx+2,cy+24,bar_w,6,fg);

    /* Processes */
    gfx_str(cx+2,cy+34,"PROCS:",fg,bg);
    int pc=0,py=cy+44;
    for(int i=0;i<MAX_PROCS;i++){
        if(proc_table[i].state==PROC_FREE) continue;
        char pid_s[8]; itoa(proc_table[i].pid,pid_s);
        gfx_str(cx+2,py,pid_s,C_WHITE,bg);
        gfx_str(cx+22,py,proc_table[i].state==PROC_RUNNING?"RUN":"ZMB",
                proc_table[i].state==PROC_RUNNING?rgb_to_pal(0,255,0):C_RED,bg);
        gfx_str_clip(cx+50,py,80,proc_table[i].name,fg,bg);
        py+=9; pc++;
        if(py>=cy+w->h-TITLE_H-4) break;
    }
    if(pc==0) gfx_str(cx+2,py,"(none)",rgb_to_pal(100,100,100),bg);

    /* Files */
    gfx_str(cx+2,cy+w->h-TITLE_H-10,"FILES:",fg,bg);
    char fc[4]; itoa(fs_count,fc);
    gfx_str(cx+50,cy+w->h-TITLE_H-10,fc,C_WHITE,bg);
}

void sysmon_click(int id,int lx,int ly){UNUSED(id);UNUSED(lx);UNUSED(ly);}
void sysmon_key(int id,char k){UNUSED(id);UNUSED(k);}

void open_sysmon() {
    if(sysmon_win>=0&&wins[sysmon_win].active){ win_focus=sysmon_win; wm_needs_redraw=true; return; }
    sysmon_win=wm_open("System Monitor",5,5,150,100,sysmon_draw,sysmon_click,sysmon_key,C_DKGREEN);
}

/* ===== TEXT EDITOR WINDOW ===== */
/*
 * A full-screen-style windowed text editor.
 * - Displays file content with line numbers
 * - Keyboard input: type, backspace, enter
 * - TAB = save current buffer to file
 * - ESC = discard and close
 * - Ctrl+N = new file prompt (enter filename in input bar first)
 * - Scrolls vertically when content exceeds visible area
 */

#define ED_COLS     34      /* visible character columns */
#define ED_ROWS     10      /* visible text rows          */
#define ED_BUFSZ    FILE_SIZE

static int   ed_win     = -1;
static char  ed_buf[ED_BUFSZ];   /* editing buffer            */
static int   ed_len     = 0;     /* current content length    */
static int   ed_cursor  = 0;     /* cursor position in buf    */
static int   ed_scroll  = 0;     /* top visible row index     */
static char  ed_filename[32];    /* currently open filename   */
static bool  ed_modified = false;

/* --- filename prompt bar state --- */
static char  ed_fname_input[32];
static int   ed_fname_len = 0;
static bool  ed_fname_mode = false; /* true = prompting for filename */

/* Count which row (0-based) and column the cursor is on */
static void ed_cursor_pos(int *out_row, int *out_col) {
    int row = 0, col = 0;
    for(int i = 0; i < ed_cursor && i < ed_len; i++) {
        if(ed_buf[i] == '\n') { row++; col = 0; }
        else col++;
    }
    *out_row = row; *out_col = col;
}

/* Return pointer to start of line number `line` in ed_buf */
static char *ed_line_start(int line) {
    char *p = ed_buf;
    for(int l = 0; l < line && *p; p++) {
        if(*p == '\n') l++;
    }
    return p;
}

/* Count total lines in buffer */
static int ed_total_lines() {
    int n = 1;
    for(int i = 0; i < ed_len; i++) if(ed_buf[i] == '\n') n++;
    return n;
}

void ed_draw(int id) {
    Window *w = &wins[id];
    int cx = w->x + BORDER;
    int cy = w->y + BORDER + TITLE_H;
    int cw = w->w - BORDER * 2;
    int ch = w->h - TITLE_H;

    /* Background */
    gfx_fill(cx, cy, cw, ch, C_BLACK);

    /* ---- Toolbar ---- */
    int toolbar_h = 10;
    gfx_fill(cx, cy, cw, toolbar_h, rgb_to_pal(30, 30, 70));
    /* Filename */
    gfx_str_clip(cx + 2, cy + 1, cw - 60, ed_filename[0] ? ed_filename : "(untitled)",
                 rgb_to_pal(180, 220, 255), rgb_to_pal(30, 30, 70));
    /* Modified indicator */
    if(ed_modified)
        gfx_str(cx + cw - 24, cy + 1, "*MOD", rgb_to_pal(255, 200, 0), rgb_to_pal(30, 30, 70));
    /* Shortcut hints */
    int hint_y = cy + ch - 9;
    gfx_fill(cx, hint_y, cw, 9, rgb_to_pal(20, 20, 50));
    gfx_str(cx + 1, hint_y + 1, "TAB=Save  ESC=Close",
            rgb_to_pal(120, 140, 200), rgb_to_pal(20, 20, 50));

    /* ---- Text area ---- */
    int text_y = cy + toolbar_h;
    int text_h = ch - toolbar_h - 9;  /* minus toolbar and hint bar */
    int vis_rows = text_h / 9;

    /* Compute cursor row/col for scroll adjustment */
    int cur_row, cur_col;
    ed_cursor_pos(&cur_row, &cur_col);

    /* Auto-scroll: keep cursor visible */
    if(cur_row < ed_scroll) ed_scroll = cur_row;
    if(cur_row >= ed_scroll + vis_rows) ed_scroll = cur_row - vis_rows + 1;
    if(ed_scroll < 0) ed_scroll = 0;

    /* Draw lines */
    for(int r = 0; r < vis_rows; r++) {
        int line_num = ed_scroll + r;
        int ry = text_y + r * 9;

        /* Line number gutter (3 chars wide) */
        uint8_t gutter_bg = rgb_to_pal(20, 20, 40);
        gfx_fill(cx, ry, 20, 9, gutter_bg);
        char lnbuf[5]; itoa(line_num + 1, lnbuf);
        gfx_str(cx + 1, ry + 1, lnbuf, rgb_to_pal(80, 80, 120), gutter_bg);

        /* Content area */
        char *lp = ed_line_start(line_num);
        if(!lp || *lp == '\0') {
            gfx_fill(cx + 20, ry, cw - 20, 9, C_BLACK);
            continue;
        }
        gfx_fill(cx + 20, ry, cw - 20, 9, C_BLACK);

        /* Draw characters on this line */
        int col = 0;
        char *p = lp;
        while(*p && *p != '\n' && col < ED_COLS) {
            int px = cx + 20 + col * 8;
            /* Cursor highlight */
            int char_idx = (int)(p - ed_buf);
            uint8_t fg = rgb_to_pal(200, 230, 200);
            uint8_t bg = C_BLACK;
            if(char_idx == ed_cursor) {
                fg = C_BLACK;
                bg = rgb_to_pal(0, 200, 80); /* cursor highlight */
            }
            gfx_char(px, ry + 1, *p, fg, bg);
            col++;
            p++;
        }
        /* Draw cursor at end of line if it's here */
        if(line_num == cur_row && (lp + cur_col) >= p) {
            int px = cx + 20 + col * 8;
            if(px < cx + cw - 8)
                gfx_fill(px, ry + 1, 2, 7, rgb_to_pal(0, 200, 80));
        }
    }

    /* ---- Filename prompt overlay ---- */
    if(ed_fname_mode) {
        int py = text_y + (vis_rows / 2) * 9 - 10;
        gfx_fill(cx + 10, py, cw - 20, 20, rgb_to_pal(40, 40, 100));
        gfx_rect(cx + 10, py, cw - 20, 20, rgb_to_pal(100, 140, 255));
        gfx_str(cx + 12, py + 2, "Open file:", rgb_to_pal(180, 200, 255), rgb_to_pal(40, 40, 100));
        gfx_str(cx + 12, py + 11, ed_fname_input, C_WHITE, rgb_to_pal(40, 40, 100));
        /* cursor */
        gfx_fill(cx + 12 + ed_fname_len * 8, py + 11, 2, 7, C_WHITE);
    }
}

void ed_key(int id, char k) {
    UNUSED(id);

    /* Filename prompt mode */
    if(ed_fname_mode) {
        if(k == '\n') {
            ed_fname_input[ed_fname_len] = '\0';
            /* Try to open the file */
            File *f = fs_find(ed_fname_input);
            if(f) {
                strncpy(ed_filename, ed_fname_input, 32);
                strncpy(ed_buf, f->content, ED_BUFSZ - 1);
                ed_buf[ED_BUFSZ - 1] = '\0';
                ed_len = strlen(ed_buf);
            } else {
                /* Create new file */
                strncpy(ed_filename, ed_fname_input, 32);
                ed_buf[0] = '\0';
                ed_len = 0;
            }
            ed_cursor = 0;
            ed_scroll = 0;
            ed_modified = false;
            ed_fname_mode = false;
            ed_fname_input[0] = '\0';
            ed_fname_len = 0;
        } else if(k == 27) {
            ed_fname_mode = false;
            ed_fname_input[0] = '\0';
            ed_fname_len = 0;
        } else if(k == '\b') {
            if(ed_fname_len > 0) { ed_fname_len--; ed_fname_input[ed_fname_len] = '\0'; }
        } else if(k >= ' ' && k <= '~' && ed_fname_len < 30) {
            ed_fname_input[ed_fname_len++] = k;
            ed_fname_input[ed_fname_len] = '\0';
        }
        wm_needs_redraw = true;
        return;
    }

    if(k == 27) {
        /* ESC: close editor */
        wm_close(id);
        wm_needs_redraw = true;
        return;
    }

    if(k == '\t') {
        /* TAB: save file */
        if(ed_filename[0] == '\0') {
            /* No filename yet — open prompt */
            ed_fname_mode = true;
            ed_fname_input[0] = '\0';
            ed_fname_len = 0;
            wm_needs_redraw = true;
            return;
        }
        File *f = fs_find(ed_filename);
        if(!f) f = fs_create(ed_filename, NULL);
        if(f) { fs_write(f, ed_buf); ed_modified = false; }
        wm_needs_redraw = true;
        return;
    }

    /* Ctrl+O = open file prompt */
    if(k == 15) { /* Ctrl+O */
        ed_fname_mode = true;
        ed_fname_input[0] = '\0';
        ed_fname_len = 0;
        wm_needs_redraw = true;
        return;
    }

    /* Regular editing */
    if(k == '\b') {
        if(ed_cursor > 0) {
            ed_cursor--;
            for(int i = ed_cursor; i < ed_len; i++) ed_buf[i] = ed_buf[i + 1];
            ed_len--;
            ed_modified = true;
        }
    } else if(k == '\n') {
        if(ed_len < ED_BUFSZ - 1) {
            for(int i = ed_len; i > ed_cursor; i--) ed_buf[i] = ed_buf[i - 1];
            ed_buf[ed_cursor] = '\n';
            ed_cursor++;
            ed_len++;
            ed_buf[ed_len] = '\0';
            ed_modified = true;
        }
    } else if(k >= 32 && k <= 126) {
        if(ed_len < ED_BUFSZ - 1) {
            for(int i = ed_len; i > ed_cursor; i--) ed_buf[i] = ed_buf[i - 1];
            ed_buf[ed_cursor] = k;
            ed_cursor++;
            ed_len++;
            ed_buf[ed_len] = '\0';
            ed_modified = true;
        }
    }
    wm_needs_redraw = true;
}

void ed_click(int id, int lx, int ly) {
    Window *w = &wins[id];
    int toolbar_h = 10;
    int cw = w->w - BORDER * 2;
    int text_y = toolbar_h;
    int hint_h = 9;
    int text_h = w->h - TITLE_H - toolbar_h - hint_h;
    int vis_rows = text_h / 9;
    UNUSED(cw);

    if(ly < toolbar_h || ly >= toolbar_h + text_h) return;

    int clicked_row = (ly - text_y) / 9 + ed_scroll;
    int clicked_col = (lx - 20) / 8;
    if(clicked_col < 0) clicked_col = 0;

    /* Move cursor to clicked position */
    char *lp = ed_line_start(clicked_row);
    if(!lp) return;
    int col = 0;
    char *p = lp;
    while(*p && *p != '\n' && col < clicked_col) { p++; col++; }
    ed_cursor = (int)(p - ed_buf);
    if(ed_cursor > ed_len) ed_cursor = ed_len;

    UNUSED(vis_rows);
    wm_needs_redraw = true;
}

void open_editor_with(const char *filename) {
    /* Reuse existing window if open */
    if(ed_win >= 0 && wins[ed_win].active) {
        win_focus = ed_win;
        wm_needs_redraw = true;
        /* If a new filename was requested, load it */
        if(filename && filename[0]) {
            strncpy(ed_filename, filename, 32);
            File *f = fs_find(filename);
            if(f) {
                strncpy(ed_buf, f->content, ED_BUFSZ - 1);
                ed_buf[ED_BUFSZ - 1] = '\0';
                ed_len = strlen(ed_buf);
            } else {
                ed_buf[0] = '\0'; ed_len = 0;
            }
            ed_cursor = 0; ed_scroll = 0; ed_modified = false;
        }
        return;
    }
    /* Fresh open */
    if(filename && filename[0]) {
        strncpy(ed_filename, filename, 32);
        File *f = fs_find(filename);
        if(f) {
            strncpy(ed_buf, f->content, ED_BUFSZ - 1);
            ed_buf[ED_BUFSZ - 1] = '\0';
            ed_len = strlen(ed_buf);
        } else {
            ed_buf[0] = '\0'; ed_len = 0;
        }
    } else {
        ed_filename[0] = '\0';
        ed_buf[0] = '\0'; ed_len = 0;
    }
    ed_cursor = 0; ed_scroll = 0; ed_modified = false;
    ed_fname_mode = false; ed_fname_input[0] = '\0'; ed_fname_len = 0;

    /* Window: 300×140, centered-ish */
    ed_win = wm_open("Text Editor", 15, 20, 300, 160,
                     ed_draw, ed_click, ed_key, rgb_to_pal(60, 0, 120));
}

void open_editor() {
    open_editor_with(NULL);
}

/* ───────────────────────────────────────────────────────────────────────────
   G8. GUI MAIN LOOP
   ─────────────────────────────────────────────────────────────────────────── */

/* Non-blocking keyboard check (returns 0 if no key ready) */
char kb_try_read() {
    if(!(inb(0x64) & 1)) return 0;  /* no data */
    uint8_t sc = inb(0x60);
    if(sc == 0xE0) { inb(0x60); return 0; } /* consume extended, skip */
    if(sc & 0x80) return 0;  /* key release, ignore */
    if(sc == 0x2A || sc == 0x36) { shift_held = true;  return 0; }
    if(sc == 0x1D) { ctrl_held  = true;  return 0; }
    if(sc < 128) {
        char c = shift_held ? keymap_hi[sc] : keymap_lo[sc];
        return c ? c : 0;
    }
    return 0;
}

/* --- THE FINAL SECTION OF YOUR KERNEL.C --- */

void kernel_main_text();

void gui_run() {
    gui_running = true;
    gui_mode    = true;   /* suppress all VGA text-mode writes and blocking get_key */

    vga_write_regs();
    vga_set_palette(); 
    mouse_init();

    /* Clear everything */
    for(int i=0; i<MAX_WINS; i++) wins[i].active = false;
    win_focus = -1;
    icon_count = 0;

    /* Icons: two columns so all 6 fit above the taskbar
       Col 1 x=8:  Term / Files / Edit / Calc
       Col 2 x=50: Apps                                  */
    icon_add( 8,  8,  "Term",  C_DKGREEN,             open_terminal);
    icon_add( 8, 52,  "Files", C_DKBLUE,              open_filemanager);
    icon_add( 8, 96,  "Edit",  rgb_to_pal(60,0,120),  open_editor);
    icon_add( 8, 140, "Calc",  C_BROWN,               open_calculator);
    icon_add(50,  8,  "Apps",  rgb_to_pal(20,50,150), open_apps);

    while(gui_running) {
        mouse_poll();

        /* Drain all pending keyboard scancodes this frame */
        static bool gui_ext = false;   /* saw 0xE0 prefix last byte */
        while(1) {
            uint8_t stat = inb(0x64);
            if(!(stat & 0x01)) break;   /* no data */
            if(stat & 0x20)  break;     /* it's mouse data, leave for mouse_poll */
            uint8_t sc = inb(0x60);

            /* Extended prefix — remember it, read the next byte next iteration */
            if(sc == 0xE0) { gui_ext = true; continue; }

            /* Track modifier key make/break (non-extended) */
            if(!gui_ext) {
                if(sc == 0x2A || sc == 0x36)  { shift_held = true;  continue; }
                if(sc == 0xAA || sc == 0xB6)  { shift_held = false; continue; }
                if(sc == 0x1D)                { ctrl_held  = true;  continue; }
                if(sc == 0x9D)                { ctrl_held  = false; continue; }
            }

            bool released = (sc & 0x80) != 0;
            uint8_t code  = sc & 0x7F;

            if(gui_ext) {
                gui_ext = false;
                /* Extended key releases — only track ctrl (right ctrl = 0x1D ext) */
                if(released) {
                    if(code == 0x1D) ctrl_held = false;
                    continue;
                }
                /* Map arrow / nav scancodes to our KEY_* sentinel chars */
                char nav = 0;
                if(code == 0x48) nav = (char)KEY_UP;
                else if(code == 0x50) nav = (char)KEY_DOWN;
                else if(code == 0x4B) nav = (char)KEY_LEFT;
                else if(code == 0x4D) nav = (char)KEY_RIGHT;
                else if(code == 0x47) nav = (char)KEY_HOME;
                else if(code == 0x4F) nav = (char)KEY_END;
                else if(code == 0x49) nav = (char)KEY_PGUP;
                else if(code == 0x51) nav = (char)KEY_PGDN;
                else if(code == 0x53) nav = (char)KEY_DEL;
                if(nav) wm_handle_key(nav);
                continue;
            }

            if(released) continue;  /* ignore key releases for normal keys */

            if(code < 128) {
                char c = shift_held ? keymap_hi[code] : keymap_lo[code];
                if(ctrl_held && c >= 'a' && c <= 'z') c = c - 'a' + 1;
                if(c == 27) { gui_running = false; break; }
                else if(c) wm_handle_key(c);
            }
        }
        
        /* App Opening Logic */
        if(mouse_clicked && wm_hit(mouse_x, mouse_y) < 0) {
            desktop_click(mouse_x, mouse_y);
        }
        wm_handle_mouse();
        mouse_clicked = false;
        mouse_released = false;

        draw_desktop(); 
        wm_draw_all();
        cursor_draw(mouse_x, mouse_y);

        gfx_flip();

        for(volatile int i=0; i<1500; i++);
    }

    cmd_reboot();  /* unreachable — gui_running never set false except by ESC */
}
/* THE ABSOLUTE END OF THE FILE */
void kernel_main() {
    kernel_main_text();
}