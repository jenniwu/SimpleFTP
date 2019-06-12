#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

typedef int bool;
#define true 1
#define false 0

#define MAX_LINE_LENGTH 1024    //todo: RFC specifies 512 chars?

struct net_buffer {
  int    fd;
  size_t max_bytes;
  size_t avail_data;
  char   buf[0];
};

enum commands {USER=1, PASS=2, STAT=3, LIST=4, RETR=5, DELE=6, NOOP=7, RSET=8, QUIT=9};

char template[]="tmp_XXXXXX";
bool expectUSER = true;
bool expectPASS = false;
bool auth = false;
bool mailLoaded = false;
mail_list_t maildrop;
int mailcount;
int sessionMailCount = 0;
size_t mailsize;
char* user;
char* pass;



// method declarations
static void handle_client(int fd);
int cmdHandler(char* line);
unsigned int getArg(char* line);
void loadMail();
void updateMailCountAndSize();
void handleUser(int fd, char *aLineFromClient, size_t lineLen);
void handlePass(int fd, char *aLineFromClient, size_t lineLen);
void handleStat(int fd, size_t lineLen);
void handleList(int fd, char *aLineFromClient);
void handleRetr(int fd, char *aLineFromClient, size_t lineLen);
void handleDele(int fd, char *aLineFromClient);
void handleNoop(int fd);
void handleRset(int fd);
void userLogout(int fd);



// programmatic entry point
int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
    return 1;
  }

  run_server(argv[1], handle_client);
  return 0;
}


void handle_client(int fd) {

  // connection established, send greeting
  send_string(fd, "+OK POP3 Server ready\r\n");

  // create buffer
  net_buffer_t netBuffer = nb_create(fd, MAX_LINE_LENGTH);
  size_t recvLineSize = netBuffer->max_bytes+1;

  while(1) {
    char *aLineFromClient = malloc(recvLineSize);
    int recvByteCount = nb_read_line(netBuffer, aLineFromClient);

    switch (recvByteCount) {
      case -1:
//        printf("connection interrupted!\n");
        unlink(template);
        return;
      case 0:
//        printf("connection terminated by client!\n");
        unlink(template);
        return;
      default:
//        printf("client says: %s", aLineFromClient);
//        printf("size: %d\n", recvByteCount);
        break;
    }

    size_t lineLen = strlen(aLineFromClient);
    //printf("%d\n", cmdHandler(aLineFromClient));

    switch(cmdHandler(aLineFromClient)) {
      // authorization state
      case USER:
        handleUser(fd, aLineFromClient, lineLen);
        break;
      case PASS:
        handlePass(fd, aLineFromClient, lineLen);
        break;
      // transaction state
      case STAT:
        handleStat(fd, lineLen);
        break;
      case LIST:
        handleList(fd, aLineFromClient);
        break;
      case RETR:
        handleRetr(fd, aLineFromClient, lineLen);
        break;
      case DELE:
        handleDele(fd, aLineFromClient);
        break;
      case NOOP:
        handleNoop(fd);
        break;
      case RSET:
        handleRset(fd);
        break;
      case QUIT:
        if(auth) {
          userLogout(fd);
        } else {
          send_string(fd, "+OK POP3 server signing off\r\n");
          unlink(template);
          return;
        }
        break;
      case 10:  //entered command that expects argument but received none
        send_string(fd, "-ERR enter an argument\r\n");
        if(expectPASS) {
          free(user);
          expectUSER = true;
          expectPASS = false;
        }
        break;
      case 0:
        send_string(fd, "-ERR unsupported command\r\n");
        break;
      case -1:
        send_string(fd, "-ERR invalid command\r\n");
        break;
      default:
        printf("ERROR: unexpected command\r\n");
        break;
    }

    free(aLineFromClient);
  }
}


// handles QUIT commands in transaction state
void userLogout(int fd) {
  if(mailLoaded) {
    destroy_mail_list(maildrop);
    send_string(fd, "+OK POP3 server signing off (%d messages left)\r\n", mailcount);
  }
  sessionMailCount = 0;
  auth = false;
  mailLoaded = false;
  expectUSER = true;
  free(user);
  free(pass);
}


// handles RSET command
void handleRset(int fd) {
  if(!auth) {
    send_string(fd, "-ERR command not valid in current state\r\n");
  } else {
    if(mailLoaded) {
      reset_mail_list_deleted_flag(maildrop);
      updateMailCountAndSize();
      send_string(fd, "+OK maildrop has %d messages (%zu octets)\r\n", mailcount, mailsize);
    }
  }
}


// handle NOOP command
void handleNoop(int fd) {
  if(!auth) {
    send_string(fd, "-ERR command not valid in current state\r\n");
  } else {
    send_string(fd, "+OK\r\n");
  }
}


// handle DELE command
void handleDele(int fd, char *aLineFromClient) {
  if(!auth) {
    send_string(fd, "-ERR command not valid in current state\r\n");
  } else {
    if(mailLoaded) {
      if (strlen(aLineFromClient) == 6) {
        send_string(fd, "-ERR a message number is required\r\n");
      } else {
        unsigned int i = getArg(aLineFromClient);
        if (i == 0L) {
          send_string(fd, "-ERR invalid message number\r\n");
        } else if (i > sessionMailCount) {
          send_string(fd, "-ERR no such message, there are only %d messages in maildrop\r\n", mailcount);
        } else if (get_mail_item(maildrop, i-1) != NULL) {
          mark_mail_item_deleted(get_mail_item(maildrop, i-1));
          send_string(fd, "+OK message %d deleted\r\n", i);
        } else {
          send_string(fd, "-ERR message already deleted\r\n");
        }

        updateMailCountAndSize();
      }
    }
  }
}


// handle RETR command
void handleRetr(int fd, char *aLineFromClient, size_t lineLen) {
  if(!auth) {
    send_string(fd, "-ERR command not valid in current state\r\n");
  } else {
    if(mailLoaded) {
      if(lineLen == 6) {
        send_string(fd, "-ERR a message number is required\r\n");
      } else {
        unsigned int i = getArg(aLineFromClient);
        if (i == 0L) {
          send_string(fd, "-ERR invalid message number\r\n");
        } else if (i > sessionMailCount) {
          send_string(fd, "-ERR no such message, there are only %d messages in maildrop\r\n", mailcount);
        } else {
          mail_item_t mi = get_mail_item(maildrop, i-1);
          if (mi != NULL) {
            send_string(fd, "+OK %zu octets\r\n", get_mail_item_size(mi));
            const char *mailname = get_mail_item_filename(mi);

            FILE * fp;
            char * lineRead = NULL;
            size_t len = 0;

            fp = fopen(mailname, "r");
            //if (fp == NULL) return;   // case should never happen

            while (getline(&lineRead, &len, fp) != -1) {
              send_string(fd, lineRead);
            }

            send_string(fd, ".\r\n");

            fclose(fp);
            if (lineRead)
              free(lineRead);
          } else {
            send_string(fd, "-ERR message was deleted\r\n");
          }
        }
      }
    }
  }
}


// handle LIST command
void handleList(int fd, char *aLineFromClient) {
  if(!auth) {
    send_string(fd, "-ERR command not valid in current state\r\n");
  } else {
    if(mailLoaded) {
      if(strlen(aLineFromClient) == 6) {
        send_string(fd, "+OK %d messages (%zu octets)\r\n", mailcount, mailsize);
        for(unsigned int i=0; i<sessionMailCount; i++) {
          if(get_mail_item(maildrop, i) != NULL) {
            send_string(fd, "%d %zu\r\n", i+1, get_mail_item_size(get_mail_item(maildrop, i)));
          }
        }
        send_string(fd, ".\r\n");
      } else {
        unsigned int i = getArg(aLineFromClient);
        if(i == 0L) {
          send_string(fd, "-ERR invalid message number\r\n");
        } else if(i > sessionMailCount) {
          send_string(fd, "-ERR no such message, there are only %d messages in maildrop\r\n", mailcount);
        } else if(get_mail_item(maildrop, i-1) != NULL) {
          send_string(fd, "+OK %d %zu\r\n", i, get_mail_item_size(get_mail_item(maildrop, i-1)));
        } else {
          send_string(fd, "-ERR message was deleted\r\n");
        }
      }
    }
  }
}


// handle STAT command
void handleStat(int fd, size_t lineLen) {
  if(!auth) {
    send_string(fd, "-ERR command not valid in current state\r\n");
  } else {
    if(lineLen > 6) {
      send_string(fd, "-ERR command does not need arguments\r\n");
    } else {
      if(mailLoaded) {
        send_string(fd, "+OK %d %zu\r\n", mailcount, mailsize);
      }
    }
  }
}


// handle PASS command
void handlePass(int fd, char *aLineFromClient, size_t lineLen) {
  if(!expectPASS) {
    send_string(fd, "-ERR PASS command not expected\r\n");
  } else if (lineLen > 7) {
    pass = malloc(lineLen-6);
    strncpy(pass, aLineFromClient+5, lineLen-5-2);
    pass[lineLen-5-2] = '\0'; //add null termination char

    if (is_valid_user(user, pass)) {
      loadMail();
      auth = true;
      expectPASS = false;
      sessionMailCount = mailcount;
      send_string(fd, "+OK %s's mailbox has %d messages (%zu octets)\r\n", user, mailcount, mailsize);
    } else {      // send client back to state expecting USER command
      free(user);
      free(pass);
      expectPASS = false;
      expectUSER = true;
      send_string(fd, "-ERR invalid password\r\n");
    }
  } else {
    expectPASS = false;
    expectUSER = true;
    send_string(fd, "-ERR enter a password\r\n");
  }
}


// handles USER command
void handleUser(int fd, char *aLineFromClient, size_t lineLen) {
  if(expectUSER) {
    if (lineLen > 7) {
      user = malloc(lineLen - 6);
      strncpy(user, aLineFromClient + 5, lineLen - 5 - 2);
      user[lineLen - 5 - 2] = '\0'; //add null termination char

      if (is_valid_user(user, NULL)) {
        expectPASS = true;
        expectUSER = false;
        send_string(fd, "+OK %s user valid\r\n", user);
      } else {
        send_string(fd, "-ERR invalid user\r\n");
      }
    } else {
      send_string(fd, "-ERR enter a user\r\n");
    }
  } else send_string(fd, "-ERR USER command not expected\r\n");
}


// gets int in argument of command
unsigned int getArg(char* line) {
  char* argBeforeWhiteSpace = malloc(strlen(line)-6);
  int i;

  for(i=5; i<(strlen(line)-2); i++) {
    if(!isspace(line[i])) {
      argBeforeWhiteSpace[i-5] = line[i];
    } else break;
  }

  argBeforeWhiteSpace[i] = '\0';
  unsigned int n = (unsigned int) strtol(argBeforeWhiteSpace, &line, 10);
  free(argBeforeWhiteSpace);

  return n;
}


// compares the equality of 2 chars without case sensitivity
// returns 1 if the chars are equal, 0 otherwise
bool caseInsensitiveCompare(char a, char b) {
  return tolower(a) == tolower(b);
}


// compares command with readline
bool compareCommand(char* cmd, char* line, size_t n){
  for(int i=0; i<n; i++) {
    //printf("the %dth character is: %c, %c\n", i, cmd[i], line[i]);
    if(!caseInsensitiveCompare(cmd[i], line[i])) {
      return false;
    }
  }

  return true;
}


// find supported commands
int cmdHandler(char* line) {
  //checks if valid supported command
  if(strlen(line)<4) return -1;

  //supported commands
  if(compareCommand("USER ", line, 5)) {
    return USER;
  } if(compareCommand("PASS ", line, 5)) {
    return PASS;
  } if(compareCommand("STAT", line, 4)) {
    return STAT;
  } if(compareCommand("LIST", line, 4)) {
    return LIST;
  } if(compareCommand("LIST ", line, 5)) {
    return LIST;
  } if(compareCommand("RETR ", line, 5)) {
    return RETR;
  } if(compareCommand("DELE ", line, 5)) {
    return DELE;
  } if(compareCommand("NOOP", line, 4)) {
    return NOOP;
  } if(compareCommand("RSET", line, 4)) {
    return RSET;
  } if(compareCommand("QUIT", line, 4)) {
    return QUIT;
  }

  // unsupported commands
  if(compareCommand("UIDL", line, 4) || compareCommand("APOP", line, 4) || compareCommand("TOP", line, 3)) {
    return 0;
  }

  if(compareCommand("USER", line, 4) || compareCommand("PASS", line, 4) || compareCommand("RETR", line, 4) || compareCommand("DELE", line, 4)) {
    return 10;
  }

  // invalid commands
  return -1;
}


// load the maildrop into variables
void loadMail() {
  maildrop = load_user_mail(user);
  mailcount = get_mail_count(maildrop);
  mailsize = get_mail_list_size(maildrop);
  mailLoaded = true;
}


void updateMailCountAndSize() {
  mailcount = get_mail_count(maildrop);
  mailsize = get_mail_list_size(maildrop);
}