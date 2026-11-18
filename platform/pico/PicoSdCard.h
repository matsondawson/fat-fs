#ifndef net_mdawson_platform_pico_PicoSdCard
#define net_mdawson_platform_pico_PicoSdCard

#include "net/mdawson/filesystem/SectorDevice.h"

#include "hardware/spi.h"

MFS_NAMESPACE_BEGIN

/**
 * SD card in SPI mode on an RP2040 or RP2350, read and write.
 *
 * The SPI instance and pins are chosen at run time, so one build of the library
 * serves any wiring. The pins must be ones the chosen SPI instance can use
 * (see the datasheet's GPIO function table); begin() checks this. CS is driven
 * as a plain GPIO, so any pin works for it.
 *
 *     PicoSdCard sd(spi1, 10, 11, 12, 13);   // SCK, MOSI, MISO, CS
 *     if (sd.begin()) { printf("%s\n", sd.lastError()); }
 *     Partition partition(sd);
 *     Fat fat(partition);
 *     if (partition.open(Partition::primary(0)) == 0 && fat.mount() == 0) { ... }
 *
 * Reads use a multiple block read (CMD18) that is left open, so reading the
 * sector after the last one read is just the next block of the stream, with no
 * command and no wait for the card to find it. Reading any other sector closes
 * the stream (CMD12) and opens a new one.
 *
 * Read ahead: after each sector, DMA reads the stream's next block into a buffer
 * in this object while the caller works, so reading the next sector is usually
 * just a copy. The card sends 0xFF until a block's start token, and DMA can
 * only start after it, so the token is waited for briefly; without it the next
 * sector is read as it would be without read ahead. Turn it off with
 * setReadAhead(false).
 *
 * DMA channels: begin() claims two unused channels from the pico-sdk
 * (dma_claim_unused_channel), so they can't clash with other code that claims
 * its channels, and the destructor releases them. If none are free, read ahead
 * is off; setDmaChannels() hands over channels the program claimed itself.
 * The driver doesn't use DMA interrupts: it polls the channels, so it leaves
 * DMA_IRQ_0 and DMA_IRQ_1 and their handlers alone. DMA only writes into this
 * object's own buffer, never into a caller's.
 *
 * Writes are single block (CMD24) and have finished when the call returns.
 * flush() waits for any read ahead and closes the stream, leaving the card idle
 * and deselected.
 *
 * The card must be the only device on its SPI bus while the driver is in use:
 * the bus is configured once, by begin(), and between reads the card stays
 * selected with a read open and DMA may be running on the bus. Use the driver
 * from one core. CRCs are not checked, as is usual for SPI mode, so keep the
 * wiring short enough for the clock you choose.
 */
class PicoSdCard : public SectorDevice
{
public:
	/** Clock while the card starts; the SD spec allows 100 to 400 kHz. */
	static const uint32_t INIT_HZ = 400000;
	/** The most every card supports before switching to high speed mode. */
	static const uint32_t DEFAULT_HZ = 25000000;

	/**
	 * @param spi spi0 or spi1
	 * @param sck, mosi, miso GPIOs carrying that SPI instance's SCK, TX and RX
	 * @param cs Any free GPIO, driven low to select the card
	 * @param hz SPI clock once the card has started; the hardware picks the
	 *           nearest rate it can make at or below this
	 */
	PicoSdCard(spi_inst_t *spi, uint32_t sck, uint32_t mosi, uint32_t miso, uint32_t cs,
	           uint32_t hz = DEFAULT_HZ);
	/** Waits for any read ahead, closes the stream and releases DMA channels it claimed. */
	~PicoSdCard() override;
	PicoSdCard(const PicoSdCard&) = delete;
	PicoSdCard& operator=(const PicoSdCard&) = delete;

	struct Stats {
		uint32_t reads;          // sectors read
		uint32_t streamStarts;   // multiple block reads opened (CMD18)
		uint32_t readAheadHits;  // reads served from the read ahead buffer
	};

	/**
	 * Optional card detect switch. Without one the card is assumed present.
	 * @param pin GPIO the socket's detect switch is on, or -1 for none
	 * @param activeLow true if the pin reads low with a card in; the pin's pull
	 *                  up (or down for active high) is enabled
	 */
	void setCardDetect(int pin, bool activeLow = true);

	/**
	 * Read ahead with DMA (default on). Turning it off takes effect at the next
	 * read; turning it on needs DMA channels, which begin() claims if it has none.
	 */
	void setReadAhead(bool readAhead) { m_readAheadWanted = readAhead; }

	/**
	 * Use these DMA channels for read ahead instead of claiming two. The program
	 * owns them: it claims them first and the driver never releases them. Call
	 * before begin(); -1, -1 goes back to claiming.
	 */
	void setDmaChannels(int rx, int tx);

	/**
	 * Configure the pins and SPI, and start the card. Call again after a card
	 * swap, or to retry.
	 * @return 0, SectorDevice_notPresent (no card), or SectorDevice_notReady
	 *         (it didn't start, or the pins are wrong); lastError() says why.
	 */
	MFS_NODISCARD md_error_t begin();

	/** Why begin() or the last read or write failed, or "" if it didn't. */
	const char *lastError() const { return m_lastError; }

	/** Whether begin() succeeded and the card hasn't been removed since. */
	bool isReady() const { return m_ready; }
	/** Whether the detect switch sees a card; always true without one. */
	bool isCardPresent() const;
	/** Card capacity in 512 byte sectors, read by begin(). */
	sector_t getSectorCount() const { return m_sectorCount; }
	/** Whether the card is SDHC/SDXC (block addressed) rather than standard capacity. */
	bool isHighCapacity() const { return m_blockAddressed; }
	/** SPI clock actually in use after begin(). */
	uint32_t getBaudRate() const { return m_actualHz; }
	/** Whether begin() turned read ahead on (wanted, and DMA channels available). */
	bool isReadAheadOn() const { return m_rxChannel >= 0 && m_readAheadWanted; }

	const Stats &getStats() const { return m_stats; }
	void resetStats();

	MFS_NODISCARD md_error_t read(uint8_t *data, sector_t sector) override;
	MFS_NODISCARD md_error_t write(const uint8_t *data, sector_t sector) override;
	uint32_t getBytesPerSector() const override { return 512; }
	/** Waits for any read ahead and closes the read stream, leaving the card idle. */
	MFS_NODISCARD md_error_t flush() override;

private:
	spi_inst_t *m_spi;
	uint32_t m_sck, m_mosi, m_miso, m_cs;
	uint32_t m_hz;
	uint32_t m_actualHz;
	int m_detectPin;
	bool m_detectActiveLow;

	bool m_ready;
	bool m_blockAddressed;
	sector_t m_sectorCount;
	const char *m_lastError;

	// A multiple block read is open, with the card selected, and its next block is m_nextSector
	bool m_streaming;
	sector_t m_nextSector;
	// A byte other than 0xFF found where the next block's start token should be
	uint8_t m_pendingToken;

	bool m_readAheadWanted;
	// DMA is reading block m_nextSector into m_readAhead
	bool m_readingAhead;
	int m_rxChannel, m_txChannel;
	// The channels were claimed by the driver, which releases them
	bool m_claimedChannels;
	// A block and its 2 byte CRC
	alignas(4) uint8_t m_readAhead[514];

	Stats m_stats;

	uint8_t transfer(uint8_t out = 0xFF);
	void select();
	void deselect();
	bool waitReady(uint32_t timeoutMs);
	uint8_t command(uint8_t index, uint32_t argument);
	uint8_t appCommand(uint8_t index, uint32_t argument);
	bool readData(uint8_t *buffer, uint32_t length, uint32_t timeoutMs);
	bool readCapacity();
	bool readBlock(uint8_t *buffer);
	bool stopStream();
	void startReadAhead();
	void waitReadAhead();
	/** Finishes any read ahead and closes the stream. @return false if the card didn't stop. */
	bool idle();
	void releaseChannels();
	MFS_NODISCARD md_error_t checkAccess(sector_t sector);
	MFS_NODISCARD md_error_t fail(md_error_t error, const char *why);
};

MFS_NAMESPACE_END

#endif
