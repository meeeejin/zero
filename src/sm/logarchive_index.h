#ifndef LOGARCHIVE_INDEX_H
#define LOGARCHIVE_INDEX_H

#include <vector>
#include <list>

#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

#include "basics.h"
#include "lsn.h"
#include "sm_options.h"

// forward declaration
class ArchiveIndex;

/** \brief Encapsulates all file and I/O operations on the log archive
 *
 * The directory object serves the following purposes:
 * - Inspecting the existing archive files at startup in order to determine
 *   the last LSN persisted (i.e., from where to resume archiving) and to
 *   delete incomplete or already merged (TODO) files that can result from
 *   a system crash.
 * - Support run generation by providing operations to open a new run,
 *   append blocks of data to the current run, and closing the current run
 *   by renaming its file with the given LSN boundaries.
 * - Support scans by opening files given their LSN boundaries (which are
 *   determined by the archive index), reading arbitrary blocks of data
 *   from them, and closing them.
 * - In the near future, it should also support the new (i.e.,
 *   instant-restore-enabled) asynchronous merge daemon (TODO).
 * - Support auxiliary file-related operations that are used, e.g., in
 *   tests and experiments.  Currently, the only such operation is
 *   parseLSN.
 *
 * \author Caetano Sauer
 */
class ArchiveDirectory {
public:
    ArchiveDirectory(const sm_options& options);
    virtual ~ArchiveDirectory();

    struct RunFileStats {
        lsn_t beginLSN;
        lsn_t endLSN;
        unsigned level;
    };

    ArchiveIndex* getIndex() { return archIndex; }
    size_t getBlockSize() const { return blockSize; }
    std::string getArchDir() const { return archdir; }
    unsigned getMaxLevel() const { return maxLevel; }

    lsn_t getLastLSN();

    // run generation methods
    rc_t append(char* data, size_t length, unsigned level);
    rc_t closeCurrentRun(lsn_t runEndLSN, unsigned level);

    // run scanning methods
    rc_t openForScan(int& fd, lsn_t runBegin, lsn_t runEnd, unsigned level);
    rc_t readBlock(int fd, char* buf, size_t& offset, size_t readSize = 0);
    rc_t closeScan(int& fd);

    void listFiles(std::vector<std::string>& list, int level = -1);
    void listFileStats(std::list<RunFileStats>& list, int level = -1);
    void deleteAllRuns();

    size_t getSkipLogrecSize() const;

    static bool parseRunFileName(string fname, RunFileStats& fstats);
    static size_t getFileSize(int fd);

private:
    ArchiveIndex* archIndex;
    std::string archdir;
    std::vector<int> appendFd;
    std::vector<off_t> appendPos;
    size_t blockSize;
    unsigned maxLevel;

    fs::path archpath;

    // closeCurrentRun needs mutual exclusion because it is called by both
    // the writer thread and the archiver thread in processFlushRequest
    pthread_mutex_t mutex;

    fs::path make_run_path(lsn_t begin, lsn_t end, unsigned level = 1) const;
    fs::path make_current_run_path() const;
    rc_t openNewRun(unsigned level);

public:
    const static string RUN_PREFIX;
    const static string CURR_RUN_FILE;
    const static string CURR_MERGE_FILE;
    const static string run_regex;
    const static string current_regex;
};

/** \brief Simple implementation of a (naive) log archive index.
 *
 * No caching and one single mutex for all operations.  When log archiver
 * is initialized, the information of every run is loaded in main memory.
 * This class is still under test and development, so more documentation
 * should be added later (TODO)
 *
 * \author Caetano Sauer
 */
class ArchiveIndex {
public:
    ArchiveIndex(size_t blockSize, size_t bucketSize = 0);
    virtual ~ArchiveIndex();

    struct ProbeResult {
        PageID pidBegin;
        PageID pidEnd;
        lsn_t runBegin;
        lsn_t runEnd;
        unsigned level;
        size_t offset;
        size_t runIndex;
    };

    void init();

    void newBlock(const vector<pair<PageID, size_t> >& buckets, unsigned level);

    rc_t finishRun(lsn_t first, lsn_t last, int fd, off_t offset, unsigned level);
    void probe(std::vector<ProbeResult>& probes,
            PageID startPID, PageID endPID, lsn_t startLSN);

    rc_t getBlockCounts(int fd, size_t* indexBlocks, size_t* dataBlocks);
    rc_t loadRunInfo(int fd, const ArchiveDirectory::RunFileStats&);
    void appendNewEntry(unsigned level);

    lsn_t getLastLSN(unsigned level = 1);
    lsn_t getFirstLSN(unsigned level);

    size_t getBucketSize() { return bucketSize; }
    size_t getRunCount(unsigned level) {
        if (level > maxLevel) { return 0; }
        return runs[level].size();
    }

    void dumpIndex(ostream& out);

private:
    struct BlockEntry {
        size_t offset;
        PageID pid;
    };
    struct BlockHeader {
        uint32_t entries;
        uint32_t blockNumber;
    };
    struct RunInfo {
        lsn_t firstLSN;
        // lastLSN must be equal to firstLSN of the following run.  We keep
        // it redundantly so that index probes don't have to look beyond
        // the last finished run. We used to keep a global lastLSN field in
        // the index, but there can be a race between the writer thread
        // inserting new runs and probes on the last finished, so it was
        // removed.
        lsn_t lastLSN;

        // Simple min-max filter for page IDs (min is in 1st entry)
        PageID lastPID;

        std::vector<BlockEntry> entries;

        bool operator<(const RunInfo& other) const
        {
            return firstLSN < other.firstLSN;
        }
    };

    pthread_mutex_t mutex;
    size_t blockSize;

    // Run information for each level of the index
    std::vector<std::vector<RunInfo>> runs;

    // Last finished run on each level -- this is required because runs are
    // generated asynchronously, so that a new one may be appended to the
    // index before the last one is finished. Thus, when calling finishRun(),
    // we cannot simply take the last run in the vector.
    std::vector<int> lastFinished;

    /** Whether this index uses variable-sized buckets, i.e., entries in
     * the index refer to fixed ranges of page ID for which the amount of
     * log records is variable. The number gives the size of a bucket in
     * terms of number of page ID's (or segment size in the restore case).
     * If this is zero, then the index behaves like a B-tree, in which a
     * bucket corresponds to a block, therefore having fixed sizes (but
     * variable number of log records, obviously).
     */
    size_t bucketSize;

    unsigned maxLevel;

    size_t findRun(lsn_t lsn, unsigned level);
    void probeInRun(ProbeResult&);
    // binary search
    size_t findEntry(RunInfo* run, PageID pid,
            int from = -1, int to = -1);
    rc_t serializeRunInfo(RunInfo&, int fd, off_t);

};

#endif
