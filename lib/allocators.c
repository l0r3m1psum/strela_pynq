// Code taken ans slightly adapted from gingerBill blog
// https://www.gingerbill.org/series/memory-allocation-strategies/
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

static bool
is_power_of_two(uintptr_t x) {
	return (x & (x-1)) == 0;
}

static uintptr_t
align_forward_uintptr(uintptr_t ptr, uintptr_t align) {
	uintptr_t a, p, modulo;

	assert(is_power_of_two(align));

	a = align;
	p = ptr;
	modulo = p & (a-1);
	if (modulo != 0) {
		p += a - modulo;
	}
	return p;
}

static size_t
align_forward_size(size_t ptr, size_t align) {
	size_t a, p, modulo;

	assert(is_power_of_two((uintptr_t)align));

	a = align;
	p = ptr;
	modulo = p & (a-1);
	if (modulo != 0) {
		p += a - modulo;
	}
	return p;
}

#ifndef DEFAULT_ALIGNMENT
#define DEFAULT_ALIGNMENT 8
#endif

typedef struct Pool_Free_Node Pool_Free_Node;
struct Pool_Free_Node {
	Pool_Free_Node *next;
};

typedef struct Pool Pool;
struct Pool {
	unsigned char *buf;
	size_t buf_len;
	size_t chunk_size;

	Pool_Free_Node *head;
};

static void pool_free_all(Pool *p);

// chunk_alignment is used to make the backing_buffer and the chunks start at a
// multiple of chunk_alignment.
static void
pool_init(Pool *p, void *backing_buffer, size_t backing_buffer_length,
               size_t chunk_size, size_t chunk_alignment) {
	// Align backing buffer to the specified chunk alignment
	uintptr_t initial_start = (uintptr_t)backing_buffer;
	uintptr_t start = align_forward_uintptr(initial_start, (uintptr_t)chunk_alignment);
	backing_buffer_length -= (size_t)(start-initial_start);

	// Align chunk size up to the required chunk_alignment
	chunk_size = align_forward_size(chunk_size, chunk_alignment);

	// Assert that the parameters passed are valid
	assert(chunk_size >= sizeof(Pool_Free_Node) &&
	       "Chunk size is too small");
	assert(backing_buffer_length >= chunk_size &&
	       "Backing buffer length is smaller than the chunk size");

	// Store the adjusted parameters
	p->buf = (unsigned char *)start;
	p->buf_len = backing_buffer_length;
	p->chunk_size = chunk_size;
	p->head = NULL; // Free List Head

	// Set up the free list for free chunks
	pool_free_all(p);
}

static void *
pool_alloc(Pool *p) {
	// Get latest free node
	Pool_Free_Node *node = p->head;

	if (node == NULL) {
		// assert(0 && "Pool allocator has no free memory");
		return NULL;
	}

	// Pop free node
	p->head = p->head->next;

	// Zero memory by default
	return memset(node, 0, p->chunk_size);
}

static void
pool_free(Pool *p, void *ptr) {
	Pool_Free_Node *node;

	void *start = p->buf;
	void *end = &p->buf[p->buf_len];

	if (ptr == NULL) {
		// Ignore NULL pointers
		return;
	}

	if (!(start <= ptr && ptr < end)) {
		assert(0 && "Memory is out of bounds of the buffer in this pool");
		return;
	}

	// Push free node
	node = (Pool_Free_Node *)ptr;
	node->next = p->head;
	p->head = node;
}

static void
pool_free_all(Pool *p) {
	size_t chunk_count = p->buf_len / p->chunk_size;
	size_t i;

	// Set all chunks to be free
	for (i = 0; i < chunk_count; i++) {
		void *ptr = &p->buf[i * p->chunk_size];
		Pool_Free_Node *node = (Pool_Free_Node *)ptr;
		// Push free node onto the free list
		node->next = p->head;
		p->head = node;
	}
}

#if 0
int main(int argc, char **argv) {
	int i;
	unsigned char backing_buffer[1024];
	Pool p;
	void *a, *b, *c, *d, *e, *f;

	pool_init(&p, backing_buffer, 1024, 64, DEFAULT_ALIGNMENT);

	a = pool_alloc(&p);
	b = pool_alloc(&p);
	c = pool_alloc(&p);
	d = pool_alloc(&p);
	e = pool_alloc(&p);
	f = pool_alloc(&p);

	pool_free(&p, f);
	pool_free(&p, c);
	pool_free(&p, b);
	pool_free(&p, d);

	d = pool_alloc(&p);

	pool_free(&p, a);

	a = pool_alloc(&p);

	pool_free(&p, e);
	pool_free(&p, a);
	pool_free(&p, d);

	return 0;
}
#endif

typedef struct Arena Arena;
struct Arena {
	unsigned char *buf;
	size_t         buf_len;
	size_t         prev_offset; // This will be useful for later on
	size_t         curr_offset;
};

static void
arena_init(Arena *a, void *backing_buffer, size_t backing_buffer_length) {
	a->buf = (unsigned char *)backing_buffer;
	a->buf_len = backing_buffer_length;
	a->curr_offset = 0;
	a->prev_offset = 0;
}


static void *
arena_alloc_align(Arena *a, size_t size, size_t align) {
	// Align 'curr_offset' forward to the specified alignment
	uintptr_t curr_ptr = (uintptr_t)a->buf + (uintptr_t)a->curr_offset;
	uintptr_t offset = align_forward_uintptr(curr_ptr, align);
	offset -= (uintptr_t)a->buf; // Change to relative offset

	// Check to see if the backing memory has space left
	if (offset+size <= a->buf_len) {
		void *ptr = &a->buf[offset];
		a->prev_offset = offset;
		a->curr_offset = offset+size;

		// Zero new memory by default
		memset(ptr, 0, size);
		return ptr;
	}
	// Return NULL if the arena is out of memory (or handle differently)
	return NULL;
}

// Because C doesn't have default parameters
static void *
arena_alloc(Arena *a, size_t size) {
	return arena_alloc_align(a, size, DEFAULT_ALIGNMENT);
}

static void
arena_free(Arena *a, void *ptr) {
	(void) a;
	(void) ptr;
	// Do nothing
}

static void *
arena_resize_align(Arena *a, void *old_memory, size_t old_size, size_t new_size, size_t align) {
	unsigned char *old_mem = (unsigned char *)old_memory;

	assert(is_power_of_two(align));

	if (old_mem == NULL || old_size == 0) {
		return arena_alloc_align(a, new_size, align);
	} else if (a->buf <= old_mem && old_mem < a->buf+a->buf_len) {
		if (a->buf+a->prev_offset == old_mem) {
			a->curr_offset = a->prev_offset + new_size;
			if (new_size > old_size) {
				// Zero the new memory by default
				memset(&a->buf[a->curr_offset], 0, new_size-old_size);
			}
			return old_memory;
		} else {
			void *new_memory = arena_alloc_align(a, new_size, align);
			size_t copy_size = old_size < new_size ? old_size : new_size;
			// Copy across old memory to the new memory
			memmove(new_memory, old_memory, copy_size);
			return new_memory;
		}

	} else {
		assert(0 && "Memory is out of bounds of the buffer in this arena");
		return NULL;
	}

}

// Because C doesn't have default parameters
static void *
arena_resize(Arena *a, void *old_memory, size_t old_size, size_t new_size) {
	return arena_resize_align(a, old_memory, old_size, new_size, DEFAULT_ALIGNMENT);
}

static void
arena_free_all(Arena *a) {
	a->curr_offset = 0;
	a->prev_offset = 0;
}

// Extra Features
typedef struct Temp_Arena_Memory Temp_Arena_Memory;
struct Temp_Arena_Memory {
	Arena *arena;
	size_t prev_offset;
	size_t curr_offset;
};

static Temp_Arena_Memory
temp_arena_memory_begin(Arena *a) {
	Temp_Arena_Memory temp;
	temp.arena = a;
	temp.prev_offset = a->prev_offset;
	temp.curr_offset = a->curr_offset;
	return temp;
}

static void
temp_arena_memory_end(Temp_Arena_Memory temp) {
	temp.arena->prev_offset = temp.prev_offset;
	temp.arena->curr_offset = temp.curr_offset;
}

#if 0
int main(int argc, char **argv) {
	int i;

	unsigned char backing_buffer[256];
	Arena a = {0};
	arena_init(&a, backing_buffer, 256);

	for (i = 0; i < 10; i++) {
		int *x;
		float *f;
		char *str;

		// Reset all arena offsets for each loop
		arena_free_all(&a);

		x = (int *)arena_alloc(&a, sizeof(int));
		f = (float *)arena_alloc(&a, sizeof(float));
		str = arena_alloc(&a, 10);

		*x = 123;
		*f = 987;
		memmove(str, "Hellope", 7);

		printf("%p: %d\n", x, *x);
		printf("%p: %f\n", f, *f);
		printf("%p: %s\n", str, str);

		str = arena_resize(&a, str, 10, 16);
		memmove(str+7, " world!", 7);
		printf("%p: %s\n", str, str);
	}

	arena_free_all(&a);

	return 0;
}
#endif
