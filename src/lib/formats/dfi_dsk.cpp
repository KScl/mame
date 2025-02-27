// license:BSD-3-Clause
// copyright-holders:Olivier Galibert

/* DONE:
 * Support auto-identification heuristics for determining disk image speed,
   capture clock rate, and number of multireads per image.
 * TODO:
 * Scale captured data based on the guessed clock rate and samplerate to match
   the internal 200mhz representation
 * Handle 0xFF bytes properly
 * Correctly note exact index timing.
 */

#include "dfi_dsk.h"

#include "ioprocs.h"


#define NUMBER_OF_MULTIREADS 3
// thresholds for brickwall windowing
//define DFI_MIN_CLOCKS 65
// number_please apple2 wants 40 min
#define DFI_MIN_CLOCKS 60
//define MAX_CLOCKS 260
#define DFI_MAX_CLOCKS 270
#define MIN_THRESH (DFI_MIN_CLOCKS*(clock_rate/25000000))
#define MAX_THRESH (DFI_MAX_CLOCKS*(clock_rate/25000000))
// constants to help guess clockrate and rpm
// constant is 25mhz / 6 revolutions per second (360rpm) = 4166667 +- 2.5%
#define REV25_MIN 4062500
#define REV25_MAX 4270833
// define the following to show a histogram per track
//define TRACK_HISTOGRAM 1
#undef TRACK_HISTOGRAM

dfi_format::dfi_format() : floppy_image_format_t()
{
}

const char *dfi_format::name() const
{
	return "dfi";
}

const char *dfi_format::description() const
{
	return "DiscFerret flux dump format";
}

const char *dfi_format::extensions() const
{
	return "dfi";
}

bool dfi_format::supports_save() const
{
	return false;
}

int dfi_format::identify(util::random_read &io, uint32_t form_factor, const std::vector<uint32_t> &variants)
{
	char sign[4];
	size_t actual;
	io.read_at(0, sign, 4, actual);
	return memcmp(sign, "DFE2", 4) ? 0 : 100;
}

bool dfi_format::load(util::random_read &io, uint32_t form_factor, const std::vector<uint32_t> &variants, floppy_image *image)
{
	size_t actual;
	char sign[4];
	io.read_at(0, sign, 4, actual);
	if(memcmp(sign, "DFER", 4) == 0) {
		osd_printf_error("dfi_dsk: Old type Discferret image detected; the mess Discferret decoder will not handle this properly, bailing out!\n");
		return false;
	}

	uint64_t size;
	if(io.length(size))
		return false;

	uint64_t pos = 4;
	std::vector<uint8_t> data;
	int onerev_time = 0; // time for one revolution, used to guess clock and rpm for DFE2 files
	unsigned long clock_rate = 100000000; // sample clock rate in megahertz
	int rpm=360; // drive rpm
	while(pos < size) {
		uint8_t h[10];
		io.read_at(pos, h, 10, actual);
		uint16_t track = (h[0] << 8) | h[1];
		uint16_t head  = (h[2] << 8) | h[3];
		// Ignore sector
		uint32_t tsize = (h[6] << 24) | (h[7] << 16) | (h[8] << 8) | h[9];

		// if the position-so-far-in-file plus 10 (for the header) plus track size
		// is larger than the size of the file, free buffers and bail out
		if(pos+tsize+10 > size) {
			return false;
		}

		// reallocate the data array if it gets too small
		data.resize(tsize);

		pos += 10; // skip the header, we already read it
		io.read_at(pos, &data[0], tsize, actual);
		pos += tsize; // for next time we read, increment to the beginning of next header

		int index_time = 0; // what point the last index happened
		int index_count = 0; // number of index pulses per track
		//int index_polarity = 1; // current polarity of index, starts high
		int total_time = 0; // total sampled time per track
		for(int i=0; i<tsize; i++) {
			uint8_t v = data[i];
			if (v == 0xFF) {
				osd_printf_error("dfi_dsk: DFI stream contained a 0xFF at t%d, position%d, THIS SHOULD NEVER HAPPEN! Bailing out!\n", track, i);
				return false;
			}
			if((v & 0x7f) == 0x7f)
				total_time += 0x7f;
			else if(v & 0x80) {
				total_time += v & 0x7f;
				index_time = total_time;
				//index_polarity ^= 1;
				//fprintf(stderr,"index state changed to %d at time=%d\n", index_polarity, total_time);
				//fprintf(stderr,"index rising edge seen at time=%d\n", total_time);
				if (onerev_time == 0) onerev_time = total_time;
				index_count += 1;//index_polarity;
			} else // (v & 0x80) == 0
				total_time += v & 0x7f;
		}

		// its possible on single read images for there to be no index pulse during the image at all!
		if (onerev_time == 0) onerev_time = total_time;

		if(!track && !head)
			osd_printf_verbose("dfi_dsk: %02d:%d tt=%10d it=%10d\n", track, head, total_time, index_time);
		if(!track && !head) {
			osd_printf_verbose("dfi_dsk: index_count: %d, onerev_time: %d\n", index_count, onerev_time);
			if ((onerev_time > REV25_MIN) && (onerev_time < REV25_MAX)) {
				osd_printf_verbose("dfi_dsk: Guess: speed: 360rpm, clock 25MHz\n");
				clock_rate = 25000000; rpm = 360;
			} else if ((onerev_time > REV25_MIN*1.2) && (onerev_time < REV25_MAX*1.2)) {
				osd_printf_verbose("dfi_dsk: Guess: speed: 300rpm, clock 25MHz\n");
				clock_rate = 25000000; rpm = 300;
			} else if ((onerev_time > REV25_MIN*2) && (onerev_time < REV25_MAX*2)) {
				osd_printf_verbose("dfi_dsk: Guess: speed: 360rpm, clock 50MHz\n");
				clock_rate = 50000000; rpm = 360;
			} else if ((onerev_time > (REV25_MIN*2)*1.2) && (onerev_time < (REV25_MAX*2)*1.2)) {
				osd_printf_verbose("dfi_dsk: Guess: speed: 300rpm, clock 50MHz\n");
				clock_rate = 50000000; rpm = 300;
			} else if ((onerev_time > REV25_MIN*4) && (onerev_time < REV25_MAX*4)) {
				osd_printf_verbose("dfi_dsk: Guess: speed: 360rpm, clock 100MHz\n");
				clock_rate = 100000000; rpm = 360;
			} else if ((onerev_time > (REV25_MIN*4)*1.2) && (onerev_time < (REV25_MAX*4)*1.2)) {
				osd_printf_verbose("dfi_dsk: Guess: speed: 300rpm, clock 100MHz\n");
				clock_rate = 100000000; rpm = 300;
			} else
				osd_printf_warning("dfi_dsk: WARNING: Cannot Guess Speed! Assuming 360rpm, 100Mhz clock!\n");
			osd_printf_verbose("dfi_dsk: Actual rpm based on index: %f\n", ((double)clock_rate/(double)onerev_time)*60);
		}

		rpm += 0;   // HACK: prevent GCC 4.6+ from warning "variable set but unused"

		if(!index_time)
			index_time = total_time;

		std::vector<uint32_t> &buf = image->get_buffer(track, head);
		buf.resize(tsize);

		int cur_time = 0;
		int prev_time = 0;
#ifdef TRACK_HISTOGRAM
		// histogram
		int time_buckets[4096];
		for (int i = 0; i < 4096; i++)
			time_buckets[i] = 0;
#endif
		index_count = 0;
		//index_polarity = 0;
		uint32_t mg = floppy_image::MG_A;
		int tpos = 0;
		buf[tpos++] = mg;
		for(int i=0; i<tsize; i++) {
			uint8_t v = data[i];
			if((v & 0x7f) == 0x7f) // 0x7F : no transition, but a carry (FF is a board-on-fire error and is checked for above)
				cur_time += 0x7f;
			else if(v & 0x80) { // 0x80 set, note the index (TODO: actually do this!) and add number to count
				cur_time += v & 0x7f;
				//index_polarity ^= 1;
				index_count += 1;//index_polarity;
				//if (index_count == NUMBER_OF_MULTIREADS) break;
				}
			else if((v & 0x80) == 0) { // 0x00-0x7E: not an index or carry, add the number and store transition
				cur_time += v & 0x7f;
				int trans_time = cur_time - prev_time;
#ifdef TRACK_HISTOGRAM
				time_buckets[(trans_time<4096)?trans_time:4095]++;
#endif
				// cur_time and onerev_time need to be converted to something standard, so we'll stick with 300rpm at the standard 200mhz rate that mfi uses internally
				// TODO for 4/22/2012: ACTUALLY DO THIS RESCALING STEP
				// filter out spurious crap
				//if (trans_time <= MIN_THRESH) osd_printf_verbose("dfi_dsk: Throwing out short transition of length %d\n", trans_time);
				// the normal case: write the transition at the appropriate time
				if ((prev_time == 0) || ((trans_time > MIN_THRESH) && (trans_time <= MAX_THRESH))) {
					mg = mg == floppy_image::MG_A ? floppy_image::MG_B : floppy_image::MG_A;
					buf[tpos++] = mg | uint32_t((200000000ULL*cur_time)/index_time);
					prev_time = cur_time;
				}
				// the long case: we probably missed a transition, stuff an extra guessed one in there to see if it helps
				if (trans_time > MAX_THRESH) {
					mg = mg == floppy_image::MG_A ? floppy_image::MG_B : floppy_image::MG_A;
					if (((track%2)==0)&&(head==0)) osd_printf_info("dfi_dsk: missed transition, total time for transition is %d\n",trans_time);
#ifndef FAKETRANS_ONE
					buf[tpos++] = mg | uint32_t((200000000ULL*(cur_time-(trans_time/2)))/index_time); // generate imaginary transition at half period
#else
					buf[tpos++] = mg | uint32_t((200000000ULL*(cur_time-((trans_time*2)/3)))/index_time);
					mg = mg == floppy_image::MG_A ? floppy_image::MG_B : floppy_image::MG_A;
					buf[tpos++] = mg | uint32_t((200000000ULL*(cur_time-(trans_time/3)))/index_time);
#endif
					mg = mg == floppy_image::MG_A ? floppy_image::MG_B : floppy_image::MG_A;
					buf[tpos++] = mg | uint32_t(200000000ULL*cur_time/index_time); // generate transition now
					prev_time = cur_time;
				}
			}
		}
#ifdef TRACK_HISTOGRAM
		if (((track%2)==0)&&(head==0)) {
			for (int i = 0; i < 4096; i++) {
				fprintf(stderr,"%4d:%4d ", i, time_buckets[i]);
				if (((i+1)%10)==0) fprintf(stderr,"\n");
			}
		}
		fprintf(stderr,"\n");
#endif
		index_count = 0;
		buf.resize(tpos);
	}

	return true;
}

const floppy_format_type FLOPPY_DFI_FORMAT = &floppy_image_format_creator<dfi_format>;
