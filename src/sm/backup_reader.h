#ifndef BACKUP_READER_H
#define BACKUP_READER_H

#include "sm_base.h"
#include "generic_page.h"

#include <deque>

class vol_t;

/** \brief Interface for accessing a backup file during restore.
 *
 * This interface is used to abstract prefetched and purely on-demand access
 * to segments of a backup file.
 *
 * \author Caetano Sauer
 */
class BackupReader {
public:
    /** Reads a segment into an internal buffer and return a pointer to this
     * buffer. Usage pattern is similar to accessing pages in a buffer pool,
     * hence the name "fix".
     */
    virtual char* fix(unsigned segment) = 0;

    /** Signalizes that restore is done accessing a currently fixed segment.
     */
    virtual void unfix(unsigned segment) = 0;

    /** Requests prefetching of a segment which will be read later. If
     * prefetching is not supported, this is a no-op. A priority argument is
     * provided for implementations that support it. Higher number should mean
     * higher priority.
     */
    virtual void prefetch(unsigned /* segment */, int /* priority */)
    {
        return; // default: no prefetching
    }

    BackupReader(size_t bufferSize)
    {
        buffer = new char[bufferSize];
    }

    virtual ~BackupReader()
    {
        delete[] buffer;
    }

protected:
    char* buffer;
    shpid_t firstDataPid;
};

/** \brief Dummy backup reader that always returns the same unmodified buffer.
 *
 * Used for backup-less restore, i.e., using only the complete log history.
 */
class DummyBackupReader : public BackupReader {
public:
    DummyBackupReader(size_t segmentSize)
        : BackupReader(segmentSize * sizeof(generic_page)),
        segmentSize(segmentSize)
    {
    }

    virtual ~DummyBackupReader()
    {
    }

    virtual char* fix(unsigned)
    {
        memset(buffer, 0, segmentSize * sizeof(generic_page));
        return buffer;
    }

    virtual void unfix(unsigned)
    {
    }

    static const std::string IMPL_NAME;

private:
    size_t segmentSize;
};

/** \brief Simple on-demand backup reader without prefetching
 */
class BackupOnDemandReader : public BackupReader {
public:
    BackupOnDemandReader(vol_t* volume, size_t segmentSize);

    virtual ~BackupOnDemandReader()
    {
    }

    virtual char* fix(unsigned segment);

    virtual void unfix(unsigned segment);

protected:
    vol_t* volume;
    size_t segmentSize;

    // Used just for assertion in debug mode
    int fixedSegment;

public:
    static const std::string IMPL_NAME;
};

/** \brief Prefethcer for backup pages while restore is on progress.
 *
 * This class represents a thread that fetches segments from a backup file in
 * the background. The goal is to perform reads on the backup device in
 * parallel with the log archive device. In a Single-Pass Restore schedule, for
 * instance, the prefetcher allows restore to complete in the time it takes to
 * sequentially read the largest of either backup or log archive.
 *
 * The class acts like a buffer pool for segments. As requests come in,
 * segments are read into a free slot in the buffer. If it's full, the request
 * method returns false and the segment must be read on-demand instead of
 * prefetched.  The get() method will search the slot array for the given
 * segment and, if not found, perform an eviction at random (e.g., always the
 * first slot) and read the page. This should not happen often, as it defeats
 * the purpose of a prefetcher.  It is the responsibility of the scheduler to
 * perform requests such that this is avoided, i.e., to request prefetches in
 * approximately the same order as they will be actually read. From this
 * perspective, the size of the buffer can be seen as a "window of tolerance"
 * for requests that come out of order (i.e., read request not in the same
 * order as prefetch requests). In a perfect scenario, a buffer of just two
 * segments would be sufficient (one being currently restored plus one being
 * prefetched). However, this is not realistic in the case of complex schedules
 * such as different priorities or on-demand mixed with single-pass restore.
 *
 * The constructor argument numSegments determines the buffer size in number
 * of segments.  Similar to a buffer pool, one of the slots is always "fixed"
 * as the one being currently used as the restore workspace.
 *
 * \author Caetano Sauer
 */
class BackupPrefetcher : public smthread_t, public BackupReader {
public:
    BackupPrefetcher(vol_t* volume, size_t numSegments, size_t segmentSize);
    virtual ~BackupPrefetcher();

    /**
     * Default priority is zero, which makes request go to end of FIFO queue.
     * Any value larger than zero pushes it to the front of the queue.
     */
    virtual void prefetch(unsigned segment, int priority = 0);
    virtual char* fix(unsigned segment);
    virtual void unfix(unsigned segment);

    virtual void run();

private:
    vol_t* volume;

    /** Number of segments to hold in buffer */
    size_t numSegments;

    /** Size of a segment in pages **/
    size_t segmentSize;

    /** Size of a segment in Bytes **/
    size_t segmentSizeBytes;

    /** Array to keep track of fetched segments. Negative value indicates a
     * free slot
     */
    int* slots;

    /** A slot is fixed when it is being used by restore. Thus, it cannot be
     * evicted. Only one fixed slot is allowed at a time, i.e., restore is
     * sequential. If no slot is fixed, its value is negative.
     */
    int fixedSegment;

    bool fixWaiting;

    /** Queue of received requests */
    std::deque<unsigned> requests;

    /** Protect concurrent access to slot array and request queue */
    srwlock_t requestMutex;

    /** Condition variable to signal when prefetcher performs a read */
    pthread_cond_t readCond;
    pthread_mutex_t readCondMutex;

    /** Condition variable to signal when a prefetch is requested */
    pthread_cond_t prefetchCond;
    pthread_mutex_t prefetchCondMutex;

    /** Keep track of last evicted slot */
    size_t lastEvicted;

public:
    static const std::string IMPL_NAME;
};

#endif
