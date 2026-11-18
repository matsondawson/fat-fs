#ifndef net_mdawson_filesystems_CachedSectorDevice
#define net_mdawson_filesystems_CachedSectorDevice

#include "net/mdawson/filesystem/SectorDevice.h"

#include <memory>

MFS_NAMESPACE_BEGIN

#ifdef MFS_BLOCK_CACHE

// Can be set from the build to change the default below.
#ifndef MFS_BLOCK_CACHE_DEFAULT_SLOTS
#define MFS_BLOCK_CACHE_DEFAULT_SLOTS 4
#endif

/** Default number of cached sectors: 4 x 512 bytes = 2KB, sensible for a Pico. */
constexpr uint32_t BLOCK_CACHE_DEFAULT_SLOTS = MFS_BLOCK_CACHE_DEFAULT_SLOTS;

/**
 * A sector cache that wraps another SectorDevice, so the driver is unchanged:
 *
 *   CachedSectorDevice cache(sd, 4);   // 4 sectors of cache
 *   Partition partition(cache);         // then partition.open(Partition::primary(0))
 *
 * - A fixed number of slots, allocated once in the constructor (no heap use after).
 * - Least recently used slot is evicted; each slot has a dirty flag.
 * - Write-back (default): dirty sectors reach the device only when evicted or on
 *   flush(). Write-through: every write goes straight to the device.
 * - Device errors are passed through. A failed write-back leaves the sector cached
 *   and dirty, and the operation that needed the slot returns the error.
 * - Optional read ahead: reading sector N from the device also reads N + 1 into
 *   the cache, and reading a sector that was read ahead reads the one after it,
 *   so a file read in order stays one sector ahead. A failed read ahead (such as
 *   past the end of the device) is ignored. It only saves time when the device
 *   reads consecutive sectors faster than separate ones; on its own it reads the
 *   same sectors, sooner, and can evict a sector that would have been used again.
 * - The destructor flushes.
 *
 * Only call invalidate() if the data in the cache may be thrown away (e.g. the card
 * was swapped); dirty sectors are discarded.
 */
class CachedSectorDevice : public SectorDevice
{
public:
	struct Stats {
		uint32_t hits;          // reads/writes served from the cache
		uint32_t misses;        // reads/writes that needed a slot
		uint32_t deviceReads;   // sectors read from the device
		uint32_t deviceWrites;  // sectors written to the device
		uint32_t readAheads;    // sectors read ahead (also counted in deviceReads)
		uint32_t readAheadHits; // reads served by a sector that was read ahead
	};

private:
	SectorDevice *m_device;
	uint32_t m_slotCount;
	bool m_writeThrough;
	bool m_readAhead;
	uint32_t m_tick;

	std::unique_ptr<sector_t[]> m_sectors;   // INVALID_SECTOR when slot unused
	std::unique_ptr<uint32_t[]> m_lastUsed;  // tick of last use, for LRU
	std::unique_ptr<bool[]> m_dirty;
	std::unique_ptr<bool[]> m_readAheadSlot; // read ahead and not yet read
	std::unique_ptr<uint8_t[]> m_data;       // m_slotCount * 512 bytes

	Stats m_stats;

	static constexpr sector_t INVALID_SECTOR = static_cast<sector_t>(-1);

	int32_t findSlot(sector_t sector) const;
	/** Find a slot to (re)use, writing back its dirty contents. slot is -1 on failure. */
	md_error_t claimSlot(int32_t &slot);
	md_error_t writeBack(uint32_t slot);
	uint8_t *slotData(uint32_t slot) const;
	/** Read sector into the cache if it isn't there, ignoring any error. */
	void readAhead(sector_t sector);

public:
	/**
	 * @param device The device to cache.
	 * @param slots Number of sectors to cache (at least 1).
	 * @param writeThrough true to write every sector to the device immediately.
	 * @param readAhead true to read the next sector into the cache as well (see above).
	 */
	explicit CachedSectorDevice(SectorDevice &device, uint32_t slots = BLOCK_CACHE_DEFAULT_SLOTS,
	                            bool writeThrough = false, bool readAhead = false);
	/** Flushes dirty sectors. */
	~CachedSectorDevice() override;
	CachedSectorDevice(const CachedSectorDevice&) = delete;
	CachedSectorDevice& operator=(const CachedSectorDevice&) = delete;

	md_error_t read(uint8_t* data, sector_t sector) override;
	md_error_t write(const uint8_t* data, sector_t sector) override;
	uint32_t getBytesPerSector() const override;

	/** Write every dirty sector to the device, then flush the device. */
	md_error_t flush() override;

	/** Drop every cached sector, discarding dirty data (e.g. after a card swap). */
	void invalidate();

	uint32_t getSlotCount() const { return m_slotCount; }
	bool isWriteThrough() const { return m_writeThrough; }
	bool isReadAhead() const { return m_readAhead; }
	/** Turn read ahead on or off; sectors already read ahead stay cached. */
	void setReadAhead(bool readAhead) { m_readAhead = readAhead; }
	uint32_t getDirtyCount() const;
	/** True if sector is cached (for tests). */
	bool isCached(sector_t sector) const { return findSlot(sector) >= 0; }

	const Stats &getStats() const { return m_stats; }
	void resetStats();
};

#endif // MFS_BLOCK_CACHE

MFS_NAMESPACE_END

#endif
