/*
 * (c) Copyright 2011-2013, Hewlett-Packard Development Company, LP
 */

#define SM_SOURCE
#include "sm_int_1.h"

#include "stnode_page.h"

#include "bf_fixed.h"


stnode_page_h::stnode_page_h(generic_page* s, const lpid_t& pid):
    generic_page_h(s, pid, t_stnode_p)
{}   



stnode_cache_t::stnode_cache_t(vid_t vid, bf_fixed_m* special_pages):
    _vid(vid), 
    _special_pages(special_pages),
    _stnode_page(special_pages->get_pages() + special_pages->get_page_cnt()-1)
{
    w_assert1(_stnode_page.vid() == _vid);
}


shpid_t stnode_cache_t::get_root_pid(snum_t store) const {
    w_assert1(store < stnode_page_h::max);

    // CRITICAL_SECTION (cs, _spin_lock);
    // Commented out to improve scalability, as this is called for
    // EVERY operation.  NOTE this protection is not needed because
    // this is unsafe only when there is a concurrent DROP INDEX (or
    // DROP TABLE).  It should be protected by intent locks (if it's
    // no-lock mode... it's user's responsibility).
    //
    // JIRA: ZERO-168 notes that DROP INDEX/TABLE currently are not
    // implemented and to fix this routine once they are.
    return _stnode_page.get(store).root;
}

bool stnode_cache_t::is_allocated(snum_t store) const {
    stnode_t s;
    get_stnode(store, s);
    return s.is_allocated();
}

void stnode_cache_t::get_stnode(snum_t store, stnode_t &stnode) const {
    w_assert1(store < stnode_page_h::max);
    CRITICAL_SECTION (cs, _spin_lock);
    stnode = _stnode_page.get(store);
}


snum_t stnode_cache_t::get_min_unused_store_ID() const {
    // This method is not very efficient, but is rarely called (i.e.,
    // only when creating new stores).

    CRITICAL_SECTION (cs, _spin_lock);
    // Let's start from 1, not 0.  All user store ID's will begin with 1.
    // Store-ID 0 will be a special store-ID for stnode_page/alloc_page's
    for (size_t i = 1; i < stnode_page_h::max; ++i) {
        if (!_stnode_page.get(i).is_allocated()) {
            return i;
        }
    }
    return stnode_page_h::max;
}

std::vector<snum_t> stnode_cache_t::get_all_used_store_ID() const {
    std::vector<snum_t> ret;

    CRITICAL_SECTION (cs, _spin_lock);
    for (size_t i = 1; i < stnode_page_h::max; ++i) {
        if (_stnode_page.get(i).is_allocated()) {
            ret.push_back((snum_t) i);
        }
    }
    return ret;
}


rc_t
stnode_cache_t::store_operation(store_operation_param param) {
    w_assert1(param.snum() < stnode_page_h::max);

    CRITICAL_SECTION (cs, _spin_lock);
    stnode_t stnode = _stnode_page.get(param.snum()); // copy out current value.

    switch (param.op())  {
        case smlevel_0::t_delete_store: 
            {
                w_assert0(false); // this case currently unused; see JIRA: ZERO-168 
                stnode.root        = 0;
                stnode.flags       = smlevel_0::st_unallocated;
                stnode.deleting    = smlevel_0::t_not_deleting_store;
            }
            break;
        case smlevel_0::t_create_store:
            {
                w_assert1(stnode.root == 0);
                w_assert1(param.new_store_flags() != smlevel_0::st_unallocated);

                stnode.root        = 0;
                stnode.flags       = param.new_store_flags();
                stnode.deleting    = smlevel_0::t_not_deleting_store;
            }
            DBGOUT3 ( << "t_create_store:" << param.snum());
            break;
        case smlevel_0::t_set_deleting:
            {
                w_assert0(false); // this case currently unused; see JIRA: ZERO-168 
                // Bogus assertion:
                // If we crash/restart between the time the xct gets
                // into xct_freeing_space and the time xct_end is
                // logged, this store operation might already have
                // been done,
                // w_assert3(stnode.deleting != param.new_deleting_value());
                w_assert3(param.old_deleting_value() == smlevel_0::t_unknown_deleting
                        || stnode.deleting == param.old_deleting_value());

                param.set_old_deleting_value(
                    (store_operation_param::store_deleting_t)stnode.deleting);

                stnode.deleting    = param.new_deleting_value();
            }
            break;
        case smlevel_0::t_set_store_flags:
            {
                w_assert1(param.new_store_flags() != smlevel_0::st_unallocated);

                if (stnode.flags == param.new_store_flags())  {
                    // xct may have converted file type to regular and
                    // then the automatic conversion at commit from
                    // insert_file to regular needs to be ignored
                    DBG(<<"store flags already set");
                    return RCOK;
                } else  {
                    w_assert3(param.old_store_flags() == smlevel_0::st_unallocated
                              || stnode.flags == param.old_store_flags());

                    param.set_old_store_flags(
                            (store_operation_param::store_flag_t)stnode.flags);

                    stnode.flags = param.new_store_flags();
                }
            }
            break;
        case smlevel_0::t_set_root:
            {
                w_assert3(stnode.root == 0);
                w_assert3(param.root());

                stnode.root = param.root();
            }
            DBGOUT3 ( << "t_set_root:" << param.snum() << ". root=" << param.root());
            break;
        default:
            w_assert0(false);
    }

    // log it and apply the change to the stnode_page
    spinlock_read_critical_section cs2(&_special_pages->get_checkpoint_lock()); // Protect against checkpoint.  See bf_fixed_m comment.
    W_DO( log_store_operation(param) );
    _stnode_page.get(param.snum()) = stnode;
    _special_pages->get_dirty_flags()[_special_pages->get_page_cnt() - 1] = true;
    return RCOK;
}
