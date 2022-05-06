#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>

#include "headers/structs.h"

#define MAX_SIZE 4096   // max buffer size
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"


int i;
char dev_data[MAX_SIZE];
char *choices[] = {
                "|1 |  Write on the device file",
                "|2 |  Read from the device file",
                "|3 |  Switch to high priority flow",
                "|4 |  Switch to low priority flow",
                "|5 |  Make operations blocking", 
                "|6 |  Make operations non blocking", 
                "|7 |  Enable a device file",
                "|8 |  Disable a device file",
                "|9 |  Exit",
};
int n_choices = sizeof(choices) / sizeof(char *); 


// Function prototypes
int min(int a, int b);
void print_menu();
void clean_stdin_line();


/* Function that will ask to the user which operation to perform */
void do_work(char* path){
    int fd;
    int result;
    memset(dev_data, '\0', MAX_SIZE-1);

	sleep(1);

	printf("opening device %s\n", path);
	fd = open(path,O_RDWR);
	if(fd == -1) {
		printf("open error on device %s\n",path);
		exit(-1);
	}
	printf("device %s successfully opened, fd is: %d\n",path, fd);
    
    long choice = 0;
    unsigned int command = 0;
    unsigned long info = 0; 

    while(1){
        memset(dev_data, 0x0, strlen(dev_data));
        print_menu(); 
        
        fgets(dev_data, 5, stdin);
        clean_stdin_line();

        choice = strtol(dev_data, NULL, 10);
        if (errno == ERANGE || errno == EINVAL){
            printf(ANSI_COLOR_RED "Read: the amount of data inserted is not valid \n" ANSI_COLOR_RESET);
            pthread_exit(NULL);
        }
        memset(dev_data, 0x0, strlen(dev_data));
		switch(choice){	
            case 1:
                printf("Insert the data you want to write (max 4096): ");
                fgets(dev_data, MAX_SIZE, stdin);
                clean_stdin_line();               

                result = write(fd,dev_data, min(strlen(dev_data), MAX_SIZE));
                if (result == 0 || result == -1)
                    printf(ANSI_COLOR_RED "\nWrite result: could not write in the buffer \n" ANSI_COLOR_RESET);
                else{
                    printf(ANSI_COLOR_GREEN "\nWrite result (%d bytes): operation completed successfully\n", result); 
                    printf(ANSI_COLOR_RESET);
                }
                memset(dev_data, 0x0, strlen(dev_data));
                break; 
            case 2:
                printf("Insert the amount of data you want to read (max 4096): ");
                fgets(dev_data, 5, stdin);
                clean_stdin_line();

                long data_len;
                data_len = strtol(dev_data, NULL, 10);
                if (errno == ERANGE || errno == EINVAL)
                    printf(ANSI_COLOR_RED "\nRead: the amount of data inserted is not valid \n" ANSI_COLOR_RESET);
                else{
                    memset(dev_data, 0x0, strlen(dev_data));
                    result = read(fd, dev_data, min(data_len, MAX_SIZE));
                    if (result == 0 || result == -1)
                        printf(ANSI_COLOR_RED "\nRead result: no data was read from the device file \n" ANSI_COLOR_RESET);
                    else{
                        printf(ANSI_COLOR_GREEN "\nRead result (%d bytes): ", result); 
                        printf(ANSI_COLOR_RESET);
                        printf("%s\n\n", dev_data);
                        memset(dev_data, 0x0, strlen(dev_data));
                    }
                }
                memset(dev_data, 0x0, strlen(dev_data));
                break;
            case 3:
                command = SET_PRIO;
                info = 1;
                break;
            case 4: 
                command = SET_PRIO;
                info = 0; 
                break;
            case 5: 
                command = SET_BLOCKING; 
                printf("Insert the timeout value, in jiffies (1 jiffie = 10000 microseconds): ");
                fgets(dev_data, MAX_SIZE, stdin);  
                clean_stdin_line();

                long timeout;
                timeout = strtol(dev_data, NULL, 10);
                if (errno == ERANGE || errno == EINVAL)
                    printf(ANSI_COLOR_RED "\nIoctl: the timeout is not valid \n" ANSI_COLOR_RESET);
                else if (timeout <= 0)
                    printf(ANSI_COLOR_RED "\nIoctl: timeout value <= 0\n" ANSI_COLOR_RESET);
                else{
                    info = timeout;
                }
                memset(dev_data, 0x0, strlen(dev_data));
                break; 
            case 6: 
                command = SET_BLOCKING;
                info = 0; 
                break;
            case 7:
                command = SET_OPENCLOSE;
                info = 0; 
                break; 
            case 8:
                command = SET_OPENCLOSE;
                info = 1; 
                break;
            case 9: 
                pthread_exit(NULL);
                close(fd);
                break;
            default:
                printf(ANSI_COLOR_RED "\nThe option is not valid \n" ANSI_COLOR_RESET);
                break;
		}
        if(3 <= choice && choice <= 8){
            result = ioctl(fd, command, info);
            if (result == -1)
                printf(ANSI_COLOR_RED "\n ioctl failed \n" ANSI_COLOR_RESET);
            else
                printf(ANSI_COLOR_GREEN "\n ioctl successfully completed \n"); 
        }
	}
    pthread_exit(NULL);
}


void clean_stdin_line(){
    char c;
    c = dev_data[strlen(dev_data)-1];
    while (c != '\n' && c != EOF)
        c = getc(stdin);
}


// Little min implementation
int min(int a, int b){
    if (a < b)
        return a;
    return b;
}


/* Print application menu, highlighting the currently selected option */
void print_menu(int highlight){
    for(i = 0; i < n_choices; ++i) {
        printf(ANSI_COLOR_MAGENTA);
		printf("%s\n", choices[i]);
        printf(ANSI_COLOR_RESET);
	}
    printf("\n\nInsert your option:");
}


int main(int argc, char** argv){
    if(argc<2){
    	printf("Usage:\n\n1)prog\n2)path name of the node\n");
	    exit(-1);
    }
    
    printf("\t\t\t\t\t| Multistream device driver |\n\n");
    do_work(argv[1]);
    return 0;
}
