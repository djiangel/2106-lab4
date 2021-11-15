#include "userswap.h"
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>

typedef struct page {
  pid_t pid;
  void *addr;
  int offset;
  int fd;
  bool isResident;
  bool isDirty;
  struct page *next;
} page;

typedef struct allocatedMem
{
  void *addr;
  int size;
  page *head;
  struct page *next;
} allocatedMem;

struct sigaction sa;
int pageSize = 4096;
int offset = 0;
allocatedMem *head = NULL;
allocatedMem *tail = NULL;

void registerHandler();
void pageFaultHandler(int sig, siginfo_t *siginfo, void *dont_care);
page *getPage(void *addr);

void userswap_set_size(size_t size) {

}

void *userswap_alloc(size_t size) {
  registerHandler();
  int numPages = (int) ceil(size/pageSize);
  int pageMem = numPages * pageSize;
  void *addr = mmap(NULL, pageMem, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  allocateMem(addr, pageMem);

  allocatedMem *curr = head;
  if (addr != NULL) {
    while (curr != NULL) {
      if (addr >= curr->addr && addr < (curr->addr+curr->size)) {
        break;
      } else {
        curr = curr->next;
      }
    }
    if (curr != NULL) {
      for (int i = 0; i < numPages; i++) {
        page *p = malloc(sizeof(page));
        p->pid = getpid();
        p->addr = addr+pageSize*i;
        p->offset = offset + (addr+pageSize*i) - addr;
        p->fd = -1;
        p->isResident = false;
        p->isDirty = false;
        p->next = NULL;
        page *currPage = curr->head;
        page *prev = NULL;
        if (currPage == NULL) {
          curr->head = p;
        } else {
          while (currPage != NULL) {
            prev = currPage;
            currPage = currPage->next;
          }
          prev->next = p;
        }
      }
    }
  }
  offset = offset + numPages * pageSize;
  return addr;
}

void userswap_free(void *mem) {
  if (mem == NULL) {
    return;
  }
  allocatedMem *curr = head;
  while (curr != NULL) {
    if (curr->addr == mem){
      break;
    } else {
      curr = curr->next;
    }
  }
  if (curr == NULL) {
    return;
  }
  page *currPage = curr->head;
  while (currPage != NULL) {
    munmap(currPage->addr, pageSize);
    page *p = currPage;
    currPage = currPage->next;
    free(p);
  }

  if (head == NULL) {
    return;
  } else {
    allocatedMem *h = head;
    allocatedMem *prev = NULL;
    while (h->addr != curr->addr) {
      if (h->next == NULL) {
        break;
      } else {
        prev = h;
        h = h->next;
      }
    }
    if (h == head) {
      head = head->next;
    } else {
      prev->next = h->next;
    }
    free(h);
    h = NULL;
  }
}

void *userswap_map(int fd, size_t size) {
  return NULL;
}

void registerHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sigaction));
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sa.sa_sigaction = pageFaultHandler;
  sigaction(SIGSEGV, &sa, NULL);
}

void pageFaultHandler(int sig, siginfo_t *siginfo, void *dont_care) {
  if (sig == SIGSEGV && siginfo != NULL) {
    void *addr = siginfo->si_addr;
    page *p = getPage(addr);
    mprotect(p->addr, pageSize, PROT_READ);
  }
}

page *getPage(void *addr) {
  allocatedMem *curr = head;
  page *p = NULL;
  while (curr != NULL) {
    if (addr >= curr->addr && addr < curr->addr+curr->size) {
      p = curr->head;
      break;
    } else {
      curr = curr->next;
    }
  }
  
  while (p != NULL) {

    if (addr >= p->addr && addr < p->addr + pageSize) {
      return p;
    } else {
      p = p->next;
    }
  }
  return NULL;
}

void allocateMem(void *addr, size_t size) {
  allocatedMem *mem = malloc(sizeof(allocatedMem));
  mem->addr = addr;
  mem->size = size;
  if (head == NULL) {
    head = mem;
    mem->next = NULL;
  } else {
    mem->next = head;
    head = mem;
  }
}