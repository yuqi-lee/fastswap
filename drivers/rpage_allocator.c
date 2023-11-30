#include "rpage_allocator.h"

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/shmem_fs.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/smp.h>


int cpu_cache_init(void) {
    struct path path_;
    struct address_space *addr_space_;
    struct page *page_;
    struct page **pages_ = NULL;
    void **slot_;
    struct radix_tree_iter iter_;
    int i = 0;

    if (kern_path("/dev/shm/cpu_cache", LOOKUP_FOLLOW, &path_) != 0) {
        // handle error
    }

    addr_space_ = path_.dentry->d_inode->i_mapping;

    pages_ = (struct page **) vmalloc(sizeof(struct page *) * addr_space_->nrpages);
    if(pages_ == NULL) {
        pr_err("Bad alloc for pages_(struct page**)\n");
        return -1;
    }
    radix_tree_for_each_slot(slot_, &addr_space_->page_tree, &iter_, 0) {
        page_ = radix_tree_deref_slot(slot_);
        // do something with page
        pages_[i] = page_;
    }

    cpu_cache_ = (struct cpu_cache_storage *) vmap(pages_, addr_space_->nrpages, VM_MAP | VM_ALLOC, PAGE_SHARED);
    if(cpu_cache_ == NULL) {
        pr_err("Bad v-mapping for cpu_cache_\n");
        vfree(pages_);
        return -1;
    }

    vfree(pages_);
    return 0;
}
EXPORT_SYMBOL(cpu_cache_init);

u32 get_length(u32 nproc) {
    u32 writer = cpu_cache_->writer[nproc];
    u32 reader = cpu_cache_->reader[nproc];
    if (writer == reader) {
        if (cpu_cache_->items[nproc][writer].addr == -1){
            return 0;
        } else {
            return max_item;
        }
    }
    if (writer > reader) {
        return (writer - reader);
    } else {
         return (max_item - reader + writer);
    }
}

int fetch_cache(u64 *raddr, u32 *rkey) {
    u32 nproc = raw_smp_processor_id();
    u32 reader;
    struct raddr_rkey fetch_one;

    while(get_length(nproc) == 0) ;
    reader = cpu_cache_->reader[nproc];
    fetch_one = cpu_cache_->items[nproc][reader];
    if(fetch_one.addr != -1 && fetch_one.rkey != 0) {
        *raddr = fetch_one.addr;
        *rkey = fetch_one.rkey;
        cpu_cache_->items[nproc][reader].addr = -1;
        cpu_cache_->items[nproc][reader].rkey = 0;
        cpu_cache_->reader[nproc] = (reader + 1) % max_item;
        return 0;
    }
    else{
        return -1;
    }
}

void add_cache(u64 raddr, u32 rkey) {
    u32 nproc = raw_smp_processor_id(); 
    u32 writer = cpu_cache_->writer[nproc];
    if(cpu_cache_->items[nproc][writer].addr == -1){
        cpu_cache_->items[nproc][writer].rkey = rkey;
        cpu_cache_->items[nproc][writer].addr = raddr;
        cpu_cache_->writer[nproc] = (writer + 1) % max_item;
    }
}

int alloc_remote_block() {
    struct block_info *bi;
    u64 raddr_ = 0;
    u32 rkey_ = 0;
    int ret;

    ret = fetch_cache(&raddr_, &rkey_);
    if(ret) {
        //
    }
    
    bi = kmalloc(sizeof(struct block_info), GFP_KERNEL);
    if(!bi) {
        //
    }

    bi->raddr = raddr_;
    bi->rkey = rkey_;
    bi->cnt = 1 << (BLOCK_SHIFT - PAGE_SHIFT);

    // insert to rhashtable (blocks_map)
    rhashtable_insert_fast(blocks_map, &bi->block_node_rhash, blocks_map_params);
    
    // insert to
    INIT_LIST_HEAD(&bi->block_node_list);
    //spin_lock(&free_blocks_list_lock);
    list_add(&bi->block_node_list, &free_blocks_list);
    //spin_unlock(&free_blocks_list_lock);

    return 0;
}
EXPORT_SYMBOL(alloc_remote_block);



u64 alloc_remote_page(void) {
    struct block_info *bi;
    u32 offset;
    u64 raddr;
    int ret;

    spin_lock(&free_blocks_list_lock);
    if(list_empty(&free_blocks_list)) {
        ret = alloc_remote_block();
        if(ret) {
            //
        }
    }

    bi = list_first_entry(&free_blocks_list, struct block_info, block_node_list);
    if(!bi) {
        //
    }

    spin_lock(&bi->block_lock);
    offset = find_first_zero_bit(bi->rpages_bitmap, rblock_size >> PAGE_SHIFT);
    set_bit(offset, bi->rpages_bitmap);
    
    bi->cnt -= 1;
    if(bi->cnt == 0) {
        list_del(&bi->block_node_list);
    }

    spin_unlock(&bi->block_lock);
    spin_unlock(&free_blocks_list_lock);

    raddr = bi->raddr + (offset << PAGE_SHIFT);
    return raddr;
}
EXPORT_SYMBOL(alloc_remote_page);

void free_remote_page(u64 raddr) {
    struct block_info *bi = NULL;
    u64 raddr_ = raddr;
    u32 offset; 

    bi = rhashtable_lookup_fast(blocks_map, &raddr_, blocks_map_params);
    if(!bi) {
        //
    }

    spin_lock(&free_blocks_list_lock);
    spin_lock(&bi->block_lock);

    offset = (raddr - bi->raddr) / rblock_size;
    if(test_bit(offset, bi->rpages_bitmap)) {
        clear_bit(offset, bi->rpages_bitmap);

        bi->cnt += 1;
        if(bi->cnt == rblock_size >> PAGE_SHIFT) {
            free_remote_block(bi);
        }
    }
    else {
        // error handler...
    }

    spin_unlock(&bi->block_lock);
    spin_unlock(&free_blocks_list_lock);
}
EXPORT_SYMBOL(free_remote_page);

void free_remote_block(struct block_info *bi) {
    list_del(&bi->block_node_list);
    rhashtable_remove_fast(blocks_map, &bi->block_node_rhash, blocks_map_params);

    add_cache(bi->raddr, bi->rkey);

    kfree(bi);
}
EXPORT_SYMBOL(free_remote_block);