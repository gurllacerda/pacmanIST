#include "board.h"
#include "display.h"
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

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

static int backup_exists = 0; // 0 -> não há backup; 1 -> já há backup

void screen_refresh(board_t *game_board, int mode)
{
    pthread_rwlock_rdlock(&game_board->mutex);
    pthread_mutex_lock(&game_board->ncurses_mutex);
    // debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
    pthread_mutex_unlock(&game_board->ncurses_mutex);
    pthread_rwlock_unlock(&game_board->mutex);

    if (game_board->tempo != 0)
        sleep_ms(game_board->tempo);
}

// int play_board(board_t *game_board)
// {
//     pacman_t *pacman = &game_board->pacmans[0];
//     command_t *play;
//     if (pacman->n_moves == 0)
//     { // if is user input
//         command_t c;
//         c.command = get_input();

//         // debug("RAW INPUT: %d ('%c')\n", (int)c.command, c.command); para debug

//         if (c.command == '\0')
//             return CONTINUE_PLAY;

//         if (c.command == 'G')
//         {
//             // Só deixa criar backup se ainda não existir nenhum
//             if (!backup_exists)
//                 return CREATE_BACKUP;
//             else
//                 return CONTINUE_PLAY; // ignora se já houver backup
//         }

//         c.turns = 1;
//         play = &c;
//     }
//     else
//     {
//         // avoid buffer overflow wrapping around with modulo of n_moves
//         // this ensures that we always access a valid move for the pacman
//         play = &pacman->moves[pacman->current_move % pacman->n_moves];
//     }

//     debug("KEY %c\n", play->command);

//     if (play->command == 'Q')
//     {
//         return QUIT_GAME;
//     }

//     pthread_rwlock_wrlock(&game_board->mutex);
//     int result = move_pacman(game_board, 0, play);
//     pthread_rwlock_unlock(&game_board->mutex);

//     if (result == REACHED_PORTAL)
//     {
//         // Next level
//         return NEXT_LEVEL;
//     }

//     if (result == DEAD_PACMAN)
//     {
//         return QUIT_GAME;
//     }
//     if (!game_board->pacmans[0].alive)
//     {
//         return QUIT_GAME;
//     }

//     return CONTINUE_PLAY;
// }

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
        command_t *cmd_ptr = NULL;
        command_t manual_cmd;
        manual_cmd.turns = 1;
        manual_cmd.turns_left = 1;
        manual_cmd.command = '\0';
        int result = 0; // Variável para guardar o resultado

        pthread_rwlock_wrlock(&board->mutex);

        if (!board->game_running || !pacman->alive)
        {
            pthread_rwlock_unlock(&board->mutex);
            break;
        }

        if (pacman->n_moves > 0)
        {
            cmd_ptr = &pacman->moves[pacman->current_move % pacman->n_moves];
        }
        else
        {
            cmd_ptr = &manual_cmd;
            if (board->next_pacman_move != '\0')
            {
                cmd_ptr->command = board->next_pacman_move;
                board->next_pacman_move = '\0';
            }
        }

        if (cmd_ptr->command != '\0')
        {
            result = move_pacman(board, 0, cmd_ptr);

            if (result == REACHED_PORTAL)
            {
                board->game_running = 0;
            }
        }

        pthread_rwlock_unlock(&board->mutex);

        int sleep_time = (board->tempo > 0) ? board->tempo : 100;
        sleep_ms(sleep_time);
    }
    return NULL;
}

void *ncurses_thread_nova(void *arg)
{
    board_t *board = (board_t *)arg;

    while (board->game_running && board->pacmans[0].alive)
    {
        pthread_rwlock_rdlock(&board->mutex);
        pthread_mutex_lock(&board->ncurses_mutex);

        draw_board(board, DRAW_MENU);
        refresh_screen();

        pthread_mutex_unlock(&board->ncurses_mutex);
        pthread_rwlock_unlock(&board->mutex);

        sleep_ms(40);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s <level_directory>\n", argv[1]);
        return 1;
    }
    char *levels_directory = argv[1];

    srand((unsigned int)time(NULL));
    open_debug_file("debug.log");
    terminal_init();

    char level_files[MAX_LEVELS][MAX_FILENAME];
    int num_levels = 0;

    if (load_levels_from_dir(levels_directory, level_files, &num_levels) != 0)
    {
        terminal_cleanup();
        close_debug_file();
        perror("Erro: Nenhum nível encontrado.\n");
        return 1;
    }

    int accumulated_points = 0;
    int current_level_idx = 0;
    bool quit_game = false;

    while (current_level_idx < num_levels && !quit_game)
    {
        board_t game_board;
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", levels_directory, level_files[current_level_idx]);

        if (load_level_from_file(full_path, &game_board, levels_directory) != 0)
            break;

        game_board.next_pacman_move = '\0';
        game_board.game_running = 1;
        pthread_mutex_init(&game_board.ncurses_mutex, NULL);
        if (game_board.n_pacmans > 0)
            game_board.pacmans[0].points = accumulated_points;

        // Threads Fantasmas
        for (int i = 0; i < game_board.n_ghosts; i++)
        {
            game_board.ghosts[i].board_ref = (struct board_t *)&game_board;
            game_board.ghosts[i].id = i;
            pthread_create(&game_board.ghosts[i].thread_id, NULL, ghost_thread, &game_board.ghosts[i]);
        }

        // Thread Pacman
        pthread_t pacman_tid;
        pthread_create(&pacman_tid, NULL, pacman_thread, &game_board);

        // Thread ncurses
        pthread_t ncurses_tid;
        pthread_create(&ncurses_tid, NULL, ncurses_thread_nova, &game_board);

        // Desenho inicial
        screen_refresh(&game_board, DRAW_MENU);

        while (game_board.game_running)
        {
            pthread_rwlock_rdlock(&game_board.mutex);
            int running = game_board.game_running;
            int is_alive = game_board.pacmans[0].alive;
            accumulated_points = game_board.pacmans[0].points;
            pthread_rwlock_unlock(&game_board.mutex);

            if (!is_alive || !running)
            {
                if (!is_alive)
                {
                    screen_refresh(&game_board, DRAW_GAME_OVER);
                    sleep_ms(2000);
                    if (backup_exists)
                        _exit(1); // filho com backup: sinaliza morte ao pai
                    quit_game = true;
                }
                else
                {
                    screen_refresh(&game_board, DRAW_WIN);
                    sleep_ms(1500);
                }
                break;
            }

            // Capturar input usando mutex para não colidir com ncurses thread
            pthread_mutex_lock(&game_board.ncurses_mutex);
            char input = get_input();
            pthread_mutex_unlock(&game_board.ncurses_mutex);

            int pacman_manual = 0;
            pthread_rwlock_rdlock(&game_board.mutex);
            if (game_board.n_pacmans > 0 && game_board.pacmans[0].n_moves == 0)
            {
                pacman_manual = 1;
            }
            pthread_rwlock_unlock(&game_board.mutex);

            if (pacman_manual && input == 'Q')
            {
                pthread_rwlock_wrlock(&game_board.mutex);
                game_board.game_running = 0;
                pthread_rwlock_unlock(&game_board.mutex);
                quit_game = true;
                break;
            }

            if (pacman_manual && input == 'G')
            {
                if (!backup_exists)
                {
                    // Parar todas as threads ANTES do fork
                    pthread_rwlock_wrlock(&game_board.mutex);
                    game_board.game_running = 0;
                    pthread_rwlock_unlock(&game_board.mutex);

                    // Esperar que todas as threads terminem
                    pthread_join(ncurses_tid, NULL);
                    pthread_join(pacman_tid, NULL);
                    for (int i = 0; i < game_board.n_ghosts; i++)
                    {
                        pthread_join(game_board.ghosts[i].thread_id, NULL);
                    }

                    int pid = fork();

                    if (pid == 0)
                    {
                        // FILHO: continua o jogo a partir do backup
                        backup_exists = 1;
                        game_board.game_running = 1;
                        game_board.pacmans[0].alive = 1;

                        // Recriar threads dos fantasmas
                        for (int i = 0; i < game_board.n_ghosts; i++)
                        {
                            game_board.ghosts[i].board_ref = (struct board_t *)&game_board;
                            game_board.ghosts[i].id = i;
                            pthread_create(&game_board.ghosts[i].thread_id,
                                           NULL, ghost_thread, &game_board.ghosts[i]);
                        }

                        // Recriar thread do Pacman
                        pthread_create(&pacman_tid, NULL, pacman_thread, &game_board);

                        // Recriar thread ncurses
                        pthread_create(&ncurses_tid, NULL, ncurses_thread_nova, &game_board);

                        continue;
                    }
                    else if (pid > 0)
                    {
                        // PAI: espera pelo filho
                        backup_exists = 1;
                        int status;
                        wait(&status);

                        if (WIFEXITED(status) && WEXITSTATUS(status) == 1)
                        {
                            // Filho morreu -> reencarnar na posição salva
                            backup_exists = 0;
                            game_board.game_running = 1;
                            game_board.pacmans[0].alive = 1;

                            // Recriar threads no pai
                            for (int i = 0; i < game_board.n_ghosts; i++)
                            {
                                game_board.ghosts[i].board_ref = (struct board_t *)&game_board;
                                game_board.ghosts[i].id = i;
                                pthread_create(&game_board.ghosts[i].thread_id,
                                               NULL, ghost_thread, &game_board.ghosts[i]);
                            }

                            pthread_create(&pacman_tid, NULL, pacman_thread, &game_board);
                            pthread_create(&ncurses_tid, NULL, ncurses_thread_nova, &game_board);
                            continue; // volta ao loop (reencarnado)
                        }
                        else
                        {
                            // Filho completou o nível com sucesso
                            quit_game = true;
                            break;
                        }
                    }
                    else
                    {
                        // Erro no fork: tenta recuperar e seguir em frente
                        perror("Fork failed");
                        pthread_rwlock_wrlock(&game_board.mutex);
                        game_board.game_running = 1;
                        pthread_rwlock_unlock(&game_board.mutex);
                        backup_exists = 0;

                        // Recriar threads após erro
                        pthread_create(&pacman_tid, NULL, pacman_thread, &game_board);
                        pthread_create(&ncurses_tid, NULL, ncurses_thread_nova, &game_board);
                    }
                }
            }
            else if (input != '\0')
            {
                // Movimento manual do Pacman
                pthread_rwlock_wrlock(&game_board.mutex);
                game_board.next_pacman_move = input;
                pthread_rwlock_unlock(&game_board.mutex);
            }

            // Pequeno sleep para não sobrecarregar a CPU
            sleep_ms(10);
        }

        // Limpeza
        pthread_rwlock_wrlock(&game_board.mutex);
        game_board.game_running = 0;
        pthread_rwlock_unlock(&game_board.mutex);
        pthread_join(ncurses_tid, NULL);
        pthread_join(pacman_tid, NULL);

        for (int i = 0; i < game_board.n_ghosts; i++)
        {
            pthread_join(game_board.ghosts[i].thread_id, NULL);
        }

        pthread_mutex_destroy(&game_board.ncurses_mutex);
        unload_level(&game_board);
        current_level_idx++;
    }

    terminal_cleanup();
    close_debug_file();
    return 0;
}
