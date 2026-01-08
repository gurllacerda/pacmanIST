#include "../include/board.h"
#include "../include/display.h"
#include "../../client-base/include/protocol.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "parser.c"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <semaphore.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct
{
    char req_pipe[40];
    char notif_pipe[40];
} session_request_t;

typedef struct
{
    session_request_t *buf;
    int cap;
    int head;
    int tail;

    pthread_mutex_t mtx;
    sem_t has_items;

    // IMPORTANTE:
    // este semáforo representa "slots de sessão disponíveis" (max_games).
    // Só é libertado (sem_post) quando uma sessão termina.
    sem_t has_space;
} request_queue_t;

static request_queue_t g_queue;
static const char *g_levels_dir = NULL;
static const char *g_register_pipe = NULL;

static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t off = 0;
    while (off < n)
    {
        ssize_t r = read(fd, (char *)buf + off, n - off);
        if (r == 0)
            return off; // EOF
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)r;
    }
    return (ssize_t)off;
}

static ssize_t write_full(int fd, const void *buf, size_t n)
{
    size_t off = 0;
    while (off < n)
    {
        ssize_t w = write(fd, (const char *)buf + off, n - off);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}

static int queue_init(request_queue_t *q, int cap)
{
    q->buf = (session_request_t *)calloc((size_t)cap, sizeof(session_request_t));
    if (!q->buf)
        return -1;

    q->cap = cap;
    q->head = 0;
    q->tail = 0;

    pthread_mutex_init(&q->mtx, NULL);
    sem_init(&q->has_items, 0, 0);
    sem_init(&q->has_space, 0, (unsigned)cap); // max_games slots
    return 0;
}

static void queue_destroy(request_queue_t *q)
{
    if (q->buf)
        free(q->buf);
    pthread_mutex_destroy(&q->mtx);
    sem_destroy(&q->has_items);
    sem_destroy(&q->has_space);
}

static void queue_push_blocking(request_queue_t *q, const session_request_t *req)
{
    // Bloqueia quando max_games sessões já estão ocupadas (requisito do enunciado)
    sem_wait(&q->has_space);

    pthread_mutex_lock(&q->mtx);
    q->buf[q->tail] = *req;
    q->tail = (q->tail + 1) % q->cap;
    pthread_mutex_unlock(&q->mtx);

    sem_post(&q->has_items);
}

static session_request_t queue_pop_blocking(request_queue_t *q)
{
    session_request_t req;
    memset(&req, 0, sizeof(req));

    sem_wait(&q->has_items);

    pthread_mutex_lock(&q->mtx);
    req = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    pthread_mutex_unlock(&q->mtx);

    // NOTA: NÃO libertamos has_space aqui.
    // has_space só é libertado quando a sessão terminar (no worker).
    return req;
}

void send_board_to_client(board_t *board)
{
    if (board->client_notif_fd == -1)
        return;

    int width = board->width;
    int height = board->height;
    int size = width * height;

    debug("Sending board to client: %dx%d, size=%d, fd=%d\n",
          width, height, size, board->client_notif_fd);

    char *data = malloc(size * sizeof(char));
    if (!data)
        return;

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int idx = y * width + x;

            char ch = board->board[idx].content;

            if (ch == 'W')
            {
                ch = '#';
            }
            else if (ch == ' ' && board->board[idx].has_dot)
            {
                ch = '.';
            }
            else if (ch == ' ' && board->board[idx].has_portal)
            {
                ch = '@';
            }

            for (int i = 0; i < board->n_ghosts; i++)
            {
                if (board->ghosts[i].pos_x == x && board->ghosts[i].pos_y == y)
                {
                    ch = (board->ghosts[i].charged) ? 'G' : 'M';
                    break;
                }
            }

            // Sobrepor Pacman (C)
            if (board->n_pacmans > 0 && board->pacmans[0].alive &&
                board->pacmans[0].pos_x == x && board->pacmans[0].pos_y == y)
            {
                ch = 'C';
            }

            data[idx] = ch;
        }
    }

    // ADICIONE DEBUG ANTES DE ENVIAR:
    debug("Board data to send: %.10s...\n", data);

    // Protocolo: OP_CODE | width | height | tempo | victory | game_over | points | DATA
    char op = OP_CODE_BOARD;
    // int victory = 0;
    int game_over = (!board->pacmans[0].alive);
    int victory = (!board->game_running) && (board->pacmans[0].alive) && (!board->exit_request);
    int points = (board->n_pacmans > 0) ? board->pacmans[0].points : 0;

    pthread_mutex_lock(&board->ncurses_mutex);

    write(board->client_notif_fd, &op, sizeof(char));
    write(board->client_notif_fd, &width, sizeof(int));
    write(board->client_notif_fd, &height, sizeof(int));
    write(board->client_notif_fd, &board->tempo, sizeof(int));
    write(board->client_notif_fd, &victory, sizeof(int));
    write(board->client_notif_fd, &game_over, sizeof(int));
    write(board->client_notif_fd, &points, sizeof(int));
    write(board->client_notif_fd, data, size);

    pthread_mutex_unlock(&board->ncurses_mutex);

    free(data);
}

void *client_input_handler(void *arg)
{
    board_t *board = (board_t *)arg;

    while (1)
    {
        pthread_rwlock_rdlock(&board->mutex);
        int running = board->game_running;
        pthread_rwlock_unlock(&board->mutex);
        if (!running)
            break;

        char op, cmd;
        int n = read(board->client_req_fd, &op, sizeof(char));
        if (n <= 0)
        {
            // cliente fechou o pipe / caiu
            pthread_rwlock_wrlock(&board->mutex);
            board->exit_request = 1;
            board->game_running = 0;
            pthread_rwlock_unlock(&board->mutex);
            break;
        }

        if (op == OP_CODE_DISCONNECT)
        {
            pthread_rwlock_wrlock(&board->mutex);
            board->exit_request = 1;
            board->game_running = 0;
            pthread_rwlock_unlock(&board->mutex);
            break;
        }

        if (op == OP_CODE_PLAY)
        {
            if (read(board->client_req_fd, &cmd, sizeof(char)) <= 0)
                continue;

            pthread_rwlock_wrlock(&board->mutex);
            board->next_pacman_move = cmd;
            pthread_rwlock_unlock(&board->mutex);
        }
    }

    return NULL;
}

void *ghost_thread(void *arg)
{
    ghost_t *ghost = (ghost_t *)arg;
    board_t *board = (board_t *)ghost->board_ref;

    while (board->game_running && board->pacmans[0].alive)
    {
        pthread_rwlock_wrlock(&board->mutex);

        // double check pois o jogo pode acabar enquanto esperamos pelo lock
        if (!board->game_running)
        {
            pthread_rwlock_unlock(&board->mutex);
            break;
        }

        if (ghost->n_moves > 0)
        {
            move_ghost(board, ghost->id, &ghost->moves[ghost->current_move % ghost->n_moves]);
        }

        pthread_rwlock_unlock(&board->mutex);

        // se tempo do board > 0 usa o tempo do board senão 100
        int sleep_time = (board->tempo > 0) ? board->tempo : 100;
        sleep_ms(sleep_time);
    }

    return NULL;
};

void *pacman_thread(void *arg)
{
    board_t *board = (board_t *)arg;
    pacman_t *pacman = &board->pacmans[0];

    while (board->game_running && pacman->alive)
    {

        pthread_rwlock_wrlock(&board->mutex);

        if (!board->game_running || !pacman->alive)
        {
            pthread_rwlock_unlock(&board->mutex);
            break;
        }

        if (board->exit_request)
        {
            board->game_running = 0;
            pthread_rwlock_unlock(&board->mutex);
            break;
        }

        if (pacman->n_moves > 0)
        {

            command_t *auto_cmd = &pacman->moves[pacman->current_move % pacman->n_moves];

            // Executa o movimento automático
            int result = move_pacman(board, 0, auto_cmd);

            if (result == REACHED_PORTAL)
            {
                board->game_running = 0;
            }
        }
        else
        {

            char cmd_char = board->next_pacman_move;

            if (cmd_char != '\0')
            {
                board->next_pacman_move = '\0';

                if (cmd_char == 'Q')
                {
                    board->exit_request = 1;
                    board->game_running = 0;
                }
                else
                {
                    command_t cmd;
                    cmd.command = cmd_char;
                    cmd.turns = 1;
                    cmd.turns_left = 1;

                    int result = move_pacman(board, 0, &cmd);

                    if (result == REACHED_PORTAL)
                    {
                        board->game_running = 0;
                    }
                }
            }
        }

        pthread_rwlock_unlock(&board->mutex);

        int sleep_time = (board->tempo > 0) ? board->tempo : 100;
        sleep_ms(sleep_time);
    }
    return NULL;
}

static void *host_thread(void *arg)
{
    (void)arg;

    int fd_r = open(g_register_pipe, O_RDONLY);
    if (fd_r < 0)
    {
        perror("host_thread: open register_pipe (read)");
        return NULL;
    }

    // manter FIFO “vivo” para evitar EOF quando não há clientes
    int fd_w_dummy = open(g_register_pipe, O_WRONLY | O_NONBLOCK);
    (void)fd_w_dummy; // pode falhar, mas não é crítico

    while (1)
    {
        char op = 0;
        ssize_t n = read_full(fd_r, &op, sizeof(char));
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            // se acontecer EOF, tenta continuar
            continue;
        }

        if (op != OP_CODE_CONNECT)
        {
            // ignorar outros opcodes no FIFO de registo
            continue;
        }

        session_request_t req;
        memset(&req, 0, sizeof(req));

        if (read_full(fd_r, req.req_pipe, 40) != 40)
            continue;
        if (read_full(fd_r, req.notif_pipe, 40) != 40)
            continue;

        // garantir terminação (por segurança)
        req.req_pipe[39] = '\0';
        req.notif_pipe[39] = '\0';

        queue_push_blocking(&g_queue, &req);
    }

    // nunca chega aqui
    // close(fd_r);
    // if (fd_w_dummy >= 0) close(fd_w_dummy);
    // return NULL;
}

void run_session(int req_fd, int notif_fd, char *levels_dir)
{
    char level_files[MAX_LEVELS][MAX_FILENAME];
    int num_levels = 0;
    if (load_levels_from_dir(levels_dir, level_files, &num_levels) != 0)
        return;

    int current_level = 0;
    int points = 0;

    while (current_level < num_levels)
    {
        board_t board;
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", levels_dir, level_files[current_level]);

        if (load_level_from_file(full_path, &board, levels_dir) != 0)
            break;
        if (board.n_pacmans > 0)
            board.pacmans[0].n_moves = 0; // forçando a leitura dos movimentos do cliente

        board.client_req_fd = req_fd;
        board.client_notif_fd = notif_fd;
        board.game_running = 1;
        board.exit_request = 0;
        board.next_pacman_move = '\0';
        if (board.n_pacmans > 0)
            board.pacmans[0].points = points;

        pthread_rwlock_init(&board.mutex, NULL);
        pthread_mutex_init(&board.ncurses_mutex, NULL);

        pthread_t tid_pacman, tid_input;
        pthread_create(&tid_pacman, NULL, pacman_thread, &board);
        pthread_create(&tid_input, NULL, client_input_handler, &board); // Nova thread de input

        for (int i = 0; i < board.n_ghosts; i++)
        {
            board.ghosts[i].board_ref = (struct board_t *)&board;
            board.ghosts[i].id = i;
            pthread_create(&board.ghosts[i].thread_id, NULL, ghost_thread, &board.ghosts[i]);
        }

        while (1)
        {
            pthread_rwlock_rdlock(&board.mutex);
            int running = board.game_running;
            int alive = (board.n_pacmans > 0) ? board.pacmans[0].alive : 0;
            points = (board.n_pacmans > 0) ? board.pacmans[0].points : 0;
            int exit_req = board.exit_request;
            pthread_rwlock_unlock(&board.mutex);

            if (!running || !alive || exit_req)
                break;

            send_board_to_client(&board);

            sleep_ms(50);
        }

        board.game_running = 0;
        pthread_join(tid_pacman, NULL);
        pthread_join(tid_input, NULL);
        for (int i = 0; i < board.n_ghosts; i++)
            pthread_join(board.ghosts[i].thread_id, NULL);

        send_board_to_client(&board);

        int must_exit = board.exit_request;
        int pacman_dead = (board.n_pacmans > 0) ? !board.pacmans[0].alive : 1;

        pthread_mutex_destroy(&board.ncurses_mutex);
        unload_level(&board);

        if (must_exit || pacman_dead)
            break;

        current_level++;
    }
}

static void *session_worker_thread(void *arg)
{
    (void)arg;

    while (1)
    {
        session_request_t req = queue_pop_blocking(&g_queue);

        int notif_fd = open(req.notif_pipe, O_WRONLY);
        if (notif_fd == -1)
        {
            // cliente desapareceu / fifo não existe → libertar slot
            sem_post(&g_queue.has_space);
            continue;
        }

        // ACK de conexão (OP_CODE=1 | result)
        char ack_op = OP_CODE_CONNECT;
        char result = 0;

        if (write_full(notif_fd, &ack_op, sizeof(char)) != (ssize_t)sizeof(char) ||
            write_full(notif_fd, &result, sizeof(char)) != (ssize_t)sizeof(char))
        {
            close(notif_fd);
            sem_post(&g_queue.has_space);
            continue;
        }

        // abrir FIFO de pedidos (vai desbloquear quando o cliente abrir o writer)
        int req_fd = open(req.req_pipe, O_RDONLY);
        if (req_fd == -1)
        {
            close(notif_fd);
            sem_post(&g_queue.has_space);
            continue;
        }

        run_session(req_fd, notif_fd, (char *)g_levels_dir);

        close(req_fd);
        close(notif_fd);

        // sessão terminou → libertar “slot” para nova sessão
        sem_post(&g_queue.has_space);
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        printf("Uso: %s <levels_dir> <max_games> <register_pipe>\n", argv[0]);
        return 1;
    }

    g_levels_dir = argv[1];
    int max_games = atoi(argv[2]);
    g_register_pipe = argv[3];

    if (max_games <= 0)
    {
        fprintf(stderr, "max_games inválido.\n");
        return 1;
    }

    unlink(g_register_pipe);
    if (mkfifo(g_register_pipe, 0666) != 0)
    {
        perror("Erro ao criar pipe de registo");
        return 1;
    }

    printf("Servidor PacmanIST iniciado no pipe '%s'\n", g_register_pipe);
    open_debug_file("server_debug.log");

    if (queue_init(&g_queue, max_games) != 0)
    {
        fprintf(stderr, "Falha a inicializar buffer produtor-consumidor.\n");
        unlink(g_register_pipe);
        return 1;
    }

    // criar workers (pool fixo)
    for (int i = 0; i < max_games; i++)
    {
        pthread_t tid;
        if (pthread_create(&tid, NULL, session_worker_thread, NULL) != 0)
        {
            perror("pthread_create worker");
            // em caso de falha, continua; mas idealmente abortar
        }
        else
        {
            pthread_detach(tid);
        }
    }

    // criar host
    pthread_t host_tid;
    if (pthread_create(&host_tid, NULL, host_thread, NULL) != 0)
    {
        perror("pthread_create host");
        queue_destroy(&g_queue);
        unlink(g_register_pipe);
        return 1;
    }

    // o servidor fica a correr
    pthread_join(host_tid, NULL);

    // nunca chega aqui, mas por completude:
    queue_destroy(&g_queue);
    close_debug_file();
    unlink(g_register_pipe);
    return 0;
}
