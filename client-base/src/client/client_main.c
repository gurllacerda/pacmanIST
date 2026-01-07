#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

Board board;
bool stop_execution = false;
int tempo = 200;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static bool should_stop(void)
{
    bool s;
    pthread_mutex_lock(&mutex);
    s = stop_execution;
    pthread_mutex_unlock(&mutex);
    return s;
}

static void request_stop(void)
{
    pthread_mutex_lock(&mutex);
    stop_execution = true;
    pthread_mutex_unlock(&mutex);
}

int get_next_token(FILE *fd, char *buffer, int max_len)
{
    char c;
    int idx = 0;
    int in_comment = 0;
    // int bytes_read;

    while ((c = fgetc(fd)) != EOF)
    {
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

        if (isspace(c))
        {
            if (idx > 0)
            {
                buffer[idx] = '\0';
                return 1;
            }
            continue;
        }

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
    return 0;
}

static void *receiver_thread(void *arg)
{
    (void)arg;

    while (true)
    {

        Board board = receive_board_update();

        if (!board.data)
        {
            request_stop();
            return NULL;
        }

        draw_board_client(board);
        refresh_screen();

        if (board.game_over != 0)
        {
            free(board.data);
            request_stop();
            return NULL;
        }

        free(board.data);
    }

    debug("Returning receiver thread...\n");
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 3 && argc != 4)
    {
        fprintf(stderr,
                "Usage: %s <client_id> <register_pipe> [commands_file]\n",
                argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    FILE *cmd_fp = NULL;
    if (commands_file)
    {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp)
        {
            perror("Failed to open commands file");
            return 1;
        }
    }

    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0)
    {
        perror("Failed to connect to server");
        return 1;
    }

    pthread_t receiver_thread_id;
    pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL);

    terminal_init();
    set_timeout(50);
    draw_board_client(board);
    refresh_screen();

    // char command;
    // int ch;

    char token[128];
    while (1)
    {
        if (should_stop())
        {
            sleep_ms(2000);
            terminal_cleanup();
            break;
        }
        char command = '\0';

        if (cmd_fp)
        {
            if (get_next_token(cmd_fp, token, sizeof(token)) == 0)
            {
                rewind(cmd_fp);
                continue;
            }

            if (strcmp(token, "PASSO") == 0)
            {
                get_next_token(cmd_fp, token, sizeof(token));
                continue;
            }
            if (strcmp(token, "POS") == 0)
            {
                get_next_token(cmd_fp, token, sizeof(token));
                get_next_token(cmd_fp, token, sizeof(token));
                continue;
            }

            command = toupper(token[0]);

            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);
            while (wait_for > 0 && !should_stop())
            {
                int step = wait_for > 50 ? 50 : wait_for;
                sleep_ms(step);
                wait_for -= step;
            }
        }
        else
        {
            command = get_input();
            command = toupper(command);
        }

        if (command == '\0')
            continue;

        pacman_play(command);

        if (command == 'Q')
            break;

        if (should_stop())
        {
            sleep_ms(2000);
            terminal_cleanup();
            break;
        }
    }
    pacman_disconnect();

    pthread_join(receiver_thread_id, NULL);

    if (cmd_fp)
        fclose(cmd_fp);

    pthread_mutex_destroy(&mutex);

    terminal_cleanup();

    return 0;
}
