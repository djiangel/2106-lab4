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
#include <fcntl.h>

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
  struct allocatedMem *next;
} allocatedMem;

struct sigaction sa;
struct sigaction prevsa;
int swapFile;
int pageSize = 4096;
int offset = 0;
int totalMem = 0;
int LORM = 8626176;
allocatedMem *head = NULL;
allocatedMem *tail = NULL;
page *pageHead = NULL;

void registerHandler();
void handler(int sig, siginfo_t *siginfo, void *dont_care);
page *getPage(void *addr);
void allocateMem(void *addr, size_t size);

void userswap_set_size(size_t size) {
  int numPages = (int) ceil(size/pageSize);
  LORM = numPages * pageSize;
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
    if (currPage->isDirty && currPage->fd != -1 && currPage->isResident) {
      mprotect(currPage->addr, pageSize, PROT_READ | PROT_WRITE);
      int code;
      code = pwrite(currPage->fd, currPage->addr, pageSize, currPage->offset);
      if (code == -1) {
        exit(0);
      }
      mprotect(currPage->addr, pageSize, PROT_NONE);
    }
    totalMem -= pageSize;
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
  registerHandler();
  int numPages = (int) ceil(size/pageSize);
  int pageMem = numPages * pageSize;
  void *addr = mmap(NULL, pageMem, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int code;
  code = pread(fd, addr, pageMem, 0);
  if (code == -1) {
    exit(0);
  }
  mprotect(addr, pageMem, PROT_NONE);
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
        p->fd = fd;
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

void registerHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sigaction));
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sa.sa_sigaction = handler;
  sigaction(SIGSEGV, &sa, NULL);
  char fileName[40];
  sprintf(fileName, "./%d.swap", getpid());
  swapFile = open(fileName, O_RDWR | O_TRUNC | O_CREAT, 0777);
}

void handler(int sig, siginfo_t *siginfo, void *dont_care) {
  if (sig == SIGSEGV && siginfo != NULL) {
    void *addr = siginfo->si_addr;
    page *p = getPage(addr);
    if (p == NULL) {
      sigaction(SIGSEGV, &prevsa, NULL);
      return;
    }
    if (p->isResident) {
      mprotect(p->addr, pageSize, PROT_READ | PROT_WRITE);
      p->isDirty = true;
    } else {
      if (p->isDirty) {
        mprotect(p->addr, pageSize, PROT_READ | PROT_WRITE);
        int code;
        if (p->fd == -1) {
          code = pread(swapFile, p->addr, pageSize, p->offset);
        } else {
          code = pread(p->fd, p->addr, pageSize, p->offset);
        }
        if (code == -1) {
          exit(0);
        }
        mprotect(p->addr, pageSize, PROT_NONE);
        p->isDirty = false;
      }
      if (totalMem+pageSize < LORM) {
        totalMem += pageSize;
        p->isResident = true;
        mprotect(p->addr, pageSize, PROT_READ);
        page *queuePage = malloc(sizeof(page));
        queuePage->addr = p->addr;
        queuePage->pid = p->pid;
        queuePage->offset = 0;
        queuePage->fd = -1;
        queuePage->isResident = true;
        queuePage->isDirty = false;
        queuePage->next = pageHead;
        pageHead = queuePage;
      } else {
        page *curr = pageHead;
        page *prev = NULL;
        while (curr->next != NULL) {
          prev = curr;
          curr = curr->next;
        }
        if (prev != NULL) {
          prev->next = NULL;
        } else {
          pageHead = NULL;
        }
        page *evictPage = getPage(curr->addr);
        free(curr);
        curr = NULL;
        if (evictPage->isDirty) {
          int code;
          if (evictPage->fd == -1) {
            code = pwrite(swapFile, evictPage->addr, pageSize, evictPage->offset);
          } else {
            code = pwrite(evictPage->fd, evictPage->addr, pageSize, evictPage->offset);
          }
          if (code == -1) {
            exit(0);
          }
          madvise(evictPage->addr, pageSize, MADV_DONTNEED);
        }
        mprotect(evictPage->addr, pageSize, PROT_NONE);
        evictPage->isResident = false;
        page *queuePage = malloc(sizeof(page));
        queuePage->addr = p->addr;
        queuePage->pid = p->pid;
        queuePage->offset = 0;
        queuePage->fd = -1;
        queuePage->isResident = true;
        queuePage->isDirty = false;
        queuePage->next = pageHead;
        pageHead = queuePage;
        p->isResident = true;
        mprotect(p->addr, pageSize, PROT_READ);
      }
    }
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