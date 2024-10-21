#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/riscv.h"

int main() {
    int npages = 110;
    char *heappages = sbrk(PGSIZE * npages);
    if (heappages == (char *)-1) {
        printf("Heap memory allocation failed\n");
        exit(1);
    }

    printf("\nTesting [PID = %d]\n", getpid());

    // Write the value 5 to every byte of the allocated heap pages
    for (int i = 0; i < npages; i++) {
        char *page = heappages + i * PGSIZE;
        for (int j = 0; j < PGSIZE; j++) {
            page[j] = 5;
        }
    }

    // Assert
    for (int i = 0; i < npages; i++) {
        char *page = heappages + i * PGSIZE;
        for (int j = 0; j < PGSIZE; j++) {
            if (page[j] != 5) {
                printf("Heap assertion failed at page %d, byte %d\n", i, j);
                exit(1);
            }
        }
    }

    printf("\nWSA Test Passed [PID = %d]\n\n", getpid());
    exit(0);
}