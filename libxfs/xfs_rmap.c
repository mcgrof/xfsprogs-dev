/*
 * Copyright (c) 2014 Red Hat, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_btree.h"
#include "xfs_trans.h"
#include "xfs_alloc.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_trace.h"

/*
 * Lookup the first record less than or equal to [bno, len, owner, offset]
 * in the btree given by cur.
 */
int
xfs_rmap_lookup_le(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		bno,
	xfs_extlen_t		len,
	uint64_t		owner,
	uint64_t		offset,
	unsigned int		flags,
	int			*stat)
{
	cur->bc_rec.r.rm_startblock = bno;
	cur->bc_rec.r.rm_blockcount = len;
	cur->bc_rec.r.rm_owner = owner;
	cur->bc_rec.r.rm_offset = offset;
	cur->bc_rec.r.rm_flags = flags;
	return xfs_btree_lookup(cur, XFS_LOOKUP_LE, stat);
}

/*
 * Lookup the record exactly matching [bno, len, owner, offset]
 * in the btree given by cur.
 */
int
xfs_rmap_lookup_eq(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		bno,
	xfs_extlen_t		len,
	uint64_t		owner,
	uint64_t		offset,
	unsigned int		flags,
	int			*stat)
{
	cur->bc_rec.r.rm_startblock = bno;
	cur->bc_rec.r.rm_blockcount = len;
	cur->bc_rec.r.rm_owner = owner;
	cur->bc_rec.r.rm_offset = offset;
	cur->bc_rec.r.rm_flags = flags;
	return xfs_btree_lookup(cur, XFS_LOOKUP_EQ, stat);
}

/*
 * Update the record referred to by cur to the value given
 * by [bno, len, owner, offset].
 * This either works (return 0) or gets an EFSCORRUPTED error.
 */
STATIC int
xfs_rmap_update(
	struct xfs_btree_cur	*cur,
	struct xfs_rmap_irec	*irec)
{
	union xfs_btree_rec	rec;

	rec.rmap.rm_startblock = cpu_to_be32(irec->rm_startblock);
	rec.rmap.rm_blockcount = cpu_to_be32(irec->rm_blockcount);
	rec.rmap.rm_owner = cpu_to_be64(irec->rm_owner);
	rec.rmap.rm_offset = cpu_to_be64(
			xfs_rmap_irec_offset_pack(irec));
	return xfs_btree_update(cur, &rec);
}

static int
xfs_rmap_btrec_to_irec(
	union xfs_btree_rec	*rec,
	struct xfs_rmap_irec	*irec)
{
	irec->rm_flags = 0;
	irec->rm_startblock = be32_to_cpu(rec->rmap.rm_startblock);
	irec->rm_blockcount = be32_to_cpu(rec->rmap.rm_blockcount);
	irec->rm_owner = be64_to_cpu(rec->rmap.rm_owner);
	return xfs_rmap_irec_offset_unpack(be64_to_cpu(rec->rmap.rm_offset),
			irec);
}

/*
 * Get the data from the pointed-to record.
 */
int
xfs_rmap_get_rec(
	struct xfs_btree_cur	*cur,
	struct xfs_rmap_irec	*irec,
	int			*stat)
{
	union xfs_btree_rec	*rec;
	int			error;

	error = xfs_btree_get_rec(cur, &rec, stat);
	if (error || !*stat)
		return error;

	return xfs_rmap_btrec_to_irec(rec, irec);
}

/*
 * Find the extent in the rmap btree and remove it.
 *
 * The record we find should always be an exact match for the extent that we're
 * looking for, since we insert them into the btree without modification.
 *
 * Special Case #1: when growing the filesystem, we "free" an extent when
 * growing the last AG. This extent is new space and so it is not tracked as
 * used space in the btree. The growfs code will pass in an owner of
 * XFS_RMAP_OWN_NULL to indicate that it expected that there is no owner of this
 * extent. We verify that - the extent lookup result in a record that does not
 * overlap.
 *
 * Special Case #2: EFIs do not record the owner of the extent, so when
 * recovering EFIs from the log we pass in XFS_RMAP_OWN_UNKNOWN to tell the rmap
 * btree to ignore the owner (i.e. wildcard match) so we don't trigger
 * corruption checks during log recovery.
 */
STATIC int
xfs_rmap_unmap(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		bno,
	xfs_extlen_t		len,
	bool			unwritten,
	struct xfs_owner_info	*oinfo)
{
	struct xfs_mount	*mp = cur->bc_mp;
	struct xfs_rmap_irec	ltrec;
	uint64_t		ltoff;
	int			error = 0;
	int			i;
	uint64_t		owner;
	uint64_t		offset;
	unsigned int		flags;
	bool			ignore_off;

	xfs_owner_info_unpack(oinfo, &owner, &offset, &flags);
	ignore_off = XFS_RMAP_NON_INODE_OWNER(owner) ||
			(flags & XFS_RMAP_BMBT_BLOCK);
	if (unwritten)
		flags |= XFS_RMAP_UNWRITTEN;
	trace_xfs_rmap_unmap(mp, cur->bc_private.a.agno, bno, len,
			unwritten, oinfo);

	/*
	 * We should always have a left record because there's a static record
	 * for the AG headers at rm_startblock == 0 created by mkfs/growfs that
	 * will not ever be removed from the tree.
	 */
	error = xfs_rmap_lookup_le(cur, bno, len, owner, offset, flags, &i);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(mp, i == 1, out_error);

	error = xfs_rmap_get_rec(cur, &ltrec, &i);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(mp, i == 1, out_error);
	trace_xfs_rmap_lookup_le_range_result(cur->bc_mp,
			cur->bc_private.a.agno, ltrec.rm_startblock,
			ltrec.rm_blockcount, ltrec.rm_owner,
			ltrec.rm_offset, ltrec.rm_flags);
	ltoff = ltrec.rm_offset;

	/*
	 * For growfs, the incoming extent must be beyond the left record we
	 * just found as it is new space and won't be used by anyone. This is
	 * just a corruption check as we don't actually do anything with this
	 * extent.  Note that we need to use >= instead of > because it might
	 * be the case that the "left" extent goes all the way to EOFS.
	 */
	if (owner == XFS_RMAP_OWN_NULL) {
		XFS_WANT_CORRUPTED_GOTO(mp, bno >= ltrec.rm_startblock +
						ltrec.rm_blockcount, out_error);
		goto out_done;
	}

	/* Make sure the unwritten flag matches. */
	XFS_WANT_CORRUPTED_GOTO(mp, (flags & XFS_RMAP_UNWRITTEN) ==
			(ltrec.rm_flags & XFS_RMAP_UNWRITTEN), out_error);

	/* Make sure the extent we found covers the entire freeing range. */
	XFS_WANT_CORRUPTED_GOTO(mp, ltrec.rm_startblock <= bno &&
		ltrec.rm_startblock + ltrec.rm_blockcount >=
		bno + len, out_error);

	/* Make sure the owner matches what we expect to find in the tree. */
	XFS_WANT_CORRUPTED_GOTO(mp, owner == ltrec.rm_owner ||
				    XFS_RMAP_NON_INODE_OWNER(owner), out_error);

	/* Check the offset, if necessary. */
	if (!XFS_RMAP_NON_INODE_OWNER(owner)) {
		if (flags & XFS_RMAP_BMBT_BLOCK) {
			XFS_WANT_CORRUPTED_GOTO(mp,
					ltrec.rm_flags & XFS_RMAP_BMBT_BLOCK,
					out_error);
		} else {
			XFS_WANT_CORRUPTED_GOTO(mp,
					ltrec.rm_offset <= offset, out_error);
			XFS_WANT_CORRUPTED_GOTO(mp,
					ltoff + ltrec.rm_blockcount >= offset + len,
					out_error);
		}
	}

	if (ltrec.rm_startblock == bno && ltrec.rm_blockcount == len) {
		/* exact match, simply remove the record from rmap tree */
		trace_xfs_rmap_delete(mp, cur->bc_private.a.agno,
				ltrec.rm_startblock, ltrec.rm_blockcount,
				ltrec.rm_owner, ltrec.rm_offset,
				ltrec.rm_flags);
		error = xfs_btree_delete(cur, &i);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(mp, i == 1, out_error);
	} else if (ltrec.rm_startblock == bno) {
		/*
		 * overlap left hand side of extent: move the start, trim the
		 * length and update the current record.
		 *
		 *       ltbno                ltlen
		 * Orig:    |oooooooooooooooooooo|
		 * Freeing: |fffffffff|
		 * Result:            |rrrrrrrrrr|
		 *         bno       len
		 */
		ltrec.rm_startblock += len;
		ltrec.rm_blockcount -= len;
		if (!ignore_off)
			ltrec.rm_offset += len;
		error = xfs_rmap_update(cur, &ltrec);
		if (error)
			goto out_error;
	} else if (ltrec.rm_startblock + ltrec.rm_blockcount == bno + len) {
		/*
		 * overlap right hand side of extent: trim the length and update
		 * the current record.
		 *
		 *       ltbno                ltlen
		 * Orig:    |oooooooooooooooooooo|
		 * Freeing:            |fffffffff|
		 * Result:  |rrrrrrrrrr|
		 *                    bno       len
		 */
		ltrec.rm_blockcount -= len;
		error = xfs_rmap_update(cur, &ltrec);
		if (error)
			goto out_error;
	} else {

		/*
		 * overlap middle of extent: trim the length of the existing
		 * record to the length of the new left-extent size, increment
		 * the insertion position so we can insert a new record
		 * containing the remaining right-extent space.
		 *
		 *       ltbno                ltlen
		 * Orig:    |oooooooooooooooooooo|
		 * Freeing:       |fffffffff|
		 * Result:  |rrrrr|         |rrrr|
		 *               bno       len
		 */
		xfs_extlen_t	orig_len = ltrec.rm_blockcount;

		ltrec.rm_blockcount = bno - ltrec.rm_startblock;
		error = xfs_rmap_update(cur, &ltrec);
		if (error)
			goto out_error;

		error = xfs_btree_increment(cur, 0, &i);
		if (error)
			goto out_error;

		cur->bc_rec.r.rm_startblock = bno + len;
		cur->bc_rec.r.rm_blockcount = orig_len - len -
						     ltrec.rm_blockcount;
		cur->bc_rec.r.rm_owner = ltrec.rm_owner;
		if (ignore_off)
			cur->bc_rec.r.rm_offset = 0;
		else
			cur->bc_rec.r.rm_offset = offset + len;
		cur->bc_rec.r.rm_flags = flags;
		trace_xfs_rmap_insert(mp, cur->bc_private.a.agno,
				cur->bc_rec.r.rm_startblock,
				cur->bc_rec.r.rm_blockcount,
				cur->bc_rec.r.rm_owner,
				cur->bc_rec.r.rm_offset,
				cur->bc_rec.r.rm_flags);
		error = xfs_btree_insert(cur, &i);
		if (error)
			goto out_error;
	}

out_done:
	trace_xfs_rmap_unmap_done(mp, cur->bc_private.a.agno, bno, len,
			unwritten, oinfo);
out_error:
	if (error)
		trace_xfs_rmap_unmap_error(mp, cur->bc_private.a.agno,
				error, _RET_IP_);
	return error;
}

/*
 * Remove a reference to an extent in the rmap btree.
 */
int
xfs_rmap_free(
	struct xfs_trans	*tp,
	struct xfs_buf		*agbp,
	xfs_agnumber_t		agno,
	xfs_agblock_t		bno,
	xfs_extlen_t		len,
	struct xfs_owner_info	*oinfo)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_btree_cur	*cur;
	int			error;

	if (!xfs_sb_version_hasrmapbt(&mp->m_sb))
		return 0;

	cur = xfs_rmapbt_init_cursor(mp, tp, agbp, agno);

	error = xfs_rmap_unmap(cur, bno, len, false, oinfo);
	if (error)
		goto out_error;

	xfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
	return 0;

out_error:
	xfs_btree_del_cursor(cur, XFS_BTREE_ERROR);
	return error;
}

/*
 * A mergeable rmap must have the same owner and the same values for
 * the unwritten, attr_fork, and bmbt flags.  The startblock and
 * offset are checked separately.
 */
static bool
xfs_rmap_is_mergeable(
	struct xfs_rmap_irec	*irec,
	uint64_t		owner,
	unsigned int		flags)
{
	if (irec->rm_owner == XFS_RMAP_OWN_NULL)
		return false;
	if (irec->rm_owner != owner)
		return false;
	if ((flags & XFS_RMAP_UNWRITTEN) ^
	    (irec->rm_flags & XFS_RMAP_UNWRITTEN))
		return false;
	if ((flags & XFS_RMAP_ATTR_FORK) ^
	    (irec->rm_flags & XFS_RMAP_ATTR_FORK))
		return false;
	if ((flags & XFS_RMAP_BMBT_BLOCK) ^
	    (irec->rm_flags & XFS_RMAP_BMBT_BLOCK))
		return false;
	return true;
}

/*
 * When we allocate a new block, the first thing we do is add a reference to
 * the extent in the rmap btree. This takes the form of a [agbno, length,
 * owner, offset] record.  Flags are encoded in the high bits of the offset
 * field.
 */
STATIC int
xfs_rmap_map(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		bno,
	xfs_extlen_t		len,
	bool			unwritten,
	struct xfs_owner_info	*oinfo)
{
	struct xfs_mount	*mp = cur->bc_mp;
	struct xfs_rmap_irec	ltrec;
	struct xfs_rmap_irec	gtrec;
	int			have_gt;
	int			have_lt;
	int			error = 0;
	int			i;
	uint64_t		owner;
	uint64_t		offset;
	unsigned int		flags = 0;
	bool			ignore_off;

	xfs_owner_info_unpack(oinfo, &owner, &offset, &flags);
	ASSERT(owner != 0);
	ignore_off = XFS_RMAP_NON_INODE_OWNER(owner) ||
			(flags & XFS_RMAP_BMBT_BLOCK);
	if (unwritten)
		flags |= XFS_RMAP_UNWRITTEN;
	trace_xfs_rmap_map(mp, cur->bc_private.a.agno, bno, len,
			unwritten, oinfo);

	/*
	 * For the initial lookup, look for an exact match or the left-adjacent
	 * record for our insertion point. This will also give us the record for
	 * start block contiguity tests.
	 */
	error = xfs_rmap_lookup_le(cur, bno, len, owner, offset, flags,
			&have_lt);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(mp, have_lt == 1, out_error);

	error = xfs_rmap_get_rec(cur, &ltrec, &have_lt);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(mp, have_lt == 1, out_error);
	trace_xfs_rmap_lookup_le_range_result(cur->bc_mp,
			cur->bc_private.a.agno, ltrec.rm_startblock,
			ltrec.rm_blockcount, ltrec.rm_owner,
			ltrec.rm_offset, ltrec.rm_flags);

	if (!xfs_rmap_is_mergeable(&ltrec, owner, flags))
		have_lt = 0;

	XFS_WANT_CORRUPTED_GOTO(mp,
		have_lt == 0 ||
		ltrec.rm_startblock + ltrec.rm_blockcount <= bno, out_error);

	/*
	 * Increment the cursor to see if we have a right-adjacent record to our
	 * insertion point. This will give us the record for end block
	 * contiguity tests.
	 */
	error = xfs_btree_increment(cur, 0, &have_gt);
	if (error)
		goto out_error;
	if (have_gt) {
		error = xfs_rmap_get_rec(cur, &gtrec, &have_gt);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(mp, have_gt == 1, out_error);
		XFS_WANT_CORRUPTED_GOTO(mp, bno + len <= gtrec.rm_startblock,
					out_error);
		trace_xfs_rmap_find_right_neighbor_result(cur->bc_mp,
			cur->bc_private.a.agno, gtrec.rm_startblock,
			gtrec.rm_blockcount, gtrec.rm_owner,
			gtrec.rm_offset, gtrec.rm_flags);
		if (!xfs_rmap_is_mergeable(&gtrec, owner, flags))
			have_gt = 0;
	}

	/*
	 * Note: cursor currently points one record to the right of ltrec, even
	 * if there is no record in the tree to the right.
	 */
	if (have_lt &&
	    ltrec.rm_startblock + ltrec.rm_blockcount == bno &&
	    (ignore_off || ltrec.rm_offset + ltrec.rm_blockcount == offset)) {
		/*
		 * left edge contiguous, merge into left record.
		 *
		 *       ltbno     ltlen
		 * orig:   |ooooooooo|
		 * adding:           |aaaaaaaaa|
		 * result: |rrrrrrrrrrrrrrrrrrr|
		 *                  bno       len
		 */
		ltrec.rm_blockcount += len;
		if (have_gt &&
		    bno + len == gtrec.rm_startblock &&
		    (ignore_off || offset + len == gtrec.rm_offset) &&
		    (unsigned long)ltrec.rm_blockcount + len +
				gtrec.rm_blockcount <= XFS_RMAP_LEN_MAX) {
			/*
			 * right edge also contiguous, delete right record
			 * and merge into left record.
			 *
			 *       ltbno     ltlen    gtbno     gtlen
			 * orig:   |ooooooooo|         |ooooooooo|
			 * adding:           |aaaaaaaaa|
			 * result: |rrrrrrrrrrrrrrrrrrrrrrrrrrrrr|
			 */
			ltrec.rm_blockcount += gtrec.rm_blockcount;
			trace_xfs_rmap_delete(mp, cur->bc_private.a.agno,
					gtrec.rm_startblock,
					gtrec.rm_blockcount,
					gtrec.rm_owner,
					gtrec.rm_offset,
					gtrec.rm_flags);
			error = xfs_btree_delete(cur, &i);
			if (error)
				goto out_error;
			XFS_WANT_CORRUPTED_GOTO(mp, i == 1, out_error);
		}

		/* point the cursor back to the left record and update */
		error = xfs_btree_decrement(cur, 0, &have_gt);
		if (error)
			goto out_error;
		error = xfs_rmap_update(cur, &ltrec);
		if (error)
			goto out_error;
	} else if (have_gt &&
		   bno + len == gtrec.rm_startblock &&
		   (ignore_off || offset + len == gtrec.rm_offset)) {
		/*
		 * right edge contiguous, merge into right record.
		 *
		 *                 gtbno     gtlen
		 * Orig:             |ooooooooo|
		 * adding: |aaaaaaaaa|
		 * Result: |rrrrrrrrrrrrrrrrrrr|
		 *        bno       len
		 */
		gtrec.rm_startblock = bno;
		gtrec.rm_blockcount += len;
		if (!ignore_off)
			gtrec.rm_offset = offset;
		error = xfs_rmap_update(cur, &gtrec);
		if (error)
			goto out_error;
	} else {
		/*
		 * no contiguous edge with identical owner, insert
		 * new record at current cursor position.
		 */
		cur->bc_rec.r.rm_startblock = bno;
		cur->bc_rec.r.rm_blockcount = len;
		cur->bc_rec.r.rm_owner = owner;
		cur->bc_rec.r.rm_offset = offset;
		cur->bc_rec.r.rm_flags = flags;
		trace_xfs_rmap_insert(mp, cur->bc_private.a.agno, bno, len,
			owner, offset, flags);
		error = xfs_btree_insert(cur, &i);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(mp, i == 1, out_error);
	}

	trace_xfs_rmap_map_done(mp, cur->bc_private.a.agno, bno, len,
			unwritten, oinfo);
out_error:
	if (error)
		trace_xfs_rmap_map_error(mp, cur->bc_private.a.agno,
				error, _RET_IP_);
	return error;
}

/*
 * Add a reference to an extent in the rmap btree.
 */
int
xfs_rmap_alloc(
	struct xfs_trans	*tp,
	struct xfs_buf		*agbp,
	xfs_agnumber_t		agno,
	xfs_agblock_t		bno,
	xfs_extlen_t		len,
	struct xfs_owner_info	*oinfo)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_btree_cur	*cur;
	int			error;

	if (!xfs_sb_version_hasrmapbt(&mp->m_sb))
		return 0;

	cur = xfs_rmapbt_init_cursor(mp, tp, agbp, agno);
	error = xfs_rmap_map(cur, bno, len, false, oinfo);
	if (error)
		goto out_error;

	xfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
	return 0;

out_error:
	xfs_btree_del_cursor(cur, XFS_BTREE_ERROR);
	return error;
}

struct xfs_rmap_query_range_info {
	xfs_rmap_query_range_fn	fn;
	void				*priv;
};

/* Format btree record and pass to our callback. */
STATIC int
xfs_rmap_query_range_helper(
	struct xfs_btree_cur	*cur,
	union xfs_btree_rec	*rec,
	void			*priv)
{
	struct xfs_rmap_query_range_info	*query = priv;
	struct xfs_rmap_irec			irec;
	int					error;

	error = xfs_rmap_btrec_to_irec(rec, &irec);
	if (error)
		return error;
	return query->fn(cur, &irec, query->priv);
}

/* Find all rmaps between two keys. */
int
xfs_rmap_query_range(
	struct xfs_btree_cur		*cur,
	struct xfs_rmap_irec		*low_rec,
	struct xfs_rmap_irec		*high_rec,
	xfs_rmap_query_range_fn	fn,
	void				*priv)
{
	union xfs_btree_irec		low_brec;
	union xfs_btree_irec		high_brec;
	struct xfs_rmap_query_range_info	query;

	low_brec.r = *low_rec;
	high_brec.r = *high_rec;
	query.priv = priv;
	query.fn = fn;
	return xfs_btree_query_range(cur, &low_brec, &high_brec,
			xfs_rmap_query_range_helper, &query);
}
