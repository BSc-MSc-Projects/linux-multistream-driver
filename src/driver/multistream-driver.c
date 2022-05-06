/** Char device driver, implementing the classical operation interface.
 *  In particular, this driver can handle a ceratin number of minor numbers (given by the MINOR define), corresponding to different 
 *  device files.
 *  The driver works, for each minor number, with 2 different data flows:
 *   - high priority
 *   - low priority, here write operations are served asynchronously using the work queues
 *  
 *  With the ioctl syscall, it is possible to manage the state of each I/O session through the driver, in terms of
 *   - changing the current priority level (0: low, 1: high)
 *   - changing the type of operations (0: non blocking, 1: blocking)
 *   - setting up a timeout that regulates the awaken of blocking operations
 *
 *  Blocking operations will lead the current thread to sleep on a wait queue, there are 2 wait queues defined for each minor number:
 *   - one used to keep threads that are waiting to acquire the lock
 *   - one keeps thread that are waiting for read/write operations
 */


#include <asm-generic/ioctl.h>
#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>	
#include <linux/pid.h>		/* For pid types */
#include <linux/tty.h>		/* For the tty declarations */
#include <linux/version.h>	/* For LINUX_VERSION_CODE */
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/fdtable.h>
#include <asm/current.h> 
#include <linux/wait.h> 
#include <linux/string.h>

#include "structs/structs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Francesco Quaglia");
MODULE_AUTHOR("Pierciro Caliandro <piercirocaliandro998@gmail.com>");

#define MODNAME "MULTISTREAM CHAR DEV"
#define DEVICE_NAME "multistream-dev"  /* Device file name in /dev/ - not mandatory  */


/* Driver function prototypes */
static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off);
static long dev_ioctl(struct file *filp, unsigned int command, unsigned long param);


/* Helper function prototypes */
void do_wq_write(unsigned long data);
int do_sleep_wqe(long op_timeout, int minor, int priority, int value, int event);
static ssize_t write_data(size_t len, int minor, char* buffer, int priority); 
int try_get_lock(io_sess_info* sess_info, int minor, const char* operation);
int try_wait_for_data(io_sess_info* sess_info, int minor, int value, int event);

/* Defines for the device driver */
//#define SINGLE_INSTANCE               // just one session at a time across all I/O node 
//#define SINGLE_SESSION_OBJECT         // just one session per I/O node at a time
//#define DEBUG_INFO                    // used to log device messages upon operations 
#define DEV_INFO                        // used for init and cleanup

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session)	MAJOR(session->f_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_inode->i_rdev)
#else
#define get_major(session)	MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_dentry->d_inode->i_rdev)
#endif

#ifdef SINGLE_INSTANCE
static DEFINE_MUTEX(device_state);
#endif


#define MINORS 128  // the numbers of minors that the driver can handle are 128
object_state objects[MINORS]; // as many structs as the number of minors that can be managed

static int Major;            /* Major number assigned to broadcast device driver */

/* Defines the total size for each of the two flows corresponding to each device file. The maximum size if therefor obtained as 
 * OBJECT_MAX_SIZE*MAX_PAGES, so that it can be changed if needed before compiling the module
 * */
#define OBJECT_MAX_SIZE  (4096) //just one page for the amount of data that each flow can handle for each of the minors
#define MAX_PAGES 5     // number of pages for each device file


/* Redefinition of these macros to allow threads to sleep in WQ_EXCLUSIVE mode */ 
#define __wait_event_interruptible_timeout_exclusive(wq_head, condition, timeout)		\
	___wait_event(wq_head, ___wait_cond_timeout(condition),			\
		      TASK_INTERRUPTIBLE, 1, timeout,				\
		      __ret = schedule_timeout(__ret))


#define wait_event_interruptible_timeout_exclusive(wq_head, condition, timeout)		\
({										\
	long __ret = timeout;							\
	might_sleep();								\
	if (!___wait_cond_timeout(condition))					\
		__ret = __wait_event_interruptible_timeout_exclusive(wq_head,		\
						condition, timeout);		\
	__ret;									\
})



/* Definition of the module parameters */

unsigned long enable_disable_array[MINORS];
module_param_array(enable_disable_array, ulong, NULL, 0660);

unsigned long high_data_count[MINORS];
module_param_array(high_data_count, ulong, NULL, 0440);

unsigned long low_data_count[MINORS];
module_param_array(low_data_count, ulong, NULL, 0440);

unsigned long high_wait_data[MINORS];
module_param_array(high_wait_data, ulong, NULL, 0440);

unsigned long low_wait_data[MINORS];
module_param_array(low_wait_data, ulong, NULL, 0440);


/* The actual driver */


static int dev_open(struct inode *inode, struct file *file) {
        int minor; 
        io_sess_info *sess_info;
        minor = get_minor(file);

        if(minor >= MINORS){
	        return -ENODEV;
        }
        if(enable_disable_array[minor]){
#ifdef DEBUG_INFO
            printk("%s: device with minor %d cannot be opened because it is disabled\n", MODNAME, minor);
#endif
            return -1;
        }
#ifdef SINGLE_INSTANCE
        // this device file is single instance
        if (!mutex_trylock(&device_state)) {
		    return -EBUSY;
        }
#endif

#ifdef SINGLE_SESSION_OBJECT
        if (!mutex_trylock(&(objects[minor].object_busy))) {
		    goto open_failure;
        }
#endif

#ifdef DEBUG_INFO
        printk("%s: device file successfully opened for object with minor %d\n",MODNAME,minor);
#endif
        sess_info = (io_sess_info* )kzalloc(sizeof(io_sess_info), GFP_ATOMIC);
        if(sess_info != NULL){
            sess_info->priority = 1;
            sess_info->timeout = 0;
            file->private_data = sess_info;
        
            //device opened by a default nop
            return 0;
        }
        else
            return -1;

#ifdef SINGLE_SESSION_OBJECT
open_failure:
#ifdef SINGE_INSTANCE
    mutex_unlock(&device_state);
#endif
    return -EBUSY;
#endif
}


static int dev_release(struct inode *inode, struct file *file) {
        int minor;
        minor = get_minor(file);
        
#ifdef SINGLE_SESSION_OBJECT
        mutex_unlock(&(objects[minor].object_busy));
#endif


#ifdef SINGLE_INSTANCE
        mutex_unlock(&device_state);
#endif

#ifdef DEBUG_INFO
        printk("%s: device file closed\n",MODNAME);
        kfree(file->private_data);
#endif
        return 0;
}


static ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *off) {
        object_state *the_object;
        io_sess_info *sess_info;
        int ret;
        int minor = get_minor(filp);
        int tot_written;
        char* temp_buffer;
        
        the_object = objects + minor;
        sess_info = (io_sess_info *)(filp->private_data); 
        
#ifdef DEBUG_INFO
        printk("%s: somebody called a write on dev with [major,minor] number [%d,%d] to write %ld\n",MODNAME,get_major(filp),get_minor(filp), len);
#endif

        /* Before copying the bytes in the stream, do a local copy. In such way, if the copy_from_user results in a PAGE FAULT, the 
         * stream is not blocked since only thsi thread will sleep
         * */
        temp_buffer = (char*)kmalloc(len*sizeof(char), GFP_ATOMIC);   // the size is unknown, so a fine grained allocator (slub) is used
        if(temp_buffer == NULL)
            goto no_mem;
        ret = copy_from_user(temp_buffer, buff, len);    // copy in an intermediate kernel buffer
        len = (len - ret);

        
        if(try_get_lock(sess_info, minor, "write") != 1){
            kfree((void*)temp_buffer);
            goto no_lock;
        }
        
        /* There is no space on the device, so try to wait for a given timeout */
        if(the_object->total_free_bytes[sess_info->priority] == 0 && sess_info->timeout > 0){
            mutex_unlock(&(the_object->operation_synchronizer[sess_info->priority]));
#ifdef DEBUG_INFO
            printk("%s: write going to wait for lack of data\n", MODNAME);
#endif
            if(try_wait_for_data(sess_info, minor, 0, WAIT_WRITE) != 1){
                kfree((void*)temp_buffer);
                return -ENOSPC;
            } 
            // Try to get the lock again, if it fails it will exit
            if(try_get_lock(sess_info, minor, "write") != 1){
                kfree((void*)temp_buffer);
                goto no_lock;
            }
        }

        
        /* Got the lock, so from now on there is the write operation */

        // Check again if the acutal copy can be performed
        if(the_object->total_free_bytes[sess_info->priority] == 0){
            mutex_unlock(&(the_object->operation_synchronizer[sess_info->priority])); 
            wake_up_interruptible(&(the_object->the_wq_head[sess_info->priority]));
            kfree((void*)temp_buffer);
#ifdef DEBUG_INFO
            printk("%s: device file is full \n", MODNAME);
#endif
            
            return -ENOSPC;
        }

        if(len > the_object->total_free_bytes[sess_info->priority])
            len = the_object->total_free_bytes[sess_info->priority];

        /* Low priority flow, the write work will be scheduled */       
        if (!sess_info->priority){  
            packed_data_wq *the_wq;
            
            if (!try_module_get(THIS_MODULE)){
                mutex_unlock(&(the_object->operation_synchronizer[0]));
                
                wake_up_interruptible(&(the_object->the_wq_head[0]));    // wakes up one thread in the wait_queue of threads that are waiting for the lock
                kfree((void*)temp_buffer);
                return -ENODEV;
            }
#ifdef DEBUG_INFO
            printk("%s: Registrering deferred write with work queues\n", MODNAME);
#endif
            the_wq = kzalloc(sizeof(packed_data_wq), GFP_ATOMIC);   // allocate new memory for the work queue data
            if (the_wq == NULL){
#ifdef DEBUG_INFO
                printk("%s: Workqueue allocation failed\n", MODNAME);
#endif
                module_put(THIS_MODULE);
                
                mutex_unlock(&(the_object->operation_synchronizer[0])); 
                
                wake_up_interruptible(&(the_object->the_wq_head[0]));    // wakes up one thread in the wait_queue of threads that are waiting for the lock
                kfree((void*)temp_buffer);
                return -1;
            }
            
            the_wq->buffer = the_wq;    // needed to use the macro container_of
            
            // initialize the needed parameters
            the_wq->minor = minor;
            the_wq->data = kmalloc(sizeof(char)*len, GFP_ATOMIC);
            if(the_wq->data == NULL){
#ifdef DEBUG_INFO
                printk("%s: work queue buffer cannot be allocated \n", MODNAME);
#endif
                kfree((void*)the_wq);
                kfree((void*)temp_buffer);
                
                mutex_unlock(&(the_object->operation_synchronizer[0])); 
                wake_up_interruptible(&(the_object->the_wq_head[0]));
                return -ENOMEM;
            }
            the_wq->data = temp_buffer;
            the_wq->len = len; 
            the_object->total_free_bytes[0] -= len;   // decrement the total free bytes, work queue will never fail
            mutex_unlock(&(the_object->operation_synchronizer[0]));
            
            __INIT_WORK(&(the_wq->the_work), (void *)do_wq_write, (unsigned long)(&(the_wq->the_work))); 
            schedule_work_on(0, &the_wq->the_work);  
            wake_up_interruptible(&(the_object->the_wq_head[0]));
            
#ifdef DEBUG_INFO
            printk("%s: Work queue successfully scheduled\n", MODNAME);
#endif
            return len;
        }

        /* High priority flow, the write is synchronously */
         
        tot_written = write_data(len, minor, temp_buffer, sess_info->priority);
        the_object->valid_bytes[1] += tot_written;
        the_object->total_free_bytes[1] -= tot_written;
        high_data_count[minor] += tot_written;
             
#ifdef DEBUG_INFO
        printk("%s: Valid bytes are now: %d\n", MODNAME, the_object->valid_bytes[1]);
#endif
        mutex_unlock(&(the_object->operation_synchronizer[1]));
        
        wake_up_interruptible(&(the_object->the_wq_head[1]));    // wakes up one thread in the wait_queue of threads that are waiting for the lock
        kfree((void*)temp_buffer);
        return tot_written;


no_lock:
#ifdef DEBUG_INFO
        printk("%s: write, cannot take the lock \n", MODNAME);
#endif
        return -1;

no_mem: 
#ifdef DEBUG_INFO
        printk("%s: write, temporary buffer allocation failed \n", MODNAME);
#endif
        return -ENOMEM;
}


static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off) {
        int minor = get_minor(filp);
        int ret;
        io_sess_info *sess_info;
        object_state *the_object; 
        object_content *obj_index;
        int read_offset;
        int len_to_read;
        int record_to_rem;
        int total_len;
        char* temp_buffer;
        
        /* Preliminary check: verify that the len requested by the user actually
         * fits the buffer limits. 
         * */
        ret = clear_user(buff, len);
        len = len -ret;

        the_object = objects + minor;
        sess_info = (io_sess_info *)(filp->private_data); 
        
        /* As for the write, the amount of bytes that are actually read are limited by the available */
        if(try_get_lock(sess_info, minor, "read") != 1)
           goto read_no_lock;
        
        // If there are no byte and the operation can wait, do it
        if(the_object->valid_bytes[sess_info->priority] == 0 && sess_info->timeout > 0){
            mutex_unlock(&(the_object->operation_synchronizer[sess_info->priority]));
            if(try_wait_for_data(sess_info, minor, 0, WAIT_READ) != 1)
                return 0;

            if(try_get_lock(sess_info, minor, "read") != 1)
                goto read_no_lock;
        } 

        /* Got the lock, so from now on there is the actual read operation */
        
        if(the_object->valid_bytes[sess_info->priority] == 0){
            mutex_unlock(&(the_object->operation_synchronizer[sess_info->priority]));
            wake_up_interruptible(&(the_object->the_wq_head[sess_info->priority]));
#ifdef DEBUG_INFO
            printk("%s: device file is empty \n", MODNAME);
#endif
            return 0;
        }
        if (len > the_object->valid_bytes[sess_info->priority])
            len = the_object->valid_bytes[sess_info->priority];
        
        temp_buffer = (char*)kzalloc(len*sizeof(char), GFP_ATOMIC);
        if(temp_buffer == NULL){
            mutex_unlock(&(the_object->operation_synchronizer[sess_info->priority])); 
            wake_up_interruptible(&(the_object->the_wq_head[sess_info->priority]));
            goto read_no_mem;
        }

#ifdef DEBUG_INFO
        printk("%s: somebody called a read on dev with [major,minor] number [%d,%d], with offset %lld\n",MODNAME,get_major(filp),get_minor(filp), *off);
#endif
        
        obj_index = the_object->list_heads[sess_info->priority]->next;
        read_offset = 0;
        len_to_read = 0;
        record_to_rem = 0;
        total_len = 0;

        while(len > 0){
            len_to_read = len;
            if(len >= (obj_index->record_length - obj_index->read_offset))
                len_to_read = obj_index->record_length - obj_index->read_offset;
            memcpy(&(temp_buffer[total_len]), &(obj_index->stream_content[obj_index->read_offset]), len_to_read);

            obj_index->read_offset += len_to_read;
            if (obj_index->read_offset == OBJECT_MAX_SIZE){
                record_to_rem++;
                obj_index = obj_index->next;
            }
            len -= len_to_read;
            total_len += len_to_read;
            read_offset += len_to_read;
        }
        if (record_to_rem > 0){
            obj_index = the_object->list_heads[sess_info->priority]->next; 
            for(; record_to_rem > 0; record_to_rem--){
                object_content* temp = obj_index;
                obj_index = obj_index->next;
                free_page((unsigned long)temp->stream_content);
                kfree((void*)temp);
#ifdef DEBUG_INFO
                printk("%s: removed one node\n", MODNAME);
#endif
            }
            the_object->list_heads[sess_info->priority]->next = obj_index;
        }
        
        // delete read data and update the number of valid bytes
        the_object->valid_bytes[sess_info->priority] -= total_len;
        the_object->total_free_bytes[sess_info->priority] += total_len;

        if(sess_info->priority){
            high_data_count[minor] -= total_len;
        }
        else{
            low_data_count[minor] -= total_len;
        }
        
        mutex_unlock(&(the_object->operation_synchronizer[sess_info->priority])); 
        wake_up_interruptible(&(the_object->the_wq_head[sess_info->priority]));    // wakes up one thread in the wait_queue of threads that are waiting for the lock

#ifdef DEBUG_INFO
        printk("%s: Read operation completed, returning %d\n", MODNAME, total_len);
#endif
        ret = copy_to_user(buff, temp_buffer, total_len);
        kfree((void*)temp_buffer);
        return total_len-ret;


read_no_lock:
#ifdef DEBUG_INFO
        printk("%s read operation could not aquire the lock\n", MODNAME);
#endif
        return -1;

read_no_mem:
#ifdef DEBUG_INFO
        printk("%s: read, memory allocation failed \n", MODNAME);
#endif
        return -ENOMEM;

}


/* Ioctl here is used to manage session information about the I/O session, in particular reguarding:
 *  - make operations blocking / non blocking 
 *  - change the priotity data flow
 *  - setup of the timeout that regulates the awaken of blocking operations 
 * @param filp: pointer to a file struct
 * @param command: ioctl command 
 * @param: the address of a _io_sess_info struct (defined in structs/struct.h)
 *
 * Returns: 0 in case of sucess, -1 in case of failure
 *  */
static long dev_ioctl(struct file *filp, unsigned int command, unsigned long param) {
        int minor = get_minor(filp);
        object_state *the_object;
        io_sess_info *sess_info;
        int prev_prio;  //used in case that the op changes the priority

        the_object = objects + minor;
        sess_info = (io_sess_info *)(filp->private_data);
        
        if(try_get_lock(sess_info, minor, "ioctl") != 1){
#ifdef DEBUG_INFO
            printk("%s: ioctl could not get the lock\n", MODNAME);
#endif
            return -ENODEV;
        }
        
        prev_prio = sess_info->priority;    // used to unlock the correct mutex in case that ioctl changes priority
        
#ifdef DEBUG_INFO
        printk("%s: somebody called an ioctl on dev with [major,minor] number [%d,%d] and device command %d \n",MODNAME,get_major(filp),get_minor(filp), command);
#endif  

        /* Here the program is not aware of the value passed, because it is belevied that the 
         * sanity checks were made in the user.c program 
         * 
         * Furthemore, the switch is actually performed on the user code, not the default ones for the ioctl
         * */
        switch (command){
            case SET_PRIO:
#ifdef DEBUG_INFO
                printk("%s: ioctl command called was SET_PRIO, with param: %ld\n", MODNAME, param);
#endif
                sess_info->priority = param;
                break;
            case SET_BLOCKING: 
#ifdef DEBUG_INFO
                printk("%s: ioctl command called was SET_BLOCKING, with param: %ld\n", MODNAME, param);
#endif
                sess_info->timeout = param;
                break;
            case SET_OPENCLOSE:  
#ifdef DEBUG_INFO
                printk("%s: ioctl command called was SET_OPENCLOSE, with param: %ld\n", MODNAME, param);
#endif
                enable_disable_array[minor] = param; // enables or disables the device file
                break;
            default: 
#ifdef DEBUG_INFO
                printk("%s: Ioctl called, but the given user command [%d], is not supported by this driver\n", MODNAME, command);
#endif
                mutex_unlock(&(the_object->operation_synchronizer[prev_prio])); 
                wake_up_interruptible(&(the_object->the_wq_head[prev_prio]));
                return -1;
        }
        mutex_unlock(&(the_object->operation_synchronizer[prev_prio]));
        wake_up_interruptible(&(the_object->the_wq_head[prev_prio]));
        return 0;
}


/* Auxiliary functions */


/* Work queue function 
 * @data: address of the parameter passed in the __INIT_WORK 
 * */
void do_wq_write(unsigned long data){
        char *buffer;
        object_state *the_object;
        size_t tot_bytes;
        int minor;

        buffer = (char *)(container_of((void*)data, packed_data_wq, the_work)->data);
        minor = (int)(container_of((void*)data, packed_data_wq, the_work)->minor);
        the_object = objects + minor;   // get the right object
        tot_bytes = (size_t)(container_of((void*)data, packed_data_wq, the_work)->len);    // get the number of bytes to copy

        mutex_lock(&(the_object->operation_synchronizer[0]));

#ifdef DEBUG_INFO
        printk("%s: Work queue called to write on the buffer\n", MODNAME);
#endif

        write_data(tot_bytes, minor, buffer, 0);
        the_object->valid_bytes[0] += tot_bytes;
#ifdef AUTID 
        printk("%s: Work queue terminated \n", MODNAME);
#endif
        low_data_count[minor] += tot_bytes;
        
        mutex_unlock(&(the_object->operation_synchronizer[0])); 
        wake_up_interruptible(&(the_object->the_wq_head[0]));    // wakes up one thread in the wait_queue of threads that are waiting for the lock
        
        kfree((void*)container_of((void*)data, packed_data_wq, the_work)->data);
        kfree((void *)container_of((void*)data, packed_data_wq, the_work));
        module_put(THIS_MODULE);
}


/** do_sleep_wqe - Put the current thread to sleep while it is waiting to get the lock. The wait is on a wait event queue
 * @timeout: the timeout (in microseconds) after which the thread will wake up
 * @minor: the minor number of the device file
 * @priority: priority level of the dta flow
 * @type: refers to the wakeup condition
 * @value: value for the condition
 *
 * Returns: 
 * * 0 if the lock hasn't been acquired, 
 * * 1 otherwhise.
 * 
 * */
int do_sleep_wqe(long op_timeout, int minor, int priority, int value, int event){
        object_state *the_object;
        int res; 

        the_object = objects + minor;
        res = 0; 

#ifdef DEBUG_INFO
        printk("%s: the current thread (%d) is going to sleep for %lu jiffies\n", MODNAME, current->pid, op_timeout);
#endif
        
        switch (event){
            case WAIT_MUTEX:
                res = wait_event_interruptible_timeout_exclusive(the_object->the_wq_head[priority], mutex_trylock(&(the_object->operation_synchronizer[priority])) == value, op_timeout);
                break;

            case WAIT_WRITE: 
                res = wait_event_interruptible_timeout_exclusive(the_object->the_wq_head[priority], (the_object->total_free_bytes[priority]) > value, op_timeout);
                break; 
            
            case WAIT_READ: 
                res = wait_event_interruptible_timeout_exclusive(the_object->the_wq_head[priority], (the_object->valid_bytes[priority]) > value, op_timeout);
                break;
        }
        

        /* The thread exited from the wait queue. This could happen either because the condition was or was not satisfied, or
         * due to a signal was received 
         * */
        if(res == -ERESTARTSYS){
#ifdef DEBUG_INFO
            printk("%s: thread %d was interrupted by a signal \n", MODNAME, current->pid);
#endif
            return -EINTR;
        }
        if (res){
#ifdef DEBUG_INFO
            printk("%s: thread %d exiting successfully from sleep \n", MODNAME, current->pid);
#endif
            return 1;
        }
#ifdef DEBUG_INFO
            printk("%s: thread %d could not compelte successfully \n", MODNAME, current->pid);
#endif
        return 0;
}


/** write_data - Perform the actual write of data on the file, depending on the priority flow, since the write operation is quite similar
 * in both cases.
 *
 * @len: length of the data to write
 * @minor: minor number of the device file
 * @buff: data buffer, either user buffer if high priority flow or kernel buffer if low priority flow
 * @priority: data flow priority
 *
 * Returns: 
 * * the number of data copied
 *
 * */
static ssize_t write_data(size_t len, int minor, char* buffer, int priority){
        int curr_length; 
        int tot_written;
        int temp_len;
        object_content* temp_object;
        object_state* the_object;

        curr_length = 0;
        tot_written = 0;
        temp_len = len;
        the_object = objects + minor;
        
        // Allocate the first page that has been deallocated previously
        if(the_object->list_heads[priority]->next == NULL){
            object_content *new_first_page = (object_content *)kzalloc(sizeof(object_content), GFP_ATOMIC);
            if(new_first_page == NULL)
                goto dev_write_no_mem;
            new_first_page->stream_content = (char* )__get_free_page(GFP_ATOMIC);
            if(new_first_page->stream_content == NULL){
                kfree((void*)new_first_page);
                goto dev_write_no_mem;
            }
            new_first_page->record_length = 0;
            new_first_page->read_offset = 0;
            new_first_page->next = NULL;
            the_object->list_heads[priority]->next = new_first_page;
        }

        temp_object = the_object->list_heads[priority]->next;
        while(temp_object->record_length == OBJECT_MAX_SIZE && temp_object->next != NULL)
            temp_object = temp_object->next;    //get the page where it is possible to write
        
        the_object = objects + minor;
        
        /* Calculate the number of pages that will be needed to store the data. In the worst case scenario there will be a surplus 
         * due to the fact that the copy_from_user is not guaranteed to move all the requested bytes, but then the memory will be 
         * used by next operations 
         *
         * */
        if(((OBJECT_MAX_SIZE - temp_object->record_length) - temp_len) < 0){
            temp_len = temp_len - (OBJECT_MAX_SIZE - temp_object->record_length);
            while (temp_len > 0){
                object_content *new_record = (object_content *)kzalloc(sizeof(object_content), GFP_ATOMIC);
                if(new_record == NULL)
                    goto dev_write_no_mem;
                new_record->stream_content = (char*)__get_free_page(GFP_ATOMIC);
                if(new_record->stream_content == NULL){
                    kfree((void*)new_record);
                    goto dev_write_no_mem;
                }
                
                new_record->record_length = 0;
                new_record->read_offset = 0;
                new_record->next = NULL;
                temp_object->next = new_record;
                temp_object = temp_object->next;
                temp_len -= OBJECT_MAX_SIZE;
            }
        }

        temp_object = the_object->list_heads[priority]->next;
        while(temp_object->record_length == OBJECT_MAX_SIZE)
            temp_object = temp_object->next;    //get the page where it is possible to write

        while(len > 0){
            curr_length = len;
            if (len > (OBJECT_MAX_SIZE - temp_object->record_length))
                curr_length = OBJECT_MAX_SIZE - temp_object->record_length;
            
            memcpy(&(temp_object->stream_content[temp_object->record_length]), &(buffer[tot_written]), curr_length);

            temp_object->record_length += curr_length;
            tot_written += curr_length;
#ifdef DEBUG_INFO
            printk("%s: actual len for the object: %d\n", MODNAME, temp_object->record_length);
            printk("%s: written %d\n", MODNAME, curr_length);
#endif
            // limit reached, needs to allocate more space
            len -= (curr_length);
            if(temp_object->record_length == OBJECT_MAX_SIZE && len > 0){
#ifdef DEBUG_INFO
                printk("%s: changing object \n", MODNAME);
#endif
                temp_object = temp_object->next;
            }

        }
        return tot_written;

dev_write_no_mem:
#ifdef DEBUG_INFO
        printk("%s: cannot allocate memory for device write", MODNAME);
#endif
        return -ENOMEM;
}


/** try_get_lock - tries to get the mutex. If it fails, and the operations are blocking
 * then it can lead to sleep on a wait event queue.
 * @sess_info: io_sess_info struct, containing session information of the calling thread
 * @minor: minor number of the device file
 *
 * Return:
 * * 1 in case of success,
 * * 0 in case of failure
 *
 * */
int try_get_lock(io_sess_info* sess_info, int minor, const char* operation){
        object_state* the_object;
        the_object = objects + minor;
        if(mutex_trylock(&(the_object->operation_synchronizer[sess_info->priority])) != 1){
            if (sess_info->timeout <= 0)   // this means that the operation is in blocking mode
                return 0;
                
#ifdef DEBUG_INFO
            printk("%s: %s is going to sleep because the lock is not available\n", MODNAME, operation);
#endif
            if(do_sleep_wqe(sess_info->timeout, minor, sess_info->priority, 1, WAIT_MUTEX) != 1)
                return 0;
            return 1;
        }
        return 1;
}


/** try_wait_for_data - put the thread in the wait queue for a read/write operation that cannot read or write 
 *  due to lack of space on the device file or lack of data.
 *  @timeout: timeout passed to wait_event_interruptible_timeout_exclusive
 *  @minor: minor number of the device file
 *  @value: value checked for the wait condition
 *  @event: sleep event, can be
 *   - WAIT_WRITE
 *   - WAIT_READ
 *
 *   Return:
 *    - 1 in case of success
 *    - 0 in case of failure
 *  */
int try_wait_for_data(io_sess_info* sess_info, int minor, int value, int event){
        int ret; 
        if (sess_info->priority)
             high_wait_data[minor] += 1;
         else
             low_wait_data[minor] += 1;

        ret = do_sleep_wqe(sess_info->timeout, minor, sess_info->priority, value, event);
        if (sess_info->priority)
             high_wait_data[minor] -= 1;
         else
             low_wait_data[minor] -= 1;

         return ret;
}



/* File ops remapping */

static struct file_operations fops = {
        .owner = THIS_MODULE,//do not forget this
        .write = dev_write,
        .read = dev_read,
        .open =  dev_open,
        .release = dev_release,
        .unlocked_ioctl = dev_ioctl
};


/* Init and cleanup module functions */

int init_module(void) {
        int i;
        int j;

	    //initialize the drive internal state
	    for(i=0;i<MINORS;i++){
#ifdef SINGLE_SESSION_OBJECT
		    mutex_init(&(objects[i].object_busy));
#endif
            /* allocate the first page for each priority flow*/
		    for(j=0;j<2;j++){
                object_content *first_page;
                
                init_waitqueue_head(&(objects[i].the_wq_head[j])); 
                objects[i].valid_bytes[j] = 0; 
                objects[i].total_free_bytes[j] = OBJECT_MAX_SIZE*MAX_PAGES;    // setup the default total size
            
                objects[i].list_heads[j] = (object_content *)kzalloc(sizeof(object_content), GFP_KERNEL);
                if(objects[i].list_heads[j] == NULL){
                    goto revert_allocation;
                }
            
                objects[i].list_heads[j]->next = NULL;
           
                first_page = (object_content *)kzalloc(sizeof(object_content), GFP_KERNEL);
                if(first_page == NULL){
                    goto revert_allocation;
                }

                first_page->stream_content = (char*)__get_free_page(GFP_KERNEL);
                if(first_page->stream_content == NULL){
                    kfree((void*)first_page);
                    goto revert_allocation;
                }
                first_page->read_offset = 0;
                first_page->next = NULL;
                first_page->record_length = 0; 
                objects[i].list_heads[j]->next = first_page;
            
                mutex_init(&(objects[i].operation_synchronizer[j]));
            }
        }

	    Major = __register_chrdev(0, 0, 256, DEVICE_NAME, &fops); //actually allowed minors are directly controlled within this driver

	    if (Major < 0) {
#ifdef DEBUG_INFO
	        printk("%s: registering device failed\n",MODNAME);
#endif
            return Major;
	    }

#ifdef DEV_INFO
	printk(KERN_INFO "%s: new device registered, it is assigned major number %d\n",MODNAME, Major);
#endif

	return 0;


revert_allocation:
	    for(;i>=0;i--){
		    free_page((unsigned long)objects[i].list_heads[0]->next->stream_content);
            free_page((unsigned long)objects[i].list_heads[1]->next->stream_content);
            kfree((void*)objects[i].list_heads[0]->next); 
            kfree((void*)objects[i].list_heads[1]->next);
            kfree((void*)objects[i].list_heads[0]);
            kfree((void*)objects[i].list_heads[1]);
	    }
	    return -ENOMEM;
}


void cleanup_module(void) {
        int i;
        int j;
        object_content* temp_obj;
	    for(i=0;i<MINORS;i++){
            for(j=0;j<2;j++){
                if(objects[i].list_heads[j]->next != NULL){
                    temp_obj = objects[i].list_heads[j]->next;
                    while(temp_obj != NULL){
                        object_content *free_node;
                        free_node = temp_obj;
                        temp_obj = temp_obj->next;
                        free_page((unsigned long)(free_node->stream_content));
                        kfree((void*)free_node);
                    }
                } 
                kfree((void*)objects[i].list_heads[j]); 
	        }
        }

	    unregister_chrdev(Major, DEVICE_NAME);

#ifdef DEV_INFO
	printk(KERN_INFO "%s: new device unregistered, it was assigned major number %d\n",MODNAME, Major);
#endif
	return;
}
