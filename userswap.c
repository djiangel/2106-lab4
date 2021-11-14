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
#include <fcntl.h>

// < ------------------------------ LINKED LIST DS ----------------------------------------------->
struct page
{
  pid_t processID;
  void *physicalAddress;
  bool isResident;
  bool isDirty;
  int offSet;
  int backingFile;
  struct page *next;
};

struct allocatedRegion
{
  void *physicalAddress;
  int allocatedRegionSize;
  struct page *firstPage;
  struct allocatedRegion *next;
};
int isRegisteredAlready = 0;
int totalMemInResident = 0;
int LORM = 8626176;
int currOffset = 0;
struct sigaction Action;
struct sigaction OldAction;
int swapFile;
struct allocatedRegion *head = NULL;
struct allocatedRegion *tail = NULL;

//insert page at the first location
void insertAllocatedRegion(void *physicalAddress, size_t allocatedRegionSize)
{
  //create a allocatedRegion
  struct allocatedRegion *link = (struct allocatedRegion *)malloc(sizeof(struct allocatedRegion));
  link->physicalAddress = physicalAddress;
  link->allocatedRegionSize = allocatedRegionSize;
  if (head == NULL)
  {
    // point the tail to the first guy in the link
    tail = link;
  }
  //point it to old first node
  link->next = head;
  //point first to new first node
  head = link;
}

//get a page with given key
struct allocatedRegion *getAllocatedRegion(void *physicalAddress, bool roundOff)
{
  //start from the first link
  if (physicalAddress == NULL)
  {
    return NULL;
  }
  struct allocatedRegion *current = head;
  //navigate through list
  if (roundOff)
  {
    while (current != NULL)
    {
      if (current->physicalAddress == physicalAddress)
      {
        return current;
      }
      else
      {
        //go to next link
        current = current->next;
      }
    }
  }
  else
  {
    while (current != NULL)
    {
      if (physicalAddress >= current->physicalAddress && physicalAddress < (current->physicalAddress + current->allocatedRegionSize))
      {
        return current;
      }
      else
      {
        //go to next link
        current = current->next;
      }
    }
  }
  return NULL;
}

void deleteAllocatedRegion(struct allocatedRegion *allocatedRegion)
{
  //start from the first link
  struct allocatedRegion *current = head;
  struct allocatedRegion *previous = NULL;

  //if list is empty
  if (head == NULL)
  {
    printf("Sorry but we can't find the node to delete");
    return;
  }

  //navigate through list to find the previous
  while (current->physicalAddress != allocatedRegion->physicalAddress)
  {
    //if it is last node
    if (current->next == NULL)
    {
      printf("Sorry but we can't find the node to delete");
      return;
    }
    else
    {
      //store reference to current link
      previous = current;
      //move to next link
      current = current->next;
    }
  }

  //found a match, update the link
  if (current == head)
  {
    //change first to point to next link
    head = head->next;
  }
  else
  {
    //bypass the current link
    previous->next = current->next;
  }
  free(current);
  current = NULL;
}

//get a page with given key
struct page *get(void *mem)
{
  if (mem == NULL)
  {
    return NULL;
  }
  // Navigate through the allocated regions first
  struct allocatedRegion *currAllocatedRegion = head;
  struct page *currentPage = NULL;
  while (currAllocatedRegion != NULL)
  {
    if (mem >= currAllocatedRegion->physicalAddress && mem < (currAllocatedRegion->physicalAddress + currAllocatedRegion->allocatedRegionSize))
    {
      currentPage = currAllocatedRegion->firstPage;
      break;
    }
    else
    {
      currAllocatedRegion = currAllocatedRegion->next;
    }
  }
  //then navigate through pages
  while (currentPage != NULL)
  {
    //if it is last page
    if (mem >= currentPage->physicalAddress && mem < (currentPage->physicalAddress + 4096))
    {
      return currentPage;
    }
    else
    {
      //go to next link
      currentPage = currentPage->next;
    }
  }
  return NULL;
}

//insert page at the first location
void insertPage(void *allocatedRegionMem, pid_t processID, void *physicalAddress, int backingFile)
{
  // find the allocatedRegion first
  struct allocatedRegion *allocatedRegion = getAllocatedRegion(allocatedRegionMem, false);
  if (allocatedRegion == NULL)
  {
    return;
  }
  //create a page
  struct page *link = (struct page *)malloc(sizeof(struct page));
  link->processID = processID;
  link->physicalAddress = physicalAddress;
  link->isResident = false;
  link->isDirty = false;
  link->offSet = currOffset + physicalAddress - allocatedRegionMem;
  link->backingFile = backingFile;
  link->next = NULL;

  // insert the page into the end of the allocatedRegion
  struct page *curr = allocatedRegion->firstPage;
  struct page *prev = NULL;
  while (curr != NULL)
  {
    prev = curr;
    curr = curr->next;
  }
  if (prev != NULL)
  {
    prev->next = link;
  }
  else
  {
    allocatedRegion->firstPage = link;
  }
}

// < ------------------------------ LINKED LIST DS END ----------------------------------------------->
// < ------------------------------ QUEUE DS ----------------------------------------------->
struct page *queueHead = NULL;

void enqueue(pid_t processID, void *physicalAddress)
{
  struct page *new_page = malloc(sizeof(struct page));
  if (!new_page)
    return;
  new_page->processID = processID;
  new_page->physicalAddress = physicalAddress;
  new_page->isResident = true;
  new_page->isDirty = false;
  new_page->next = queueHead;
  new_page->offSet = 0;
  queueHead = new_page;
}

struct page *dequeue()
{
  struct page *current, *prev = NULL;
  if (queueHead == NULL)
  {
    return NULL;
  }
  current = queueHead;
  while (current->next != NULL)
  {
    prev = current;
    current = current->next;
  }
  if (prev)
    prev->next = NULL;
  else
    queueHead = NULL;
  return current;
}
// < ------------------------------ QUEUE DS END ----------------------------------------------->

void evict(struct page *replacementPage)
{
  // 1. remove the oldest from queue
  struct page *oldestPageMirror = dequeue();
  struct page *oldestPage = get(oldestPageMirror->physicalAddress);
  free(oldestPageMirror);

  // 2. check if page is dirty
  if (oldestPage->isDirty)
  {
    int res;
    // writing contents at physicalAddress swap file
    if (oldestPage->backingFile == -1)
    {
      // write back to swapfile
      res = pwrite(swapFile, oldestPage->physicalAddress, 4096, oldestPage->offSet);
    }
    else
    {
      // write back to backingFile
      res = pwrite(oldestPage->backingFile, oldestPage->physicalAddress, 4096, oldestPage->offSet);
    }

    if (res < 0)
    {
      printf("Something went wrong with writing!");
      exit(1);
    }
    // don't change the dirty bit to false yet. Only change it when we are rehydrating
  }

  // 3. change to PROT_NONE and release the contents
  mprotect(oldestPage->physicalAddress, 4096, PROT_NONE);
  madvise(oldestPage->physicalAddress, 4096, MADV_DONTNEED);
  oldestPage->isResident = false;

  // 4. insert the replacement to queue and enable residency
  enqueue(replacementPage->processID, replacementPage->physicalAddress);
  mprotect(replacementPage->physicalAddress, 4096, PROT_READ);
  replacementPage->isResident = true;
  return;
}

void pageFaultHandler(int signum, siginfo_t *info, void *empty)
{
  if (signum == SIGSEGV && info != NULL)
  {
    void *mem = info->si_addr;
    struct page *allocatedPage = get(mem);
    if (allocatedPage == NULL)
    {
      // accessing out the CMR. Revert back to default handler and fail with segmentation fault.
      printf("really kena \n");
      sigaction(SIGSEGV, &OldAction, NULL);
      return;
    }

    if (allocatedPage->isResident == true)
    {
      // content is already in memory
      // implies that a write was attempted
      allocatedPage->isDirty = true;
      mprotect(allocatedPage->physicalAddress, 4096, PROT_READ | PROT_WRITE);
    }
    else
    {
      // content is in storage. Check if needs to be rehydrated.
      if (allocatedPage->isDirty == true)
      {
        int res;
        // re-hydrate memory
        mprotect(allocatedPage->physicalAddress, 4096, PROT_READ | PROT_WRITE);
        if (allocatedPage->backingFile == -1)
        {
          res = pread(swapFile, allocatedPage->physicalAddress, 4096, allocatedPage->offSet);
        }
        else
        {
          res = pread(allocatedPage->backingFile, allocatedPage->physicalAddress, 4096, allocatedPage->offSet);
        }
        mprotect(allocatedPage->physicalAddress, 4096, PROT_NONE);
        if (res < 0)
        {
          printf("Something went wrong when re-hydrating memory.\n");
          exit(1);
        }
        allocatedPage->isDirty = false;
      }

      if (totalMemInResident + 4096 < LORM)
      {
        // within the limit.
        totalMemInResident = totalMemInResident + 4096;
        allocatedPage->isResident = true;
        mprotect(allocatedPage->physicalAddress, 4096, PROT_READ);
        enqueue(allocatedPage->processID, allocatedPage->physicalAddress);
      }
      else
      {
        // exceeded limit, we need to evict page right now.
        evict(allocatedPage);
      }
    }
  }
}

void registerHandler()
{
  // Helps to register the handler and the Controlled Mem Region
  if (isRegisteredAlready == 1)
  {
    return;
  }
  memset(&Action, 0, sizeof(struct sigaction));
  sigemptyset(&Action.sa_mask);
  Action.sa_flags = SA_SIGINFO | SA_RESTART;
  Action.sa_sigaction = pageFaultHandler;
  sigaction(SIGSEGV, &Action, NULL);
  isRegisteredAlready = 1;
  char fileName[40];
  sprintf(fileName, "./%d.swap", getpid());
  swapFile = open(fileName, O_RDWR | O_TRUNC | O_CREAT, 0777);
  printf("Swap file create: %s \n", fileName);
}

void userswap_set_size(size_t size)
{
  int numberOfPagesAllocated = (size + (4096 - 1)) / 4096;
  int adjustedSize = numberOfPagesAllocated * 4096;
  LORM = adjustedSize;
}

void *userswap_alloc(size_t size)
{
  registerHandler();
  pid_t processID = getpid();
  // 1. Round up the requested size to nearest page
  int numberOfPagesAllocated = (size + (4096 - 1)) / 4096;

  // 2. allocate memory using mmap. All content initialized to zero with MAP_ANONYMOUS flag
  void *physicalAddress = mmap(NULL, numberOfPagesAllocated * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  // 4. Insert a allocated Region
  insertAllocatedRegion(physicalAddress, numberOfPagesAllocated * 4096);

  // 5. generate and insert pages into allocated region
  for (int i = 0; i < numberOfPagesAllocated; i++)
  {
    insertPage(physicalAddress, processID, physicalAddress + 4096 * i, -1);
  }
  currOffset = currOffset + numberOfPagesAllocated * 4096;
  return physicalAddress;
}

void userswap_free(void *mem)
{
  // 1. find the allocated page
  if (mem == NULL)
  {
    return;
  }
  struct allocatedRegion *allocatedRegion = getAllocatedRegion(mem, true);
  // 2. free the entire page allocation using munmap
  if (allocatedRegion == NULL)
  {
    return;
  }
  struct page *currPage = allocatedRegion->firstPage;
  while (currPage != NULL)
  {
    totalMemInResident = totalMemInResident - 4096;
    if (currPage->isDirty && currPage->backingFile != -1 && currPage->isResident)
    {
      // dirty and has backing file
      mprotect(currPage->physicalAddress, 4096, PROT_READ);
      int res = pwrite(currPage->backingFile, currPage->physicalAddress, 4096, currPage->offSet);
      if (res < 0)
      {
        printf("Something bad happed when cleaning \n");
        exit(1);
      }
      mprotect(currPage->physicalAddress, 4096, PROT_NONE);
    }
    munmap(currPage->physicalAddress, 4096);
    struct page *pageToFree = currPage;
    currPage = currPage->next;
    free(pageToFree);
  }
  // 3. Free the allocatedRegion and delete it;
  deleteAllocatedRegion(allocatedRegion);
}

void *userswap_map(int fd, size_t size)
{
  registerHandler();
  pid_t processID = getpid();
  // 1. Round up the requested size to nearest page
  int numberOfPagesAllocated = (size + (4096 - 1)) / 4096;

  // 2. allocate memory using mmap. All content initialized to zero with MAP_ANONYMOUS flag
  void *physicalAddress = mmap(NULL, numberOfPagesAllocated * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  // 3. read the file fd into the allocated mem
  int res = pread(fd, physicalAddress, numberOfPagesAllocated * 4096, 0);
  if (res < 0)
  {
    printf("Something bad happened when reading from given fd \n");
    exit(1);
  }
  mprotect(physicalAddress, numberOfPagesAllocated * 4096, PROT_NONE);

  // 4. Insert a allocated Region
  insertAllocatedRegion(physicalAddress, numberOfPagesAllocated * 4096);

  // 5. generate and insert pages into allocated region
  for (int i = 0; i < numberOfPagesAllocated; i++)
  {
    insertPage(physicalAddress, processID, physicalAddress + 4096 * i, fd);
  }
  currOffset = currOffset + numberOfPagesAllocated * 4096;
  return physicalAddress;
}
