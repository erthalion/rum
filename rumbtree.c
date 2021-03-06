/*-------------------------------------------------------------------------
 *
 * rumbtree.c
 *	  page utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 2015-2016, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/generic_xlog.h"

#include "rum.h"

/*
 * Locks buffer by needed method for search.
 */
static int
rumTraverseLock(Buffer buffer, bool searchMode)
{
	Page		page;
	int			access = RUM_SHARE;

	LockBuffer(buffer, RUM_SHARE);
	page = BufferGetPage(buffer);
	if (RumPageIsLeaf(page))
	{
		if (searchMode == FALSE)
		{
			/* we should relock our page */
			LockBuffer(buffer, RUM_UNLOCK);
			LockBuffer(buffer, RUM_EXCLUSIVE);

			/* But root can become non-leaf during relock */
			if (!RumPageIsLeaf(page))
			{
				/* restore old lock type (very rare) */
				LockBuffer(buffer, RUM_UNLOCK);
				LockBuffer(buffer, RUM_SHARE);
			}
			else
				access = RUM_EXCLUSIVE;
		}
	}

	return access;
}

RumBtreeStack *
rumPrepareFindLeafPage(RumBtree btree, BlockNumber blkno)
{
	RumBtreeStack *stack = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));

	stack->blkno = blkno;
	stack->buffer = ReadBuffer(btree->index, stack->blkno);
	stack->parent = NULL;
	stack->predictNumber = 1;

	rumTraverseLock(stack->buffer, btree->searchMode);

	return stack;
}

/*
 * Locates leaf page contained tuple
 */
RumBtreeStack *
rumReFindLeafPage(RumBtree btree, RumBtreeStack * stack)
{
	/*
	 * Traverse the tree upwards until we sure that requested leaf page is in
	 * this subtree. Or we can stop at root page.
	 */
	while (stack->parent)
	{
		RumBtreeStack *ptr;
		Page		page;
		OffsetNumber maxoff;

		LockBuffer(stack->buffer, RUM_UNLOCK);
		stack->parent->buffer =
			ReleaseAndReadBuffer(stack->buffer, btree->index, stack->parent->blkno);
		LockBuffer(stack->parent->buffer, RUM_SHARE);

		ptr = stack;
		stack = stack->parent;
		pfree(ptr);

		page = BufferGetPage(stack->buffer);
		maxoff = RumPageGetOpaque(page)->maxoff;

		/*
		 * We don't know right bound of rightmost pointer. So, we can be sure
		 * that requested leaf page is in this subtree only when requested
		 * item pointer is less than item pointer previous to rightmost.
		 */
		if (rumCompareItemPointers(
			  &(((PostingItem *) RumDataPageGetItem(page, maxoff - 1))->key),
								   &btree->items[btree->curitem].iptr) >= 0)
		{
			break;
		}
	}

	/* Traverse tree downwards. */
	stack = rumFindLeafPage(btree, stack);
	return stack;
}

/*
 * Locates leaf page contained tuple
 */
RumBtreeStack *
rumFindLeafPage(RumBtree btree, RumBtreeStack * stack)
{
	bool		isfirst = TRUE;
	BlockNumber rootBlkno;

	if (!stack)
		stack = rumPrepareFindLeafPage(btree, RUM_ROOT_BLKNO);
	rootBlkno = stack->blkno;

	for (;;)
	{
		Page		page;
		BlockNumber child;
		int			access = RUM_SHARE;

		stack->off = InvalidOffsetNumber;

		page = BufferGetPage(stack->buffer);

		if (isfirst)
		{
			if (RumPageIsLeaf(page) && !btree->searchMode)
				access = RUM_EXCLUSIVE;
			isfirst = FALSE;
		}
		else
			access = rumTraverseLock(stack->buffer, btree->searchMode);

		/*
		 * ok, page is correctly locked, we should check to move right ..,
		 * root never has a right link, so small optimization
		 */
		while (btree->fullScan == FALSE && stack->blkno != rootBlkno &&
			   btree->isMoveRight(btree, page))
		{
			BlockNumber rightlink = RumPageGetOpaque(page)->rightlink;

			if (rightlink == InvalidBlockNumber)
				/* rightmost page */
				break;

			stack->buffer = rumStepRight(stack->buffer, btree->index, access);
			stack->blkno = rightlink;
			page = BufferGetPage(stack->buffer);
		}

		if (RumPageIsLeaf(page))	/* we found, return locked page */
			return stack;

		/* now we have correct buffer, try to find child */
		child = btree->findChildPage(btree, stack);

		LockBuffer(stack->buffer, RUM_UNLOCK);
		Assert(child != InvalidBlockNumber);
		Assert(stack->blkno != child);

		if (btree->searchMode)
		{
			/* in search mode we may forget path to leaf */
			RumBtreeStack *ptr = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));
			Buffer		buffer = ReleaseAndReadBuffer(stack->buffer, btree->index, child);

			ptr->parent = stack;
			ptr->predictNumber = stack->predictNumber;
			stack->buffer = InvalidBuffer;

			stack = ptr;
			stack->blkno = child;
			stack->buffer = buffer;
		}
		else
		{
			RumBtreeStack *ptr = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));

			ptr->parent = stack;
			stack = ptr;
			stack->blkno = child;
			stack->buffer = ReadBuffer(btree->index, stack->blkno);
			stack->predictNumber = 1;
		}
	}
}

/*
 * Step right from current page.
 *
 * The next page is locked first, before releasing the current page. This is
 * crucial to protect from concurrent page deletion (see comment in
 * rumDeletePage).
 */
Buffer
rumStepRight(Buffer buffer, Relation index, int lockmode)
{
	Buffer		nextbuffer;
	Page		page = BufferGetPage(buffer);
	bool		isLeaf = RumPageIsLeaf(page);
	bool		isData = RumPageIsData(page);
	BlockNumber blkno = RumPageGetOpaque(page)->rightlink;

	nextbuffer = ReadBuffer(index, blkno);
	LockBuffer(nextbuffer, lockmode);
	UnlockReleaseBuffer(buffer);

	/* Sanity check that the page we stepped to is of similar kind. */
	page = BufferGetPage(nextbuffer);
	if (isLeaf != RumPageIsLeaf(page) || isData != RumPageIsData(page))
		elog(ERROR, "right sibling of RUM page is of different type");

	/*
	 * Given the proper lock sequence above, we should never land on a deleted
	 * page.
	 */
	if (RumPageIsDeleted(page))
		elog(ERROR, "right sibling of RUM page was deleted");

	return nextbuffer;
}

void
freeRumBtreeStack(RumBtreeStack * stack)
{
	while (stack)
	{
		RumBtreeStack *tmp = stack->parent;

		if (stack->buffer != InvalidBuffer)
			ReleaseBuffer(stack->buffer);

		pfree(stack);
		stack = tmp;
	}
}

/*
 * Try to find parent for current stack position, returns correct
 * parent and child's offset in  stack->parent.
 * Function should never release root page to prevent conflicts
 * with vacuum process
 */
void
rumFindParents(RumBtree btree, RumBtreeStack * stack,
			   BlockNumber rootBlkno)
{
	Page		page;
	Buffer		buffer;
	BlockNumber blkno,
				leftmostBlkno;
	OffsetNumber offset;
	RumBtreeStack *root = stack->parent;
	RumBtreeStack *ptr;

	if (!root)
	{
		/* XLog mode... */
		root = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));
		root->blkno = rootBlkno;
		root->buffer = ReadBuffer(btree->index, rootBlkno);
		LockBuffer(root->buffer, RUM_EXCLUSIVE);
		root->parent = NULL;
	}
	else
	{
		/*
		 * find root, we should not release root page until update is
		 * finished!!
		 */
		while (root->parent)
		{
			ReleaseBuffer(root->buffer);
			root = root->parent;
		}

		Assert(root->blkno == rootBlkno);
		Assert(BufferGetBlockNumber(root->buffer) == rootBlkno);
		LockBuffer(root->buffer, RUM_EXCLUSIVE);
	}
	root->off = InvalidOffsetNumber;

	page = BufferGetPage(root->buffer);
	Assert(!RumPageIsLeaf(page));

	/* check trivial case */
	if ((root->off = btree->findChildPtr(btree, page, stack->blkno, InvalidOffsetNumber)) != InvalidOffsetNumber)
	{
		stack->parent = root;
		return;
	}

	leftmostBlkno = blkno = btree->getLeftMostPage(btree, page);
	LockBuffer(root->buffer, RUM_UNLOCK);
	Assert(blkno != InvalidBlockNumber);

	for (;;)
	{
		buffer = ReadBuffer(btree->index, blkno);
		LockBuffer(buffer, RUM_EXCLUSIVE);
		page = BufferGetPage(buffer);
		if (RumPageIsLeaf(page))
			elog(ERROR, "Lost path");

		leftmostBlkno = btree->getLeftMostPage(btree, page);

		while ((offset = btree->findChildPtr(btree, page, stack->blkno, InvalidOffsetNumber)) == InvalidOffsetNumber)
		{
			blkno = RumPageGetOpaque(page)->rightlink;
			if (blkno == InvalidBlockNumber)
			{
				UnlockReleaseBuffer(buffer);
				break;
			}
			buffer = rumStepRight(buffer, btree->index, RUM_EXCLUSIVE);
			page = BufferGetPage(buffer);
		}

		if (blkno != InvalidBlockNumber)
		{
			ptr = (RumBtreeStack *) palloc(sizeof(RumBtreeStack));
			ptr->blkno = blkno;
			ptr->buffer = buffer;
			ptr->parent = root; /* it's may be wrong, but in next call we will
								 * correct */
			ptr->off = offset;
			stack->parent = ptr;
			return;
		}

		blkno = leftmostBlkno;
	}
}

/*
 * Insert value (stored in RumBtree) to tree described by stack
 *
 * During an index build, buildStats is non-null and the counters
 * it contains should be incremented as needed.
 *
 * NB: the passed-in stack is freed, as though by freeRumBtreeStack.
 */
void
rumInsertValue(Relation index, RumBtree btree, RumBtreeStack * stack,
			   GinStatsData *buildStats)
{
	RumBtreeStack *parent;
	BlockNumber rootBlkno;
	Page		page,
				rpage,
				lpage;
	GenericXLogState *state;

	/* extract root BlockNumber from stack */
	Assert(stack != NULL);
	parent = stack;
	while (parent->parent)
		parent = parent->parent;
	rootBlkno = parent->blkno;
	Assert(BlockNumberIsValid(rootBlkno));

	/* this loop crawls up the stack until the insertion is complete */
	for (;;)
	{
		BlockNumber savedRightLink;

		page = BufferGetPage(stack->buffer);
		savedRightLink = RumPageGetOpaque(page)->rightlink;

		if (btree->isEnoughSpace(btree, stack->buffer, stack->off))
		{
			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, stack->buffer, 0);

			btree->placeToPage(btree, page, stack->off);
			GenericXLogFinish(state);

			LockBuffer(stack->buffer, RUM_UNLOCK);
			freeRumBtreeStack(stack);

			return;
		}
		else
		{
			Buffer		rbuffer = RumNewBuffer(btree->index);
			Page		newlpage;

			/* During index build, count the newly-split page */
			if (buildStats)
			{
				if (btree->isData)
					buildStats->nDataPages++;
				else
					buildStats->nEntryPages++;
			}

			parent = stack->parent;

			if (parent == NULL)
			{
				Buffer		lbuffer;

				state = GenericXLogStart(index);

				page = GenericXLogRegisterBuffer(state, stack->buffer, 0);
				rpage = GenericXLogRegisterBuffer(state, rbuffer,
												  GENERIC_XLOG_FULL_IMAGE);

				/*
				 * newlpage is a pointer to memory page, it doesn't associate
				 * with buffer, stack->buffer should be untouched
				 */
				newlpage = btree->splitPage(btree, stack->buffer, rbuffer,
											page, rpage, stack->off);

				/*
				 * split root, so we need to allocate new left page and place
				 * pointer on root to left and right page
				 */
				lbuffer = RumNewBuffer(btree->index);
				lpage = GenericXLogRegisterBuffer(state, lbuffer,
												  GENERIC_XLOG_FULL_IMAGE);

				RumPageGetOpaque(rpage)->rightlink = InvalidBlockNumber;
				RumPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);

				RumInitPage(page, RumPageGetOpaque(newlpage)->flags & ~RUM_LEAF,
							BufferGetPageSize(stack->buffer));
				PageRestoreTempPage(newlpage, lpage);
				btree->fillRoot(btree, stack->buffer, lbuffer, rbuffer,
								page, lpage, rpage);

				GenericXLogFinish(state);

				UnlockReleaseBuffer(rbuffer);
				UnlockReleaseBuffer(lbuffer);
				LockBuffer(stack->buffer, RUM_UNLOCK);

				freeRumBtreeStack(stack);

				/* During index build, count the newly-added root page */
				if (buildStats)
				{
					if (btree->isData)
						buildStats->nDataPages++;
					else
						buildStats->nEntryPages++;
				}

				return;
			}
			else
			{
				/* split non-root page */

				state = GenericXLogStart(index);

				lpage = GenericXLogRegisterBuffer(state, stack->buffer, 0);
				rpage = GenericXLogRegisterBuffer(state, rbuffer, 0);

				/*
				 * newlpage is a pointer to memory page, it doesn't associate
				 * with buffer, stack->buffer should be untouched
				 */
				newlpage = btree->splitPage(btree, stack->buffer, rbuffer,
											lpage, rpage, stack->off);

				RumPageGetOpaque(rpage)->rightlink = savedRightLink;
				RumPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);

				PageRestoreTempPage(newlpage, lpage);

				GenericXLogFinish(state);

				UnlockReleaseBuffer(rbuffer);
			}
		}

		btree->isDelete = FALSE;

		/* search parent to lock */
		LockBuffer(parent->buffer, RUM_EXCLUSIVE);

		/* move right if it's needed */
		page = BufferGetPage(parent->buffer);
		while ((parent->off = btree->findChildPtr(btree, page, stack->blkno, parent->off)) == InvalidOffsetNumber)
		{
			BlockNumber rightlink = RumPageGetOpaque(page)->rightlink;

			if (rightlink == InvalidBlockNumber)
			{
				/*
				 * rightmost page, but we don't find parent, we should use
				 * plain search...
				 */
				LockBuffer(parent->buffer, RUM_UNLOCK);
				rumFindParents(btree, stack, rootBlkno);
				parent = stack->parent;
				Assert(parent != NULL);
				break;
			}

			parent->buffer = rumStepRight(parent->buffer, btree->index, RUM_EXCLUSIVE);
			parent->blkno = rightlink;
			page = BufferGetPage(parent->buffer);
		}

		UnlockReleaseBuffer(stack->buffer);
		pfree(stack);
		stack = parent;
	}
}
