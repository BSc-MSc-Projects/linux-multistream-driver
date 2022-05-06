/* Structs used in the user.c */


enum ctl_ops{SET_PRIO=1, SET_BLOCKING=3, SET_OPENCLOSE=4};


typedef struct _dev_info{
    int command;    // command to control the device
    unsigned long parameter;
} dev_info;

