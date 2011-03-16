#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#include "castle_public.h"
#include "castle_utils.h"
#include "castle.h"
#include "castle_cache.h"
#include "castle_btree.h"
#include "castle_time.h"
#include "castle_versions.h"
#include "castle_extent.h"
#include "castle_ctrl.h"
#include "castle_da.h"
#include "castle_trace.h"
#include "castle_sysfs.h"
#include "castle_objects.h"
#include "castle_bloom.h"

#ifndef CASTLE_PERF_DEBUG
#define ts_delta_ns(a, b)                       ((void)0)
#define castle_perf_debug_getnstimeofday(ts)    ((void)0)
#define castle_perf_debug_bump_ctr(ctr, a, b)   ((void)0)
#else
#define ts_delta_ns(a, b)                       (timespec_to_ns(&a) - timespec_to_ns(&b))
#define castle_perf_debug_getnstimeofday(ts)    (getnstimeofday(ts))
#define castle_perf_debug_bump_ctr(ctr, a, b)   (ctr += ts_delta_ns(a, b))
#endif

//#define DEBUG
#ifndef DEBUG
#define debug(_f, ...)            ((void)0)
#define debug_verbose(_f, ...)    ((void)0)
#define debug_iter(_f, ...)       ((void)0)
#if 1
#define debug_merges(_f, ...)     ((void)0)
#else
#define debug_merges(_f, _a...)   (printk("%s:%.4d: DA=%d, level=%d: " \
                                        _f, __FILE__, __LINE__ , da->id, level, ##_a))
#endif
#else
#define debug(_f, _a...)          (printk("%s:%.4d: " _f, __FILE__, __LINE__ , ##_a))
#define debug_verbose(_f, ...)    (printk("%s:%.4d: " _f, __FILE__, __LINE__ , ##_a))
#define debug_iter(_f, _a...)     (printk("%s:%.4d: " _f, __FILE__, __LINE__ , ##_a))
#define debug_merges(_f, _a...)   (printk("%s:%.4d: DA=%d, level=%d: " \
                                        _f, __FILE__, __LINE__ , da->id, level, ##_a))
#endif

#define VLBA_HDD_RO_TREE_NODE_SIZE      (64)  /**< Size of the default RO tree node size. */
#define VLBA_SSD_RO_TREE_NODE_SIZE      (2)   /**< Size of the RO tree node size on SSDs. */

#define MAX_DYNAMIC_TREE_SIZE           (20) /* In C_CHK_SIZE. */
#define MAX_DYNAMIC_DATA_SIZE           (20) /* In C_CHK_SIZE. */

#define CASTLE_DA_HASH_SIZE             (1000)
#define CASTLE_CT_HASH_SIZE             (4000)
static struct list_head        *castle_da_hash       = NULL;
static struct list_head        *castle_ct_hash       = NULL;
static struct castle_mstore    *castle_da_store      = NULL;
static struct castle_mstore    *castle_tree_store    = NULL;
static struct castle_mstore    *castle_lo_store      = NULL;
       da_id_t                  castle_next_da_id    = 1; 
static tree_seq_t               castle_next_tree_seq = 1; 
static int                      castle_da_exiting    = 0;

static int                      castle_dynamic_driver_merge = 1; 

static struct
{
    int                     cnt;    /**< Size of cpus array.                        */
    int                    *cpus;   /**< Array of CPU ids for handling requests.    */
} request_cpus;

module_param(castle_dynamic_driver_merge, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(castle_dynamic_driver_merge, "Dynamic driver merge");

/* set to 0 to disable using SSDs for btree leaf nodes */
static int                      castle_use_ssd_leaf_nodes = 1;

module_param(castle_use_ssd_leaf_nodes, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(castle_use_ssd_leaf_nodes, "Use SSDs for btree leaf nodes");

/**********************************************************************************************/
/* Notes about the locking on doubling arrays & component trees.
   Each doubling array has a spinlock which protects the lists of component trees rooted in
   the trees array.
   Each component tree has a reference count, initialised to 1 at the tree creation. Each IO
   and other operation which uses the tree needs to take a reference to the tree. Reference
   should be taken under doubling array lock (which guarantees that the component tree is
   currently threaded onto the doubling array tree list, and vice versa. When a tree is 
   removed from the doubling array, no-one else will take references to it any more.
   Component trees are destroyed when reference count reaches 0. The only operation which
   causes trees to be destroyed is the merge process. It decrements the reference count by 1,
   if there are any outstanding IOs, the ref count will reach 0 when last IO completes.
   When a new RW component tree (rwct) is created, previous rwct is moved onto level one. There
   may be ongoing writes to this component tree. This is safe, because all further reads to 
   the tree (either doubling array reads, or merge) chain lock the tree nodes appropriately.
   RW tree creation and merges are serialised using the flags field.

   For DAs, only an attached DA is guaranteed to be in the hash.
 */

DEFINE_HASH_TBL(castle_da, castle_da_hash, CASTLE_DA_HASH_SIZE, struct castle_double_array, hash_list, da_id_t, id);
DEFINE_HASH_TBL(castle_ct, castle_ct_hash, CASTLE_CT_HASH_SIZE, struct castle_component_tree, hash_list, tree_seq_t, seq);
static LIST_HEAD(castle_deleted_das);

/**********************************************************************************************/
/* Prototypes */
static struct castle_component_tree* castle_ct_alloc(struct castle_double_array *da, 
                                                     btree_t type, 
                                                     int level);
static inline int castle_da_is_locked(struct castle_double_array *da);
void castle_ct_get(struct castle_component_tree *ct, int write);
void castle_ct_put(struct castle_component_tree *ct, int write);
static void castle_component_tree_add(struct castle_double_array *da,
                                      struct castle_component_tree *ct,
                                      struct list_head *head,
                                      int in_init);
static void castle_component_tree_del(struct castle_double_array *da,
                                      struct castle_component_tree *ct);
struct castle_da_merge;
static USED void castle_da_merges_print(struct castle_double_array *da);
static int castle_da_merge_restart(struct castle_double_array *da, void *unused);
static int castle_da_merge_start(struct castle_double_array *da, void *unused);
void castle_double_array_merges_fini(void);
static void castle_da_merge_budget_consume(struct castle_da_merge *merge);
static void castle_da_queue_kick(struct work_struct *work);
static void castle_da_read_bvec_start(struct castle_double_array *da, c_bvec_t *c_bvec);
static void castle_da_write_bvec_start(struct castle_double_array *da, c_bvec_t *c_bvec);
static void castle_da_get(struct castle_double_array *da);
static void castle_da_put(struct castle_double_array *da);

struct workqueue_struct *castle_da_wqs[NR_CASTLE_DA_WQS];
char *castle_da_wqs_names[NR_CASTLE_DA_WQS] = {"castle_da0"};

/**********************************************************************************************/
/* Utils */

static inline int castle_da_growing_rw_test_and_set(struct castle_double_array *da)
{
    return test_and_set_bit(DOUBLE_ARRAY_GROWING_RW_TREE_BIT, &da->flags);
}

static inline int castle_da_growing_rw_test(struct castle_double_array *da)
{
    return test_bit(DOUBLE_ARRAY_GROWING_RW_TREE_BIT, &da->flags);
}

static inline void castle_da_growing_rw_clear(struct castle_double_array *da)
{
    clear_bit(DOUBLE_ARRAY_GROWING_RW_TREE_BIT, &da->flags);
}

#define FOR_EACH_MERGE_TREE(_i, _merge) for((_i)=0; (_i)<(_merge)->nr_trees; (_i)++)

static inline int castle_da_deleted(struct castle_double_array *da)
{
    return test_bit(DOUBLE_ARRAY_DELETED_BIT, &da->flags);
}

static inline void castle_da_deleted_set(struct castle_double_array *da)
{
    set_bit(DOUBLE_ARRAY_DELETED_BIT, &da->flags);
}

/* Note: Freezing of DA and unfreezing it could be racing. Unfreeze can happen
 * between failed castle_extent_alloc() and set_bit(FROZEN), consequently we
 * would miss a wake-up cycle. We need two bits to de-couple freezing and
 * un-freezing. Unfreezing just sets a bit. Freezing first checks if some one
 * did a unfreeze, if so don't set freeze and clear unfreeze. 
 *
 * All freeze/unfreeze functions require a hold on da->lock. */

/**
 * Is the doubling array unfrozen.
 *
 * WARNING: Caller must have at least a read lock on the da.
 */
static inline int castle_da_unfrozen(struct castle_double_array *da)
{
    return test_bit(DOUBLE_ARRAY_UNFROZEN_BIT, &da->flags);
}

/**
 * Unfreze the doubling array.
 */
static int castle_da_unfrozen_set(struct castle_double_array *da, void *unused)
{
    write_lock(&da->lock);

    if (test_bit(DOUBLE_ARRAY_FROZEN_BIT, &da->flags))
    {
        printk("Un-freezing Doubling Array: %u\n", da->id);
        set_bit(DOUBLE_ARRAY_UNFROZEN_BIT, &da->flags);
        write_unlock(&da->lock);
        castle_da_merge_restart(da, NULL);
    }
    else
        write_unlock(&da->lock);

    return 0;
}

/**
 * @FIXME
 *
 * WARNING: Caller must have at least a read lock on the da.
 */
static inline int _castle_da_frozen(struct castle_double_array *da)
{
    if (castle_da_unfrozen(da))
    {
        clear_bit(DOUBLE_ARRAY_FROZEN_BIT, &da->flags);
        clear_bit(DOUBLE_ARRAY_UNFROZEN_BIT, &da->flags);
    }

    return test_bit(DOUBLE_ARRAY_FROZEN_BIT, &da->flags);
}

/**
 * Is the doubling array frozen.
 */
static inline int castle_da_frozen(struct castle_double_array *da)
{
    int ret;

    read_lock(&da->lock);
    ret = _castle_da_frozen(da);
    read_unlock(&da->lock);

    return ret;
}

/**
 * Freeze the doubling array.
 */
static inline void castle_da_frozen_set(struct castle_double_array *da)
{
    write_lock(&da->lock);

    if (castle_da_unfrozen(da))
    {
        clear_bit(DOUBLE_ARRAY_FROZEN_BIT, &da->flags);
        clear_bit(DOUBLE_ARRAY_UNFROZEN_BIT, &da->flags);

        write_unlock(&da->lock);
        return;
    }

    printk("Freezing Doubling Array: %u\n", da->id);
    set_bit(DOUBLE_ARRAY_FROZEN_BIT, &da->flags);

    write_unlock(&da->lock);
}

/**
 * Unfreeze all doubling arrays.
 */
int castle_double_arrays_unfreeze(void)
{
    castle_da_hash_iterate(castle_da_unfrozen_set, NULL); 

    return 0;
}

/**********************************************************************************************/
/* Iterators */
struct castle_immut_iterator;

typedef void (*castle_immut_iter_node_start) (struct castle_immut_iterator *);

typedef struct castle_immut_iterator {
    struct castle_component_tree *tree;
    struct castle_btree_type     *btree;
    int                           completed;  /**< set to 1 when iterator is exhausted            */
    c2_block_t                   *curr_c2b;   /**< node c2b currently providing entries           */
    struct castle_btree_node     *curr_node;  /**< btree node (curr_c2b->buffer)                  */
    int                           curr_idx;   /**< offset within curr_node of current entry
                                                   (where current is really next())               */
    c2_block_t                   *next_c2b;   /**< node c2b to provide next entires               */
    int                           next_idx;   /**< offset within next_c2b of first entry to return*/
    castle_immut_iter_node_start  node_start; /**< callback handler to fire whenever iterator moves
                                                   to a new node within the btree                 */
    void                         *private;    /**< callback handler private data                  */
} c_immut_iter_t;

static int castle_ct_immut_iter_entry_find(c_immut_iter_t *iter,
                                           struct castle_btree_node *node,
                                           int start_idx) 
{
    int disabled;
    c_val_tup_t cvt;

    for(; start_idx<node->used; start_idx++)
    {
        disabled = iter->btree->entry_get(node, start_idx, NULL, NULL, &cvt);
        if(!CVT_LEAF_PTR(cvt) && !disabled)
            return start_idx; 
    }

    return -1;
}

/**
 * Update iterator with new btree node.
 *
 * @param iter  Iterator to update
 * @param node  Proposed next node
 *
 * @return 0    Node is not leaf or has no entries.
 * @return 1    Node is leaf and has entries.
 */
static int castle_ct_immut_iter_next_node_init(c_immut_iter_t *iter,
                                               struct castle_btree_node *node)
{
    /* Non-leaf nodes do not contain any entries for the enumerator, continue straight through */
    if(!node->is_leaf)
        return 0;

    /* Non-dynamic trees do not contain leaf pointers => the node must be non-empty, 
       and will not contain leaf pointers */
    if(!iter->tree->dynamic)
    {
        iter->next_idx = 0;
        BUG_ON(castle_ct_immut_iter_entry_find(iter, node, 0 /* start_idx */) != iter->next_idx);
        BUG_ON(node->used == 0);
        return 1;
    }

    /* Finally, for dynamic trees, check if we have at least non-leaf pointer */
    iter->next_idx = castle_ct_immut_iter_entry_find(iter, node, 0 /* start_idx */);
    if(iter->next_idx >= 0)
        return 1;

    return 0;
}

/**
 * Find the next leaf node starting from cep.
 *
 * @also castle_ct_immut_iter_next_node_init()
 */
static void castle_ct_immut_iter_next_node_find(c_immut_iter_t *iter, 
                                                c_ext_pos_t cep,
                                                uint16_t node_size)
{
    struct castle_btree_node *node;
    c2_block_t *c2b;
#ifdef CASTLE_PERF_DEBUG
    struct timespec ts_start, ts_end;
#endif
     
    debug("Looking for next node starting with "cep_fmt_str_nl, cep2str(cep));
    BUG_ON(iter->next_c2b);
    c2b=NULL;
    while(!EXT_POS_INVAL(cep))
    {
        /* Release c2b if we've got one */
        if(c2b)
            put_c2b(c2b);
        /* Get cache block for the current c2b */
        castle_perf_debug_getnstimeofday(&ts_start);
        c2b = castle_cache_block_get(cep, node_size); 
        castle_perf_debug_getnstimeofday(&ts_end);
        /* Update time spent obtaining c2bs. */
        castle_perf_debug_bump_ctr(iter->tree->get_c2b_ns, ts_end, ts_start);
        debug("Node in immut iter.\n");
        castle_cache_advise(c2b->cep, C2_ADV_PREFETCH|C2_ADV_FRWD, -1, -1, 0);
        write_lock_c2b(c2b);
        /* If c2b is not up to date, issue a blocking READ to update */
        if(!c2b_uptodate(c2b))
        {
            castle_perf_debug_getnstimeofday(&ts_start);
            BUG_ON(submit_c2b_sync(READ, c2b));
            castle_perf_debug_getnstimeofday(&ts_end);
            castle_perf_debug_bump_ctr(iter->tree->bt_c2bsync_ns, ts_end, ts_start);
        }
        write_unlock_c2b(c2b);
        node = c2b_bnode(c2b);
        /* Determine if this is a leaf-node with entries */
        if(castle_ct_immut_iter_next_node_init(iter, node))
        {
            /* It is */
            debug("Cep "cep_fmt_str " will be used next, exiting.\n",
                   cep2str(cep));
            iter->next_c2b = c2b;
            return;
        }
        cep = node->next_node;
        node_size = node->next_node_size;
        debug("Node non-leaf or no non-leaf-ptr entries, moving to " cep_fmt_str_nl, 
               cep2str(cep));
    } 
    /* Drop c2b if we failed to find a leaf node, but have an outstanding reference to 
       a non-leaf node */
    if(c2b)
        put_c2b(c2b);
}

/**
 * Find the next leaf node for iter.
 *
 * @also castle_ct_immut_iter_next_node_find()
 */
static void castle_ct_immut_iter_next_node(c_immut_iter_t *iter)
{
    BUG_ON(!iter->next_c2b);
    /* Drop the current c2b, if one exists. */
    if(iter->curr_c2b)
    {
        debug("Moving to the next block after: "cep_fmt_str_nl, 
               cep2str(iter->curr_c2b->cep));
        put_c2b(iter->curr_c2b);
    }
    /* next_c2b becomes curr_c2b */ 
    iter->curr_c2b  = iter->next_c2b;
    BUG_ON(!c2b_uptodate(iter->curr_c2b));
    iter->curr_node = c2b_bnode(iter->curr_c2b); 
    if(!iter->curr_node->is_leaf ||
           (iter->curr_node->used <= iter->next_idx))
    {
        printk("curr_node=%d, used=%d, next_idx=%d\n",
                iter->curr_node->is_leaf,
                iter->curr_node->used,
                iter->next_idx);
    }
    BUG_ON(!iter->curr_node->is_leaf ||
           (iter->curr_node->used <= iter->next_idx));
    iter->curr_idx  = iter->next_idx;
    debug("Moved to cep="cep_fmt_str_nl, cep2str(iter->curr_c2b->cep));

    /* Fire the node_start callback. */
    if (iter->node_start)
        iter->node_start(iter);

    /* Find next c2b following the list pointers */
    iter->next_c2b = NULL;
    castle_ct_immut_iter_next_node_find(iter, 
                                        iter->curr_node->next_node,
                                        iter->curr_node->next_node_size);
}

static void castle_ct_immut_iter_next(c_immut_iter_t *iter, 
                                      void **key_p, 
                                      version_t *version_p, 
                                      c_val_tup_t *cvt_p)
{
    int disabled;

    /* Check if we can read from the curr_node. If not move to the next node. 
       Make sure that if entries exist, they are not leaf pointers. */
    if(iter->curr_idx >= iter->curr_node->used || iter->curr_idx < 0) 
    {
        debug("No more entries in the current node. Asking for next.\n");
        BUG_ON((iter->curr_idx >= 0) && (iter->curr_idx > iter->curr_node->used));
        castle_ct_immut_iter_next_node(iter);
        BUG_ON((iter->curr_idx >= 0) && (iter->curr_idx >= iter->curr_node->used));
    }
    disabled = iter->btree->entry_get(iter->curr_node, 
                                      iter->curr_idx, 
                                      key_p, 
                                      version_p, 
                                      cvt_p);
    /* curr_idx should have been set to a non-leaf pointer */
    BUG_ON(CVT_LEAF_PTR(*cvt_p) || disabled);
    iter->curr_idx = castle_ct_immut_iter_entry_find(iter, iter->curr_node, iter->curr_idx + 1);
    debug("Returned next, curr_idx is now=%d / %d.\n", iter->curr_idx, iter->curr_node->used);
}

static int castle_ct_immut_iter_has_next(c_immut_iter_t *iter)
{
    if(unlikely(iter->completed))
        return 0;

    if((iter->curr_idx >= iter->curr_node->used || iter->curr_idx < 0) && (!iter->next_c2b))
    {
        iter->completed = 1;
        BUG_ON(!iter->curr_c2b);
        put_c2b(iter->curr_c2b);
        iter->curr_c2b = NULL;

        return 0;
    }

    return 1;
} 

/**
 * Initialise iterator for immutable btrees.
 *
 * @param iter          Iterator to initialise
 * @param node_start    CB handler when iterator moves to a new btree node
 * @param private       Private data to pass to CB handler
 */
static void castle_ct_immut_iter_init(c_immut_iter_t *iter,
                                      castle_immut_iter_node_start node_start,
                                      void *private)
{
    debug("Initialising immut enumerator for ct id=%d\n", iter->tree->seq);
    iter->btree     = castle_btree_type_get(iter->tree->btree_type);
    iter->completed = 0;
    iter->curr_c2b  = NULL;
    iter->next_c2b  = NULL;
    iter->node_start= node_start;
    iter->private   = private;
    castle_ct_immut_iter_next_node_find(iter, 
                                        iter->tree->first_node,
                                        iter->tree->first_node_size);
    /* Check if we succeeded at finding at least a single node */
    BUG_ON(!iter->next_c2b);
    /* Init curr_c2b correctly */
    castle_ct_immut_iter_next_node(iter);
}

static void castle_ct_immut_iter_cancel(c_immut_iter_t *iter)
{
    debug("Cancelling immut enumerator for ct id=%d\n", iter->tree->seq);
    if (iter->curr_c2b)
        put_c2b(iter->curr_c2b);
    if (iter->next_c2b)
        put_c2b(iter->next_c2b);
}

struct castle_iterator_type castle_ct_immut_iter = {
    .register_cb = NULL,
    .prep_next   = NULL,
    .has_next    = (castle_iterator_has_next_t)castle_ct_immut_iter_has_next,
    .next        = (castle_iterator_next_t)    castle_ct_immut_iter_next,
    .skip        = NULL,
    .cancel      = (castle_iterator_cancel_t)castle_ct_immut_iter_cancel,
};

/**
 * Compare verion tuples k1,v1 against k2,v2.
 *
 * @param btree Source btree (for compare function)
 * @param k1    Key to compare against
 * @param v1    Version to compare against
 * @param k2    Key to compare with
 * @param v2    Version to compare with
 *
 * @return -1   (k1, v1) <  (k2, v2)
 * @return  0   (k1, v1) == (k2, v2)
 * @return  1   (k1, v1) >  (k2, v2)
 */
static int castle_kv_compare(struct castle_btree_type *btree,
                             void *k1, version_t v1,
                             void *k2, version_t v2)
{
    int ret = btree->key_compare(k1, k2);
    if(ret != 0)
        return ret;
    
    /* Reverse v achieved by inverting v1<->v2 given to version_compare() function */
    return castle_version_compare(v2, v1);
}

static void castle_da_node_buffer_init(struct castle_btree_type *btree,
                                       struct castle_btree_node *buffer,
                                       uint16_t node_size)
{
    debug("Resetting btree node buffer.\n");
    /* Buffers are proper btree nodes understood by castle_btree_node_type function sets.
       Initialise the required bits of the node, so that the types don't complain. */
    buffer->magic     = BTREE_NODE_MAGIC;
    buffer->type      = btree->magic;
    buffer->version   = 0;
    buffer->used      = 0;
    buffer->is_leaf   = 1;
    buffer->next_node = INVAL_EXT_POS;
    buffer->size      = node_size;
}

/**
 * Modlist B-tree iterator structure.
 *
 * @also castle_ct_modlist_iter_init()
 */
typedef struct castle_modlist_iterator {
    struct castle_btree_type *btree;
    struct castle_component_tree *tree;
    uint16_t leaf_node_size;
    struct castle_da_merge *merge;
    c_immut_iter_t *enumerator;
    uint8_t enum_advanced;          /**< Set if enumerator has advanced to a new node             */
    int err;
    uint32_t nr_nodes;              /**< Number of nodes in the buffer                            */
    void *node_buffer;              /**< Buffer to store all the nodes                            */
    uint32_t nr_items;              /**< Number of items in the buffer                            */
    uint32_t next_item;             /**< Next item to return in iterator                          */
    struct item_idx {
        uint32_t node;              /**< Which btree node                                         */
        uint32_t node_offset;       /**< Offset within btree node                                 */
    } *src_entry_idx;               /**< 1 of 2 arrays of entry pointers (used for sort)          */
    struct item_idx *dst_entry_idx; /**< 2nd array of entry pointers                              */
    struct entry_range {            /**< Entry range describes start,end within *_entry_idx       */
        uint32_t start;
        uint32_t end;
    } *ranges;
    uint32_t nr_ranges;             /**< Number of elements in node_ranges                        */
} c_modlist_iter_t;

/**
 * Free all memory allocated by the iterator.
 */
static void castle_ct_modlist_iter_free(c_modlist_iter_t *iter)
{
    if(iter->enumerator)
    {
        castle_ct_immut_iter.cancel(iter->enumerator);
        castle_free(iter->enumerator);
    }
    if(iter->node_buffer)
        castle_vfree(iter->node_buffer);
    if (iter->src_entry_idx)
        castle_vfree(iter->src_entry_idx);
    if (iter->dst_entry_idx)
        castle_vfree(iter->dst_entry_idx);
    if (iter->ranges)
        castle_vfree(iter->ranges);
}

/**
 * Get requested btree node from the node_buffer.
 */
static struct castle_btree_node* castle_ct_modlist_iter_buffer_get(c_modlist_iter_t *iter, 
                                                                   uint32_t idx)
{
    char *buffer = iter->node_buffer;

    return (struct castle_btree_node *)(buffer + idx * iter->leaf_node_size * C_BLK_SIZE); 
}

/**
 * Return key, version, cvt for entry sort_idx within iter->src_entry_idx[].
 */
static void castle_ct_modlist_iter_item_get(c_modlist_iter_t *iter, 
                                            uint32_t sort_idx,
                                            void **key_p,
                                            version_t *version_p,
                                            c_val_tup_t *cvt_p)
{
    struct castle_btree_type *btree = iter->btree;
    struct castle_btree_node *node;
   
    debug_verbose("Node_idx=%d, offset=%d\n", 
                  iter->sort_idx[sort_idx].node,
                  iter->sort_idx[sort_idx].node_offset);
    node = castle_ct_modlist_iter_buffer_get(iter, iter->src_entry_idx[sort_idx].node);
    btree->entry_get(node,
                     iter->src_entry_idx[sort_idx].node_offset,
                     key_p,
                     version_p,
                     cvt_p);
}

/**
 * Return the next entry from the iterator.
 *
 * - Uses the final sorted src_entry_idx[].
 *
 * @also castle_ct_modlist_iter_fill()
 * @also castle_ct_modlist_iter_mergesort()
 */
static void castle_ct_modlist_iter_next(c_modlist_iter_t *iter, 
                                        void **key_p, 
                                        version_t *version_p, 
                                        c_val_tup_t *cvt_p)
{
    castle_ct_modlist_iter_item_get(iter, iter->next_item, key_p, version_p, cvt_p);
    iter->next_item++;
}

/**
 * Does the iterator have further entries.
 *
 * @return 1    Entry has more entries
 * @return 0    No further entries
 */
static int castle_ct_modlist_iter_has_next(c_modlist_iter_t *iter)
{
    return (!iter->err && (iter->next_item < iter->nr_items));
}

/**
 * Fill count entry pointers in dst_entry_idx from src_entry_idx.
 *
 * @param iter  Modlist iterator (provides src_entry_idx, dst_entry_idx)
 * @param src   Starting src_entry_idx entry to source entry pointers from
 * @param dst   Starting dst_entry_idx entry to populate from
 * @param count Number of entries to populate
 */
static inline void castle_ct_modlist_iter_merge_index_fill(c_modlist_iter_t *iter,
                                                           uint32_t src,
                                                           uint32_t dst,
                                                           uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++, src++, dst++)
    {
        iter->dst_entry_idx[dst].node        = iter->src_entry_idx[src].node;
        iter->dst_entry_idx[dst].node_offset = iter->src_entry_idx[src].node_offset;
    }
}

/**
 * Mergesort two contiguous entry ptr ranges (r1, r2) from src_entry_idx into dst_entry_idx.
 *
 * @param iter  Modlist iterator (provides src_entry_idx, dst_entry_idx)
 * @param r1    First range of node entry pointers
 * @param r2    Second range of node entry pointers
 *
 * - Iterate over entries pointed to by r1->start,r1->end and r2->start,r2->end
 *   from src_entry_idx[]
 * - Write out entry pointers in smallest to largest order into dst_entry_idx[]
 *   starting at index r1->start
 * - Result is that dst_entry_idx[r1->start] to dst_entry_idx[r2->end] will be
 *   sorted in smallest to largest order
 *
 * @also castle_ct_modlist_iter_mergesort()
 */
static void castle_ct_modlist_iter_merge_ranges(c_modlist_iter_t *iter,
                                                struct entry_range *r1,
                                                struct entry_range *r2)
{
    uint32_t r1_idx = r1->start;    /* current index for r1 */
    uint32_t r2_idx = r2->start;    /* current index for r2 */
    uint32_t dst_idx = r1->start;   /* output index */
    uint32_t src_idx = 0;           /* index of next smallest entry (from r1 or r2) */
    void *r1_key, *r2_key;
    version_t r1_ver, r2_ver;

    BUG_ON(r1->end+1 != r2->start); /* ranges *MUST* be contiguous */

    for (dst_idx = r1->start; dst_idx <= r2->end; dst_idx++)
    {
        /* Both ranges have more entries if their indexes lie within the range. */
        if (r1_idx <= r1->end && r2_idx <= r2->end)
        {
            /* Both ranges have more entries, we need to do a comparison to
             * determine which range has the next smallest value. */
            castle_ct_modlist_iter_item_get(iter, r1_idx, &r1_key, &r1_ver, NULL);
            castle_ct_modlist_iter_item_get(iter, r2_idx, &r2_key, &r2_ver, NULL);

            if (castle_kv_compare(iter->btree, r1_key, r1_ver, r2_key, r2_ver) < 0)
            {
                /* r1 smaller than r2. */
                src_idx = r1_idx;
                r1_idx++;
            }
            else
            {
                /* r1 larger than or equal to r2. */
                src_idx = r2_idx;
                r2_idx++;
            }

            /* Update dst_entry_idx with the smallest available entry pointer. */
            castle_ct_modlist_iter_merge_index_fill(iter, src_idx, dst_idx, 1);

            continue;
        }

        /* If we reached here then one of the two entry ranges has been
         * exhausted.  We need do no more comparisons and can just populate
         * the remainder of the output index with the entries from the range
         * that has not yet been exhausted. */

        if (r1_idx <= r1->end)
            castle_ct_modlist_iter_merge_index_fill(iter, r1_idx, dst_idx, r1->end-r1_idx+1);
        else if (r2_idx <= r2->end)
            castle_ct_modlist_iter_merge_index_fill(iter, r2_idx, dst_idx, r2->end-r2_idx+1);
        else
            BUG();

        /* We're done. */
        break;
    }
}

/**
 * Handler called when immutable iterator advances to a new source btree node.
 *
 * - Set modlist_iter->enum_advanced
 * - Provides a mechanism for the modlist iterator to know when the immutable
 *   iterator has advanced to a new node
 * - Used for sorting efficiency
 *
 * @also castle_ct_modlist_iter_fill()
 */
static void castle_ct_modlist_iter_next_node(c_immut_iter_t *immut_iter)
{
    c_modlist_iter_t *modlist_iter = immut_iter->private;
    modlist_iter->enum_advanced = 1;
}

/**
 * Populate node_buffer with leaf btree nodes, set up entry indexes and node ranges.
 *
 * - Using immutable iterator (iter->enumerator) iterate over entries in the
 *   unsorted btree
 * - Immutable iterator has a callback when it advances to a new btree node.
 *   castle_ct_modlist_iter_next_node() is registered as the callback handler
 *   and sets iter->enum_advanced whenever a new source node is used
 * - Get a new buffer btree node whenever the source iterator node advances
 * - Keep getting (unsorted) entries from the immutable iterator and store them
 *   in the node_buffer.  Put an entry in dst_entry_idx[] pointing to the node
 *   and node_offset
 * - As we move to a new node when the immutable iterator moves, we are
 *   guaranteed that individual btree nodes are sorted.  Fill ranges[] with
 *   start and end index within dst_entry_idx[]
 *
 * @also castle_ct_modlist_iter_mergesort()
 */
static void castle_ct_modlist_iter_fill(c_modlist_iter_t *iter)
{
    struct castle_btree_type *btree = iter->btree;
    struct castle_btree_node *node = NULL;
    uint32_t node_idx, item_idx, node_offset;
    version_t version;
    c_val_tup_t cvt;
    void *key;

    node_idx = item_idx = node_offset = 0;
    while (castle_ct_immut_iter.has_next(iter->enumerator))
    {
        might_resched();

        /* Get the next (unsorted) entry from the immutable iterator. */
        castle_ct_immut_iter.next(iter->enumerator, &key, &version, &cvt);
        debug("In enum got next: k=%p, version=%d, %u/%llu, cep="cep_fmt_str_nl,
                key, version, (uint32_t)cvt.type, cvt.length, cep2str(cvt.cep));
        debug("Dereferencing first 4 bytes of the key (should be length)=0x%x.\n",
                *((uint32_t *)key));
        debug("Inserting into the node=%d, under idx=%d\n", node_idx, node_offset);
        BUG_ON(CVT_LEAF_PTR(cvt));

        /* Advance to a new node if the immutable iterator has moved on.  This
         * is handled via the immutable iterator callback.  We rely on source
         * nodes being identically sized to our destination nodes. */
        if (iter->enum_advanced)
        {
            /* Set end entry for node range we just completed. */
            if (likely(node_idx))
                iter->ranges[node_idx-1].end = item_idx-1;
            /* Set start entry for node range we're moving to. */
            iter->ranges[node_idx].start = item_idx;

            /* Get a new node. */
            node = castle_ct_modlist_iter_buffer_get(iter, node_idx);
            castle_da_node_buffer_init(btree, node, btree->node_size(iter->tree, 0));

            /* We've advance, initialise a good state. */
            iter->enum_advanced = 0;
            node_offset = 0;
            node_idx++;
        }

        /* Insert entry into node. */
        btree->entry_add(node, node_offset, key, version, cvt);
        iter->dst_entry_idx[item_idx].node        = node_idx-1;
        iter->dst_entry_idx[item_idx].node_offset = node_offset;
        node_offset++;
        item_idx++;
    }

    if (likely(node_idx))
        iter->ranges[node_idx-1].end = item_idx-1;    /* FIXME this should be tidier */

    if (item_idx != atomic64_read(&iter->tree->item_count))
    {
        printk("Error. Different number of items than expected in CT=%d (dynamic=%d). "
               "Item_idx=%d, item_count=%ld\n",
            iter->tree->seq, iter->tree->dynamic,
            item_idx, atomic64_read(&iter->tree->item_count));
        WARN_ON(1);
    }
    iter->nr_items = item_idx;
    iter->nr_ranges = node_idx;
    //iter->err = iter->enumerator->err;
}

/**
 * Mergesort the underlying component tree into smallest->largest k,<-v order.
 *
 * T1 btrees are in insertion order but individual nodes have entries sorted in
 * k,<-v order.  To iterate over the btree we must first sort the whole tree.
 * This is done by merging leaf-nodes together repeatedly until we have a single
 * large k,<-v sorted set of entries.
 *
 * Internally the iterator uses:
 *
 * - node_buffer: contiguous buffer of btree leaf-nodes with entries
 * - src_entry_idx[], dst_entry_idx[]: two indirect indexes of entries within
 *   node_buffer.  We sort the data indirectly and hence for simplicity
 *   alternate src_entry_idx[] and dst_entry_idx[] for each round of merges
 * - ranges: ranges of entries within src_entry_idx[] that are guaranteed to
 *   be k,<-v sorted
 * - nr_ranges: number of ranges in src_entry_idx[]
 *
 * Mergesort implementation as follows:
 *
 * castle_ct_modlist_iter_fill() fills iter->entry_buffer with leaf-nodes from
 * the source btree.  For each entry that gets inserted into the buffer a
 * pointer to that entry goes into dst_entry_idx[].  Individual source btree
 * nodes are k,<-v sorted so we define ranges of entries on top of
 * dst_entry_idx[].  Each range encompasses the entries from a single source
 * btree node.  iter->nr_ranges contains the number of active ranges in
 * src_entry_idx[] (except after a fill when it is valid for dst_entry_idx[]).
 *
 * We go through the main mergesort loop until nr_ranges has reached 1 (single
 * sorted range of entries).  Each time we go through the loop we swap the src
 * and dst entry_idx[] such that src_entry_idx[] contains the most up-to-date
 * sorted data we have available.
 *
 * Take two ranges of entries and merge them together in _merge_ranges().  This
 * takes the entries from src_entry_idx[] and writes out sorted entries into
 * dst_entry_idx[].
 *
 * Update ranges[] with the new range start and end (new range start will be
 * range1.start and end will be range2.end - ranges must be contiguous).
 *
 * If we have an uneven number of ranges move the entry pointers from src_
 * to dst_entry_idx[] and ensure the range points to the correct entries.  No
 * merge is performed in this instance.  @FIXME this is inefficient
 *
 * Update the total number of ranges and go again if necessary.
 *
 * @also castle_ct_modlist_iter_fill()
 * @also castle_ct_modlist_iter_merge_ranges()
 * @also castle_ct_modlist_iter_init()
 */
static void castle_ct_modlist_iter_mergesort(c_modlist_iter_t *iter)
{
    uint32_t src_range, dst_range;
    void *tmp_entry_idx;

    /* Populate internal entry buffer and initialise dst_entry_idx[] and the
     * initial node ranges for sorting. */
    castle_ct_modlist_iter_fill(iter);

    /* Repeatedly merge ranges of entry pointers until we have a single
     * all-encompassing smallest->largest sorted range we can use to return
     * entries when the iterator .has_next(), .next() functions are called. */
    while (iter->nr_ranges > 1)
    {
        /* Another merge.  Swap the src and dst entry indexes around.
         * We will now be sourcing from the previous iteration's dst_entry_idx
         * (also used by castle_ct_modlist_iter_fill()) and writing our values
         * out to our previous source. */
        tmp_entry_idx = iter->src_entry_idx;
        iter->src_entry_idx = iter->dst_entry_idx;  /* src = dst */
        iter->dst_entry_idx = tmp_entry_idx;        /* dst = src */

        src_range = dst_range = 0;

        /* So long as we have two remaining entry ranges, mergesort the entries
         * together to create a single range spanning the capacity of both. */
        while (src_range+1 < iter->nr_ranges)
        {
            /* Mergesort. */
            castle_ct_modlist_iter_merge_ranges(iter,
                                                &iter->ranges[src_range],
                                                &iter->ranges[src_range+1]);

            /* Update the destination range. */
            iter->ranges[dst_range].start = iter->ranges[src_range].start;
            iter->ranges[dst_range].end   = iter->ranges[src_range+1].end;

            src_range += 2;
            dst_range++;
        }

        /* Above we merged pairs of ranges.  Part of the merge process (handled
         * within castle_ct_modlist_iter_merge_ranges() is to populate the
         * dst_entry_idx.  If we started with an odd number of ranges we must
         * deal with the straggling range as a special case. */
        if (src_range < iter->nr_ranges)
        {
            /* We only have one range to merge so we fake up a range that
             * castle_ct_modlist_iter_merge_ranges() will determine to be
             * exhausted and therefore will populate dst_entry_idx with only
             * those entries from our one remaining src_range. */
            struct entry_range null_range;

            /* Mergesort. */
            null_range.start = iter->ranges[src_range].end+1;
            null_range.end   = iter->ranges[src_range].end;
            castle_ct_modlist_iter_merge_ranges(iter,
                                                &iter->ranges[src_range],
                                                &null_range);

            /* Update the destination range. */
            iter->ranges[dst_range].start = iter->ranges[src_range].start;
            iter->ranges[dst_range].end   = iter->ranges[src_range].end;

            src_range++;
            dst_range++;
        }
        /* else even number of source ranges */

        iter->nr_ranges = dst_range;
    }

    /* Finally ensure dst_entry_idx points to the final sorted index and free
     * the other temporary index right now. */
    castle_vfree(iter->src_entry_idx);
    iter->src_entry_idx = iter->dst_entry_idx;
    iter->dst_entry_idx = NULL;
}

/**
 * Initialise modlist btree iterator.
 *
 * See castle_ct_modlist_iter_mergesort() for full implementation details.
 *
 * - Initialise members
 * - Allocate memory for node_buffer, src_ and dst_entry_idx[] and ranges
 * - Initialise immutable iterator (for sort)
 * - Kick of mergesort
 *
 * @also castle_ct_modlist_iter_mergesort()
 */
static void castle_ct_modlist_iter_init(c_modlist_iter_t *iter)
{
    struct castle_component_tree *ct = iter->tree;

    BUG_ON(atomic64_read(&ct->item_count) == 0);
    BUG_ON(!ct); /* component tree must be provided */

    iter->err = 0;
    iter->btree = castle_btree_type_get(ct->btree_type);
    iter->leaf_node_size = iter->btree->node_size(ct, 0);

    /* Allocate immutable iterator.
     * For iterating over source entries during sort. */
    iter->enumerator = castle_malloc(sizeof(c_immut_iter_t), GFP_KERNEL);

    /* Allocate btre-entry buffer, two indexes for the buffer (for sorting)
     * and space to define ranges of sorted nodes within the index. */
    iter->nr_nodes = 1.1 * (atomic64_read(&ct->node_count) + 1); /* a few extra for luck! */
    iter->node_buffer = castle_vmalloc(iter->nr_nodes * iter->leaf_node_size * C_BLK_SIZE);
    iter->src_entry_idx = castle_vmalloc(atomic64_read(&ct->item_count) * sizeof(struct item_idx));
    iter->dst_entry_idx = castle_vmalloc(atomic64_read(&ct->item_count) * sizeof(struct item_idx));
    iter->ranges = castle_vmalloc(iter->nr_nodes * sizeof(struct entry_range));
    if(!iter->enumerator || !iter->node_buffer || !iter->src_entry_idx || !iter->dst_entry_idx)
    {
        castle_ct_modlist_iter_free(iter);
        iter->err = -ENOMEM;
        return;
    }

    /* Initialise the immutable iterator */
    iter->enumerator->tree = ct;
    castle_ct_immut_iter_init(iter->enumerator, castle_ct_modlist_iter_next_node, iter);

    /* Finally, sort the data so we can return sorted entries to the caller. */
    castle_ct_modlist_iter_mergesort(iter);

    /* Good state before we accept requests. */
    iter->err = 0;
    iter->next_item = 0;
}

struct castle_iterator_type castle_ct_modlist_iter = {
    .register_cb = NULL,
    .prep_next   = NULL,
    .has_next    = (castle_iterator_has_next_t)castle_ct_modlist_iter_has_next,
    .next        = (castle_iterator_next_t)    castle_ct_modlist_iter_next,
    .skip        = NULL,
};

/**
 * Insert a kv pair into RB tree. Delete the oldest entry if found a duplicate. 
 *
 * @param iter [in] merged iterator that the RB tree belongs to
 * @param comp_iter [in] component iterator that the new kv pair belongs to
 */
static int castle_ct_merged_iter_rbtree_insert(c_merged_iter_t *iter, 
                                               struct component_iterator *comp_iter)
{
    struct rb_root *root = &iter->rb_root;
    struct rb_node **p = &root->rb_node;
    struct rb_node *parent = NULL;
    struct rb_node *node = &comp_iter->rb_node;
    int nr_cmps = 0;
    int ret = 0;

    /* Go until end of the tree. */
    while (*p)
    {
        struct component_iterator *c_iter, *dup_iter = NULL;
        int kv_cmp;

        parent = *p;
        c_iter = rb_entry(parent, struct component_iterator, rb_node);

        BUG_ON(!c_iter->cached);
        BUG_ON(c_iter == comp_iter);

        /* Compare the entry in RB Tree with new entry. */
        kv_cmp = castle_kv_compare(iter->btree,
                                   comp_iter->cached_entry.k,
                                   comp_iter->cached_entry.v,
                                   c_iter->cached_entry.k,
                                   c_iter->cached_entry.v);
        nr_cmps++;

        /* New key is smaller than the key in tree. Traverse left. */
        if (kv_cmp < 0)
            p = &(*p)->rb_left;
        /* New key is bigger than the key in tree. Traverse right. */
        else if (kv_cmp > 0)
            p = &(*p)->rb_right;
        /* Both kv pairs are equal. Find the newest element. Iterators are
         * allocated in an array with the iterator of latest CT coming first.
         * So, compare pointers and smallest pointer is latest. */
        else if (c_iter > comp_iter)
        {
            /* If the new key is the latest, then jsut replace the one in
             * rb-tree with the new key. */
            rb_replace_node(&c_iter->rb_node, &comp_iter->rb_node, root);
            dup_iter = c_iter;
        }
        else
        {
            ret = 1;
            dup_iter = comp_iter;
        }

        /* Skip the duplicated entry and clear cached bit of the component
         * iterator. */
        if (dup_iter)
        {
            debug("Duplicate entry found. Removing.\n");
            if (iter->each_skip)
                iter->each_skip(iter, dup_iter);
            dup_iter->cached = 0;
            return ret;
        }
    }

    /* Link the node to tree. */
    rb_link_node(node, parent, p);
    /* Set color and inturn balance the tree. */
    rb_insert_color(node, root);

    return ret;
}

static struct component_iterator * castle_ct_merge_iter_rbtree_min_del(c_merged_iter_t *iter)
{
    struct rb_root *root = &iter->rb_root;
    struct rb_node *min;

    /* Get the first element in the sorted order(minimum). */
    min = rb_first(root);
    BUG_ON(!min);

    /* Erase the element from tree. */
    rb_erase(min, root);

    /* Return component tree. */
    return rb_entry(min, struct component_iterator, rb_node);
}

static void castle_ct_merge_iter_rbtree_remove(c_merged_iter_t *iter,
                                               struct component_iterator *comp_iter)
{
    struct rb_root *root = &iter->rb_root;
    struct rb_node *node = &comp_iter->rb_node;

    /* Erase the element from tree. */
    rb_erase(node, root);
}

static int _castle_ct_merged_iter_prep_next(c_merged_iter_t *iter,
                                            int sync_call)
{
    int i;
    struct component_iterator *comp_iter;

    debug_iter("No of comp_iters: %u\n", iter->nr_iters);
    for(i=0; i<iter->nr_iters; i++)
    {
        comp_iter = iter->iterators + i; 

        debug_iter("%s:%p:%d\n", __FUNCTION__, iter, i);
        /* Replenish the cache */
        if(!comp_iter->completed && !comp_iter->cached)
        {
            debug("Reading next entry for iterator: %d.\n", i);
            if (!sync_call &&
                !comp_iter->iterator_type->prep_next(comp_iter->iterator)) {
                debug_iter("%s:%p:%p:%d - schedule\n", __FUNCTION__, iter, comp_iter->iterator, i);
                return 0;
            }
            if (comp_iter->iterator_type->has_next(comp_iter->iterator))
            {
                comp_iter->iterator_type->next(comp_iter->iterator,
                                               &comp_iter->cached_entry.k,
                                               &comp_iter->cached_entry.v,
                                               &comp_iter->cached_entry.cvt);
                comp_iter->cached = 1;
                iter->src_items_completed++;
                debug_iter("%s:%p:%d - cached\n", __FUNCTION__, iter, i);
                /* Insert the kv pair into RB tree. */ 
                /* It is possible that. this call could delete kv pairs of the component
                 * iterators (which is fine, as we go through that component iterator anyway)
                 * coming after this or it could delete the current kv pair itself. */
                if (castle_ct_merged_iter_rbtree_insert(iter, comp_iter))
                {
                    /* If the current kv pair is deleted, get the next entry in this 
                     * iterator. */
                    i--;
                    continue;
                }
            }
            else
            {
                debug_iter("%s:%p:%d - nothing left\n", __FUNCTION__, iter, i);
                comp_iter->completed = 1;
                iter->non_empty_cnt--;
                debug("A component iterator run out of stuff, we are left with"
                      "%d iterators.\n",
                      iter->non_empty_cnt);
            }
        }
    }

    return 1;
}

static void castle_ct_merged_iter_register_cb(c_merged_iter_t *iter,
                                              castle_iterator_end_io_t cb,
                                              void *data)
{
    iter->end_io  = cb;
    iter->private = data;
}

static int castle_ct_merged_iter_prep_next(c_merged_iter_t *iter)
{
    debug_iter("%s:%p\n", __FUNCTION__, iter);
    return _castle_ct_merged_iter_prep_next(iter, 0);
}

static void castle_ct_merged_iter_end_io(void *rq_enum_iter, int err)
{
    c_merged_iter_t *iter = ((c_rq_enum_t *) rq_enum_iter)->private;

    debug_iter("%s:%p\n", __FUNCTION__, iter);
    if (castle_ct_merged_iter_prep_next(iter))
    {
        debug_iter("%s:%p - Done\n", __FUNCTION__, iter);
        iter->end_io(iter, 0);
        return;
    }
}

static int castle_ct_merged_iter_has_next(c_merged_iter_t *iter)
{
    debug_iter("%s:%p\n", __FUNCTION__, iter);
    BUG_ON(!_castle_ct_merged_iter_prep_next(iter, 1));
    debug("Merged iterator has next, err=%d, non_empty_cnt=%d\n", 
            iter->err, iter->non_empty_cnt);
    return (!iter->err && (iter->non_empty_cnt > 0));
}

static void castle_ct_merged_iter_next(c_merged_iter_t *iter,
                                       void **key_p,
                                       version_t *version_p,
                                       c_val_tup_t *cvt_p)
{
    struct component_iterator *comp_iter; 

    debug_iter("%s:%p\n", __FUNCTION__, iter);
    debug("Merged iterator next.\n");

    /* Get the smallest kv pair from RB tree. */ 
    comp_iter = castle_ct_merge_iter_rbtree_min_del(iter);
    debug("Smallest entry is from iterator: %p.\n", comp_iter);
    comp_iter->cached = 0;

    /* Return the smallest entry */
    if(key_p) *key_p = comp_iter->cached_entry.k;
    if(version_p) *version_p = comp_iter->cached_entry.v;
    if(cvt_p) *cvt_p = comp_iter->cached_entry.cvt;
}

static void castle_ct_merged_iter_skip(c_merged_iter_t *iter,
                                       void *key)
{
    struct component_iterator *comp_iter; 
    int i, skip_cached;

    debug_iter("%s:%p\n", __FUNCTION__, iter);
    /* Go through iterators, and do the following:
       * call skip in each of the iterators
       * check if we have something cached
       * if we do, and the cached k < key, flush it
     */
    for(i=0; i<iter->nr_iters; i++)
    {
        comp_iter = iter->iterators + i; 
        if(comp_iter->completed)
            continue;

        /* Check if the cached entry needs to be skipped AHEAD of the skip
           being called on the appropriate component iterator (which may 
           invalidate the cached key pointer */
        skip_cached = comp_iter->cached && 
                     (iter->btree->key_compare(comp_iter->cached_entry.k, key) < 0);
        /* Next skip in the component iterator */
        BUG_ON(!comp_iter->iterator_type->skip);

        /* If cached entry is not being skipped, bigger than the skip key, then no
         * need to call skip on low level iterator. */
        /* Flush cached entry if it was to small (this doesn't inspect the cached entry
           any more). */
        if (skip_cached) 
        {
            comp_iter->iterator_type->skip(comp_iter->iterator, key);
            BUG_ON(iter->each_skip);
            if (comp_iter->cached)
            {
                castle_ct_merge_iter_rbtree_remove(iter, comp_iter);
                comp_iter->cached = 0;
            }
        }
    }
}

static void castle_ct_merged_iter_cancel(c_merged_iter_t *iter)
{
    castle_free(iter->iterators);
}

/**
 * Initialise a meta iterator from a number of component iterators.
 *
 * Once initialised the iterator will return the smallest entry from any of the
 * component trees when castle_ct_merged_iter_next() is called.
 *
 * This iterator is used for merges and range queries (non-exhaustive list).
 */
static void castle_ct_merged_iter_init(c_merged_iter_t *iter,
                                       void **iterators,
                                       struct castle_iterator_type **iterator_types,
                                       castle_merged_iterator_each_skip each_skip)
{
    int i;

    debug("Initing merged iterator for %d component iterators.\n", iter->nr_iters);
    BUG_ON(iter->nr_iters <= 0);
    BUG_ON(!iter->btree);
    iter->err = 0;
    iter->src_items_completed = 0;
    iter->end_io = NULL;
    iter->rb_root = RB_ROOT;
    iter->iterators = castle_malloc(iter->nr_iters * sizeof(struct component_iterator), GFP_KERNEL);
    if(!iter->iterators)
    {
        printk("Failed to allocate memory for merged iterator.\n");
        iter->err = -ENOMEM;
        return;
    }
    iter->each_skip = each_skip;
    /* Memory allocated for the iterators array, init the state. 
       Assume that all iterators have something in them, and let the has_next_check() 
       handle the opposite. */
    iter->non_empty_cnt = iter->nr_iters; 
    for(i=0; i<iter->nr_iters; i++)
    {
        struct component_iterator *comp_iter = iter->iterators + i; 

        comp_iter->iterator      = iterators[i];
        comp_iter->iterator_type = iterator_types[i];
        comp_iter->cached        = 0;
        comp_iter->completed     = 0;

        if (comp_iter->iterator_type->register_cb)
            comp_iter->iterator_type->register_cb(comp_iter->iterator,
                                                  castle_ct_merged_iter_end_io,
                                                  (void *)iter);
    } 
}

struct castle_iterator_type castle_ct_merged_iter = {
    .register_cb = (castle_iterator_register_cb_t)castle_ct_merged_iter_register_cb,
    .prep_next   = (castle_iterator_prep_next_t)  castle_ct_merged_iter_prep_next,
    .has_next    = (castle_iterator_has_next_t)   castle_ct_merged_iter_has_next,
    .next        = (castle_iterator_next_t)       castle_ct_merged_iter_next,
    .skip        = (castle_iterator_skip_t)       castle_ct_merged_iter_skip, 
    .cancel      = (castle_iterator_cancel_t)     castle_ct_merged_iter_cancel, 
};


#ifdef DEBUG
c_modlist_iter_t test_iter1;
c_modlist_iter_t test_iter2;
c_merged_iter_t  test_miter;
static USED void castle_ct_sort(struct castle_component_tree *ct1,
                                struct castle_component_tree *ct2)
{
    version_t version;
    void *key;
    c_val_tup_t cvt;
    int i=0;
    void *iters[2];
    struct castle_iterator_type *iter_types[2];

    debug("Number of items in the component tree1: %ld, number of nodes: %ld, ct2=%ld, %ld\n", 
            atomic64_read(&ct1->item_count),
            atomic64_read(&ct1->node_count),
            atomic64_read(&ct2->item_count),
            atomic64_read(&ct2->node_count));

    test_iter1.tree = ct1;
    castle_ct_modlist_iter_init(&test_iter1);
    test_iter2.tree = ct2;
    castle_ct_modlist_iter_init(&test_iter2);

#if 0
    while(castle_ct_modlist_iter_has_next(&test_iter))
    {
        castle_ct_modlist_iter_next(&test_iter, &key, &version, &cep); 
        debug("Sorted: %d: k=%p, version=%d, cep=(0x%x, 0x%x)\n",
                i, key, version, cep.ext_id, cep.offset);
        debug("Dereferencing first 4 bytes of the key (should be length)=0x%x.\n",
                *((uint32_t *)key));
        i++;
    }
#endif
    test_miter.nr_iters = 2;
    test_miter.btree = test_iter1.btree;
    iters[0] = &test_iter1;
    iters[1] = &test_iter2;
    iter_types[0] = &castle_ct_modlist_iter;
    iter_types[1] = &castle_ct_modlist_iter;
    castle_ct_merged_iter_init(&test_miter,
                               iters,
                               iter_types, 
                               NULL);
    debug("=============== SORTED ================\n");
    while(castle_ct_merged_iter_has_next(&test_miter))
    {
        castle_ct_merged_iter_next(&test_miter, &key, &version, &cvt);
        debug("Sorted: %d: k=%p, version=%d, cep=" cep_fmt_str_nl,
                i, key, version, cep2str(cvt.cep));
        debug("Dereferencing first 4 bytes of the key (should be length)=0x%x.\n",
                *((uint32_t *)key));
        i++;
    }
}
#endif

/* Has next, next and skip only need to call the corresponding functions on
   the underlying merged iterator */

static void castle_da_rq_iter_register_cb(c_da_rq_iter_t *iter,
                                          castle_iterator_end_io_t cb,
                                          void *data)
{
    iter->end_io  = cb;
    iter->private = data;
}

static int castle_da_rq_iter_prep_next(c_da_rq_iter_t *iter)
{
    return castle_ct_merged_iter_prep_next(&iter->merged_iter);
} 


static int castle_da_rq_iter_has_next(c_da_rq_iter_t *iter)
{
    return castle_ct_merged_iter_has_next(&iter->merged_iter);
} 

static void castle_da_rq_iter_end_io(void *merged_iter, int err)
{
    c_da_rq_iter_t *iter = ((c_merged_iter_t *)merged_iter)->private;

    if (castle_da_rq_iter_prep_next(iter))
    {
        iter->end_io(iter, 0);
        return;
    }
    else
        BUG();
}

static void castle_da_rq_iter_next(c_da_rq_iter_t *iter,
                                   void **key_p,
                                   version_t *version_p,
                                   c_val_tup_t *cvt_p)
{
    castle_ct_merged_iter_next(&iter->merged_iter, key_p, version_p, cvt_p);
}

static void castle_da_rq_iter_skip(c_da_rq_iter_t *iter, void *key)
{
    castle_ct_merged_iter_skip(&iter->merged_iter, key);
}

void castle_da_rq_iter_cancel(c_da_rq_iter_t *iter)
{
    int i;

    castle_ct_merged_iter_cancel(&iter->merged_iter);
    for(i=0; i<iter->nr_cts; i++)
    {
        struct ct_rq *ct_rq = iter->ct_rqs + i;
        castle_btree_rq_enum_cancel(&ct_rq->ct_rq_iter);
        castle_ct_put(ct_rq->ct, 0);
    }
    castle_free(iter->ct_rqs);
}

/**
 * Range query iterator initialiser.
 *
 * Implemented as a merged iterator of CTs at every level of the doubling array.
 */
void castle_da_rq_iter_init(c_da_rq_iter_t *iter,
                            version_t version,
                            da_id_t da_id,
                            void *start_key,
                            void *end_key)
{
    void **iters;
    struct castle_iterator_type **iter_types;
    struct castle_double_array *da;
    struct list_head *l;
    int i, j;

    da = castle_da_hash_get(da_id);
    BUG_ON(!da);
    BUG_ON(!castle_version_is_ancestor(da->root_version, version));
again:
    /* Try to allocate the right amount of memory, but remember that nr_trees
       may change, because we are not holding the da lock (cannot kmalloc holding
       a spinlock). */
    iter->nr_cts = da->nr_trees;
    iter->err    = 0;
    iter->end_io = NULL;
    iter->ct_rqs = castle_zalloc(iter->nr_cts * sizeof(struct ct_rq), GFP_KERNEL);
    iters        = castle_malloc(iter->nr_cts * sizeof(void *), GFP_KERNEL);
    iter_types   = castle_malloc(iter->nr_cts * sizeof(struct castle_iterator_type *), GFP_KERNEL);
    if(!iter->ct_rqs || !iters || !iter_types)
    {
        if(iter->ct_rqs)
            castle_free(iter->ct_rqs);
        if(iters)
            castle_free(iters);
        if(iter_types)
            castle_free(iter_types);
        iter->err = -ENOMEM;
        return;
    }

    read_lock(&da->lock);
    /* Check the number of trees under lock. Retry again if # changed. */ 
    if(iter->nr_cts != da->nr_trees)
    {
        read_unlock(&da->lock);
        printk("Warning. Untested path. # of cts changed while allocating memory for rq.\n");
        castle_free(iter->ct_rqs);
        castle_free(iters);
        castle_free(iter_types);
        goto again;
    }
    /* Get refs to all the component trees, and release the lock */
    j=0;
    for(i=0; i<MAX_DA_LEVEL; i++)
    {
        list_for_each(l, &da->levels[i].trees)
        {
            struct castle_component_tree *ct;

            BUG_ON(j >= iter->nr_cts);
            ct = list_entry(l, struct castle_component_tree, da_list);
            iter->ct_rqs[j].ct = ct; 
            castle_ct_get(ct, 0);
            BUG_ON((castle_btree_type_get(ct->btree_type)->magic != RW_VLBA_TREE_TYPE) &&
                   (castle_btree_type_get(ct->btree_type)->magic != RO_VLBA_TREE_TYPE));
            j++;
        }
    }
    read_unlock(&da->lock);
    BUG_ON(j != iter->nr_cts);

    /* Initialise range queries for individual cts */
    /* TODO: Better to re-organize the code, such that these iterators belong to
     * merged iterator. Easy to manage resources - Talk to Gregor */
    for(i=0; i<iter->nr_cts; i++)
    {
        struct ct_rq *ct_rq = iter->ct_rqs + i;

        castle_btree_rq_enum_init(&ct_rq->ct_rq_iter,
                                   version,
                                   ct_rq->ct,
                                   start_key,
                                   end_key);
        /* TODO: handle errors! Don't know how to destroy ct_rq_iter ATM. */
        BUG_ON(ct_rq->ct_rq_iter.err);
        iters[i]        = &ct_rq->ct_rq_iter;
        iter_types[i]   = &castle_btree_rq_iter;
    }

    /* Iterators have been initialised, now initialise the merged iterator */
    iter->merged_iter.nr_iters = iter->nr_cts;
    iter->merged_iter.btree    = castle_btree_type_get(RO_VLBA_TREE_TYPE);
    castle_ct_merged_iter_init(&iter->merged_iter,
                                iters,
                                iter_types,
                                NULL);
    castle_ct_merged_iter_register_cb(&iter->merged_iter, 
                                      castle_da_rq_iter_end_io,
                                      iter);
    castle_free(iters);
    castle_free(iter_types);
}

struct castle_iterator_type castle_da_rq_iter = {
    .register_cb= (castle_iterator_register_cb_t)castle_da_rq_iter_register_cb,
    .prep_next  = (castle_iterator_prep_next_t)  castle_da_rq_iter_prep_next,
    .has_next   = (castle_iterator_has_next_t)   castle_da_rq_iter_has_next,
    .next       = (castle_iterator_next_t)       castle_da_rq_iter_next,
    .skip       = (castle_iterator_skip_t)       castle_da_rq_iter_skip,
    .cancel     = (castle_iterator_cancel_t)     castle_da_rq_iter_cancel, 
};

/**********************************************************************************************/
/* Merges */
struct castle_da_merge {
    struct castle_double_array   *da;
    struct castle_btree_type     *out_btree;
    int                           level;
    int                           nr_trees;     /**< num of component trees being merged */
    struct castle_component_tree **in_trees;    /**< array of component trees to be merged */
    struct castle_component_tree *out_tree;
    void                         **iters;       /**< iterators for component trees */
    c_merged_iter_t              *merged_iter;
    int                           root_depth;
    c2_block_t                   *last_node_c2b;
    c2_block_t                   *last_leaf_node_c2b; /**< Previous node c2b at depth 0. */
    void                         *last_key;           /**< last_key added to
                                                           out tree at depth 0. */
    c_ext_pos_t                   first_node;
    uint16_t                      first_node_size;
    int                           completing;
    uint64_t                      nr_entries;
    uint64_t                      nr_nodes;
    uint64_t                      large_chunks; 
    int                           is_new_key;      /**< Is the current key different
                                                        from last key added to out_tree. */
    struct castle_da_merge_level {
        /* Node we are currently generating, and book-keeping variables about the node. */
        c2_block_t               *node_c2b;
        void                     *last_key;
        int                       next_idx;
        int                       valid_end_idx;
        version_t                 valid_version;
    } levels[MAX_BTREE_DEPTH];

    /* Deamortization variables */
    struct work_struct            work;
    int                           budget_cons_rate;
    int                           budget_cons_units;
    int                           ssds_used;       /**< set to true if at least some btree
                                                        nodes will be stored on SSDs.   */
    c_ext_free_t                  internal_ext_free;
    c_ext_free_t                  tree_ext_free;
    c_ext_free_t                  data_ext_free;
    int                           bloom_exists;
    castle_bloom_t                bloom;
    struct list_head              large_objs;

    struct castle_version_delete_state snapshot_delete; /**< snapshot delete state. */

#ifdef CASTLE_PERF_DEBUG
    u64                           get_c2b_ns;       /**< ns in castle_cache_block_get() */
    u64                           merged_iter_next_ns;
    u64                           da_medium_obj_copy_ns;
    u64                           nodes_complete_ns;
    u64                           budget_consume_ns;
    u64                           progress_update_ns;
    u64                           merged_iter_next_hasnext_ns;
    u64                           merged_iter_next_compare_ns;
#endif
#ifdef CASTLE_DEBUG
    uint8_t                       is_recursion;
#endif
    uint32_t                      skipped_count;    /**< num of entries from deleted versions. */
};

#define MAX_IOS             (1000) /* Arbitrary constants */
/* TODO: Merges are now effectively always full throughput, because MIN is set high. */ 
#define MIN_BUDGET_DELTA    (1000000)
#define MAX_BUDGET          (1000000)
#define BIG_MERGE           (0)

/************************************/
/* Marge rate control functionality */
static void castle_da_merge_budget_consume(struct castle_da_merge *merge)
{
    struct castle_double_array *da;

    BUG_ON(in_atomic());
    if(castle_da_exiting)
        return;

    /* Check if we need to consume some merge budget */
    merge->budget_cons_units++;
    if(merge->budget_cons_units < merge->budget_cons_rate)
        return;

    da = merge->da;
    /* Consume a single unit of budget. */
    while(atomic_dec_return(&da->merge_budget) < 0)
    {
        /* We failed to get merge budget, readd the unit, and wait for some to appear. */
        atomic_inc(&da->merge_budget);
        /* Extra warning message, which we shouldn't see. Increase the MIN, if we do. */
        printk("WARNING, possible error: Merges running fast, but not throttling.\n");
        atomic_add(MIN_BUDGET_DELTA, &da->merge_budget);
        return;
        //wait_event(da->merge_budget_waitq, atomic_read(&da->merge_budget) > 0);
    }
}

#define REPLENISH_FREQUENCY (10)        /* Replenish budgets every 100ms. */
static int castle_da_merge_budget_replenish(struct castle_double_array *da, void *unused)
{
    int ios = atomic_read(&da->epoch_ios);
    int budget_delta = 0, merge_budget;

    atomic_set(&da->epoch_ios, 0);
    debug("Merge replenish, number of ios in last second=%d.\n", ios);
    if(ios < MAX_IOS) 
        budget_delta = MAX_IOS - ios;
    if(budget_delta < MIN_BUDGET_DELTA)
        budget_delta = MIN_BUDGET_DELTA;
    BUG_ON(budget_delta <= 0);
    merge_budget = atomic_add_return(budget_delta, &da->merge_budget);
    if(merge_budget > MAX_BUDGET)
        atomic_sub(merge_budget - MAX_BUDGET, &da->merge_budget);
    wake_up(&da->merge_budget_waitq);

    return 0;
}

static void castle_merge_budgets_replenish(void *unused)
{
   castle_da_hash_iterate(castle_da_merge_budget_replenish, NULL); 
}

/**
 * Replenish ios_budget from ios_rate and schedule IO wait queue kicks.
 *
 * NOTE: This might remove rather than replenish the budget, depending on
 * whether inserts are enabled/disabled(/throttled) on the DA.
 *
 * ios_rate is used to throttle inserts into the btree.  It is used as an
 * initialiser for ios_budget.
 *
 * This function is expected to be called periodically (e.g. via a timer) with
 * values of ios_rate that maintain a sustainable flow of inserts.
 *
 * - Update ios_budget
 * - Schedule queue kicks for all IO wait queues that have elements
 *
 * @also struct castle_double_array
 * @also castle_da_queue_kick()
 */
static int castle_da_ios_budget_replenish(struct castle_double_array *da, void *unused)
{
    int i;

    atomic_set(&da->ios_budget, da->ios_rate);

    if (da->ios_rate)
    {
        /* We just replenished the DA's ios_budget.
         *
         * We need to kick all of the write IO wait queues.  In the current
         * context we hold spin_lock_irq(&castle_da_hash_lock) so schedule this
         * work so we can drop the lock and return immediately. */
        for (i = 0; i < request_cpus.cnt; i++)
        {
            struct castle_da_io_wait_queue *wq = &da->ios_waiting[i];

            spin_lock(&wq->lock);
            if (!list_empty(&wq->list))
                queue_work_on(request_cpus.cpus[i], castle_wqs[19], &wq->work);
            spin_unlock(&wq->lock);
        }
    }

    return 0;
}

/**
 * Replenish ios_budget for all DAs on the system.
 */
static void castle_ios_budgets_replenish(void *unused)
{
   castle_da_hash_iterate(castle_da_ios_budget_replenish, NULL); 
}

static inline void castle_da_merge_budget_io_end(struct castle_double_array *da)
{
    atomic_inc(&da->epoch_ios);
}

static DECLARE_WORK(merge_budgets_replenish_work, castle_merge_budgets_replenish, NULL);
static DECLARE_WORK(ios_budgets_replenish_work, castle_ios_budgets_replenish, NULL);

/************************************/
/* Throttling timers */
static struct timer_list throttle_timer; 
static void castle_throttle_timer_fire(unsigned long first)
{
    schedule_work(&merge_budgets_replenish_work);
    schedule_work(&ios_budgets_replenish_work);
    /* Reschedule ourselves */
    setup_timer(&throttle_timer, castle_throttle_timer_fire, 0);
    mod_timer(&throttle_timer, jiffies + HZ/REPLENISH_FREQUENCY);
}

/************************************/
/* Actual merges */
static void castle_da_iterator_destroy(struct castle_component_tree *tree,
                                       void *iter)
{
    if(!iter)
        return;

    if(tree->dynamic)
    {
        /* For dynamic trees we are using modlist iterator. */ 
        castle_ct_modlist_iter_free(iter);
        castle_free(iter);
    } else
    {
        /* For static trees, we are using immut iterator. */
        /* TODO: do we need to do better resource release here? */
        castle_ct_immut_iter_cancel(iter);
        castle_free(iter);
    }
}

/**
 * Allocate/initialise correct iterator type for level of merge.
 *
 * - Allocate a castle_ct_modlist_iter for T1 merges
 * - Allocate a castle_ct_immut_iter for all higher level merges
 */
static void castle_da_iterator_create(struct castle_da_merge *merge,
                                      struct castle_component_tree *tree,
                                      void **iter_p)
{
    if (tree->dynamic)
    {
        c_modlist_iter_t *iter = castle_malloc(sizeof(c_modlist_iter_t), GFP_KERNEL);
        if (!iter)
            return;
        iter->tree = tree;
        iter->merge = merge; 
        if (tree->level == 1)
            castle_trace_da_merge(TRACE_START, TRACE_DA_MERGE_MODLIST_ITER_INIT_ID,
                    merge->da->id, tree->level, 0, 0);
        castle_ct_modlist_iter_init(iter);
        if (tree->level == 1)
            castle_trace_da_merge(TRACE_END, TRACE_DA_MERGE_MODLIST_ITER_INIT_ID,
                    merge->da->id, tree->level, 0, 0);
        if (iter->err)
        {
            castle_da_iterator_destroy(tree, iter);
            return;
        }
        /* Success */
        *iter_p = iter; 
    }
    else
    {
        c_immut_iter_t *iter = castle_malloc(sizeof(c_immut_iter_t), GFP_KERNEL);
        if (!iter)
            return;
        iter->tree = tree;
        castle_ct_immut_iter_init(iter, NULL, NULL);
        /* TODO: after init errors? */
        *iter_p = iter;
    }
}
        
static struct castle_iterator_type* castle_da_iter_type_get(struct castle_component_tree *ct)
{
    if(ct->dynamic)
        return &castle_ct_modlist_iter;
    else
        return &castle_ct_immut_iter;
}

static void castle_da_each_skip(c_merged_iter_t *iter,  
                                struct component_iterator *comp_iter)
{
    BUG_ON(!comp_iter->cached);

    if (CVT_LARGE_OBJECT(comp_iter->cached_entry.cvt))
    {
        /* No need to remove this large object, it gets deleted part of Tree
         * deletion. */
    }
}

/**
 * Creates iterators for each of the input trees. And merged iterator used to
 * construct the output tree.
 *
 * Doesn't cleanup half-created state on failure. It is done by castle_da_merge_dealloc() 
 * which would be called from castle_da_merge_init.
 *
 * @param merge [in] merge to be created
 *
 * @return non-zero if failed to create iterators
 *
 * @see castle_da_merge_init
 * @see castle_da_merge_dealloc
 */
static int castle_da_iterators_create(struct castle_da_merge *merge)
{
    struct castle_btree_type *btree;
    int ret;
    struct castle_iterator_type *iter_types[merge->nr_trees]; 
    int i;

    /* Make sure iter_types is not too big. Its on stack. */
    BUG_ON(sizeof(iter_types) > 512);

    printk("Creating iterators for the merge.\n");
    FOR_EACH_MERGE_TREE(i, merge)
        BUG_ON(!merge->in_trees[i]);

    btree = castle_btree_type_get(merge->in_trees[0]->btree_type);

    /* Wait until there are no outstanding writes on the trees */
    FOR_EACH_MERGE_TREE(i, merge)
    {
        while(atomic_read(&merge->in_trees[i]->write_ref_count))
        {
            debug("Found non-zero write ref count on a tree scheduled for merge (%d)\n",
                    atomic_read(&merge->in_trees[i]->write_ref_count)); 
            msleep(10);
        }
    }

    /* Alloc space for iterators. */
    ret = -ENOMEM;
    merge->iters = castle_zalloc(sizeof(void *) * merge->nr_trees, GFP_KERNEL);
    if (!merge->iters)
        goto err_out;

    /* Create apprapriate iterators for all of the trees. */
    ret = -EINVAL;
    FOR_EACH_MERGE_TREE(i, merge) 
    {
        castle_da_iterator_create(merge, merge->in_trees[i], &merge->iters[i]);
        
        /* Check if the iterators got created properly. */
        if (!merge->iters[i])
            goto err_out;
    }
    debug("Tree iterators created.\n");
    
    /* Init the merged iterator */
    ret = -ENOMEM;
    merge->merged_iter = castle_malloc(sizeof(c_merged_iter_t), GFP_KERNEL);
    if(!merge->merged_iter)
        goto err_out;
    debug("Merged iterator allocated.\n");

    merge->merged_iter->nr_iters = merge->nr_trees;
    merge->merged_iter->btree    = btree;
    FOR_EACH_MERGE_TREE(i, merge)
        iter_types[i] = castle_da_iter_type_get(merge->in_trees[i]);
    castle_ct_merged_iter_init(merge->merged_iter,
                               merge->iters,
                               iter_types,
                               castle_da_each_skip);
    ret = merge->merged_iter->err;
    debug("Merged iterator inited with ret=%d.\n", ret);
    if(ret)
        goto err_out;
    
    /* Success */
    return 0;

err_out:
    debug("Failed to create iterators. Ret=%d\n", ret);

    BUG_ON(!ret);
    return ret;
}

/**
 * Allocates extents for the output tree, medium objects and Bloom filetrs. Tree may be split
 * between two extents (internal nodes in an SSD-backed extent, leaf nodes on HDDs).
 *
 * @param merge     Merge state structure.
 */
static int castle_da_merge_extents_alloc(struct castle_da_merge *merge)
{
    c_byte_off_t internal_tree_size, tree_size, data_size, bloom_size;
    int i, ret;

    /* Allocate an extent for merged tree for the size equal to sum of all the
     * trees being merged (could be a total merge).
     */
    internal_tree_size = tree_size = data_size = bloom_size = 0;
    FOR_EACH_MERGE_TREE(i, merge)
    {
        BUG_ON(!castle_ext_freespace_consistent(&merge->in_trees[i]->tree_ext_free));
        tree_size += atomic64_read(&merge->in_trees[i]->tree_ext_free.used);

        BUG_ON(!castle_ext_freespace_consistent(&merge->in_trees[i]->data_ext_free));
        data_size += atomic64_read(&merge->in_trees[i]->data_ext_free.used);

        bloom_size += atomic64_read(&merge->in_trees[i]->item_count);
    }
    /* In case of multiple version test-case, in worst case tree could grow upto
     * double the size. Ex: For every alternative k_n in o/p stream of merged
     * iterator, k_n has only one version and k_(n+1) has (p-1) versions, where p
     * is maximum number of versions that can fit in a node. */
    tree_size = 2 * (MASK_CHK_OFFSET(tree_size) + C_CHK_SIZE);
    /* Calculate total size of internal nodes, assuming that leafs are stored on HDDs ... */
    internal_tree_size = tree_size;
    /* ... number of leaf nodes ... */
    internal_tree_size /= (VLBA_HDD_RO_TREE_NODE_SIZE * C_BLK_SIZE);
    /* ... number of level 1 nodes ... */
    internal_tree_size /= castle_btree_vlba_max_nr_entries_get(VLBA_SSD_RO_TREE_NODE_SIZE);
    internal_tree_size ++;
    /* ... size of level 1 ... */
    internal_tree_size *= (VLBA_SSD_RO_TREE_NODE_SIZE * C_BLK_SIZE);
    /* ... chunk rounding ... */
    internal_tree_size  = MASK_CHK_OFFSET(internal_tree_size + C_CHK_SIZE);
    /* ... factor of 2 explosion, just as before ... */
    internal_tree_size *= 2;

    /* TODO: change the alignment back to the actual node size, once we worked
             out which levels we'll be storing in this extent. */
    BUG_ON(!EXT_ID_INVAL(merge->internal_ext_free.ext_id) || 
           !EXT_ID_INVAL(merge->tree_ext_free.ext_id));
    /* Assume that SSDs will be used first. */
    merge->ssds_used = 1;
    /* First, attempt to allocate an SSD extent for the entire tree. */
    if (castle_use_ssd_leaf_nodes)
    {
        merge->tree_ext_free.ext_id = castle_extent_alloc(SSD_RDA,
                                                          merge->da->id,
                                                          CHUNK(tree_size));
    }
    /* If failed or disabled, try to allocate SSD extent for the internal nodes. */
    if(EXT_ID_INVAL(merge->tree_ext_free.ext_id))
    {
        merge->internal_ext_free.ext_id = castle_extent_alloc(SSD_RDA,
                                                              merge->da->id,
                                                              CHUNK(internal_tree_size));
        /* If the internal nodes extent is still invalid, we failed to
           allocate from SSDs. */
        if(EXT_ID_INVAL(merge->internal_ext_free.ext_id))
            merge->ssds_used = 0;
        /* HDD extent has to be allocated. */
        merge->tree_ext_free.ext_id = castle_extent_alloc(DEFAULT_RDA,
                                                          merge->da->id,
                                                          CHUNK(tree_size));
    }
    /* If the tree extent is still invalid, there is no space even on HDDs, go out. */
    if(EXT_ID_INVAL(merge->tree_ext_free.ext_id))
    {
        printk("Merge failed due to space constraint for tree\n");
        goto no_space;
    }

    /* Now, initialise freespace structure for the extents allocated. */
    if(!EXT_ID_INVAL(merge->tree_ext_free.ext_id))
        castle_ext_freespace_init(&merge->tree_ext_free,
                                   merge->tree_ext_free.ext_id,
                                   C_BLK_SIZE);
    if(!EXT_ID_INVAL(merge->internal_ext_free.ext_id))
        castle_ext_freespace_init(&merge->internal_ext_free,
                                   merge->internal_ext_free.ext_id,
                                   C_BLK_SIZE);

    /* Allocate an extent for medium objects of merged tree for the size equal to
     * sum of both the trees. */
    data_size = MASK_CHK_OFFSET(data_size + C_CHK_SIZE);
    if ((ret = castle_new_ext_freespace_init(&merge->data_ext_free,
                                              merge->da->id,
                                              data_size,
                                              C_BLK_SIZE)))
    {
        printk("Merge failed due to space constraint for data\n");
        goto no_space;
    }

    /* Allocate Bloom filters. */
    if ((ret = castle_bloom_create(&merge->bloom, merge->da->id, bloom_size)))
        merge->bloom_exists = 0;
    else
        merge->bloom_exists = 1;

    return 0;

no_space:
    castle_da_frozen_set(merge->da);

    return -ENOSPC;
}


static c_val_tup_t castle_da_medium_obj_copy(struct castle_da_merge *merge,
                                             c_val_tup_t old_cvt)
{
    c_ext_pos_t old_cep, new_cep;
    c_val_tup_t new_cvt;
    int total_blocks, blocks, i;
    c2_block_t *s_c2b, *c_c2b;
#ifdef CASTLE_PERF_DEBUG
    struct castle_component_tree *tree = NULL;
    struct timespec ts_start, ts_end;
#endif

    old_cep = old_cvt.cep;
    /* Old cvt needs to be a medium object. */
    BUG_ON(!CVT_MEDIUM_OBJECT(old_cvt));
    /* It needs to be of the right size. */
    BUG_ON((old_cvt.length <= MAX_INLINE_VAL_SIZE) || (old_cvt.length > MEDIUM_OBJECT_LIMIT));
    /* It must belong to one of the in_trees data extent. */
    FOR_EACH_MERGE_TREE(i, merge)
        if (old_cvt.cep.ext_id == merge->in_trees[i]->data_ext_free.ext_id)
            break;
    BUG_ON(i == merge->nr_trees);
    /* We assume objects are page aligned. */
    BUG_ON(BLOCK_OFFSET(old_cep.offset) != 0);

    /* Allocate space for the new copy. */
    total_blocks = (old_cvt.length - 1) / C_BLK_SIZE + 1;
    BUG_ON(castle_ext_freespace_get(&merge->data_ext_free,
                                     total_blocks * C_BLK_SIZE,
                                     0,
                                    &new_cep) < 0);
    BUG_ON(BLOCK_OFFSET(new_cep.offset) != 0);
    /* Save the cep to return later. */
    new_cvt = old_cvt;
    new_cvt.cep = new_cep;

    /* Do the actual copy. */
    debug("Copying "cep_fmt_str" to "cep_fmt_str_nl,
            cep2str(old_cep), cep2str(new_cep));
#ifdef CASTLE_PERF_DEBUG
    /* Figure out which tree to update stats for. */
    FOR_EACH_MERGE_TREE(i, merge)
        if (old_cep.ext_id == merge->in_trees[i]->data_ext_free.ext_id)
            tree = merge->in_trees[i];
#endif

    while (total_blocks > 0)
    {
        int chk_off, pgs_to_end;

        /* Chunk-align blocks if total_blocks is large enough to make it worthwhile. */
        chk_off = CHUNK_OFFSET(old_cep.offset);
        if (chk_off)
            pgs_to_end = (C_CHK_SIZE - chk_off) >> PAGE_SHIFT;

        /* Be careful about subtraction, if it goes negative, and is compared to 
           BLKS_PER_CHK the test is likely not to work correctly. */
        if (chk_off && (total_blocks >= 2*BLKS_PER_CHK + pgs_to_end))
            /* Align for a minimum of 2 full blocks (1 can be inefficient) */
            blocks = pgs_to_end;
        else if (total_blocks > BLKS_PER_CHK)
            blocks = BLKS_PER_CHK;
        else
            blocks = total_blocks;
        total_blocks -= blocks;

        castle_perf_debug_getnstimeofday(&ts_start);
        s_c2b = castle_cache_block_get(old_cep, blocks);
        c_c2b = castle_cache_block_get(new_cep, blocks);
        castle_perf_debug_getnstimeofday(&ts_end);
        castle_perf_debug_bump_ctr(tree->get_c2b_ns, ts_end, ts_start);
        if (merge->level > 1)
            castle_cache_advise(s_c2b->cep, C2_ADV_PREFETCH|C2_ADV_SOFTPIN|C2_ADV_FRWD, -1, -1, 0);
        else
            castle_cache_advise(s_c2b->cep, C2_ADV_PREFETCH|C2_ADV_FRWD, -1, -1, 0);
        /* Make sure that we lock _after_ prefetch call. */
        write_lock_c2b(s_c2b);
        write_lock_c2b(c_c2b);
        if(!c2b_uptodate(s_c2b))
        {
            /* c2b is not marked as up-to-date.  We hope this is because we are
             * at the start of the extent and have just issued a prefetch call.
             * If this is true, the underlying c2p is up-to-date so a quick call
             * into submit_c2b_sync() should detect this and update the c2b to
             * reflect this change.
             *
             * Alternatively it could mean that some of our prefetched c2bs have
             * been evicted.
             *
             * By analysing the time spent in submit_c2b_sync() it should be
             * possible to determine which of these scenarios are occurring. */
            castle_perf_debug_getnstimeofday(&ts_start);
            BUG_ON(submit_c2b_sync(READ, s_c2b));
            castle_perf_debug_getnstimeofday(&ts_end);
            castle_perf_debug_bump_ctr(tree->data_c2bsync_ns, ts_end, ts_start);
        }
        update_c2b(c_c2b);
        memcpy(c2b_buffer(c_c2b), c2b_buffer(s_c2b), blocks * PAGE_SIZE);
        dirty_c2b(c_c2b);
        write_unlock_c2b(c_c2b);
        write_unlock_c2b(s_c2b);
        put_c2b(c_c2b);
        put_c2b(s_c2b);
        old_cep.offset += blocks * PAGE_SIZE;
        new_cep.offset += blocks * PAGE_SIZE;
    }
    debug("Finished copy, i=%d\n", i);

    return new_cvt;
}

/**
 * Works out which extent, and what node size should be used for given level in the btree
 * in a given merge. 
 *
 * @param merge     Merge state structure.
 * @param level     Level counted from leaves. 
 * @param node_size Return argument: size of the node.
 * @param ext_free  Return argument: extent freespace structure.
 */
static inline void castle_da_merge_node_info_get(struct castle_da_merge *merge,
                                                 uint8_t level,
                                                 uint16_t *node_size,
                                                 c_ext_free_t **ext_free)
{
    /* Initialise the return variables, assuming that nodes will be stored on HDDs. */
    *node_size = VLBA_HDD_RO_TREE_NODE_SIZE;
    *ext_free = &merge->tree_ext_free;

    /* If SSDs are not used, the node must be on HDDs. */
    if(!merge->ssds_used)
    {
        /* There shouldn't be an extent for internal nodes if SSDs aren't used. */
        BUG_ON(!EXT_ID_INVAL(merge->internal_ext_free.ext_id));
        return;
    }
    
    /* SSDs are used, but the node may still live on HDDs, but only if there is a 
       separate extent for internal nodes, and level is 0 (leaf). */
    if(!EXT_ID_INVAL(merge->internal_ext_free.ext_id) && (level == 0))
    {
        /* There should be an extent for leaf nodes on HDDs. */
        BUG_ON(EXT_ID_INVAL(merge->tree_ext_free.ext_id));
        return;
    }

    /* Node must be stored on SSDs. Change the size appropriately. */
    *node_size = VLBA_SSD_RO_TREE_NODE_SIZE;

    /* Internal nodes extent should be used if it exists, and level>0. */
    if(!EXT_ID_INVAL(merge->internal_ext_free.ext_id) && (level > 0))
        *ext_free = &merge->internal_ext_free;
}

/**
 * Add an entry to the nodes that are being constructed in merge. 
 *
 * @param merge [in, out] Doubling array merge structure
 * @param depth [in] B-tree depth at which entry should be added. 0 being leaf nodes. 
 * @param key [in] key of the entry to be added
 * @param version [in]  version of the entry to be added
 * @param cvt [in] value tuple of the entry to be added
 * @param is_re_add [in] are we trying to re-add the entry to output tree?
 *                       (possible when we are trying to move entries from one node to 
 *                       another node while completing the former node.)
 * Note: if is_re_add flag is set, then the data wont be processed again, just
 * the key gets added.  Used when entry is being moved from one node to another
 * node.
 */
static inline void castle_da_entry_add(struct castle_da_merge *merge,
                                       int depth,
                                       void *key,
                                       version_t version,
                                       c_val_tup_t cvt,
                                       int is_re_add)
{
    struct castle_da_merge_level *level = merge->levels + depth;
    struct castle_btree_type *btree = merge->out_btree;
    struct castle_btree_node *node;
    int key_cmp;
#ifdef CASTLE_PERF_DEBUG
    struct timespec ts_start, ts_end;
#endif

    /* Deal with medium and large objects first. For medium objects, we need to copy them
       into our new medium object extent. For large objects, we need to save the aggregate
       size. plus take refs to extents? */
    /* It is possible to do castle_da_entry_add() on the same entry multiple
     * times. Don't process data again. */
    if (!is_re_add)
    {
        if(CVT_MEDIUM_OBJECT(cvt))
        {
            castle_perf_debug_getnstimeofday(&ts_start);
            cvt = castle_da_medium_obj_copy(merge, cvt);
            castle_perf_debug_getnstimeofday(&ts_end);
            castle_perf_debug_bump_ctr(merge->da_medium_obj_copy_ns, ts_end, ts_start);
        }
        if(CVT_LARGE_OBJECT(cvt))
        {
            merge->large_chunks += castle_extent_size_get(cvt.cep.ext_id);
            /* No need to add Large Objects under lock as merge is done in
             * sequence. No concurrency issues on the tree. */
            castle_ct_large_obj_add(cvt.cep.ext_id, cvt.length, &merge->large_objs, NULL);
            castle_extent_get(cvt.cep.ext_id);
        }
    }
    
    BUG_ON(is_re_add && 
           CVT_MEDIUM_OBJECT(cvt) && 
           (cvt.cep.ext_id != merge->data_ext_free.ext_id));

    debug("Adding an entry at depth: %d\n", depth);
    BUG_ON(depth >= MAX_BTREE_DEPTH);
    /* Alloc a new block if we need one */
    if(!level->node_c2b)
    {
        c_ext_free_t *ext_free;
        uint16_t node_size;
        c_ext_pos_t cep;

        castle_da_merge_node_info_get(merge, depth, &node_size, &ext_free);
        if(merge->root_depth < depth)
        {
            debug("Creating a new root level: %d\n", depth);
            BUG_ON(merge->root_depth != depth - 1);
            merge->root_depth = depth;
            merge->out_tree->node_sizes[depth] = node_size;
        }
        BUG_ON(level->next_idx      != 0);
        BUG_ON(level->valid_end_idx >= 0);

        debug("Allocating a new node at depth: %d\n", depth);
        BUG_ON(node_size != btree->node_size(merge->out_tree, depth));
        BUG_ON(castle_ext_freespace_get(ext_free,
                                        node_size * C_BLK_SIZE,
                                        0,
                                        &cep) < 0);
        debug("Got "cep_fmt_str_nl, cep2str(cep));

        castle_perf_debug_getnstimeofday(&ts_start);
        level->node_c2b = castle_cache_block_get(cep, node_size);
        castle_perf_debug_getnstimeofday(&ts_end);
        castle_perf_debug_bump_ctr(merge->get_c2b_ns, ts_end, ts_start);
        debug("Locking the c2b, and setting it up to date.\n");
        write_lock_c2b(level->node_c2b);
        update_c2b(level->node_c2b);
        /* Init the node properly */
        node = c2b_bnode(level->node_c2b);
        castle_da_node_buffer_init(btree, node, node_size);
    }

    node = c2b_bnode(level->node_c2b);
    debug("Adding an idx=%d, key=%p, *key=%d, version=%d\n", 
            level->next_idx, key, *((uint32_t *)key), version);
    /* Add the entry to the node (this may get dropped later, but leave it here for now */
    BUG_ON(CVT_LEAF_PTR(cvt));
    btree->entry_add(node, level->next_idx, key, version, cvt);
    /* Compare the current key to the last key. Should never be smaller */
    /* key_compare() is a costly function. Trying to avoid duplicates. We already
     * did comparision between last key added to the out_tree and current key in
     * snapshot_delete algorithm (in castle_da_entry_skip()). Reuse the result
     * of it here again. */
    /* Note: In case of re-adds is_new_key doesnt represent comparision between key being 
     * added and last key added to the node. But, it repesents the comparision between last 
     * 2 keys added to the tree. Still, it is okay as in case of re-adds both the comparisions
     * yield same value. */
    key_cmp = (level->next_idx != 0) ? 
               ((depth == 0)? merge->is_new_key: btree->key_compare(key, level->last_key)) :
               0;
    debug("Key cmp=%d\n", key_cmp);
    BUG_ON(key_cmp < 0);

    /* Work out if the current/previous entry could be a valid node end.
       Case 1: We've just started a new node (node_idx == 0) => current must be a valid node entry */
    if(level->next_idx == 0)
    {
        debug("Node valid_end_idx=%d, Case1.\n", level->next_idx);
        BUG_ON(level->valid_end_idx >= 0);
        /* Save last_key, version as a valid_version, and init valid_end_idx.
           Note: last_key has to be taken from the node, bacuse current key pointer
                 may get invalidated on the iterator next() call. 
         */
        level->valid_end_idx = 0;
        btree->entry_get(node, level->next_idx, &level->last_key, NULL, NULL);
        level->valid_version = version;
    } else
    /* Case 2: We've moved on to a new key. Previous entry is a valid node end. */
    if(key_cmp > 0)
    {
        debug("Node valid_end_idx=%d, Case2.\n", level->next_idx);
        btree->entry_get(node, level->next_idx, &level->last_key, NULL, NULL);
        BUG_ON(level->next_idx <= 0);
        level->valid_end_idx = level->next_idx - 1;
        level->valid_version = 0;
    } else
    /* Case 3: Version is STRONGLY ancestoral to valid_version. */
    if(castle_version_is_ancestor(version, level->valid_version))
    {
        debug("Node valid_end_idx=%d, Case3.\n", level->next_idx);
        BUG_ON(version == level->valid_version);
        level->valid_end_idx = level->next_idx;
        level->valid_version = version;
    }

    /* Node may be (over-)complete now, if it is full. Set next_idx to -1 (invalid) */
    if(btree->need_split(node, 0))
    {
        debug("Node now complete.\n");
        level->next_idx = -1;
    }
    else
        /* Go to the next node_idx */
        level->next_idx++;

    /* Get the last_key stored in leaf nodes. */
    if (depth == 0)
    {
        merge->last_key = level->last_key;
        BUG_ON(merge->last_key == NULL);
    }
}

static void castle_da_node_complete(struct castle_da_merge *merge, int depth)
{
    struct castle_da_merge_level *level = merge->levels + depth;
    struct castle_btree_type *btree = merge->out_btree;
    struct castle_btree_node *node, *prev_node;
    int node_idx;
    void *key;
    version_t version;
    c_val_tup_t cvt, node_cvt;
    c2_block_t *node_c2b;
    int valid_end_idx;

    /* Make sure we are not in recursion. */
#ifdef CASTLE_DEBUG
    BUG_ON(merge->is_recursion);
    merge->is_recursion = 1;
#endif

    debug("Completing node at depth=%d\n", depth);
    BUG_ON(depth >= MAX_BTREE_DEPTH);
    node      = c2b_bnode(level->node_c2b);
    BUG_ON(!node);
    /* Version of the node should be the last valid_version */
    debug("Node version=%d\n", level->valid_version);
    node->version = level->valid_version;
    if(depth > 0)
        node->is_leaf = 0;

    /* Note: This code calls castle_da_entry_add(), which would change all
     * parameters in level. Taking a copy of required members. */
    node_c2b        = level->node_c2b;
    valid_end_idx   = level->valid_end_idx;

    /* Reset the variables to the correct state for castle_da_entry_add(). */
    level->node_c2b      = NULL;
    level->last_key      = NULL;
    level->next_idx      = 0;
    level->valid_end_idx = -1;
    level->valid_version = INVAL_VERSION;  

    /* When a node is complete, we need to copy the entries after valid_end_idx to 
       the corresponding buffer */
    node_idx = valid_end_idx + 1;
    BUG_ON(node_idx <= 0 || node_idx > node->used);
    debug("Entries to be copied to the buffer are in range [%d, %d)\n",
            node_idx, node->used);
    while(node_idx < node->used) 
    {
        /* If merge is completing, there shouldnt be any splits any more. */
        BUG_ON(merge->completing);
        btree->entry_get(node, node_idx,  &key, &version, &cvt);
        BUG_ON(CVT_LEAF_PTR(cvt));
        castle_da_entry_add(merge, depth, key, version, cvt, 1); 
        node_idx++;
        BUG_ON(level->node_c2b == NULL);
        /* Check if the node completed, it should never do */
        BUG_ON(level->next_idx < 0);
    }
    debug("Dropping entries [%d, %d] from the original node\n",
            valid_end_idx + 1, node->used - 1);
    /* Now that entries are safely in the new node, drop them from the node */ 
    if((valid_end_idx + 1) <= (node->used - 1))
        btree->entries_drop(node, valid_end_idx + 1, node->used - 1);

    BUG_ON(node->used != valid_end_idx + 1);
    btree->entry_get(node, valid_end_idx, &key, &version, &cvt);
    debug("Inserting into parent key=%p, *key=%d, version=%d\n",
            key, *((uint32_t*)key), node->version);
    BUG_ON(CVT_LEAF_PTR(cvt));
 
    /* Insert correct pointer in the parent, unless we've just completed the
       root node at the end of the merge. */ 
    if(merge->completing && (merge->root_depth == depth) && (level->node_c2b == NULL)) 
    {
        debug("Just completed the root node (depth=%d), at the end of the merge.\n",
                depth);
        goto release_node;
    }
    CVT_NODE_SET(node_cvt, (node_c2b->nr_pages * C_BLK_SIZE), node_c2b->cep);
    castle_da_entry_add(merge, depth+1, key, node->version, node_cvt, 0);
release_node:
    debug("Releasing c2b for cep=" cep_fmt_str_nl, cep2str(node_c2b->cep));
    debug("Completing a node with %d entries at depth %d\n", node->used, depth);
    /* Hold on to last leaf node for the sake of last_key. No need of lock, this
     * is a immutable node. */
    if (depth == 0)
    {
        if (merge->last_leaf_node_c2b)
            put_c2b(merge->last_leaf_node_c2b);

        merge->last_leaf_node_c2b = node_c2b;
        get_c2b(node_c2b);
    }
    /* Write the list pointer into the previous node we've completed (if one exists).
       Then release it. */
    prev_node = merge->last_node_c2b ? c2b_bnode(merge->last_node_c2b) : NULL; 
    if(prev_node)
    {
        prev_node->next_node = node_c2b->cep;
        prev_node->next_node_size = node_c2b->nr_pages;
        dirty_c2b(merge->last_node_c2b);
        write_unlock_c2b(merge->last_node_c2b);
        put_c2b(merge->last_node_c2b);
    } else
    {
        /* We've just created the first node, save it */
        merge->first_node = node_c2b->cep;
        merge->first_node_size = node_c2b->nr_pages;
    }
    /* Save this node as the last node now */
    merge->last_node_c2b = node_c2b;

    /* Increment node count */
    merge->nr_nodes++;

#ifdef CASTLE_DEBUG
    merge->is_recursion = 0;
#endif
}
       
static inline int castle_da_nodes_complete(struct castle_da_merge *merge, int depth)
{
    struct castle_da_merge_level *level;
    int i;
    
    debug("Checking if we need to complete nodes starting at level: %d\n", depth);
    /* Check if the level 'depth' node has been completed, which may trigger a cascade of
       completes up the tree. */ 
    for(i=depth; i<MAX_BTREE_DEPTH-1; i++)
    {
        level = merge->levels + i;
        /* Complete if next_idx < 0 */
        if(level->next_idx < 0)
            castle_da_node_complete(merge, i);
        else
            /* As soon as we see an incomplete node, we need to break out: */
            goto out;
    }
    /* If we reached the top of the tree, we must fail the merge */
    if(i == MAX_BTREE_DEPTH - 1)
        return -EINVAL;

out:
    debug("We got as far as depth=%d\n", i);

    return 0;
}
   
static struct castle_component_tree* castle_da_merge_package(struct castle_da_merge *merge)
{
    struct castle_component_tree *out_tree;
    int i;

    out_tree = merge->out_tree; 
    debug("Using component tree id=%d to package the merge.\n", out_tree->seq);
    /* Root node is the last node that gets completed, and therefore will be saved in last_node */
    out_tree->tree_depth = merge->root_depth+1;
    printk("Depth of ct=%d (%p) is: %d\n", out_tree->seq, out_tree, out_tree->tree_depth);
    out_tree->root_node = merge->last_node_c2b->cep;
    out_tree->first_node = merge->first_node;
    out_tree->first_node_size = merge->first_node_size;
    out_tree->last_node = INVAL_EXT_POS;
    out_tree->last_node_size = -1;
    out_tree->bloom_exists = merge->bloom_exists;
    out_tree->bloom = merge->bloom;

    /* Release the last node c2b */
    if(merge->last_node_c2b)
    {
        dirty_c2b(merge->last_node_c2b);
        write_unlock_c2b(merge->last_node_c2b);
        put_c2b(merge->last_node_c2b);
        merge->last_node_c2b = NULL;
    }
    
    debug("Root for that tree is: " cep_fmt_str_nl, cep2str(out_tree->root_node));
    /* Write counts out */
    atomic_set(&out_tree->ref_count, 1);
    atomic_set(&out_tree->write_ref_count, 0);
    atomic64_set(&out_tree->item_count, merge->nr_entries);
    atomic64_set(&out_tree->node_count, merge->nr_nodes);
    atomic64_set(&out_tree->large_ext_chk_cnt, merge->large_chunks);
    out_tree->internal_ext_free = merge->internal_ext_free;
    out_tree->tree_ext_free = merge->tree_ext_free;
    out_tree->data_ext_free = merge->data_ext_free;
    atomic64_set(&out_tree->tree_ext_free.used, 
                 atomic64_read(&merge->tree_ext_free.used));
    atomic64_set(&out_tree->data_ext_free.used, 
                 atomic64_read(&merge->data_ext_free.used));
    atomic64_set(&out_tree->tree_ext_free.blocked, 
                 atomic64_read(&merge->tree_ext_free.blocked));
    atomic64_set(&out_tree->data_ext_free.blocked, 
                 atomic64_read(&merge->data_ext_free.blocked));

    /* Calculate latest key in both trees. */
    if (castle_latest_key)
    {
        FOR_EACH_MERGE_TREE(i, merge)
        {
            //BUG_ON(merge->in_tree1->seq <=  merge->in_tree2->seq);
            if (merge->in_trees[i]->last_key)
            {
                out_tree->last_key = merge->in_trees[i]->last_key;
                merge->in_trees[i]->last_key = NULL;
                break;
            }
        }
    }

    /* Add list of large objects to CT. */
    list_replace(&merge->large_objs, &out_tree->large_objs);
    merge->large_objs.prev = merge->large_objs.next = NULL;

    debug("Number of entries=%ld, number of nodes=%ld\n",
            atomic64_read(&out_tree->item_count),
            atomic64_read(&out_tree->node_count));

    /* Add the new tree to the doubling array */
    BUG_ON(merge->da->id != out_tree->da); 
    printk("Finishing merge of ");
    FOR_EACH_MERGE_TREE(i, merge)
        printk("ct%d=%d, ", i, merge->in_trees[i]->seq);
    printk("new_tree=%d\n", out_tree->seq);
    debug("Adding to doubling array, level: %d\n", out_tree->level);

    FAULT(MERGE_FAULT);

    return out_tree;
}

static void castle_da_max_path_complete(struct castle_da_merge *merge)
{
    struct castle_btree_type *btree = merge->out_btree;
    struct castle_btree_node *node;
    c2_block_t *root_c2b, *node_c2b, *next_node_c2b;
    struct castle_component_tree *ct = merge->out_tree;
    uint8_t level;

    BUG_ON(!merge->completing);
    /* Root stored in last_node_c2b at the end of the merge */
    root_c2b = merge->last_node_c2b;
    printk("Maxifying the right most path, starting with root_cep="cep_fmt_str_nl,
            cep2str(root_c2b->cep));
    /* Start of with root node */
    node_c2b = root_c2b;
    node = c2b_bnode(node_c2b);
    level = 0;
    while(!node->is_leaf)
    {
        void *k;
        version_t v;
        c_val_tup_t cvt;

        /* Replace right-most entry with (k=max_key, v=0) */
        btree->entry_get(node, node->used-1, &k, &v, &cvt);
        BUG_ON(!CVT_NODE(cvt) || CVT_LEAF_PTR(cvt));
        debug("The node is non-leaf, replacing the right most entry with (max_key, 0).\n");
        btree->entry_replace(node, node->used-1, btree->max_key, 0, cvt);
        /* Change the version of the node to 0 */
        node->version = 0;
        /* Dirty the c2b */
        dirty_c2b(node_c2b);
        /* Go to the next btree node */
        debug("Locking next node cep=" cep_fmt_str_nl,
              cep2str(cvt.cep));
        next_node_c2b = castle_cache_block_get(cvt.cep, 
                                               btree->node_size(ct, merge->root_depth - level));
        write_lock_c2b(next_node_c2b);
        /* We unlikely to need a blocking read, because we've just had these
           nodes in the cache. */
        if(!c2b_uptodate(next_node_c2b))
            BUG_ON(submit_c2b_sync(READ, next_node_c2b));
        /* Release the old node, if it's not the same as the root node */
        if(node_c2b != root_c2b) 
        {
            debug("Unlocking prev node cep=" cep_fmt_str_nl, 
                   cep2str(node_c2b->cep));
            write_unlock_c2b(node_c2b);
            put_c2b(node_c2b);
        }
        node_c2b = next_node_c2b;
        node = c2b_bnode(node_c2b);
        level++;
    }
    /* Release the leaf node, if it's not the same as the root node */
    if(node_c2b != root_c2b) 
    {
        debug("Unlocking prev node cep="cep_fmt_str_nl, 
               cep2str(node_c2b->cep));
        write_unlock_c2b(node_c2b);
        put_c2b(node_c2b);
    }
}

/**
 * Complete merge process. 
 *
 * Each level can have atmost one uncompleted node. Complete each node with the 
 * entries we got now, and link the node to its parent. During this process, each 
 * non-leaf node can get one extra entry in worst case. Mark valid_end_idx in each 
 * level to used-1. And call castle_da_node_complete on every level, which would
 * complete the node and might add one entry in next higher level.
 *
 * @param merge [in, out] merge strucutre to be completed.
 *
 * @return ct Complete out tree
 *
 * @see castle_da_node_complete
 */
static struct castle_component_tree* castle_da_merge_complete(struct castle_da_merge *merge)
{
    struct castle_da_merge_level *level;
    struct castle_btree_node *node;
    int next_idx, i;

    merge->completing = 1;
    debug("Complete merge at level: %d|%d\n", merge->level, merge->root_depth);
    /* Force the nodes to complete by setting next_idx negative. Valid node idx
       can be set to the last entry in the node safely, because it happens in
       conjunction with setting the version to 0. This guarantees that all
       versions in the node are decendant of the node version. */
    for(i=0; i<MAX_BTREE_DEPTH; i++)
    {
        debug("Flushing at depth: %d\n", i);
        level = merge->levels + i;
        /* Node index == 0 indicates that there is no node at this level,
           therefore we don't have to complete anything. */
        next_idx = level->next_idx;
        if(next_idx != 0)
        {
            debug("Artificially completing the node at depth: %d\n", i);

            /* Complete the node by marking last entry as valid end. Also, mark
             * the version of this node to 0, as the node might contain multiple
             * entries. */
            node = c2b_bnode(level->node_c2b);
            /* Point the valid_end_idx past the last entry ... */
            level->valid_end_idx = next_idx < 0 ? node->used : level->next_idx;
            /* ... and now point it at the last entry. */ 
            level->valid_end_idx--;
            level->valid_version = 0;
            level->next_idx = -1;
            castle_da_node_complete(merge, i);
        } 
    }
    /* Write out the max keys along the max path. */
    castle_da_max_path_complete(merge);

    /* Complete Bloom filters. */
    if (merge->bloom_exists)
        castle_bloom_complete(&merge->bloom);

    /* Package the merge result. */ 
    return castle_da_merge_package(merge);
}

static void castle_da_merge_dealloc(struct castle_da_merge *merge, int err)
{
    int i;

    if(!merge)
        return;

    /* Release the last leaf node c2b. */
    if (merge->last_leaf_node_c2b)
        put_c2b(merge->last_leaf_node_c2b);

    /* Release the last node c2b */
    if(merge->last_node_c2b)
    {
        dirty_c2b(merge->last_node_c2b);
        write_unlock_c2b(merge->last_node_c2b);
        put_c2b(merge->last_node_c2b);
    }
    
    /* Free all the buffers */
    if (merge->snapshot_delete.occupied)
        castle_free(merge->snapshot_delete.occupied);
    if (merge->snapshot_delete.need_parent)
        castle_free(merge->snapshot_delete.need_parent);

    for(i=0; i<MAX_BTREE_DEPTH; i++)
    {
        c2_block_t *c2b = merge->levels[i].node_c2b;
        if(c2b)
        {
            write_unlock_c2b(c2b);
            put_c2b(c2b);
        }
    }
    FOR_EACH_MERGE_TREE(i, merge) 
        castle_da_iterator_destroy(merge->in_trees[i], merge->iters[i]);
    castle_free(merge->iters);
    castle_ct_merged_iter_cancel(merge->merged_iter);
    /* If succeeded at merging, old trees need to be destroyed (they've already been removed
       from the DA by castle_da_merge_package(). */
    if(!err)
    {
        debug("Destroying old CTs.\n");
        FOR_EACH_MERGE_TREE(i, merge) 
            castle_ct_put(merge->in_trees[i], 0);
    }
    else
    {
        struct castle_component_tree *out_tree;

        castle_ext_freespace_fini(&merge->internal_ext_free);
        castle_ext_freespace_fini(&merge->tree_ext_free);
        castle_ext_freespace_fini(&merge->data_ext_free);

        if (merge->bloom_exists)
            castle_bloom_destroy(&merge->bloom);

        out_tree = merge->out_tree;
        /* Free the component tree, if one was allocated. */
        if(out_tree)
        {
            BUG_ON(atomic_read(&out_tree->write_ref_count) != 0);
            BUG_ON(atomic_read(&out_tree->ref_count) != 1);
            castle_ct_put(merge->out_tree, 0);
        }
    }
    /* Free the merged iterator, if one was allocated. */
    if(merge->merged_iter)
        castle_free(merge->merged_iter);
    castle_free(merge);
}

static int castle_da_merge_progress_update(struct castle_da_merge *merge, uint32_t unit_nr)
{
    uint64_t items_completed, total_items, unit_items;
    uint32_t total_units;
    int i;

    /* If the merge is not marked for deamortization complete the merge. */
    if (!merge->da->levels[merge->level].merge.deamortize)
        return 0;

    total_units = 1 << merge->level;
    /* Don't stop the last merge unit, let it run out of iterator. */
    if(unit_nr >= total_units)
        return 0;
    /* Otherwise, check whether we've got far enough. */
    total_items = 0;
    FOR_EACH_MERGE_TREE(i, merge)
        total_items  += atomic64_read(&merge->in_trees[i]->item_count);
    unit_items   = total_items * (uint64_t)unit_nr / (uint64_t)total_units;
    items_completed = merge->merged_iter->src_items_completed;
    if(items_completed >= unit_items)
        return 1;
    return 0;
}

/**
 * Determines whether the entry can be deleted, if the version is marked for
 * deletion. 
 *
 * @param merge [in] merge stream that entry comes from
 * @param key [in] key of the entry
 * @param version [in] version of the entry
 *
 * @return return 1, if the entry needs to be skipped
 *
 * @see castle_version_is_deletable
 */
static int castle_da_entry_skip(struct castle_da_merge *merge, 
                                void *key,
                                version_t version)
{
    struct castle_btree_type *btree = merge->out_btree;
    struct castle_version_delete_state *state = &merge->snapshot_delete;
    void *last_key = merge->last_key;

    merge->is_new_key = (last_key)? btree->key_compare(key, last_key): 1;
    /* Compare the keys. If looking at new key then reset data
     * structures. */
    if (merge->is_new_key)
    {
        int nr_bytes = state->last_version/8 + 1;

        memset(state->occupied, 0, nr_bytes);
        memset(state->need_parent, 0, nr_bytes);
        state->next_deleted = NULL;
    }

    return castle_version_is_deletable(state, version);
}

static int castle_da_merge_unit_do(struct castle_da_merge *merge, uint32_t unit_nr)
{
    void *key;
    version_t version;
    c_val_tup_t cvt;
    int ret;
#ifdef CASTLE_PERF_DEBUG
    struct timespec ts_start, ts_end;
#endif

    while(castle_ct_merged_iter_has_next(merge->merged_iter))
    {
        might_resched();
        /* TODO: we never check iterator errors. We should! */
        castle_perf_debug_getnstimeofday(&ts_start);
        castle_ct_merged_iter_next(merge->merged_iter, &key, &version, &cvt);
        castle_perf_debug_getnstimeofday(&ts_end);
        castle_perf_debug_bump_ctr(merge->merged_iter_next_ns, ts_end, ts_start);
        debug("Merging entry id=%lld: k=%p, *k=%d, version=%d, cep="cep_fmt_str_nl,
                i, key, *((uint32_t *)key), version, cep2str(cvt.cep));
        BUG_ON(CVT_INVALID(cvt));
        /* Check whether we need to skip the entry.
         * Note: Nothing to be done to delete the skipped keys. They would get
         * deleted while dropping the component tree. */
        if (castle_da_entry_skip(merge, key, version))
        {
            merge->skipped_count++;
            goto entry_done;
        }
        /* Add entry to level 0 node (and recursively up the tree). */
        castle_da_entry_add(merge, 0, key, version, cvt, 0);
        /* Add entry to bloom filter */
        if (merge->bloom_exists)
            castle_bloom_add(&merge->bloom, merge->out_btree, key);
        /* Increment the number of entries stored in the output tree. */
        merge->nr_entries++;
        /* Try to complete node. */
        castle_perf_debug_getnstimeofday(&ts_start);
        ret = castle_da_nodes_complete(merge, 0);
        castle_perf_debug_getnstimeofday(&ts_end);
        castle_perf_debug_bump_ctr(merge->nodes_complete_ns, ts_end, ts_start);
        if (ret != EXIT_SUCCESS)
            goto err_out;
entry_done:            
        castle_perf_debug_getnstimeofday(&ts_start);
        castle_da_merge_budget_consume(merge);
        castle_perf_debug_getnstimeofday(&ts_end);
        castle_perf_debug_bump_ctr(merge->budget_consume_ns, ts_end, ts_start);
        /* Update the progress, returns non-zero if we've completed the current unit. */
        castle_perf_debug_getnstimeofday(&ts_start);
        if(castle_da_merge_progress_update(merge, unit_nr))
        {
            castle_perf_debug_getnstimeofday(&ts_end);
            castle_perf_debug_bump_ctr(merge->progress_update_ns, ts_end, ts_start);
            return EAGAIN;
        }

        FAULT(MERGE_FAULT);
    }

    /* Return success, if we are finished with the merge. */
    return EXIT_SUCCESS;

err_out:
    if(ret)
        printk("Merge failed with %d\n", ret);
    castle_da_merge_dealloc(merge, ret);

    return ret; 
}

static inline void castle_da_merge_token_return(struct castle_double_array *da, 
                                                int level,
                                                struct castle_merge_token *token)
{
    int driver_level;

    BUG_ON(!castle_da_is_locked(da));
    BUG_ON(token->ref_cnt <= 0);
    driver_level = token->driver_level;
    token->ref_cnt--;
    if(token->ref_cnt == 0)
    {
        /* Return the token to the driver level => anihilate the token. */
        BUG_ON(da->levels[driver_level].merge.driver_token != token);
        da->levels[driver_level].merge.driver_token = NULL;
        token->driver_level = -1;
        token->ref_cnt      = 0;
        list_add(&token->list, &da->merge_tokens);
    }
}

static inline void castle_da_merge_token_push(struct castle_double_array *da, 
                                              int level,
                                              struct castle_merge_token *token)
{
    BUG_ON(!castle_da_is_locked(da));
    /* Token push moves the token to the next level, if that level is in a merge, 
       or returns it to the driver level if not. */
    BUG_ON(level+1 >= MAX_DA_LEVEL);
    token->ref_cnt++;
    if(da->levels[level+1].nr_trees >= 2)
        list_add(&token->list, &da->levels[level+1].merge.merge_tokens);
    else
        castle_da_merge_token_return(da, level, token); 
}

static inline void castle_da_merge_token_activate(struct castle_double_array *da, 
                                                  int level,
                                                  struct castle_merge_token *token)
{
    BUG_ON(!castle_da_is_locked(da));
    /* Token is activated by pushing it to the next level up, and saving it as the active
       token at this level. */
    BUG_ON(level+1 >= MAX_DA_LEVEL);
    /* Take a ref for this active token. */
    token->ref_cnt++;
    da->levels[level].merge.active_token = token; 
    /* Attempt to push it to the higher level. */
    castle_da_merge_token_push(da, level, token); 
}

static inline struct castle_merge_token* castle_da_merge_token_get(struct castle_double_array *da,
                                                                   int level)
{
    struct castle_merge_token *token;

    if(list_empty(&da->levels[level].merge.merge_tokens))
        return NULL;

    token = list_first_entry(&da->levels[level].merge.merge_tokens, 
                              struct castle_merge_token, 
                              list);
    /* Remove the token from list of inactive tokens. */
    list_del(&token->list);

    return token;
}

static inline struct castle_merge_token* castle_da_merge_token_generate(struct castle_double_array *da,
                                                                        int level)
{
    struct castle_merge_token *token;

    BUG_ON(list_empty(&da->merge_tokens));
    BUG_ON(da->levels[level].merge.driver_token); 
    /* Get a token out of the list. */
    token = list_first_entry(&da->merge_tokens, struct castle_merge_token, list);
    list_del(&token->list);
    /* Initialise the token. */
    token->driver_level = level;
    token->ref_cnt      = 0;
    /* Save the token as our driver token. */
    da->levels[level].merge.driver_token = token; 

    return token;
}

#define exit_cond (castle_da_exiting || castle_da_deleted(da))
static inline int castle_da_merge_wait_event(struct castle_double_array *da, int level)
{
    int32_t this_level_units, prev_level_units, nr_trees, backlog;
    struct castle_merge_token *token;
    int not_ready_wake;

    not_ready_wake = 0;
    /* Protect the reads/updates to merge variables with DA lock. */
    write_lock(&da->lock);
    /* If the merge isn't deamortised (total merges only), start immediately. */
    if (!da->levels[level].merge.deamortize)
    {
        BUG_ON(level != BIG_MERGE);
        da->levels[level].merge.units_commited++;
        write_unlock(&da->lock);
        return 1;
    }

    this_level_units = da->levels[level].merge.units_commited;
    /* Level 1 merges don't have any merges happening below. */
    prev_level_units = (level == 1) ? 0 : da->levels[level-1].merge.units_commited;
    nr_trees = da->levels[level].nr_trees;
    BUG_ON(nr_trees < 2);
    /* Backlog is - work to be done - work completed. */
    backlog = (1U << (level - 1)) * (nr_trees - 2) + prev_level_units - this_level_units;

    debug_merges("Checking whether to merge the next unit. tlu=%d, plu=%d, nt=%d\n",
            this_level_units, prev_level_units, nr_trees);

    /* We should not have any active tokens (tokens are returned to the driver merge on unit 
       complete). */
    BUG_ON(da->levels[level].merge.active_token != NULL);

    /* If we have merge backlog of more than 1 unit, schedule it without any further checks. */ 
    if(exit_cond || ((level != 1) && (backlog > 1)))
    {
        debug_merges("Unthrottled merge.\n");
        goto ready_out;
    }

    /* Otherwise, there are two cases. Either this merge is a driver merge, or not. */
    if ((level == da->driver_merge) && (level == 1 || da->levels[level-1].nr_trees < 2))
    {
        debug_merges("This is a driver merge.\n");
        /* Return any tokens that we may have. Should that actually every happen?. */
        while((token = castle_da_merge_token_get(da, level)))
        {
            printk("WARNING: merge token in a driver merge!.\n");
            castle_da_merge_token_return(da, level, token);
            not_ready_wake = 1;
        }
        /* If we are a driver merge, check whether we can generate a token to make progress. */ 
        if(da->levels[level].merge.driver_token != NULL)
        {
            debug_merges("The merge has an outstanding driver token.\n");
            goto not_ready_out;
        }
        /* Generate the token. */
        token = castle_da_merge_token_generate(da, level);
        /* Activate the token. */
        castle_da_merge_token_activate(da, level, token); 
        goto ready_out;
    }
    
    /* We are not driving merges, and the backlog <= 1. We are only allowed to make progress
       if backlog==1 _and_ we can activate a token. */
    if(backlog == 1)
    {
        token = castle_da_merge_token_get(da, level);
        if(!token)
        {
            debug_merges("Backlog of 1, but no token.\n");
            goto not_ready_out;
        }

        debug_merges("Deamortised merge currently at %d units, token from driver level %d.\n", 
                this_level_units, token->driver_level);
        /* Activate the token. */
        castle_da_merge_token_activate(da, level, token); 
        /* We already had a ref to this token, before doing activate. Activate took one more,
           return one of them back. */
        BUG_ON(token->ref_cnt < 2);
        /* This only does ref_cnt--, because ref_cnt is >= 2 */
        castle_da_merge_token_return(da, level, token);

        goto ready_out;
    }

    debug_merges("The merge is ahead (backlog=%d)\n", backlog);
    /* We are not driving merges, and the backlog <= 0. We are therefore ahead of other merges,
       and therefore we shoud not hold on to any tokens we may have on our inactive token list. */
    BUG_ON(backlog > 0);
    while((token = castle_da_merge_token_get(da, level)))
    {
        debug_merges("Pushing token for driver_level=%d\n", token->driver_level);
        castle_da_merge_token_push(da, level, token);
        /* We are getting rid of the token, therefore we must drop the ref to it. */
        castle_da_merge_token_return(da, level, token);
        not_ready_wake = 1;
    }

not_ready_out:
    write_unlock(&da->lock);
    if(not_ready_wake)
        wake_up(&da->merge_waitq);
    return 0;

ready_out:
    da->levels[level].merge.units_commited = this_level_units+1; 
    write_unlock(&da->lock);
    wake_up(&da->merge_waitq);
    return 1;
}

static inline uint32_t castle_da_merge_units_inc_return(struct castle_double_array *da, int level)
{
    int ignore;

    /* Wait until we are allowed to proceed with the merge. */
    __wait_event_interruptible(da->merge_waitq, castle_da_merge_wait_event(da, level), ignore);
    debug_merges("Merging unit %d.\n", da->levels[level].merge.units_commited);

    return da->levels[level].merge.units_commited;
}

static inline void castle_da_merge_unit_complete(struct castle_double_array *da, int level)
{
    struct castle_merge_token *token;

    debug_merges("Completing unit %d\n", da->levels[level].merge.units_commited);
    BUG_ON(!castle_da_is_locked(da));
    /* We'll be looking at level+1, make sure we don't go out of bounds. */
    BUG_ON(level+1 >= MAX_DA_LEVEL);

    /* Return the token back to the driver merge, if we've got one. */ 
    if((token = da->levels[level].merge.active_token))
    {
        debug_merges("Returning an active merge token for driver_level=%d\n", token->driver_level);
        castle_da_merge_token_return(da, level, token); 
        da->levels[level].merge.active_token = NULL;
    }
    /* Wakeup everyone waiting on merge state update. */
    wake_up(&da->merge_waitq);
}

static inline void castle_da_merge_intermediate_unit_complete(struct castle_double_array *da, 
                                                              int level)
{
    write_lock(&da->lock);
    castle_da_merge_unit_complete(da, level);
    write_unlock(&da->lock);
}

/**
 *
 * WARNING: Caller must hold da write lock.
 */
static inline void castle_da_driver_merge_reset(struct castle_double_array *da)
{
    int level;

    /* Function should be called with DA locked. */
    BUG_ON(!castle_da_is_locked(da));

    if (!castle_dynamic_driver_merge)
    {
        da->driver_merge = 1;
        return;
    }

    /* Set the lowest level with two fullygrown trees as driver. */
    for (level=1; level<MAX_DA_LEVEL; level++)
    {
        if (da->levels[level].nr_trees >= 2)
        {
            if (level != da->driver_merge)
                printk("Changing driver merge %d -> %d\n", da->driver_merge, level);
            da->driver_merge = level;
            break;
        }
    }
}

/**
 * Computes the appropriate level to put the output array from a total merge.
 */
static int castle_da_total_merge_output_level_get(struct castle_double_array *da,
                                                  struct castle_component_tree *out_tree)
{
    int i, nr_units, unit_is_tree, out_tree_level;

    /* DA should be write locked => we shouldn't be able to read lock. */
    BUG_ON(read_can_lock(&da->lock));
    out_tree_level = 1;
    /* Take either MAX_DYNAMIC_TREE_SIZE or MAX_DYNAMIC_DATA_SIZE as unit - based on 
     * which part of the out_tree is bigger. */
    unit_is_tree = (atomic64_read(&out_tree->tree_ext_free.used) >
                    atomic64_read(&out_tree->data_ext_free.used));
    
    /* Calculate the output size (in terms of # of units). */
    nr_units = (unit_is_tree)? 
               atomic64_read(&out_tree->tree_ext_free.used) / (MAX_DYNAMIC_TREE_SIZE * C_CHK_SIZE): 
               atomic64_read(&out_tree->data_ext_free.used) / (MAX_DYNAMIC_DATA_SIZE * C_CHK_SIZE); 

    /* Calculate the level it should go. Logarithm of nr_units. */
    out_tree_level = order_base_2(nr_units);
    /* Total merge output _must_ be put in level 2+, because we don't want to mix different tree
       types in level 1, and of course we don't want to put it in level 0 either. */
    if(out_tree_level <= 1)
        out_tree_level = 2;
    printk("Total merge: #units: %d, size appropriate for level: %d\n", nr_units, out_tree_level);
    /* Make sure no other trees exist above this level. */
    for (i=MAX_DA_LEVEL-1; i>=out_tree_level; i--)
        if (da->levels[i].nr_trees)
            break;
    out_tree_level = i+1;
    printk("Outputting at level: %d\n", out_tree_level);

    return out_tree_level;
}

static tree_seq_t castle_da_merge_last_unit_complete(struct castle_double_array *da, 
                                                     int level, 
                                                     struct castle_da_merge *merge)
{
    struct castle_component_tree *out_tree;
    struct castle_merge_token *token;
    tree_seq_t out_tree_id;
    int i;
    
    out_tree = castle_da_merge_complete(merge);
    if(!out_tree)
        return INVAL_TREE;

    out_tree_id = out_tree->seq;
    /* If we succeeded at creating the last tree, remove the in_trees, and add the out_tree.
       All under appropriate locks. */
    CASTLE_TRANSACTION_BEGIN;

    /* Get the lock. */
    write_lock(&merge->da->lock);
    /* Notify interested parties about merge completion, _before_ moving trees around. */ 
    castle_da_merge_unit_complete(da, level);
    /* If this was a total merge, the output level needs to be computed. 
       Otherwise the level should already be set to the next level up. */
    if(level == BIG_MERGE)
        out_tree->level = castle_da_total_merge_output_level_get(da, out_tree);
    else
        BUG_ON(out_tree->level != level + 1);
    /* Delete the old trees from DA list.
       Note 1: Old trees may still be used by IOs and will only be destroyed on the last ct_put. 
               But we want to remove it from the DA straight away. The out_tree now takes over 
               their functionality.
       Note 2: DA structure modifications don't race with checkpointing because transaction lock 
               is taken.
     */
    FOR_EACH_MERGE_TREE(i, merge) 
    {
        BUG_ON(merge->da->id != merge->in_trees[i]->da);
        castle_component_tree_del(merge->da, merge->in_trees[i]);
    }
    castle_component_tree_add(merge->da, out_tree, NULL /*head*/, 0 /*not in init*/);
    /* Reset the number of completed units. */ 
    BUG_ON(da->levels[level].merge.units_commited != (1U << level));
    da->levels[level].merge.units_commited = 0;
    /* Return any merge tokens we may still hold if we are not going to be doing more merges. */ 
    if(da->levels[level].nr_trees < 2)
    {
        while((token = castle_da_merge_token_get(da, level)))
        {
            debug_merges("Returning merge token from completed merge, driver_level=%d\n",
                    token->driver_level);
            castle_da_merge_token_return(da, level, token);
        }
    }
    castle_da_driver_merge_reset(da);
    /* Release the lock. */
    write_unlock(&merge->da->lock);

    CASTLE_TRANSACTION_END;
    castle_da_merge_restart(da, NULL);

    printk("Completed merge at level: %d and deleted %u entries\n",
            merge->level, merge->skipped_count);

    return out_tree_id;
} 

/**
 * Initialize merge process for multiple component trees. Merges, other than
 * compaction, process on 2 trees only.
 *
 * @param da [in] doubling array to be merged
 * @param level [in] merge level in doubling array
 * @param nr_trees [in] number of trees to be merged
 * @param in_trees [in] component trees to be merged
 *
 * @return intialized merge structure. NULL in case of error. 
 */
static struct castle_da_merge* castle_da_merge_init(struct castle_double_array *da,
                                                    int level,
                                                    int nr_trees,
                                                    struct castle_component_tree **in_trees)
{
    struct castle_btree_type *btree;
    struct castle_da_merge *merge = NULL;
    int i, ret;

    debug_merges("Merging ct=%d (dynamic=%d) with ct=%d (dynamic=%d)\n", 
             in_trees[0]->seq, in_trees[0]->dynamic, in_trees[1]->seq, in_trees[1]->dynamic);

    /* Sanity checks. */
    BUG_ON(nr_trees < 2);
    BUG_ON(da->levels[level].merge.units_commited != 0);
    BUG_ON((level != BIG_MERGE) && (nr_trees != 2));
    /* Work out what type of trees are we going to be merging. Bug if in_trees don't match. */
    btree = castle_btree_type_get(in_trees[0]->btree_type);
    for (i=0; i<nr_trees; i++)
    {
        /* Btree types may, and often will be different during big merges. */
        BUG_ON((level != BIG_MERGE) && (btree != castle_btree_type_get(in_trees[i]->btree_type)));
        BUG_ON((level != BIG_MERGE) && (in_trees[i]->level != level));
    }

    /* Malloc everything ... */
    ret = -ENOMEM;
    merge = castle_zalloc(sizeof(struct castle_da_merge), GFP_KERNEL);
    if(!merge)
        goto error_out;
    merge->out_tree          = castle_ct_alloc(da, RO_VLBA_TREE_TYPE, level+1);
    if(!merge->out_tree)
        goto error_out;
    merge->da                = da;
    merge->out_btree         = castle_btree_type_get(RO_VLBA_TREE_TYPE);
    merge->level             = level;
    merge->nr_trees          = nr_trees;
    merge->in_trees          = in_trees;
    merge->root_depth        = -1;
    merge->last_node_c2b     = NULL;
    merge->last_leaf_node_c2b= NULL;
    merge->last_key          = NULL;
    merge->first_node        = INVAL_EXT_POS;
    merge->completing        = 0;
    merge->nr_entries        = 0;
    merge->nr_nodes          = 0;
    merge->large_chunks      = 0;
    merge->budget_cons_rate  = 1; 
    merge->budget_cons_units = 0; 
    merge->is_new_key        = 1;
    for(i=0; i<MAX_BTREE_DEPTH; i++)
    {
        merge->levels[i].last_key      = NULL; 
        merge->levels[i].next_idx      = 0; 
        merge->levels[i].valid_end_idx = -1; 
        merge->levels[i].valid_version = INVAL_VERSION;  
    }
    merge->internal_ext_free.ext_id = INVAL_EXT_ID;
    merge->tree_ext_free.ext_id = INVAL_EXT_ID;
    merge->data_ext_free.ext_id = INVAL_EXT_ID;
    INIT_LIST_HEAD(&merge->large_objs);
#ifdef CASTLE_PERF_DEBUG
    merge->get_c2b_ns                   = 0;
    merge->merged_iter_next_ns          = 0;
    merge->da_medium_obj_copy_ns        = 0;
    merge->nodes_complete_ns            = 0;
    merge->budget_consume_ns            = 0;
    merge->progress_update_ns           = 0;
    merge->merged_iter_next_hasnext_ns  = 0;
    merge->merged_iter_next_compare_ns  = 0;
#endif
#ifdef CASTLE_DEBUG
    merge->is_recursion                 = 0;
#endif
    merge->skipped_count                = 0;
    /* Bit-arrays for snapshot delete algorithm. */
    merge->snapshot_delete.last_version = castle_version_max_get();
    printk("MERGE Level: %d, #versions: %d\n", level, merge->snapshot_delete.last_version);
    merge->snapshot_delete.occupied     = castle_malloc(merge->snapshot_delete.last_version / 8 + 1,
                                                        GFP_KERNEL);
    if (!merge->snapshot_delete.occupied)
        goto error_out;
    merge->snapshot_delete.need_parent  = castle_malloc(merge->snapshot_delete.last_version / 8 + 1,
                                                        GFP_KERNEL);
    if (!merge->snapshot_delete.need_parent)
        goto error_out;
    merge->snapshot_delete.next_deleted = NULL;

    ret = castle_da_iterators_create(merge);
    if(ret)
        goto error_out;
    ret = castle_da_merge_extents_alloc(merge);
    if(ret)
        goto error_out;
    
    return merge;

error_out:
    BUG_ON(!ret);
    castle_da_merge_dealloc(merge, ret);
    debug_merges("Failed a merge with ret=%d\n", ret);

    return NULL;
}

#ifdef CASTLE_PERF_DEBUG
static void castle_da_merge_perf_stats_flush_reset(struct castle_double_array *da,
                                                   struct castle_da_merge *merge,
                                                   uint32_t units_cnt)
{
    u64 ns;
    int i;

    /* Btree c2b_sync() time. */
    ns = 0;
    FOR_EACH_MERGE_TREE(i, merge) 
    {
        ns += in_trees[i]->bt_c2bsync_ns;
        in_trees[i]->bt_c2bsync_ns = 0;
    }
    castle_trace_da_merge_unit(TRACE_VALUE, 
                               TRACE_DA_MERGE_UNIT_C2B_SYNC_WAIT_BT_NS_ID,
                               da->id,
                               merge->level,
                               units_cnt,
                               ns);

    /* Data c2b_sync() time. */
    ns = 0;
    FOR_EACH_MERGE_TREE(i, merge) 
    {
        ns += in_trees[i]->data_c2bsync_ns;
        in_trees[i]->data_c2bsync_ns = 0;
    }
    castle_trace_da_merge_unit(TRACE_VALUE,
                               TRACE_DA_MERGE_UNIT_C2B_SYNC_WAIT_DATA_NS_ID,
                               da->id,
                               merge->level,
                               units_cnt,
                               ns);

    /* castle_cache_block_get() time. */
    castle_trace_da_merge_unit(TRACE_VALUE,
                               TRACE_DA_MERGE_UNIT_GET_C2B_NS_ID,
                               da->id,
                               merge->level,
                               units_cnt,
                               merge->get_c2b_ns);
    merge->get_c2b_ns = 0;

    /* Merge time. */
    castle_trace_da_merge_unit(TRACE_VALUE,
                               TRACE_DA_MERGE_UNIT_MOBJ_COPY_NS_ID,
                               da->id,
                               merge->level,
                               units_cnt,
                               merge->da_medium_obj_copy_ns);
    merge->da_medium_obj_copy_ns = 0;

}
#endif /* CASTLE_PERF_DEBUG */


/**
 * Merge multiple trees into one. The same function gets used by both compaction
 * (total merges) and standard 2 tree merges. 
 *
 * @param da [in] doubling array to be merged
 * @param nr_trees [in] number of trees to be merged
 * @param in_trees [in] list of trees
 * @param level [in] level of the double array - 0 for total merge
 *
 * @return non-zero if failure 
 */
static int castle_da_merge_do(struct castle_double_array *da, 
                              int nr_trees,
                              struct castle_component_tree *in_trees[],
                              int level)
{
    struct castle_da_merge *merge;
    uint32_t units_cnt;
    tree_seq_t out_tree_id;
    int ret;

    castle_trace_da_merge(TRACE_START,
                          TRACE_DA_MERGE_ID,
                          da->id,
                          level,
                          in_trees[0]->seq,
                          in_trees[1]->seq);

    merge = castle_da_merge_init(da, level, nr_trees, in_trees);
    if(!merge)
    {
        printk("Could not start a merge for DA=%d, level=%d.\n", da->id, level);
        return -EAGAIN;
    }
#ifdef DEBUG
    debug_merges("MERGE START - L%d -> ", level);
    FOR_EACH_MERGE_TREE(i, merge)
        debug_merges("[%d]", merge->in_trees[i]->seq);
    debug_merges("\n");
#endif

    /* Hard-pin T1s in the cache. */
    if (level == 1)
    {
        castle_cache_advise((c_ext_pos_t){in_trees[0]->data_ext_free.ext_id, 0},
                C2_ADV_EXTENT|C2_ADV_HARDPIN, -1, -1, 0);
        castle_cache_advise((c_ext_pos_t){in_trees[1]->data_ext_free.ext_id, 0},
                C2_ADV_EXTENT|C2_ADV_HARDPIN, -1, -1, 0);
    }
    /* Do the merge. */
    do {
        /* Wait until we are allowed to do next unit of merge. */
        units_cnt = castle_da_merge_units_inc_return(da, level);
        /* Trace event. */
        castle_trace_da_merge_unit(TRACE_START,
                                   TRACE_DA_MERGE_UNIT_ID,
                                   da->id,
                                   level,
                                   units_cnt,
                                   0);
        /* Perform the merge work. */
        ret = castle_da_merge_unit_do(merge, units_cnt);
        /* Trace event. */
        castle_trace_da_merge_unit(TRACE_END,
                                   TRACE_DA_MERGE_UNIT_ID,
                                   da->id,
                                   level,
                                   units_cnt,
                                   0);
        debug_merges("Completing %d unit for merge at level: %d\n", units_cnt, level);

#ifdef CASTLE_PERF_DEBUG
        /* Output & reset performance stats. */
        castle_da_merge_perf_stats_flush_reset(da, merge, units_cnt);
#endif
        /* Exit on errors. */
        if(ret < 0)
        {
            out_tree_id = INVAL_TREE;
            goto merge_failed;
        }
        /* Only ret>0 we are expecting to continue, i.e. ret==EAGAIN. */
        BUG_ON(ret && (ret != EAGAIN));
        /* Notify interested parties that we've completed current merge unit. */
        if(ret == EAGAIN)
            castle_da_merge_intermediate_unit_complete(da, level);
    } while(ret);

    /* Finish the last unit, packaging the output tree. */
    out_tree_id = castle_da_merge_last_unit_complete(da, level, merge);
    ret = TREE_INVAL(out_tree_id) ? -ENOMEM : 0;
merge_failed:
    /* Unhard-pin T1s in the cache. Do this before we deallocate the merge and extents. */
    if (level == 1)
    {
        castle_cache_advise_clear((c_ext_pos_t){in_trees[0]->data_ext_free.ext_id, 0},
                C2_ADV_EXTENT|C2_ADV_HARDPIN, -1, -1, 0);
        castle_cache_advise_clear((c_ext_pos_t){in_trees[1]->data_ext_free.ext_id, 0},
                C2_ADV_EXTENT|C2_ADV_HARDPIN, -1, -1, 0);
    }

    debug_merges("MERGE END - L%d -> [%u]\n", level, out_tree_id);
    castle_da_merge_dealloc(merge, ret);
    castle_trace_da_merge(TRACE_END, TRACE_DA_MERGE_ID, da->id, level, out_tree_id, 0);
    if(ret)
    {
        printk("Merge for DA=%d, level=%d, failed to merge err=%d.\n", da->id, level, ret);
        return -EAGAIN;
    }

    return 0;
}

/**
 * Marks the DA 'dirty', i.e. that a total merge will be required to deal with snapshot deletion.
 *
 * @param da_id     DA id to mark as dirty.
 */
void castle_da_version_delete(da_id_t da_id)
{
    atomic_inc(&(castle_da_hash_get(da_id)->nr_del_versions));
}

/**
 * Checks for ongoing merge units in any of the merges above the given level.
 *
 * @param da    Doubling array.
 * @param level Search will start with level+1 
 * @return 0:   If no ongoing merge units.
 * @return 1:   If there are some ongoing merge units.
 */
static int castle_da_merge_units_ongoing(struct castle_double_array *da, int level)
{
    int i;

    BUG_ON(write_can_lock(&da->lock));
    /* Check for ongoing merge units on top levels. */
    for(i=level+1; i<MAX_DA_LEVEL; i++)
    {
        /* Check for ongoing merge units. */
        if (da->levels[i].merge.active_token)
            return 1;
    }

    return 0;
}

/**
 * Determines whether to do a total merge. 
 *
 * Do not do big-merge in case: 
 *  - DA is frozen 
 *  - DA is not marked for compaction
 *  - there is a ongoing merge unit
 *
 * @param da [in] DA id. 
 * @return whether to start big-merge or not.
 */
static int castle_da_big_merge_trigger(struct castle_double_array *da)
{
    int ret = 0;

    write_lock(&da->lock);

    if (_castle_da_frozen(da))
        goto out;

    /* Check if marked for compaction. */
    if (!da->compacting)
    {
        debug_merges("Not marked for compaction.\n");
        goto out;
    }

    /* Make sure there are no ongoing merge units anywhere. */
    if (castle_da_merge_units_ongoing(da, 0))
    {
        debug_merges("Total merge cannot be triggered - ongoing merges\n");
        goto out;
    }
    
    /* All checks succeeded, total merge can start. */ 
    ret = 1;

out:
    write_unlock(&da->lock);

    return ret;
}

/**
 * Do a total merge on all trees in a DA. Triggered, after completing the last level
 * merge, if any versions marked for deletion.
 *
 * @param da_p [in] doubling array to run total merge on.
 */
static int castle_da_big_merge_run(void *da_p)
{
    struct castle_double_array *da = (struct castle_double_array *)da_p;
    struct castle_component_tree **in_trees;
    struct list_head *l;
    int level=0, ignore;
    int nr_trees, nr_trees_estimate;
    int i;

    /* Disable deamortization of total merges. */
    da->levels[BIG_MERGE].merge.deamortize = 0;

    debug_merges("Starting big-merge thread.\n");
    do {
        /* Start big-merge only when the DA has versions marked for deletion
         * and only after completing the top-level merge(to make sure no merge
         * is going on). */
        __wait_event_interruptible(da->merge_waitq,
                                   exit_cond || castle_da_big_merge_trigger(da),
                                   ignore);
       
        /* Exit without doing a merge, if we are stopping execution, or da has been deleted. */ 
        if(exit_cond)
            break;

        /* Otherwise do a merge. */
        printk("Triggered a total merge.\n");

        /* Allocate array for in_tree pointers, but do that without holding the lock. */
        in_trees = NULL;
read_trees_again:
        /* If we jump to wait_and_try from here, in_trees must be NULL. */
        BUG_ON(in_trees != NULL);
        /* Lock the DA, because we may reset the compacting flag. */
        write_lock(&da->lock);
        nr_trees_estimate = 0;
        for (level=1; level<MAX_DA_LEVEL; level++)
            nr_trees_estimate += da->levels[level].nr_trees;
        /* Merge cannot be scheduled with < 2 trees. */
        if(nr_trees_estimate < 2)
        {
            /* Don't compact any more (not enough trees). */
            da->compacting = 0;
            write_unlock(&da->lock);
            goto wait_and_try;
        }
        write_unlock(&da->lock);
        /* Allocate in_trees array for appropriate number of trees. */
        in_trees = castle_zalloc(sizeof(struct castle_component_tree *) * nr_trees_estimate, 
                                 GFP_KERNEL);
        if (!in_trees)
            goto wait_and_try;
        
        /* Now, lock the DA, confirm the #trees, either retry again or start the merge. */
        write_lock(&da->lock);
        nr_trees = 0;
        for (level=1; level<MAX_DA_LEVEL; level++)
            nr_trees += da->levels[level].nr_trees;
        /* If the # of trees changed, free the array, and try again. */
        if(nr_trees != nr_trees_estimate)
        {
            write_unlock(&da->lock);
            castle_free(in_trees);
            in_trees = NULL;
            goto read_trees_again;
        }
        /* Number of trees still the same, construct the array of trees that will be merged. */
        for (level=1, i=0; level<MAX_DA_LEVEL; level++)
        {
            list_for_each(l, &da->levels[level].trees)
            {
                in_trees[i] = list_entry(l, struct castle_component_tree, da_list);
                in_trees[i++]->compacting = 1;
                da->levels[level].nr_trees--;
                da->levels[level].nr_compac_trees++;
                BUG_ON(i > nr_trees);
            }
        }
        BUG_ON(i != nr_trees);
        
        da->compacting = 0;
        atomic_set(&da->nr_del_versions, 0);

        /* Unlock the DA. */
        write_unlock(&da->lock);

        /* Wakeup everyone waiting on merge state update. */
        wake_up(&da->merge_waitq);

        printk("Starting total merge on %d trees\n", nr_trees);

        /* Do the merge. If fails, retry after 10s. */
        if (castle_da_merge_do(da, nr_trees, in_trees, BIG_MERGE))
        {
wait_and_try:
            printk("Total merge failed\n");
            /* If the merge was actually scheduled (i.e. some trees were collected),
               but failed afterward (e.g. due to NOSPC), readjust the counters again. */
            if (in_trees)
            {
                write_lock(&da->lock);
                for (i=0; i<nr_trees; i++)
                    in_trees[i]->compacting = 0;

                for (i=0; i<MAX_DA_LEVEL; i++)
                {
                    da->levels[i].nr_trees += da->levels[i].nr_compac_trees;
                    da->levels[i].nr_compac_trees = 0;
                }
                write_unlock(&da->lock);
                castle_free(in_trees);
                in_trees = NULL;
            }
            /* Wakeup everyone waiting on merge state update. */
            wake_up(&da->merge_waitq);
            /* In case we failed the merge because of no memory for in_trees, wait and retry. */
            msleep(10000);
        }
    } while(1);

    debug_merges("Merge thread exiting.\n");

    write_lock(&da->lock);
    /* Remove ourselves from the da merge threads array to indicate that we are finished. */  
    da->levels[BIG_MERGE].merge.thread = NULL;
    write_unlock(&da->lock);
    /* castle_da_alloc() took a reference for us, we have to drop it now. */
    castle_da_put(da);

    return 0;
}

/**
 * Determines whether to do merge or not. 
 *
 * Do not do merge if one of following is true:
 *  - DA is frozen 
 *  - DA is marked for compaction
 *  - There is a ongoing merge unit at a level above
 *
 * @param da [in] doubling array to check for
 * @param level [out] merge level
 *
 * @return whether to start merge or not.
 */
static int castle_da_merge_trigger(struct castle_double_array *da, int level)
{
    int ret = 0;

    read_lock(&da->lock);

    if (_castle_da_frozen(da))
        goto out;

    if (da->levels[level].nr_trees < 2)
        goto out;

    /* Make sure there are no ongoing merge units on top levels. */
    /* (or) if doubling array marked for compaction, dont start merges yet. Let
     * the compaction start first. */
    if (castle_da_merge_units_ongoing(da, level) || da->compacting)
    {
        debug_merges("Merge %d cant be triggered - ongoing merges or compaction.\n", level);
        goto out;
    }

    ret = 1;

out:
    read_unlock(&da->lock);
    return ret;
}

/**
 * Merge doubling array trees at a level.
 *
 * @param da_p [in] Doubling array to do merge on.
 */
static int castle_da_merge_run(void *da_p)
{
    struct castle_double_array *da = (struct castle_double_array *)da_p;
    struct castle_component_tree *in_trees[2];
    struct list_head *l;
    int level, ignore;

    /* Work out the level at which we are supposed to be doing merges.
       Do that by working out where is this thread in threads array. */
    for(level=1; level<MAX_DA_LEVEL; level++)
        if(da->levels[level].merge.thread == current)
            break;
    BUG_ON(level >= MAX_DA_LEVEL);

    /* Enable deamortization of normal merges. */
    da->levels[level].merge.deamortize = 1;

    debug_merges("Starting merge thread.\n");
    do {
        /* Wait for 2+ trees to appear at this level. DA must not be frozen either. */
        __wait_event_interruptible(da->merge_waitq,
                    exit_cond || (castle_da_merge_trigger(da, level)),
                    ignore);
        
        /* Exit without doing a merge, if we are stopping execution, or da has been deleted. */ 
        if(exit_cond)
            break;

        /* Otherwise do a merge. */
        in_trees[0] = in_trees[1] = NULL;

        read_lock(&da->lock);
        BUG_ON(da->compacting);
        list_for_each_prev(l, &da->levels[level].trees)
        {
            struct castle_component_tree *ct = 
                                list_entry(l, struct castle_component_tree, da_list);

            /* If there are any trees being compacted, they must be older than the
               two trees we want to merge here. */
            BUG_ON(ct->compacting);

            if(!in_trees[1])
                in_trees[1] = ct;
            else 
            if(!in_trees[0])
                in_trees[0] = ct;
        }
        read_unlock(&da->lock);

        BUG_ON(!in_trees[0] || !in_trees[1]);

        debug_merges("Doing merge, trees=[%u]+[%u]\n", in_trees[0]->seq, in_trees[1]->seq);

        /* Do the merge. If fails, retry after 10s. */
        if (castle_da_merge_do(da, 2, in_trees, level))
        {
            msleep(10000);
            continue;
        }
    } while(1);

    debug_merges("Merge thread exiting.\n");

    write_lock(&da->lock);
    /* Remove ourselves from the da merge threads array to indicate that we are finished. */  
    da->levels[level].merge.thread = NULL;
    write_unlock(&da->lock);
    /* castle_da_alloc() took a reference for us, we have to drop it now. */
    castle_da_put(da);

    return 0;
}

static int __castle_da_threads_priority_set(struct castle_double_array *da, void *_value);

static int castle_da_merge_start(struct castle_double_array *da, void *unused)
{
    int i;

    /* Wake up all of the merge threads. */
    for(i=0; i<MAX_DA_LEVEL; i++)
        wake_up_process(da->levels[i].merge.thread);

    __castle_da_threads_priority_set(da, &castle_nice_value);

    return 0;
}

static int castle_da_merge_stop(struct castle_double_array *da, void *unused)
{
    int i;

    /* castle_da_exiting should have been set by now. */
    BUG_ON(!exit_cond);
    wake_up(&da->merge_waitq);
    for(i=0; i<MAX_DA_LEVEL; i++)
    {
        while(da->levels[i].merge.thread)
            msleep(10);
        printk("Stopped merge thread for DA=%d, level=%d\n", da->id, i);
    }

    return 0;
}

/**
 * Enable/disable inserts for da and wake-up merge thread.
 *
 * @param da    Doubling array to throttle and merge
 */
static int castle_da_merge_restart(struct castle_double_array *da, void *unused)
{
    debug("Restarting merge for DA=%d\n", da->id);

    write_lock(&da->lock);
    if (da->levels[1].nr_trees >= 4 * request_cpus.cnt)
    {
        if (da->ios_rate != 0)
        {
            printk("Disabling inserts on da=%d.\n", da->id);
            castle_trace_da(TRACE_START, TRACE_DA_INSERTS_DISABLED_ID, da->id, 0);
        }
        da->ios_rate = 0; 
    }
    else
    {
        if (da->ios_rate == 0)
        {
            printk("Enabling inserts on da=%d.\n", da->id);
            castle_trace_da(TRACE_END, TRACE_DA_INSERTS_DISABLED_ID, da->id, 0);
        }
        da->ios_rate = INT_MAX;
    }
    write_unlock(&da->lock);
    wake_up(&da->merge_waitq);

    return 0;
}

static void castle_da_merges_print(struct castle_double_array *da)
{
    struct castle_merge_token *token;
    struct list_head *l;
    int level, print;
    struct timeval time;
                       
    print = 0;
    do_gettimeofday(&time);
    read_lock(&da->lock);
    printk("\nPrinting merging stats for DA=%d, t=(%ld,%ld)\n", 
            da->id, time.tv_sec, time.tv_usec/1000);
    for(level=MAX_DA_LEVEL-1; level>0; level--)
    {
        if(!print && (da->levels[level].nr_trees == 0))
            continue;
        print = 1;
        printk(" level[%.2d]: nr_trees=%d, units_commited=%.3d,"
              " active_token_dl=%.2d, driver_token_dl=%.2d\n",
              level,
              da->levels[level].nr_trees,
              da->levels[level].merge.units_commited,
              da->levels[level].merge.active_token ? 
                da->levels[level].merge.active_token->driver_level : 0,
              da->levels[level].merge.driver_token ? 
                da->levels[level].merge.driver_token->driver_level : 0);
        list_for_each(l, &da->levels[level].merge.merge_tokens)
        {
            token = list_entry(l, struct castle_merge_token, list);
            printk("  merge_token_dl=%d\n", token->driver_level);
        }
    }
    printk("\n");
    read_unlock(&da->lock);
}

/**********************************************************************************************/
/* Generic DA code */

/**
 * Return whether the da is write-locked.
 *
 * NOTE: Calling read_can_lock() with a write-lock should be race safe, unlike
 *       calling it with just a read-lock.
 */
static inline int castle_da_is_locked(struct castle_double_array *da)
{
    /* must be write-locked if readers can't get a lock, or we have 2^24 readers */
    return !read_can_lock(&da->lock);
}

static int castle_da_ct_dec_cmp(struct list_head *l1, struct list_head *l2)
{
    struct castle_component_tree *ct1 = list_entry(l1, struct castle_component_tree, da_list);
    struct castle_component_tree *ct2 = list_entry(l2, struct castle_component_tree, da_list);
    BUG_ON(ct1->seq == ct2->seq);

    return ct1->seq > ct2->seq ? -1 : 1;
}

/**
 * Calculate hash of userland key (okey) length key_len and modulo for cpu_index.
 *
 * @FIXME named wrongly?
 *
 * @FIXME Currently hashes just the first dimension of the key which will not
 *        be terribly even in distributing load among the btrees under certain
 *        circumstances.  This will likely go away when we hash the bkey as part
 *        of the T0 hash refactoring that is scheduled.
 *
 * @return  Offset into request_cpus.cpus[]
 */
int castle_double_array_okey_cpu_index(c_vl_okey_t *okey, uint32_t key_len)
{
    if (likely(okey->nr_dims > 0))
        return murmur_hash_32(okey->dims[0],
                              okey->dims[0]->length,
                              (uint32_t)0xDA82B27204D27F7)
            % request_cpus.cnt;
    else
        return 0;
}

/**
 * Get cpu id for specified cpu_index.
 *
 * @FIXME named wrongly?
 *
 * @return  CPU id
 */
int castle_double_array_request_cpu(int cpu_index)
{
    return request_cpus.cpus[cpu_index];
}

/**
 * Get number of cpus handling requests.
 *
 * @return  Number of cpus handling requests.
 */
int castle_double_array_request_cpus(void)
{
    return request_cpus.cnt;
}

/**
 * Allocate write IO wait queues for specified DA.
 *
 * @return  EXIT_SUCCESS    Successfully allocated wait queues
 * @return  1               Failed to allocate wait queues
 *
 * @also castle_da_rwct_create()
 */
static int castle_da_wait_queue_create(struct castle_double_array *da, void *unused)
{
    int i;

    da->ios_waiting = castle_malloc(request_cpus.cnt * sizeof(struct castle_da_io_wait_queue),
            GFP_KERNEL);
    if (!da->ios_waiting)
        return 1;

    for (i = 0; i < request_cpus.cnt; i++)
    {
        spin_lock_init(&da->ios_waiting[i].lock);
        INIT_LIST_HEAD(&da->ios_waiting[i].list);
        CASTLE_INIT_WORK(&da->ios_waiting[i].work, castle_da_queue_kick);
        da->ios_waiting[i].cnt = 0;
        da->ios_waiting[i].da = da;
    }

    return 0;
}

/**
 * Deallocate doubling array and all associated data.
 *
 * @param da    Doubling array for deallocate
 *
 * - Merge threads
 * - IO wait queues
 */
static void castle_da_dealloc(struct castle_double_array *da)
{
    int i;

    for (i=0; i<MAX_DA_LEVEL; i++)
    {
        if(da->levels[i].merge.thread != NULL)
            kthread_stop(da->levels[i].merge.thread);
    }
    if (da->ios_waiting)
        castle_free(da->ios_waiting);
    /* Poison and free (may be repoisoned on debug kernel builds). */
    memset(da, 0xa7, sizeof(struct castle_double_array));
    castle_free(da);
}

static struct castle_double_array* castle_da_alloc(da_id_t da_id)
{
    struct castle_double_array *da;
    int i = 0;

    da = castle_zalloc(sizeof(struct castle_double_array), GFP_KERNEL); 
    if(!da)
        return NULL; 

    printk("Allocating DA=%d\n", da_id);
    da->id              = da_id; 
    da->root_version    = INVAL_VERSION;
    rwlock_init(&da->lock);
    da->flags           = 0;
    da->nr_trees        = 0;
    atomic_set(&da->ref_cnt, 1);
    da->attachment_cnt  = 0;
    atomic_set(&da->ios_waiting_cnt, 0);
    if (castle_da_wait_queue_create(da, NULL) != EXIT_SUCCESS)
        goto err_out;
    atomic_set(&da->ios_budget, 0);
    da->ios_rate        = 0;
    da->last_key        = NULL;
    da->top_level       = 0;
    atomic_set(&da->nr_del_versions, 0);
    da->compacting      = 0;
    /* For existing double arrays driver merge has to be reset after loading it. */
    da->driver_merge    = -1;
    atomic_set(&da->epoch_ios, 0);
    atomic_set(&da->merge_budget, 0);
    init_waitqueue_head(&da->merge_waitq);
    init_waitqueue_head(&da->merge_budget_waitq);
    /* Initialise the merge tokens list. */
    INIT_LIST_HEAD(&da->merge_tokens);
    for(i=0; i<MAX_DA_LEVEL; i++)
    {
        da->merge_tokens_array[i].driver_level = -1; 
        da->merge_tokens_array[i].ref_cnt      = 0; 
        list_add(&da->merge_tokens_array[i].list, &da->merge_tokens);
    }
    for(i=0; i<MAX_DA_LEVEL; i++)
    {
        INIT_LIST_HEAD(&da->levels[i].trees);
        da->levels[i].nr_trees             = 0;
        da->levels[i].nr_compac_trees      = 0;
        INIT_LIST_HEAD(&da->levels[i].merge.merge_tokens);
        da->levels[i].merge.active_token   = NULL;
        da->levels[i].merge.driver_token   = NULL;
        da->levels[i].merge.units_commited = 0;
        da->levels[i].merge.thread         = NULL;

        /* Create merge threads, and take da ref for all levels >= 1. */
        castle_da_get(da);
        printk("Starting thread: %d\n", i);
        da->levels[i].merge.thread = 
            kthread_create((i == BIG_MERGE)? castle_da_big_merge_run: castle_da_merge_run, 
                           da, "castle-m-%d-%.2d", da_id, i);

        if(!da->levels[i].merge.thread)
            goto err_out;
    }
    printk("Allocated DA=%d successfully.\n", da_id);

    return da;

err_out:
#ifdef CASTLE_DEBUG
    {
        int j;
        for(j=0; j<MAX_DA_LEVEL; j++)
        {
            BUG_ON((j<i)  && (da->levels[j].merge.thread == NULL));
            BUG_ON((j>=i) && (da->levels[j].merge.thread != NULL));
        }
    }
#endif
    castle_da_dealloc(da);

    return NULL;
}

void castle_da_marshall(struct castle_dlist_entry *dam,
                        struct castle_double_array *da)
{
    dam->id           = da->id;
    dam->root_version = da->root_version;
}
 
static void castle_da_unmarshall(struct castle_double_array *da,
                                 struct castle_dlist_entry *dam)
{
    da->id           = dam->id;
    da->root_version = dam->root_version;
    castle_sysfs_da_add(da);
}

struct castle_component_tree* castle_component_tree_get(tree_seq_t seq)
{
    return castle_ct_hash_get(seq);
}

/**
 * Insert ct into da->levels[ct->level].trees list at index.
 *
 * @param   da      To insert onto
 * @param   ct      To be inserted
 * @param   head    List head to add ct after (or NULL)
 * @param   in_init @FIXME
 *
 * WARNING: Caller must hold da->lock
 */
static void castle_component_tree_add(struct castle_double_array *da,
                                      struct castle_component_tree *ct,
                                      struct list_head *head,
                                      int in_init)
{
    struct castle_component_tree *cmp_ct;

    BUG_ON(da->id != ct->da);
    BUG_ON(ct->level >= MAX_DA_LEVEL);
    BUG_ON(!castle_da_is_locked(da));
    BUG_ON(!CASTLE_IN_TRANSACTION);

    /* Default insert point is the front of the list. */
    if (!head)
        head = &da->levels[ct->level].trees;

    /* CTs are sorted by decreasing seq number (newer trees towards the front
     * of the list) to guarantee newest values are returned during gets.
     *
     * Levels 0,1 are a special case as their seq numbers are 'prefixed' with
     * the cpu_index.  This means an older CT would appear before a newer CT if
     * it had a greater cpu_index prefixed.
     *
     * At level 0 this is valid because inserts are disjoint (they go to a
     * specific CT based on the key->cpu_index hash).
     * At level 1 this is valid because CTs from a given cpu_index are still in
     * order, and for the same reasons it is valid at level 0.
     *
     * Skip ordering checks during init (we sort the tree afterwards). */
    if (!in_init && !list_empty(&da->levels[ct->level].trees))
    {
        struct list_head *l;

        /* RWCTs at level 0 are promoted to level 1 in a random order based on how
         * many keys get hashed to which CPU.  As a result for inserts at level 1
         * we search the list to find the correct place to insert these trees. */
        if (ct->level == 1)
        {
            list_for_each(l, &da->levels[ct->level].trees)
            {
                cmp_ct = list_entry(l, struct castle_component_tree, da_list);
                if (ct->seq > cmp_ct->seq)
                    break; /* list_for_each() */
                head = l;
            }
        }

        /* CT seq should be < head->next seq (skip if head is the last elephant) */
        if (!list_is_last(head, &da->levels[ct->level].trees))
        {
            cmp_ct = list_entry(head->next, struct castle_component_tree, da_list);
            BUG_ON(ct->seq <= cmp_ct->seq);
        }
    }

    list_add(&ct->da_list, head);
    da->levels[ct->level].nr_trees++;
    da->nr_trees++;

    if (ct->level > da->top_level)
    {
        BUG_ON(!in_init && (da->top_level + 1 != ct->level));
        da->top_level = ct->level;
        printk("DA: %d growing one level to %d, del_vers: %d\n", 
                da->id, ct->level, atomic_read(&da->nr_del_versions));
        if (!in_init && atomic_read(&da->nr_del_versions))
        {
            printk("Marking DA for compaction\n");
            da->compacting = 1;
            wake_up(&da->merge_waitq);
        }
    }
}

/**
 * Unlink ct from da->level[ct->level].trees list.
 */
static void castle_component_tree_del(struct castle_double_array *da,
                                      struct castle_component_tree *ct)
{
    BUG_ON(da->id != ct->da);
    BUG_ON(!castle_da_is_locked(da));
    BUG_ON(!CASTLE_IN_TRANSACTION);
   
    list_del(&ct->da_list); 
    ct->da_list.next = NULL;
    ct->da_list.prev = NULL;
    if (ct->compacting)
        da->levels[ct->level].nr_compac_trees--;
    else
        da->levels[ct->level].nr_trees--;
    da->nr_trees--;
}

static void castle_ct_large_obj_writeback(struct castle_large_obj_entry *lo, 
                                          struct castle_component_tree *ct)
{
    struct castle_lolist_entry mstore_entry;

    mstore_entry.ext_id = lo->ext_id;
    mstore_entry.length = lo->length;
    mstore_entry.ct_seq = ct->seq;

    castle_mstore_entry_insert(castle_lo_store, &mstore_entry);
}

static void castle_ct_large_objs_remove(struct castle_component_tree *ct)
{
    struct list_head *lh, *tmp;

    list_for_each_safe(lh, tmp, &ct->large_objs)
    {
        struct castle_large_obj_entry *lo = 
                            list_entry(lh, struct castle_large_obj_entry, list);

        /* No need of locks as it is done in the removal context of CT. */
        list_del(&lo->list);
        castle_extent_put(lo->ext_id);
        castle_free(lo);
    }
}

int castle_ct_large_obj_add(c_ext_id_t              ext_id, 
                            uint64_t                length, 
                            struct list_head       *head,
                            struct mutex           *mutex)
{
    struct castle_large_obj_entry *lo;

    if (EXT_ID_INVAL(ext_id))
        return -EINVAL;

    lo = castle_malloc(sizeof(struct castle_large_obj_entry), GFP_KERNEL);
    if (!lo)
        return -ENOMEM;

    lo->ext_id = ext_id;
    lo->length = length;

    if (mutex) mutex_lock(mutex);
    list_add(&lo->list, head);
    if (mutex) mutex_unlock(mutex);

    return 0;
}

/**
 * Get a reference to the CT.
 *
 * @param ct    Component Tree to get bump reference count on
 * @param write True to get a write reference count
 *              False to get a read reference count
 *
 * NOTE: Caller should hold castle_da_lock.
 */
void castle_ct_get(struct castle_component_tree *ct, int write)
{
    atomic_inc(&ct->ref_count);
    if (write)
        atomic_inc(&ct->write_ref_count);
}

void castle_ct_put(struct castle_component_tree *ct, int write)
{
    BUG_ON(in_atomic());
    if(write)
        atomic_dec(&ct->write_ref_count);

    if(likely(!atomic_dec_and_test(&ct->ref_count)))
        return;

    BUG_ON(atomic_read(&ct->write_ref_count) != 0);

    debug("Ref count for ct id=%d went to 0, releasing.\n", ct->seq);
    /* If the ct still on the da list, this must be an error. */
    if(ct->da_list.next != NULL)
    {
        printk("CT=%d, still on DA list, but trying to remove.\n", ct->seq);
        BUG();
    }
    /* Destroy the component tree */
    BUG_ON(TREE_GLOBAL(ct->seq) || TREE_INVAL(ct->seq));
    castle_ct_hash_remove(ct);

    debug("Releasing freespace occupied by ct=%d\n", ct->seq);
    /* Freeing all large objects. */
    castle_ct_large_objs_remove(ct);

    /* Free the extents. */
    castle_ext_freespace_fini(&ct->internal_ext_free);
    castle_ext_freespace_fini(&ct->tree_ext_free);
    castle_ext_freespace_fini(&ct->data_ext_free);

    if (ct->last_key)
        castle_object_okey_free(ct->last_key);

    if (ct->bloom_exists)
        castle_bloom_destroy(&ct->bloom);

    /* Poison ct (note this will be repoisoned by kfree on kernel debug build. */
    memset(ct, 0xde, sizeof(struct castle_component_tree));
    castle_free(ct);
}

static int castle_da_trees_sort(struct castle_double_array *da, void *unused)
{
    int i;

    write_lock(&da->lock);
    for(i=0; i<MAX_DA_LEVEL; i++)
        list_sort(&da->levels[i].trees, castle_da_ct_dec_cmp);
    write_unlock(&da->lock);

    return 0;
}

void castle_da_ct_marshall(struct castle_clist_entry *ctm,
                           struct castle_component_tree *ct)
{
    int i;

    ctm->da_id       		= ct->da; 
    ctm->item_count  		= atomic64_read(&ct->item_count);
    ctm->btree_type  		= ct->btree_type; 
    ctm->dynamic     		= ct->dynamic;
    ctm->seq         		= ct->seq;
    ctm->level       		= ct->level;
    ctm->tree_depth  		= ct->tree_depth;
    ctm->root_node   		= ct->root_node;
    ctm->first_node  		= ct->first_node;
    ctm->first_node_size    = ct->first_node_size;
    ctm->last_node   		= ct->last_node;
    ctm->last_node_size     = ct->last_node_size;
    ctm->node_count  		= atomic64_read(&ct->node_count);
    ctm->large_ext_chk_cnt	= atomic64_read(&ct->large_ext_chk_cnt);
    for(i=0; i<MAX_BTREE_DEPTH; i++)
        ctm->node_sizes[i] = ct->node_sizes[i];

    castle_ext_freespace_marshall(&ct->internal_ext_free, &ctm->internal_ext_free_bs);
    castle_ext_freespace_marshall(&ct->tree_ext_free, &ctm->tree_ext_free_bs);
    castle_ext_freespace_marshall(&ct->data_ext_free, &ctm->data_ext_free_bs);

    ctm->bloom_exists = ct->bloom_exists;
    if (ct->bloom_exists)
        castle_bloom_marshall(&ct->bloom, ctm);
}

static da_id_t castle_da_ct_unmarshall(struct castle_component_tree *ct,
                                       struct castle_clist_entry *ctm)
{
    int i;

    ct->seq         		= ctm->seq;
    atomic_set(&ct->ref_count, 1);
    atomic_set(&ct->write_ref_count, 0);
    atomic64_set(&ct->item_count, ctm->item_count);
    ct->btree_type  		= ctm->btree_type; 
    ct->dynamic     		= ctm->dynamic;
    ct->da          		= ctm->da_id; 
    ct->level       		= ctm->level;
    ct->tree_depth  		= ctm->tree_depth;
    ct->root_node   		= ctm->root_node;
    ct->first_node  		= ctm->first_node;
    ct->first_node_size     = ctm->first_node_size;
    ct->last_node   		= ctm->last_node;
    ct->last_node_size      = ctm->last_node_size;
    ct->new_ct              = 0;
    ct->compacting          = 0;
    atomic64_set(&ct->large_ext_chk_cnt, ctm->large_ext_chk_cnt);
    init_rwsem(&ct->lock);
    mutex_init(&ct->lo_mutex);
    atomic64_set(&ct->node_count, ctm->node_count);
    for(i=0; i<MAX_BTREE_DEPTH; i++)
        ct->node_sizes[i] = ctm->node_sizes[i];
    castle_ext_freespace_unmarshall(&ct->internal_ext_free, &ctm->internal_ext_free_bs);
    castle_ext_freespace_unmarshall(&ct->tree_ext_free, &ctm->tree_ext_free_bs);
    castle_ext_freespace_unmarshall(&ct->data_ext_free, &ctm->data_ext_free_bs);
    castle_extent_mark_live(ct->internal_ext_free.ext_id);
    castle_extent_mark_live(ct->tree_ext_free.ext_id);
    castle_extent_mark_live(ct->data_ext_free.ext_id);
    ct->da_list.next = NULL;
    ct->da_list.prev = NULL;
    INIT_LIST_HEAD(&ct->large_objs);
    mutex_init(&ct->last_key_mutex);
    ct->last_key = NULL;
    ct->bloom_exists = ctm->bloom_exists;
    if (ctm->bloom_exists)
        castle_bloom_unmarshall(&ct->bloom, ctm);

    return ctm->da_id;
}

/**
 * Run fn() on each CT in the doubling array.
 *
 * @param da    Doubling array's CTs to enumerate
 * @param fn    Function to pass each of the da's CTs too
 * @param token @FIXME
 */
static void __castle_da_foreach_tree(struct castle_double_array *da,
                                     int (*fn)(struct castle_double_array *da,
                                               struct castle_component_tree *ct,
                                               int level_cnt,
                                               void *token), 
                                     void *token)
{
    struct castle_component_tree *ct;
    struct list_head *lh, *t;
    int i, j;

    for(i=0; i<MAX_DA_LEVEL; i++)
    {
        j = 0;
        list_for_each_safe(lh, t, &da->levels[i].trees)
        {
            ct = list_entry(lh, struct castle_component_tree, da_list); 
            if(fn(da, ct, j, token))
                return;
            j++;
        }
    }
}

static void castle_da_foreach_tree(struct castle_double_array *da,
                                   int (*fn)(struct castle_double_array *da,
                                             struct castle_component_tree *ct,
                                             int level_cnt,
                                             void *token), 
                                   void *token)
{
    write_lock(&da->lock);
    __castle_da_foreach_tree(da, fn, token);
    write_unlock(&da->lock);
}

static int castle_ct_hash_destroy_check(struct castle_component_tree *ct, void *ct_hash)
{
    struct list_head *lh, *t;
    int    err = 0;

    /* Only the global component tree should remain when we destroy DA hash. */ 
    if(((unsigned long)ct_hash > 0) && !TREE_GLOBAL(ct->seq))
    {
        printk("Error: Found CT=%d not on any DA's list, it claims DA=%d\n", 
            ct->seq, ct->da);
        err = -1;
    }

   /* All CTs apart of global are expected to be on a DA list. */
   if(!TREE_GLOBAL(ct->seq) && (ct->da_list.next == NULL))
   {
       printk("Error: CT=%d is not on DA list, for DA=%d\n", 
               ct->seq, ct->da);
       err = -2;
   }

   if(TREE_GLOBAL(ct->seq) && (ct->da_list.next != NULL))
   {
       printk("Error: Global CT=%d is on DA list, for DA=%d\n", 
               ct->seq, ct->da);
       err = -3;
   }

   /* Ref count should be 1 by now. */
   if(atomic_read(&ct->ref_count) != 1)
   {
       printk("Error: Bogus ref count=%d for ct=%d, da=%d when exiting.\n", 
               atomic_read(&ct->ref_count), ct->seq, ct->da);
       err = -4;
   }

   BUG_ON(err);

   /* Free large object structures. */
   list_for_each_safe(lh, t, &ct->large_objs)
   {
       struct castle_large_obj_entry *lo = 
                list_entry(lh, struct castle_large_obj_entry, list);
       list_del(lh);
       castle_free(lo);
   }
   
    return 0;
}

static int castle_da_ct_dealloc(struct castle_double_array *da,
                                struct castle_component_tree *ct,
                                int level_cnt,
                                void *unused)
{
    castle_ct_hash_destroy_check(ct, (void*)0UL);
    list_del(&ct->da_list);
    list_del(&ct->hash_list);
    if (ct->last_key)
        castle_object_okey_free(ct->last_key);
    castle_free(ct);

    return 0;
}

static int castle_da_hash_dealloc(struct castle_double_array *da, void *unused) 
{
    castle_sysfs_da_del(da);
    castle_da_foreach_tree(da, castle_da_ct_dealloc, NULL);
    list_del(&da->hash_list);
    castle_da_dealloc(da);

    return 0;
}

static void castle_da_hash_destroy(void)
{
    /* No need for the lock, end-of-day stuff. */
   __castle_da_hash_iterate(castle_da_hash_dealloc, NULL); 
   castle_free(castle_da_hash);
}

static void castle_ct_hash_destroy(void)
{
    castle_ct_hash_iterate(castle_ct_hash_destroy_check, (void *)1UL);
    castle_free(castle_ct_hash);
}

static int castle_da_tree_writeback(struct castle_double_array *da,
                                    struct castle_component_tree *ct,
                                    int level_cnt,
                                    void *unused)
{
    struct castle_clist_entry mstore_entry;
    struct list_head *lh, *tmp;

    /* For periodic checkpoints flush component trees onto disk. */
    if (!castle_da_exiting)
    {
        /* Always writeback Global tree structure but, don't writeback. */
        /* Note: Global Tree is not Crash-Consistent. */
        if (TREE_GLOBAL(ct->seq))
            goto mstore_writeback;

        /* Don't write back T0. */
        if (ct->level == 0)
            return 0;

        /* Don't write back trees with outstanding writes. */
        if (atomic_read(&ct->write_ref_count) != 0)
            return 0;

        /* Mark new trees for flush. */
        if (ct->new_ct)
        {
            /* Schedule flush of new CT onto disk. */
            castle_cache_extent_flush_schedule(ct->tree_ext_free.ext_id, 0,
                                               atomic64_read(&ct->tree_ext_free.used));
            castle_cache_extent_flush_schedule(ct->data_ext_free.ext_id, 0,
                                               atomic64_read(&ct->data_ext_free.used));
            ct->new_ct = 0;
        }
    }

mstore_writeback:
    if (da && !da->last_key)
        da->last_key = ct->last_key;

    /* Never writeback T0 in periodic checkpoints. */
    BUG_ON((ct->level == 0) && !castle_da_exiting);

    mutex_lock(&ct->lo_mutex);
    list_for_each_safe(lh, tmp, &ct->large_objs)
    {
        struct castle_large_obj_entry *lo = 
                            list_entry(lh, struct castle_large_obj_entry, list);

        castle_ct_large_obj_writeback(lo, ct);
    }
    mutex_unlock(&ct->lo_mutex);

    castle_da_ct_marshall(&mstore_entry, ct); 
    castle_mstore_entry_insert(castle_tree_store, &mstore_entry);

    return 0;
}

static int castle_da_hash_count(struct castle_double_array *da, void *_count)
{
    uint32_t *count = _count;

    (*count)++;
    return 0;
}

uint32_t castle_da_count(void)
{
    uint32_t count = 0;

    castle_da_hash_iterate(castle_da_hash_count, (void *)&count); 

    return count;
}

static int castle_da_writeback(struct castle_double_array *da, void *unused) 
{
    struct castle_dlist_entry mstore_dentry;

    castle_da_marshall(&mstore_dentry, da);

    /* We get here with hash spinlock held. But since we're calling sleeping functions
       we need to drop it. Hash consitancy is guaranteed, because by this point 
       noone should be modifying it anymore */
    read_unlock_irq(&castle_da_hash_lock);

    if (da->last_key)
        da->last_key = NULL;

    /* Writeback is happening under CASTLE_TRANSACTION LOCK, which guarentees no
     * addition/deletions to component tree list, no need of DA lock here. */
    __castle_da_foreach_tree(da, castle_da_tree_writeback, NULL);

    debug("Inserting a DA id=%d\n", da->id);
    castle_mstore_entry_insert(castle_da_store, &mstore_dentry);

    read_lock_irq(&castle_da_hash_lock);

    return 0;
}

void castle_double_arrays_writeback(void)
{
    BUG_ON(castle_da_store || castle_tree_store || castle_lo_store);

    castle_da_store   = castle_mstore_init(MSTORE_DOUBLE_ARRAYS,
                                         sizeof(struct castle_dlist_entry));
    castle_tree_store = castle_mstore_init(MSTORE_COMPONENT_TREES,
                                         sizeof(struct castle_clist_entry));
    castle_lo_store   = castle_mstore_init(MSTORE_LARGE_OBJECTS,
                                         sizeof(struct castle_lolist_entry));

    if(!castle_da_store || !castle_tree_store || !castle_lo_store)
        goto out;

    castle_da_hash_iterate(castle_da_writeback, NULL); 
    castle_da_tree_writeback(NULL, &castle_global_tree, -1, NULL);

out:
    if (castle_lo_store)    castle_mstore_fini(castle_lo_store);
    if (castle_tree_store)  castle_mstore_fini(castle_tree_store);
    if (castle_da_store)    castle_mstore_fini(castle_da_store);

    castle_da_store = castle_tree_store = castle_lo_store = NULL;
}

static int castle_da_rwct_make(struct castle_double_array *da, int cpu_index, int in_tran);

/**
 * Create T0 for specified DA if it does not already exist.
 *
 * - Allocate one CT per CPU handling requests
 *
 * When any of these CTs subsequently get exhausted a new CT is allocated and
 * the old CT promoted in an atomic fashion (da->lock held).  This means we are
 * guaranteed to have none or all of the CTs at level 0.
 *
 * @FIXME currently the system will panic if the filesystem is imported on a
 * machine with a different number of CPUs
 *
 * @FIXME is the locking here correct?  could these be read_locks()?
 *
 * @also castle_double_array_start()
 */
static int castle_da_rwct_create(struct castle_double_array *da)
{
    struct list_head *l, *p;
    LIST_HEAD(list);
    int cpu_index;

    write_lock(&da->lock);
    /* Early exit if we already have T0s. */
    if (!list_empty(&da->levels[0].trees))
    {
        BUG_ON(da->levels[0].nr_trees != request_cpus.cnt);
        write_unlock(&da->lock);
        return 0;
    }

    /* Otherwise, there should be no trees at this level. */
    BUG_ON(da->levels[0].nr_trees != 0);
    write_unlock(&da->lock); /* castle_da_rwct_make() gets da lock */

    /* There are no existing CTs at level 0 in this DA.
     * Create one CT per CPU handling requests. */
    for (cpu_index = 0; cpu_index < request_cpus.cnt; cpu_index++)
    {
        if (castle_da_rwct_make(da, cpu_index, 1 /* in_tran */) != EXIT_SUCCESS)
        {
            printk("Failed to create T0 %d for DA %u\n", cpu_index, da->id);
            goto err_out;
        }
    }

    printk("Created %d CTs for DA %u T0\n", cpu_index, da->id);

    return 0;

err_out:
    /* We couldn't create all T0s we need, free the ones we managed to alloc.
       Remove them frot the da list into our private list first. */
    write_lock(&da->lock);
    list_splice_init(&da->levels[0].trees, &list);
    write_unlock(&da->lock);

    /* Put them all. */
    list_for_each_safe(l, p, &list)
    {
        struct castle_component_tree *ct;
        list_del(l);
        /* Nullify the list head. Expected by castle_ct_put. */
        l->next = NULL;
        l->prev = NULL;
        /* Work out the CT, and put it. */
        ct = list_entry(l, struct castle_component_tree, da_list);
        castle_ct_put(ct, 0);
    }

    return -EINVAL;
}

/**
 * Called at start of day from the hash iterator. Tries to allocate RWCTs for a DA.
 * It ignores errors, and returns 0 in order to continue the iterator.
 */
static int castle_da_rwct_init(struct castle_double_array *da, void *unused)
{
    castle_da_rwct_create(da);

    return 0;
}

static int __castle_da_driver_merge_reset(struct castle_double_array *da, void *unused)
{
    write_lock(&da->lock);
    castle_da_driver_merge_reset(da);
    write_unlock(&da->lock);

    return 0;
}

/**
 * Start existing doubling arrays.
 *
 * - Called during module initialisation only
 *
 * @also castle_fs_init()
 */
int castle_double_array_start(void)
{
    /* Create T0 for all DAs that don't have them (function acquires lock). */
    __castle_da_hash_iterate(castle_da_rwct_init, NULL);

    /* Reset driver merge for all DAs. */
    castle_da_hash_iterate(__castle_da_driver_merge_reset, NULL);

    /* Check all DAs to see whether any merges need to be done. */
    castle_da_hash_iterate(castle_da_merge_restart, NULL); 

    return 0;
}

int castle_double_array_read(void)
{
    struct castle_dlist_entry mstore_dentry;
    struct castle_clist_entry mstore_centry;
    struct castle_lolist_entry mstore_loentry;
    struct castle_mstore_iter *iterator = NULL;
    struct castle_component_tree *ct;
    struct castle_double_array *da;
    c_mstore_key_t key;
    da_id_t da_id;
    int ret = 0;

    castle_da_store   = castle_mstore_open(MSTORE_DOUBLE_ARRAYS,
                                         sizeof(struct castle_dlist_entry));
    castle_tree_store = castle_mstore_open(MSTORE_COMPONENT_TREES,
                                         sizeof(struct castle_clist_entry));
    castle_lo_store   = castle_mstore_open(MSTORE_LARGE_OBJECTS,
                                         sizeof(struct castle_lolist_entry));

    if(!castle_da_store || !castle_tree_store || !castle_lo_store)
        goto error_out;
    
    /* Read doubling arrays */
    iterator = castle_mstore_iterate(castle_da_store);
    if(!iterator)
        goto error_out;

    while(castle_mstore_iterator_has_next(iterator))
    {
        castle_mstore_iterator_next(iterator, &mstore_dentry, &key);
        da = castle_da_alloc(mstore_dentry.id); 
        if(!da) 
            goto error_out;
        castle_da_unmarshall(da, &mstore_dentry);
        castle_da_hash_add(da);
        debug("Read DA id=%d\n", da->id);
        castle_next_da_id = (da->id >= castle_next_da_id) ? da->id + 1 : castle_next_da_id;
    }
    castle_mstore_iterator_destroy(iterator);

    /* Read component trees */
    iterator = castle_mstore_iterate(castle_tree_store);
    if(!iterator)
        goto error_out;
   
    while(castle_mstore_iterator_has_next(iterator))
    {
        castle_mstore_iterator_next(iterator, &mstore_centry, &key);
        /* Special case for castle_global_tree, it doesn't have a da associated with it. */
        if(TREE_GLOBAL(mstore_centry.seq))
        {
            da_id = castle_da_ct_unmarshall(&castle_global_tree, &mstore_centry);
            BUG_ON(!DA_INVAL(da_id));
            castle_ct_hash_add(&castle_global_tree);
            continue;
        }
        /* Otherwise allocate a ct structure */
        ct = castle_malloc(sizeof(struct castle_component_tree), GFP_KERNEL);
        if(!ct)
            goto error_out;
        da_id = castle_da_ct_unmarshall(ct, &mstore_centry);
        castle_ct_hash_add(ct);
        da = castle_da_hash_get(da_id);
        if(!da)
            goto error_out;
        debug("Read CT seq=%d\n", ct->seq);
        write_lock(&da->lock);
        castle_component_tree_add(da, ct, NULL /*head*/, 1 /*in init*/);
        write_unlock(&da->lock);
        castle_next_tree_seq = (ct->seq >= castle_next_tree_seq) ? ct->seq + 1 : castle_next_tree_seq;
    }
    castle_mstore_iterator_destroy(iterator);
    iterator = NULL;
    debug("castle_next_da_id = %d, castle_next_tree_id=%d\n", 
            castle_next_da_id, 
            castle_next_tree_seq);

    /* Read all Large Objects lists. */
    iterator = castle_mstore_iterate(castle_lo_store);
    if(!iterator)
        goto error_out;

    while(castle_mstore_iterator_has_next(iterator))
    {
        struct castle_component_tree *ct;

        castle_mstore_iterator_next(iterator, &mstore_loentry, &key);
        ct = castle_component_tree_get(mstore_loentry.ct_seq);
        if (!ct)
        {
            printk("Found zombi Large Object(%llu, %u)\n",
                    mstore_loentry.ext_id, mstore_loentry.ct_seq);
            BUG();
        }
        if (castle_ct_large_obj_add(mstore_loentry.ext_id,
                                    mstore_loentry.length,
                                    &ct->large_objs, NULL))
        {
            printk("Failed to add Large Object %llu to CT: %u\n", 
                    mstore_loentry.ext_id,
                    mstore_loentry.ct_seq);
            goto error_out;
        }
        castle_extent_mark_live(mstore_loentry.ext_id);
    }
    castle_mstore_iterator_destroy(iterator);
    iterator = NULL;

    /* Sort all the tree lists by the sequence number */
    castle_da_hash_iterate(castle_da_trees_sort, NULL); 
    castle_da_hash_iterate(castle_da_merge_start, NULL); 

    goto out;

error_out:
    /* The doubling arrays we've created so far should be destroyed by the module fini code. */
    ret = -EINVAL;
out:
    if (iterator)           castle_mstore_iterator_destroy(iterator);
    if (castle_da_store)    castle_mstore_fini(castle_da_store);
    if (castle_tree_store)  castle_mstore_fini(castle_tree_store);
    if (castle_lo_store)    castle_mstore_fini(castle_lo_store);
    castle_da_store = castle_tree_store = castle_lo_store = NULL;
    
    return ret;
}

/**
 * Allocate and initialise a CT.
 *
 * - Does not allocate extents
 *
 * @return NULL (CT could not be allocated) or pointer to new CT
 */
static struct castle_component_tree* castle_ct_alloc(struct castle_double_array *da, 
                                                     btree_t type,  
                                                     int level)
{
    struct castle_component_tree *ct;

    BUG_ON((type != RO_VLBA_TREE_TYPE) && (type != RW_VLBA_TREE_TYPE));
    ct = castle_zalloc(sizeof(struct castle_component_tree), GFP_KERNEL); 
    if(!ct) 
        return NULL;
    
    /* Allocate an id for the tree, init the ct. */
    ct->seq             = castle_next_tree_seq++;
    atomic_set(&ct->ref_count, 1);
    atomic_set(&ct->write_ref_count, 0);
    atomic64_set(&ct->item_count, 0); 
    atomic64_set(&ct->large_ext_chk_cnt, 0);
    ct->btree_type      = type; 
    ct->dynamic         = type == RW_VLBA_TREE_TYPE ? 1 : 0;
    ct->da              = da->id;
    ct->level           = level;
    ct->tree_depth      = -1;
    ct->root_node       = INVAL_EXT_POS;
    ct->first_node      = INVAL_EXT_POS;
    ct->first_node_size = -1;
    ct->last_node       = INVAL_EXT_POS;
    ct->last_node_size  = -1;
    ct->new_ct          = 1;
    ct->compacting      = 0;
    init_rwsem(&ct->lock);
    mutex_init(&ct->lo_mutex);
    atomic64_set(&ct->node_count, 0);
    ct->da_list.next = NULL;
    ct->da_list.prev = NULL;
    INIT_LIST_HEAD(&ct->hash_list);
    INIT_LIST_HEAD(&ct->large_objs);
    castle_ct_hash_add(ct);
    ct->internal_ext_free.ext_id = INVAL_EXT_ID;
    ct->tree_ext_free.ext_id     = INVAL_EXT_ID;
    ct->data_ext_free.ext_id     = INVAL_EXT_ID;
    ct->last_key        = NULL;
    ct->bloom_exists    = 0;
    mutex_init(&ct->last_key_mutex);
#ifdef CASTLE_PERF_DEBUG
    ct->bt_c2bsync_ns   = 0;
    ct->data_c2bsync_ns = 0;
    ct->get_c2b_ns      = 0;
#endif

    return ct;
}

/**
 * Allocate and initialise a T0 component tree.
 *
 * @param da        DA to create new T0 for
 * @param cpu_index Offset within list to insert newly allocated CT
 * @param in_tran   Set if the caller is already within CASTLE_TRANSACTION
 *
 * Holds the DA growing lock while:
 *
 * - Allocating a new CT
 * - Allocating data and btree extents
 * - Initialises root btree node
 * - Places allocated CT/extents onto DA list of level 0 CTs
 * - Restarts merges as necessary
 *
 * @also castle_ct_alloc()
 * @also castle_ext_fs_init()
 */
static int castle_da_rwct_make(struct castle_double_array *da, int cpu_index, int in_tran)
{
    struct castle_component_tree *ct, *old_ct;
    struct castle_btree_type *btree;
    struct list_head *l = NULL;
    c2_block_t *c2b;
    int ret;
    static int t0_count = 0;

    /* Only allow one rwct_make() at any point in time. If we fail to acquire the bit lock
       wait for whoever is doing it, to create the RWCT.
       TODO: use bit wait instead of msleep here. */ 
    if(castle_da_growing_rw_test_and_set(da))
    {
        debug("Racing RWCT make on da=%d\n", da->id);
        while(castle_da_growing_rw_test(da))
            msleep(1);
        return -EAGAIN; 
    }

    /* We've acquired the 'growing' lock. Proceed. */
    ret = -ENOMEM;
    ct = castle_ct_alloc(da, RW_VLBA_TREE_TYPE, 0 /* level */);
    if(!ct)
        goto out;

    btree = castle_btree_type_get(ct->btree_type);

    /* RWCTs are present only at levels 0,1 in the DA.
     * Prefix these CTs with cpu_index to preserve operation ordering when
     * inserting into the DA trees list at RWCT levels. */
    BUG_ON(sizeof(ct->seq) != 4);
    ct->seq = (cpu_index << TREE_SEQ_SHIFT) + ct->seq;

    /* Allocate data and btree extents. */
    if ((ret = castle_new_ext_freespace_init(&ct->tree_ext_free,
                                              da->id,
                                              MAX_DYNAMIC_TREE_SIZE * C_CHK_SIZE,
                                              btree->node_size(ct, 0) * C_BLK_SIZE)))
    {
        printk("Failed to get space for T0 tree\n");
        goto no_space;
    }

    if ((ret = castle_new_ext_freespace_init(&ct->data_ext_free,
                                              da->id,
                                              MAX_DYNAMIC_DATA_SIZE * C_CHK_SIZE,
                                              C_BLK_SIZE)))
    {
        printk("Failed to get space for T0 data\n");
        goto no_space;
    }

    /* Create a root node for this tree, and update the root version */
    c2b = castle_btree_node_create(ct,
                                   0 /* version */,
                                   0 /* level */,
                                   0 /* wasn't preallocated */);
    castle_btree_node_save_prepare(ct, c2b->cep, c2b->nr_pages);
    ct->root_node = c2b->cep;
    ct->tree_depth = 1;
    write_unlock_c2b(c2b);
    put_c2b(c2b);

    if (!in_tran) CASTLE_TRANSACTION_BEGIN;
    write_lock(&da->lock);

    /* Find cpu_index^th element from back and promote to level 1. */
    // @FIXME multi-t0-rwct-cpu-count-mismatch
    if (cpu_index < da->levels[0].nr_trees)
    {
        int index = 0;
        list_for_each_prev(l, &da->levels[0].trees)
        {
            if (index++ == cpu_index)
            {
                /* Found cpu_index^th element. */
                old_ct = list_entry(l, struct castle_component_tree, da_list);
                l = old_ct->da_list.prev; /* Position to insert new CT. */
                castle_component_tree_del(da, old_ct);
                old_ct->level = 1;
                castle_component_tree_add(da, old_ct, NULL /* append */, 0 /* not in init */);
                /* Recompute merge driver. */
                castle_da_driver_merge_reset(da);
                break;
            }
        }
    }
    /* Insert new CT onto list.  l will be the previous element (from delete above) or NULL. */
    castle_component_tree_add(da, ct, l, 0 /* not in init */);

    debug("Added component tree seq=%d, root_node="cep_fmt_str
          ", it's threaded onto da=%p, level=%d\n",
            ct->seq, cep2str(c2b->cep), da, ct->level);

    FAULT(MERGE_FAULT);

    if (!in_tran) CASTLE_TRANSACTION_END;

    printk("Created T0: %d\n", ++t0_count);
    /* DA is attached, therefore we must be holding a ref, therefore it is safe to schedule
       the merge check. */
    write_unlock(&da->lock);
    castle_da_merge_restart(da, NULL);
    ret = 0;
    goto out;

no_space:
    castle_da_frozen_set(da);
    if (ct)
        castle_ct_put(ct, 0);
out:
    castle_da_growing_rw_clear(da);
    return ret;
}

/**
 * Allocate a new doubling array.
 *
 * - Called when userland creates a new doubling array
 *
 * @param da_id         id of doubling array (unique)
 * @param root_version  Root version
 *
 * @also castle_control_create()
 */
int castle_double_array_make(da_id_t da_id, version_t root_version)
{
    struct castle_double_array *da;
    int ret;

    debug("Creating doubling array for da_id=%d, version=%d\n", da_id, root_version);
    da = castle_da_alloc(da_id);
    if(!da)
        return -ENOMEM;
    /* Write out the id, and the root version. */
    da->id = da_id;
    da->root_version = root_version;
    /* Allocate T0s. */
    ret = castle_da_rwct_create(da);
    if (ret != EXIT_SUCCESS)
    {
        printk("Exiting from failed ct create.\n");
        castle_da_dealloc(da);
        
        return ret;
    }
    debug("Successfully made a new doubling array, id=%d, for version=%d\n",
        da_id, root_version);
    castle_da_hash_add(da);
    castle_sysfs_da_add(da);
    /* DA make succeeded, start merge threads. */
    castle_da_merge_start(da, NULL);

    return 0;
}

/**
 * Return CT that logically follows passed ct, from the next level, if necessary.
 *
 * @param ct    Current CT to use as basis for finding next CT
 *
 * - Advance to the next level if the current CT has been removed from the DA or
 *   if the current CT is from level 0 (keys are hashed to specific CTs at level
 *   0 so there's no point searching other CTs)
 * - Keep going up the levels until a CT is found (or none)
 *
 * @return  Next CT with a reference held
 * @return  NULL if no more trees
 */
struct castle_component_tree* castle_da_ct_next(struct castle_component_tree *ct)
{
    struct castle_double_array *da = castle_da_hash_get(ct->da);
    struct castle_component_tree *next_ct;
    struct list_head *ct_list;
    uint8_t level;

    debug_verbose("Asked for component tree after %d\n", ct->seq);
    BUG_ON(!da);
    read_lock(&da->lock);
    /* Start from the current list, from wherever the current ct is in the da_list. */
    level = ct->level;
    ct_list = &ct->da_list;

    /* Advance to the next level of the DA if:
     *
     * - Current CT is level 0: each CT at level 0 handles inserts for a
     *   specific hash of keys.  The only CT at this level that could contain a
     *   hit is the one the key hashed to (i.e. the current CT).
     * - Current CT was removed from the DA (da_list is NULL): we can safely
     *   move to the next level as merges always remove the two oldest trees.
     *   Any other trees in the current CT's level will be newer and therefore
     *   predate a lookup. */
    if (level == 0 || ct_list->next == NULL)
    {
        BUG_ON(ct_list->next == NULL && ct_list->prev != NULL);

        level++;
        ct_list = &da->levels[level].trees;
    }

    /* Loop through all levels trying to find the next tree. */
    while (level < MAX_DA_LEVEL)
    {
        if (!list_is_last(ct_list, &da->levels[level].trees))
        {
            /* CT found at (level), return it. */
            next_ct = list_entry(ct_list->next, struct castle_component_tree, da_list); 
            debug_verbose("Found component tree %d\n", next_ct->seq);
            castle_ct_get(next_ct, 0);
            read_unlock(&da->lock);

            return next_ct;
        }

        /* No CT found at (level), advance to the next level. */
        level++;
        ct_list = &da->levels[level].trees;
    }     
    read_unlock(&da->lock);

    return NULL;
}

/**
 * Return cpu_index^th T0 CT for da.
 *
 * - Does not take a reference
 *
 * NOTE: Caller must hold da read-lock.
 *
 * @return  cpu_index^th element from back of da->levels[0].trees list
 */
static struct castle_component_tree* __castle_da_rwct_get(struct castle_double_array *da, 
                                                          int cpu_index)
{
    struct list_head *l;

    BUG_ON(cpu_index >= da->levels[0].nr_trees);
    list_for_each_prev(l, &da->levels[0].trees)
    {
        if (cpu_index == 0)
            /* Found cpu_index^th element. */
            return list_entry(l, struct castle_component_tree, da_list);
        else
            cpu_index--;
    }
    BUG_ON(cpu_index < 0);

    return NULL;
}

/**
 * Return cpu_index^th T0 CT for da with a reference held.
 *
 * @also __castle_da_rwct_get()
 */
static struct castle_component_tree* castle_da_rwct_get(struct castle_double_array *da,
                                                        int cpu_index)
{
    struct castle_component_tree *ct = NULL;

    read_lock(&da->lock);
    ct = __castle_da_rwct_get(da, cpu_index);
    BUG_ON(!ct);
    castle_ct_get(ct, 1 /*write*/);
    read_unlock(&da->lock);

    return ct;
}

/**
 * Get first CT from DA that satisfies bvec.
 *
 * - Check if we have an appropriate CT at level 0 (specifically one that
 *   matches the bvec's cpu_index)
 * - Iterate over all levels of the DA until we find the first CT
 * - Return the first CT we find
 *
 * @return  The youngest CT that satisfies bvec
 */
static struct castle_component_tree* castle_da_first_ct_get(struct castle_double_array *da,
                                                            c_bvec_t *c_bvec)
{
    struct castle_component_tree *ct = NULL;
    struct list_head *l;
    int level = 1;

    read_lock(&da->lock);

    /* Level 0 is handled as a special case due to its ordering constraints. */
    ct = __castle_da_rwct_get(da, c_bvec->cpu_index);
    if (ct)
        goto out;

    /* Find the first level with trees and return it. */
    while (level < MAX_DA_LEVEL)
    {
        l = &da->levels[level].trees;
        if (!list_empty(l))
        {
            ct = list_first_entry(l, struct castle_component_tree, da_list);
            goto out;
        }
        level++;
    }

out:
    if (ct)
        castle_ct_get(ct, 0 /*write*/);
    read_unlock(&da->lock);

    return ct;
}

/**
 * Get T0 CT from da and preallocate space for writing.
 *
 * - Get CT for c_bvec->cpu_index
 * - Preallocate space in CT for writes
 *   - Promote and get fresh CT if it cannot satisfy preallocation
 *
 * @return Write case:  T0 CT preallocated for write
 *
 * @also castle_da_rwct_get()
 */
static struct castle_component_tree* castle_da_rwct_acquire(struct castle_double_array *da,
                                                            c_bvec_t *c_bvec)
{
    uint64_t value_len, req_btree_space, req_medium_space;
    struct castle_component_tree *ct;
    struct castle_btree_type *btree;
    int nr_nodes, ret;

    BUG_ON(c_bvec_data_dir(c_bvec) != WRITE);

again:
    if (castle_da_frozen(da))
        return NULL;

    ct = castle_da_rwct_get(da, c_bvec->cpu_index);
    // @FIXME some sort of error handling here if we can't allocate a new RWCT

    /* Attempt to preallocate space in the btree and m-obj extents for writes. */
    btree = castle_btree_type_get(ct->btree_type);
    /* Allocate worst case number of nodes we may need to create for this write. */
    nr_nodes = (2 * ct->tree_depth + 3);
    req_btree_space = nr_nodes * btree->node_size(ct, 0) * C_BLK_SIZE;
    if (castle_ext_freespace_prealloc(&ct->tree_ext_free, req_btree_space) < 0)
        goto new_ct;
    /* Save how many nodes we've pre-allocated. */
    atomic_set(&c_bvec->reserv_nodes, nr_nodes);

    /* Pre-allocate space for Medium objects. */
    value_len = c_bvec->c_bio->replace->value_len;
    /* If not a medium object, we are done. */
    if ((value_len <= MAX_INLINE_VAL_SIZE) || (value_len > MEDIUM_OBJECT_LIMIT))
        return ct;

    /* Preallocate (ceil to C_BLK_SIZE) space for the medium object. */
    req_medium_space = ((value_len - 1) / C_BLK_SIZE + 1) * C_BLK_SIZE;
    if (castle_ext_freespace_prealloc(&ct->data_ext_free, req_medium_space) >= 0)
        return ct;

    /* We failed to preallocate space for the medium object. Free the space in btree extent. */
    castle_ext_freespace_free(&ct->tree_ext_free, req_btree_space);

new_ct:
    debug("Number of items in component tree %d, # items %ld. Trying to add a new rwct.\n",
            ct->seq, atomic64_read(&ct->item_count));
    ret = castle_da_rwct_make(da, c_bvec->cpu_index, 0 /* in_tran */);

    /* Drop reference for old CT. */
    castle_ct_put(ct, 1 /*write*/);
    if((ret == 0) || (ret == -EAGAIN))
        goto again;
  
    printk("Warning: failed to create RWCT with errno=%d\n", ret);
    return NULL;
}

/**
 * Queue a write IO for later submission.
 *
 * @param da        Doubling array to queue IO for
 * @param c_bvec    IO to queue
 *
 * WARNING: Caller must hold c_bvec's wait queue lock.
 */
static void castle_da_bvec_queue(struct castle_double_array *da, c_bvec_t *c_bvec)
{
    struct castle_da_io_wait_queue *wq = &da->ios_waiting[c_bvec->cpu_index];

    BUG_ON(!spin_is_locked(&wq->lock));

    /* Queue the bvec. */
    list_add_tail(&c_bvec->io_list, &wq->list);

    /* Increment IO waiting counters. */
    wq->cnt++;
    atomic_inc(&da->ios_waiting_cnt);
}

/**
 * Submit write IOs queued on wait queue to btree.
 *
 * @param work  Embedded in struct castle_da_io_wait_queue
 *
 * - Remove pending IOs from the wait queue so long as ios_budget is positive
 * - Place pending IOs on a new list of IOs to be submitted to the appropriate
 *   btree
 * - We use an intermediate list to minimise the amount of time we hold the
 *   wait queue lock (although subsequent IOs should be hitting the same CPU)
 *
 * @also struct castle_da_io_wait_queue
 * @also castle_da_ios_budget_replenish()
 * @also castle_da_write_bvec_start()
 */
static void castle_da_queue_kick(struct work_struct *work)
{
    struct castle_da_io_wait_queue *wq = container_of(work, struct castle_da_io_wait_queue, work);
    struct list_head *l, *t;
    LIST_HEAD(submit_list);
    c_bvec_t *c_bvec;

    /* Get as many c_bvecs as we can and place them on the submit list. */
    spin_lock(&wq->lock);
    while ((atomic_dec_return(&wq->da->ios_budget) >= 0) && !list_empty(&wq->list))
    {
        /* New IOs are queued at the end of the list.  Always pull from the
         * front of the list to preserve ordering. */
        c_bvec = list_first_entry(&wq->list, c_bvec_t, io_list);
        list_del(&c_bvec->io_list);
        list_add(&c_bvec->io_list, &submit_list);

        /* Decrement IO waiting counters. */
        BUG_ON(--wq->cnt < 0);
        BUG_ON(atomic_dec_return(&wq->da->ios_waiting_cnt) < 0);
    }
    spin_unlock(&wq->lock);

    /* Submit c_bvecs to the btree. */
    list_for_each_safe(l, t, &submit_list)
    {
        c_bvec = list_entry(l, c_bvec_t, io_list);
        list_del(&c_bvec->io_list);
        castle_da_write_bvec_start(wq->da, c_bvec);
    }
}

static void castle_da_ct_walk_complete(c_bvec_t *c_bvec, int err, c_val_tup_t cvt)
{
    void (*callback) (struct castle_bio_vec *c_bvec, int err, c_val_tup_t cvt);
    struct castle_component_tree *ct, *next_ct;
    struct castle_double_array *da;
    int read; 
    
    callback = c_bvec->da_endfind;
    ct = c_bvec->tree;
    da = castle_da_hash_get(ct->da);

    read = (c_bvec_data_dir(c_bvec) == READ);
    BUG_ON(read && atomic_read(&c_bvec->reserv_nodes));
    /* For reads, if the key hasn't been found, check in the next tree. */
    if(read && CVT_INVALID(cvt) && (!err))
    {
#ifdef CASTLE_BLOOM_FP_STATS
        if (ct->bloom_exists && c_bvec->bloom_positive)
        {
            atomic64_inc(&ct->bloom.false_positives);
            c_bvec->bloom_positive = 0;
        }
#endif
        debug_verbose("Checking next ct.\n");
        next_ct = castle_da_ct_next(ct);
        /* We've finished looking through all the trees. */
        if(!next_ct)
        {
            callback(c_bvec, err, INVAL_VAL_TUP); 
            return;
        }
        /* Put the previous tree, now that we know we've got a ref to the next. */
        castle_ct_put(ct, 0);
        c_bvec->tree = next_ct;
        debug_verbose("Scheduling btree read in %s tree: %d.\n", 
                ct->dynamic ? "dynamic" : "static", ct->seq);
        castle_bloom_submit(c_bvec);
        return;
    }
    castle_request_timeline_checkpoint_stop(c_bvec->timeline);
    castle_request_timeline_destroy(c_bvec->timeline);
    debug_verbose("Finished with DA, calling back.\n");
    castle_da_merge_budget_io_end(castle_da_hash_get(ct->da));
    /* Release the preallocated space in the btree extent. */
    if (atomic_read(&c_bvec->reserv_nodes))
    {
        struct castle_btree_type *btree = castle_btree_type_get(ct->btree_type);

        castle_ext_freespace_free(&ct->tree_ext_free, 
                                   atomic_read(&c_bvec->reserv_nodes) * 
                                   btree->node_size(ct, 0) * 
                                   C_BLK_SIZE);
    }
    BUG_ON(CVT_MEDIUM_OBJECT(cvt) && (cvt.cep.ext_id != c_bvec->tree->data_ext_free.ext_id));

    /* Don't release the ct reference in order to hold on to medium objects array, etc. */
    callback(c_bvec, err, cvt);
}

/**
 * Hand-off write request (bvec) to DA.
 *
 * - Get T0 CT for bvec
 * - Configure endfind handlers
 * - Submit immediately to btree
 *
 * @also castle_da_read_bvec_start()
 * @also castle_btree_submit()
 */
static void castle_da_write_bvec_start(struct castle_double_array *da, c_bvec_t *c_bvec)
{ 
    debug_verbose("Doing DA write for da_id=%d\n", da_id);
    BUG_ON(c_bvec_data_dir(c_bvec) != WRITE);

    /* Get a reference to the current RW CT (a new one may be created). */
    c_bvec->tree = castle_da_rwct_acquire(da, c_bvec);
    if (!c_bvec->tree)
    {
        c_bvec->endfind(c_bvec, -ENOSPC, INVAL_VAL_TUP);
        return;
    }

    c_bvec->da_endfind = c_bvec->endfind;
    c_bvec->endfind    = castle_da_ct_walk_complete;

    //castle_request_timeline_create(c_bvec->timeline);
    castle_request_timeline_checkpoint_start(c_bvec->timeline);
    debug_verbose("Looking up in ct=%d\n", c_bvec->tree->seq); 

    /* Submit directly to btree. */
    castle_btree_submit(c_bvec);
}

/**
 * Hand-off read request (bvec) to DA via bloom filter.
 *
 * - Get first CT for bvec (not necessarily a RWCT)
 * - Configure endfind handlers
 * - Pass off to the bloom layer
 *
 * @also castle_da_bvec_write_start()
 * @also castle_bloom_submit()
 */
static void castle_da_read_bvec_start(struct castle_double_array *da, c_bvec_t *c_bvec)
{
    debug_verbose("Doing DA read for da_id=%d\n", da_id);
    BUG_ON(c_bvec_data_dir(c_bvec) != READ);

    /* Get a reference to the first appropriate CT for this bvec. */
    c_bvec->tree = castle_da_first_ct_get(da, c_bvec);
    if (!c_bvec->tree)
    {
        c_bvec->endfind(c_bvec, -EINVAL, INVAL_VAL_TUP);
        return;
    }

    c_bvec->da_endfind = c_bvec->endfind;
    c_bvec->endfind    = castle_da_ct_walk_complete;

    //castle_request_timeline_create(c_bvec->timeline);
    castle_request_timeline_checkpoint_start(c_bvec->timeline);
    debug_verbose("Looking up in ct=%d\n", c_bvec->tree->seq);

    /* Submit via bloom filter. */
#ifdef CASTLE_BLOOM_FP_STATS
    c_bvec->bloom_positive = 0;
#endif
    castle_bloom_submit(c_bvec);
}

/**
 * Submit request to DA, queueing write IOs that are not within the DA ios_budget.
 *
 * Read requests:
 * - Processed immediately
 *
 * Write requests:
 * - Hold appropriate write queue spinlock to guarantee ordering
 * - If we're within ios_budget and there write queue is empty, queue the write
 *   IO immediately
 * - Otherwise queue write IO and wait for the ios_budget to be replenished
 *
 * @also castle_da_bvec_queue()
 * @also castle_da_read_bvec_start()
 * @also castle_da_write_bvec_start()
 */
void castle_double_array_submit(c_bvec_t *c_bvec)
{
    struct castle_attachment *att = c_bvec->c_bio->attachment;
    struct castle_da_io_wait_queue *wq;
    struct castle_double_array *da;
    da_id_t da_id; 

    down_read(&att->lock);
    /* Since the version is attached, it must be found */
    BUG_ON(castle_version_read(att->version, &da_id, NULL, NULL, NULL));
    up_read(&att->lock);

    da = castle_da_hash_get(da_id);
    BUG_ON(!da);
    /* da_endfind should be null it is for our privte use */
    BUG_ON(c_bvec->da_endfind);
    BUG_ON(atomic_read(&c_bvec->reserv_nodes) != 0);

    /* Start the read bvecs without any queueing. */
    if(c_bvec_data_dir(c_bvec) == READ)
    {
        castle_da_read_bvec_start(da, c_bvec);
        return;
    }

    /* If the DA is frozen the best we can do is return an error. */
    if (castle_da_frozen(da))
    {
        c_bvec->endfind(c_bvec, -ENOSPC, INVAL_VAL_TUP);
        return;
    }

    /* Write requests must operate within the ios_budget but reads can be
     * scheduled immediately. */
    wq = &da->ios_waiting[c_bvec->cpu_index];

    spin_lock(&wq->lock);
    if ((atomic_dec_return(&da->ios_budget) >= 0) && list_empty(&wq->list))
    {
        /* We're within the budget and there are no other IOs on the queue,
         * schedule this write IO immediately. */
        spin_unlock(&wq->lock);
        castle_da_write_bvec_start(da, c_bvec);
    }
    else
    {
        /* Either the budget is exhausted or there are other IOs pending on
         * the write queue.  Queue this write IO.
         *
         * Don't do a manual queue kick as if/when ios_budget is replenished
         * kicks for all of the DA's write queues will be scheduled.  The
         * kick for 'our' write queue will block on the spinlock we hold.
         *
         * ios_budget will be replenished; save an atomic op and leave it
         * in a negative state. */
        castle_da_bvec_queue(da, c_bvec);
        spin_unlock(&wq->lock);
    }
}

/**************************************/
/* Double Array Management functions. */

int castle_double_array_create(void)
{
    /* Make sure that the global tree is in the ct hash */
    castle_ct_hash_add(&castle_global_tree);

    return 0;
}

int castle_double_array_init(void)
{
    int ret, cpu, i, j;

    ret = -ENOMEM;

    for (i = 0; i < NR_CASTLE_DA_WQS; i++)
    {
        castle_da_wqs[i] = create_workqueue(castle_da_wqs_names[i]);
        if (!castle_da_wqs[i])
        {
            printk(KERN_ALERT "Error: Could not alloc wq\n");
            goto err0;
        }
    }

    /* Populate request_cpus with CPU ids ready to handle requests. */
    request_cpus.cpus = castle_malloc(sizeof(int) * num_online_cpus(), GFP_KERNEL);
    if (!request_cpus.cpus)
        goto err0;
    request_cpus.cnt = 0;
    for_each_online_cpu(cpu)
    {
        request_cpus.cpus[request_cpus.cnt] = cpu;
        request_cpus.cnt++;
    }

    castle_da_hash = castle_da_hash_alloc();
    if(!castle_da_hash)
        goto err1;
    castle_ct_hash = castle_ct_hash_alloc();
    if(!castle_ct_hash)
        goto err2;

    castle_da_hash_init();
    castle_ct_hash_init();
    /* Start up the timer which replenishes merge and write IOs budget */
    castle_throttle_timer_fire(1); 

    return 0;
 
err2:
    castle_free(castle_da_hash);
err1:
    castle_free(request_cpus.cpus);
err0:
    for (j = 0; j < i; j++)
        destroy_workqueue(castle_da_wqs[j]);
    BUG_ON(!ret);
    return ret;
}

void castle_double_array_merges_fini(void)
{
    int deleted_das;

    castle_da_exiting = 1;
    del_singleshot_timer_sync(&throttle_timer);
    /* This is happening at the end of execution. No need for the hash lock. */
    __castle_da_hash_iterate(castle_da_merge_stop, NULL);
    /* Also, wait for merges on deleted DAs. Merges will hold the last references to those DAs. */
    do {
        CASTLE_TRANSACTION_BEGIN;
        deleted_das = !list_empty(&castle_deleted_das);
        CASTLE_TRANSACTION_END;
        if(deleted_das)
            msleep(10);
    } while(deleted_das);
}

void castle_double_array_fini(void)
{
    int i;
    castle_da_hash_destroy();
    castle_ct_hash_destroy();

    castle_free(request_cpus.cpus);

    for (i = 0; i < NR_CASTLE_DA_WQS; i++)
        destroy_workqueue(castle_da_wqs[i]);
}

void castle_da_destroy_complete(struct castle_double_array *da)
{ /* Called with lock held. */
    int i;

    /* Sanity Checks. */
    BUG_ON(!castle_da_deleted(da));

    printk("Cleaning DA: %u\n", da->id);

    /* Destroy Component Trees. */
    for(i=0; i<MAX_DA_LEVEL; i++)
    {
        struct list_head *l, *lt;

        list_for_each_safe(l, lt, &da->levels[i].trees)
        {
            struct castle_component_tree *ct;
 
            ct = list_entry(l, struct castle_component_tree, da_list);
            /* No out-standing merges and active attachments. Componenet Tree
             * shouldn't be referenced any-where. */
            BUG_ON(atomic_read(&ct->ref_count) != 1);
            BUG_ON(atomic_read(&ct->write_ref_count));
            list_del(&ct->da_list);
            ct->da_list.next = ct->da_list.prev = NULL;
            castle_ct_put(ct, 0);
        }
    }

    /* Destroy Version and Rebuild Version Tree. */
    castle_version_tree_delete(da->root_version);

    /* Delete the DA from the list of deleted DAs. */
    list_del(&da->hash_list);

    /* Dealloc the DA. */
    castle_da_dealloc(da);
}

static void castle_da_get(struct castle_double_array *da)
{
    /* Increment ref count, it should never be zero when we get here. */
    BUG_ON(atomic_inc_return(&da->ref_cnt) <= 1);
}

static void castle_da_put(struct castle_double_array *da)
{
    if(atomic_dec_return(&da->ref_cnt) == 0)
    {
        /* Ref count dropped to zero -> delete. There should be no outstanding attachments. */
        BUG_ON(da->attachment_cnt != 0);
        BUG_ON(!castle_da_deleted(da));
        CASTLE_TRANSACTION_BEGIN;
        castle_da_destroy_complete(da);
        CASTLE_TRANSACTION_END;
    }
}

static void castle_da_put_locked(struct castle_double_array *da)
{
    BUG_ON(!CASTLE_IN_TRANSACTION);
    if(atomic_dec_return(&da->ref_cnt) == 0)
    {
        /* Ref count dropped to zero -> delete. There should be no outstanding attachments. */
        BUG_ON(da->attachment_cnt != 0);
        BUG_ON((da->hash_list.next != NULL) || (da->hash_list.prev != NULL));
        BUG_ON(!castle_da_deleted(da));
        castle_da_destroy_complete(da);
    }
}

static struct castle_double_array* castle_da_ref_get(da_id_t da_id)
{
    struct castle_double_array *da;
    unsigned long flags;

    read_lock_irqsave(&castle_da_hash_lock, flags);
    da = __castle_da_hash_get(da_id);
    if(!da)
        goto out;
    castle_da_get(da);
out:
    read_unlock_irqrestore(&castle_da_hash_lock, flags);

    return da;
}

int castle_double_array_get(da_id_t da_id)
{
    struct castle_double_array *da;
    unsigned long flags;

    read_lock_irqsave(&castle_da_hash_lock, flags);
    da = __castle_da_hash_get(da_id);
    if(!da)
        goto out;
    castle_da_get(da);
    da->attachment_cnt++;
out:
    read_unlock_irqrestore(&castle_da_hash_lock, flags);

    return (da == NULL ? -EINVAL : 0);
}

void castle_double_array_put(da_id_t da_id)
{
    struct castle_double_array *da;

    /* We only call this for attached DAs which _must_ be in the hash. */
    da = castle_da_hash_get(da_id);
    BUG_ON(!da);
    /* DA allocated + our ref count on it. */
    BUG_ON(atomic_read(&da->ref_cnt) < 2);
    write_lock(&da->lock);
    da->attachment_cnt--;
    write_unlock(&da->lock);
    /* Put the ref cnt too. */
    castle_da_put(da);
}

int castle_double_array_destroy(da_id_t da_id)
{
    struct castle_double_array *da;
    unsigned long flags;
    int ret;

    write_lock_irqsave(&castle_da_hash_lock, flags);
    da = __castle_da_hash_get(da_id);
    /* Fail if we cannot find the da in the hash. */
    if(!da)
    {
        ret = -EINVAL;
        goto err_out;
    }
    BUG_ON(da->attachment_cnt < 0);
    /* Fail if there are attachments to the DA. */
    if(da->attachment_cnt > 0)
    {
        ret = -EBUSY;
        goto err_out;
    }
    /* Now we are happy to delete the DA. Remove it from the hash. */ 
    BUG_ON(castle_da_deleted(da));
    __castle_da_hash_remove(da); 
    da->hash_list.next = da->hash_list.prev = NULL;
    write_unlock_irqrestore(&castle_da_hash_lock, flags);

    castle_sysfs_da_del(da);

    printk("Marking DA %u for deletion\n", da_id);
    /* Set the destruction bit, which will stop further merges. */
    castle_da_deleted_set(da);
    /* Restart the merge threads, so that they get to exit, and drop their da refs. */
    castle_da_merge_restart(da, NULL);
    /* Add it to the list of deleted DAs. */
    list_add(&da->hash_list, &castle_deleted_das);
    /* Put the (usually) last reference to the DA. */
    castle_da_put_locked(da);

    return 0;

err_out:
    write_unlock_irqrestore(&castle_da_hash_lock, flags);
    return ret;
}

static int castle_da_size_get(struct castle_double_array *da, 
                              struct castle_component_tree *ct, 
                              int level_cnt, 
                              void *token)
{
    c_byte_off_t *size = (c_byte_off_t *)token;
    *size += castle_extent_size_get(ct->tree_ext_free.ext_id);
    *size += castle_extent_size_get(ct->data_ext_free.ext_id);
    *size += atomic64_read(&ct->large_ext_chk_cnt);

    return 0;
}

int castle_double_array_size_get(da_id_t da_id, c_byte_off_t *size)
{
    struct castle_double_array *da;
    int err_code = 0;
    c_byte_off_t s = 0;

    da = castle_da_ref_get(da_id);
    if (!da)
    {
        err_code = -EINVAL;
        goto out;
    }

    castle_da_foreach_tree(da, castle_da_size_get, (void *)&s);

    castle_da_put(da);

out:
    *size = s;
    return err_code;
}

/**
 * Set nice value for all merge threads within a DA.
 */
static int __castle_da_threads_priority_set(struct castle_double_array *da, void *_value)
{
    int i;
    int nice_value = *((int *)_value);

    for (i=0; i<MAX_DA_LEVEL; i++)
    {
        if (da->levels[i].merge.thread)
            set_user_nice(da->levels[i].merge.thread, nice_value);
    }

    return 0;
}

/**
 * Change the priority of merge threads for all doubling arrays.
 */
void castle_da_threads_priority_set(int nice_value)
{
    int i;

    castle_da_hash_iterate(__castle_da_threads_priority_set, &nice_value);

    for(i=0; i<NR_CASTLE_DA_WQS; i++)
        castle_wq_priority_set(castle_da_wqs[i]);
}
