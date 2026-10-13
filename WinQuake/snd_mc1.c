/*
snd_mc1.c: sound output for MC1. The core has no audio device yet, so this mixes into a
memory ring buffer whose play position follows the cycle counter (for measuring what the
mixer costs). Build with -DMC1_SOUND=ON.
*/
#include "quakedef.h"

static unsigned char	dma_buffer[16384];

qboolean SNDDMA_Init (void)
{
	shm = &sn;
	shm->splitbuffer = 0;
	shm->samplebits = 16;
	shm->speed = 11025;
	shm->channels = 2;
	shm->samples = sizeof(dma_buffer) / (shm->samplebits / 8);
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	shm->buffer = dma_buffer;
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	const int frames = (int)(Sys_FloatTime () * shm->speed);

	shm->samplepos = (frames * shm->channels) & (shm->samples - 1);
	return shm->samplepos;
}

void SNDDMA_Shutdown (void)
{
}

void SNDDMA_Submit (void)
{
}
