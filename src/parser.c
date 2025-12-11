#define _DEFAULT_SOURCE
#include "../include/board.h" // Adjust the path to the correct location of board.h
#include "../include/display.h"
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

int get_next_token(int fd, char *buffer, int max_len)
{
    char c;
    int idx = 0;
    int in_comment = 0;
    int bytes_read;

    while ((bytes_read = read(fd, &c, 1)) > 0)
    {
        // Tratamento de comentários
        if (c == '#')
        {
            in_comment = 1;
            continue;
        }
        if (in_comment)
        {
            if (c == '\n')
                in_comment = 0;
            continue;
        }

        // Se for espaço (space, tab, newline)
        if (isspace(c))
        {
            if (idx > 0)
            {
                // Se já tínhamos lido caracteres, terminamos o token aqui
                buffer[idx] = '\0';
                return 1;
            }
            // Se ainda não lemos nada, continuamos a ignorar os espaços
            continue;
        }

        // Acumula o carácter no buffer
        if (idx < max_len - 1)
        {
            buffer[idx++] = c;
        }
    }

    if (idx > 0)
    {
        buffer[idx] = '\0';
        return 1;
    }
    return 0; // EOF
}

void load_entity_behavior(const char *path, board_t *board, char type, int index)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Erro ao abrir ficheiro de entidade %s", path);
        perror(err_msg);
        return;
    }

    char token[128];
    pacman_t *p = NULL;
    ghost_t *g = NULL;
    command_t *moves_array = NULL;
    int *n_moves_ptr = NULL;

    if (type == 'P')
    {
        p = &board->pacmans[index];
        moves_array = p->moves;
        n_moves_ptr = &p->n_moves;
        p->n_moves = 0; 
    }
    else
    {
        g = &board->ghosts[index];
        moves_array = g->moves;
        n_moves_ptr = &g->n_moves;
        g->n_moves = 0; 
    }

    while (get_next_token(fd, token, sizeof(token)))
    {
        if (strcmp(token, "PASSO") == 0)
        {
            char val[16];
            get_next_token(fd, val, sizeof(val));
            if (type == 'P')
                p->passo = atoi(val);
            else
                g->passo = atoi(val);
        }
        else if (strcmp(token, "POS") == 0)
        {
            char row[16], col[16];
            get_next_token(fd, row, sizeof(row));
            get_next_token(fd, col, sizeof(col));
            if (type == 'P')
            {
                p->pos_y = atoi(row);
                p->pos_x = atoi(col);
                p->alive = 1;
                board->board[p->pos_y * board->width + p->pos_x].content = 'P';
            }
            else
            {
                g->pos_y = atoi(row);
                g->pos_x = atoi(col);
            }
        }
        else
        {

            if (*n_moves_ptr < MAX_MOVES)
            {
                char cmd = token[0];
                int turns = 1;
                if (cmd == 'T'){
                    if (strlen(token) > 1)
                    {
                        // Exemplo: T2
                        turns = atoi(token + 1);
                    }
                    else
                    {
                        char duration_token[16];
                        if (get_next_token(fd, duration_token, sizeof(duration_token)))
                        {
                            turns = atoi(duration_token);
                        }
                    }
                }

                moves_array[*n_moves_ptr].command = cmd;
                moves_array[*n_moves_ptr].turns = turns;
                moves_array[*n_moves_ptr].turns_left = turns;
                (*n_moves_ptr)++;
            }
        }
    }
    close(fd);
}

int load_level_from_file(const char *filepath, board_t *board, const char *base_dir)
{
    int fd = open(filepath, O_RDONLY);
    if (fd < 0)
    {
        perror("Erro ao abrir ficheiro de nível");
        return -1;
    }
    memset(board, 0, sizeof(board_t));
    char token[128];
    // Valores default
    board->n_pacmans = 0;
    board->n_ghosts = 0;
    board->pacman_file[0] = '\0';

    while (get_next_token(fd, token, sizeof(token)))
    {
        if (strcmp(token, "DIM") == 0)
        {
            char h[16], w[16]; // altura e largura
            get_next_token(fd, h, sizeof(h));
            get_next_token(fd, w, sizeof(w));
            board->height = atoi(h);
            board->width = atoi(w);
            // Alocar memória
            board->board = calloc(board->width * board->height, sizeof(board_pos_t));
            // max 1 pacman
            board->pacmans = calloc(1, sizeof(pacman_t));
            board->ghosts = calloc(MAX_GHOSTS, sizeof(ghost_t));
        }
        else if (strcmp(token, "TEMPO") == 0)
        {
            char t[16];
            get_next_token(fd, t, sizeof(t));
            board->tempo = atoi(t);
        }

        else if (strcmp(token, "PAC") == 0)
        {
            get_next_token(fd, board->pacman_file, sizeof(board->pacman_file));
            board->n_pacmans = 1;
            // debug("entrou no ficheiro do pac");

            // Carregar comportamento do Pacman
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, board->pacman_file);

            load_entity_behavior(full_path, board, 'P', 0);
        }
        else if (strcmp(token, "MON") == 0)
        {
            while (1)
            {
                char temp_token[128];

                if (!get_next_token(fd, temp_token, sizeof(temp_token)))
                    break;

                if (strstr(temp_token, ".m") != NULL)
                {
                    if (board->n_ghosts < MAX_GHOSTS)
                    {
                        strcpy(board->ghosts_files[board->n_ghosts], temp_token);

                        char full_path[512];
                        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, temp_token);
                        load_entity_behavior(full_path, board, 'M', board->n_ghosts);

                        board->n_ghosts++;
                    }
                }
                else
                {
                    // Este token é a primeira linha do lvl
                    int row = 0;
                    int col = 0;

                    // Processar a primeira linha (temp_token)
                    for (size_t i = 0; i < strlen(temp_token) && col < board->width; i++)
                    {
                        char c = temp_token[i];
                        int idx = row * board->width + col;
                        if (idx < board->width * board->height)
                        {
                            board->board[idx].content = (c == 'X') ? 'W' : ' ';
                            board->board[idx].has_portal = (c == '@');
                            board->board[idx].has_dot = (c == 'o') ? 1 : 0;
                        }
                        col++;
                    }

                    // Avançar para a próxima linha
                    row++;
                    col = 0;

                    // Ler o resto do mapa linha por linha
                    char c;
                    while (read(fd, &c, 1) > 0 && row < board->height)
                    {
                        if (c == '\n' || c == '\r')
                        {
                            // Nova linha
                            if (col > 0)
                            {
                                row++;
                                col = 0;
                            }
                            continue;
                        }

                        if (isspace(c))
                            continue;

                        if (col < board->width)
                        {
                            int idx = row * board->width + col;
                            if (idx < board->width * board->height)
                            {
                                board->board[idx].content = (c == 'X') ? 'W' : ' ';
                                board->board[idx].has_portal = (c == '@');
                                board->board[idx].has_dot = (c == 'o') ? 1 : 0;
                            }
                            col++;
                        }
                    }
                    break; // Sai do loop MON apenas
                }
            }
        }
    }

    // Se não houver ficheiro .p, o Pacman é manual
    // debug("numero de pacmans  == %i", board->n_pacmans);
    if (board->n_pacmans == 0)
    {
        board->n_pacmans = 1;
        board->pacmans[0].n_moves = 0; // Manual
        board->pacmans[0].alive = 1;
        board->pacmans[0].pos_x = 1;
        board->pacmans[0].pos_y = 1;

        board->board[1 * board->width + 1].content = 'P';
    }
    if (pthread_rwlock_init(&board->mutex, NULL) != 0)
    {
        perror("Erro ao inicializar lock do tabuleiro");
        return -1;
    }
    board->game_running = 1;

    close(fd);
    return 0;
}

bool has_lvl_extension(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    return ext && strcmp(ext, ".lvl") == 0;
}

int load_levels_from_dir(const char *levels_directory, char level_files[][MAX_FILENAME], int *numLevels)
{
    struct dirent **namelist;
    int n;

    // Usa scandir com 'alphasort' para garantir q vai ler 1.lvl, 2.lvl por ordem
    n = scandir(levels_directory, &namelist, NULL, alphasort);

    if (n < 0)
    {
        perror("Erro ao fazer leitura da diretoria");
        return -1;
    }

    *numLevels = 0;

    for (int i = 0; i < n; i++)
    {
        if (has_lvl_extension(namelist[i]->d_name))
        {
            if (*numLevels < MAX_LEVELS)
            {

                strncpy(level_files[*numLevels], namelist[i]->d_name, MAX_FILENAME - 1);
                level_files[*numLevels][MAX_FILENAME - 1] = '\0'; // Garante null-terminator
                (*numLevels)++;
            }
        }
        free(namelist[i]); // Liberta a memória alocada pelo scandir para cada entrada
    }
    free(namelist); // Liberta a lista

    return 0;
}
