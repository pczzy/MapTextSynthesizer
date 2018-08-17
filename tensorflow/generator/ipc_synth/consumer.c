#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <signal.h>

#include "prod_cons.h"
#include "ipc_consumer.h"

//#define PRINT_DEBUG

// Necessary for signal handler
void* g_buff;

typedef struct chunk {
  uint64_t height;
  char label[MAX_WORD_LENGTH + 1];
  unsigned char img_data[MAX_IMAGE_SIZE]; //TODO change
} chunk_t;

sample_t* consume(intptr_t buff, uint64_t* consume_offset, int semid, char* have_buff_lock) {

  /* For wrapping -- test to see if producer wrapped */
  if(*consume_offset + BASE_CHUNK_SIZE >= SHM_SIZE
     || *(uint64_t*)(buff + *consume_offset) == NO_SPACE_TO_PRODUCE) {
    //printf("wrappin'\n");
    *consume_offset = START_BUFF_OFFSET;
  }

  /* Ensure this data chunk hasn't already been consumed */
  if(*(unsigned char*)(buff + *consume_offset)) {
#ifdef PRINT_DEBUG
    if(*(unsigned char*)(buff + *consume_offset) != 1) {
      fprintf(stderr, "Likely memory corruption. Offset @ %ld\n",
	      	      *consume_offset);
    }
    printf("Nothing to be consumed.\n");
#endif
    return NULL;
  }
  
  /* Ensure something has been produced (base offset and next 8 bytes are 0 */
  if(*consume_offset == START_BUFF_OFFSET
     && !(*(uint64_t*)(buff+*consume_offset + sizeof(char)))) {
    return NULL;
  }
  
  /* If consumer gets ahead of producer */
  if(!*(uint64_t*)(buff+*consume_offset)) {
#ifdef PRINT_DEBUG
    printf("Nothing to consume.\n");
#endif
    return NULL;
  }
  
  // Cache initial buff value
  intptr_t start_buff = buff;
  
  // Jump to the next available element (8 is to skip past `consumed`)
  buff += *consume_offset + sizeof(uint8_t);
  
  sample_t* spl = (sample_t*)malloc(sizeof(sample_t));
  if(spl == NULL) {
    perror("malloc");
    exit(1);
  }

  uint32_t height = *((uint32_t*)buff);
  buff += sizeof(uint32_t);

  char* label = strdup((char*)buff);
  if(label == NULL) {
    perror("strdup");
    exit(1);
  }

  buff += (MAX_WORD_LENGTH + 1)*sizeof(char);

  uint64_t sz = *((uint64_t*)buff);
  buff += sizeof(uint64_t);

  unsigned char* img_flat = (unsigned char*)malloc(sz);
  if(img_flat == NULL) {
    perror("malloc");
    fprintf(stderr, "Requested %lu bytes.\n", sz);
    exit(1);
  }

  // Copy image data into img_flat
  memcpy(img_flat, (void*)buff, sz);
  buff += sz*sizeof(unsigned char);

  spl->height = height;
  if(sz % height != 0) { fprintf(stderr, "invalid image dimensions.\n"); }
  spl->width = sz/height; //should be evenly divisible
  spl->caption = label;
  spl->img_data = img_flat;

  // Buff is now consumed!
  *(uint8_t*)(start_buff+*consume_offset) = (uint8_t)1;

  // Update consume_offset
  *consume_offset = buff - start_buff;

  // NOTE this is new and potentially buggy -- should be trying to prevent race
  if(*((uint64_t*)buff) < *consume_offset
     && *((uint64_t*)buff) + MAX_IMAGE_SIZE*50 >= *consume_offset
     && !*have_buff_lock) {
    printf("Sleeping producer until 50 more images are consumed...\n");
    //do this once
    lock_buff(semid);
    *have_buff_lock = 50;
  } else if(have_buff_lock){
    //do this only if locked by consumer
    *have_buff_lock--;
  } else {
    unlock_buff(semid);
    SEM_UNLOCK;
  }
  return spl;
  }

  
sample_t* ipc_get_sample(void* buff, uint64_t* consume_offset,
		 int semid, char* have_buff_lock) {
  
  sample_t* spl = consume((intptr_t)buff, consume_offset,
			  semid, have_buff_lock);
  
  //if this happens, then something broke
  if(*consume_offset >= SHM_SIZE) {
    fprintf(stderr, "Consumer did not wrap appropriately.\n");
    exit(1);
  }

  return spl;
}
/*
sample_t* ipc_get_sample(void* buff, uint64_t* consume_offset,
			 int semid, char* have_buff_lock) {
  return (sample_t*)get_sample(buff, consume_offset, semid, have_buff_lock);
}
*/
void print_sample_struct(sample_t* spl) {
  // Contains all of the raw data of a sample
  //printf("Image data: %s\n", spl->img_data); //NOTE this won't be a string later
  printf("Height: %ld\n", spl->height);
  printf("Caption: %s\n", spl->caption);
}

uint64_t consume_offset = START_BUFF_OFFSET;
void consumer_free_sample(sample_t* spl) {
  free(spl->img_data);
  free(spl->caption);
  free(spl);
}

void cleanup(int signo) {
  /* detach from segment */
  if(shmdt(g_buff) == -1) {
    perror("shmdt");
  }
  exit(1);
}

int mainless_consumer(void) {
  struct sigaction sa;
  sa.sa_handler = cleanup;
  sigaction(SIGTERM, NULL, &sa);
  
  g_buff = (void*)((intptr_t)get_shared_buff(0));
  int semid = get_semaphores(0);
  sample_t* spl;
  char have_buff_lock = 0;
  
  int counter = 0;
  while((spl = (sample_t*)ipc_get_sample(g_buff, &consume_offset, semid, &have_buff_lock)) || 1) {
    //#ifdef PRINT_DEBUG
    //#endif
    if(spl) {
      //printf("consume_offset: %ld\n", consume_offset);
      //print_sample_struct(spl);
      //printf("%d\n", counter);
      consumer_free_sample(spl);
      counter++;
    }
  }

  /* detach from segment */
  if(shmdt(g_buff) == -1) {
    perror("shmdt");
    exit(1);
  }
  
  printf("Retrieved %d elements.\n", counter);
  return 0;
}