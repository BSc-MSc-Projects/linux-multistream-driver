/* Header file that contains the definition of the struct 
 * used by the driver
 * 
 */

#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/semaphore.h>
#include <linux/wait.h>


enum ctl_ops{SET_PRIO=1, SET_BLOCKING=3, SET_OPENCLOSE=4};  // used by ioctl to determine which command was called 
enum wait_ops{WAIT_MUTEX, WAIT_WRITE, WAIT_READ};           // used to determine the type of wait event in the wait queue function

#define NR_FLOWS 2

/* The data information for the object, 
 * used to keep track of the situation in terms of bytes.
 * This is kept as a linked list, and each stream_content has by deafult the dimension of a memory page (4KB)
 * */
typedef struct _object_content{
    int record_length;
    int read_offset;
    char *stream_content;
    struct _object_content *next;
} object_content;


/* Struct used to handle control information for a given session
 * This is copied in the private_data field of the struct file
 * */
typedef struct _io_sess_info{
    int priority;
    long timeout;
} io_sess_info;


typedef struct _queue_elem{
    struct task_struct *the_task;
    int already_hit;
    struct _queue_elem *next; 
    struct _queue_elem *prev;
} queue_elem;


/* Keeps the global state of an I/O object 
 * In this case, two flows are handled, high and low, so the 
 * arrays are indexed in this way:
 *  - 0: low fields
 *  - 1: high fields 
 *  */
typedef struct _object_state{
#ifdef SINGLE_SESSION_OBJECT
        struct mutex object_busy;
#endif
        struct mutex operation_synchronizer[NR_FLOWS];
        int valid_bytes[NR_FLOWS];
        int total_free_bytes[NR_FLOWS];    // the number of free bytes, can depend also on bytes reserved in the low priority flow
        object_content* list_heads[NR_FLOWS];
        wait_queue_head_t the_wq_head[NR_FLOWS];
} object_state;


/* Struct used by the tasklet */
typedef struct _packed_write_data_wq{
    void *buffer;
    char *data;
    int minor;  // the minor number identifing the device
    size_t len; // len of the data buffer
    struct work_struct the_work;  // work queue struct
} packed_data_wq;  


/* Redefinition of the struct used in the user.c program, usefull
 * to deal with the settings of the device driver or the single 
 * device file
 * */
typedef struct _dev_info{
    int command;    // command to control the device, that will be one of the enum ctl_ops
    unsigned long parameter;
} dev_info;
