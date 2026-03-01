// Snake - classic snake game with VT100 color
//
// Arrow keys to move, Ctrl+C to quit.
// Eat the red food to grow. Don't hit walls or yourself!

#define MAX_LEN 256
#define SPEED_H 100   // ms per frame moving horizontally
#define SPEED_V 180   // ms per frame moving vertically (cells are 8x16)

int sx[MAX_LEN], sy[MAX_LEN]; // snake body ring buffer
int head, tail, slen;
int fx, fy;       // food position
int dx, dy;       // direction
int w, h;         // play area
int score;
int alive;
int rng;          // PRNG state

// Simple LCG — avoids cc runtime rand() issues
int my_rand(int max) {
    rng = rng * 1103515245 + 12345;
    return ((rng >> 16) & 0x7FFF) % max;
}

void cursor(int x, int y) { printf("\033[%d;%dH", y, x); }
void color(int c)          { printf("\033[%dm", c); }
void bold_color(int c)     { printf("\033[1;%dm", c); }

void hide_cursor() { printf("\033[?25l"); }
void show_cursor() { printf("\033[?25h"); }

void draw_at(int x, int y, char c) {
    cursor(x, y);
    putchar(c);
}

void place_food() {
    int ok, i, tries;
    tries = 0;
    while (tries < 1000) {
        fx = my_rand(w - 2) + 2;
        fy = my_rand(h - 2) + 2;
        // make sure food doesn't land on snake
        ok = 1;
        i = tail;
        while (i != head) {
            if (sx[i] == fx && sy[i] == fy) { ok = 0; break; }
            i = (i + 1) % MAX_LEN;
        }
        if (sx[head] == fx && sy[head] == fy) ok = 0;
        if (ok) break;
        tries = tries + 1;
    }
    bold_color(31); // bright red
    draw_at(fx, fy, '*');
    color(0);
}

void draw_border() {
    int i;
    color(33); // yellow
    // top + bottom
    for (i = 1; i <= w; i++) {
        draw_at(i, 1, '#');
        draw_at(i, h, '#');
    }
    // left + right
    for (i = 1; i <= h; i++) {
        draw_at(1, i, '#');
        draw_at(w, i, '#');
    }
    color(0);
}

void draw_score() {
    cursor(1, h + 1);
    bold_color(37); // bright white
    printf("Score: %d", score);
    color(0);
}

int read_key() {
    int c = getchar_timeout_us(0);
    if (c < 0) return -1;
    if (c == 3) return 'q';  // Ctrl+C
    if (c == 27) {
        // ESC sequence - read [ and direction
        c = getchar_timeout_us(50000);
        if (c != '[') return 27;
        c = getchar_timeout_us(50000);
        if (c == 'A') return 'u';  // up
        if (c == 'B') return 'd';  // down
        if (c == 'C') return 'r';  // right
        if (c == 'D') return 'l';  // left
        return -1;
    }
    return c;
}

void game_over() {
    int mx, my;
    mx = w / 2 - 6;
    my = h / 2;
    cursor(mx, my);
    bold_color(31);
    printf("  GAME OVER!  ");
    cursor(mx, my + 1);
    color(37);
    printf(" Score: %d ", score);
    cursor(mx, my + 2);
    printf(" Press a key  ");
    color(0);
    // flush input
    while (getchar_timeout_us(0) >= 0)
        ;
    // wait for key
    while (getchar_timeout_us(100000) < 0)
        ;
}

int main() {
    int key, nx, ny, i, ate, speed;

    w = screen_width();
    h = screen_height() - 1; // leave room for score line
    if (w > 78) w = 78;
    if (h > 28) h = 28;
    if (w < 16) w = 16;
    if (h < 10) h = 10;

    rng = time_us_32();

    // init screen
    printf("\033[H\033[J");
    hide_cursor();
    draw_border();

    // init snake in center, moving right
    slen = 3;
    head = 2;
    tail = 0;
    dx = 1;
    dy = 0;
    sx[0] = w / 2 - 2; sy[0] = h / 2;
    sx[1] = w / 2 - 1; sy[1] = h / 2;
    sx[2] = w / 2;     sy[2] = h / 2;

    // draw initial snake
    color(32); // green body
    for (i = tail; i < head; i++)
        draw_at(sx[i], sy[i], 'o');
    bold_color(32); // bright green head
    draw_at(sx[head], sy[head], 'O');
    color(0);

    score = 0;
    alive = 1;
    draw_score();
    place_food();

    while (alive) {
        // adjust speed: vertical cells are 2x taller than wide
        speed = (dy != 0) ? SPEED_V : SPEED_H;
        sleep_ms(speed);

        // read input - drain all pending keys, keep last direction
        while (1) {
            key = read_key();
            if (key < 0) break;
            if (key == 'q') { alive = 0; break; }
            if (key == 'u' && dy != 1)  { dx = 0;  dy = -1; }
            if (key == 'd' && dy != -1) { dx = 0;  dy = 1;  }
            if (key == 'l' && dx != 1)  { dx = -1; dy = 0;  }
            if (key == 'r' && dx != -1) { dx = 1;  dy = 0;  }
        }
        if (!alive) break;

        // compute new head position
        nx = sx[head] + dx;
        ny = sy[head] + dy;

        // wall collision
        if (nx <= 1 || nx >= w || ny <= 1 || ny >= h) {
            alive = 0;
            break;
        }

        // self collision
        i = tail;
        while (i != head) {
            if (sx[i] == nx && sy[i] == ny) { alive = 0; break; }
            i = (i + 1) % MAX_LEN;
        }
        if (!alive) break;

        // check food
        ate = (nx == fx && ny == fy);
        if (ate) {
            score = score + 10;
            slen = slen + 1;
            draw_score();
        }

        // move: old head becomes body
        color(32);
        draw_at(sx[head], sy[head], 'o');

        // advance head
        head = (head + 1) % MAX_LEN;
        sx[head] = nx;
        sy[head] = ny;

        // draw new head
        bold_color(32);
        draw_at(nx, ny, 'O');

        // erase tail (unless we just ate)
        if (!ate) {
            draw_at(sx[tail], sy[tail], ' ');
            tail = (tail + 1) % MAX_LEN;
        } else {
            place_food();
        }

        color(0);
    }

    game_over();
    show_cursor();
    printf("\033[H\033[J");
    return 0;
}
