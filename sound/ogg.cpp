//       _________ __                 __
//      /   _____//  |_____________ _/  |______     ____  __ __  ______
//      \_____  \\   __\_  __ \__  \\   __\__  \   / ___\|  |  \/  ___/
//      /        \|  |  |  | \// __ \|  |  / __ \_/ /_/  >  |  /\___ |
//     /_______  /|__|  |__|  (____  /__| (____  /\___  /|____//____  >
//             \/                  \/          \//_____/            \/
//  ______________________                           ______________________
//                        T H E   W A R   B E G I N S
//        Stratagus - A free fantasy real time strategy game engine
//
/**@name ogg.c - ogg support */
//
//      (c) Copyright 2005 by Nehal Mistry
//
//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; only version 2 of the License.
//
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
//      02111-1307, USA.
//
//      $Id$

//@{

//	This file contains code for both the ogg file format, and ogg vorbis.

/*----------------------------------------------------------------------------
--  Includes
----------------------------------------------------------------------------*/

#include "stratagus.h"

#ifdef USE_VORBIS // {

#include <stdlib.h>
#include <string.h>

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>
#ifdef USE_THEORA
#include <theora/theora.h>
#endif

#include "myendian.h"
#include "iolib.h"
#include "movie.h"
#include "sound_server.h"

/*----------------------------------------------------------------------------
--  Declaration
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
--  Functions
----------------------------------------------------------------------------*/

int OggGetNextPage(ogg_page *page, ogg_sync_state *sync, CLFile *f)
{
	char *buf;
	int bytes;

	while (ogg_sync_pageout(sync, page) != 1) {
		// need more bytes
		buf = ogg_sync_buffer(sync, 4096);
		bytes = CLread(f, buf, 4096);
		if (!bytes || ogg_sync_wrote(sync, bytes)) {
			return -1;
		}
	}

	return 0;
}

int VorbisProcessData(OggData *data, char *buffer)
{
	int num_samples;
	float **pcm;
	float *chan;
	int i, j;
	int val;
	ogg_packet packet;
	int len;

	len = 0;
	num_samples = 0;

	while (!len) {
		if (ogg_stream_packetout(&data->astream, &packet) != 1) {
			if (OggGetNextPage(&data->page, &data->sync, data->File)) {
				// EOF
				return -1;
			}

			ogg_stream_pagein(&data->astream, &data->page);
		} else {
			if (vorbis_synthesis(&data->vblock, &packet) == 0) {
				vorbis_synthesis_blockin(&data->vdsp, &data->vblock);
			}

			while ((num_samples = vorbis_synthesis_pcmout(&data->vdsp, &pcm)) > 0) {
				j = 0;
				for (i = 0; i < data->vinfo.channels; ++i) {
					chan = pcm[i];
					for (j = 0; j < num_samples; ++j) {
						val = chan[j] * 32767.f;
						if (val > 32767) {
							val = 32767;
						} else if (val < -32768) {
							val = -32768;
						}

						*(int16_t*)(buffer + len
						  + (j * 2 * data->vinfo.channels) + i * 2) = (int16_t)val;
					}
				}
				len += j * 2 * data->vinfo.channels;

				// we consumed num_samples
				vorbis_synthesis_read(&data->vdsp, num_samples);
			}
		}
	}

	return len;
}

int OggInit(CLFile *f, OggData *data)
{
	ogg_packet packet;
	int num_vorbis;
#ifdef USE_THEORA
	int num_theora;
#endif
	int stream_start;
	int ret;

	int magic[1];
	CLread(f, magic, sizeof(magic));
	if (AccessLE32(magic) != 0x5367674F) { // "OggS" in ASCII
		return -1;
	}
	CLseek(f, 0, SEEK_SET);

	ogg_sync_init(&data->sync);

	vorbis_info_init(&data->vinfo);
	vorbis_comment_init(&data->vcomment);

#ifdef USE_THEORA
	theora_info_init(&data->tinfo);
	theora_comment_init(&data->tcomment);
#endif

#ifdef USE_THEORA
	num_theora = 0;
#endif
	num_vorbis = 0;
	stream_start = 0;
	while (!stream_start) {
		ogg_stream_state test;

		if (OggGetNextPage(&data->page, &data->sync, f)) {
			return -1;
		}

		if (!ogg_page_bos(&data->page)) {
			if (num_vorbis) {
				ogg_stream_pagein(&data->astream, &data->page);
			}
#ifdef USE_THEORA
			if (num_theora) {
				ogg_stream_pagein(&data->vstream, &data->page);
			}
#endif
			stream_start = 1;
			break;
		}

		ogg_stream_init(&test, ogg_page_serialno(&data->page));
		if (ogg_stream_pagein(&test, &data->page))
			exit(-1);

		// initial codec headers
		while (ogg_stream_packetout(&test, &packet) == 1) {
#ifdef USE_THEORA
			if (theora_decode_header(&data->tinfo, &data->tcomment, &packet) >= 0) {
				memcpy(&data->vstream, &test, sizeof(test));
				++num_theora;
			} else
#endif
			if (!vorbis_synthesis_headerin(&data->vinfo, &data->vcomment, &packet)) {
				memcpy(&data->astream, &test, sizeof(test));
				++num_vorbis;
			} else {
				ogg_stream_clear(&test);
			}
		}
	}

	data->audio = num_vorbis;
#ifdef USE_THEORA
	data->video = num_theora;
#endif

	// remainint codec headers
	while ((num_vorbis && num_vorbis < 3)
#ifdef USE_THEORA
	  || (num_theora && num_theora < 3) ) {
		// are we in the theora page ?
		while (num_theora && num_theora < 3 &&
		  (ret = ogg_stream_packetout(&data->vstream, &packet))) {
			if (ret < 0) {
				return -1;
			}
			if (theora_decode_header(&data->tinfo, &data->tcomment, &packet)) {
				return -1;
			}
			++num_theora;
		}
#else
	  ) {
#endif

		// are we in the vorbis page ?
		while (num_vorbis && num_vorbis < 3 && 
		  (ret = ogg_stream_packetout(&data->astream, &packet))) {
			if (ret < 0) {
				return -1;
			}
			if (vorbis_synthesis_headerin(&data->vinfo, &data->vcomment, &packet)) {
				return -1;
				
			}
			++num_vorbis;
		}

		if (OggGetNextPage(&data->page, &data->sync, f)) {
				break;
		}

		if (num_vorbis) {
			ogg_stream_pagein(&data->astream, &data->page);
		}
#ifdef USE_THEORA
		if (num_theora) {
			ogg_stream_pagein(&data->vstream, &data->page);
		}
#endif
	}

	if (num_vorbis) {
		vorbis_synthesis_init(&data->vdsp, &data->vinfo);
		vorbis_block_init(&data->vdsp, &data->vblock);
	} else {
    	vorbis_info_clear(&data->vinfo);
    	vorbis_comment_clear(&data->vcomment);
	}

#ifdef USE_THEORA
	if (num_theora) {
		theora_decode_init(&data->tstate, &data->tinfo);
	} else {
    	theora_info_clear(&data->tinfo);
    	theora_comment_clear(&data->tcomment);
	}

	return !(num_vorbis || num_theora);
#else
	return !num_vorbis;
#endif
}

static int VorbisStreamRead(Sample* sample, void* buf, int len)
{
	OggData* data;
	int bytes;

	data = sample->User;

	if (sample->Pos > SOUND_BUFFER_SIZE / 2) {
		memcpy(sample->Buffer, sample->Buffer + sample->Pos, sample->Len);
		sample->Pos = 0;
	}

	while (sample->Len < SOUND_BUFFER_SIZE / 4) {
		bytes = VorbisProcessData(data, sample->Buffer + sample->Pos + sample->Len);
		if (bytes > 0) {
			sample->Len += bytes;
		} else {
			break;
		}
	}

	if (sample->Len < len) {
		len = sample->Len;
	}

	memcpy(buf, sample->Buffer + sample->Pos, len);
	sample->Pos += len;
	sample->Len -= len;

	return len;
}

static void VorbisStreamFree(Sample* sample)
{
	OggData *data;
	data = sample->User;

	CLclose(data->File);

	if (data->audio) {
		ogg_stream_clear(&data->astream);
		vorbis_block_clear(&data->vblock);
		vorbis_dsp_clear(&data->vdsp);
		vorbis_comment_clear(&data->vcomment);
		vorbis_info_clear(&data->vinfo);
	}
#ifdef USE_THEORA
	if (data->video) {
		ogg_stream_clear(&data->vstream);
		theora_comment_clear(&data->tcomment);
		theora_info_clear(&data->tinfo);
	}
#endif

	free(data);
	free(sample->Buffer);
	free(sample);
}

/**
** Ogg stream type structure.
*/
static const SampleType VorbisStreamSampleType = {
	VorbisStreamRead,
	VorbisStreamFree,
};



/**
**  Load vorbis.
**
**  @param name   File name.
**  @param flags  Load flags.
**
**  @return       Returns the loaded sample.
*/
Sample* LoadVorbis(const char* name,int flags)
{
	Sample* sample;
	OggData *data;
	CLFile* f;
	vorbis_info* info;

	if (!(f = CLopen(name, CL_OPEN_READ))) {
		fprintf(stderr, "Can't open file `%s'\n", name);
		return NULL;
	}

	data = malloc(sizeof(OggData));

	if (OggInit(f, data) || !data->audio) {
		free(data);
		CLclose(f);
		return NULL;
	}

	info = &data->vinfo;

	sample = malloc(sizeof(Sample));
	sample->Channels = info->channels;
	sample->SampleSize = 16;
	sample->Frequency = info->rate;

	sample->Len = 0;
	sample->Pos = 0;
//	sample->Buffer = malloc(sizeof(SOUND_BUFFER_SIZE));
	sample->User = data;
	data->File = f;

	if (flags & PlayAudioStream) {
		sample->Buffer = malloc(SOUND_BUFFER_SIZE);
		sample->Type = &VorbisStreamSampleType;
	} else {
		exit(-1);
		// todo
	}

	return sample;
}

#endif // USE_VORBIS

//@}
