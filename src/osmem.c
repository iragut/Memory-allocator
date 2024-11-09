// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "block_meta.h"
#include "unistd.h"
#include "../utils/printf.h"

#define SIZE_OF_STRUCT sizeof(block_meta)
#define MMAP_THRESHOLD	(128 * 1024)
#define size_map (4096)

void *head_list;
int prealloc_heap;

void *alignment_memory(void *ptr)
{
	size_t padding = (8 - ((size_t)ptr % 8)) % 8;

	return (char *)ptr + padding;
}

block_meta *find_block_meta(void *ptr)
{
	block_meta *current = (block_meta *)head_list;
	block_meta *memory_block = (block_meta *)ptr - 1;

	while (current != NULL) {
		if (current->next == memory_block)
			return current;
		current = current->next;
	}
	return NULL;
}

block_meta *find_last_block(void)
{
	block_meta *current = (block_meta *)head_list;

	while (current->next != NULL)
		current = current->next;

	return current;
}

block_meta *find_free_block(size_t size)
{
	block_meta *current = (block_meta *)head_list;
	block_meta *best_fit = NULL;

	while (current != NULL) {
		if (current->status == STATUS_FREE && current->size >= size) {
			if (best_fit == NULL)
				best_fit = current;
			else if (current->size < best_fit->size)
				best_fit = current;
		}
		current = current->next;
	}

	return best_fit;
}


void split_block(block_meta *memory_block, size_t size)
{
	block_meta *new_block;
	size_t new_size = memory_block->size - size;

	if (new_size > SIZE_OF_STRUCT) {
		new_block = (block_meta *)((char *)memory_block + size);
		new_block->status = STATUS_FREE;

		new_block->size = new_size;
		new_block->next = memory_block->next;

		memory_block->next = new_block;
		memory_block->size = size;
	}
}

void coalesce(void)
{
	block_meta *current = (block_meta *)head_list;
	block_meta *next = current->next;

	while (next != NULL) {
		if (current->status == STATUS_FREE && next->status == STATUS_FREE) {
			current->size = current->size + next->size;
			current->next = next->next;
			next = current->next;
		} else {
			current = next;
			next = next->next;
		}
	}
}

block_meta *coalesce_realloc(block_meta *first, block_meta *second)
{
	first->size = first->size + second->size;
	first->next = second->next;
	return first;
}

block_meta *init_memory_and_block(size_t size, int counter)
{
	size_t max_counter_size;
	block_meta *memory_block, *last;
	void *memory_request;

	if (counter == 0)
		max_counter_size = MMAP_THRESHOLD;
	else
		max_counter_size = size_map;

	if (size < max_counter_size) {
		// Prealocarea
		if (prealloc_heap == 0) {
			memory_block = sbrk(0);
			memory_request = sbrk(MMAP_THRESHOLD);
			prealloc_heap = 1;
			memory_block->status = STATUS_ALLOC;
			memory_block->size = MMAP_THRESHOLD;
			memory_block = (block_meta *)memory_request;
			split_block(memory_block, size);

		// Alocare folosind brk
		} else {
			memory_request = sbrk(size);
			memory_block = (block_meta *)memory_request;
			memory_block->status = STATUS_ALLOC;
			memory_block->next = NULL;
			memory_block->size = size;
		}
	// Alocare folosind mmap
	} else {
		memory_request = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		memory_block = (block_meta *)memory_request;
		memory_block->status = STATUS_MAPPED;
		memory_block->next = NULL;
		memory_block->size = size;
	}

	// Creare head list
	if (head_list == NULL) {
		head_list = memory_block;
	} else {
		last = find_last_block();
		last->next = memory_block;
		memory_block->prev = memory_block;
	}

	return memory_block;
}

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;

	block_meta *memory_block;
	size_t increment;
	size_t total_size = size + SIZE_OF_STRUCT;

	total_size = total_size + ((8 - (total_size % 8)) % 8);

	if (head_list != NULL)
		coalesce();
	memory_block = find_free_block(total_size);
	if (memory_block != NULL) {
		split_block(memory_block, total_size);
		memory_block->status = STATUS_ALLOC;
	} else {
		// Expand last block
		if (head_list != NULL) {
			memory_block = find_last_block();
			if (memory_block->status == STATUS_FREE) {
				// Find the increment
				increment = total_size - memory_block->size;
				increment = increment + ((8 - (increment % 8)) % 8);
				sbrk(increment);
				memory_block->size = total_size;
				memory_block->status = STATUS_ALLOC;

				return (void *)alignment_memory(memory_block + 1);
			}
		}
		memory_block = init_memory_and_block(total_size, 0);
	}

	return (void *)alignment_memory(memory_block + 1);
}

void os_free(void *ptr)
{
	block_meta *memory_block, *prev;
	size_t len;

	if (ptr != NULL) {
		memory_block = (block_meta *)ptr - 1;
		if (memory_block->status == STATUS_MAPPED) {
			len = memory_block->size;
			prev = find_block_meta(ptr);
			if (prev != NULL)
				prev->next = memory_block->next;
			else
				head_list = memory_block->next;
			munmap((char *)ptr - SIZE_OF_STRUCT, len);
		} else {
			memory_block->status = STATUS_FREE;
		}
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;
	block_meta *memory_block;
	size_t increment;
	size_t total_size = nmemb * size + SIZE_OF_STRUCT;

	total_size = total_size + ((8 - (total_size % 8)) % 8);

	if (head_list != NULL)
		coalesce();

	memory_block = find_free_block(total_size);
	if (memory_block != NULL && size < size_map) {
		split_block(memory_block, total_size);
		memory_block->status = STATUS_ALLOC;
	} else {
		// Expand last block
		if (head_list != NULL && size < size_map) {
			memory_block = find_last_block();
			if (memory_block->status == STATUS_FREE) {
				// Find the increment
				increment = total_size - memory_block->size;
				increment = increment + ((8 - (increment % 8)) % 8);
				sbrk(increment);
				memory_block->size = total_size;
				memory_block->status = STATUS_ALLOC;

				memset(memory_block + 1, 0, total_size - SIZE_OF_STRUCT);
				return (void *)alignment_memory(memory_block + 1);
			}
		}
		memory_block = init_memory_and_block(total_size, 1);
	}
	memset(memory_block + 1, 0, total_size - SIZE_OF_STRUCT);
	return (void *)alignment_memory(memory_block + 1);
}

int find_minimum(int num1, int num2)
{
	if (num1 < num2)
		return num1;
	else
		return num2;
}

int is_last_block(block_meta *memory_block)
{
	if (memory_block == NULL)
		return 1;

	block_meta *current = memory_block->next;

	while (current != NULL) {
		if (current->status != STATUS_MAPPED)
			return 0;
		current = current->next;
	}
	return 1;
}

block_meta *find_last_block_free(void)
{
	block_meta *current = (block_meta *)head_list;
	block_meta *last = NULL;

	while (current != NULL) {
		if (current->status == STATUS_FREE)
			last = current;
		current = current->next;
	}

	if (is_last_block(last) == 1)
		return last;
	else
		return NULL;
}

void *os_realloc(void *ptr, size_t size)
{
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}
	if (ptr == NULL)
		return os_malloc(size);

	block_meta *new_memory_block;
	size_t alignment_size = size + SIZE_OF_STRUCT;
	block_meta *old_memory_block = (block_meta *)ptr - 1;

	alignment_size = alignment_size + ((8 - (alignment_size % 8)) % 8);

	if (old_memory_block->status == STATUS_FREE)
		return NULL;

	if (old_memory_block->size == alignment_size)
		return ptr;

	if (head_list != NULL)
		coalesce();
	// daca sa cerut mai putin decat are blocul si o fost alocat prin mmap
	if ((old_memory_block->size > alignment_size && old_memory_block->size > MMAP_THRESHOLD)) {
		new_memory_block = find_free_block(alignment_size);
		if (new_memory_block != NULL) {
			split_block(new_memory_block, alignment_size);
			new_memory_block->status = STATUS_ALLOC;
			memmove(new_memory_block + 1, ptr, size);
			os_free(ptr);
			// init new block
		} else {
			new_memory_block = init_memory_and_block(alignment_size, 0);
			memmove(new_memory_block + 1, ptr, size);
			os_free(ptr);
		}
	// daca sa cerut mai putin decat are blocul si o fost alocat prin brk
	} else if ((old_memory_block->size > alignment_size && old_memory_block->size < MMAP_THRESHOLD)
			&& old_memory_block->status == STATUS_ALLOC) {
		split_block(old_memory_block, alignment_size);
		old_memory_block->status = STATUS_ALLOC;
		new_memory_block = old_memory_block;

	// daca sa cerut mai mult decat are blocul si o fost alocat prin mmap
	} else if (old_memory_block->size < alignment_size && old_memory_block->size > MMAP_THRESHOLD) {
		new_memory_block = init_memory_and_block(alignment_size, 0);
		memmove(new_memory_block + 1, ptr, size);
		os_free(ptr);

	// daca sa cerut mai mult decat are blocul si o fost alocat prin brk
	} else if ((old_memory_block->size < alignment_size && old_memory_block->size < MMAP_THRESHOLD)) {
		new_memory_block = find_free_block(alignment_size);
		// expand last block
		if (old_memory_block->next == NULL && old_memory_block->status != STATUS_MAPPED) {
			sbrk(alignment_size - old_memory_block->size);
			old_memory_block->size = alignment_size;
			new_memory_block = old_memory_block;
		// coalesce with next block
		} else if (old_memory_block->next != NULL && old_memory_block->next->status == STATUS_FREE) {
			if (old_memory_block->size + old_memory_block->next->size >= alignment_size) {
				old_memory_block = coalesce_realloc(old_memory_block, old_memory_block->next);
				new_memory_block = old_memory_block;
			} else if (new_memory_block != NULL) {
				split_block(new_memory_block, alignment_size);
				new_memory_block->status = STATUS_ALLOC;
				memmove(new_memory_block + 1, ptr, old_memory_block->size);
			// init new block
			} else {
				new_memory_block = init_memory_and_block(alignment_size, 0);
				memmove(new_memory_block + 1, ptr, old_memory_block->size);
				os_free(ptr);
			}

		} else if (new_memory_block != NULL) {
			split_block(new_memory_block, alignment_size);
			new_memory_block->status = STATUS_ALLOC;
			memmove(new_memory_block + 1, ptr, find_minimum(old_memory_block->size, size));
			os_free(ptr);
		} else if (find_last_block_free() != NULL && alignment_size < MMAP_THRESHOLD) {
			new_memory_block = find_last_block_free();
			sbrk(alignment_size - new_memory_block->size);
			new_memory_block->status = STATUS_ALLOC;
			memmove(new_memory_block + 1, ptr, old_memory_block->size);
			os_free(ptr);
		} else {
			new_memory_block = init_memory_and_block(alignment_size, 0);
			memmove(new_memory_block + 1, ptr, find_minimum(old_memory_block->size, alignment_size));
			os_free(ptr);
		}
	} else if (old_memory_block->size > alignment_size && old_memory_block->size < MMAP_THRESHOLD
		&& old_memory_block->status == STATUS_MAPPED) {
		new_memory_block = find_free_block(alignment_size);
		if (new_memory_block != NULL) {
			split_block(new_memory_block, alignment_size);
			memmove(new_memory_block + 1, ptr, size);
			new_memory_block->status = STATUS_ALLOC;
			os_free(ptr);
		} else {
			new_memory_block = init_memory_and_block(alignment_size, 0);
			memmove(new_memory_block + 1, ptr, size);
			os_free(ptr);
		}
	}

	return (void *)alignment_memory(new_memory_block + 1);
}
