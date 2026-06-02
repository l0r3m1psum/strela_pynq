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
	return (x != 0) && (x & (x - 1)) == 0;
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

	if (!((uintptr_t) start <= (uintptr_t) ptr && (uintptr_t) ptr < (uintptr_t) end)) {
		assert(0 && "Memory is out of bounds of the buffer in this pool");
		return;
	}

	uintptr_t offset = (uintptr_t)ptr - (uintptr_t)start;
	if (offset % p->chunk_size != 0) {
		assert(0 && "Pointer does not align to a valid pool chunk boundary");
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
			if (a->prev_offset + new_size > a->buf_len) {
				return NULL;
			}
			a->curr_offset = a->prev_offset + new_size;
			if (new_size > old_size) {
				// Zero the new memory by default
				memset(&a->buf[a->prev_offset + old_size], 0, new_size-old_size);
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

/* The free list allocator manages the memory area with an intrusive linked list
 * too keep track of available blocks and headers associated with every
 * allocation to record its size.
 */

typedef struct Free_List_Allocation_Header Free_List_Allocation_Header;
struct Free_List_Allocation_Header {
	// We could have another linked list to keep track of the occupied blocks...
	size_t orig_size;
	size_t block_size; // after this header
	size_t padding; // before this header
};

typedef struct Free_List_Node Free_List_Node;
struct Free_List_Node {
	Free_List_Node *next;
	size_t block_size;
};

typedef enum Placement_Policy Placement_Policy;
enum Placement_Policy {
	Placement_Policy_Find_First,
	Placement_Policy_Find_Best,
};

typedef struct Free_List Free_List;
struct Free_List {
	void            *data;
	size_t           size;
	size_t           used; // This can be used to avoid traversing the free list when allocating

	Free_List_Node  *head;
	Placement_Policy policy;
};

static void
free_list_free_all(Free_List *fl) {
	fl->used = 0;
	Free_List_Node *first_node = (Free_List_Node *)fl->data;
	first_node->block_size = fl->size;
	first_node->next = NULL;
	fl->head = first_node;
}

static void
free_list_init(Free_List *fl, void *data, size_t size) {
	uintptr_t start = align_forward_uintptr((uintptr_t)data, sizeof (max_align_t));
	size_t adjustment = start - (uintptr_t)data;

	assert(size + sizeof (Free_List_Node) > adjustment && "Buffer too small for alignment");

	fl->data = (void *)start;
	fl->size = size - adjustment;
	free_list_free_all(fl);
}

static size_t
calc_padding_with_header(uintptr_t ptr, uintptr_t alignment, size_t header_size) {
	uintptr_t p, a, modulo, padding, needed_space;

	assert(is_power_of_two(alignment));

	p = ptr;
	a = alignment;
	modulo = p & (a-1); // (p % a) as it assumes alignment is a power of two

	padding = 0;
	needed_space = 0;

	if (modulo != 0) { // Same logic as 'align_forward'
		padding = a - modulo;
	}

	needed_space = (uintptr_t)header_size;

	if (padding < needed_space) {
		needed_space -= padding;

		if ((needed_space & (a-1)) != 0) {
			padding += a * (1+(needed_space/a));
		} else {
			padding += a * (needed_space/a);
		}
	}

	return (size_t)padding;
}

static Free_List_Node *
free_list_find_first(Free_List *fl, size_t size, size_t alignment, size_t *padding_, Free_List_Node **prev_node_) {
	// Iterates the list and finds the first free block with enough space
	Free_List_Node *node = fl->head;
	Free_List_Node *prev_node = NULL;

	size_t padding = 0;

	while (node != NULL) {
		padding = calc_padding_with_header((uintptr_t)node, (uintptr_t)alignment, sizeof(Free_List_Allocation_Header));
		size_t required_space = size + padding;
		if (node->block_size >= required_space) {
			break;
		}
		prev_node = node;
		node = node->next;
	}
	if (padding_) *padding_ = padding;
	if (prev_node_) *prev_node_ = prev_node;
	return node;
}

static Free_List_Node *
free_list_find_best(Free_List *fl, size_t size, size_t alignment, size_t *padding_, Free_List_Node **prev_node_) {
	// This iterates the entire list to find the best fit
	// O(n)
	size_t smallest_diff = ~(size_t)0;

	Free_List_Node *node = fl->head;
	Free_List_Node *prev_node = NULL;
	Free_List_Node *best_node = NULL;

	Free_List_Node *best_prev_node = NULL;
	size_t best_padding = 0;

	size_t padding = 0;

	while (node != NULL) {
		padding = calc_padding_with_header((uintptr_t)node, (uintptr_t)alignment, sizeof(Free_List_Allocation_Header));
		size_t required_space = size + padding;
		if (node->block_size >= required_space && (node->block_size - required_space < smallest_diff)) {
			best_node = node;
			smallest_diff = node->block_size - required_space;
			best_padding = padding;
			best_prev_node = prev_node;
		}
		prev_node = node;
		node = node->next;
	}
	if (padding_) *padding_ = best_padding;
	if (prev_node_) *prev_node_ = best_prev_node;
	return best_node;
}

static void
free_list_node_insert(Free_List_Node **phead, Free_List_Node *prev_node, Free_List_Node *new_node) {
	if (prev_node == NULL) {
		assert(phead);
		new_node->next = *phead;
		*phead = new_node;
	} else {
		if (prev_node->next == NULL) {
			prev_node->next = new_node;
			new_node->next  = NULL;
		} else {
			new_node->next  = prev_node->next;
			prev_node->next = new_node;
		}
	}
}

static void
free_list_node_remove(Free_List_Node **phead, Free_List_Node *prev_node, Free_List_Node *del_node) {
	if (prev_node == NULL) {
		*phead = del_node->next;
	} else {
		prev_node->next = del_node->next;
	}
}

static void *
free_list_alloc(Free_List *fl, size_t size, size_t alignment) {
	size_t padding = 0;
	Free_List_Node *prev_node = NULL;
	Free_List_Node *node = NULL;
	size_t alignment_padding, required_space, remaining;
	Free_List_Allocation_Header *header_ptr;
	size_t orig_size = size;

	if (size < sizeof(Free_List_Node)) {
		size = sizeof(Free_List_Node);
	}
	if (alignment < sizeof (max_align_t)) {
		alignment = sizeof (max_align_t);
	}

	// This should be done by calc_padding_with_header
	size = align_forward_size(size, alignment);

	if (fl->policy == Placement_Policy_Find_Best) {
		node = free_list_find_best(fl, size, alignment, &padding, &prev_node);
	} else {
		node = free_list_find_first(fl, size, alignment, &padding, &prev_node);
	}
	if (node == NULL) {
		// assert(0 && "Free list has no free memory");
		return NULL;
	}

	alignment_padding = padding - sizeof(Free_List_Allocation_Header);
	required_space = size + padding;
	remaining = node->block_size - required_space;

	if (remaining >= sizeof (Free_List_Node) + sizeof (Free_List_Node)) {
		Free_List_Node *new_node = (Free_List_Node *)((char *)node + required_space);
		new_node->block_size = remaining;
		free_list_node_insert(&fl->head, node, new_node);
	} else {
		required_space = node->block_size;
	}

	free_list_node_remove(&fl->head, prev_node, node);

	header_ptr = (Free_List_Allocation_Header *)((char *)node + alignment_padding);
	header_ptr->orig_size = orig_size;
	header_ptr->block_size = required_space;
	header_ptr->padding = alignment_padding;

	fl->used += required_space;

	return (void *)((char *)header_ptr + sizeof(Free_List_Allocation_Header));
}

static void
free_list_coalescence(Free_List *fl, Free_List_Node *prev_node, Free_List_Node *free_node) {
	if (free_node->next != NULL && (void *)((char *)free_node + free_node->block_size) == free_node->next) {
		free_node->block_size += free_node->next->block_size;
		free_list_node_remove(&fl->head, free_node, free_node->next);
	}

	if (prev_node != NULL && (void *)((char *)prev_node + prev_node->block_size) == free_node) {
		prev_node->block_size += free_node->block_size;
		free_list_node_remove(&fl->head, prev_node, free_node);
	}
}

static void
free_list_free(Free_List *fl, void *ptr) {
	Free_List_Allocation_Header *header;
	Free_List_Node *free_node;
	Free_List_Node *node;
	Free_List_Node *prev_node = NULL;

	if (ptr == NULL) {
		return;
	}

	header = (Free_List_Allocation_Header *)((char *)ptr - sizeof(Free_List_Allocation_Header));
	free_node = (Free_List_Node *)((char *)header - header->padding);
	free_node->block_size = header->block_size;
	free_node->next = NULL;

	node = fl->head;
	while (node != NULL) {
		if (free_node < node) {
			free_list_node_insert(&fl->head, prev_node, free_node);
			break;
		}
		prev_node = node;
		node = node->next;
	}

	if (node == NULL) {
		free_list_node_insert(&fl->head, prev_node, free_node);
	}

	fl->used -= free_node->block_size;

	free_list_coalescence(fl, prev_node, free_node);
}
