/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

     /*<std-header orig-src='shore' incl-file-exclusion='RESTART_H'>

        $Id: restart.h,v 1.27 2010/07/01 00:08:22 nhall Exp $

        SHORE -- Scalable Heterogeneous Object REpository

        Copyright (c) 1994-99 Computer Sciences Department, University of
                                             Wisconsin -- Madison
        All Rights Reserved.

        Permission to use, copy, modify and distribute this software and its
        documentation is hereby granted, provided that both the copyright
        notice and this permission notice appear in all copies of the
        software, derivative works or modified versions, and any portions
        thereof, and that both notices appear in supporting documentation.

        THE AUTHORS AND THE COMPUTER SCIENCES DEPARTMENT OF THE UNIVERSITY
        OF WISCONSIN - MADISON ALLOW FREE USE OF THIS SOFTWARE IN ITS
        "AS IS" CONDITION, AND THEY DISCLAIM ANY LIABILITY OF ANY KIND
        FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.

        This software was developed with support by the Advanced Research
        Project Agency, ARPA order number 018 (formerly 8230), monitored by
        the U.S. Army Research Laboratory under contract DAAB07-91-C-Q518.
        Further funding for this work was provided by DARPA through
        Rome Research Laboratory Contract No. F30602-97-2-0247.

        */

#ifndef RESTART_H
#define RESTART_H

#include "w_defines.h"
#include "w_heap.h"

#include "thread_wrapper.h"
#include "sm_base.h"
#include "chkpt.h"
#include "lock.h"               // Lock re-acquisition
#include "logarchive_scanner.h"

#include <map>

// Child thread created by restart_m for concurrent recovery operation
// It is to carry out the REDO and UNDO phases while the system is
// opened for user transactions
class restart_thread_t : public thread_wrapper_t
{
public:

    restart_thread_t()
    {
        smthread_t::set_lock_timeout(timeout_t::WAIT_FOREVER);
        working = false;
    };

    // Main body of the child thread
    void run();
    bool in_restart() { return working; }

private:

    bool working;

private:
    // disabled
    restart_thread_t(const restart_thread_t&);
    restart_thread_t& operator=(const restart_thread_t&);
};

/*
 * A log-record iterator that encapsulates a log archive scan and a recovery
 * log scan. It reads from the former until it runs out, after which it reads
 * from the latter, which is collected by following the per-page chain in the
 * recovery log.
 */
class SprIterator
{
public:

    // CS TODO: expand it to cover a pid range and reuse it for restore
    SprIterator(PageID pid, lsn_t firstLSN, lsn_t lastLSN,
            bool prioritizeArchive = true);
    ~SprIterator();

    bool next(logrec_t*& lr);

    void apply(fixable_page_h& page);

private:

    char* buffer;
    size_t buffer_capacity;
    std::list<uint32_t> lr_offsets;
    std::list<uint32_t>::const_iterator lr_iter;
    std::unique_ptr<ArchiveScanner> archive_scan;
    // CS TODO unify ArchiveScanner and RunMerger
    std::unique_ptr<ArchiveScanner::RunMerger> merger;

    lsn_t last_lsn;
    unsigned replayed_count;
};

class restart_m
{
    friend class restart_thread_t;

public:
    restart_m(const sm_options&);
    ~restart_m();

    // Function used for concurrent operations, open system after Log Analysis
    // we need a child thread to carry out the REDO and UNDO operations
    // while concurrent user transactions are coming in
    void spawn_recovery_thread()
    {
        // CS TODO: concurrency?

        DBGOUT1(<< "Spawn child recovery thread");

        _restart_thread = new restart_thread_t;
        W_COERCE(_restart_thread->fork());
        w_assert1(_restart_thread);
    }

    void log_analysis();
    void redo_log_pass();
    void redo_page_pass();
    void undo_pass();

    chkpt_t* get_chkpt() { return &chkpt; }

private:

    // System state object, updated by log analysis
    chkpt_t chkpt;

    bool instantRestart;

    // Child thread, used only if open system after Log Analysis phase while REDO and UNDO
    // will be performed with concurrent user transactions
    restart_thread_t*           _restart_thread;

public:

    /**
     * \ingroup Single-Page-Recovery
     * Defined in log_spr.cpp.
     * @copydoc ss_m::dump_page_lsn_chain(std::ostream&, const PageID &, const lsn_t&)
     */
    static void dump_page_lsn_chain(std::ostream &o, const PageID &pid, const lsn_t &max_lsn);

private:
    // Function used for serialized operations, open system after the entire restart process finished
    // brief sub-routine of redo_pass() for logs that have pid.
    void                 _redo_log_with_pid(
                                logrec_t& r,                   // In: Incoming log record
                                PageID page_updated,
                                bool &redone,                  // Out: did REDO occurred?  Validation purpose
                                uint32_t &dirty_count);        // Out: dirty page count, validation purpose
};

// CS: documentation code copied from old log_spr.h
/**
 * \defgroup Single-Page-Recovery
 * \brief \b Single-Page-Recovery (\b SPR) is a novel feature to recover a single page
 * without going through the entire restart procedure.
 * \ingroup SSMLOG
 * \details
 * \section Benefits Key Benefits
 * Suppose the situation where we have 999,999 pages that are intact and flushed
 * since the previous checkpoint. Now, while running our database, we updated one page,
 * flushed it to disk, evicted from bufferpool, then later re-read it from disk, finding that
 * the page gets corrupted.
 *
 * The time to recover the single page in traditional database would be hours, analyzing the
 * logs, applying REDOs and UNDOs.
 * Single-Page-Recovery, on the other hand, finishes in milliseconds. It fetches a single page from the backup,
 * collects only relevant transactional logs for the page, and applies them to the page.
 *
 * \section Terminology Terminology
 * TODO: A few words below should have more mathematic definitions.
 * \li A \b page in this module refers to a fixable page. It does NOT refer to
 * other types of pages (e.g., page-allocation bitmap pages, store-node pages).
 *
 * \li \b Page-LSN of page X, or PageLSN(X), refers to the log sequence number
 * of the latest update to the logical page X, regardless of whether the page is currently
 * in the buffer pool or its cleanliness.
 *
 * \li \b Recorded-Page-LSN of page X in data source Y, or RecordedPageLSN(X, Y), refers to
 * the log sequence number of the latest update to a particular image of page X stored in
 * data source Y (e.g., bufferpool, media, backup as of Wed, backup as of Thr, etc)
 *
 * \li \b Expected-Minimum-LSN of page X, or EMLSN(X), refers to the Page-LSN of X at the
 * time of its latest eviction from the buffer pool when the page X was dirty. EM-LSN of X
 * is stored in X's parent page (in media or bufferpool), except in the case of a root page.
 *
 * \section Invariants Invariants
 * \li EMLSN(X) <= PageLSN(X)
 * \li EMLSN(X) = PageLSN(X) only when X had no updates since previous write-out.
 * \li EMLSN(X) > 0
 *
 * \section Algorithm Algorithm Overview
 * Single page recovery is invoked only when a page is brought into the buffer pool from media.
 * When we bring a page, X, into the buffer pool, we compare RecordedPageLSN(X, media)
 * to EMLSN(X) in the parent page image in bufferpool.
 * If we see that that EMLSN(X) > RecordedPageLSN(X, media),
 * then we will invoke single page recovery.
 *
 * Given a parent page P with a child page C, EMLSN(C) is updated only when the child page C is
 * evicted from the buffer pool. This is in keeping with the invariant EMLSN(C) <= PageLSN(C).
 * Given a parent page P with a child page C, updating EMLSN(C) is a logged system transaction
 * that does change PageLSN(P).
 *
 * The buffer pool will evict only complete subtrees. Evicting a page from the buffer pool
 * means that all of that page's children must have already been evicted.
 * \li The buffer pool evicts pages bottom-up (leaf pages first, then parents,
 * then grandparents, etc).
 * \li Single page recovery works top-down --- a page cannot be recovered until all of its
 * ancestors have been recovered.
 * \li Conversely, bringing a page into the buffer pool requires that all of that page's
 * ancestors must also be in the buffer pool.
 *
 * Initially, the log records to be recovered will fit in memory. Ultimately, we intend to
 * create a recovery index (so that the log records won't have to fit into memory).
 *
 * \section LSN-CHAIN Per-Page LSN Chain
 * To maintain the per-page LSN chain mentioned above, we have baseLogHeader#_page_prv
 * in logrec_t and multi_page_log_t#_page2_prv.
 * These properties are populated in log manager when we call
 * xct_t#give_logbuf(). We use these
 * fields to collect relevant logs in log_core#_collect_single_page_recovery_logs().
 *
 * \section EMLSN-BTREE EMLSN in B-tree pages
 * We also have EMLSN fields for all page pointers in B-tree pages, which is required
 * to tell "from where we should start collecting relevant per-page logs" in case of Single-Page-Recovery.
 * See the setters/getters of btree_page_h linked to this HTML.
 * We also run a system transaction to update the EMLSN whenever we evict the child page
 * from bufferpool. See page_evict_t and log_page_evict().
 *
 * \section References References
 * More details are given in the following papers.
 * \li G. Graefe et al., "Definition, Detection, and Recovery of Single-page Failures,
 *  a Fourth Class of Database Failures". VLDB'12.
 * \li G. Graefe et al., "Self-diagnosing and self-healing indexes". DBTest'12.
 * \li G. Graefe et al., "Foster B-trees". TODS'12.
 */

#endif
