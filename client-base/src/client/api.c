#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>

struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1, .req_pipe = -1, .notif_pipe = -1};

static void sent_fixed_string(int fd, const char *str){
  char buffer[MAX_PIPE_PATH_LENGTH];
  memset(buffer, 0, MAX_PIPE_PATH_LENGTH); 
  strncpy(buffer, str, MAX_PIPE_PATH_LENGTH - 1);
  write(fd, buffer, MAX_PIPE_PATH_LENGTH);
}

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
    unlink(req_pipe_path);
    unlink(notif_pipe_path);

    if(mkfifo(req_pipe_path, 0666) != 0){
      perror("Erro ao criar fifo de requisição");
      return -1;
    }

    if (mkfifo(notif_pipe_path, 0666) != 0 ) {
      perror("Erro ao criar FIFO de notificação");
      unlink(req_pipe_path);
      return 1;
    }

    // Guardar os caminhos 
    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

    //Conectar ao Pipe de Registo do Servidor
    int server_fd = open(server_pipe_path, O_WRONLY);
    if (server_fd == -1) {
        perror("Erro ao abrir FIFO do servidor");
        return 1;
    }

    char op = OP_CODE_CONNECT;
    if (write(server_fd, &op, sizeof(char)) == -1) {
        perror("Erro ao escrever no server pipe");
        close(server_fd);
        return 1;
    }
    
    sent_fixed_string(server_fd, req_pipe_path);
    sent_fixed_string(server_fd, notif_pipe_path);
    
    
    close(server_fd);

    session.notif_pipe = open(notif_pipe_path, O_RDONLY);
    if (session.notif_pipe == -1) {
        perror("Erro ao abrir pipe de notificação");
        return 1;
    }

    char response_op, result;
    read(session.notif_pipe, &response_op, sizeof(char));
    read(session.notif_pipe, &result, sizeof(char));

    if (result != 0) {
        fprintf(stderr, "Servidor recusou conexão.\n");
        close(session.notif_pipe);
        return 1;
    }

    session.req_pipe = open(req_pipe_path, O_WRONLY);
    if (session.req_pipe == -1) {
        perror("Erro ao abrir pipe de requisição");
        return 1;
    }

    return 0;
}

void pacman_play(char command) {
  if (session.req_pipe == -1) return;

  char op = OP_CODE_PLAY;
  write(session.req_pipe, &op, sizeof(char));
  write(session.req_pipe, &command, sizeof(char));
}

int pacman_disconnect() {
  if (session.req_pipe != -1) {
      char op = OP_CODE_DISCONNECT;
      write(session.req_pipe, &op, sizeof(char));
      
      close(session.req_pipe);
      session.req_pipe = -1;
  }
  
  if (session.notif_pipe != -1) {
      close(session.notif_pipe);
      session.notif_pipe = -1;
  }

  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);
  
  return 0;
}

Board receive_board_update(void) {
  Board b = {0}; 
  
  if (session.notif_pipe == -1) {
      b.game_over = 1;
      return b;
  }

  char op;
  int n = read(session.notif_pipe, &op, sizeof(char));
  
  if (n <= 0) {
      b.game_over = 1; 
      return b;
  }

  if (op != OP_CODE_BOARD) {
      b.game_over = 1;
      return b;
  }

  // lewr byte a byte do tabuleiro na ordem 
  read(session.notif_pipe, &b.width, sizeof(int));
  read(session.notif_pipe, &b.height, sizeof(int));
  read(session.notif_pipe, &b.tempo, sizeof(int));
  read(session.notif_pipe, &b.victory, sizeof(int));
  read(session.notif_pipe, &b.game_over, sizeof(int));
  read(session.notif_pipe, &b.accumulated_points, sizeof(int));

  
  int size = b.width * b.height;
  b.data = malloc(size * sizeof(char));
  
  if (b.data) {
      read(session.notif_pipe, b.data, size);
  } else {
      perror("Erro de alocação de memória no cliente");
      b.game_over = 1;
  }

  return b;
}