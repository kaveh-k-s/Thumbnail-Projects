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
//For lookaside cache structues
#include <linux/slab.h>
//Ofcourse for using memeory pools
#include <linux/mempool.h>
//For string functions
#include <linux/string.h>


//It is always good to have a meaningful constant as a return code
#define SUCCESS 0
//This will be our module name
#define DEVICE_NAME "stack"
//This is the constant that used for determination of buffer length
#define MAX_BUF_LEN 30


//These are our ioctl definition
#define MAGIC 'T'

#define STACK_IOCTL_RESET		_IO(MAGIC, 0)
#define STACK_IOCTL_FULL		_IOR(MAGIC, 1, int)
#define STACK_IOCTL_EMPTY		_IOR(MAGIC, 2, int)
#define STACK_IOCTL_SIZE		_IOR(MAGIC, 3, int)
#define STACK_IOCTL_AVAIL		_IOR(MAGIC, 4, int)

#define IOC_MAXNR 5

//These are some useful information that could reveald with modinfo command
//Set module license to get rid of tainted kernel warnings
MODULE_LICENSE("GPL");
//Introduce the module's developer and it's functionality
MODULE_AUTHOR("Aliireeza Teymoorian");
MODULE_DESCRIPTION("This is an advance Character Device driver with ioctl, concurrency, capabilities, and auto dev file creation which provide a simple string stack functions to the operating system");


//Major is for device driver major number and device open is just a flag
//to make sure that only one device can access the /dev entry at a moment
//Minor is just a range base for obtaining a range of minor number
// associated with major number
static int stack_open_flag = 0;
static int stack_major, stack_minor;
static int stack_minor_base = 0, stack_minor_count = 1;
static atomic_t stack_open_atomic = ATOMIC_INIT(1);

//Now we have to create some data structures related to our charachter device
static dev_t stack_device;
static struct class *device_class;
static struct cdev stack_cdev;

//Now we have to create a buffer for our longest message (MAX_BUF_LEN)
//and a variable to count our actual message sizeK
static char stack_buffer[MAX_BUF_LEN];
static unsigned long stack_buffer_size = 0;

//Creating a proc directory entry structure
static struct proc_dir_entry* our_proc_file;

//Concurrency structs are defined here
static spinlock_t stack_file_spinlock, stack_file_spinlock;
static wait_queue_head_t our_stack;

//Stack and Stack variables
static struct stack_item{
	char buffer[MAX_BUF_LEN];
	struct stack_item *next;
	};

static struct stack_item *stack_top, *stack_push, *stack_pop, *stack_comm;
static atomic_t stack_count = ATOMIC_INIT(0);

//This is our lookaside cache
struct kmem_cache *our_cache;
//And finally this is our memory pool
mempool_t *our_pool;


//This function calls on demand of read request from seq_files
static int proc_show(struct seq_file *m, void *v){
	static int i ;
	printk(KERN_INFO "STACKCHARDEV: There are %d itmes left in the stack\n", atomic_read(&stack_count));

	spin_lock(&stack_file_spinlock);
	//Now before we begin, we have to be sure that stack_top won't be missing
	stack_comm = stack_top;
	//Then for each member of our stack, we will print out the buffer
	for(i=0;i<atomic_read(&stack_count);i++){
		seq_printf(m, "%d: %s\n", i, stack_top->buffer);
		stack_top = stack_top->next;
			}
	//Now, the remainig step is just restoring the correct position of stack_top pointer
	stack_top = stack_comm;
	spin_unlock(&stack_file_spinlock);

	return 0;
}

//This is where system functionallity triggers every time some process try to read from our proc entry
static int proc_open(struct inode *inode, struct file *file){
	printk(KERN_INFO "STACKCHARDEV: ProcFS Open Function, Process \"%s:%i\"\n", current->comm, current->pid);
	return single_open(file, proc_show, NULL);
}



//When device recive ioctl commands this function will perform the job depending on what kind of command it recieved
long stack_ioctl(struct file *file, unsigned int cmd, unsigned long arg){
	static int err = 0, retval = 0;

	printk(KERN_INFO "STACKCHARDEV: IOCTL Function, Process \"%s:%i\" ", current->comm, current->pid);
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
		case STACK_IOCTL_RESET:
			//This command only works for system administrators
			if(!capable(CAP_SYS_ADMIN))
				return -EPERM;
			//Here just flush all datas and reset the pointer so you could use an empty stack
			printk(KERN_INFO "STACK_IOCTL_RESET\n");
			//For each member of stack we have to free mempool allocated memory
			//And keep tracking of how many items left in the stack with our atomic counter
			for(;atomic_read(&stack_count)>0;atomic_dec(&stack_count)){
				stack_pop = stack_top;
				stack_top = stack_top->next;
				mempool_free(stack_pop, our_pool);
			}

			break;

		case STACK_IOCTL_FULL:
			//Here we check whether the stack is full or not
			printk(KERN_INFO "STACK_IOCTL_FULL\n");
			retval = atomic_read(&stack_count) == 128 ? 1:0;
			break;

		case STACK_IOCTL_EMPTY:
			//Just like the previous condition but here we check whether it is empty or not
			printk(KERN_INFO "STACK_IOCTL_EMPTY\n");
			retval = atomic_read(&stack_count) == 0 ? 1:0;
			break;

		case STACK_IOCTL_SIZE:
			//This ioctl signal will return the size of the stack in how many data quantum it could get
			printk(KERN_INFO "STACK_IOCTL_SIZE\n");
			retval = 128 ;
			break;

		case STACK_IOCTL_AVAIL:
			//This is how we could obtain how many empty room left in the stack for new datas
			printk(KERN_INFO "STACK_IOCTL_AVAIL\n");
			retval = 128 - atomic_read(&stack_count) ;
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
int stack_open(struct inode *inode, struct file *file){
	printk(KERN_INFO "STACKCHARDEV: DEV File Open Function, Process \"%s:%i\"\n", current->comm, current->pid);
	//stack_open_flag just used as a simple mutex, but it is not as safe as one, in real concurrent multitasking
	//So whenever the user try to open the entry point, we have to check that if someone else does the same thing
	//Then increase it and let the process continue

	//This is a more secure way, to use atomic variables
	spin_lock(&stack_file_spinlock);
	if(stack_open_flag == 1 && !atomic_dec_and_test(&stack_open_atomic)){
		spin_unlock(&stack_file_spinlock);
		//If user process can not wait for respose and device was opend by another process, just reject it
		if(file->f_flags & O_NONBLOCK)
			return -EBUSY;

		//If device previously opend, the the user process have to wait until the condition goes wrong
		if(wait_event_interruptible(our_stack, (stack_open_flag == 1))){
			printk(KERN_ALERT "STACKCHARDEV: Stack open function put process \"%s:%i\" in Block mode\n", current->comm, current->pid);
			return -ERESTARTSYS;
		}
		spin_lock(&stack_file_spinlock);
	}
	spin_unlock(&stack_file_spinlock);

	spin_lock(&stack_file_spinlock);
	stack_open_flag++;
	spin_unlock(&stack_file_spinlock);
	atomic_inc(&stack_open_atomic);
	//Eachtime you open the entry point, infact you are using the device, so you have to
	//count the references to it, in order to when you want to release it, you could safely release
	//the device with reference count of zero
	//So we increase the reference count using try_module_get
	try_module_get(THIS_MODULE);
	return SUCCESS;
}


//When a user try to cat or otherwise read the /dev entry, this function does the job
static ssize_t stack_read(struct file *filp, char *buffer, size_t length, loff_t * offset){
	static int ret = 0;
	printk(KERN_INFO "STACKCHARDEV: DEV file read function, Destack, process \"%s:%i\"\n", current->comm, current->pid);

	if (ret) {
		/* we have finished to read, return 0 */
		printk(KERN_INFO "STACKCHARDEV: DEV entry read has END\n");
		ret = 0;
	}
	else{
		spin_lock(&stack_file_spinlock);
		//Pop from the stack and fill the buffer, return the buffer size
		if(atomic_read(&stack_count)>0){
			stack_pop = stack_top;
			stack_top = stack_top->next;
			strcpy(stack_buffer, stack_pop->buffer);
			mempool_free(stack_pop, our_pool);
			atomic_dec(&stack_count);
			if(copy_to_user(buffer, stack_buffer, sizeof(stack_buffer)))
				return -EFAULT;
		}

		spin_unlock(&stack_file_spinlock);

		printk(KERN_INFO "STACKCHARDEV: %lu bytes has read from DEV entry\n", stack_buffer_size);
		ret = stack_buffer_size;
	}

	//The function returns read charachters count
	return ret;
}


//Each time user try to echo something or otherwise write anything to the /dev entry, this function does the job
static ssize_t stack_write(struct file *file, const char *buffer, size_t length, loff_t * off){
	printk(KERN_INFO "STACKCHARDEV: DEV file write function, Enstack, process \"%s:%i\"\n", current->comm, current->pid);
	if (length > MAX_BUF_LEN)
		stack_buffer_size = MAX_BUF_LEN;
	else
		stack_buffer_size = length;

	//write data to the buffer
	spin_lock(&stack_file_spinlock);
	if(copy_from_user(stack_buffer, buffer, stack_buffer_size))
		return -EFAULT;

	spin_unlock(&stack_file_spinlock);

	//Now we have to push data into the stack and reassign the pointers
	if(atomic_read(&stack_count) == 0){
		stack_top = mempool_alloc(our_pool, GFP_USER);
		strcpy(stack_top->buffer, stack_buffer);
		stack_top->next = NULL;
	}
	else{
		stack_push = mempool_alloc(our_pool, GFP_USER);
		strcpy(stack_push->buffer, stack_top->buffer);
		stack_push->next = stack_top->next;

		strcpy(stack_top->buffer, stack_buffer);
		stack_top->next = stack_push;
	}
	atomic_inc(&stack_count);
	spin_unlock(&stack_file_spinlock);
	
	//The function returns wrote charachters count
	printk(KERN_INFO "STACKCHARDEV: %lu bytes has wrote to DEV entry\n", stack_buffer_size);
	return stack_buffer_size;
}


//Each time you release the /dev entry after read or write somthing from and to /dev entry
//This function have to adjust the reference count and does its job
int stack_release(struct inode *inode, struct file *file){
	printk(KERN_INFO "STACKCHARDEV: DEV file release function, process \"%s:%i\"\n", current->comm, current->pid);
	//In time of device release we have to decrease the mutex like flag, stack_open_flag
	spin_lock(&stack_file_spinlock);
	stack_open_flag--;
	spin_unlock(&stack_file_spinlock);
	atomic_inc(&stack_open_atomic);
	wake_up_interruptible(&our_stack);
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
	.read = stack_read,
	.write = stack_write,
	.unlocked_ioctl = stack_ioctl,
	.open = stack_open,
	.release = stack_release
};

static const struct file_operations proc_fops = {
	.open = proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};





//Your module's entry point
static int __init stack_chardev_init(void){
	//These mesages will apear in kernel log
	//You can observe the kernel log in /dev/kmsg or with using dmsg command
	printk(KERN_INFO "STACKCHARDEV: Initialization.\n");
	printk(KERN_INFO "STACKCHARDEV: Init module, process \"%s:%i\"\n", current->comm, current->pid);

	//Now to allocate essential memories for our stack data
	//First, we have to create a lookaside cache
	our_cache = kmem_cache_create("our_lookaside_cache", sizeof(stack_top), 0, SLAB_HWCACHE_ALIGN, NULL);
	if(!our_cache){
		printk(KERN_ALERT "STACKCHARDEV: Cache Registration Failure.\n");
		//Because of this fact that lookaside cache will be allocated from system RAM, this error means the lack of enough memory
		return -ENOMEM;
	}
	printk(KERN_INFO "STACKCHARDEV: Lookaside cache has been created.\n");


	//Second we are going to use memory pool to improve perfomance of slab allocation in lookaside cache
	our_pool = mempool_create(128, mempool_alloc_slab, mempool_free_slab, our_cache);
	if(!our_pool){
		printk(KERN_ALERT "STACKCHARDEV: Pool Creation Failure.\n");
		kmem_cache_destroy(our_cache);
		printk(KERN_INFO "STACKCHARDEV: Lookaside cache has been destroyed.\n");
		//Because of this fact that memory pool like cache will be allocated from system RAM, this error means the lack of enough memory
		return -ENOMEM;
	}
	printk(KERN_INFO "STACKCHARDEV: Memory pool for stack head has been created and attached to lookaside cache\n");


	//We are going to obtain a major number and a range of minor numbers
	//and registering the charachter device and creating its dev entry.
	//This procedure will be accomplished in four steps
	//First, We need to get from system major and minor numbers

	//Second, we have to create a class for our driver
	//First, We need to get from system major and minor numbers
	if(alloc_chrdev_region(&stack_device, stack_minor_base, stack_minor_count, DEVICE_NAME)<0){
		printk(KERN_ALERT "STACKCHARDEV: Device cannot obtain major and minor numbers\n");
		//Free the stack allocated memory
		//First we have to free our pool
		mempool_destroy(our_pool);
		printk(KERN_INFO "STACKCHARDEV: Memory pool has been destroyed.\n");
		//Second, freeing our cache
		kmem_cache_destroy(our_cache);
		printk(KERN_INFO "STACKCHARDEV: Lookaside cache has been destroyed.\n");
		return -EFAULT;
	}
	//Now we will use these tow macros to obtain device information
	stack_major = MAJOR(stack_device);
	stack_minor = MINOR(stack_device);
	printk(KERN_INFO "STACKCHARDEV: Device has obtained Major: %d and Minor: %d successfully.\n", stack_major, stack_minor);

	//Second, we have to create a class for our driver but we did it once
	device_class = class_create(THIS_MODULE, DEVICE_NAME);
	if(!device_class){
		printk(KERN_ALERT "STACKCHARDEV: Device could not create associated class.\n");
		unregister_chrdev_region(stack_minor_base, stack_minor_count);
		printk(KERN_INFO "STACKCHARDEV: Device unregistred Major: %d and Minor: %d\n", stack_major, stack_minor);
		//Free the stack allocated memory
		//First we have to free our pool
		mempool_destroy(our_pool);
		printk(KERN_INFO "STACKCHARDEV: Memory pool has been destroyed.\n");
		//Second, freeing our cache
		kmem_cache_destroy(our_cache);
		printk(KERN_INFO "STACKCHARDEV: Lookaside cache has been destroyed.\n");
		return -EFAULT;
	}
	printk(KERN_INFO "STACKCHARDEV: Device has created associated class successfully.\n");

	//Third, We have to create our device and device file
	if(!device_create(device_class, NULL, stack_device, NULL, "stack")){

		printk(KERN_INFO "STACKCHARDEV: Device removed associated /dev entry.\n");
		unregister_chrdev_region(stack_minor_base, stack_minor_count);
		printk(KERN_INFO "STACKCHARDEV: Device unregistred Major: %d and Minor: %d\n", stack_major, stack_minor);

		class_destroy(device_class);
		printk(KERN_INFO "STACKCHARDEV: Device unregistred associated class.\n");

		//Free the stack allocated memory
		//First we have to free our pool
		mempool_destroy(our_pool);
		printk(KERN_INFO "STACKCHARDEV: Memory pool has been destroyed.\n");
		//Second, freeing our cache
		kmem_cache_destroy(our_cache);
		printk(KERN_INFO "STACKCHARDEV: Lookaside cache has been destroyed.\n");
		return -EFAULT;
	}
	printk(KERN_INFO "STACKCHARDEV: Device file has been created in /dev/stack\n");

	//Fourth, We have to initiate our device to its file_operations struct
	cdev_init(&stack_cdev, &dev_fops);
	if(cdev_add(&stack_cdev, stack_device, stack_minor_count) < 0){
		printk(KERN_ALERT "STACKCHARDEV: Device could not initialize to the system.\n");
		device_destroy(device_class, stack_device);
		printk(KERN_INFO "STACKCHARDEV: Device removed associated /dev entry.\n");
		unregister_chrdev_region(stack_minor_base, stack_minor_count);
		printk(KERN_INFO "STACKCHARDEV: Device unregistred Major: %d and Minor: %d\n", stack_major, stack_minor);

		class_destroy(device_class);
		printk(KERN_INFO "STACKCHARDEV: Device unregistred associated class.\n");

		//Free the stack allocated memory
		//First we have to free our pool
		mempool_destroy(our_pool);
		printk(KERN_INFO "STACKCHARDEV: Memory pool has been destroyed.\n");
		//Second, freeing our cache
		kmem_cache_destroy(our_cache);
		printk(KERN_INFO "STACKCHARDEV: Lookaside cache has been destroyed.\n");
		return -EFAULT;
	}


	our_proc_file = proc_create(DEVICE_NAME, 0644 , NULL, &proc_fops);
	//Put an error message in kernel log if cannot create proc entry
	if(!our_proc_file){
		printk(KERN_ALERT "STACKCHARDEV: ProcFS registration failure.\n");

		cdev_del(&stack_cdev);
		printk(KERN_INFO "STACKCHARDEV: Device released associated file operations.\n");
		device_destroy(device_class, stack_device);
		printk(KERN_INFO "STACKCHARDEV: Device removed associated /dev entry.\n");
		unregister_chrdev_region(stack_minor_base, stack_minor_count);
		printk(KERN_INFO "STACKCHARDEV: Device unregistred Major: %d and Minor: %d\n", stack_major, stack_minor);

		class_destroy(device_class);
		printk(KERN_INFO "STACKCHARDEV: Device unregistred associated class.\n");

		//Free the stack allocated memory
		//First we have to free our pool
		mempool_destroy(our_pool);
		printk(KERN_INFO "STACKCHARDEV: Memory pool has been destroyed.\n");
		//Second, freeing our cache
		kmem_cache_destroy(our_cache);
		printk(KERN_INFO "STACKCHARDEV: Lookaside cache has been destroyed.\n");
		//Because of this fact that procfs is a ram filesystem, this error means the lack of enough memory
		return -ENOMEM;
	}
	printk(KERN_INFO "STACKCHARDEV: /proc/%s has been created.\n", DEVICE_NAME);


	//This is where we print out the ioctl commands through kernel log
	printk(KERN_INFO "STACKCHARDEV: Ioctl Commands:\n");
	printk(KERN_INFO "STACKCHARDEV: STACK_IOCTL_RESET\t%u\n", STACK_IOCTL_RESET);
	printk(KERN_INFO "STACKCHARDEV: STACK_IOCTL_FULL\t%lu\n", STACK_IOCTL_FULL);
	printk(KERN_INFO "STACKCHARDEV: STACK_IOCTL_EMPTY\t%lu\n", STACK_IOCTL_EMPTY);
	printk(KERN_INFO "STACKCHARDEV: STACK_IOCTL_SIZE\t%lu\n", STACK_IOCTL_SIZE);
	printk(KERN_INFO "STACKCHARDEV: STACK_IOCTL_AVAIL\t%lu\n", STACK_IOCTL_AVAIL);

	spin_lock_init(&stack_file_spinlock);
	init_waitqueue_head(&our_stack);


	//The init_module should return a value to the rest of kernel that asure
	//them to its successfully registration of its functionality
	return SUCCESS;
}


//You sould clean up the mess before exiting the module
static void __exit stack_chardev_exit(void){
	printk(KERN_INFO "STACKCHARDEV: Cleanup module, process \"%s:%i\"\n", current->comm, current->pid);

	//To get rid of our character device we have to do a four-step procedure as well
	cdev_del(&stack_cdev);
	printk(KERN_INFO "STACKCHARDEV: device released associated file operations.\n");
	device_destroy(device_class, stack_device);
	printk(KERN_INFO "STACKCHARDEV: device removed associated /dev entry.\n");
	unregister_chrdev_region(stack_minor_base, stack_minor_count);
	printk(KERN_INFO "STACKCHARDEV: device unregistred Major: %d and Minor: %d\n", stack_major, stack_minor);

	class_destroy(device_class);
	printk(KERN_INFO "STACKCHARDEV: Device unregistred associated class.\n");

	//Remove procfs entry
	remove_proc_entry(DEVICE_NAME, NULL);
	printk(KERN_INFO "STACKCHARDEV: /proc/%s has been removed.\n", DEVICE_NAME);

	//Free the stack allocated memory
	//First we have to free our pool
	mempool_destroy(our_pool);
	printk(KERN_INFO "STACKCHARDEV: Memory pool has been destroyed.\n");
	//Second, freeing our cache
	kmem_cache_destroy(our_cache);
	printk(KERN_INFO "STACKCHARDEV: Lookaside cache has been destroyed.\n");

	printk(KERN_INFO "STACKCHARDEV: Device unregistred associated Stack.\n");

	printk(KERN_INFO "STACKCHARDEV: GoodBye.\n");
	//The cleanup_module function doesn't need to return any value to the rest of the kernel
}

//Now we need to define init-module and cleanup_module aliases
module_init(stack_chardev_init);
module_exit(stack_chardev_exit);
