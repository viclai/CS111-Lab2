#include <linux/version.h>
#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>  /* printk() */
#include <linux/errno.h>   /* error codes */
#include <linux/types.h>   /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/wait.h>
#include <linux/file.h>

#include "spinlock.h"
#include "osprd.h"

/* The size of an OSPRD sector. */
#define SECTOR_SIZE	512

/* This flag is added to an OSPRD file's f_flags to indicate that the file
 * is locked. */
#define F_OSPRD_LOCKED	0x80000

/* eprintk() prints messages to the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("CS 111 RAM Disk");
// EXERCISE: Pass your names into the kernel as the module's authors.
MODULE_AUTHOR("Gloria Chan & Victor Lai");

#define OSPRD_MAJOR	222

/* This module parameter controls how big the disk will be.
 * You can specify module parameters when you load the module,
 * as an argument to insmod: "insmod osprd.ko nsectors=4096" */
static int nsectors = 32;
module_param(nsectors, int, 0);

struct process {
	struct task_struct* info;
	int reqNotif;    // Tells if the process requested a notification. 
			 // 1: wants notification, 0: no notification
	int sectors[32]; // Keeps track of which sectors of the disk have been 
			 // written to. 
			 // 1: sector has been modified, 0: no change to sector
	unsigned sect;  // Only used if the process is writing 
};

struct pidNode {
	struct process* proc;
	struct pidNode* next;
};

struct pidList {
	struct pidNode* head;
	unsigned size;
};

struct ticketNode {
	unsigned ticket;
	struct ticketNode* next;
};

struct ticketList {
	struct ticketNode* head;
	unsigned size;
};

/* The internal representation of our device. */
typedef struct osprd_info {
	uint8_t *data;                   // The data array. Its size is
	                                 // (nsectors * SECTOR_SIZE) bytes.

	osp_spinlock_t mutex;            // Mutex for synchronizing access to
					 // this block device

	unsigned ticket_head;		 // Currently running ticket for
					 // the device lock

	unsigned ticket_tail;		 // Next available ticket for
					 // the device lock

	wait_queue_head_t blockq;        // Wait queue for tasks blocked on
					 // the device lock

	/* HINT: You may want to add additional fields to help
	         in detecting deadlock. */
	struct pidList* readProcs;       // Maintain a list of processes that 
					 // hold a read lock

	struct pidList* writeProcs;      // Maintain a list of processes that 
					 // hold a write lock

	struct ticketList* exitedTickets;// Maintain a list of tickets that 
					 // have exited

	int isHoldingOtherLocks;	 // Tells if the device has another 
					 // lock. If so, there's a deadlock! 
					 // 1: has another lock, 0: no lock 

	struct pidList* notifProcs;	 // Maintain a list of processes that 
					 // requested a change notification

	struct pidList* writeNlkProcs;	 // Maintain a list of processes that 
					 // want to write with no lock. 

	// The following elements are used internally; you don't need
	// to understand them.
	struct request_queue *queue;    // The device request queue.
	spinlock_t qlock;		// Used internally for mutual
	                                //   exclusion in the 'queue'.
	struct gendisk *gd;             // The generic disk.
} osprd_info_t;

#define NOSPRD 4
static osprd_info_t osprds[NOSPRD];


// Declare useful helper functions

/* Precondition: l is the pidList specified to add the process p to. */
void addToPidList(struct pidList** l, struct process* p)
{
	struct pidNode* newNode;
	/* Just add a pid node if the list is empty. */
	if (*l == NULL) {
		/* kzalloc: allocates kernel memory and zeroes out the allocated
		 * memory (defined in <linux/slab.h>). */
		*l = kzalloc(sizeof(struct pidList), GFP_ATOMIC);
		(*l)->head = NULL;
		(*l)->size = 0;
	}
	newNode = kzalloc(sizeof(struct pidNode), GFP_ATOMIC);
	newNode->proc = p;
	/* Head of list always points to the new pid node. */
	if ((*l)->head == NULL) {
		(*l)->head = newNode;
		newNode->next = NULL;
	}
	else {
		newNode->next = (*l)->head;
		(*l)->head = newNode;
	}
	(*l)->size = (*l)->size + 1;
}

/* Precondition: l is the pidList specified to remove the pid value p from. */
void removeFromPidList(struct pidList** l, pid_t p)
{
	struct pidNode* cur;
	struct pidNode* deleteMe;

	if (l == NULL)
		return;

	cur = (*l)->head;
	if (cur == NULL)
		return;

	if (cur->proc->info->pid == p) {
		(*l)->head = cur->next;
		kfree(cur->proc);
		kfree(cur); // kfree: frees kernel memory
		(*l)->size = (*l)->size - 1;
	}
	/* Remove other occurences of p in the list. */
	while (cur->next != NULL) {
		if (cur->next->proc->info->pid == p) {
			deleteMe = cur->next;
			cur->next = cur->next->next;
			kfree(deleteMe->proc);
			kfree(deleteMe);
			(*l)->size = (*l)->size - 1;
			return;
		}
		cur = cur->next;
	}
	/* Deallocate list if there are no more nodes. */
	if ((*l)->size == 0) {
		kfree(*l);
		*l = NULL;
	}
}

/* Precondition: l is the pidList specified to see if the pid value p exits. 
 * Postcondition: Returns the address of "struct process*" if p is in the list 
 * and NULL otherwise. */
struct process* isInPidList(struct pidList* l, pid_t p)
{
	struct pidNode* cur;
	if (l == NULL)
		return NULL;
	cur = l->head;
	while (cur != NULL) {
		if (cur->proc->info->pid == p)
			return cur->proc;
		cur = cur->next;
	}
	return NULL;
}

/* Precondition: l is the ticketList to add to and t is the ticket to be added. */
void addToTicketList(struct ticketList** l, unsigned t)
{
	struct ticketNode* newNode;
	/* Just add a ticket node if the list is empty. */
	if (*l == NULL) {
		*l = kzalloc(sizeof(struct ticketList), GFP_ATOMIC);
		(*l)->head = NULL;
		(*l)->size = 0;
	}
	newNode = kzalloc(sizeof(struct ticketNode), GFP_ATOMIC);
	newNode->ticket = t;
	if ((*l)->head == NULL) {
		(*l)->head = newNode;
		newNode->next = NULL;
	}
	else {
		newNode->next = (*l)->head;
		(*l)->head = newNode;
	}
}

/* Precondition: l is the ticketList specified to remove the ticket t from. */
void removeFromTicketList(struct ticketList** l, unsigned t)
{
	struct ticketNode* cur;
	struct ticketNode* deleteMe;

	if (l == NULL)
		return;

	cur = (*l)->head;
	if (cur == NULL)
		return;

	if (cur->ticket == t) {
		(*l)->head = cur->next;
		kfree(cur);
		(*l)->size = (*l)->size - 1;
	}
	/* Remove other occurrences of t in the list. */
	while (cur->next != NULL) {
		if (cur->next->ticket == t) {
			deleteMe = cur->next;
			cur->next = cur->next->next;
			kfree(deleteMe);
			(*l)->size = (*l)->size - 1;
			return;
		}
		cur = cur->next;
	}
	/* Deallocate list if there are no more nodes. */
	if ((*l)->size == 0) {
		kfree(*l);
		*l = NULL;
	}
}

/* Precondition: l is the ticketList specified to see if the ticket t exists. 
 * Postcondition: Returns 1 if t is in the list and 0 otherwise. */
int isInTicketList(struct ticketList* l, unsigned t)
{
	struct ticketNode* cur;
	if (l == NULL)
		return 0;
	cur = l->head;
	while (cur != NULL) {
		if (cur->ticket == t)
			return 1;
		cur = cur->next;
	}
	return 0;
}

/* Increment ticket_tail so that exited tickets are avoided. */
void incrementTicket(osprd_info_t* d)
{
	d->ticket_tail = d->ticket_tail + 1;
	while (1) {
		if (!isInTicketList(d->exitedTickets, d->ticket_tail))
			break; // The next process is alive (not exited).
		else
			removeFromTicketList(&(d->exitedTickets), 
				d->ticket_tail);
		d->ticket_tail = d->ticket_tail + 1;
	}
}

/*
 * file2osprd(filp)
 *   Given an open file, check whether that file corresponds to an OSP ramdisk.
 *   If so, return a pointer to the ramdisk's osprd_info_t.
 *   If not, return NULL.
 */
static osprd_info_t *file2osprd(struct file *filp);

/*
 * for_each_open_file(task, callback, user_data)
 *   Given a task, call the function 'callback' once for each of 'task's open
 *   files.  'callback' is called as 'callback(filp, user_data)'; 'filp' is
 *   the open file, and 'user_data' is copied from for_each_open_file's third
 *   argument.
 */
static void for_each_open_file(struct task_struct *task,
			       void (*callback)(struct file *filp,
						osprd_info_t *user_data),
			       osprd_info_t *user_data);

/* Pass this function into the second argument of for_each_open_file to see if
 * the current process has any locks in other ramdisks. */
void checkForOtherLocks(struct file *filp, osprd_info_t *data)
{
        osprd_info_t* dev = file2osprd(filp);
        if (dev != NULL) {
                if (isInPidList(dev->writeProcs, current->pid) ||
                        isInPidList(dev->readProcs, current->pid))
                        data->isHoldingOtherLocks = 1;
        }
}

/*
 * osprd_process_request(d, req)
 *   Called when the user reads or writes a sector.
 *   Should perform the read or write, as appropriate.
 */
static void osprd_process_request(osprd_info_t *d, struct request *req)
{
	struct pidNode* cur;
	struct process* p;
	uint8_t* dPtr;
	unsigned int reqType;

	if (!blk_fs_request(req)) {
		end_request(req, 0);
		return;
	}

	// EXERCISE: Perform the read or write request by copying data between
	// our data array and the request's buffer.
	// Hint: The 'struct request' argument tells you what kind of request
	// this is, and which sectors are being read or written.
	// Read about 'struct request' in <linux/blkdev.h>.
	// Consider the 'req->sector', 'req->current_nr_sectors', and
	// 'req->buffer' members, and the rq_data_dir() function.

	// Your code here.
	
	/* Get pointer to data on disk requested by the user.
	 * req->sector: sector specified by the user to read/write to.
	 * (req->sector * SECTOR_SIZE): offset */
	dPtr = d->data + ((req->sector) * SECTOR_SIZE);
        /* Determine whether the request is a read or a write.
         * READ: 0, WRITE: 1 (defined in <linux/fs.h>) */
	reqType = rq_data_dir(req);
	/* req->current_nr_sectors: number of sectors to read/write to. */
	if (reqType == READ) {
		/* Copy contents of data buffer into request's buffer. */
		memcpy((void*) req->buffer, (void*) dPtr, 
			req->current_nr_sectors * SECTOR_SIZE);
	}
	else { // reqType == WRITE
		/* Copy contents of request's buffer into data buffer. */
		memcpy((void*) dPtr, (void*) req->buffer,
			req->current_nr_sectors * SECTOR_SIZE);
		/* Notify processes that requested change notifications. */
		if (d->notifProcs != NULL) {
			osp_spin_lock(&(d->mutex));
			cur = d->notifProcs->head;
			p = isInPidList(d->writeProcs, current->pid);
			if (p == NULL)
				p = isInPidList(d->writeNlkProcs, current->pid);
			while (cur != NULL) {
				cur->proc->reqNotif = 0;
				/* Set the sector of the disk that was 
				 * changed. */
				cur->proc->sectors[p->sect] = 1;
				cur = cur->next;
			}
			osp_spin_unlock(&(d->mutex));
//			wake_up_all(&(d->blockq));
		}
	}

	end_request(req, 1);
}


// This function is called when a /dev/osprdX file is opened.
// You aren't likely to need to change this.
static int osprd_open(struct inode *inode, struct file *filp)
{
	// Always set the O_SYNC flag. That way, we will get writes immediately
	// instead of waiting for them to get through write-back caches.
	filp->f_flags |= O_SYNC;
	return 0;
}


// This function is called when a /dev/osprdX file is finally closed.
// (If the file descriptor was dup2ed, this function is called only when the
// last copy is closed.)
static int osprd_close_last(struct inode *inode, struct file *filp)
{
	if (filp) {
		osprd_info_t *d = file2osprd(filp);
		int filp_writable = filp->f_mode & FMODE_WRITE;

		// EXERCISE: If the user closes a ramdisk file that holds
		// a lock, release the lock.  Also wake up blocked processes
		// as appropriate.

		// Your code here.

		// This line avoids compiler warnings; you may remove it.
		(void) filp_writable, (void) d;

		if (d == NULL)
			return 1;
		osp_spin_lock(&(d->mutex));

		if (isInPidList(d->writeProcs, current->pid))
			removeFromPidList(&(d->writeProcs), current->pid);
		if (isInPidList(d->readProcs, current->pid))
			removeFromPidList(&(d->readProcs), current->pid);
		if (isInPidList(d->notifProcs, current->pid))
			removeFromPidList(&(d->notifProcs), current->pid);
		if (isInPidList(d->writeNlkProcs, current->pid))
			removeFromPidList(&(d->writeNlkProcs), current->pid);

		if (d->readProcs == NULL && d->writeProcs == NULL)
			filp->f_flags &= !F_OSPRD_LOCKED; // Clear lock

		osp_spin_unlock(&(d->mutex));
		wake_up_all(&(d->blockq));
	}

	return 0;
}


/*
 * osprd_lock
 */

/*
 * osprd_ioctl(inode, filp, cmd, arg)
 *   Called to perform an ioctl on the named file.
 */
int osprd_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	osprd_info_t *d = file2osprd(filp);	// device info
	int r = 0;			// return value: initially 0

	// is file open for writing?
	int filp_writable = (filp->f_mode & FMODE_WRITE) != 0;
	unsigned curTicket;
	struct process* newProc;
	struct process* tmp;
	unsigned long sector = 0; // User did not specify sector (for 
				  // OSPRDIOCNOTIFY), so default is 1st sector

	// This line avoids compiler warnings; you may remove it.
	(void) filp_writable, (void) d;

	// Set 'r' to the ioctl's return value: 0 on success, negative on error

	if (cmd == OSPRDIOCSECTOR) {
		
		if (d->notifProcs != NULL) {
			tmp = isInPidList(d->writeProcs, current->pid);
			if (tmp)
				tmp->sect = ((ssize_t) arg) / SECTOR_SIZE;
			else {
				newProc = kzalloc(sizeof(struct process), 
					GFP_ATOMIC);
				newProc->info = current;
				newProc->reqNotif = 0;
				newProc->sect = ((ssize_t) arg) / SECTOR_SIZE;
				addToPidList(&(d->writeNlkProcs), newProc);
			}
		}

	} else if (cmd == OSPRDIOCNOTIFY) {

		newProc = kzalloc(sizeof(struct process), GFP_ATOMIC);
		newProc->info = current;
		newProc->reqNotif = 1;
		addToPidList(&(d->notifProcs), newProc);

		if (filp_writable) {

                        if (arg != 0) // Assign sector that the user specified
				sector = arg - 1;
			/* Wait until another process has written to file. */
                        if (wait_event_interruptible(d->blockq, 
                                newProc->reqNotif == 0 && 
                                newProc->sectors[sector] == 1)) {

//				return -ERESTARTSYS;
                        }
			r = 0;
		}
		else { // Requested a read

			if (arg != 0)
				sector = arg - 1;
			if (wait_event_interruptible(d->blockq, 
				newProc->reqNotif == 0 &&
				newProc->sectors[sector] == 1)) {
                                
//				return -ERESTARTSYS;
                        }
                        r = 0;
		}

	} else if (cmd == OSPRDIOCACQUIRE) {

		// EXERCISE: Lock the ramdisk.
		//
		// If *filp is open for writing (filp_writable), then attempt
		// to write-lock the ramdisk; otherwise attempt to read-lock
		// the ramdisk.
		//
                // This lock request must block using 'd->blockq' until:
		// 1) no other process holds a write lock;
		// 2) either the request is for a read lock, or no other process
		//    holds a read lock; and
		// 3) lock requests should be serviced in order, so no process
		//    that blocked earlier is still blocked waiting for the
		//    lock.
		//
		// If a process acquires a lock, mark this fact by setting
		// 'filp->f_flags |= F_OSPRD_LOCKED'.  You also need to
		// keep track of how many read and write locks are held:
		// change the 'osprd_info_t' structure to do this.
		//
		// Also wake up processes waiting on 'd->blockq' as needed.
		//
		// If the lock request would cause a deadlock, return -EDEADLK.
		// If the lock request blocks and is awoken by a signal, then
		// return -ERESTARTSYS.
		// Otherwise, if we can grant the lock request, return 0.

		// 'd->ticket_head' and 'd->ticket_tail' should help you
		// service lock requests in order.  These implement a ticket
		// order: 'ticket_tail' is the next ticket, and 'ticket_head'
		// is the ticket currently being served.  You should set a local
		// variable to 'd->ticket_head' and increment 'd->ticket_head'.
		// Then, block at least until 'd->ticket_tail == local_ticket'.
		// (Some of these operations are in a critical section and must
		// be protected by a spinlock; which ones?)

		// Your code here (instead of the next two lines).
		if (filp_writable) { // Requested a write lock

			osp_spin_lock(&(d->mutex));
			/* Current process gets a ticket from ticket_head. */
			curTicket = d->ticket_head;
			d->ticket_head = d->ticket_head + 1;

			/* DEADLOCK: Requesting same lock that the process 
			 * already has OR there is a process that is reading.
			 */
			if (isInPidList(d->writeProcs, current->pid) || 
				isInPidList(d->readProcs, current->pid)) {
				wake_up_all(&(d->blockq));
				d->isHoldingOtherLocks = 0;
				incrementTicket(d);	
				osp_spin_unlock(&(d->mutex));
				return -EDEADLK;
			}

			/* DEADLOCK: Holding a lock in another device. */
			for_each_open_file(current, checkForOtherLocks, d);
			if (d->isHoldingOtherLocks) {

				wake_up_all(&(d->blockq));
                                d->isHoldingOtherLocks = 0;
                                incrementTicket(d);
                                osp_spin_unlock(&(d->mutex));
                                return -EDEADLK;
			}

			osp_spin_unlock(&(d->mutex));
			
			/* In order to get the write lock, it must be the 
			 * process's turn (so its ticket must match 
			 * ticket_tail) and no other process can be reading or
			 * writing. */
			if (wait_event_interruptible(d->blockq, 
				curTicket == d->ticket_tail &&
				d->readProcs == NULL && 
				d->writeProcs == NULL)) {
				/* Conditions were not met so add to 
				 * wait_queue_head_t with state marked as 
				 * TASK_INTERRUPTIBLE. */
			
				if (d->ticket_tail == curTicket)
					incrementTicket(d);
				else
					addToTicketList(&(d->exitedTickets),
						curTicket);
	
				return -ERESTARTSYS;
			}
			
			/* wait_event_interruptible returned 0 so the 
			 * conditions were met! This means the process has the 
			 * ticket to continue and no other process has a read 
			 * or write lock.*/
			osp_spin_lock(&(d->mutex));
			filp->f_flags |= F_OSPRD_LOCKED; // Claim the lock

			newProc = kzalloc(sizeof(struct process), GFP_ATOMIC);
			newProc->info = current;
			newProc->reqNotif = 0;
			addToPidList(&(d->writeProcs), newProc);
			incrementTicket(d);

			osp_spin_unlock(&(d->mutex));
			/* Wake up all processes in the wait queue that were
			 * put to sleep by wait_event_interruptible. */
			wake_up_all(&(d->blockq));
			return 0;
		}
		else { // Requested a read lock

			osp_spin_lock(&(d->mutex));
			/* Current process gets a ticket from ticket_head. */
			curTicket = d->ticket_head;
			d->ticket_head = d->ticket_head + 1;

			/* DEADLOCK: Requesting same lock that the process 
                         * already has OR there is a process that is writing.
                         */
                        if (isInPidList(d->readProcs, current->pid) ||
                                isInPidList(d->writeProcs, current->pid)) {

                                wake_up_all(&(d->blockq));
                                d->isHoldingOtherLocks = 0;
                                incrementTicket(d);
                                osp_spin_unlock(&(d->mutex));
                                return -EDEADLK;
                        }

                        /* DEADLOCK: Holding a lock in another device. */
                        for_each_open_file(current, checkForOtherLocks, d);
                        if (d->isHoldingOtherLocks) {

                                wake_up_all(&(d->blockq));
                                d->isHoldingOtherLocks = 0;
                                incrementTicket(d);
                                osp_spin_unlock(&(d->mutex));
                                return -EDEADLK;
                        }

			osp_spin_unlock(&(d->mutex));

			/* In order to get the read lock, it must be the 
                         * process's turn (so its ticket must match 
                         * ticket_tail) and no other process can be writing. */
			if (wait_event_interruptible(d->blockq, 
				curTicket == d->ticket_tail &&
				d->writeProcs == NULL)) {
				/* Conditions were not met so add to 
                                 * wait_queue_head_t with state marked as 
                                 * TASK_INTERRUPTIBLE. */
				if (curTicket == d->ticket_tail)
					incrementTicket(d);
				else
					addToTicketList(&(d->exitedTickets),
						curTicket);

				return -ERESTARTSYS;
			}

			/* wait_event_interruptible returned 0 so the 
                         * conditions were met! This means the process has the 
                         * ticket to continue and no other process has a write 
                         * lock.*/
			osp_spin_lock(&(d->mutex));
			filp->f_flags |= F_OSPRD_LOCKED; // Claim the lock

			newProc = kzalloc(sizeof(struct process), GFP_ATOMIC);
			newProc->info = current;
			newProc->reqNotif = 0;
			addToPidList(&(d->readProcs), newProc);
			incrementTicket(d);

			osp_spin_unlock(&(d->mutex));
			/* Wake up all processes in the wait queue that were
                         * put to sleep by wait_event_interruptible. */
			wake_up_all(&(d->blockq));
			r = 0;
		}
		
	} else if (cmd == OSPRDIOCTRYACQUIRE) {

		// EXERCISE: ATTEMPT to lock the ramdisk.
		//
		// This is just like OSPRDIOCACQUIRE, except it should never
		// block.  If OSPRDIOCACQUIRE would block or return deadlock,
		// OSPRDIOCTRYACQUIRE should return -EBUSY.
		// Otherwise, if we can grant the lock request, return 0.

		// Your code here (instead of the next two lines).
		
		/* Acquire the lock only if it's possible. */
		for_each_open_file(current, checkForOtherLocks, d);

		if (d->writeProcs == NULL && !d->isHoldingOtherLocks &&
			(!filp_writable | (d->readProcs == NULL))) {
			/* Acquired the lock successfully! */

			osp_spin_lock(&(d->mutex));
			/* Current process gets a ticket from ticket_head. */
			curTicket = d->ticket_head;
			d->ticket_head = d->ticket_head + 1;

			if (filp_writable) { // Requested a write lock
				
				filp->f_flags |= F_OSPRD_LOCKED;

				newProc = kzalloc(sizeof(struct process),
					GFP_ATOMIC);
				newProc->info = current;
				newProc->reqNotif = 0;
				addToPidList(&(d->writeProcs), newProc);
				incrementTicket(d);
			}
			else { // Requested a read lock

				filp->f_flags |= F_OSPRD_LOCKED;

				newProc = kzalloc(sizeof(struct process),
					GFP_ATOMIC);
				newProc->info = current;
				newProc->reqNotif = 0;
				addToPidList(&(d->readProcs), newProc);
				incrementTicket(d);
			}

			wake_up_all(&(d->blockq));
			r = 0;
			osp_spin_unlock(&(d->mutex));
		}
		else // Instead of blocking, mark as busy. 
			r = -EBUSY;

	} else if (cmd == OSPRDIOCRELEASE) {

		// EXERCISE: Unlock the ramdisk.
		//
		// If the file hasn't locked the ramdisk, return -EINVAL.
		// Otherwise, clear the lock from filp->f_flags, wake up
		// the wait queue, perform any additional accounting steps
		// you need, and return 0.

		// Your code here (instead of the next line).
		osp_spin_lock(&(d->mutex));
		
		if (isInPidList(d->writeProcs, current->pid))
			removeFromPidList(&(d->writeProcs), current->pid);
		if (isInPidList(d->readProcs, current->pid))
			removeFromPidList(&(d->readProcs), current->pid);
		if (isInPidList(d->notifProcs, current->pid))
                        removeFromPidList(&(d->notifProcs), current->pid);
		if (isInPidList(d->writeNlkProcs, current->pid))
			removeFromPidList(&(d->writeNlkProcs), current->pid);

		if (d->readProcs == NULL && d->writeProcs == NULL)
			filp->f_flags &= !F_OSPRD_LOCKED; // Clear the lock

		wake_up_all(&(d->blockq));
		r = 0;
		osp_spin_unlock(&(d->mutex));

	} else
		r = -ENOTTY; /* unknown command */
	return r;
}


// Initialize internal fields for an osprd_info_t.

static void osprd_setup(osprd_info_t *d)
{
	/* Initialize the wait queue. */
	init_waitqueue_head(&d->blockq);
	osp_spin_lock_init(&d->mutex);
	d->ticket_head = d->ticket_tail = 0;
	/* Add code here if you add fields to osprd_info_t. */
	d->readProcs = d->writeProcs = d->notifProcs = d->writeNlkProcs = NULL;
	d->exitedTickets = NULL;
	d->isHoldingOtherLocks = 0;
}


/*****************************************************************************/
/*         THERE IS NO NEED TO UNDERSTAND ANY CODE BELOW THIS LINE!          */
/*                                                                           */
/*****************************************************************************/

// Process a list of requests for a osprd_info_t.
// Calls osprd_process_request for each element of the queue.

static void osprd_process_request_queue(request_queue_t *q)
{
	osprd_info_t *d = (osprd_info_t *) q->queuedata;
	struct request *req;

	while ((req = elv_next_request(q)) != NULL)
		osprd_process_request(d, req);
}


// Some particularly horrible stuff to get around some Linux issues:
// the Linux block device interface doesn't let a block device find out
// which file has been closed.  We need this information.

static struct file_operations osprd_blk_fops;
static int (*blkdev_release)(struct inode *, struct file *);

static int _osprd_release(struct inode *inode, struct file *filp)
{
	if (file2osprd(filp))
		osprd_close_last(inode, filp);
	return (*blkdev_release)(inode, filp);
}

static int _osprd_open(struct inode *inode, struct file *filp)
{
	if (!osprd_blk_fops.open) {
		memcpy(&osprd_blk_fops, filp->f_op, sizeof(osprd_blk_fops));
		blkdev_release = osprd_blk_fops.release;
		osprd_blk_fops.release = _osprd_release;
	}
	filp->f_op = &osprd_blk_fops;
	return osprd_open(inode, filp);
}


// The device operations structure.

static struct block_device_operations osprd_ops = {
	.owner = THIS_MODULE,
	.open = _osprd_open,
	// .release = osprd_release, // we must call our own release
	.ioctl = osprd_ioctl
};


// Given an open file, check whether that file corresponds to an OSP ramdisk.
// If so, return a pointer to the ramdisk's osprd_info_t.
// If not, return NULL.

static osprd_info_t *file2osprd(struct file *filp)
{
	if (filp) {
		struct inode *ino = filp->f_dentry->d_inode;
		if (ino->i_bdev
		    && ino->i_bdev->bd_disk
		    && ino->i_bdev->bd_disk->major == OSPRD_MAJOR
		    && ino->i_bdev->bd_disk->fops == &osprd_ops)
			return (osprd_info_t *) ino->i_bdev->bd_disk->private_data;
	}
	return NULL;
}


// Call the function 'callback' with data 'user_data' for each of 'task's
// open files.

static void for_each_open_file(struct task_struct *task,
		  void (*callback)(struct file *filp, osprd_info_t *user_data),
		  osprd_info_t *user_data)
{
	int fd;
	task_lock(task);
	spin_lock(&task->files->file_lock);
	{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 13)
		struct files_struct *f = task->files;
#else
		struct fdtable *f = task->files->fdt;
#endif
		for (fd = 0; fd < f->max_fds; fd++)
			if (f->fd[fd])
				(*callback)(f->fd[fd], user_data);
	}
	spin_unlock(&task->files->file_lock);
	task_unlock(task);
}


// Destroy a osprd_info_t.

static void cleanup_device(osprd_info_t *d)
{
	wake_up_all(&d->blockq);
	if (d->gd) {
		del_gendisk(d->gd);
		put_disk(d->gd);
	}
	if (d->queue)
		blk_cleanup_queue(d->queue);
	if (d->data)
		vfree(d->data);
}


// Initialize a osprd_info_t.

static int setup_device(osprd_info_t *d, int which)
{
	memset(d, 0, sizeof(osprd_info_t));

	/* Get memory to store the actual block data. */
	if (!(d->data = vmalloc(nsectors * SECTOR_SIZE)))
		return -1;
	memset(d->data, 0, nsectors * SECTOR_SIZE);

	/* Set up the I/O queue. */
	spin_lock_init(&d->qlock);
	if (!(d->queue = blk_init_queue(osprd_process_request_queue, &d->qlock)))
		return -1;
	blk_queue_hardsect_size(d->queue, SECTOR_SIZE);
	d->queue->queuedata = d;

	/* The gendisk structure. */
	if (!(d->gd = alloc_disk(1)))
		return -1;
	d->gd->major = OSPRD_MAJOR;
	d->gd->first_minor = which;
	d->gd->fops = &osprd_ops;
	d->gd->queue = d->queue;
	d->gd->private_data = d;
	snprintf(d->gd->disk_name, 32, "osprd%c", which + 'a');
	set_capacity(d->gd, nsectors);
	add_disk(d->gd);

	/* Call the setup function. */
	osprd_setup(d);

	return 0;
}

static void osprd_exit(void);


// The kernel calls this function when the module is loaded.
// It initializes the 4 osprd block devices.

static int __init osprd_init(void)
{
	int i, r;

	// shut up the compiler
	(void) for_each_open_file;
#ifndef osp_spin_lock
	(void) osp_spin_lock;
	(void) osp_spin_unlock;
#endif

	/* Register the block device name. */
	if (register_blkdev(OSPRD_MAJOR, "osprd") < 0) {
		printk(KERN_WARNING "osprd: unable to get major number\n");
		return -EBUSY;
	}

	/* Initialize the device structures. */
	for (i = r = 0; i < NOSPRD; i++)
		if (setup_device(&osprds[i], i) < 0)
			r = -EINVAL;

	if (r < 0) {
		printk(KERN_EMERG "osprd: can't set up device structures\n");
		osprd_exit();
		return -EBUSY;
	} else
		return 0;
}


// The kernel calls this function to unload the osprd module.
// It destroys the osprd devices.

static void osprd_exit(void)
{
	int i;
	for (i = 0; i < NOSPRD; i++)
		cleanup_device(&osprds[i]);
	unregister_blkdev(OSPRD_MAJOR, "osprd");
}


// Tell Linux to call those functions at init and exit time.
module_init(osprd_init);
module_exit(osprd_exit);
