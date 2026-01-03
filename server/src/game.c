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


#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

void send_board_to_client(board_t *board) {
    if (board->client_notif_fd == -1) return;

    int width = board->width;
    int height = board->height;
    int size = width * height;
    
    debug("Sending board to client: %dx%d, size=%d, fd=%d\n", 
          width, height, size, board->client_notif_fd);
    
    char *data = malloc(size * sizeof(char));
    if (!data) return;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            
            char ch = board->board[idx].content;
            
            if (ch == 'W') {
                ch = '#';
            }
            else if (ch == ' ' && board->board[idx].has_dot) {
                ch = '.';
            }
            else if (ch == ' ' && board->board[idx].has_portal) {
                ch = '@';
            }

            for (int i = 0; i < board->n_ghosts; i++) {
                if (board->ghosts[i].pos_x == x && board->ghosts[i].pos_y == y) {
                    ch = (board->ghosts[i].charged) ? 'G' : 'M';
                    break;
                }
            }
            
            // Sobrepor Pacman (C)
            if (board->n_pacmans > 0 && board->pacmans[0].alive &&
                board->pacmans[0].pos_x == x && board->pacmans[0].pos_y == y) {
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
    int game_over = (!board->pacmans[0].alive) && (board->game_running);
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

void *client_input_handler(void *arg) {
    board_t *board = (board_t *)arg;
    
    while (board->game_running) {
        char op, cmd;
        int n = read(board->client_req_fd, &op, sizeof(char));
        if (n <= 0) break; 

        if (op == OP_CODE_DISCONNECT) {
            board->exit_request = 1; 
            break;
        }

        if (op == OP_CODE_PLAY) {
            read(board->client_req_fd, &cmd, sizeof(char));
            
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

void *pacman_thread(void *arg) {
    board_t *board = (board_t *)arg;
    pacman_t *pacman = &board->pacmans[0];

    while (board->game_running && pacman->alive) {
        
        pthread_rwlock_wrlock(&board->mutex);

        if (!board->game_running || !pacman->alive) {
            pthread_rwlock_unlock(&board->mutex);
            break;
        }

        if (board->exit_request) {
            board->game_running = 0;
            pthread_rwlock_unlock(&board->mutex);
            break;
        }
        
        
        
        if (pacman->n_moves > 0) {
            
            command_t *auto_cmd = &pacman->moves[pacman->current_move % pacman->n_moves];
            
            // Executa o movimento automático
            int result = move_pacman(board, 0, auto_cmd);
            
            if (result == REACHED_PORTAL) {
                board->game_running = 0;
            }

        } else {
            
            char cmd_char = board->next_pacman_move;
            
            if (cmd_char != '\0') {
                board->next_pacman_move = '\0'; 

                if (cmd_char == 'Q') {
                    board->exit_request = 1;
                    board->game_running = 0;
                } 
                else {
                    command_t cmd;
                    cmd.command = cmd_char;
                    cmd.turns = 1;
                    cmd.turns_left = 1;
                    
                    int result = move_pacman(board, 0, &cmd);
                    
                    if (result == REACHED_PORTAL) {
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


void run_session(int req_fd, int notif_fd, char *levels_dir) {
    char level_files[MAX_LEVELS][MAX_FILENAME];
    int num_levels = 0;
    if (load_levels_from_dir(levels_dir, level_files, &num_levels) != 0) return;

    int current_level = 0;
    int points = 0;

    while (current_level < num_levels) {
        board_t board;
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", levels_dir, level_files[current_level]);

        if (load_level_from_file(full_path, &board, levels_dir) != 0) break;
        if (board.n_pacmans > 0) board.pacmans[0].n_moves = 0;//forçando a leitura dos movimentos do cliente

        board.client_req_fd = req_fd;
        board.client_notif_fd = notif_fd;
        board.game_running = 1;
        board.exit_request = 0;
        board.next_pacman_move = '\0';
        if (board.n_pacmans > 0) board.pacmans[0].points = points;

        pthread_rwlock_init(&board.mutex, NULL);
        pthread_mutex_init(&board.ncurses_mutex, NULL);

        pthread_t tid_pacman, tid_input;
        pthread_create(&tid_pacman, NULL, pacman_thread, &board);
        pthread_create(&tid_input, NULL, client_input_handler, &board); // Nova thread de input

        for (int i = 0; i < board.n_ghosts; i++) {
            board.ghosts[i].board_ref = (struct board_t *)&board;
            board.ghosts[i].id = i;
            pthread_create(&board.ghosts[i].thread_id, NULL, ghost_thread, &board.ghosts[i]);
        }

        while (1) {
            pthread_rwlock_rdlock(&board.mutex);
            int running = board.game_running;
            int alive = (board.n_pacmans > 0) ? board.pacmans[0].alive : 0;
            points = (board.n_pacmans > 0) ? board.pacmans[0].points : 0;
            int exit_req = board.exit_request;
            pthread_rwlock_unlock(&board.mutex);

            if (!running || !alive || exit_req) break;

            send_board_to_client(&board);

            sleep_ms(50); 
        }

        board.game_running = 0; 
        pthread_join(tid_pacman, NULL);
        pthread_join(tid_input, NULL); 
        for (int i = 0; i < board.n_ghosts; i++) pthread_join(board.ghosts[i].thread_id, NULL);

        send_board_to_client(&board); 

        pthread_mutex_destroy(&board.ncurses_mutex);
        pthread_rwlock_destroy(&board.mutex);
        unload_level(&board);

        if (board.exit_request || !board.pacmans[0].alive) break; 

        current_level++;
    }
}


int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Uso: %s <levels_dir> <max_games> <register_pipe>\n", argv[0]);
        return 1;
    }

    char *levels_dir = argv[1];
    // int max_games = atoi(argv[2]); 
    char *register_pipe = argv[3];

    unlink(register_pipe);
    if (mkfifo(register_pipe, 0666) != 0) {
        perror("Erro ao criar pipe de registo");
        return 1;
    }

    printf("Servidor PacmanIST iniciado no pipe '%s'...\n", register_pipe);
    open_debug_file("server_debug.log");

    while (1) {
        int server_fd = open(register_pipe, O_RDONLY);
        if (server_fd == -1) {
            perror("Erro no pipe de registo");
            break;
        }

        char op;
        if (read(server_fd, &op, sizeof(char)) > 0 && op == OP_CODE_CONNECT) {
            char req_pipe[40], notif_pipe[40];
            read(server_fd, req_pipe, 40);
            read(server_fd, notif_pipe, 40);
            
            printf("Cliente a conectar-se: Req='%s' Notif='%s'\n", req_pipe, notif_pipe);

            int notif_fd = open(notif_pipe, O_WRONLY);
            char ack_op = OP_CODE_CONNECT;
            char result = 0; 
            write(notif_fd, &ack_op, sizeof(char));
            write(notif_fd, &result, sizeof(char));

            int req_fd = open(req_pipe, O_RDONLY);

            if (notif_fd != -1 && req_fd != -1) {
                run_session(req_fd, notif_fd, levels_dir);
                
                close(notif_fd);
                close(req_fd);
            }
        }
        
        close(server_fd);
    }

    close_debug_file();
    unlink(register_pipe);
    return 0;
}
