#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

typedef int bool;
#define true 1
#define false 0

enum commands {
    HELO = 1, MAIL = 2, RCPT = 3, DATA = 4, NOOP = 5, QUIT = 6
};

//Copy from netbuffer.c
struct net_buffer {
    int fd;
    size_t max_bytes;
    size_t avail_data;
    char buf[0];
};

//I wrote my own recipient linked list
struct recipient {
    char *recipient;
    struct recipient *next;
};

//declare state flags
bool haveHelo = false;
bool haveMail = false;
bool haveRcpt = false;

//declare global variables
char *clientName;
char *sender;
char *aRecipient;
struct recipient *recipientList;
char *dataLine;
bool readData = false;
char template[] = "tmp_XXXXXX";
int tempFile;

//declare function signatures, see actual functions for explanations
bool isLastDataLine(char *in, int lineSize);
static void handle_client(int fd);
void resetTempName();
bool goodStr(char *in);
void printStringInx(char *in);
void charcpy(char *out, char *in);
int parseLine(char *out, char *in, int lineSize);
int findEndingBrace(char *in, int lineSize);
int parseMail(char *out, char *in, int lineSize);
int parseRcpt(char *out, char *in, int lineSize);
void addToRcptList(char *aRecipient, int size);
void killRcptList();
void printAllRcpt();
bool charMatch(char a, char b);
bool cmdMatch(char *CMD, char *line);
bool cmdMatchOffset(char *CMD, char *line, int size, int offset);
int cmdHandler(char *Line);

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
        return 1;
    }

    run_server(argv[1], handle_client);

    return 0;
}

void handle_client(int fd) {

    // TODO To be implemented
    //printf("In handler.\n");
    send_string(fd, "220 Connection established\n");

    //create buffer
    net_buffer_t recvBuffer = nb_create(fd, MAX_LINE_LENGTH);

    //prepare to read line, set size
    size_t recvLineSize = recvBuffer->max_bytes + 1;

    //Try to read lines repeatedly
    while (1) {
        char *aLineFromClient = malloc(recvLineSize);

        int recvByteCount = nb_read_line(recvBuffer, aLineFromClient);
        //See if connection is good:
        //Terminate the process if connection is closed
        switch (recvByteCount) {
            case -1:
                printf("Connection interrupted!\n");
                unlink(template);
                return;
            case 0:
                printf("Connection terminated by client!\n");
                unlink(template);
                return;
            default:
                printf("Client says: %s, size: %d\n", aLineFromClient, recvByteCount);
                break;
        }

        //printf("%d\n", cmdHandler(aLineFromClient));

        //If we are trying to read data:
        if (readData) {
            //if (aLineFromClient[0] != '.' || recvByteCount > 3) {//Read until we have a single line of dot
            if(!isLastDataLine(aLineFromClient,recvByteCount)){
                //Write to cache file until see a line of only .
                write(tempFile, aLineFromClient, recvByteCount);
            } else {
                //sending a mail is complete
                //reset state flags:
                readData = false;
                haveMail = false;
                haveRcpt = false;
                //Tell user we got the message, same email for each recipient, delete temp file
                printf("Temp File name: %s\n", template);
                send_string(fd, "250 OK\n");
                save_user_mail(template, (user_list_t) recipientList);
                unlink(template);
                resetTempName();
                printAllRcpt();
            }
        } else {
            //Determine what command we received
            switch (cmdHandler(aLineFromClient)) {
                case HELO:
                    if (recvByteCount <= 7) {//Command has no name
                        printf("Hello no name\n");
                        send_string(fd, "501 helo requires name\n");
                        break;
                    }
                    clientName = malloc(recvByteCount - 5);
                    int heloParseResult = parseLine(clientName, aLineFromClient, recvByteCount);
                    if (heloParseResult == -1) {//Command format is wrong
                        printf("Command format is wrong");
                        send_string(fd, "501 Unacceptable command argument\n");
                    } else {
                        if (!haveHelo) {//Expecting helo
                            printf("Client name: %s\n", clientName);
                            send_string(fd, "250 Hello, %s\n", clientName);
                            haveHelo = true;
                        } else {//Not expectiong helo
                            printf("Duplicate HELO\n");
                            send_string(fd, "503 Bad sequence of commands\n");
                        }
                    }
                    break;
                case MAIL:
                    if (!haveHelo) {//Not expecting helo
                        printf("No hello\n");
                        send_string(fd, "503 Bad sequence of commands\n");
                        break;
                    }
                    sender = malloc(recvByteCount - 5);
                    int mailParseResult = parseMail(sender, aLineFromClient, recvByteCount);
                    if (mailParseResult == 0) {//Bad sender address format
                        printf("Bad sender address format");
                        send_string(fd, "500 Invalid command\n");
                    } else if (mailParseResult == -1) {//Bad command argument
                        send_string(fd, "501 Unacceptable command argument\n");
                    } else {
                        printf("Sender name: %s\n", sender);
                        killRcptList();//Clear current list of recipients
                        haveMail = true;
                        send_string(fd, "250 OK\n");
                    }
                    break;
                case RCPT:
                    if (!haveHelo || !haveMail) {//wrong command sequence
                        printf("No hello\n");
                        send_string(fd, "503 Bad sequence of commands\n");
                        break;
                    }
                    aRecipient = malloc(recvByteCount - 5);
                    int rcptParseResult = parseRcpt(aRecipient, aLineFromClient, recvByteCount);
                    if (rcptParseResult == 0) {
                        send_string(fd, "500 Invalid command\n");
                    } else if (rcptParseResult == -1) {
                        send_string(fd, "501 Unacceptable command argument\n");
                    } else {
                        if (sender != NULL) {
                            if (is_valid_user(aRecipient, NULL)) {
                                printf("Recipient name: %s\n", aRecipient);
                                haveRcpt = true;
                                addToRcptList(aRecipient, recvByteCount - 5);//add to linked list
                                send_string(fd, "250 OK, Recipient name: %s\n", aRecipient);
                            } else {
                                printf("Not a recipient: %s\n", aRecipient);
                                send_string(fd, "550 No such user - %s\n", aRecipient);
                            }
                        } else {
                            printf("No sender: %s\n", aRecipient);
                            send_string(fd, "503 Bad sequence of commands\n");
                        }
                    }
                    free(aRecipient);
                    break;
                case DATA:
                    if (!haveHelo || !haveRcpt || !haveMail) {//wrong command sequence
                        printf("No hello\n");
                        send_string(fd, "503 Bad sequence of commands\n");
                        break;
                    }
                    if (sender == NULL) {//We haven't received valid sender yet
                        send_string(fd, "503 Bad sequence of commands\n");
                    } else if (recipientList == NULL) {//We haven't received VALID recipients yet
                        send_string(fd, "554 No valid recipients\n");
                    } else {
                        readData = true;
                        printf("Data read start\n");
                        if ((tempFile = mkstemp(template)) == -1) {
                            printf("Cannot create temp file.\n");
                        }
                        send_string(fd, "354 Enter mail, end with .\n");
                    }
                    break;
                case NOOP:
                    //Do nothing
                    break;
                case QUIT:
                    //return to wait for helo
                    send_string(fd, "221\n");
                    //reset state flags:
                    haveHelo=false;
                    readData = false;
                    haveMail = false;
                    haveRcpt = false;
                    //delete temp file:
                    unlink(template);
                    //free up memory
                    free(clientName);
                    free(sender);
                    break;
                case 0:
                    //Unsupported command
                    send_string(fd, "502 Unsupported command\n");
                    break;
                case -1:
                    //Invalid command
                    send_string(fd, "500 Invalid command\n");
                    break;
                default:
                    //Should never reach this line
                    printf("What command is this?\n");
            }
        }
        //Free buffer
        free(aLineFromClient);
    }
}

//Determine whether this line is a single line of .CRLF
bool isLastDataLine(char *in, int lineSize){
    if(lineSize<=3&&in[0]=='.'&&in[1]==13&&in[2]==10){
        return true;
    }
    return false;
}

//Reset temporary file name to tmp_XXXXXX
void resetTempName() {
    for (int i = 4; i <= 9; i++)
        template[i] = 'X';
}

//check string for confusing characters, @PIAZZA Post 326
bool goodStr(char *in) {
    char badChars[] = {60, 32, 62, 34, 40, 41, 58, 92, 39};// < >"():\' ASCII codes
    while (*in) {
        for (int i = 0; i < (sizeof(badChars) / sizeof(char)); i++) {
            if (*in == badChars[i]) {
                return false;
            }
        }
        in++;
    }
    return true;
}

//print a string with its indices
void printStringInx(char *in) {
    int counter = 0;
    while (*in) {
        printf("%d: %c\n", counter, *in);
        in++;
        counter++;
    }
}

//copy char array until 0
void charcpy(char *out, char *in) {
    while (*in) {
        *out = *in;
        out++;
        in++;
    }
    *out = *in;
}

//Parse text out of a line with command
int parseLine(char *out, char *in, int lineSize) {
    strncpy(out, in + 5, lineSize - 7);
    out[lineSize - 7] = 0;
    if (!goodStr(out)) {
        return -1;
    }
    return true;
}

//find the index of ending barrack
int findEndingBrace(char *in, int lineSize) {
    for (int i = lineSize - 1; i >= 0; i--) {
        if (in[i] == '>') {
            return i;
        }
        if (in[i] == '<') {
            return -1;
        }
    }
    return -1;
}

//Parse mail out of a line with command
//no accept without <>
int parseMail(char *out, char *in, int lineSize) {
    char from[] = "FROM";
    if (!cmdMatchOffset(from, in, 4, 5)) {
        return false;
    }
    int startOffset = 11;
    //printStringInx(in);

    int endBraceIndex = findEndingBrace(in, lineSize);
    if (endBraceIndex == -1) return false;
    int endOffset = lineSize - endBraceIndex;
    int copyLength = lineSize - startOffset - endOffset;
    if (in[10] == '<') {
        strncpy(out, in + startOffset, copyLength);
        out[copyLength] = 0;
        if (!goodStr(out)) {
            return -1;
        }
    } else {
        return false;
    }
    return true;
}

//Parse Rcpt out of a line with command
//no accept without <>
int parseRcpt(char *out, char *in, int lineSize) {
    char to[] = "TO";
    if (!cmdMatchOffset(to, in, 2, 5)) {
        return false;
    }
    int startOffset = 9;
    //printStringInx(in);

    int endBraceIndex = findEndingBrace(in, lineSize);
    if (endBraceIndex == -1) return false;
    int endOffset = lineSize - endBraceIndex;
    int copyLength = lineSize - startOffset - endOffset;
    if (in[8] == '<') {
        strncpy(out, in + startOffset, copyLength);
        out[copyLength] = 0;
        if (!goodStr(out)) {
            return -1;
        }
    } else {
        return false;
    }
    return true;
}

//save a reecipient to a linked list of recipients
void addToRcptList(char *aRecipient, int size) {
    struct recipient *newRctp = malloc(sizeof(struct recipient));
    newRctp->next = NULL;
    newRctp->recipient = malloc(size);
    charcpy(newRctp->recipient, aRecipient);

    struct recipient *head = recipientList;
    if (head == NULL) {
        recipientList = newRctp;
    } else {
        while (head->next != NULL) {
            head = head->next;
        }
        head->next = newRctp;
    }
}

//destory the recipient linked list
void killRcptList() {
    while (recipientList) {
        struct recipient *next = recipientList->next;
        free(recipientList->recipient);
        free(recipientList);
        recipientList = next;
    }
}

//print all recipients
void printAllRcpt() {
    struct recipient *head = recipientList;

    while (head != NULL) {
        printf("Recipient: %s\n", head->recipient);
        head = head->next;
    }
}

//case insensetively compare chars
bool charMatch(char a, char b) {
    return tolower(a) == tolower(b);
}

//compare command between static string and result from readline
bool cmdMatch(char *CMD, char *line) {
    for (int i = 0; i < 4; i++) {
        if (!charMatch(CMD[i], line[i]))
            return false;
    }
    return true;
}

//compare command with readline with size offset
bool cmdMatchOffset(char *CMD, char *line, int size, int offset) {
    for (int i = 0; i < size; i++) {
        if (!charMatch(CMD[i], line[i + offset]))
            return false;
    }
    return true;
}

//Find what command we received
int cmdHandler(char *Line) {
    //support commands
    char helo[] = "HELO";
    char mail[] = "MAIL";
    char rcpt[] = "RCPT";
    char data[] = "DATA";
    char noop[] = "NOOP";
    char quit[] = "QUIT";

    if (cmdMatch(helo, Line)) {
        return HELO;
    } else if (cmdMatch(mail, Line)) {
        return MAIL;
    } else if (cmdMatch(rcpt, Line)) {
        return RCPT;
    } else if (cmdMatch(data, Line)) {
        return DATA;
    } else if (cmdMatch(noop, Line)) {
        return NOOP;
    } else if (cmdMatch(quit, Line)) {
        return QUIT;
    }

    //unsupported commands
    char ehlo[] = "EHLO";
    char rset[] = "RSET";
    char vrfy[] = "VRFY";
    char expn[] = "EXPN";
    char help[] = "HELP";

    if (cmdMatch(ehlo, Line) || cmdMatch(rset, Line) || cmdMatch(vrfy, Line) ||
        cmdMatch(expn, Line) || cmdMatch(help, Line)) {
        return 0;
    }

    //invalid commands
    return -1;
}
