#include <stdlib.h>
#include <time.h>

#include "allocators.c"

#define ARENA_SIZE (1024 * 1024 * 128) // 128 MB Backing Buffer
#define MAX_ACTIVE_ALLOCS 20000        // Max concurrent allocations
#define TEST_ITERATIONS 2000000        // How many alloc/free cycles to run

typedef struct {
	void *ptr;
	size_t size;
	unsigned char expected_pattern;
} Allocation_Record;

// Generate a wildly varying size to maximize fragmentation
static size_t get_torture_size() {
	int r = rand() % 100;
	if (r < 15) return (rand() % 16) + 1;               // 15% chance: 1 to 16 bytes (extreme small)
	if (r < 80) return (rand() % 2048) + 17;            // 65% chance: 17 to 2064 bytes (standard)
	return (rand() % (1024 * 1024 * 2)) + 2049;         // 20% chance: Up to ~2MB (huge)
}

void run_allocator_torture_test() {
	printf("Starting Free List Torture Test...\n");

	// 1. Setup the backing buffer
	void *backing_memory = malloc(ARENA_SIZE);
	assert(backing_memory != NULL && "System out of memory");

	Free_List fl;
	free_list_init(&fl, backing_memory, ARENA_SIZE);
	fl.policy = Placement_Policy_Find_Best; // 'Best fit' resists fragmentation better for long loops

	// Record keeping
	Allocation_Record *active_records = malloc(sizeof(Allocation_Record) * MAX_ACTIVE_ALLOCS);
	int active_count = 0;
	size_t peak_used = 0;

	srand((unsigned int)time(NULL));

	// 2. The Torture Loop
	for (int i = 0; i < TEST_ITERATIONS; i++) {
		if (i % (TEST_ITERATIONS / 10) == 0) {
			printf("  Progress: %d%%\n", (i / (TEST_ITERATIONS / 100)));
		}

		// Coin flip: Allocate or Free? 
		// (Force alloc if empty, force free if full)
		int do_alloc = rand() % 2;
		if (active_count == 0) do_alloc = 1;
		if (active_count == MAX_ACTIVE_ALLOCS) do_alloc = 0;

		if (do_alloc) {
			size_t size = get_torture_size();
			size_t alignment = (rand() % 2 == 0) ? 8 : 16; // Mix up alignments

			void *ptr = free_list_alloc(&fl, size, alignment);
			if (ptr != NULL) {
				// Generate a random byte pattern to "paint" the memory
				unsigned char pattern = (unsigned char)(rand() & 0xFF);
				memset(ptr, pattern, size);

				// Track it
				active_records[active_count].ptr = ptr;
				active_records[active_count].size = size;
				active_records[active_count].expected_pattern = pattern;
				active_count++;

				if (fl.used > peak_used) peak_used = fl.used;
			}
		} else {
			// Pick a random active allocation to free
			int idx = rand() % active_count;
			Allocation_Record rec = active_records[idx];

			// DATA INTEGRITY CHECK: Verify no other allocation overwrote our memory
			unsigned char *mem = (unsigned char *)rec.ptr;
			for (size_t k = 0; k < rec.size; k++) {
				if (mem[k] != rec.expected_pattern) {
					printf("\n[FATAL] Memory Corruption Detected!\n");
					printf("Byte at offset %zu expected 0x%02X but was 0x%02X.\n", 
						   k, rec.expected_pattern, mem[k]);
					exit(1);
				}
			}

			// Free the memory
			free_list_free(&fl, rec.ptr);

			// Remove from tracking array (swap with last element)
			active_records[idx] = active_records[active_count - 1];
			active_count--;
		}
	}

	printf("\nLoop finished. Cleaning up remaining %d allocations...\n", active_count);

	// 3. Teardown
	for (int i = 0; i < active_count; i++) {
		free_list_free(&fl, active_records[i].ptr);
	}

	// 4. Ultimate Coalescence Verification
	// If the allocator perfectly coalesced, the head node should be the only node
	// and its size should be exactly fl.size.
	assert(fl.used == 0 && "Memory leak: fl.used is not 0 after freeing everything!");
	
	Free_List_Node *final_node = fl.head;
	assert(final_node != NULL && "List head is NULL, meaning list broke entirely!");
	assert(final_node->next == NULL && "List is still fragmented! Nodes didn't coalesce.");
	assert(final_node->block_size == fl.size && "Lost memory! Total block size doesn't match original arena size.");

	printf("==========================================\n");
	printf("Torture Test Passed!\n");
	printf("Peak Memory Used: %zu bytes (%.2f MB)\n", peak_used, (double)peak_used / (1024*1024));
	printf("Perfect memory coalescence achieved.\n");
	printf("==========================================\n");

	free(active_records);
	free(backing_memory);
}

int main() {
	run_allocator_torture_test();
	return 0;
}
