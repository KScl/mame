// license:BSD-3-Clause
// copyright-holders:R. Belmont
/*
    SD Card emulation, SPI interface only currently
    Emulation by R. Belmont

    This emulates an SDHC card, which means the block size is fixed at 512 bytes and makes things simpler.
    Adapting the code to also handle SD version 2 non-HC cards would be relatively straightforward as well;
    the block size defaults to 1 byte in that case but can be overridden with CMD16.

    Adding the native 4-bit-wide SD interface is also possible

    Multiple block read/write commands are not supported but would be straightforward to add.

    Refrences:
    https://www.sdcard.org/downloads/pls/ (Physical Layer Simplified Specification)
    http://www.dejazzer.com/ee379/lecture_notes/lec12_sd_card.pdf
    https://embdev.net/attachment/39390/TOSHIBA_SD_Card_Specification.pdf
    http://elm-chan.org/docs/mmc/mmc_e.html
*/

#include "emu.h"
#include "spi_sdcard.h"
#include "imagedev/harddriv.h"

#define LOG_GENERAL (1U << 0)
#define LOG_COMMAND (1U << 1)
#define LOG_SPI     (1U << 2)

//#define VERBOSE (LOG_COMMAND)
#define LOG_OUTPUT_FUNC osd_printf_info

#include "logmacro.h"

static constexpr u8 DATA_RESPONSE_OK        = 0x05;
static constexpr u8 DATA_RESPONSE_IO_ERROR  = 0x0d;

DEFINE_DEVICE_TYPE(SPI_SDCARD, spi_sdcard_device, "spi_sdcard", "SD Card (SPI Interface)")

spi_sdcard_device::spi_sdcard_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, uint32_t clock) :
	device_t(mconfig, type, tag, owner, clock),
	write_miso(*this),
	m_image(*this, "image"),
	m_harddisk(nullptr),
	m_in_latch(0), m_out_latch(0), m_cmd_ptr(0), m_state(0), m_out_ptr(0), m_out_count(0), m_ss(0), m_in_bit(0),
	m_cur_bit(0), m_write_ptr(0), m_bACMD(false)
{
}

spi_sdcard_device::spi_sdcard_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	spi_sdcard_device(mconfig, SPI_SDCARD, tag, owner, clock)
{
}

void spi_sdcard_device::device_start()
{
	write_miso.resolve_safe();
	save_item(NAME(m_in_latch));
	save_item(NAME(m_out_latch));
	save_item(NAME(m_cmd_ptr));
	save_item(NAME(m_state));
	save_item(NAME(m_out_ptr));
	save_item(NAME(m_out_count));
	save_item(NAME(m_ss));
	save_item(NAME(m_in_bit));
	save_item(NAME(m_cur_bit));
	save_item(NAME(m_write_ptr));
	save_item(NAME(m_cmd));
	save_item(NAME(m_data));
	save_item(NAME(m_bACMD));
}

void spi_sdcard_device::device_reset()
{
	m_harddisk = m_image->get_hard_disk_file();
}

void spi_sdcard_device::device_add_mconfig(machine_config &config)
{
	HARDDISK(config, m_image).set_interface("spi_sdcard");
}

void spi_sdcard_device::send_data(int count)
{
	m_out_ptr = 0;
	m_out_count = count;
}

void spi_sdcard_device::spi_clock_w(int state)
{
	// only respond if selected
	if (m_ss)
	{
		// We implmement SPI Mode 3 signalling, in which we latch the data on
		// rising clock edges, and shift the data on falling clock edges.
		// See http://www.dejazzer.com/ee379/lecture_notes/lec12_sd_card.pdf for details
		// on the 4 SPI signalling modes.  SD Cards can work in ether Mode 0 or Mode 3,
		// both of which shift on the falling edge and latch on the rising edge but
		// have opposite CLK polarity.

		if (state)
		{
			m_in_latch &= ~0x01;
			m_in_latch |= m_in_bit;
			LOGMASKED(LOG_SPI, "\tsdcard: L %02x (%d) (out %02x)\n", m_in_latch, m_cur_bit, m_out_latch);
			m_cur_bit++;
			if (m_cur_bit == 8)
			{
				LOGMASKED(LOG_SPI, "SDCARD: got %02x\n", m_in_latch);

				switch (m_state)
				{
					case SD_STATE_IDLE:
						for (int i = 0; i < 5; i++)
						{
							m_cmd[i] = m_cmd[i + 1];
						}
						m_cmd[5] = m_in_latch;

						if ((((m_cmd[0] & 0xc0) == 0x40) && (m_cmd[5] & 1)) && (m_out_count == 0))
						{
							do_command();
						}
						break;

					case SD_STATE_WRITE_WAITFE:
						if (m_in_latch == 0xfe)
						{
							m_state = SD_STATE_WRITE_DATA;
							m_out_latch = 0xff;
							m_write_ptr = 0;
						}
						break;

					case SD_STATE_WRITE_DATA:
						m_data[m_write_ptr++] = m_in_latch;
						if (m_write_ptr == 514)
						{
							u32 blk = (m_cmd[1] << 24) | (m_cmd[2] << 16) | (m_cmd[3] << 8) | m_cmd[4];
							LOGMASKED(LOG_GENERAL, "writing LBA %x, data %02x %02x %02x %02x\n", blk, m_data[0], m_data[1], m_data[2], m_data[3]);
							if (hard_disk_write(m_harddisk, blk, &m_data[0]))
							{
								m_data[0] = DATA_RESPONSE_OK;
							}
							else
							{
								m_data[0] = DATA_RESPONSE_IO_ERROR;
							}
							m_data[1] = 0x01;

							m_state = SD_STATE_IDLE;
							send_data(2);
						}
						break;
				}
			}
		}
		else
		{
			m_in_latch <<= 1;
			m_out_latch <<= 1;
			LOGMASKED(LOG_SPI, "\tsdcard: S %02x %02x (%d)\n", m_in_latch, m_out_latch, m_cur_bit);
			if (m_cur_bit == 8)
			{
				m_cur_bit = 0;
			}

			if (m_cur_bit == 0)
			{
				if (m_out_count > 0)
				{
					m_out_latch = m_data[m_out_ptr++];
					LOGMASKED(LOG_SPI, "SDCARD: latching %02x (start of shift)\n", m_out_latch);
					m_out_count--;
				}
			}

			write_miso(BIT(m_out_latch, 7)  ? ASSERT_LINE : CLEAR_LINE);
		}
	}
}

void spi_sdcard_device::do_command()
{
	LOGMASKED(LOG_COMMAND, "SDCARD: cmd %02d %02x %02x %02x %02x %02x\n", m_cmd[0] & 0x3f, m_cmd[1], m_cmd[2], m_cmd[3], m_cmd[4], m_cmd[5]);
	switch (m_cmd[0] & 0x3f)
	{
	case 0: // CMD0 - GO_IDLE_STATE
		if (m_harddisk)
		{
			m_data[0] = 0x01;
		}
		else
		{
			m_data[0] = 0x00;
		}
		send_data(1);
		break;

	case 8: // CMD8 - SEND_IF_COND (SD v2 only)
		m_data[0] = 0x01;
		m_data[1] = 0;
		m_data[2] = 0;
		m_data[3] = 0;
		m_data[4] = 0xaa;
		send_data(5);
		break;

	case 17: // CMD17 - READ_SINGLE_BLOCK
		if (m_harddisk)
		{
			m_data[0] = 0x00; // initial R1 response
			// data token occurs some time after the R1 response.  A2SD expects at least 1
			// byte of space between R1 and the data packet.
			m_data[2] = 0xfe; // data token
			u32 blk = (m_cmd[1] << 24) | (m_cmd[2] << 16) | (m_cmd[3] << 8) | m_cmd[4];
			LOGMASKED(LOG_GENERAL, "reading LBA %x\n", blk);
			hard_disk_read(m_harddisk, blk, &m_data[3]);
			send_data(3 + 512 + 2);
		}
		else
		{
			m_data[0] = 0xff; // show an error
			send_data(1);
		}
		break;

	case 24: // CMD24 - WRITE_BLOCK
		m_data[0] = 0;
		send_data(1);
		m_state = SD_STATE_WRITE_WAITFE;
		break;

	case 41:
		if (m_bACMD) // ACMD41 - SD_SEND_OP_COND
		{
			m_data[0] = 0;
		}
		else        // CMD41 - illegal
		{
			m_data[0] = 0xff;
		}
		send_data(1);
		break;

	case 55: // CMD55 - APP_CMD
		m_data[0] = 0x01;
		send_data(1);
		break;

	case 58: // CMD58 - READ_OCR
		m_data[0] = 0;
		m_data[1] = 0x40; // indicate SDHC support
		m_data[2] = 0;
		m_data[3] = 0;
		m_data[4] = 0;
		send_data(5);
		break;

	default:
		break;
	}

	// if this is command 55, that's a prefix indicating the next command is an "app command" or "ACMD"
	if ((m_cmd[0] & 0x3f) == 55)
	{
		m_bACMD = true;
	}
	else
	{
		m_bACMD = false;
	}
}
