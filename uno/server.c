#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h> 

#define MAX_PLAYERS 5
#define HAND_SIZE 5
#define MAX_HAND 8
#define PORT 9000
#define DECK_SIZE 108
#define LOG_BUF 256

/* ---------- CARD TYPES ---------- */
typedef enum { RED, BLUE, GREEN, YELLOW } Color;
typedef enum { NUMBER, SKIP, PLUS2 } Type;

typedef struct {
    Color color;
    Type type;
    int number; // -1 for special
} Card;

/* ---------- SHARED GAME STATE ---------- */
typedef struct {
    int players;
    int current_turn;
    int game_over;
    int active[MAX_PLAYERS];
    int no_card_count[MAX_PLAYERS];
    Card top_card;

    /* Hands */
    Card hands[MAX_PLAYERS][20];
    int hand_count[MAX_PLAYERS];

    /* Deck */
    Card deck[DECK_SIZE];
    int deck_top; // next card to draw

    /* Scores */
    int scores[MAX_PLAYERS];

    pthread_mutex_t game_mutex;   // protects game state
    pthread_mutex_t score_mutex;  // protects scores.txt updates
} GameState;

GameState *game;
int log_pipe[2]; // pipe for logger thread

/* ---------- UTIL STRINGS ---------- */
const char *color_str(Color c) {
    static const char *names[] = {"RED","BLUE","GREEN","YELLOW"};
    return names[c];
}

const char *type_str(Type t) {
    return t == NUMBER ? "NUMBER" : (t == SKIP ? "SKIP" : "+2");
}

/* ---------- SIGCHLD HANDLER (NO ZOMBIES) ---------- */
void sigchld_handler(int s) {
    (void)s;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* ---------- LOGGER THREAD ---------- */
void *logger_thread(void *arg) {
    (void)arg;
    char buf[LOG_BUF];
    FILE *f = fopen("game.log", "a");
    if (!f) {
        perror("fopen game.log");
        return NULL;
    }

    while (1) {
        ssize_t n = read(log_pipe[0], buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            fprintf(f, "%s", buf);
            fflush(f);
        }
    }
    fclose(f);
    return NULL;
}

/* ---------- LOG HELPER ---------- */
void log_msg(const char *fmt, ...) {
    char buf[LOG_BUF];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    write(log_pipe[1], buf, strlen(buf));
}

/* ---------- BUILD FULL UNO DECK ---------- */
void build_deck() {
    int idx = 0;
    for (int c = 0; c < 4; c++) {
        /* One 0 per color */
        game->deck[idx++] = (Card){c, NUMBER, 0};
        /* Two of 1-9 per color */
        for (int n = 1; n <= 9; n++) {
            game->deck[idx++] = (Card){c, NUMBER, n};
            game->deck[idx++] = (Card){c, NUMBER, n};
        }
        /* Two SKIP and two +2 per color */
        game->deck[idx++] = (Card){c, SKIP, -1};
        game->deck[idx++] = (Card){c, SKIP, -1};
        game->deck[idx++] = (Card){c, PLUS2, -1};
        game->deck[idx++] = (Card){c, PLUS2, -1};
    }
    game->deck_top = 0;
}

void shuffle_deck() {
    for (int i = DECK_SIZE-1; i > 0; i--) {
        int j = rand() % (i+1);
        Card tmp = game->deck[i];
        game->deck[i] = game->deck[j];
        game->deck[j] = tmp;
    }
}

Card draw_card() {
    if (game->deck_top >= DECK_SIZE) {
        build_deck();
        shuffle_deck();
    }
    return game->deck[game->deck_top++];
}

/* ---------- VALID MOVE ---------- */
int valid_move(Card a, Card b) {
    if (a.color == b.color) return 1;
    if (a.type == b.type) return 1;
    if (a.type == NUMBER && b.type == NUMBER && a.number == b.number)
        return 1;
    return 0;
}

/* ---------- NEXT ACTIVE PLAYER ---------- */
int next_player(int cur) {
    for (int i = 1; i <= game->players; i++) {
        int n = (cur + i) % game->players;
        if (game->active[n]) return n;
    }
    return -1;
}

/* ---------- CHECK LAST PLAYER ---------- */
void check_last_player() {
    int alive = 0, last = -1;
    for (int i = 0; i < game->players; i++) {
        if (game->active[i]) { alive++; last = i; }
    }
    if (alive == 1) {
        printf("\n PLAYER %d WINS THE GAME! \n", last);
        log_msg("PLAYER %d WINS THE GAME\n", last);

        /* Update persistent score */
        pthread_mutex_lock(&game->score_mutex);
        game->scores[last]++;
        FILE *f = fopen("scores.txt", "w");
        for (int i = 0; i < game->players; i++)
            fprintf(f, "Player %d: %d\n", i, game->scores[i]);
        fclose(f);
        pthread_mutex_unlock(&game->score_mutex);

        game->game_over = 1;
    }
}

/* ---------- BUILD MENU FOR CLIENT ---------- */
void build_menu(int pid, char *out) {
    char line[128];
    sprintf(out,
"\n===== TOP CARD =====\n%s %s",
            color_str(game->top_card.color),
            type_str(game->top_card.type));
    if (game->top_card.type == NUMBER)
        sprintf(out + strlen(out), " %d\n", game->top_card.number);
    else
        strcat(out, "\n");

    strcat(out, "====================\n\nYour cards:\n---------------------\n");

    for (int i = 0; i < game->hand_count[pid]; i++) {
        Card c = game->hands[pid][i];
        if (c.type == NUMBER)
            sprintf(line, "%d) %s %d\n", i+1, color_str(c.color), c.number);
        else
            sprintf(line, "%d) %s %s\n", i+1, color_str(c.color), type_str(c.type));
        strcat(out, line);
    }
    strcat(out, "\nType card NUMBER to play, or type: NO_CARD\n> END\n");
}

/* ---------- CLIENT HANDLER (FORKED) ---------- */
void handle_client(int pid, int sock) {
    char buf[256];
    char menu[2048];

    while (!game->game_over && game->active[pid]) {
        pthread_mutex_lock(&game->game_mutex);
        if (game->current_turn != pid) {
            pthread_mutex_unlock(&game->game_mutex);
            sleep(1);
            continue;
        }

        printf("\n PLAYER %d'S TURN\n", pid);
        log_msg("PLAYER %d TURN\n", pid);

        send(sock, "\n[SERVER] Invalid move, please choose again.\n", 48, 0);
        build_menu(pid, menu);
        send(sock, menu, strlen(menu), 0);
        pthread_mutex_unlock(&game->game_mutex);

        int n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        buf[strcspn(buf, "\n")] = 0;

        pthread_mutex_lock(&game->game_mutex);

        if (strcmp(buf, "NO_CARD") == 0) {
            printf(" Player %d clicked: NO_CARD\n", pid);
            log_msg("PLAYER %d NO_CARD\n", pid);
            game->no_card_count[pid]++;
            game->hands[pid][game->hand_count[pid]++] = draw_card();

            if (game->no_card_count[pid] >= 3) {
                printf(" Player %d DISQUALIFIED (3 NO_CARD)\n", pid);
                game->active[pid] = 0;
                send(sock, "\n YOU ARE DISQUALIFIED (3 NO_CARD)\n", 40, 0);
                close(sock);
                game->current_turn = next_player(pid);
                check_last_player();
                pthread_mutex_unlock(&game->game_mutex);
                exit(0);
            }
            game->current_turn = next_player(pid);
            pthread_mutex_unlock(&game->game_mutex);
            continue;
        }

        int choice = atoi(buf) - 1;

    if (choice < 0 || choice >= game->hand_count[pid]) {
        send(sock, "\n[SERVER] Invalid card number. Try again.\n", 44, 0);
        pthread_mutex_unlock(&game->game_mutex);
        continue;
    }

    Card selected = game->hands[pid][choice];

    if (!valid_move(selected, game->top_card)) {
        send(sock,
            "\n[SERVER] Invalid move! Card does not match color/type/number.\n",
            67, 0);
        pthread_mutex_unlock(&game->game_mutex);
        continue;
    }


        Card played = game->hands[pid][choice];
        game->top_card = played;
        printf(" Player %d played: %s %s", pid, color_str(played.color), type_str(played.type));
        if (played.type == NUMBER) printf(" %d", played.number);
        printf("\n");
        log_msg("PLAYER %d PLAYED\n", pid);

        for (int i = choice; i < game->hand_count[pid]-1; i++)
            game->hands[pid][i] = game->hands[pid][i+1];
        game->hand_count[pid]--;

        if (game->hand_count[pid] == 0) {
            printf("\n PLAYER %d WINS THE GAME! \n", pid);
            send(sock, "YOU WIN \n", 11, 0);
            game->game_over = 1;
            pthread_mutex_unlock(&game->game_mutex);
            exit(0);
        }

        if (played.type == SKIP) {
            printf(" SKIP played! Skipping next player.\n");
            game->current_turn = next_player(next_player(pid));
        } else if (played.type == PLUS2) {
            int n2 = next_player(pid);
            game->hands[n2][game->hand_count[n2]++] = draw_card();
            game->hands[n2][game->hand_count[n2]++] = draw_card();
            printf(" Player %d draws 2 cards\n", n2);
            game->current_turn = next_player(n2);
        } else {
            game->current_turn = next_player(pid);
        }

        if (game->hand_count[pid] >= MAX_HAND) {
            printf(" Player %d DISQUALIFIED (8 CARDS)\n", pid);
            game->active[pid] = 0;
            send(sock, "\n YOU ARE DISQUALIFIED (8 CARDS)\n", 40, 0);
            close(sock);
            game->current_turn = next_player(pid);
            check_last_player();
            pthread_mutex_unlock(&game->game_mutex);
            exit(0);
        }

        pthread_mutex_unlock(&game->game_mutex);
    }
    close(sock);
}

/* ---------- SCHEDULER THREAD (ROUND ROBIN) ---------- */
void *scheduler_thread(void *arg) {
    (void)arg;
    while (!game->game_over) {
        sleep(1);
        pthread_mutex_lock(&game->game_mutex);
        if (game->current_turn < 0)
            game->current_turn = next_player(0);
        pthread_mutex_unlock(&game->game_mutex);
    }
    return NULL;
}

/* ---------- LOAD SCORES ---------- */
void load_scores() {
    FILE *f = fopen("scores.txt", "r");
    if (!f) {
        f = fopen("scores.txt", "w");
        for (int i = 0; i < MAX_PLAYERS; i++) fprintf(f, "Player %d: 0\n", i);
        fclose(f);
        memset(game->scores, 0, sizeof(game->scores));
        return;
    }
    for (int i = 0; i < MAX_PLAYERS; i++)
        fscanf(f, "Player %*d: %d", &game->scores[i]);
    fclose(f);
}

/* ---------- MAIN ---------- */
int main() {
    srand(time(NULL));
    signal(SIGCHLD, sigchld_handler);
    pipe(log_pipe);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(server_fd, 5) < 0) { perror("listen"); exit(1); }

    game = mmap(NULL, sizeof(GameState), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&game->game_mutex, &attr);
    pthread_mutex_init(&game->score_mutex, &attr);

    load_scores();

    printf("Enter number of players (3-5): ");
    scanf("%d", &game->players);
    if (game->players < 3 || game->players > 5) game->players = 3;

    build_deck();
    shuffle_deck();

    for (int i = 0; i < game->players; i++) {
        game->active[i] = 1;
        game->no_card_count[i] = 0;
        game->hand_count[i] = HAND_SIZE;
        for (int j = 0; j < HAND_SIZE; j++)
            game->hands[i][j] = draw_card();
    }
    do {
        game->top_card = draw_card();
    } while (game->top_card.type != NUMBER);

    printf("\n INITIAL TOP CARD: %s %s", color_str(game->top_card.color), type_str(game->top_card.type));
    if (game->top_card.type == NUMBER) printf(" %d", game->top_card.number);
    printf("\n");

    pthread_t sched_t, log_t;
    pthread_create(&sched_t, NULL, scheduler_thread, NULL);
    pthread_create(&log_t, NULL, logger_thread, NULL);

    for (int i = 0; i < game->players; i++) {
        int c = accept(server_fd, NULL, NULL);
        printf("[SERVER] Player %d connected\n", i);
        if (fork() == 0) handle_client(i, c);
    }

    while (wait(NULL) > 0);
    printf("\nGAME OVER\n");
    return 0;
}
