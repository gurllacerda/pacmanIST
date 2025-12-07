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
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
    if (game_board->tempo != 0)
        sleep_ms(game_board->tempo);
}

int play_board(board_t *game_board)
{
    pacman_t *pacman = &game_board->pacmans[0];
    command_t *play;
    if (pacman->n_moves == 0)
    { // if is user input
        command_t c;
        c.command = get_input();

        // debug("RAW INPUT: %d ('%c')\n", (int)c.command, c.command); para debug

        if (c.command == '\0')
            return CONTINUE_PLAY;

        if (c.command == 'G')
        {
            // Só deixa criar backup se ainda não existir nenhum
            if (!backup_exists)
                return CREATE_BACKUP;
            else
                return CONTINUE_PLAY; // ignora se já houver backup
        }

        c.turns = 1;
        play = &c;
    }
    else
    {
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the pacman
        play = &pacman->moves[pacman->current_move % pacman->n_moves];

        // debug("PRE-DEFINED MOVE: %c (move %d/%d)\n",
        //       play->command, pacman->current_move, pacman->n_moves);
    }

    debug("KEY %c\n", play->command);

    if (play->command == 'Q')
    {
        return QUIT_GAME;
    }

    pthread_rwlock_wrlock(&game_board->mutex);
    int result = move_pacman(game_board, 0, play);
    pthread_rwlock_unlock(&game_board->mutex);

    if (result == REACHED_PORTAL)
    {
        // Next level
        return NEXT_LEVEL;
    }

    if (result == DEAD_PACMAN)
    {
        return QUIT_GAME;
    }

    // for (int i = 0; i < game_board->n_ghosts; i++)
    // {
    //     ghost_t *ghost = &game_board->ghosts[i];
    //     // avoid buffer overflow wrapping around with modulo of n_moves
    //     // this ensures that we always access a valid move for the ghost
    //     if (ghost->n_moves > 0)
    //     {
    //         move_ghost(game_board, i, &ghost->moves[ghost->current_move % ghost->n_moves]);
    //     }
    // }

    if (!game_board->pacmans[0].alive)
    {
        return QUIT_GAME;
    }

    return CONTINUE_PLAY;
}

void *ghost_thread(void *arg)
{
    ghost_t *ghost = (ghost_t *)arg;
    board_t *board = (board_t *)ghost->board_ref;

    while (board->game_running && board->pacmans[0].alive)
    {
        pthread_rwlock_wrlock(&board->mutex);

        // double check pois o jogo pode acabar enquanteo esperamos pelo lock
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

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s <level_directory>\n", argv[0]);
        // TODO receive inputs
    }
    char *levels_directory = argv[1];

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    open_debug_file("debug.log");

    terminal_init();

    char level_files[MAX_LEVELS][MAX_FILENAME];
    int num_levels = 0;

    if (load_levels_from_dir(levels_directory, level_files, &num_levels) != 0)
    {
        terminal_cleanup();
        close_debug_file();
        fprintf(stderr, "Erro: Nenhum nível encontrado ou diretoria inválida.\n");
        return 1;
    }

    int accumulated_points = 0;
    int current_level_idx = 0;
    bool quit_game = false;

    while (current_level_idx < num_levels && !quit_game)
    {
        board_t game_board;
        char full_path[512];

        // Constrói o caminho completo: "alumaDiretoria/1.lvl"
        snprintf(full_path, sizeof(full_path), "%s/%s", levels_directory, level_files[current_level_idx]);

        if (load_level_from_file(full_path, &game_board, levels_directory) != 0)
        {
            break; // Erro ao carregar nível
        }

        for (int i = 0; i < game_board.n_ghosts; i++)
        {
            game_board.ghosts[i].board_ref = (struct board_t *)&game_board; // Ponteiro para o pai
            game_board.ghosts[i].id = i;

            if (pthread_create(&game_board.ghosts[i].thread_id, NULL, ghost_thread, &game_board.ghosts[i]) != 0)
            {
                perror("Erro ao criar thread do fantasma");
            }
        }
        if (game_board.n_pacmans > 0)
        {
            game_board.pacmans[0].points = accumulated_points;
        }

        pthread_rwlock_rdlock(&game_board.mutex);
        draw_board(&game_board, DRAW_MENU);
        pthread_rwlock_unlock(&game_board.mutex);

        refresh_screen();

        while (true)
        {
            int result = play_board(&game_board);

            if (result == CREATE_BACKUP)
            {
                // Só cria backup se ainda não existir nenhum
                if (!backup_exists)
                {
                    int pid = fork();
                    if (pid < 0)
                    {
                        perror("fork");
                        // Não conseguimos criar backup, continuamos o jogo normalmente
                    }
                    else if (pid > 0)
                    {
                        // === Processo PAI ===
                        // Este processo fica como "snapshot" à espera do filho
                        int status;
                        backup_exists = 1;

                        wait(&status);

                        // Se o filho terminou porque o Pacman morreu -> reencarnar
                        if (WIFEXITED(status) && WEXITSTATUS(status) == 1)
                        {
                            // voltar a não ter backup ativo
                            backup_exists = 0;

                            // Recomeçar este nível a partir do estado guardado
                            screen_refresh(&game_board, DRAW_MENU);
                            continue; // volta ao topo do while(true) com o mesmo game_board
                        }
                        else
                        {
                            // O filho terminou por vitória, por 'Q' ou outro motivo -> sai do jogo
                            quit_game = true;
                            break;
                        }
                    }
                    else
                    {
                        // === Processo FILHO ===
                        // Este continua o jogo ativo a partir do estado atual
                        backup_exists = 1; // para impedir novo 'G'
                        continue;          // volta ao topo e joga normalmente
                    }
                }

                // Já existia backup: ignora novo 'G'
                continue;
            }

            if (result == NEXT_LEVEL)
            {
                screen_refresh(&game_board, DRAW_WIN);
                sleep_ms(game_board.tempo);

                accumulated_points = game_board.pacmans[0].points;

                // Passa para o próximo nível na lista
                current_level_idx++;
                break;
            }

            if (result == QUIT_GAME)
            {
                screen_refresh(&game_board, DRAW_GAME_OVER);
                sleep_ms(game_board.tempo);

                // se existe backup e o Pacman está morto, sinaliza ao pai para reencarnar
                if (backup_exists && !game_board.pacmans[0].alive)
                {
                    _exit(1); // código 1 = Pacman morto com backup
                }

                quit_game = true; // Marca para sair de tudo
                break;
            }

            screen_refresh(&game_board, DRAW_MENU);

            // Atualiza pontos locais para visualização
            accumulated_points = game_board.pacmans[0].points;
        }

        pthread_rwlock_wrlock(&game_board.mutex);
        game_board.game_running = 0;
        pthread_rwlock_unlock(&game_board.mutex);

        // Esperar que cada fantasma termine (Join)
        for (int i = 0; i < game_board.n_ghosts; i++)
        {
            pthread_join(game_board.ghosts[i].thread_id, NULL);
        }

        // Limpa a memória do nível que acabou de ser jogado antes de carregar o próximo
        // print_board(&game_board);
        unload_level(&game_board);
    }

    terminal_cleanup();

    close_debug_file();

    return 0;
}
