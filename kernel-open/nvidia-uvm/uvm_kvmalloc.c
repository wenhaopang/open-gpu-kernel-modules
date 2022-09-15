/*******************************************************************************
    Copyright (c) 2016-2020 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#include "uvm_common.h"
#include "uvm_linux.h"
#include "uvm_global.h"
#include "uvm_kvmalloc.h"
#include "uvm_rb_tree.h"

// To implement realloc for vmalloc-based allocations we need to track the size
// of the original allocation. We can do that by allocating a header along with
// the allocation itself. Since vmalloc is only used for relatively large
// allocations, this overhead is very small.
//
//要为基于vmalloc的分配实现realloc，需要追踪原始分配的大小。
//可以通过分配标头以及分配本身来做到这一点。
//由于vmalloc仅用于相对较大的分配，因此这种开销非常小。

// We don't need this for kmalloc since we can use ksize().
// 该结构体包含申请虚拟内存的大小和起始指针？
typedef struct
{
    size_t alloc_size;
    uint8_t ptr[0];
} uvm_vmalloc_hdr_t;
//该结构体包含申请的物理内存和虚拟内存的信息
// file、function、line表示调用申请内存的文件、其中的函数和在文件中的位置
// node是定义红黑树结构的节点
typedef struct
{
    const char *file;
    const char *function;
    int line;
    uvm_rb_tree_node_t node;
} uvm_kvmalloc_info_t;

// 定义一个枚举型的变量
// NONE=0,后面BYTES为1，以此类推。
typedef enum
{
    UVM_KVMALLOC_LEAK_CHECK_NONE = 0,
    UVM_KVMALLOC_LEAK_CHECK_BYTES,
    UVM_KVMALLOC_LEAK_CHECK_ORIGIN,
    UVM_KVMALLOC_LEAK_CHECK_COUNT
} uvm_kvmalloc_leak_check_t;

// This is used just to make sure that the APIs aren't used outside of
// uvm_kvmalloc_init/uvm_kvmalloc_exit. The memory allocation would still work
// fine, but the leak checker would get confused.
// 确保API不在uvm_kvmalloc_init/uvm_kvmalloc_exit之外使用，内存分配仍可以正常工作。
static bool g_malloc_initialized = false;

static struct
{
    // Current outstanding bytes allocated当前分配的未完成字节
    atomic_long_t bytes_allocated;

    // Number of allocations made which failed their info allocations. Used just
    // for sanity checks.
    // 信息分配失败的分配数，仅用于完整性检查。
    atomic_long_t untracked_allocations;

    // Use a raw spinlock rather than a uvm_spinlock_t because the kvmalloc
    // layer is initialized and torn down before the thread context layer.
    // 因为kvmalloc在线程上下文之前被初始化和拆除。
    spinlock_t lock;

    // Table of all outstanding allocations
    // 所有未分配完成的表
    uvm_rb_tree_t allocation_info;

    struct kmem_cache *info_cache;
} g_uvm_leak_checker;

// Default to byte-count-only leak checking for non-release builds. This can
// always be overridden by the module parameter.
static int uvm_leak_checker = (UVM_IS_DEBUG() || UVM_IS_DEVELOP()) ?
                                UVM_KVMALLOC_LEAK_CHECK_BYTES :
                                UVM_KVMALLOC_LEAK_CHECK_NONE;

module_param(uvm_leak_checker, int, S_IRUGO);
MODULE_PARM_DESC(uvm_leak_checker,
                 "Enable uvm memory leak checking. "
                 "0 = disabled, 1 = count total bytes allocated and freed, 2 = per-allocation origin tracking.");

NV_STATUS uvm_kvmalloc_init(void)
{
    if (uvm_leak_checker >= UVM_KVMALLOC_LEAK_CHECK_ORIGIN) {
        spin_lock_init(&g_uvm_leak_checker.lock);
        uvm_rb_tree_init(&g_uvm_leak_checker.allocation_info);

        g_uvm_leak_checker.info_cache = NV_KMEM_CACHE_CREATE("uvm_kvmalloc_info_t", uvm_kvmalloc_info_t);
        if (!g_uvm_leak_checker.info_cache)
            return NV_ERR_NO_MEMORY;
    }

    g_malloc_initialized = true;
    return NV_OK;
}

void uvm_kvmalloc_exit(void)
{
    if (!g_malloc_initialized)
        return;

    if (atomic_long_read(&g_uvm_leak_checker.bytes_allocated) > 0) {
        printk(KERN_ERR NVIDIA_UVM_PRETTY_PRINTING_PREFIX "!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        printk(KERN_ERR NVIDIA_UVM_PRETTY_PRINTING_PREFIX "Memory leak of %lu bytes detected.%s\n",
                      atomic_long_read(&g_uvm_leak_checker.bytes_allocated),
                      uvm_leak_checker < UVM_KVMALLOC_LEAK_CHECK_ORIGIN ?
                        " insmod with uvm_leak_checker=2 for detailed information." :
                        "");
        printk(KERN_ERR NVIDIA_UVM_PRETTY_PRINTING_PREFIX "!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

        if (g_uvm_global.unload_state.ptr)
            *g_uvm_global.unload_state.ptr |= UVM_TEST_UNLOAD_STATE_MEMORY_LEAK;
    }

    if (uvm_leak_checker >= UVM_KVMALLOC_LEAK_CHECK_ORIGIN) {
        uvm_rb_tree_node_t *node, *next;

        uvm_rb_tree_for_each_safe(node, next, &g_uvm_leak_checker.allocation_info) {
            uvm_kvmalloc_info_t *info = container_of(node, uvm_kvmalloc_info_t, node);

            printk(KERN_ERR NVIDIA_UVM_PRETTY_PRINTING_PREFIX "    Leaked %zu bytes from %s:%d:%s (0x%llx)\n",
                   uvm_kvsize((void *)((uintptr_t)info->node.key)),
                   kbasename(info->file),
                   info->line,
                   info->function,
                   info->node.key);

            // Free so we don't keep eating up memory while debugging. Note that
            // this also removes the entry from the table, frees info, and drops
            // the allocated bytes count.
            uvm_kvfree((void *)((uintptr_t)info->node.key));
        }

        if (atomic_long_read(&g_uvm_leak_checker.untracked_allocations) == 0)
            UVM_ASSERT(atomic_long_read(&g_uvm_leak_checker.bytes_allocated) == 0);

        kmem_cache_destroy_safe(&g_uvm_leak_checker.info_cache);
    }

    g_malloc_initialized = false;
}

static void insert_info(uvm_kvmalloc_info_t *info)
{
    NV_STATUS status;
    unsigned long irq_flags;

    spin_lock_irqsave(&g_uvm_leak_checker.lock, irq_flags);
    status = uvm_rb_tree_insert(&g_uvm_leak_checker.allocation_info, &info->node);
    spin_unlock_irqrestore(&g_uvm_leak_checker.lock, irq_flags);

    // We shouldn't have duplicates
    UVM_ASSERT(status == NV_OK);
}

static uvm_kvmalloc_info_t *remove_info(void *p)
{
    uvm_rb_tree_node_t *node;
    uvm_kvmalloc_info_t *info = NULL;
    unsigned long irq_flags;

    spin_lock_irqsave(&g_uvm_leak_checker.lock, irq_flags);
    node = uvm_rb_tree_find(&g_uvm_leak_checker.allocation_info, (NvU64)p);
    if (node)
        uvm_rb_tree_remove(&g_uvm_leak_checker.allocation_info, node);
    spin_unlock_irqrestore(&g_uvm_leak_checker.lock, irq_flags);

    if (!node) {
        UVM_ASSERT(atomic_long_read(&g_uvm_leak_checker.untracked_allocations) > 0);
        atomic_long_dec(&g_uvm_leak_checker.untracked_allocations);
    }
    else {
        info = container_of(node, uvm_kvmalloc_info_t, node);
        UVM_ASSERT(info->node.key == (NvU64)((uintptr_t)p));
    }
    return info;
}

static void alloc_tracking_add(void *p, const char *file, int line, const char *function)
{
    // Add uvm_kvsize(p) instead of size because uvm_kvsize might be larger (due
    // to ksize), and uvm_kvfree only knows about uvm_kvsize
    size_t size = uvm_kvsize(p);
    uvm_kvmalloc_info_t *info;

    UVM_ASSERT(g_malloc_initialized);

    if (ZERO_OR_NULL_PTR(p))
        return;

    atomic_long_add(size, &g_uvm_leak_checker.bytes_allocated);

    if (uvm_leak_checker >= UVM_KVMALLOC_LEAK_CHECK_ORIGIN) {
        // Silently ignore OOM errors
        info = nv_kmem_cache_zalloc(g_uvm_leak_checker.info_cache, NV_UVM_GFP_FLAGS);
        if (!info) {
            atomic_long_inc(&g_uvm_leak_checker.untracked_allocations);
            return;
        }

        info->node.key  = (NvU64)p;
        info->file      = file;
        info->function  = function;
        info->line      = line;

        insert_info(info);
    }
}

static void alloc_tracking_remove(void *p)
{
    size_t size = uvm_kvsize(p);
    uvm_kvmalloc_info_t *info;

    UVM_ASSERT(g_malloc_initialized);

    if (ZERO_OR_NULL_PTR(p))
        return;

    atomic_long_sub(size, &g_uvm_leak_checker.bytes_allocated);

    if (uvm_leak_checker >= UVM_KVMALLOC_LEAK_CHECK_ORIGIN) {
        info = remove_info(p);
        if (info)
            kmem_cache_free(g_uvm_leak_checker.info_cache, info);
    }
}

static uvm_vmalloc_hdr_t *get_hdr(void *p)
{
    uvm_vmalloc_hdr_t *hdr;
    UVM_ASSERT(is_vmalloc_addr(p));
    hdr = container_of(p, uvm_vmalloc_hdr_t, ptr);
    UVM_ASSERT(hdr->alloc_size > UVM_KMALLOC_THRESHOLD);
    return hdr;
}

int count_alloc_internal = 0;

static void *alloc_internal(size_t size, bool zero_memory)
{
    uvm_vmalloc_hdr_t *hdr;
	
	printk(KERN_ALERT  "alloc_internal被调用次数 %d \n",count_alloc_internal++);
	printk(KERN_ALERT  "第 %d 次调用alloc_internal__ ，size:%zu--zero_memory:%d\n", count_alloc_internal,size,zero_memory);
	
    // Make sure that the allocation pointer is suitably-aligned for a natively-
    // sized allocation.
    // 确保分配指针与本机大小的分配适当对齐。
    BUILD_BUG_ON(offsetof(uvm_vmalloc_hdr_t, ptr) != sizeof(void *));

    // Make sure that (sizeof(hdr) + size) is what it should be
    BUILD_BUG_ON(sizeof(uvm_vmalloc_hdr_t) != offsetof(uvm_vmalloc_hdr_t, ptr));

    if (size <= UVM_KMALLOC_THRESHOLD) {
        if (zero_memory)
            return kzalloc(size, NV_UVM_GFP_FLAGS);
        return kmalloc(size, NV_UVM_GFP_FLAGS);
    }

    if (zero_memory)
        hdr = vzalloc(sizeof(*hdr) + size);
    else
        hdr = vmalloc(sizeof(*hdr) + size);

    if (!hdr)
        return NULL;

    hdr->alloc_size = size;
    return hdr->ptr;
}

int count___uvm_kvmalloc = 0;

void *__uvm_kvmalloc(size_t size, const char *file, int line, const char *function)
{
    void *p = alloc_internal(size, false);

	printk(KERN_ALERT  "__uvm_kvmalloc被调用次数 %d \n",count___uvm_kvmalloc++);
	if(count___uvm_kvmalloc >=0){
		/*printk(KERN_ALERT  "第 %d 次调用__uvm_kvmalloc ，__uvm_kvmalloc--size 输出 %zu \n", count___uvm_kvmalloc,size);
		printk(KERN_ALERT  "第 %d 次调用__uvm_kvmalloc ，__uvm_kvmalloc--file 输出 %s \n", count___uvm_kvmalloc,file);
		printk(KERN_ALERT  "第 %d 次调用__uvm_kvmalloc ，__uvm_kvmalloc--line 输出 %d \n", count___uvm_kvmalloc,line);
		printk(KERN_ALERT  "第 %d 次调用__uvm_kvmalloc ，__uvm_kvmalloc--function 输出 %s \n", count___uvm_kvmalloc,function);
		*/
		//printk(KERN_ALERT  "第 %d 次调用__uvm_kvmalloc__ ，size:%zu--file:%s--line:%d--function:%s\n", count___uvm_kvmalloc,size,file,line,function);
	}
    if (uvm_leak_checker && p)
        alloc_tracking_add(p, file, line, function);

    return p;
}

int count___uvm_kvmalloc_zero = 0;

void *__uvm_kvmalloc_zero(size_t size, const char *file, int line, const char *function)
{
    void *p = alloc_internal(size, true);
	printk(KERN_ALERT  "__uvm_kvmalloc_zero被调用次数 %d \n",count___uvm_kvmalloc_zero++);
	printk(KERN_ALERT  "第 %d 次调用__uvm_kvmalloc_zero__ ，size:%zu--file:%s--line:%d--function%s\n", count___uvm_kvmalloc_zero,size,file,line,function);
    if (uvm_leak_checker && p)
        alloc_tracking_add(p, file, line, function);

    return p;
}

int count_uvm_kvfree = 0;

void uvm_kvfree(void *p)
{
    if (!p)
        return;

	printk(KERN_ALERT  "uvm_kvfree被调用次数 %d \n",count_uvm_kvfree++);
	
    if (uvm_leak_checker)
        alloc_tracking_remove(p);

    if (is_vmalloc_addr(p))
        vfree(get_hdr(p));
    else
        kfree(p);
}

// Handle reallocs of kmalloc-based allocations
int count_realloc_from_kmalloc = 0;

static void *realloc_from_kmalloc(void *p, size_t new_size)
{
    void *new_p;

	printk(KERN_ALERT  "realloc_from_kmalloc被调用次数 %d \n",count_realloc_from_kmalloc++);
	
    // Simple case: kmalloc -> kmalloc
    if (new_size <= UVM_KMALLOC_THRESHOLD)
        return krealloc(p, new_size, NV_UVM_GFP_FLAGS);

    // kmalloc -> vmalloc
    new_p = alloc_internal(new_size, false);
    if (!new_p)
        return NULL;
    memcpy(new_p, p, min(ksize(p), new_size));
    kfree(p);
    return new_p;
}

// Handle reallocs of vmalloc-based allocations
int count_realloc_from_vmalloc = 0;

static void *realloc_from_vmalloc(void *p, size_t new_size)
{
    uvm_vmalloc_hdr_t *old_hdr = get_hdr(p);
    void *new_p;
	printk(KERN_ALERT  "realloc_from_vmalloc被调用次数 %d \n",count_realloc_from_vmalloc++);

    if (new_size == 0) {
        vfree(old_hdr);
        return ZERO_SIZE_PTR; // What krealloc returns for this case
    }

    if (new_size == old_hdr->alloc_size)
        return p;

    // vmalloc has no realloc functionality so we need to do a separate alloc +
    // copy.
    new_p = alloc_internal(new_size, false);
    if (!new_p)
        return NULL;

    memcpy(new_p, p, min(new_size, old_hdr->alloc_size));
    vfree(old_hdr);
    return new_p;
}


int count___uvm_kvrealloc = 0;

void *__uvm_kvrealloc(void *p, size_t new_size, const char *file, int line, const char *function)
{
    void *new_p;

	printk(KERN_ALERT  "__uvm_kvrealloc被调用次数 %d \n",count___uvm_kvrealloc++);
	
    uvm_kvmalloc_info_t *info = NULL;
    size_t old_size;

    if (ZERO_OR_NULL_PTR(p))
        return __uvm_kvmalloc(new_size, file, line, function);

    old_size = uvm_kvsize(p);

    if (uvm_leak_checker) {
        // new_size == 0 is a free, so just remove everything
        if (new_size == 0) {
            alloc_tracking_remove(p);
        }
        else {
            // Remove the old pointer. If the realloc gives us a new pointer
            // with the old one still in the tracking table, that pointer could
            // be reallocated by another thread before we remove it from the
            // table.
            atomic_long_sub(old_size, &g_uvm_leak_checker.bytes_allocated);
            if (uvm_leak_checker >= UVM_KVMALLOC_LEAK_CHECK_ORIGIN)
                info = remove_info(p);
        }
    }

    if (is_vmalloc_addr(p))
        new_p = realloc_from_vmalloc(p, new_size);
    else
        new_p = realloc_from_kmalloc(p, new_size);

    if (uvm_leak_checker) {
        if (!new_p) {
            // The realloc failed, so put the old info back
            atomic_long_add(old_size, &g_uvm_leak_checker.bytes_allocated);
            if (uvm_leak_checker >= UVM_KVMALLOC_LEAK_CHECK_ORIGIN && info)
                insert_info(info);
        }
        else if (new_size != 0) {
            // Drop the old info and insert the new
            if (info)
                kmem_cache_free(g_uvm_leak_checker.info_cache, info);
            alloc_tracking_add(new_p, file, line, function);
        }
    }

    return new_p;
}


int count_uvm_kvsize = 0;

size_t uvm_kvsize(void *p)
{
	printk(KERN_ALERT  "uvm_kvsize被调用次数 %d \n",count_uvm_kvsize++);
    UVM_ASSERT(g_malloc_initialized);
    UVM_ASSERT(p);
    if (is_vmalloc_addr(p))
        return get_hdr(p)->alloc_size;
    return ksize(p);
}

