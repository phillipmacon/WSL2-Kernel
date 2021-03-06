// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2019, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@microsoft.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 */

/*
 * Dxgkrnl Graphics Port Driver handle manager
 *
 */

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>

#include "misc.h"
#include "dxgkrnl.h"
#include "hmgr.h"

/*
 * Handle parameters
 */
#define HMGRHANDLE_INSTANCE_BITS	6
#define HMGRHANDLE_INDEX_BITS		24
#define HMGRHANDLE_UNIQUE_BITS		2

#define HMGRHANDLE_INSTANCE_SHIFT	0
#define HMGRHANDLE_INDEX_SHIFT	\
	(HMGRHANDLE_INSTANCE_BITS + HMGRHANDLE_INSTANCE_SHIFT)
#define HMGRHANDLE_UNIQUE_SHIFT	\
	(HMGRHANDLE_INDEX_BITS + HMGRHANDLE_INDEX_SHIFT)

#define HMGRHANDLE_INSTANCE_MASK \
	(((1 << HMGRHANDLE_INSTANCE_BITS) - 1) << HMGRHANDLE_INSTANCE_SHIFT)
#define HMGRHANDLE_INDEX_MASK      \
	(((1 << HMGRHANDLE_INDEX_BITS)    - 1) << HMGRHANDLE_INDEX_SHIFT)
#define HMGRHANDLE_UNIQUE_MASK     \
	(((1 << HMGRHANDLE_UNIQUE_BITS)   - 1) << HMGRHANDLE_UNIQUE_SHIFT)

#define HMGRHANDLE_INSTANCE_MAX	((1 << HMGRHANDLE_INSTANCE_BITS) - 1)
#define HMGRHANDLE_INDEX_MAX	((1 << HMGRHANDLE_INDEX_BITS) - 1)
#define HMGRHANDLE_UNIQUE_MAX	((1 << HMGRHANDLE_UNIQUE_BITS) - 1)

/*
 * Handle entry
 */
struct hmgrentry {
	union {
		void *object;
		struct {
			uint prev_free_index;
			uint next_free_index;
		};
	};
	uint type:HMGRENTRY_TYPE_BITS + 1;
	uint unique:HMGRHANDLE_UNIQUE_BITS;
	uint instance:HMGRHANDLE_INSTANCE_BITS;
	uint destroyed:1;
};

#define HMGRTABLE_SIZE_INCREMENT	1024
#define HMGRTABLE_MIN_FREE_ENTRIES 128
#define HMGRTABLE_INVALID_INDEX (~((1 << HMGRHANDLE_INDEX_BITS) - 1))
#define HMGRTABLE_SIZE_MAX		0xFFFFFFF

static uint table_size_increment = HMGRTABLE_SIZE_INCREMENT;

static inline uint get_unique(d3dkmt_handle h)
{
	return (h & HMGRHANDLE_UNIQUE_MASK) >> HMGRHANDLE_UNIQUE_SHIFT;
}

static uint get_index(d3dkmt_handle h)
{
	return (h & HMGRHANDLE_INDEX_MASK) >> HMGRHANDLE_INDEX_SHIFT;
}

uint get_instance(d3dkmt_handle h)
{
	return (h & HMGRHANDLE_INSTANCE_MASK) >> HMGRHANDLE_INSTANCE_SHIFT;
}

static bool is_handle_valid(struct hmgrtable *table, d3dkmt_handle h,
			    bool ignore_destroyed, enum hmgrentry_type t)
{
	uint index = get_index(h);
	uint unique = get_unique(h);
	struct hmgrentry *entry;

	if (index >= table->table_size) {
		pr_err("%s Invalid index %x %d\n", __func__, h, index);
		return false;
	}

	entry = &table->entry_table[index];
	if (unique != entry->unique) {
		pr_err("%s Invalid unique %x %d %d %d %p",
			   __func__, h, unique, entry->unique,
			   index, entry->object);
		return false;
	}

	if (entry->destroyed && !ignore_destroyed) {
		pr_err("%s Invalid destroyed", __func__);
		return false;
	}

	if (entry->type == HMGRENTRY_TYPE_FREE) {
		pr_err("%s Entry is freed %x %d", __func__, h, index);
		return false;
	}

	if (t != HMGRENTRY_TYPE_FREE && t != entry->type) {
		pr_err("%s type mismatch %x %d %d", __func__, h,
			   t, entry->type);
		return false;
	}

	return true;
}

static d3dkmt_handle build_handle(uint index, uint unique, uint instance)
{
	uint handle_bits;

	handle_bits = (index << HMGRHANDLE_INDEX_SHIFT) & HMGRHANDLE_INDEX_MASK;
	handle_bits |= (unique << HMGRHANDLE_UNIQUE_SHIFT) &
	    HMGRHANDLE_UNIQUE_MASK;
	handle_bits |= (instance << HMGRHANDLE_INSTANCE_SHIFT) &
	    HMGRHANDLE_INSTANCE_MASK;

	return (d3dkmt_handle) handle_bits;
}

inline uint hmgrtable_get_used_entry_count(struct hmgrtable *table)
{
	DXGKRNL_ASSERT(table->table_size >= table->free_count);
	return (table->table_size - table->free_count);
}

bool hmgrtable_mark_destroyed(struct hmgrtable *table, d3dkmt_handle h)
{
	if (!is_handle_valid(table, h, false, HMGRENTRY_TYPE_FREE))
		return false;

	table->entry_table[get_index(h)].destroyed = true;
	return true;
}

bool hmgrtable_unmark_destroyed(struct hmgrtable *table, d3dkmt_handle h)
{
	if (!is_handle_valid(table, h, true, HMGRENTRY_TYPE_FREE))
		return true;

	DXGKRNL_ASSERT(table->entry_table[get_index(h)].destroyed);
	table->entry_table[get_index(h)].destroyed = 0;
	return true;
}

static inline bool is_empty(struct hmgrtable *table)
{
	return (table->free_count == table->table_size);
}

void print_status(struct hmgrtable *table)
{
	int i;

	TRACE_DEBUG(1, "hmgrtable head, tail %p %d %d\n",
		    table, table->free_handle_list_head,
		    table->free_handle_list_tail);
	if (table->entry_table == NULL)
		return;
	for (i = 0; i < 3; i++) {
		if (table->entry_table[i].type != HMGRENTRY_TYPE_FREE)
			TRACE_DEBUG(1, "hmgrtable entry %p %d %p\n",
				    table, i, table->entry_table[i].object);
		else
			TRACE_DEBUG(1, "hmgrtable entry %p %d %d %d\n",
				    table, i,
				    table->entry_table[i].next_free_index,
				    table->entry_table[i].prev_free_index);
	}
}

static bool expand_table(struct hmgrtable *table, uint NumEntries)
{
	uint new_table_size;
	struct hmgrentry *new_entry;
	uint table_index;
	uint new_free_count;
	uint prev_free_index;
	uint tail_index = table->free_handle_list_tail;

	TRACE_DEBUG(1, "%s\n", __func__);

	/* The tail should point to the last free element in the list */
	if (!(table->free_count == 0 ||
	      table->entry_table[tail_index].next_free_index ==
	      HMGRTABLE_INVALID_INDEX)) {
		pr_err("%s:corruption\n", __func__);
		return false;
	}

	new_table_size = table->table_size + table_size_increment;
	if (new_table_size < NumEntries)
		new_table_size = NumEntries;

	if (new_table_size > HMGRHANDLE_INDEX_MAX) {
		pr_err("%s:corruption\n", __func__);
		return false;
	}

	new_entry = (struct hmgrentry *)
	    dxgmem_alloc(table->process, DXGMEM_HANDLE_TABLE,
			 new_table_size * sizeof(struct hmgrentry));
	if (new_entry == NULL) {
		pr_err("%s:allocation failed\n", __func__);
		return false;
	}

	if (table->entry_table) {
		memcpy(new_entry, table->entry_table,
		       table->table_size * sizeof(struct hmgrentry));
		dxgmem_free(table->process, DXGMEM_HANDLE_TABLE,
			    table->entry_table);
	} else {
		table->free_handle_list_head = 0;
	}

	table->entry_table = new_entry;

	/* Initialize new table entries and add to the free list */
	table_index = table->table_size;
	new_free_count = table->free_count + table_size_increment;

	prev_free_index = table->free_handle_list_tail;

	while (table_index < new_table_size) {
		struct hmgrentry *entry = &table->entry_table[table_index];

		entry->prev_free_index = prev_free_index;
		entry->next_free_index = table_index + 1;
		entry->type = HMGRENTRY_TYPE_FREE;
		entry->unique = 1;
		entry->instance = 0;
		prev_free_index = table_index;

		table_index++;
	}

	table->entry_table[table_index - 1].next_free_index =
	    (uint) HMGRTABLE_INVALID_INDEX;

	if (table->free_count != 0) {
		/* Link the current free list with the new entries */
		struct hmgrentry *entry;

		entry = &table->entry_table[table->free_handle_list_tail];
		entry->next_free_index = table->table_size;
	}
	table->free_handle_list_tail = new_table_size - 1;
	if (table->free_handle_list_head == HMGRTABLE_INVALID_INDEX)
		table->free_handle_list_head = table->table_size;

	table->table_size = new_table_size;
	table->free_count = new_free_count;

	TRACE_DEBUG(1, "%s end\n", __func__);
	return true;
}

void hmgrtable_init(struct hmgrtable *table, struct dxgprocess *process)
{
	table->process = process;
	table->entry_table = NULL;
	table->table_size = 0;
	table->free_handle_list_head = HMGRTABLE_INVALID_INDEX;
	table->free_handle_list_tail = HMGRTABLE_INVALID_INDEX;
	table->free_count = 0;
	init_rwsem(&table->table_lock);
}

void hmgrtable_destroy(struct hmgrtable *table)
{
	if (table->entry_table) {
		dxgmem_free(table->process, DXGMEM_HANDLE_TABLE,
			    table->entry_table);
		table->entry_table = NULL;
	}
}

void hmgrtable_lock(struct hmgrtable *table, enum dxglockstate state)
{
	dxglockorder_acquire(DXGLOCK_HANDLETABLE);
	if (state == DXGLOCK_EXCL)
		down_write(&table->table_lock);
	else
		down_read(&table->table_lock);
}

void hmgrtable_unlock(struct hmgrtable *table, enum dxglockstate state)
{
	if (state == DXGLOCK_EXCL)
		up_write(&table->table_lock);
	else
		up_read(&table->table_lock);
	dxglockorder_release(DXGLOCK_HANDLETABLE);
}

d3dkmt_handle hmgrtable_alloc_handle(struct hmgrtable *table, void *object,
				     enum hmgrentry_type type, bool make_valid)
{
	uint index;
	struct hmgrentry *entry;
	uint unique;

	DXGKRNL_ASSERT(type <= HMGRENTRY_TYPE_LIMIT);
	DXGKRNL_ASSERT(type > HMGRENTRY_TYPE_FREE);

	if (table->free_count <= HMGRTABLE_MIN_FREE_ENTRIES) {
		if (!expand_table(table, 0)) {
			pr_err("hmgrtable expand_table failed\n");
			return 0;
		}
	}

	if (table->free_handle_list_head >= table->table_size) {
		pr_err("hmgrtable corrupted handle table head\n");
		return 0;
	}

	index = table->free_handle_list_head;
	entry = &table->entry_table[index];

	if (entry->type != HMGRENTRY_TYPE_FREE) {
		pr_err("hmgrtable expected free handle\n");
		return 0;
	}

	table->free_handle_list_head = entry->next_free_index;

	if (entry->next_free_index != table->free_handle_list_tail) {
		if (entry->next_free_index >= table->table_size) {
			pr_err("hmgrtable invalid next free index\n");
			return 0;
		}
		table->entry_table[entry->next_free_index].prev_free_index =
		    HMGRTABLE_INVALID_INDEX;
	}

	unique = table->entry_table[index].unique;

	table->entry_table[index].object = object;
	table->entry_table[index].type = type;
	table->entry_table[index].instance = 0;
	table->entry_table[index].destroyed = !make_valid;
	table->free_count--;

	return build_handle(index, unique, table->entry_table[index].instance);
}

int hmgrtable_assign_handle_safe(struct hmgrtable *table, void *object,
				 enum hmgrentry_type type, d3dkmt_handle h)
{
	int ret;

	hmgrtable_lock(table, DXGLOCK_EXCL);
	ret = hmgrtable_assign_handle(table, object, type, h);
	hmgrtable_unlock(table, DXGLOCK_EXCL);
	return ret;
}

int hmgrtable_assign_handle(struct hmgrtable *table, void *object,
			    enum hmgrentry_type type, d3dkmt_handle h)
{
	uint index = get_index(h);
	uint unique = get_unique(h);
	struct hmgrentry *entry = NULL;

	TRACE_DEBUG(1, "%s %x, %d %p, %p\n",
		    __func__, h, index, object, table);

	if (index >= HMGRHANDLE_INDEX_MAX) {
		pr_err("handle index is too big: %x %d", h, index);
		return STATUS_INVALID_PARAMETER;
	}

	if (index >= table->table_size) {
		uint new_size = index + HMGRTABLE_SIZE_INCREMENT;

		if (new_size > HMGRHANDLE_INDEX_MAX)
			new_size = HMGRHANDLE_INDEX_MAX;
		if (!expand_table(table, new_size)) {
			pr_err("failed to expand table\n");
			return STATUS_NO_MEMORY;
		}
	}

	entry = &table->entry_table[index];

	if (entry->type != HMGRENTRY_TYPE_FREE) {
		pr_err("the entry is already busy: %d %x",
			   entry->type,
			   hmgrtable_build_entry_handle(table, index));
		return STATUS_INVALID_PARAMETER;
	}

	if (index != table->free_handle_list_tail) {
		if (entry->next_free_index >= table->table_size) {
			pr_err("hmgr: invalid next free index %d\n",
				   entry->next_free_index);
			return STATUS_INVALID_PARAMETER;
		}
		table->entry_table[entry->next_free_index].prev_free_index =
		    entry->prev_free_index;
	} else {
		table->free_handle_list_tail = entry->prev_free_index;
	}

	if (index != table->free_handle_list_head) {
		if (entry->prev_free_index >= table->table_size) {
			pr_err("hmgr: invalid next prev index %d\n",
				   entry->prev_free_index);
			return STATUS_INVALID_PARAMETER;
		}
		table->entry_table[entry->prev_free_index].next_free_index =
		    entry->next_free_index;
	} else {
		table->free_handle_list_head = entry->next_free_index;
	}

	entry->prev_free_index = HMGRTABLE_INVALID_INDEX;
	entry->next_free_index = HMGRTABLE_INVALID_INDEX;
	entry->object = object;
	entry->type = type;
	entry->instance = 0;
	entry->unique = unique;
	entry->destroyed = false;

	table->free_count--;
	return 0;
}

d3dkmt_handle hmgrtable_alloc_handle_safe(struct hmgrtable *table, void *obj,
					  enum hmgrentry_type type,
					  bool make_valid)
{
	d3dkmt_handle h;

	hmgrtable_lock(table, DXGLOCK_EXCL);
	h = hmgrtable_alloc_handle(table, obj, type, make_valid);
	hmgrtable_unlock(table, DXGLOCK_EXCL);
	return h;
}

void hmgrtable_free_handle(struct hmgrtable *table, enum hmgrentry_type t,
			   d3dkmt_handle h)
{
	struct hmgrentry *entry;
	uint i = get_index(h);

	DXGKRNL_ASSERT(table->free_count < table->table_size);
	DXGKRNL_ASSERT(table->free_count >= HMGRTABLE_MIN_FREE_ENTRIES);

	TRACE_DEBUG(1, "%s: %p %x\n", __func__, table, h);

	/* Ignore the destroyed flag when checking the handle */
	if (is_handle_valid(table, h, true, t)) {
		entry = &table->entry_table[i];
		entry->unique = 1;
		entry->type = HMGRENTRY_TYPE_FREE;
		entry->destroyed = 0;
		if (entry->unique != HMGRHANDLE_UNIQUE_MAX)
			entry->unique += 1;
		else
			entry->unique = 1;

		table->free_count++;

		/*
		 * Insert the index to the free list at the tail.
		 */
		entry->next_free_index = HMGRTABLE_INVALID_INDEX;
		entry->prev_free_index = table->free_handle_list_tail;
		entry = &table->entry_table[table->free_handle_list_tail];
		entry->next_free_index = i;
		table->free_handle_list_tail = i;
	} else {
		pr_err("%s:error: %d %x\n", __func__, i, h);
	}
}

void hmgrtable_free_handle_safe(struct hmgrtable *table, enum hmgrentry_type t,
				d3dkmt_handle h)
{
	hmgrtable_lock(table, DXGLOCK_EXCL);
	hmgrtable_free_handle(table, t, h);
	hmgrtable_unlock(table, DXGLOCK_EXCL);
}

d3dkmt_handle hmgrtable_build_entry_handle(struct hmgrtable *table, uint index)
{
	DXGKRNL_ASSERT(index < table->table_size);

	return build_handle(index, table->entry_table[index].unique,
			    table->entry_table[index].instance);
}

void *hmgrtable_get_object(struct hmgrtable *table, d3dkmt_handle h)
{
	if (!is_handle_valid(table, h, false, HMGRENTRY_TYPE_FREE))
		return NULL;

	return table->entry_table[get_index(h)].object;
}

void *hmgrtable_get_object_by_type(struct hmgrtable *table,
				   enum hmgrentry_type type, d3dkmt_handle h)
{
	if (!is_handle_valid(table, h, false, type)) {
		pr_err("%s invalid handle %x\n", __func__, h);
		return NULL;
	}
	return table->entry_table[get_index(h)].object;
}

void *hmgrtable_get_entry_object(struct hmgrtable *table, uint index)
{
	DXGKRNL_ASSERT(index < table->table_size);
	DXGKRNL_ASSERT(table->entry_table[index].type != HMGRENTRY_TYPE_FREE);

	return table->entry_table[index].object;
}

enum hmgrentry_type hmgrtable_get_entry_type(struct hmgrtable *table,
					     uint index)
{
	DXGKRNL_ASSERT(index < table->table_size);
	return (enum hmgrentry_type)table->entry_table[index].type;
}

enum hmgrentry_type hmgrtable_get_object_type(struct hmgrtable *table,
					      d3dkmt_handle h)
{
	if (!is_handle_valid(table, h, false, HMGRENTRY_TYPE_FREE))
		return HMGRENTRY_TYPE_FREE;

	return hmgrtable_get_entry_type(table, get_index(h));
}

void *hmgrtable_get_object_ignore_destroyed(struct hmgrtable *table,
					    d3dkmt_handle h,
					    enum hmgrentry_type type)
{
	if (!is_handle_valid(table, h, true, type))
		return NULL;
	return table->entry_table[get_index(h)].object;
}

bool hmgrtable_next_entry(struct hmgrtable *tbl, uint *index,
			  enum hmgrentry_type *type, d3dkmt_handle *handle,
			  void **object)
{
	uint i;
	struct hmgrentry *entry;

	for (i = *index; i < tbl->table_size; i++) {
		entry = &tbl->entry_table[i];
		if (entry->type != HMGRENTRY_TYPE_FREE) {
			*index = i + 1;
			*object = entry->object;
			*handle = build_handle(i, entry->unique,
					       entry->instance);
			*type = entry->type;
			return true;
		}
	}
	return false;
}
