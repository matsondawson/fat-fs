#include "PicoSdCard.h"

#include "net/mdawson/filesystem/GlobalError.h"

#include "hardware/gpio.h"
#include "pico/time.h"

MFS_NAMESPACE_BEGIN

namespace {

const uint8_t CMD0_GO_IDLE = 0;
const uint8_t CMD8_SEND_IF_COND = 8;
const uint8_t CMD9_SEND_CSD = 9;
const uint8_t CMD13_SEND_STATUS = 13;
const uint8_t CMD16_SET_BLOCKLEN = 16;
const uint8_t CMD17_READ_SINGLE_BLOCK = 17;
const uint8_t CMD24_WRITE_BLOCK = 24;
const uint8_t CMD55_APP_CMD = 55;
const uint8_t CMD58_READ_OCR = 58;
const uint8_t ACMD41_SEND_OP_COND = 41;

const uint8_t R1_IDLE = 0x01;
const uint8_t R1_ILLEGAL_COMMAND = 0x04;
const uint8_t DATA_START_TOKEN = 0xFE;
const uint8_t DATA_RESPONSE_MASK = 0x1F;
const uint8_t DATA_ACCEPTED = 0x05;

// Longest times the SD spec allows: a read's start token within 100 ms, a write
// busy within 250 ms (SDHC 500 ms), starting up within 1 s.
const uint32_t READ_TIMEOUT_MS = 300;
const uint32_t WRITE_TIMEOUT_MS = 600;
const uint32_t INIT_TIMEOUT_MS = 2000;

// Which SPI instance and function a GPIO has: the pattern repeats every 8 pins,
// SPI0 then SPI1, on both the RP2040 and RP2350.
enum SpiFunction { SPI_RX = 0, SPI_CSN = 1, SPI_SCK = 2, SPI_TX = 3 };

bool pinIs(uint32_t pin, spi_inst_t *spi, SpiFunction function) {
	if (pin >= NUM_BANK0_GPIOS) {
		return false;
	}
	return ((pin >> 3) & 1) == spi_get_index(spi) && (pin & 3) == function;
}

} // namespace

PicoSdCard::PicoSdCard(spi_inst_t *spi, uint32_t sck, uint32_t mosi, uint32_t miso, uint32_t cs, uint32_t hz)
	: m_spi(spi), m_sck(sck), m_mosi(mosi), m_miso(miso), m_cs(cs), m_hz(hz), m_actualHz(0),
	  m_detectPin(-1), m_detectActiveLow(true),
	  m_ready(false), m_blockAddressed(false), m_sectorCount(0), m_lastError("") {
}

void PicoSdCard::setCardDetect(int pin, bool activeLow) {
	m_detectPin = pin;
	m_detectActiveLow = activeLow;
}

bool PicoSdCard::isCardPresent() const {
	if (m_detectPin < 0) {
		return true;
	}
	return gpio_get((uint)m_detectPin) != m_detectActiveLow;
}

md_error_t PicoSdCard::fail(md_error_t error, const char *why) {
	m_lastError = why;
	return error;
}

uint8_t PicoSdCard::transfer(uint8_t out) {
	uint8_t in;
	spi_write_read_blocking(m_spi, &out, &in, 1);
	return in;
}

void PicoSdCard::select() {
	gpio_put(m_cs, false);
}

void PicoSdCard::deselect() {
	gpio_put(m_cs, true);
	// The card only releases MISO after a clock with CS high
	transfer();
}

// The card holds MISO low while busy
bool PicoSdCard::waitReady(uint32_t timeoutMs) {
	absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
	while (transfer() != 0xFF) {
		if (time_reached(deadline)) {
			return false;
		}
	}
	return true;
}

// Sends a command with the card selected, leaving it selected to read the rest of
// the response. Returns the R1 response, with bit 7 set if there was none.
uint8_t PicoSdCard::command(uint8_t index, uint32_t argument) {
	select();
	if (!waitReady(WRITE_TIMEOUT_MS)) {
		return 0xFF;
	}
	// Only these two are CRC checked until CRC checking is turned on, which it never is
	uint8_t crc = index == CMD0_GO_IDLE ? 0x95 : index == CMD8_SEND_IF_COND ? 0x87 : 0x01;
	uint8_t frame[6] = {
		(uint8_t)(0x40 | index),
		(uint8_t)(argument >> 24), (uint8_t)(argument >> 16), (uint8_t)(argument >> 8), (uint8_t)argument,
		crc
	};
	spi_write_blocking(m_spi, frame, sizeof(frame));
	uint8_t response = 0xFF;
	for (int i = 0; i < 10 && (response & 0x80); i++) {
		response = transfer();
	}
	return response;
}

uint8_t PicoSdCard::appCommand(uint8_t index, uint32_t argument) {
	command(CMD55_APP_CMD, 0);
	deselect();
	return command(index, argument);
}

// Reads a data block after a read command: its start token, the data and the CRC
bool PicoSdCard::readData(uint8_t *buffer, uint32_t length, uint32_t timeoutMs) {
	absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
	uint8_t token;
	while ((token = transfer()) == 0xFF) {
		if (time_reached(deadline)) {
			return false;
		}
	}
	if (token != DATA_START_TOKEN) {
		return false;
	}
	spi_read_blocking(m_spi, 0xFF, buffer, length);
	// The CRC, not checked
	transfer();
	transfer();
	return true;
}

// Reads the card's size from its CSD register
bool PicoSdCard::readCapacity() {
	uint8_t csd[16];
	uint8_t response = command(CMD9_SEND_CSD, 0);
	bool ok = response == 0 && readData(csd, sizeof(csd), READ_TIMEOUT_MS);
	deselect();
	if (!ok) {
		return false;
	}
	switch (csd[0] >> 6) {
	case 0: {
		// CSD version 1: (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN bytes
		uint32_t readBlLen = csd[5] & 0x0F;
		uint32_t cSize = ((uint32_t)(csd[6] & 0x03) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
		uint32_t cSizeMult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
		uint64_t bytes = (uint64_t)(cSize + 1) << (cSizeMult + 2 + readBlLen);
		m_sectorCount = (sector_t)(bytes / 512);
		return true;
	}
	case 1: {
		// CSD version 2 (SDHC/SDXC): (C_SIZE + 1) * 512 KB
		uint32_t cSize = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
		uint64_t sectors = (uint64_t)(cSize + 1) * 1024;
		// sector_t is 32 bits, so the most it can reach is 2 TB
		m_sectorCount = sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : (sector_t)sectors;
		return true;
	}
	default:
		return false;
	}
}

md_error_t PicoSdCard::begin() {
	m_ready = false;
	m_lastError = "";
	m_sectorCount = 0;

	if (m_spi != spi0 && m_spi != spi1) {
		return fail(SectorDevice_notReady, "SPI instance must be spi0 or spi1");
	}
	if (!pinIs(m_sck, m_spi, SPI_SCK)) {
		return fail(SectorDevice_notReady, "SCK pin isn't an SCK pin of this SPI instance");
	}
	if (!pinIs(m_mosi, m_spi, SPI_TX)) {
		return fail(SectorDevice_notReady, "MOSI pin isn't a TX pin of this SPI instance");
	}
	if (!pinIs(m_miso, m_spi, SPI_RX)) {
		return fail(SectorDevice_notReady, "MISO pin isn't an RX pin of this SPI instance");
	}
	if (m_cs >= NUM_BANK0_GPIOS) {
		return fail(SectorDevice_notReady, "CS pin doesn't exist");
	}

	if (m_detectPin >= 0) {
		gpio_init((uint)m_detectPin);
		gpio_set_dir((uint)m_detectPin, GPIO_IN);
		if (m_detectActiveLow) {
			gpio_pull_up((uint)m_detectPin);
		} else {
			gpio_pull_down((uint)m_detectPin);
		}
		// Let the pull settle before reading it
		sleep_us(10);
		if (!isCardPresent()) {
			return fail(SectorDevice_notPresent, "No card in the socket");
		}
	}

	gpio_init(m_cs);
	gpio_set_dir(m_cs, GPIO_OUT);
	gpio_put(m_cs, true);
	spi_init(m_spi, INIT_HZ);
	spi_set_format(m_spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
	gpio_set_function(m_sck, GPIO_FUNC_SPI);
	gpio_set_function(m_mosi, GPIO_FUNC_SPI);
	gpio_set_function(m_miso, GPIO_FUNC_SPI);
	// MISO floats when no card drives it
	gpio_pull_up(m_miso);

	// At least 74 clocks with CS high, to enter native mode
	for (int i = 0; i < 10; i++) {
		transfer();
	}

	uint8_t response = 0xFF;
	for (int attempt = 0; attempt < 20 && response != R1_IDLE; attempt++) {
		response = command(CMD0_GO_IDLE, 0);
		deselect();
	}
	if (response != R1_IDLE) {
		return fail(m_detectPin >= 0 ? SectorDevice_notReady : SectorDevice_notPresent,
		            "No card responding (CMD0)");
	}

	// Version 2 cards echo the check pattern, older ones reject the command
	response = command(CMD8_SEND_IF_COND, 0x1AA);
	bool version2 = false;
	if (!(response & R1_ILLEGAL_COMMAND)) {
		uint32_t echo = 0;
		for (int i = 0; i < 4; i++) {
			echo = echo << 8 | transfer();
		}
		if ((echo & 0xFFF) != 0x1AA) {
			deselect();
			return fail(SectorDevice_notReady, "Card failed its voltage check (CMD8)");
		}
		version2 = true;
	}
	deselect();

	// Asks version 2 cards to allow block addressing, and waits for the card to finish starting
	absolute_time_t deadline = make_timeout_time_ms(INIT_TIMEOUT_MS);
	for (;;) {
		response = appCommand(ACMD41_SEND_OP_COND, version2 ? 0x40000000 : 0);
		deselect();
		if (response == 0) {
			break;
		}
		if (time_reached(deadline)) {
			return fail(SectorDevice_notReady, "Card didn't finish starting (ACMD41)");
		}
		sleep_ms(10);
	}

	m_blockAddressed = false;
	if (version2) {
		response = command(CMD58_READ_OCR, 0);
		uint32_t ocr = 0;
		for (int i = 0; i < 4; i++) {
			ocr = ocr << 8 | transfer();
		}
		deselect();
		if (response != 0) {
			return fail(SectorDevice_notReady, "Couldn't read the card's capacity type (CMD58)");
		}
		m_blockAddressed = (ocr & 0x40000000) != 0;
	}
	if (!m_blockAddressed) {
		response = command(CMD16_SET_BLOCKLEN, 512);
		deselect();
		if (response != 0) {
			return fail(SectorDevice_notReady, "Couldn't set the block size (CMD16)");
		}
	}

	if (!readCapacity()) {
		return fail(SectorDevice_notReady, "Couldn't read the card's size (CMD9)");
	}

	m_actualHz = spi_set_baudrate(m_spi, m_hz);
	m_ready = true;
	return 0;
}

md_error_t PicoSdCard::checkAccess(sector_t sector) {
	if (m_detectPin >= 0 && !isCardPresent()) {
		m_ready = false;
		return fail(SectorDevice_notPresent, "Card removed");
	}
	if (!m_ready) {
		return fail(SectorDevice_notReady, "Card not started; call begin()");
	}
	if (sector >= m_sectorCount) {
		return fail(SectorDevice_sectorOutOfRange, "Sector past the end of the card");
	}
	return 0;
}

md_error_t PicoSdCard::read(uint8_t *data, sector_t sector) {
	md_error_t error = checkAccess(sector);
	if (error) {
		return error;
	}
	uint8_t response = command(CMD17_READ_SINGLE_BLOCK, m_blockAddressed ? sector : sector * 512);
	bool ok = response == 0 && readData(data, 512, READ_TIMEOUT_MS);
	deselect();
	if (!ok) {
		return fail(SectorDevice_ioError, response == 0 ? "Read data didn't arrive (CMD17)" : "Read refused (CMD17)");
	}
	m_lastError = "";
	return 0;
}

md_error_t PicoSdCard::write(const uint8_t *data, sector_t sector) {
	md_error_t error = checkAccess(sector);
	if (error) {
		return error;
	}
	uint8_t response = command(CMD24_WRITE_BLOCK, m_blockAddressed ? sector : sector * 512);
	if (response != 0) {
		deselect();
		return fail(SectorDevice_ioError, "Write refused (CMD24)");
	}
	// A gap byte, the start token, the data and a dummy CRC
	transfer();
	transfer(DATA_START_TOKEN);
	spi_write_blocking(m_spi, data, 512);
	transfer();
	transfer();
	uint8_t dataResponse = transfer() & DATA_RESPONSE_MASK;
	// Programming finishes while the card holds MISO low
	bool finished = waitReady(WRITE_TIMEOUT_MS);
	deselect();
	if (dataResponse != DATA_ACCEPTED) {
		return fail(SectorDevice_ioError, "Card rejected the written data");
	}
	if (!finished) {
		return fail(SectorDevice_ioError, "Write didn't finish in time");
	}
	// The data response doesn't cover programming errors, so ask for the status
	response = command(CMD13_SEND_STATUS, 0);
	uint8_t status = transfer();
	deselect();
	if (response != 0 || status != 0) {
		return fail(SectorDevice_ioError, "Card reported a write error (CMD13)");
	}
	m_lastError = "";
	return 0;
}

MFS_NAMESPACE_END
