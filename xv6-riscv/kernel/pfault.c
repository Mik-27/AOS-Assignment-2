/* This file contains code for a generic page fault handler for processes. */
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz);
int flags2perm(int flags);

/* CSE 536: (2.4) read current time. */
uint64 read_current_timestamp() {
  uint64 curticks = 0;
  acquire(&tickslock);
  curticks = ticks;
  wakeup(&ticks);
  release(&tickslock);
  return curticks;
}

bool psa_tracker[PSASIZE];

/* All blocks are free during initialization. */
void init_psa_regions(void)
{
    for (int i = 0; i < PSASIZE; i++) 
        psa_tracker[i] = false;
}

/* FIFO ALGORITHM */
/* Evict heap page to disk when resident pages exceed limit */
void evict_page_to_disk(struct proc* p) {
    /* Find free block */
    int blockno = -1;
    for (int i=0; i < PSASIZE; i+=4) {
        if (psa_tracker[i] == false) {
            blockno = i;
            for(int k=blockno;k<blockno+4;k++)
            {
                psa_tracker[k]=true;
            }
            break;
        }
    }

    if(blockno == -1) {
        panic("Error: No PSA Block Available");
    }

    /* Find victim page using FIFO. */
    // int idx = -1;
    // uint64 earliestLoadTime =__INT64_MAX__;
    // for(int i=0; i < MAXHEAP; i++){
    //     if(p->heap_tracker[i].startblock == -1 && p->heap_tracker[i].last_load_time < earliestLoadTime){
    //         idx = i;
    //         earliestLoadTime = p->heap_tracker[i].last_load_time;
    //     }
    // }


    // int idx = -1;
    // for (int i = 0; i < MAXHEAP; i++) {
    //     if (p->heap_tracker[i].loaded == true && idx == -1) {
    //         // printf("for loop");
    //         idx = i;
    //     }
    //     else if (p->heap_tracker[i].loaded == true && p->heap_tracker[i].last_load_time < p->heap_tracker[idx].last_load_time) {
    //         // printf("for loop 2");
    //         idx = i;
    //     }
    //     else {}
    // }

    int idx = -1;
    uint64 least_recent_time = (uint64) -1; 
    const uint64 WORKING_SET_WINDOW = 50;

    for (int i = 0; i < MAXHEAP; i++) {
        if (p->heap_tracker[i].loaded) {
            uint64 last_used = p->heap_tracker[i].last_load_time;
            uint64 current_time = read_current_timestamp();

            if (current_time - last_used > WORKING_SET_WINDOW) {
                if (last_used < least_recent_time) {
                    least_recent_time = last_used;
                    idx = i;
                }
            }
        }
    }

    // If no page was found outside the working set, use FIFO
    if (idx == -1) {
        for (int i = 0; i < MAXHEAP; i++) {
            if (p->heap_tracker[i].loaded) {
                if (idx == -1 || 
                    p->heap_tracker[i].last_load_time < p->heap_tracker[idx].last_load_time) {
                    idx = i;
                }
            }
        }
    }
    // printf("%d", idx);

    /* Print statement. */
    print_evict_page(p->heap_tracker[idx].addr, blockno);

    p->heap_tracker[idx].startblock = blockno;
    p->heap_tracker[idx].loaded = false;

    /* Read memory from the user to kernel memory first. */
    char *kernel_copy_page = kalloc();
    copyin(p->pagetable, kernel_copy_page, p->heap_tracker[idx].addr, PGSIZE);
    
    /* Write to the disk blocks. Below is a template as to how this works. There is
     * definitely a better way but this works for now. :p */
    struct buf* b;
    for (int i=0; i < 4; i++) {
        b = bread(1, PSASTART+(blockno+i));
        // Copy page contents to b.data using memmove.
        memmove(b->data, kernel_copy_page + (i*BSIZE), BSIZE);
        bwrite(b);
        brelse(b);
        psa_tracker[blockno+i] = true;
    }

    /* Unmap swapped out page */
    uvmunmap(p->pagetable, p->heap_tracker[idx].addr, 1, 1);

    /* Update the resident heap tracker. */
    kfree(kernel_copy_page);
    p->resident_heap_pages--;
}


/* Retrieve faulted page from disk. */
void retrieve_page_from_disk(struct proc* p, uint64 uvaddr) {
    /* Find where the page is located in disk */
    int page_idx = -1;
    for (int i=0; i<MAXHEAP; i++) {
        if(p->heap_tracker[i].addr == uvaddr && p->heap_tracker[i].startblock != -1){
            page_idx = i;
            break;
        }
    }
    int blockno = p->heap_tracker[page_idx].startblock;
    
    if(page_idx == -1) {
        panic("Error: Issue Retrieving page from disk");
    }
    /* Print statement. */
    print_retrieve_page(uvaddr, blockno);

    /* Create a kernel page to read memory temporarily into first. */
    char *kernel_copy_page = kalloc();
    
    /* Read the disk block into temp kernel page. */
    struct buf* b;
    for (int i=0; i < 4; i++) {
        b = bread(1, PSASTART+(blockno + i));
        // Copy page contents to b.data using memmove.
        memmove(kernel_copy_page + (i*BSIZE), b->data, BSIZE);
        brelse(b);
        psa_tracker[blockno+i] = false;
    }

    /* Copy from temp kernel page to uvaddr (use copyout) */
    copyout(p->pagetable, uvaddr, kernel_copy_page, PGSIZE);

    kfree(kernel_copy_page);
}


void page_fault_handler(void) 
{
    /* Current process struct */
    struct proc *p = myproc();

    struct elfhdr elf;
    struct inode *ip;
    struct proghdr ph;


    /* Find faulting address. */
    uint64 faulting_addr = PGROUNDDOWN(r_stval());

    print_page_fault(p->name, faulting_addr);

    if((p->cow_enabled) && (r_scause() == 15)){
      copy_on_write();
      goto out;
    }

    /* Track whether the heap page should be brought back from disk or not. */
    bool load_from_disk = false;

    for (int i = 0; i < MAXHEAP; i++) {
        if (p->heap_tracker[i].addr == faulting_addr && p->heap_tracker[i].startblock != -1) {
            load_from_disk = true;
            break;
        }
    }

    /* Check if the fault address is a heap page. Use p->heap_tracker */
    bool isHeapPage = false;
    int index = -1;
    for (int i=0; i < MAXHEAP; i++) {
        if (p->heap_tracker[i].addr == faulting_addr) {
            index = i;
            isHeapPage = true;
            break;
            // if (p->heap_tracker[i].loaded == true){
            //     load_from_disk = true;
            // }
            // goto heap_handle;
        }
    }
    if (isHeapPage) {
        goto heap_handle;
    }

    /* Similar to exec.c */
    if((ip = namei(p->name)) == 0){
      return;
    }

    if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
        return;

    // Load program into memory.
    for(int i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
        if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
            return;
        if(ph.type != ELF_PROG_LOAD)
            continue;
        if(ph.memsz < ph.filesz)
            return;
        if(ph.vaddr + ph.memsz < ph.vaddr)
            return;
        if(ph.vaddr % PGSIZE != 0)
            return;
        if (ph.vaddr <= faulting_addr && faulting_addr < (ph.vaddr + ph.memsz)) {  
            break;
        }
    }
    uvmalloc(p->pagetable, faulting_addr, faulting_addr + ph.memsz, flags2perm(ph.flags));
    loadseg(p->pagetable, faulting_addr , ip, ph.off, ph.filesz);
    /* If it came here, it is a page from the program binary that we must load. */
    print_load_seg(faulting_addr, ph.off, ph.filesz);
    
    /* Go to out, since the remainder of this code is for the heap. */
    goto out;

heap_handle:
    // printf("Heap Handle");
    /* 2.4: Check if resident pages are more than heap pages. If yes, evict. */
    if (p->resident_heap_pages == MAXRESHEAP) {
        evict_page_to_disk(p);
    }

    /* 2.3: Map a heap page into the process' address space. (Hint: check growproc) */
    uint64 sz_heap;
    if((sz_heap = uvmalloc(p->pagetable, faulting_addr, faulting_addr + PGSIZE, PTE_W) < 0))
        panic("uvmalloc error");

    /* 2.4: Heap page was swapped to disk previously. We must load it from disk. */
    if (load_from_disk) {
        retrieve_page_from_disk(p, faulting_addr);
    }

    /* 2.4: Update the last load time for the loaded heap page in p->heap_tracker. */
    p->heap_tracker[index].last_load_time = read_current_timestamp();
    p->heap_tracker[index].loaded = true;

    /* Track that another heap page has been brought into memory. */
    p->resident_heap_pages++;

out:
    /* Flush stale page table entries. This is important to always do. */
    sfence_vma();
    return;
}