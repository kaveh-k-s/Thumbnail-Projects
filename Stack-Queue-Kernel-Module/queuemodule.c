//We need this header in all kernel modules
#include <linux/module.h>
//Absolutely because we are doing kernel job
#include <linux/kernel.h>
//And this is needed for the macros
#include <linux/init.h>
//For create and register a procfs entry
#include <linux/proc_fs.h>
//For providing read function of the entry with ease
#include <linux/seq_file.h>
//For struct file_operations
#include <linux/fs.h>
//For cdev struct and functions
#include <linux/cdev.h>
//For device create and structs and other functions
#include <linux/device.h>
//For copy_to_user, copy_from_user, put_user
#include <asm/uaccess.h>
//For finding the parent process ID of the module
#include <asm/current.h>
//For using task_struct
#include <linux/sched.h>
//For ioctl commands and macros
#include <linux/ioctl.h>
//For evaluating capabilities
#include <linux/capability.h>
//For using spinlocks
#include <linux/spinlock.h>
//For blocking I/O
#include <linux/wait.h>
//For making a Queue
#include <linux/kfifo.h>
//For string functions
#include <linux/string.h>

//It is always good to have a meaningful constant as a return code
#define SUCCESS 0
//This will be our module name
#define DEVICE_NAME "queue"
//This is the constant that used for determination of buffer length
#define MAX_BUF_LEN 30


//These are our ioctl definition
#define MAGIC 'Q'

#define QUEUE_IOCTL_RESET		_IO(MAGIC, 0)
#define QUEUE_IOCTL_FULL		_IOR(MAGIC, 1, int)
#define QUEUE_IOCTL_EMPTY		_IOR(MAGIC, 2, int)
#define QUEUE_IOCTL_SIZE		_IOR(MAGIC, 3, int)
#define QUEUE_IOCTL_AVAIL		_IOR(MAGIC, 4, int)

#define IOC_MAXNR 5

//These are some useful information that could reveald with modinfo command
//Set module license to get rid of tainted kernel warnings
MODULE_LICENSE("GPL");
//Introduce the module's developer and it's functionality
MODULE_AUTHOR("Aliireeza Teymoorian");
MODULE_DESCRIPTION("This is an advance Character Device driver with ioctl, concurrency, capabilities, and auto dev file creation which provide a simple string queue functions to the operating system");


//Major is for device driver major number and device open is just a flag
//to make sure that only one device can access the /dev entry at a moment
//Minor is just a range base for obtaining a range of minor number
// associated with major number
static int queue_open_flag = 0;
static int queue_major, queue_minor;
static int queue_minor_base = 0, queue_minor_count = 1;
static atomic_t queue_open_atomic = ATOMIC_INIT(1);

//Now we have to create some data structures related to our charachter device
static dev_t queue_device;
static struct class *device_class;
static struct cdev queue_cdev;

//Now we have to create a buffer for our longest message (MAX_BUF_LEN)
//and a variable to count our actual message sizeK
static char queue_buffer[MAX_BUF_LEN];
static unsigned long queue_buffer_size = 0;

//Creating a proc directory entry structure
static struct proc_dir_entry* our_proc_file;

//Concurrency structs are defined here
static spinlock_t stack_file_spinlock, queue_file_spinlock;
static wait_queue_head_t our_queue;

//Queue and Stack variables
static struct kfifo data_queue;
typedef struct {char buf[MAX_BUF_LEN];} data_queue_type;


//This function calls on demand of read request from seq_files
static int proc_show(struct seq_file *m, void *v){
	static int occupied_space, i;
	static char peek_queue_buffer[MAX_BUF_LEN];

	spin_lock(&queue_file_spinlock);

	//Here we have to obtain how many items left in the queue
	//So we calculate the difference between actual queue size (in bytes) and available space (in bytes too)
	//Then we divide the result by our data quantum (in bytes), the result means the number of items
	occupied_space = (kfifo_size(&data_queue) - kfifo_avail(&data_queue))/ sizeof(data_queue_type);
	printk(KERN_INFO "QUEUECHARDEV: There are %d itmes left in the queue\n", occupied_space);

	//Then for each item, we pop it, print the result and insert in at the end of the queue ;)
	for(i=0; i<occupied_space; i++){
		kfifo_out(&data_queue, peek_queue_buffer, sizeof(data_queue_type));
		seq_printf(m, "%d: %s\n", i, peek_queue_buffer);
		kfifo_in(&data_queue,  peek_queue_buffer, sizeof(data_queue_type));
	}
	spin_unlock(&queue_file_spinlock);

	return 0;
}

//This is where system functionallity triggers every time some process try to read from our proc entry
static int proc_open(struct inode *inode, struct file *file){
	printk(KERN_INFO "QUEUECHARDEV: ProcFS Open Function, Process \"%s:%i\"\n", current->comm, current->pid);
	return single_open(file, proc_show, NULL);
}



//When device recive ioctl commands this function will perform the job depending on what kind of command it recieved
long queue_ioctl(struct file *file, unsigned int cmd, unsigned long arg){
	static int err = 0, retval = 0;

	printk(KERN_INFO "QUEUECHARDEV: IOCTL Function, Process \"%s:%i\" ", current->comm, current->pid);
	printk(KERN_INFO ",Command, %d:", cmd);

	if(_IOC_TYPE(cmd) != MAGIC || _IOC_NR(cmd) > IOC_MAXNR)
		return -ENOTTY;

	if(_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *) arg, _IOC_SIZE(cmd));
	if(_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void __user *) arg, _IOC_SIZE(cmd));
	if(err)
		return -EFAULT;

	if(!capable(CAP_SYS_ADMIN))
			return -EPERM;

	//After we asure that this is a proper ioctl signal we have to decode and run it
	switch(cmd){
		case QUEUE_IOCTL_RESET:
			//Here just flush all datas and reset the pointer so you could use an empty queue
			printk(KERN_INFO "QUEUE_IOCTL_RESET\n");
			kfifo_reset(&data_queue);
			break;

		case QUEUE_IOCTL_FULL:
			//Here we check whether the queue is full or not
			printk(KERN_INFO "QUEUE_IOCTL_FULL\n");
			retval = kfifo_is_full(&data_queue);
			break;

		case QUEUE_IOCTL_EMPTY:
			//Just like the previous condition but here we check whether it is empty or not
			printk(KERN_INFO "QUEUE_IOCTL_EMPTY\n");
			retval = kfifo_is_empty(&data_queue);
			break;

		case QUEUE_IOCTL_SIZE:
			//This ioctl signal will return the size of the queue in how many data quantum it could get
			printk(KERN_INFO "QUEUE_IOCTL_SIZE\n");
			retval = kfifo_size(&data_queue)/sizeof(data_queue_type);
			break;

		case QUEUE_IOCTL_AVAIL:
			//This is how we could obtain how many empty room left in the queue for new datas
			printk(KERN_INFO "QUEUE_IOCTL_AVAIL\n");
			retval = kfifo_avail(&data_queue)/sizeof(data_queue_type);
			break;

		default:
			//I dont know how logically this situation might be possible, but just in case
			//This means a proper ioctl command recieved badly or anything :)
			return -ENOTTY;
	}

	return retval;
}


//Each time a process try to open /dev entry in order to write to or read from it
//This function have to do something like adjusting reference count
int queue_open(struct inode *inode, struct file *file){
	printk(KERN_INFO "QUEUECHARDEV: DEV File Open Function, Process \"%s:%i\"\n", current->comm, current->pid);
	//queue_open_flag just used as a simple mutex, but it is not as safe as one, in real concurrent multitasking
	//So whenever the user try to open the entry point, we have to check that if someone else does the same thing
	//Then increase it and let the process continue

	//This is a more secure way, to use atomic variables
	spin_lock(&queue_file_spinlock);
	if(queue_open_flag == 1 && !atomic_dec_and_test(&queue_open_atomic)){
		spin_unlock(&queue_file_spinlock);
		//If user process can not wait for respose and device was opend by another process, just reject it
		if(file->f_flags & O_NONBLOCK)
			return -EBUSY;

		//If device previously opend, the the user process have to wait until the condition goes wrong
		if(wait_event_interruptible(our_queue, (queue_open_flag == 1))){
			printk(KERN_ALERT "QUEUECHARDEV: Queue open function put process \"%s:%i\" in Block mode\n", current->comm, current->pid);
			return -ERESTARTSYS;
		}
		spin_lock(&queue_file_spinlock);
	}
	spin_unlock(&queue_file_spinlock);

	spin_lock(&queue_file_spinlock);
	queue_open_flag++;
	spin_unlock(&queue_file_spinlock);
	atomic_inc(&queue_open_atomic);
	//Eachtime you open the entry point, infact you are using the device, so you have to
	//count the references to it, in order to when you want to release it, you could safely release
	//the device with reference count of zero
	//So we increase the reference count using try_module_get
	try_module_get(THIS_MODULE);
	return SUCCESS;
}


//When a user try to cat or otherwise read the /dev entry, this function does the job
static ssize_t queue_read(struct file *filp, char *buffer, size_t length, loff_t * offset){
	static int ret = 0;
	printk(KERN_INFO "QUEUECHARDEV: DEV file read function, Dequeue, process \"%s:%i\"\n", current->comm, current->pid);

	if (ret) {
		/* we have finished to read, return 0 */
		printk(KERN_INFO "QUEUECHARDEV: DEV entry read has END\n");
		ret = 0;
	}
	else{
		spin_lock(&queue_file_spinlock);
		//Pop from the queue and fill the buffer, return the buffer size
		if(!kfifo_is_empty(&data_queue))
			kfifo_out(&data_queue, queue_buffer, sizeof(data_queue_type));
		else{
			static int i = 0;
			for(i=0; i<MAX_BUF_LEN; queue_buffer[i++] = NULL);
			}

		if(copy_to_user(buffer, queue_buffer, sizeof(queue_buffer)))
			return -EFAULT;
		spin_unlock(&queue_file_spinlock);

		printk(KERN_INFO "QUEUECHARDEV: %lu bytes has read from DEV entry\n", queue_buffer_size);
		ret = queue_buffer_size;
	}

	//The function returns read charachters count
	return ret;
}


//Each time user try to echo something or otherwise write anything to the /dev entry, this function does the job
static ssize_t queue_write(struct file *file, const char *buffer, size_t length, loff_t * off){
	printk(KERN_INFO "QUEUECHARDEV: DEV file write function, Enqueue, process \"%s:%i\"\n", current->comm, current->pid);
	if (length > MAX_BUF_LEN)
		queue_buffer_size = MAX_BUF_LEN;
	else
		queue_buffer_size = length;

	//write data to the buffer
	spin_lock(&queue_file_spinlock);
	if(copy_from_user(queue_buffer, buffer, queue_buffer_size))
		return -EFAULT;

	if(!kfifo_is_full(&data_queue))
		kfifo_in(&data_queue, queue_buffer, sizeof(data_queue_type));
	spin_unlock(&queue_file_spinlock);

	//The function returns wrote charachters count
	printk(KERN_INFO "QUEUECHARDEV: %lu bytes has wrote to DEV entry\n", queue_buffer_size);
	return queue_buffer_size;
}


//Each time you release the /dev entry after read or write somthing from and to /dev entry
//This function have to adjust the reference count and does its job
int queue_release(struct inode *inode, struct file *file){
	printk(KERN_INFO "QUEUECHARDEV: DEV file release function, process \"%s:%i\"\n", current->comm, current->pid);
	//In time of device release we have to decrease the mutex like flag, queue_open_flag
	spin_lock(&queue_file_spinlock);
	queue_open_flag--;
	spin_unlock(&queue_file_spinlock);
	atomic_inc(&queue_open_atomic);
	wake_up_interruptible(&our_queue);
	//When you release the entry point, that means you have finished with the device so
	//decrese the reference count wit module_put
	module_put(THIS_MODULE);
	return SUCCESS;
}


//Struct file_operations is the key to the functionality of the module
//functions that defined here are going to add to the kernel functionallity
//in order to respond to userspace access demand to the correspond /dev entry
static struct file_operations dev_fops = {
	.owner = THIS_MODULE,
	.read = queue_read,
	.write = queue_write,
	.unlocked_ioctl = queue_ioctl,
	.open = queue_open,
	.release = queue_release
};

static const struct file_operations proc_fops = {
	.open = proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


//You sould clean up the mess before exiting the module
static void __exit queue_chardev_exit(void){
	printk(KERN_INFO "QUEUECHARDEV: Cleanup module, process \"%s:%i\"\n", current->comm, current->pid);

	//To get rid of our character device we have to do a four-step procedure as well
	cdev_del(&queue_cdev);
	printk(KERN_INFO "QUEUECHARDEV: device released associated file operations.\n");
	device_destroy(device_class, queue_device);
	printk(KERN_INFO "QUEUECHARDEV: device removed associated /dev entry.\n");
	unregister_chrdev_region(queue_minor_base, queue_minor_count);
	printk(KERN_INFO "QUEUECHARDEV: device unregistred Major: %d and Minor: %d\n", queue_major, queue_minor);

	class_destroy(device_class);
	printk(KERN_INFO "QUEUECHARDEV: Device unregistred associated class.\n");

	//Remove procfs entry
	remove_proc_entry(DEVICE_NAME, NULL);
	printk(KERN_INFO "QUEUECHARDEV: /proc/%s has been removed.\n", DEVICE_NAME);

	//Free the queue allocated memory
	kfifo_free(&data_queue);
	printk(KERN_INFO "QUEUECHARDEV: Device unregistred associated Queue.\n");

	printk(KERN_INFO "QUEUECHARDEV: GoodBye.\n");
	//The cleanup_module function doesn't need to return any value to the rest of the kernel
}


//Your module's entry point
static int __init queue_chardev_init(void){
	//These mesages will apear in kernel log
	//You can observe the kernel log in /dev/kmsg or with using dmsg command
	printk(KERN_INFO "QUEUECHARDEV: Initialization.\n");
	printk(KERN_INFO "QUEUECHARDEV: Init module, process \"%s:%i\"\n", current->comm, current->pid);
	//We are going to obtain a major number and a range of minor numbers
	//and registering the charachter device and creating its dev entry.
	//This procedure will be accomplished in four steps
	//First, We need to get from system major and minor numbers

	//Second, we have to create a class for our driver
	//First, We need to get from system major and minor numbers
	if(alloc_chrdev_region(&queue_device, queue_minor_base, queue_minor_count, DEVICE_NAME)<0){
		printk(KERN_ALERT "QUEUECHARDEV: Device cannot obtain major and minor numbers\n");
		return -EFAULT;
	}
	//Now we will use these tow macros to obtain device information
	queue_major = MAJOR(queue_device);
	queue_minor = MINOR(queue_device);
	printk(KERN_INFO "QUEUECHARDEV: Device has obtained Major: %d and Minor: %d successfully.\n", queue_major, queue_minor);

	//Second, we have to create a class for our driver but we did it once
	device_class = class_create(THIS_MODULE, DEVICE_NAME);
	if(!device_class){
		printk(KERN_ALERT "QUEUECHARDEV: Device could not create associated class.\n");
		unregister_chrdev_region(queue_minor_base, queue_minor_count);
		printk(KERN_INFO "QUEUECHARDEV: Device unregistred Major: %d and Minor: %d\n", queue_major, queue_minor);
		return -EFAULT;
	}
	printk(KERN_INFO "QUEUECHARDEV: Device has created associated class successfully.\n");

	//Third, We have to create our device and device file
	if(!device_create(device_class, NULL, queue_device, NULL, "queue")){

		printk(KERN_INFO "QUEUECHARDEV: Device removed associated /dev entry.\n");
		unregister_chrdev_region(queue_minor_base, queue_minor_count);
		printk(KERN_INFO "QUEUECHARDEV: Device unregistred Major: %d and Minor: %d\n", queue_major, queue_minor);

		class_destroy(device_class);
		printk(KERN_INFO "QUEUECHARDEV: Device unregistred associated class.\n");
		return -EFAULT;
	}
	printk(KERN_INFO "QUEUECHARDEV: Device file has been created in /dev/queue\n");

	//Fourth, We have to initiate our device to its file_operations struct
	cdev_init(&queue_cdev, &dev_fops);
	if(cdev_add(&queue_cdev, queue_device, queue_minor_count) < 0){
		printk(KERN_ALERT "QUEUECHARDEV: Device could not initialize to the system.\n");
		device_destroy(device_class, queue_device);
		printk(KERN_INFO "QUEUECHARDEV: Device removed associated /dev entry.\n");
		unregister_chrdev_region(queue_minor_base, queue_minor_count);
		printk(KERN_INFO "QUEUECHARDEV: Device unregistred Major: %d and Minor: %d\n", queue_major, queue_minor);

		class_destroy(device_class);
		printk(KERN_INFO "QUEUECHARDEV: Device unregistred associated class.\n");
		return -EFAULT;
	}


	our_proc_file = proc_create(DEVICE_NAME, 0644 , NULL, &proc_fops);
	//Put an error message in kernel log if cannot create proc entry
	if(!our_proc_file){
		printk(KERN_ALERT "QUEUECHARDEV: ProcFS registration failure.\n");

		cdev_del(&queue_cdev);
		printk(KERN_INFO "QUEUECHARDEV: Device released associated file operations.\n");
		device_destroy(device_class, queue_device);
		printk(KERN_INFO "QUEUECHARDEV: Device removed associated /dev entry.\n");
		unregister_chrdev_region(queue_minor_base, queue_minor_count);
		printk(KERN_INFO "QUEUECHARDEV: Device unregistred Major: %d and Minor: %d\n", queue_major, queue_minor);

		class_destroy(device_class);
		printk(KERN_INFO "QUEUECHARDEV: Device unregistred associated class.\n");
		//Because of this fact that procfs is a ram filesystem, this error means the lack of enough memory
		return -ENOMEM;
	}
	printk(KERN_INFO "QUEUECHARDEV: /proc/%s has been created.\n", DEVICE_NAME);


	if(kfifo_alloc(&data_queue, PAGE_SIZE, GFP_USER) != 0){
		queue_chardev_exit();
		return -ENOMEM;
	}

	DEFINE_KFIFO(data_queue, data_queue_type, 128);
	printk(KERN_INFO "QUEUECHARDEV: KFIFO has been allocated.\n");

	spin_lock_init(&queue_file_spinlock);
	init_waitqueue_head(&our_queue);
	//The init_module should return a value to the rest of kernel that asure
	//them to its successfully registration of its functionality
	return SUCCESS;
}


//Now we need to define init-module and cleanup_module aliases
module_init(queue_chardev_init);
module_exit(queue_chardev_exit);
