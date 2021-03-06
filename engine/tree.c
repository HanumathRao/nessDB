/*
 * Copyright (c) 2012-2014 The nessDB Project Developers. All rights reserved.
 * Code is licensed with GPL. See COPYING.GPL file.
 *
 */

#include "cache.h"
#include "hdrse.h"
#include "se.h"
#include "atomic.h"
#include "file.h"
#include "leaf.h"
#include "txn.h"
#include "tree.h"

/*
 *
 * if the layout as follows(3 pivots, 4 partitions):
 *
 * 		+--------+--------+--------+
 * 		|   15   |   17   |   19   | +∞
 * 		+--------+--------+--------+\
 * 		  pidx0    pidx1    pidx2    pidx3
 *
 *
 * so if spk is 16, pidx = 1, after added(4 pivtos, 5 partitions):
 *
 * 		+--------+--------+--------+--------+
 * 		|   15   |  [16]  |   17   |   19   | +∞
 * 		+--------+--------+--------+--------+\
 * 		  pidx0   [pidx1]   pidx2    pidx3     pidx4
 *
 *
 * REQUIRES:
 * a) parent locked(L_WRITE)
 * b) node a locked(L_WRITE)
 * c) node b locked(L_WRITE)
 */
void _add_pivot_to_parent(struct tree *t,
                          struct node *parent,
                          int child_num,
                          struct node *a,
                          struct node *b,
                          struct msg *spk)
{
	int i;
	int pidx;

	pidx = child_num;
	parent->u.n.pivots = xrealloc(parent->u.n.pivots,
	                              parent->u.n.n_children * PIVOT_SIZE);

	parent->u.n.parts = xrealloc(parent->u.n.parts,
	                             (parent->u.n.n_children + 1) * PART_SIZE);

	/* slide pivots */
	for (i = parent->u.n.n_children - 1; i > pidx; i--) {
		parent->u.n.pivots[i] = parent->u.n.pivots[i - 1];
	}

	/* slide parts */
	for (i = parent->u.n.n_children; i > pidx; i--) {
		parent->u.n.parts[i] = parent->u.n.parts[i - 1];
	}

	msgcpy(&parent->u.n.pivots[pidx], spk);
	parent->u.n.parts[pidx].child_nid = a->nid;
	parent->u.n.parts[pidx].buffer = msgbuf_new();

	parent->u.n.parts[pidx + 1].child_nid = b->nid;
	parent->u.n.n_children += 1;
	node_set_dirty(parent);

	t->status->tree_add_pivots_nums++;
}

/*
 * split leaf to two leaves:a & b
 *
 * REQUIRES:
 * a) leaf lock (L_WRITE)
 */
void _leaf_split(struct tree *t,
                 struct node *leaf,
                 struct node **a,
                 struct node **b,
                 struct msg **split_key)
{
	int mid;
	int i = 0;
	struct node *leafa;
	struct node *leafb;
	struct msgbuf *mb;
	struct msgbuf *buffa;
	struct msgbuf *buffb;
	struct msg *spk = NULL;
	struct msgbuf *old_buff;
	struct msgbuf_iter iter;

	leafa = leaf;
	old_buff = leafa->u.l.buffer;
	buffa = msgbuf_new();
	buffb = msgbuf_new();

	mid = old_buff->count / 2;
	msgbuf_iter_init(&iter, old_buff);
	msgbuf_iter_seektofirst(&iter);
	while (msgbuf_iter_valid(&iter)) {
		if (i <= mid) {
			mb = buffa;
		} else {
			mb = buffb;
		}

		while (msgbuf_internal_iter_next(&iter)) {
			msgbuf_put(mb,
			           iter.msn, iter.type,
			           &iter.key, &iter.val,
			           &iter.xidpair);
			if (i == mid)
				spk = msgdup(&iter.key);

			i++;
		}

		msgbuf_iter_next(&iter);
	}

	msgbuf_free(old_buff);
	leafa->u.l.buffer = buffa;

	/* new leafb */
	cache_create_node_and_pin(t->cf, 0, 0, &leafb);

	leafb->u.l.buffer = buffb;
	node_set_dirty(leafa);
	node_set_dirty(leafb);

	*a = leafa;
	*b = leafb;
	*split_key = spk;

	t->status->tree_leaf_split_nums++;
}

/*
 * 	+-----------------------------------------------+
 * 	| 	5 	| 	7 	| 	9 	|
 * 	+-----------------------------------------------+
 * 				|
 * 			+---------------+
 * 			|  60 | 61 | 62 |
 * 			+---------------+
 *
 * 	+---------------------------------------------------------------+
 * 	|  	5 	| 	60	 |	7 	| 	9 	|
 * 	+---------------------------------------------------------------+
 * 				|		|
 * 			    +--------+	    +---------+
 * 			    |   60   |	    | 61 | 62 |
 * 			    +--------+	    +---------+
 *
 *
 * REQUIRES:
 * a) node lock(L_WRITE)
 */
void _nonleaf_split(struct tree *t,
                    struct node *node,
                    struct node **a,
                    struct node **b,
                    struct msg **split_key)
{
	int i;
	int pivots_old;
	int pivots_in_a;
	int pivots_in_b;
	struct node *nodea;
	struct node *nodeb;
	struct msg *spk;

	nodea = node;
	pivots_old = node->u.n.n_children - 1;
	nassert(pivots_old > 2);

	pivots_in_a = pivots_old / 2;
	pivots_in_b = pivots_old - pivots_in_a;

	/* node a */
	nodea->u.n.n_children = pivots_in_a + 1;

	/* node b */
	cache_create_node_and_pin(t->cf,
	                          1,			/* height */
	                          pivots_in_b + 1,	/* children */
	                          &nodeb);

	for (i = 0; i < (pivots_in_b); i++) {
		nodeb->u.n.pivots[i] = node->u.n.pivots[pivots_in_a + i];
	}

	for (i = 0; i < (pivots_in_b + 1); i++) {
		nodeb->u.n.parts[i] = node->u.n.parts[pivots_in_a + i];
	}

	/* the rightest partition of nodea */
	nodea->u.n.parts[pivots_in_a].buffer = msgbuf_new();

	/* split key */
	spk = msgdup(&node->u.n.pivots[pivots_in_a - 1]);

	node_set_dirty(nodea);
	node_set_dirty(nodeb);

	*a = nodea;
	*b = nodeb;
	*split_key = spk;

	t->status->tree_nonleaf_split_nums++;
}

/*
 * split the child
 * add a new pivot(split-key) to parent
 *
 * REQUIRES:
 * a) parent lock(L_WRITE)
 * b) child lock(L_WRITE)
 */
void _node_split_child(struct tree *t,
                       struct node *parent,
                       struct node *child)
{
	int child_num;
	struct node *a;
	struct node *b;
	struct msg *split_key;

	if (child->height == 0) {
		_leaf_split(t, child, &a, &b, &split_key);
	} else {
		_nonleaf_split(t, child, &a, &b, &split_key);
	}

	child_num = node_partition_idx(parent, split_key);

	/* add pivot to parent */
	_add_pivot_to_parent(t, parent, child_num, a, b, split_key);
	msgfree(split_key);
	cache_unpin(t->cf, b);

	t->status->tree_nonleaf_split_nums++;
}

enum reactivity _get_reactivity(struct tree *t, struct node *node)
{
	if (node->height == 0) {
		if ((node_size(node) >= t->opts->leaf_node_page_size && node_count(node) > 1) ||
		    node_count(node) >= t->opts->leaf_node_page_count)
			return FISSIBLE;
	} else {
		uint32_t children = node->u.n.n_children;

		if (children >= t->opts->inner_node_fanout)
			return FISSIBLE;

		uint32_t i;
		int haszero = 0;

		for (i = 0; i < children; i++) {
			if (msgbuf_memsize(node->u.n.parts[i].buffer) == 0) {
				haszero = 1;
				break;
			}
		}

		if (((node_size(node) > t->opts->inner_node_page_size) && !haszero) ||
		    node_count(node) >= t->opts->inner_node_page_count)
			return FLUSHBLE;
	}

	return STABLE;
}

/*
 * put cmd to leaf
 *
 * REQUIRES:
 * a) leaf lock (L_WRITE)
 * b) leaf dmt write lock
 */
void leaf_put_cmd(struct node *leaf, struct bt_cmd *cmd)
{
	leaf_apply_msg(leaf, cmd);
}

/*
 * put cmd to nonleaf
 *
 * REQUIRES:
 * a) node lock (L_READ)
 * b) partition write lock
 */
void nonleaf_put_cmd(struct node *node, struct bt_cmd *cmd)
{
	uint32_t pidx;
	struct partition *part;

	pidx = node_partition_idx(node, cmd->key);
	part = &node->u.n.parts[pidx];

	if (!part->buffer)
		__PANIC("partiton buffer is null, index %d", pidx);

	write_lock(&part->rwlock);
	msgbuf_put(part->buffer,
	           cmd-> msn,
	           cmd->type,
	           cmd->key,
	           cmd->val,
	           &cmd->xidpair);
	node->msn = cmd->msn > node->msn ? cmd->msn : node->msn;
	node_set_dirty(node);
	write_unlock(&part->rwlock);

}

/*
 * put cmd to node
 *
 * REQUIRES:
 * a) node lock(L_WRITE)
 */
void _node_put_cmd(struct tree *t, struct node *node, struct bt_cmd *cmd)
{
	if (node->height == 0) {
		leaf_put_cmd(node, cmd);
		t->status->tree_leaf_put_nums++;
	} else {
		nonleaf_put_cmd(node, cmd);
		t->status->tree_nonleaf_put_nums++;
	}
}

/*
 * TODO:(BohuTANG) I am wanna in background thread
 *
 * REQUIRES:
 * a) parent lock (L_WRITE)
 * b) child lock (L_WRITE)
 */
int _flush_some_child(struct tree *t, struct node *parent)
{
	MSN msn;
	int childnum;

	struct node *child;
	struct partition *part;
	enum reactivity re_child;
	struct msgbuf *mb;
	struct msgbuf_iter iter;

	childnum = node_find_heaviest_idx(parent);
	nassert(childnum < (int)parent->u.n.n_children);

	part = &parent->u.n.parts[childnum];
	if (cache_get_and_pin(t->cf,
	                      part->child_nid,
	                      &child,
	                      L_WRITE) != NESS_OK) {
		__ERROR("cache get node error, nid [%" PRIu64 "]",
		        part->child_nid);

		return NESS_ERR;
	}

	msn = child->msn;
	mb = part->buffer;
	msgbuf_iter_init(&iter, mb);
	msgbuf_iter_seektofirst(&iter);
	while (msgbuf_iter_valid(&iter)) {
		if (msn >= iter.msn) continue;

		struct bt_cmd cmd = {
			.msn = iter.msn,
			.type = iter.type,
			.key = &iter.key,
			.val = &iter.val,
			.xidpair = iter.xidpair
		};
		_node_put_cmd(t, child, &cmd);
		msgbuf_iter_next(&iter);
	}

	/* free flushed msgbuffer */
	msgbuf_free(part->buffer);
	part->buffer = msgbuf_new();
	node_set_dirty(parent);
	node_set_dirty(child);

	/*
	 * check child reactivity
	 * 1. if STABLE, return
	 * 2. if FISSIBLE, split the node
	 * 3. if FLUSHBLE, flush child node w/recursive
	 * 4. others it will get panic
	 */
	re_child = _get_reactivity(t, child);
	switch (re_child) {
	case STABLE:
		cache_unpin(t->cf, child);
		cache_unpin(t->cf, parent);
		break;
	case FISSIBLE:
		_node_split_child(t, parent, child);
		cache_unpin(t->cf, child);
		cache_unpin(t->cf, parent);
		break;
	case FLUSHBLE:
		nassert(child->height > 0);
		cache_unpin(t->cf, parent);
		_flush_some_child(t, child);
		break;
	default:
		__PANIC("%s", "unsupport reactivity enum");
	}

	return NESS_OK;
}

/*
 * in order to keep the root NID is eternal
 * so swap me!
 *
 * REQUIRES:
 * a) new_root lock(L_WRITE)
 * b) old_root lock(L_WRITE)
 */
void _root_swap(struct tree *t,
                struct node *new_root,
                struct node *old_root)
{
	NID old_nid;
	NID new_nid;

	old_nid = old_root->nid;
	new_nid = new_root->nid;

	cache_cpair_value_swap(t->cf, new_root, old_root);

	/* swap nid */
	old_root->nid = new_nid;
	new_root->nid = old_nid;

	/* swap root flag */
	old_root->isroot = 0;
	new_root->isroot = 1;
}

void _root_split(struct tree *t,
                 struct node *new_root,
                 struct node *old_root)
{
	struct node *a;
	struct node *b;
	struct msg *split_key;

	if (old_root->height == 0) {
		_leaf_split(t, old_root, &a, &b, &split_key);
	} else {
		_nonleaf_split(t, old_root, &a, &b, &split_key);
	}

	/* swap two roots */
	_root_swap(t, new_root, old_root);

	msgcpy(&new_root->u.n.pivots[0], split_key);
	new_root->u.n.parts[0].child_nid = a->nid;
	new_root->u.n.parts[0].buffer = msgbuf_new();

	new_root->u.n.parts[1].child_nid = b->nid;
	new_root->u.n.parts[1].buffer = msgbuf_new();
	msgfree(split_key);

	node_set_dirty(b);
	cache_unpin(t->cf, b);

	node_set_dirty(old_root);
	node_set_dirty(new_root);

	t->status->tree_root_new_nums++;
	t->hdr->height++;
}

/*
 * REQUIRES:
 * a) root node locked
 *
 * PROCESSES:
 * a) to check root reactivity
 *    a1) if not stable, relock from L_READ to L_WRITE
 */
int root_put_cmd(struct tree *t, struct bt_cmd *cmd)
{
	struct node *root;
	enum reactivity re;
	enum lock_type locktype = L_READ;

CHANGE_LOCK_TYPE:
	if (!cache_get_and_pin(t->cf, t->hdr->root_nid, &root, locktype)) {
		return NESS_ERR;
	}

	re = _get_reactivity(t, root);
	switch (re) {
	case STABLE:
		break;
	case FISSIBLE: {
			if (locktype == L_READ) {
				cache_unpin(t->cf, root);

				locktype = L_WRITE;
				goto CHANGE_LOCK_TYPE;
			}

			struct node *new_root;
			uint32_t new_root_height = 1;
			uint32_t new_root_children = 2;

			cache_create_node_and_pin(t->cf,
			                          new_root_height,
			                          new_root_children,
			                          &new_root);
			/*
			 * split root node,
			 * now new_root is real root node with same NID
			 * after root swap
			 */
			_root_split(t, new_root, root);

			cache_unpin(t->cf, root);
			cache_unpin(t->cf, new_root);

			locktype = L_READ;
			goto CHANGE_LOCK_TYPE;
		}
		break;
	case FLUSHBLE:
		if (locktype == L_READ) {
			cache_unpin(t->cf, root);
			locktype = L_WRITE;
			goto CHANGE_LOCK_TYPE;
		}
		_flush_some_child(t, root);
		t->status->tree_flush_child_nums++;

		locktype = L_READ;
		goto CHANGE_LOCK_TYPE;

		break;
	default :
		abort();
	}

	_node_put_cmd(t, root, cmd);
	cache_unpin(t->cf, root);

	return NESS_OK;
}

NID hdr_next_nid(struct tree *t)
{
	atomic64_increment(&t->hdr->last_nid);
	nassert(t->hdr->last_nid >= NID_START);

	return t->hdr->last_nid;
}

MSN hdr_next_msn(struct tree *t)
{
	atomic64_increment(&t->hdr->last_msn);

	return t->hdr->last_msn;
}

struct tree *tree_open(const char *dbname,
                       struct options *opts,
                       struct status *status,
                       struct cache *cache,
                       struct tree_callback *tcb) {
	int fd;
	int flag;
	int is_create = 0;
	mode_t mode;
	struct tree *t;
	struct node *root;
	struct cache_file *cf;

	t = xcalloc(1, sizeof(*t));
	t->opts = opts;
	t->status = status;

	mode = S_IRWXU | S_IRWXG | S_IRWXO;
	flag = O_RDWR | O_BINARY;
	if (opts->use_directio)
		fd = ness_os_open_direct(dbname, flag, mode);
	else
		fd = ness_os_open(dbname, flag, mode);

	if (fd == -1) {
		if (opts->use_directio)
			fd = ness_os_open(dbname, flag | O_CREAT, mode);
		else
			fd = ness_os_open_direct(dbname, flag | O_CREAT, mode);
		if (fd == -1)
			goto ERR;
		is_create = 1;
	}

	t->fd = fd;
	t->block = block_new();

	/* tree header */
	if (is_create) {
		t->hdr = xcalloc(1, sizeof(*t->hdr));
		t->hdr->height = 0U;
		t->hdr->last_nid = NID_START;
		t->hdr->method = opts->compress_method;
	} else
		tcb->fetch_hdr(t);

	/* create cache file */
	cf = cache_file_create(cache, tcb, t);
	t->cf = cf;

	/* tree root node */
	if (is_create) {
		cache_create_node_and_pin(cf, 0U, 0U, &root);
		leaf_alloc_msgbuf(root);
		root->isroot = 1;
		node_set_dirty(root);

		cache_unpin(cf, root);
		t->hdr->root_nid = root->nid;
	} else {
		/* get the root node */
		if (cache_get_and_pin(cf, t->hdr->root_nid, &root, L_READ) !=
		    NESS_OK) {
			__PANIC("get root from cache error [%" PRIu64 "]",
			        t->hdr->root_nid);
		}
		root->isroot = 1;
		cache_unpin(cf, root);
	}

	return t;

ERR:
	xfree(t);
	return NESS_ERR;
}

int tree_put(struct tree *t,
             struct msg *k,
             struct msg *v,
             msgtype_t type,
             TXN *txn)
{
	TXNID child_xid = TXNID_NONE;
	TXNID parent_xid = TXNID_NONE;

	if (txn) {
		FILENUM fn = {.fileid = t->cf->filenum};
		child_xid = txn->txnid;
		parent_xid = txn->root_parent_txnid;

		switch (type) {
		case MSG_INSERT:
			rollback_save_cmdinsert(txn, fn, k);
			break;
		case MSG_DELETE:
			rollback_save_cmddelete(txn, fn, k);
			break;
		case MSG_UPDATE:
			rollback_save_cmdupdate(txn, fn, k);
			break;
		default:
			break;
		}
	}

	struct bt_cmd cmd = {
		.msn = hdr_next_msn(t),
		.type = type,
		.key = k,
		.val = v,
		.xidpair = {.child_xid = child_xid, .parent_xid = parent_xid}
	};

	return  root_put_cmd(t, &cmd);
}

void tree_free(struct tree *t)
{
	if (!t) return;

	ness_os_close(t->fd);
	block_free(t->block);
	xfree(t->hdr);
	xfree(t);
}
