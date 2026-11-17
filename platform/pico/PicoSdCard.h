#ifndef net_mdawson_platform_pico_PicoSdCard
#define net_mdawson_platform_pico_PicoSdCard

#include "net/mdawson/filesystem/SectorDevice.h"

#include "hardware/spi.h"

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
 *     Partition partition(sd, Partition::primary(0));
 *     Fat fat(partition);
 *
 * Each read or write is one single block command (CMD17 / CMD24) that has
 * finished when the call returns, so flush() has nothing to do. Put a
 * CachedSectorDevice in front for fewer card accesses.
 *
 * The card must be the only device on its SPI bus while the driver is in use:
 * the bus is configured once, by begin(). CRCs are not checked, as is usual for
 * SPI mode, so keep the wiring short enough for the clock you choose.
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

	/**
	 * Optional card detect switch. Without one the card is assumed present.
	 * @param pin GPIO the socket's detect switch is on, or -1 for none
	 * @param activeLow true if the pin reads low with a card in; the pin's pull
	 *                  up (or down for active high) is enabled
	 */
	void setCardDetect(int pin, bool activeLow = true);

	/**
	 * Configure the pins and SPI, and start the card. Call again after a card
	 * swap, or to retry.
	 * @return 0, SectorDevice_notPresent (no card), or SectorDevice_notReady
	 *         (it didn't start, or the pins are wrong); lastError() says why.
	 */
	md_error_t begin();

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

	virtual md_error_t read(uint8_t *data, sector_t sector);
	virtual md_error_t write(const uint8_t *data, sector_t sector);
	virtual uint32_t getBytesPerSector() const { return 512; }

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

	uint8_t transfer(uint8_t out = 0xFF);
	void select();
	void deselect();
	bool waitReady(uint32_t timeoutMs);
	uint8_t command(uint8_t index, uint32_t argument);
	uint8_t appCommand(uint8_t index, uint32_t argument);
	bool readData(uint8_t *buffer, uint32_t length, uint32_t timeoutMs);
	bool readCapacity();
	md_error_t checkAccess(sector_t sector);
	md_error_t fail(md_error_t error, const char *why);
};

#endif
