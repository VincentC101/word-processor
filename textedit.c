/*** includes ****/
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>

/*** definitions ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define TAB_STOP 4
enum editorKey {
    BACKSPACE = 127,
    UP_ARROW = 1000,
    DOWN_ARROW,
    LEFT_ARROW,
    RIGHT_ARROW,
    PAGE_UP,
    PAGE_DOWN, 
    DEL_KEY, 
    END_KEY
};

/*** data ***/
typedef struct erow {
    int size;
    char* chars;
    int rsize;
    char *render;
} erow;

struct editorConfig {
    int cursor_x, cursor_y;
    int rx;
    int rowoffset;
    int coloffset;
    int screen_rows;
    int screen_cols;
    int num_rows;
    erow* row;
    bool dirty;
    char* filename;
    char message[100];
    struct termios orig_termios;
};
struct editorConfig E;

void editorSetStatusMessage(const char *fmt, ...);

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1){
        die("tcsetattr");
    }
}

void enableRaw(void){
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1){
        die("tcgetattr");
    }

    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)==-1){
        die("tcsetattr");
    }
}

int readKey(void) {
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    if(c == '\x1b') {
        char seq[3];
        if(read(STDIN_FILENO, &seq[0], 1) != 1) return -1;
        if(read(STDIN_FILENO, &seq[1], 1) != 1) return -1;

        if(seq[0] == '[') {
            if(seq[1] >= '0' && seq[1] <= '9'){
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if(seq[2] == '~') {
                    switch(seq[1]) {
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6': 
                            return PAGE_DOWN;
                    }
                }
            } else {
                switch(seq[1]) {
                    case 'A':
                        return UP_ARROW;
                    case 'B':
                        return DOWN_ARROW;
                    case 'C':
                        return RIGHT_ARROW;
                    case 'D':
                        return LEFT_ARROW;

                }
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPos(int *rows, int *cols) {
    char buff[32];
    unsigned int i = 0;
    
    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while(i < sizeof(buff) - 1) {
        if(read(STDIN_FILENO, &buff[i], 1) != 1){
            break;
        }
        if(buff[i] == 'R') {
            break;
        }
        i++;
    }
    buff[i] = '\0';
    if (buff[0] != '\x1b' || buff[1] != '[') return -1;
    if (sscanf(&buff[2], "%d;%d", rows, cols) != 2) return -1;
     return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return getCursorPos(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row ops */
int charToRender(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j=0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx +=  (TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j=0; j < row->size; j++) {
        if(row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);

    row->render = malloc(row->size + (tabs * (TAB_STOP - 1)) + 1);
    int idx = 0;
    for (j=0; j < row->size; j++) {
        if(row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while(idx % TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char* s, size_t len) {
    if(at < 0 || at > E.num_rows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));
    memmove(&E.row[at+1], &E.row[at], sizeof(erow) * (E.num_rows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.num_rows ++;
    E.dirty = true;
}

void editorFreeRow(erow* row) {
    free(row->render);
    free(row->chars);
}

void editorDeleteRow(int at) {
    if(at < 0 || at >= E.num_rows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.num_rows - at - 1));
    E.num_rows--;
}

void editorRowInsertChar(erow *row, int at, char c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
    row->size ++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty = true;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at > row->size) at = row->size;
    memmove(&row->chars[at], &row->chars[at+1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty = true;
}

/*** File IN OUT ***/
char* editorRowsToString(int* bufferlen) {
    int totlen = 0;
    int j;
    for (j=0; j < E.num_rows; j++) {
        totlen += E.row[j].size + 1;
    }
    *bufferlen += totlen;
    char* text = malloc(totlen);
    char *p = text;

    for (j=0; j < E.num_rows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return text;
}

void editorOpen(char* filename) {
    free(E.filename);
    E.filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
            line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.num_rows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = false;
}

void editorSave(void) {
    if (E.filename == NULL) return;

    int len;
    char* buffer = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0664);
    if (fd != -1){
        if (ftruncate(fd, len) != -1){
            if (write(fd, buffer, len) == len){
                close(fd);
                free(buffer);
                E.dirty = false;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buffer);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** editor ops ***/
void editorInsertChar(char c) {
    if(E.cursor_y == E.num_rows) {
        editorInsertRow(E.num_rows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}

void editorDeleteChar(void) {
    if(E.cursor_y == E.num_rows) {
        return;
    }
    if(E.cursor_x == 0 && E.cursor_y == 0) {
        return;
    }
    erow *row = &E.row[E.cursor_y];
    if(E.cursor_x > 0){
        editorRowDelChar(row, E.cursor_x-1);
        E.cursor_x--;
    } else {
        E.cursor_x = E.row[E.cursor_y - 1].size;
        editorRowAppendString(&E.row[E.cursor_y - 1], row->chars, row->size);
        editorDeleteRow(E.cursor_y);
        E.cursor_y--;
    }
}

void editorInsertNewline(void) {
    if(E.cursor_x == 0){
        editorInsertRow(E.cursor_y, "", 0);
    } else {
        erow *row = &E.row[E.cursor_y];
        editorInsertRow(E.cursor_y + 1, &row->chars[E.cursor_x], row->size - E.cursor_x);
        row = &E.row[E.cursor_y];
        row->size = E.cursor_x;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cursor_y++;
    E.cursor_x = 0;
}

/*** append buffer ***/
struct buffer {
    char *b;
    int len;
};
#define ABUF_INIT {NULL, 0}

void appendBuffer(struct buffer *ab, const char *s, int len){
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL){
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void freeBuffer(struct buffer *ab){
    free(ab->b);
}

/*** output ***/

void editorScroll(void) {
    E.rx = 0;
    if(E.cursor_y < E.num_rows) {
        E.rx = charToRender(&E.row[E.cursor_y], E.cursor_x);
    }

    if (E.cursor_y < E.rowoffset) {
        E.rowoffset = E.cursor_y;
    }
    if (E.cursor_y >= E.rowoffset + E.screen_rows) {
        E.rowoffset = E.cursor_y - E.screen_rows + 1;
    }
    if (E.rx < E.coloffset) {
        E.coloffset = E.rx;
    }
    if (E.rx >= E.coloffset + E.screen_cols) {
        E.coloffset = E.rx - E.screen_cols + 1;
    }
}

void drawRows(struct buffer *ab){
    int y;
    for (y = 0; y<E.screen_rows; y++){
        int filerow = y + E.rowoffset;
        if(filerow >= E.num_rows) {
            if (E.num_rows == 0 && y == E.screen_rows / 3) { //welcome message will delete later
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- version 0.1");
                if (welcomelen > E.screen_cols) welcomelen = E.screen_cols; 
                appendBuffer(ab, welcome, welcomelen);//welcome last line
            } else {
                appendBuffer(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloffset;
            if(len < 0) len = 0;
            if (len > E.screen_cols) len = E.screen_cols;
            appendBuffer(ab, &E.row[filerow].render[E.coloffset], len);
        }
        
        appendBuffer(ab, "\x1b[K", 3);

        appendBuffer(ab, "\r\n", 2);
    }
}

void drawStatusBar(struct buffer *ab) {
    appendBuffer(ab, "\x1b[7m", 4);
    char status[100];
    int len = snprintf(status, sizeof(status), "%.20s - line %d %s", E.filename ? E.filename : "[No Name]", 
    E.cursor_y+1, E.dirty ? "(modified)" : "");
    if (len > E.screen_cols) len = E.screen_cols;
    appendBuffer(ab, status, len);
    while (len < E.screen_cols){
        appendBuffer(ab, " ", 1);
        len++;
    }
    appendBuffer(ab, "\x1b[m", 3);
    appendBuffer(ab, "\r\n", 2);
}

void drawMessageBar(struct buffer *ab) {
    appendBuffer(ab, "\x1b[K", 3);
    appendBuffer(ab, "\x1b[7m", 4);
    char* status = E.message;
    int len = strlen(E.message);
    if (len > E.screen_cols) len = E.screen_cols;
    appendBuffer(ab, status, len);
    while (len < E.screen_cols){
        appendBuffer(ab, " ", 1);
        len++;
    }
    appendBuffer(ab, "\x1b[m", 3);
}

void clearScreen(void) {
    editorScroll();
    struct buffer ab = ABUF_INIT;

    appendBuffer(&ab, "\x1b[?25l", 6);
    appendBuffer(&ab, "\x1b[H", 3);

    drawRows(&ab);
    drawStatusBar(&ab);
    drawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.rowoffset) + 1, (E.rx - E.coloffset) + 1);
    appendBuffer(&ab, buf, strlen(buf));

    appendBuffer(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    freeBuffer(&ab);
}

void editorSetStatusMessage(const char* fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.message, sizeof(E.message), fmt, ap);
    va_end(ap);
}
/*** input ***/

void moveCursor(int key) {
    erow *row = (E.cursor_y >= E.num_rows) ? NULL : &E.row[E.cursor_y];

    switch(key) {
        case UP_ARROW:
            if(E.cursor_y != 0) {
                E.cursor_y--;
            }
            break;
        case DOWN_ARROW:
            if(E.cursor_y < E.num_rows) {
                E.cursor_y++;
            }
            break;
        case LEFT_ARROW:
            if(E.cursor_x != 0) {
                E.cursor_x--;
            } else if (E.cursor_y > 0) {
                E.cursor_y --;
                E.cursor_x = E.row[E.cursor_y].size;
            }
            break;
        case RIGHT_ARROW:
            if (row && E.cursor_x < row->size) {
                E.cursor_x++;
            } else if (row && E.cursor_x == row->size) {
                E.cursor_y ++;
                E.cursor_x = 0;
            }
            break;
    }
    row = (E.cursor_y >= E.num_rows) ? NULL : &E.row[E.cursor_y];
    int rowlen = row ? row->size : 0;
    if(E.cursor_x > rowlen) {
        E.cursor_x = rowlen;
    }
}

void processKey(void) {
    static int quit_times = 1;
    int c = readKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) moveCursor(RIGHT_ARROW);
            editorDeleteChar();
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0){
                editorSetStatusMessage("You have unsaved changes, are you sure you want to quit?? "
                "Press CTRL-Q again to quit.");
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cursor_y = E.rowoffset;
                } else if (c == PAGE_DOWN) {
                    E.cursor_y = E.rowoffset + E.screen_rows - 1;
                    if (E.cursor_y > E.num_rows) {
                        E.cursor_y = E.num_rows;
                    }
                }
                int times = E.screen_rows;
                while (times--){
                    moveCursor(c == PAGE_UP ? UP_ARROW : DOWN_ARROW);
                }
            }
            break;
        
        case END_KEY:
            if(E.cursor_y < E.num_rows) {
                E.cursor_x = E.row[E.cursor_y].size;
            }
        
        case UP_ARROW:
        case DOWN_ARROW:
        case LEFT_ARROW:
        case RIGHT_ARROW:
            moveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }
    quit_times = 2;
}

/*** init ***/
void initializeEditor(void){
    E.cursor_x = 0;
    E.cursor_y = 0;
    E.rowoffset = 0;
    E.coloffset = 0;
    E.rx = 0;
    if(getWindowSize(&E.screen_rows, &E.screen_cols) == -1) {
        die("getWindowSize");
    }
    E.screen_rows -= 2;
    E.num_rows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.dirty = false;
    E.message[0] = '\0';
}

int main(int argc, char* argv[]){
    enableRaw();
    initializeEditor();
    if (argc >= 2) {
    editorOpen(argv[1]);
    }
    editorSetStatusMessage("CTRL-1: Show Help Menu | Ctrl-S = save | Ctrl-Q = quit");

    while(1) {
        clearScreen();
        processKey();
    }

    return 0;
}
