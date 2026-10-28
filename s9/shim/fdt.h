#pragma once
#include <stdint.h>

/* Copies the root string property into buf; returns its length, 0 if absent. */
int fdt_root_str(uintptr_t fdt, const char *prop, char *buf, int n);
/* /chosen property of 4 or 8 bytes; 1 if found. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
/* Copies the /chosen/bootargs word that follows key ("lcd_id=0x"); 0 if absent. */
int fdt_bootarg(uintptr_t fdt, const char *key, char *buf, int n);
/*
 * Calls cb for every property in the tree. names[1..depth] are the node
 * names on the path (names[1] = "" for the root), as DTB addresses.
 * Returns 0 if fdt is not a device tree.
 */
typedef void (*fdt_prop_cb)(void *ctx, int depth, const uintptr_t *names,
			    uintptr_t prop, uintptr_t val, uint32_t len);
int fdt_walk(uintptr_t fdt, fdt_prop_cb cb, void *ctx);
uint32_t fdt_totalsize(uintptr_t fdt);
