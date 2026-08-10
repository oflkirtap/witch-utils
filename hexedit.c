#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define HEX_VERSION "1.0-witch"
#define MODE_BREW 0
#define MODE_CHANT 1

/* arrow key mappings outside standard char range */
enum KeyMappings {
    ARROW_UP = 1000,
    ARROW_DOWN,
    ARROW_RIGHT,
    ARROW_LEFT
};

/* --- WITCHCRAFT & DATA STRUCTURES --- */

typedef struct {
    int size;
    int cap;
    char *runes;
} Spell;

struct Grimoire {
    int cx, cy;          
    int rowoff, coloff;  
    int num_spells;      
    int spells_cap;      
    Spell *spells;       
    char *scroll;        
    int mode;            
    int screenrows;
    int screencols;
    int dirty;           
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
} cauldron;

/* --- MEMORY SAFETY & ERROR HANDLING --- */

void fizzle(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void exit_grimoire(int status) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    for (int i = 0; i < cauldron.num_spells; i++) {
        free(cauldron.spells[i].runes);
    }
    free(cauldron.spells);
    exit(status);
}

void *safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) fizzle("Out of mana (malloc failed)");
    return ptr;
}

void *safe_realloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr) fizzle("Out of mana (realloc failed)");
    return new_ptr;
}

/* --- TERMINAL ALCHEMY --- */

void break_spell() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &cauldron.orig_termios) == -1)
        fizzle("tcsetattr");
}

void cast_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &cauldron.orig_termios) == -1) fizzle("tcgetattr");
    atexit(break_spell);

    struct termios raw = cauldron.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) fizzle("tcsetattr");
}

void get_cauldron_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        fizzle("ioctl");
    } else {
        cauldron.screencols = ws.ws_col;
        cauldron.screenrows = ws.ws_row;
    }
}

/* --- STATUS MESSAGE --- */
void set_status_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cauldron.statusmsg, sizeof(cauldron.statusmsg), fmt, ap);
    va_end(ap);
    cauldron.statusmsg_time = time(NULL);
}

/* --- SPELLBOOK (BUFFER) OPERATIONS --- */

void insert_spell(int at, const char *s, size_t len) {
    if (at < 0 || at > cauldron.num_spells) return;
    if (cauldron.num_spells >= cauldron.spells_cap) {
        cauldron.spells_cap = (cauldron.spells_cap == 0) ? 16 : cauldron.spells_cap * 2;
        cauldron.spells = safe_realloc(cauldron.spells, sizeof(Spell) * cauldron.spells_cap);
    }
    memmove(&cauldron.spells[at + 1], &cauldron.spells[at], sizeof(Spell) * (cauldron.num_spells - at));

    cauldron.spells[at].size = len;
    cauldron.spells[at].cap = len + 1;
    cauldron.spells[at].runes = safe_malloc(len + 1);
    memcpy(cauldron.spells[at].runes, s, len);
    cauldron.spells[at].runes[len] = '\0';

    cauldron.num_spells++;
    cauldron.dirty = 1;
}

void append_spell(const char *s, size_t len) {
    insert_spell(cauldron.num_spells, s, len);
}

void insert_rune(Spell *spell, int at, int c) {
    if (at < 0 || at > spell->size) at = spell->size;
    if (spell->size + 2 > spell->cap) {
        spell->cap = (spell->cap == 0) ? 16 : spell->cap * 2;
        if (spell->cap < spell->size + 2) spell->cap = spell->size + 2;
        spell->runes = safe_realloc(spell->runes, spell->cap);
    }
    memmove(&spell->runes[at + 1], &spell->runes[at], spell->size - at + 1);
    spell->size++;
    spell->runes[at] = c;
    cauldron.dirty = 1;
}

void delete_spell(int at) {
    if (at < 0 || at >= cauldron.num_spells) return;
    free(cauldron.spells[at].runes);
    memmove(&cauldron.spells[at], &cauldron.spells[at + 1], sizeof(Spell) * (cauldron.num_spells - at - 1));
    cauldron.num_spells--;
    cauldron.dirty = 1;
}

void save_scroll() {
    if (cauldron.scroll == NULL) {
        set_status_msg("Error: No scroll bound! Launch with ./hexedit <filename>");
        return;
    }
    FILE *fp = fopen(cauldron.scroll, "w");
    if (!fp) {
        set_status_msg("Error: Cannot write to scroll %s", cauldron.scroll);
        return;
    }
    for (int i = 0; i < cauldron.num_spells; i++) {
        fwrite(cauldron.spells[i].runes, cauldron.spells[i].size, 1, fp);
        fwrite("\n", 1, 1, fp);
    }
    fclose(fp);
    cauldron.dirty = 0;
    set_status_msg("Scroll preserved safely.");
}

/* --- SCREEN RENDERING --- */

struct abuf { char *b; int len; int cap; };
#define ABUF_INIT {NULL, 0, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->cap) {
        ab->cap = (ab->cap == 0) ? 128 : ab->cap * 2;
        if (ab->len + len > ab->cap) ab->cap = ab->len + len;
        ab->b = safe_realloc(ab->b, ab->cap);
    }
    memcpy(&ab->b[ab->len], s, len);
    ab->len += len;
}

void scroll_cauldron() {
    if (cauldron.cy < cauldron.rowoff) cauldron.rowoff = cauldron.cy;
    if (cauldron.cy >= cauldron.rowoff + cauldron.screenrows - 2) {
        cauldron.rowoff = cauldron.cy - cauldron.screenrows + 3;
    }
    if (cauldron.cx < cauldron.coloff) cauldron.coloff = cauldron.cx;
    if (cauldron.cx >= cauldron.coloff + cauldron.screencols) {
        cauldron.coloff = cauldron.cx - cauldron.screencols + 1;
    }
}

void draw_spells(struct abuf *ab) {
    /* make room for 2 lines at the bottom (status bar and message bar) */
    for (int y = 0; y < cauldron.screenrows - 2; y++) {
        int filerow = y + cauldron.rowoff;
        
        if (filerow < cauldron.num_spells) {
            int len = cauldron.spells[filerow].size - cauldron.coloff;
            if (len < 0) len = 0;
            if (len > cauldron.screencols) len = cauldron.screencols;
            if (len > 0) abAppend(ab, &cauldron.spells[filerow].runes[cauldron.coloff], len);
        } else if (cauldron.num_spells == 0 && y == cauldron.screenrows / 3) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome), "HexEdit - Witch's Grimoire v%s", HEX_VERSION);
            if (welcomelen > cauldron.screencols) welcomelen = cauldron.screencols;
            int padding = (cauldron.screencols - welcomelen) / 2;
            if (padding) { abAppend(ab, "~", 1); padding--; }
            while (padding--) abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcomelen);
        } else {
            abAppend(ab, "~", 1);
        }
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void draw_status_bar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80];
    char modified = cauldron.dirty ? '*' : ' ';
    
    int len = snprintf(status, sizeof(status), " [%s] - %.20s %c | L:%d ", 
        cauldron.mode == MODE_CHANT ? "CHANTING" : "BREWING",
        cauldron.scroll ? cauldron.scroll : "[Unbound Scroll]",
        modified,
        cauldron.cy + 1);
    
    if (len > cauldron.screencols) len = cauldron.screencols;
    abAppend(ab, status, len);
    while (len < cauldron.screencols) { abAppend(ab, " ", 1); len++; }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void draw_message_bar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(cauldron.statusmsg);
    if (msglen > cauldron.screencols) msglen = cauldron.screencols;
    if (msglen && time(NULL) - cauldron.statusmsg_time < 3)
        abAppend(ab, cauldron.statusmsg, msglen);
}

void refresh_screen() {
    scroll_cauldron();
    
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    draw_spells(&ab);
    draw_status_bar(&ab);
    draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (cauldron.cy - cauldron.rowoff) + 1, (cauldron.cx - cauldron.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    free(ab.b);
}

/* --- INPUT HANDLING --- */

int scry_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) fizzle("read");
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        }
        return '\x1b';
    }
    return c;
}

void move_cursor(int key) {
    Spell *spell = (cauldron.cy >= cauldron.num_spells) ? NULL : &cauldron.spells[cauldron.cy];

    switch (key) {
        case 'h': case ARROW_LEFT: 
            if (cauldron.cx > 0) cauldron.cx--; 
            else if (cauldron.cy > 0) { cauldron.cy--; cauldron.cx = cauldron.spells[cauldron.cy].size; } 
            break;
        case 'l': case ARROW_RIGHT: 
            if (spell && cauldron.cx < spell->size) cauldron.cx++; 
            else if (spell && cauldron.cx == spell->size) { cauldron.cy++; cauldron.cx = 0; } 
            break;
        case 'k': case ARROW_UP: 
            if (cauldron.cy > 0) cauldron.cy--; 
            break;
        case 'j': case ARROW_DOWN: 
            if (cauldron.cy < cauldron.num_spells - 1) cauldron.cy++; 
            break;
    }

    /* snap cursor to end of shorter lines */
    spell = (cauldron.cy >= cauldron.num_spells) ? NULL : &cauldron.spells[cauldron.cy];
    int spell_len = spell ? spell->size : 0;
    if (cauldron.cx > spell_len) cauldron.cx = spell_len;
}

void process_keypress() {
    static int quit_pending = 0;
    int c = scry_key();

    if (cauldron.mode == MODE_BREW) {
        switch (c) {
            case 'q':
                if (cauldron.dirty && !quit_pending) {
                    set_status_msg("Unsaved changes! Press 'q' again to quit without saving.");
                    quit_pending = 1;
                    return;
                }
                exit_grimoire(0);
            case 'i': cauldron.mode = MODE_CHANT; set_status_msg(""); break;
            case 'w': save_scroll(); break;
            case 'd': {
                delete_spell(cauldron.cy);
                if (cauldron.cy >= cauldron.num_spells && cauldron.cy > 0) cauldron.cy--;

                /* snap cursor to bounds after deletion */
                Spell *spell = (cauldron.cy < cauldron.num_spells) ? &cauldron.spells[cauldron.cy] : NULL;
                int spell_len = spell ? spell->size : 0;
                if (cauldron.cx > spell_len) cauldron.cx = spell_len;
                break;
            }
            case 'h': case 'j': case 'k': case 'l':
            case ARROW_UP: case ARROW_DOWN: case ARROW_LEFT: case ARROW_RIGHT:
                move_cursor(c); break;
        }
    } else { /* MODE_CHANT */
        if (c == '\x1b') { 
            cauldron.mode = MODE_BREW;
            if (cauldron.cx > 0) cauldron.cx--; 
        } else if (c == ARROW_UP || c == ARROW_DOWN || c == ARROW_LEFT || c == ARROW_RIGHT) {
            move_cursor(c);
        } else if (c == '\r') {
            if (cauldron.cy == cauldron.num_spells) {
                insert_spell(cauldron.num_spells, "", 0);
            } else {
                Spell *spell = &cauldron.spells[cauldron.cy];
                insert_spell(cauldron.cy + 1, &spell->runes[cauldron.cx], spell->size - cauldron.cx);
                spell = &cauldron.spells[cauldron.cy];
                spell->size = cauldron.cx;
                spell->runes[spell->size] = '\0';
            }
            cauldron.cy++;
            cauldron.cx = 0;
            cauldron.dirty = 1;
        } else if (c == 127) { /* backspace */
            if (cauldron.cx > 0) {
                Spell *spell = &cauldron.spells[cauldron.cy];
                memmove(&spell->runes[cauldron.cx - 1], &spell->runes[cauldron.cx], spell->size - cauldron.cx + 1);
                spell->size--;
                cauldron.cx--;
                cauldron.dirty = 1;
            } else if (cauldron.cx == 0 && cauldron.cy > 0) {
                /* pull current line up to previous line */
                Spell *current = &cauldron.spells[cauldron.cy];
                Spell *prev = &cauldron.spells[cauldron.cy - 1];
                cauldron.cx = prev->size; 

                int needed = prev->size + current->size + 1;
                if (needed > prev->cap) {
                    prev->cap = needed;
                    prev->runes = safe_realloc(prev->runes, prev->cap);
                }
                memcpy(&prev->runes[prev->size], current->runes, current->size);
                prev->size += current->size;
                prev->runes[prev->size] = '\0';

                delete_spell(cauldron.cy);
                cauldron.cy--;
                cauldron.dirty = 1;
            }
        } else if (c == '\t') { /* tab expansion to 4 spaces */
            for (int i = 0; i < 4; i++) {
                if (cauldron.cy == cauldron.num_spells) insert_spell(cauldron.num_spells, "", 0);
                insert_rune(&cauldron.spells[cauldron.cy], cauldron.cx, ' ');
                cauldron.cx++;
            }
        } else if (!iscntrl((unsigned char)c)) {
            if (cauldron.cy == cauldron.num_spells) insert_spell(cauldron.num_spells, "", 0);
            insert_rune(&cauldron.spells[cauldron.cy], cauldron.cx, c);
            cauldron.cx++;
        }
    }

    quit_pending = 0;
}

/* --- INVOCATION --- */

void init_cauldron() {
    cauldron.cx = 0;
    cauldron.cy = 0;
    cauldron.rowoff = 0;
    cauldron.coloff = 0;
    cauldron.num_spells = 0;
    cauldron.spells_cap = 0;
    cauldron.spells = NULL;
    cauldron.scroll = NULL;
    cauldron.mode = MODE_BREW;
    cauldron.dirty = 0;
    cauldron.statusmsg[0] = '\0';
    cauldron.statusmsg_time = 0;
    get_cauldron_size();
}

int main(int argc, char *argv[]) {
    cast_raw_mode();
    init_cauldron();
    
    if (argc >= 2) {
        cauldron.scroll = argv[1];
        FILE *fp = fopen(cauldron.scroll, "r");
        if (fp) {
            char *line = NULL;
            size_t linecap = 0;
            ssize_t linelen;
            while ((linelen = getline(&line, &linecap, fp)) != -1) {
                while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
                    linelen--;
                append_spell(line, linelen);
            }
            free(line);
            fclose(fp);
            cauldron.dirty = 0; 
        }
    }

    set_status_msg("HexEdit - Witch's Grimoire v%s | Press 'i' to Chants", HEX_VERSION);

    while (1) {
        refresh_screen();
        process_keypress();
    }
    return 0;
}
