#include "segment.h"		//hmfs.h is included

static void __mark_sit_entry_dirty(struct sit_info *sit_i, u64 segno)
{
	if (!__test_and_set_bit(segno, sit_i->dirty_sentries_bitmap))
		sit_i->dirty_sentries++;
}

static void update_sit_entry(struct hmfs_sb_info *sbi, u64 segno, int blkoff,
			     int del)
{
	struct seg_entry *se;
	struct sit_info *sit_i = SIT_I(sbi);
	long new_vblocks;

	se = &sit_i->sentries[segno];
	new_vblocks = se->valid_blocks + del;

	BUG_ON(new_vblocks < 0 || new_vblocks > HMFS_PAGE_PER_SEG);

	se->valid_blocks = new_vblocks;
	se->mtime = get_mtime(0);

	if (del > 0) {
		if (hmfs_set_bit(blkoff, se->cur_valid_map))
			BUG();
	} else {
		if (!hmfs_clear_bit(blkoff, se->cur_valid_map))
			BUG();
	}

	__mark_sit_entry_dirty(sit_i, segno);
}

static void reset_curseg(struct curseg_info *seg_i)
{
	seg_i->segno = seg_i->next_segno;
	seg_i->next_blkoff = 0;
	seg_i->next_segno = 0;
}

static inline unsigned long cal_page_addr(struct hmfs_sb_info *sbi,
					  struct curseg_info *seg_i)
{
	return (seg_i->segno << HMFS_SEGMENT_SIZE_BITS) +
	    (seg_i->next_blkoff << HMFS_PAGE_SIZE_BITS) + sbi->main_addr_start;
}

/*
 * get_new_segment -- Find a new segment from the free segments bitmap
 * @newseg returns the found segment
 * must be success (otherwise cause error)
 */
static void get_new_segment(struct hmfs_sb_info *sbi, u64 * newseg)
{
	struct free_segmap_info *free_i = FREE_I(sbi);
	u64 segno;

	write_lock(&free_i->segmap_lock);

	//FIXME: always look forward?
	segno =
	    find_next_zero_bit(free_i->free_segmap, TOTAL_SEGS(sbi), *newseg);
	//if(segno >= TOTAL_SEGS(sbi))

	BUG_ON(test_bit(segno, free_i->free_segmap));
	__set_inuse(sbi, segno);
	*newseg = segno;
	write_unlock(&free_i->segmap_lock);
}

static void move_to_new_segment(struct hmfs_sb_info *sbi,
				struct curseg_info *seg_i)
{
	u64 segno = seg_i->segno;

	get_new_segment(sbi, &segno);
	seg_i->next_segno = segno;
	reset_curseg(seg_i);
}

static u64 get_free_block(struct hmfs_sb_info *sbi, struct curseg_info *seg_i)
{
	u64 page_addr = 0;
	struct sit_info *sit_i = SIT_I(sbi);

	mutex_lock(&seg_i->curseg_mutex);
	page_addr = cal_page_addr(sbi, seg_i);

	mutex_lock(&sit_i->sentry_lock);
	update_sit_entry(sbi, seg_i->segno, seg_i->next_blkoff, 1);
	mutex_unlock(&sit_i->sentry_lock);

	seg_i->next_blkoff++;
	if (seg_i->next_blkoff == HMFS_PAGE_PER_SEG) {
		//TODO : journal when writing CP
		move_to_new_segment(sbi, seg_i);
	}
	mutex_unlock(&seg_i->curseg_mutex);

	return page_addr;
}

u64 get_free_data_block(struct hmfs_sb_info * sbi)
{
	struct curseg_info *seg_i = &CURSEG_I(sbi)[CURSEG_DATA];

	return get_free_block(sbi, seg_i);
}

u64 get_free_node_block(struct hmfs_sb_info * sbi)
{
	struct curseg_info *seg_i = &CURSEG_I(sbi)[CURSEG_NODE];

	return get_free_block(sbi, seg_i);
}

/*
 * NVM related segment management functions
 */
/* get a sit/nat page from sit/nat in-NVM tree */
void* get_sn_page(struct hmfs_sb_info *sbi, void *root, u64 segno, u8 height)
{
	void *new_root;
	u64 new_segno;
	block_t *logical_root = (block_t*)root;

	if (!height)
		return (void *) ADDR(sbi, logical_root[segno]);

	new_root = ADDR(sbi, logical_root[segno >> (height * LOG2_ADDRS_PER_BLOCK)]);
	new_segno = segno
	    & ~(ADDRS_PER_BLOCK_MASK << (height * LOG2_ADDRS_PER_BLOCK));
	return get_sn_page(sbi, new_root, new_segno, height - 1);
}

/* allocate a sit page, returns its fs logical address */
block_t sit_page_alloc(struct hmfs_sb_info *sbi){
	struct curseg_info* curseg = &CURSEG_I(sbi)[CURSEG_NODE];
	block_t logical_addr;
	//FIXME : allocate a new segment, ENOMEM
	mutex_lock(&curseg->curseg_mutex);

	logical_addr = (block_t) (curseg->segno * HMFS_SEGMENT_SIZE + curseg->next_blkoff * HMFS_PAGE_SIZE);
	curseg->next_blkoff++;
	mutex_unlock(&curseg->curseg_mutex);
	return logical_addr;
}

block_t recursive_flush_sit_page(struct hmfs_sb_info *sbi, block_t old_root, void *root_addr, u64 segno, u8 height){
	//FIXME : cannot handle no space
	void *new_addr, *current_addr=NULL;
       	block_t *modi_ptr;
	block_t current_root;
	u64 new_segno;
	unsigned int _offset;
	
	//1. if at leaf, alloc & copy sit entry block 
	if (!height){
		current_root = get_free_node_block(sbi);
		//TODO : copy sit entry blk to current_root 
		return current_root;
	}

	//2. if this node is wandered COW
	if(old_root!=NULL && ADDR(sbi,old_root) == root_addr){
		//need new block allocation
		current_root = get_free_node_block(sbi);
		current_addr = ADDR(sbi, current_root);
		memcpy(current_addr, root_addr, HMFS_PAGE_SIZE);
	}

	//3. go to child, 
	_offset = segno >> (height * LOG2_ADDRS_PER_BLOCK)*sizeof(block_t);
	new_segno = segno
	    & ~(ADDRS_PER_BLOCK_MASK << (height * LOG2_ADDRS_PER_BLOCK));
	if(current_addr!=NULL){
		modi_ptr = (block_t*)(current_addr+_offset);
		new_addr = (void*)(*modi_ptr);

		*modi_ptr = recursive_flush_sit_page(sbi, NULL, new_addr, new_segno, height - 1);//FIXME:not le64
		return current_root;
	} else {
		modi_ptr = (block_t*)(root_addr+_offset);
		recursive_flush_sit_page(sbi, *modi_ptr, ADDR(sbi, modi_ptr), new_segno, height-1);
		return old_root;
	}
}
/* write  inner-NVM tree */
block_t flush_sit_pages(struct hmfs_sb_info *sbi) //TODO:@2 should be sit_blk&seg_no
{
	struct hmfs_sm_info* sm_info = SM_I(sbi);
	struct sit_info* sit_info = SIT_I(sbi);
	struct checkpoint_info *cp_i = CURCP_I(sbi);
	block_t cur_sit_root = cp_i->cur_sit_root;

	/* TODO : 
	 * foreach dirty sit page
	 *	cur_sit_root = recursive_flush_sit_page(sbi, ADDR(sbi, cur_sit_root), 
	 */ 
}

/*
 * DRAM related segment management functions
 */
static int restore_curseg_summaries(struct hmfs_sb_info *sbi)
{
	//TODO -- read summaries from 
	// 1. inner-cp journal
	// read_compacted_summaries(sbi))
	// 2. read_normal_summaries(sbi, type))
	struct checkpoint_info *cp_i = CURCP_I(sbi);
	struct curseg_info *seg_i = CURSEG_I(sbi);

	mutex_lock(&seg_i[CURSEG_NODE].curseg_mutex);
	seg_i[CURSEG_NODE].segno = cp_i->cur_node_segno;
	seg_i[CURSEG_NODE].next_blkoff = cp_i->cur_node_blkoff;
	seg_i[CURSEG_NODE].next_segno = 0;
	seg_i[CURSEG_NODE].sum_blk =
	    ADDR(sbi, cp_i->cur_node_segno * HMFS_SUMMARY_BLOCK_SIZE);
	mutex_unlock(&seg_i[CURSEG_NODE].curseg_mutex);

	mutex_lock(&seg_i[CURSEG_DATA].curseg_mutex);
	seg_i[CURSEG_DATA].segno = cp_i->cur_data_segno;
	seg_i[CURSEG_DATA].next_blkoff = cp_i->cur_data_blkoff;
	seg_i[CURSEG_DATA].next_segno = 0;
	seg_i[CURSEG_DATA].sum_blk =
	    ADDR(sbi, cp_i->cur_data_segno * HMFS_SUMMARY_BLOCK_SIZE);
	mutex_unlock(&seg_i[CURSEG_DATA].curseg_mutex);
	return 0;
}

/* 
 * new_curseg -- Allocate a current working segment.
 * XXX : use this instead of s_op 
 *
 */
static void new_curseg(struct hmfs_sb_info *sbi)
{
	struct curseg_info *curseg = CURSEG_I(sbi);
	u64 segno = curseg->segno;

	//TODO:write-back 
	//write_sum_page(sbi, curseg->sum_blk, 
	//                        GET_SUM_BLOCK(sbi, segno)); 

	get_new_segment(sbi, &segno);
	curseg->next_segno = segno;
	//TODO: set current seg to segno
	//reset_curseg(sbi, type, 1); 
}

static unsigned short  __next_free_blkoff(struct hmfs_sb_info *sbi,
			struct curseg_info *seg, block_t start)
{
	struct seg_entry *se = get_seg_entry(sbi, seg->segno);
	block_t ofs;
	for (ofs = start; ofs < HMFS_PAGE_PER_SEG; ofs++) {
		if(!hmfs_test_bit(ofs, se->cur_valid_map))
			break;
	}
	seg->next_blkoff = ofs;
	return ofs;
}

void allocate_new_segments(struct hmfs_sb_info *sbi)
{
	struct curseg_info *curseg;
	unsigned int old_curseg;

	curseg = CURSEG_I(sbi);
	old_curseg = curseg->segno;
	new_curseg(sbi);
	//TODO:locate_dirty_segment(sbi, old_curseg);
}

static void *get_sit_page(struct hmfs_sb_info *sbi, u64 segno)
{
	struct hmfs_super_block* raw_super = HMFS_RAW_SUPER(sbi);
	struct checkpoint_info* cp_info = sbi->cp_info;

	return get_sn_page(sbi, ADDR(sbi, cp_info->cur_sit_root), segno, raw_super->sit_height);

}

static inline void __set_test_and_inuse(struct hmfs_sb_info *sbi,
					unsigned int segno)
{
	struct free_segmap_info *free_i = FREE_I(sbi);
	write_lock(&free_i->segmap_lock);
	if (!test_and_set_bit(segno, free_i->free_segmap)) {
		free_i->free_segments--;
	}
	write_unlock(&free_i->segmap_lock);
}

//	Given a CP, compare and modify/add a series of sit entry in it, Log-structed way.
//	Modify itself and levels of branches up there.
//	Input: dirty block bit map or specific to one single sit entry
//	return an sit b-tree root address.
static u64 add_CP_sit_entry(struct hmfs_sb_info *sbi)
{
//	TODO:[Billy]
}

static int mark_dirty_sit_blocks(unsigned long *dirty_sentries_bitmap,unsigned long *dirty_blocks_bitmap)
{
//	TODO:[goku] convert from entry bitmap to block bitmap
}

//	just assume sit tree is complete in DRAM for now
//	return an sit b-tree root address.
//	@obsolete
block_t save_sit_entries(struct hmfs_sb_info *sbi)
{
	struct checkpoint_info *cp_i = sbi->cp_info;

	unsigned long index;
	void *item;
	struct radix_tree_root root = sbi->sm_info->sit_info->sentries_root;

	u64 sit_bt_entries_root;
	sit_bt_entries_root = add_CP_sit_entry(sbi);
	return sit_bt_entries_root;
}

/*	Operations for sit entry radix-tree in DRAM 	*/

//	Find sit entry with segment number 'segno'
//	Called under sit_tree_rcu_read_lock
static void *lookup_sit_entry(struct sit_info *sit_i, u64 segno)
{
	return radix_tree_lookup(&sit_i->sentries_root, segno);
}
//	Insert sit entry 'se' with segment number 'segno'
static void *insert_sit_entry(struct sit_info *sit_i, struct seg_entry *se, u64 segno)
{
	return radix_tree_insert(&sit_i->sentries_root, segno, &se);
}
//	Delete sit entry with segment number 'segno'
static void *delete_sit_entry(struct sit_info *sit_i, u64 segno)
{
	return radix_tree_delete(&sit_i->sentries_root, segno);
}
//	Set sit entry with segment number 'segno ' 'DIRTY'
static void *set_dirty_sit_entry(struct sit_info *sit_i, u64 segno)
{
	return radix_tree_tag_set(&sit_i->sentries_root, segno,0);
}
//	Set sit entry with segment number 'segno ' 'CLEAN'
static void *set_clean_sit_entry(struct sit_info *sit_i, u64 segno)
{
	return radix_tree_tag_clear(&sit_i->sentries_root, segno,0);
}
//	Get the tag of sit entry with segment number 'segno '
static int get_tag_sit_entry(struct sit_info *sit_i, u64 segno)
{
	int ret;
	ret = radix_tree_tag_get(&sit_i->sentries_root, segno, 0);
	if (ret==1)
		return SIT_ENTRY_DIRTY;
	else
		return SIT_ENTRY_CLEAN;
}
//	Test whether radix tree is dirty
static int is_sit_radix_tree_dirty (struct sit_info *sit_i)
{
	int ret;
	ret = radix_tree_tagged(&sit_i->sentries_root, 0);
	if (ret==1)
		return SIT_ENTRY_DIRTY;
	else
		return SIT_ENTRY_CLEAN;
}

//	Modified from kernel
static inline void *indirect_to_ptr(void *ptr)
{
	return (void *)((unsigned long)ptr & ~RADIX_TREE_INDIRECT_PTR);
}

unsigned int
radix_tree_gang_lookup_tag_with_index(struct radix_tree_root *root, void **results, int *index,
		unsigned long first_index, unsigned int max_items,
		unsigned int tag)
{
	struct radix_tree_iter iter;
	void **slot;
	unsigned int ret = 0;

	if (unlikely(!max_items))
		return 0;

	radix_tree_for_each_tagged(slot, root, &iter, first_index, tag) {
		results[ret] = indirect_to_ptr(rcu_dereference_raw(*slot));
		index[ret] = iter.index;
		if (!results[ret])
			continue;
		if (++ret == max_items)
			break;
	}

	return ret;
}

//	Test whether radix tree is dirty
//	Return the number of dirty sit entry in 'results'
static unsigned int gang_lookup_sit_entry (struct sit_info *sit_i, struct seg_entry **results, int *index)
{
	int ret;
	ret = radix_tree_gang_lookup_tag_with_index(&sit_i->sentries_root, results, index, 0, MAX_SIT_ITEMS_FOR_GANG_LOOKUP, 0);
	return ret;
}

struct hmfs_sit_journal convert_seg_entry_to_journal (int *index, struct seg_entry *results)
{

}

static unsigned int convert_gang_result_to_journal (struct sit_info *sit_i,  struct hmfs_sit_journal *journal)
{
	int count, done;
	struct seg_entry **results;
	int *index;
	count = radix_tree_gang_lookup_tag_with_index(&sit_i->sentries_root, results, index, 0, MAX_SIT_ITEMS_FOR_GANG_LOOKUP, 0);
	done = 0;
	while ( done < count )
	{
//		Format change
		journal[done++] = convert_seg_entry_to_journal(index[done], results[done]);
	}
	return done;
}

/*
 * routines for build segment manager
 */
static int build_sit_info(struct hmfs_sb_info *sbi)
{
	struct hmfs_super_block *raw_super = HMFS_RAW_SUPER(sbi);
	struct sit_info *sit_i;
	unsigned int start;
	unsigned long long bitmap_size, sit_segs;

	/* allocate memory for SIT information */
	sit_i = kzalloc(sizeof(struct sit_info), GFP_KERNEL);
	if (!sit_i)
		return -ENOMEM;

	SM_I(sbi)->sit_info = sit_i;

	sit_i->sentries = vzalloc(TOTAL_SEGS(sbi) * sizeof(struct seg_entry));
	if (!sit_i->sentries)
		return -ENOMEM;

	bitmap_size = hmfs_bitmap_size(TOTAL_SEGS(sbi));
	sit_i->dirty_sentries_bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!sit_i->dirty_sentries_bitmap)
		return -ENOMEM;

	for (start = 0; start < TOTAL_SEGS(sbi); start++) {
		sit_i->sentries[start].cur_valid_map
		    = kzalloc(SIT_VBLOCK_MAP_SIZE, GFP_KERNEL);
		//TODO: checkpoint
		if (!sit_i->sentries[start].cur_valid_map)
			return -ENOMEM;
	}

	//TODO: allocate bitmap according to checkpoint design
	/* setup SIT bitmap from ckeckpoint pack */
	//bitmap_size = __bitmap_size(sbi, SIT_BITMAP);
	//src_bitmap = __bitmap_ptr(sbi, SIT_BITMAP);

//FIXME:cal sit_segs
	sit_segs = 0;

	INIT_RADIX_TREE(&sit_i->sentries_root,GFP_KERNEL);//XXX:check allocating sentries_root

	sit_i->sit_blocks = sit_segs << HMFS_PAGE_PER_SEG_BITS;
	//sit_i->written_valid_blocks = le64_to_cpu(ckpt->valid_block_count);
	//sit_i->sit_bitmap = dst_bitmap;
	sit_i->bitmap_size = bitmap_size;
	sit_i->dirty_sentries = 0;
	sit_i->sents_per_block = SIT_ENTRY_PER_BLOCK;
	//sit_i->elapsed_time = le64_to_cpu(sbi->ckpt->elapsed_time);
	mutex_init(&sit_i->sentry_lock);
	return 0;
}

static int build_free_segmap(struct hmfs_sb_info *sbi)
{
	struct hmfs_sm_info *sm_info = SM_I(sbi);
	struct free_segmap_info *free_i;
	unsigned int bitmap_size;

	/* allocate memory for free segmap information */
	free_i = kzalloc(sizeof(struct free_segmap_info), GFP_KERNEL);
	if (!free_i)
		return -ENOMEM;

	SM_I(sbi)->free_info = free_i;

	bitmap_size = hmfs_bitmap_size(TOTAL_SEGS(sbi));
	free_i->free_segmap = kmalloc(bitmap_size, GFP_KERNEL);
	if (!free_i->free_segmap)
		return -ENOMEM;

	/* set all segments as dirty temporarily */
	memset(free_i->free_segmap, 0xff, bitmap_size);

	/* init free segmap information */
	free_i->start_segno =
	    (unsigned int)(sm_info->main_blkaddr >> HMFS_PAGE_PER_SEG_BITS);
	free_i->free_segments = 0;
	rwlock_init(&free_i->segmap_lock);
	return 0;
}

static int build_curseg(struct hmfs_sb_info *sbi)
{
	struct curseg_info *array;
	int i;

	array = kzalloc(sizeof(*array) * NR_CURSEG_TYPE, GFP_KERNEL);
	if (!array)
		return -ENOMEM;

	SM_I(sbi)->curseg_array = array;

	for (i = 0; i < NR_CURSEG_TYPE; i++) {
		mutex_init(&array[i].curseg_mutex);
		array[i].sum_blk = NULL;
		array[i].segno = NULL_SEGNO;
		array[i].next_blkoff = 0;
	}
	return restore_curseg_summaries(sbi);
}

static void build_sit_entries(struct hmfs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	u64 start;
	struct checkpoint_info *cp_i = CURCP_I(sbi);
	struct hmfs_checkpoint *hmfs_cp = ADDR(sbi, cp_i->load_checkpoint_addr);

	for (start = 0; start < TOTAL_SEGS(sbi); start++) {
		struct seg_entry *se = &sit_i->sentries[start];
		struct hmfs_sit_block *sit_blk;
		struct hmfs_sit_entry sit;
		int i;

		read_lock(&cp_i->journal_lock);
		for (i = 0; i < NUM_SIT_JOURNALS_IN_CP; ++i) {
			if (le64_to_cpu(hmfs_cp->sit_journals[i].segno) ==
			    start) {
				sit = hmfs_cp->sit_journals[i].entry;
				read_unlock(&cp_i->journal_lock);
				goto found;
			}
		}
		read_unlock(&cp_i->journal_lock);
		//XXX : neednt check summay cuz no journal inside

		sit_blk = get_sit_page(sbi, start);

		if (sit_blk == NULL) {
			sit.mtime = 0;
			sit.vblocks = 0;
			memset_nt(sit.valid_map, 0, SIT_VBLOCK_MAP_SIZE);
		} else
			sit = sit_blk->entries[SIT_ENTRY_OFFSET(sit_i, start)];

		//TODO : invalid block not checked yet 
		//check_block_count(sbi, start, &sit);
found:
		seg_info_from_raw_sit(se, &sit);
	}
}

static void init_free_segmap(struct hmfs_sb_info *sbi)
{
	unsigned int start;
	struct curseg_info *curseg_t = NULL;
	int i;

	for (start = 0; start < TOTAL_SEGS(sbi); start++) {
		struct seg_entry *sentry = get_seg_entry(sbi, start);
		if (!sentry->valid_blocks)
			__set_free(sbi, start);
	}

	/* set use the current segments */
	curseg_t = CURSEG_I(sbi);
	for (i = 0; i < NR_CURSEG_TYPE; i++)
		__set_test_and_inuse(sbi, curseg_t[i].segno);
}

int build_segment_manager(struct hmfs_sb_info *sbi)
{
	struct hmfs_super_block *raw_super = HMFS_RAW_SUPER(sbi);
	struct hmfs_sm_info *sm_info;
	int err;

	sm_info = kzalloc(sizeof(struct hmfs_sm_info), GFP_KERNEL);
	if (!sm_info)
		return -ENOMEM;

	/* init sm info */
	sbi->sm_info = sm_info;
	INIT_LIST_HEAD(&sm_info->wblist_head);
	spin_lock_init(&sm_info->wblist_lock);
	sm_info->main_blkaddr = le64_to_cpu(raw_super->main_blkaddr);
	sm_info->segment_count = le64_to_cpu(raw_super->segment_count);
	sm_info->main_segments = le64_to_cpu(raw_super->segment_count_main);
	sm_info->ssa_blkaddr = le64_to_cpu(raw_super->ssa_blkaddr);
	//TODO: reserved & overprovisioned segment in ckpt

	err = build_sit_info(sbi);
	if (err)
		return err;
	err = build_free_segmap(sbi);
	if (err)
		return err;
	err = build_curseg(sbi);
	if (err)
		return err;

	/* reinit free segmap based on SIT */
	build_sit_entries(sbi);

	init_free_segmap(sbi);
	//XXX : err = build_dirty_segmap(sbi);
	if (err)
		return err;

	//TODO : GC time init
	//init_min_max_mtime(sbi);
	return 0;
}

static void destroy_dirty_segmap(struct hmfs_sb_info *sbi)
{
	//FIXME
	//normal file block will be discarded,
	//but dirty nat/sit file block should be WB
}

static void destroy_curseg(struct hmfs_sb_info *sbi)
{
	struct curseg_info *array = SM_I(sbi)->curseg_array;

	if (!array)
		return;
	SM_I(sbi)->curseg_array = NULL;
	kfree(array);
}

static void destroy_free_segmap(struct hmfs_sb_info *sbi)
{
	struct free_segmap_info *free_i = SM_I(sbi)->free_info;
	if (!free_i)
		return;
	SM_I(sbi)->free_info = NULL;
	kfree(free_i->free_segmap);
	kfree(free_i);
}

static void destroy_sit_info(struct hmfs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int start;

	if (!sit_i)
		return;

	if (sit_i->sentries) {
		for (start = 0; start < TOTAL_SEGS(sbi); start++) {
			kfree(sit_i->sentries[start].cur_valid_map);
		}
	}
	vfree(sit_i->sentries);
	kfree(sit_i->dirty_sentries_bitmap);

	SM_I(sbi)->sit_info = NULL;
	kfree(sit_i);
}

void destroy_segment_manager(struct hmfs_sb_info *sbi)
{
	struct hmfs_sm_info *sm_info = SM_I(sbi);
	destroy_dirty_segmap(sbi);
	destroy_curseg(sbi);
	destroy_free_segmap(sbi);
	destroy_sit_info(sbi);
	sbi->sm_info = NULL;
	kfree(sm_info);
}

struct hmfs_summary *get_summary_by_addr(struct hmfs_sb_info *sbi,
					 void *blk_addr)
{
	u64 logic_addr;
	u64 segno;
	int blkoff;
	struct hmfs_summary_block *summary_blk = NULL;

	logic_addr = blk_addr - sbi->virt_addr;
	segno = logic_addr >> HMFS_SEGMENT_SIZE_BITS;
	summary_blk = ADDR(sbi, segno * HMFS_SUMMARY_BLOCK_SIZE);

	blkoff = (logic_addr & ~HMFS_SEGMENT_MASK) >> HMFS_PAGE_SIZE_BITS;

	return &summary_blk->entries[blkoff];
}

void invalidate_blocks(struct hmfs_sb_info *sbi, u64 blk_addr)
{
	struct sit_info *sit_i = SIT_I(sbi);
	u64 segno = GET_SEGNO(sbi, blk_addr);
	int blkoff =
	    (blk_addr & (HMFS_SEGMENT_SIZE - 1)) >> HMFS_PAGE_SIZE_BITS;

	mutex_lock(&sit_i->sentry_lock);

	update_sit_entry(sbi, segno, blkoff, -1);

	mutex_unlock(&sit_i->sentry_lock);
}
