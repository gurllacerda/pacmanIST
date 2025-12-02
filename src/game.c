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

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

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

    int result = move_pacman(game_board, 0, play);
    if (result == REACHED_PORTAL)
    {
        // Next level
        return NEXT_LEVEL;
    }

    if (result == DEAD_PACMAN)
    {
        return QUIT_GAME;
    }

    for (int i = 0; i < game_board->n_ghosts; i++)
    {
        ghost_t *ghost = &game_board->ghosts[i];
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the ghost
        if (ghost->n_moves > 0)
        {
            move_ghost(game_board, i, &ghost->moves[ghost->current_move % ghost->n_moves]);
        }
    }

    if (!game_board->pacmans[0].alive)
    {
        return QUIT_GAME;
    }

    return CONTINUE_PLAY;
}

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
    // bool end_game = false;
    // board_t game_board;

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
        if (game_board.n_pacmans > 0)
        {
            game_board.pacmans[0].points = accumulated_points;
        }

        draw_board(&game_board, DRAW_MENU);
        refresh_screen();

        while (true)
        {
            int result = play_board(&game_board);

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
                quit_game = true; // Marca para sair de tudo
                break;
            }

            screen_refresh(&game_board, DRAW_MENU);

            // Atualiza pontos locais para visualização
            accumulated_points = game_board.pacmans[0].points;
        }

        // Limpa a memória do nível que acabou de ser jogado antes de carregar o próximo
        // print_board(&game_board);
        unload_level(&game_board);
    }

    terminal_cleanup();

    close_debug_file();

    return 0;
}
