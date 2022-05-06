/* Creates a given number of nodes, that can user application can
 * use to do operations with the driver
 * */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

int i;
char buff[4096];

int main(int argc, char **argv){
    int ret;
    int major;
    int minors;
    char *path;
    
    if(argc<4){
        printf("usage:\n1)prog\n2)pathname where the nodes will be created(including the node basename)\n3)major number of the device driver\n4)minors numbers to associate to the different nodes\n");
        return -1;
    }
    
    path = argv[1];
    major = strtol(argv[2],NULL,10);
    minors = strtol(argv[3],NULL,10);
    
    // sanity check on the major and minors passed
    if (errno == ERANGE || major < 0 || minors < 0){
        printf("Major or minors are not valid");
        return -1;
    }

    printf("creating %d minors for device %s with major %d\n",minors,path,major);
   
    for(i=0;i<minors;i++){
        sprintf(buff,"mknod %s%d c %d %i\n\n",path,i,major,i);
        system(buff);
        sprintf(buff,"%s%d",path,i);
        printf("node %s%d created\n", path,i);
    }

    return 0;              
}
